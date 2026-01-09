/* Copyright (C) 2007 Google Inc.
   Copyright (c) 2008, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include <my_global.h>
#include "semisync_master.h"
#include <algorithm>
#include <mysql_com.h>

#define TIME_THOUSAND 1000
#define TIME_MILLION  1000000
#define TIME_BILLION  1000000000

/* This indicates whether semi-synchronous replication is enabled. */
my_bool rpl_semi_sync_master_enabled= 0;
unsigned long long rpl_semi_sync_master_request_ack = 0;
unsigned long long rpl_semi_sync_master_get_ack = 0;
my_bool rpl_semi_sync_master_wait_no_slave = 1;
my_bool rpl_semi_sync_master_status        = 0;
ulong rpl_semi_sync_master_wait_point       =
    SEMI_SYNC_MASTER_WAIT_POINT_AFTER_STORAGE_COMMIT;
ulong rpl_semi_sync_master_timeout;
ulong rpl_semi_sync_master_trace_level;
ulong rpl_semi_sync_master_yes_transactions = 0;
ulong rpl_semi_sync_master_no_transactions  = 0;
ulong rpl_semi_sync_master_off_times        = 0;
ulong rpl_semi_sync_master_timefunc_fails   = 0;
ulong rpl_semi_sync_master_wait_timeouts     = 0;
ulong rpl_semi_sync_master_wait_sessions    = 0;
ulong rpl_semi_sync_master_wait_pos_backtraverse = 0;
ulong rpl_semi_sync_master_avg_trx_wait_time = 0;
ulonglong rpl_semi_sync_master_trx_wait_num = 0;
ulong rpl_semi_sync_master_avg_net_wait_time    = 0;
ulonglong rpl_semi_sync_master_net_wait_num = 0;
ulong rpl_semi_sync_master_clients          = 0;
ulonglong rpl_semi_sync_master_net_wait_time = 0;
ulonglong rpl_semi_sync_master_trx_wait_time = 0;

Repl_semi_sync_master *repl_semisync_master;
Ack_receiver ack_receiver;


static int get_wait_time(const struct timespec& start_ts);

static ulonglong timespec_to_usec(const struct timespec *ts)
{
  return (ulonglong) ts->tv_sec * TIME_MILLION + ts->tv_nsec / TIME_THOUSAND;
}

static void
signal_waiting_transaction(THD *waiting_thd, bool thd_valid)
{
  /*
    It is possible that the connection thd waiting for an ACK was killed. In
    such circumstance, the connection thread will nullify the thd member of its
    Active_tranx node. So before we try to signal, ensure the THD exists.

    The thd_valid is only set while the THD is waiting in commit_trx(); this
    is defensive coding to not signal an invalid THD if we somewhere
    accidentally did not remove the transaction from the list.
  */
  if (waiting_thd && thd_valid)
    mysql_cond_signal(&waiting_thd->COND_wakeup_ready);
}

/*******************************************************************************
 *
 * <Active_tranx> class : manage all active transaction nodes
 *
 ******************************************************************************/

Active_tranx::Active_tranx(mysql_mutex_t *lock,
                           mysql_cond_t *cond,
                           ulong trace_level)
  : Trace(trace_level), m_allocator(max_connections),
    m_num_entries(max_connections << 1), /* Transaction hash table size
                                         * is set to double the size
                                         * of max_connections */
    m_lock(lock),
    m_cond_empty(cond)
{
  /* No transactions are in the list initially. */
  m_trx_front = NULL;
  m_trx_rear  = NULL;

  /* Create the hash table to find a transaction's ending event. */
  m_trx_htb = new Tranx_node *[m_num_entries];
  for (int idx = 0; idx < m_num_entries; ++idx)
    m_trx_htb[idx] = NULL;

#ifdef EXTRA_DEBUG
  sql_print_information("Semi-sync replication initialized for transactions.");
#endif
}

Active_tranx::~Active_tranx()
{
  delete [] m_trx_htb;
  m_trx_htb          = NULL;
  m_num_entries      = 0;
}


Active_tranx_file_pos::Active_tranx_file_pos(mysql_mutex_t *lock,
                                             mysql_cond_t *cond,
                                             ulong trace_level)
  : Active_tranx(lock, cond, trace_level)
{
}


Active_tranx_gtid::Active_tranx_gtid(mysql_mutex_t *lock, mysql_cond_t *cond,
                                     ulong trace_level)
  : Active_tranx(lock, cond, trace_level)
{
}


unsigned int Active_tranx::calc_hash(const unsigned char *key, size_t length)
{
  unsigned int nr = 1, nr2 = 4;

  /* The hash implementation comes from calc_hashnr() in mysys/hash.c. */
  while (length--)
  {
    nr  ^= (((nr & 63)+nr2)*((unsigned int) (unsigned char) *key++))+ (nr << 8);
    nr2 += 3;
  }
  return((unsigned int) nr);
}


unsigned int
Active_tranx_file_pos::get_hash_value(const Repl_semi_sync_trx_info *inf)
{
  unsigned int hash1 = calc_hash((const unsigned char *)inf->log_file,
                                 strlen(inf->log_file));
  unsigned int hash2 = calc_hash((const unsigned char *)(&inf->log_pos),
                                 sizeof(inf->log_pos));

  return (hash1 + hash2) % m_num_entries;
}


unsigned int
Active_tranx_gtid::get_hash_value(const Repl_semi_sync_trx_info *inf)
{
  DBUG_ASSERT(inf->gtid.seq_no != 0);
  unsigned int hash = calc_hash((const unsigned char *)&inf->gtid,
                                sizeof(inf->gtid));

  return hash % m_num_entries;
}


bool
Active_tranx_file_pos::compare(const Repl_semi_sync_trx_info *inf1,
                               const Repl_semi_sync_trx_info *inf2)
{
  int cmp = strcmp(inf1->log_file, inf2->log_file);

  if (cmp != 0)
    return cmp;

  if (inf1->log_pos != inf2->log_pos)
    return 1;
  return 0;
}


bool
Active_tranx_gtid::compare(const Repl_semi_sync_trx_info *inf1,
                           const Repl_semi_sync_trx_info *inf2)
{
  return memcmp(&inf1->gtid, &inf2->gtid, sizeof(rpl_gtid));
}


int Active_tranx::insert_tranx_node(THD *thd_to_wait,
                                    const Repl_semi_sync_trx_info *inf)
{
  Tranx_node  *ins_node;
  int         result = 0;
  unsigned int        hash_val;

  DBUG_ENTER("Active_tranx:insert_tranx_node");

  ins_node = m_allocator.allocate_node();
  if (!ins_node)
  {
    sql_print_error("%s: transaction node allocation failed for: (%s, %lu) "
                    "GTID %u-%u-%llu",
                    "Active_tranx:insert_tranx_node",
                    inf->log_file, (ulong)inf->log_pos, inf->gtid.domain_id,
                    inf->gtid.server_id, (ulonglong)inf->gtid.seq_no);
    result = -1;
    goto l_end;
  }

  /* insert the binlog position in the active transaction list. */
  strncpy(ins_node->log_name, inf->log_file, FN_REFLEN-1);
  ins_node->log_name[FN_REFLEN-1] = 0; /* make sure it ends properly */
  ins_node->log_pos = inf->log_pos;
  ins_node->gtid= inf->gtid;
  ins_node->thd= thd_to_wait;
  ins_node->thd_valid= false;

  if (!m_trx_front)
  {
    /* The list is empty. */
    m_trx_front = m_trx_rear = ins_node;
  }
  else
  {
    DBUG_ASSERT(compare(ins_node, m_trx_rear)
                 /* Must not try to insert same position twice */);
    /* Insert the new transaction at the tail of the list. */
    m_trx_rear->next = ins_node;
    m_trx_rear = ins_node;
  }

  hash_val = get_hash_value(inf);
  ins_node->hash_next = m_trx_htb[hash_val];
  m_trx_htb[hash_val]   = ins_node;

  DBUG_PRINT("semisync", ("%s: insert (%s, %lu) %u-%u-%llu in entry(%u)",
                          "Active_tranx:insert_tranx_node",
                          ins_node->log_name, (ulong)ins_node->log_pos,
                          inf->gtid.domain_id, inf->gtid.server_id,
                          (ulonglong)inf->gtid.seq_no, hash_val));
 l_end:

  DBUG_RETURN(result);
}

void Active_tranx::clear_active_tranx_nodes(const Repl_semi_sync_trx_info *inf)
{
  Tranx_node *new_front;
  bool found= false;

  DBUG_ENTER("Active_tranx::::clear_active_tranx_nodes");

  new_front= m_trx_front;
  while (new_front && !found)
  {
    if ((inf != NULL) && !compare(new_front, inf))
      found= true;  /* Stop after deleting the specified node */
    signal_waiting_transaction(new_front->thd, new_front->thd_valid);
    new_front = new_front->next;
  }
  /* We should never try to delete something that's not in the list. */
  DBUG_ASSERT(inf == NULL || found);

  if (new_front == NULL)
  {
    /* No active transaction nodes after the call. */

    /* Clear the hash table. */
    memset(m_trx_htb, 0, m_num_entries * sizeof(Tranx_node *));
    m_allocator.free_all_nodes();

    /* Clear the active transaction list. */
    if (m_trx_front != NULL)
    {
      m_trx_front = NULL;
      m_trx_rear  = NULL;
    }

    DBUG_PRINT("semisync", ("%s: cleared all nodes",
                            "Active_tranx::::clear_active_tranx_nodes"));
  }
  else if (new_front != m_trx_front)
  {
    Tranx_node *curr_node, *next_node;

    /* Delete all transaction nodes before the confirmation point. */
#ifdef DBUG_TRACE
    int n_frees = 0;
#endif
    curr_node = m_trx_front;
    while (curr_node != new_front)
    {
      next_node = curr_node->next;
#ifdef DBUG_TRACE
      n_frees++;
#endif

      /* Remove the node from the hash table. */
      unsigned int hash_val = get_hash_value(curr_node);
      Tranx_node **hash_ptr = &(m_trx_htb[hash_val]);
      while ((*hash_ptr) != NULL)
      {
        if ((*hash_ptr) == curr_node)
	{
          (*hash_ptr) = curr_node->hash_next;
          break;
        }
        hash_ptr = &((*hash_ptr)->hash_next);
      }

      curr_node = next_node;
    }

    m_trx_front = new_front;
    m_allocator.free_nodes_before(m_trx_front);

    DBUG_PRINT("semisync", ("%s: cleared %d nodes back until pos (%s, %lu)",
                            "Active_tranx::::clear_active_tranx_nodes",
                            n_frees,
                            m_trx_front->log_name, (ulong)m_trx_front->log_pos));
  }

  /*
    m_cond_empty aliases Repl_semi_sync_master::COND_binlog, which holds the
    condition variable to notify that we have cleared all nodes, e.g. used by
    SHUTDOWN WAIT FOR ALL SLAVES.
  */
  if (is_empty())
    mysql_cond_signal(m_cond_empty);

  DBUG_VOID_RETURN;
}


Tranx_node *
Active_tranx::find_latest(slave_connection_state *state)
{
  Tranx_node *cur_node, *found_node= nullptr;

  cur_node= m_trx_front;
  while (cur_node)
  {
    rpl_gtid *gtid= state->find(cur_node->gtid.domain_id);
    if (gtid &&
        gtid->server_id == cur_node->gtid.server_id &&
        gtid->seq_no == cur_node->gtid.seq_no)
      found_node= cur_node;
    cur_node= cur_node->next;
  }

  return found_node;
}


void Active_tranx::unlink_thd_as_waiter(const Repl_semi_sync_trx_info *inf)
{
  DBUG_ENTER("Active_tranx::unlink_thd_as_waiter");
  mysql_mutex_assert_owner(m_lock);

  unsigned int hash_val = get_hash_value(inf);
  Tranx_node *entry = m_trx_htb[hash_val];

  while (entry != NULL)
  {
    if (!compare(entry, inf))
      break;

    entry = entry->hash_next;
  }

  if (entry)
  {
    entry->thd= NULL;
    entry->thd_valid= false;
  }

  DBUG_VOID_RETURN;
}

Tranx_node *
Active_tranx::is_thd_waiter(Repl_semi_sync_trx_info *inf)
{
  DBUG_ENTER("Active_tranx::assert_thd_is_waiter");
  mysql_mutex_assert_owner(m_lock);

  unsigned int hash_val = get_hash_value(inf);
  Tranx_node *entry = m_trx_htb[hash_val];

  while (entry != NULL)
  {
    if (!compare(entry, inf))
    {
      break;
    }

    entry = entry->hash_next;
  }

  DBUG_RETURN(entry);
}


Repl_semi_sync_trx_info::Repl_semi_sync_trx_info()
{
  gtid.domain_id= 0;
  gtid.server_id= 0;
  gtid.seq_no= 0;
  log_file= "";
  log_pos= 0;
}


Repl_semi_sync_trx_info::Repl_semi_sync_trx_info(const char *file_name_,
                                                 my_off_t log_pos_)
{
  gtid.domain_id= 0;
  gtid.server_id= 0;
  gtid.seq_no= 0;
  log_file= file_name_;
  log_pos= log_pos_;
}

Repl_semi_sync_trx_info::Repl_semi_sync_trx_info(const char *file_name_,
                                                 my_off_t log_pos_,
                                                 const rpl_gtid *gtid_)
{
  gtid= *gtid_;
  log_file= file_name_;
  log_pos= log_pos_;
}


Trans_binlog_info::Trans_binlog_info()
  : Repl_semi_sync_trx_info(file_name_buf, 0)
{
  file_name_buf[0]= '\0';
}


Trans_binlog_info::Trans_binlog_info(const char *file_name,
                                                  my_off_t log_pos)
  : Repl_semi_sync_trx_info(file_name_buf, log_pos)
{
  strmake_buf(file_name_buf, file_name);
}


Trans_binlog_info::Trans_binlog_info(const char *file_name,
                                                  my_off_t log_pos,
                                                  const rpl_gtid *gtid)
  : Repl_semi_sync_trx_info(file_name_buf, log_pos, gtid)
{
  strmake_buf(file_name_buf, file_name);
}


void
Trans_binlog_info::set(const Repl_semi_sync_trx_info *inf)
{
  strmake_buf(file_name_buf, inf->log_file);
  gtid= inf->gtid;
  log_file= file_name_buf;
  log_pos= inf->log_pos;
}


void
Trans_binlog_info::set(const char *log_file_, my_off_t log_pos_,
                       const rpl_gtid *gtid_)
{
  strmake_buf(file_name_buf, log_file_);
  gtid= *gtid_;
  log_file= file_name_buf;
  log_pos= log_pos_;
}


void
Trans_binlog_info::clear()
{
  file_name_buf[0]= '\0';
  log_file= NULL;
  log_pos= 0;
  gtid.domain_id= 0;
  gtid.server_id= 0;
  gtid.seq_no= 0;
}


/*******************************************************************************
 *
 * <Repl_semi_sync_master> class: the basic code layer for semisync master.
 * <Repl_semi_sync_slave>  class: the basic code layer for semisync slave.
 *
 * The most important functions during semi-syn replication listed:
 *
 * Master:
 *  . report_reply_binlog():  called by the binlog dump thread when it receives
 *                          the slave's status information.
 *  . update_sync_header():   based on transaction waiting information, decide
 *                          whether to request the slave to reply.
 *  . write_tranx_in_binlog(): called by the transaction thread when it finishes
 *                          writing all transaction events in binlog.
 *  . commit_trx():          transaction thread wait for the slave reply.
 *
 * Slave:
 *  . slave_read_sync_header(): read the semi-sync header from the master, get
 *                              the sync status and get the payload for events.
 *  . slave_reply():          reply to the master about the replication progress.
 *
 ******************************************************************************/

Repl_semi_sync_master::Repl_semi_sync_master()
  : m_active_tranxs(NULL),
    m_init_done(false),
    m_reply_inf_inited(false),
    m_reply_inf(),
    m_master_enabled(false),
    m_wait_timeout(0L),
    m_state(0),
    m_wait_point(0)
{
}

int Repl_semi_sync_master::init_object()
{
  int result= 0;

  m_init_done = true;

  /* References to the parameter works after set_options(). */
  set_wait_timeout(rpl_semi_sync_master_timeout);
  set_trace_level(rpl_semi_sync_master_trace_level);
  set_wait_point(rpl_semi_sync_master_wait_point);

  /* Mutex initialization can only be done after MY_INIT(). */
  mysql_mutex_init(key_LOCK_rpl_semi_sync_master_enabled,
                   &LOCK_rpl_semi_sync_master_enabled, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_binlog,
                   &LOCK_binlog, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_binlog_send,
                  &COND_binlog_send, NULL);

  if (rpl_semi_sync_master_enabled)
  {
    result = enable_master();
    if (!result)
      result= ack_receiver.start(); /* Start the ACK thread. */
  }
  else
    disable_master();

  return result;
}


template <typename F> int
Repl_semi_sync_master::enable_master(F tranx_alloc)
{
  int result = 0;

  /* Must have the lock when we do enable of disable. */
  lock();

  if (!get_master_enabled())
  {
    m_active_tranxs= tranx_alloc(&LOCK_binlog, &COND_binlog_send, m_trace_level);
    if (m_active_tranxs != NULL)
    {
      m_commit_inf_inited = false;
      m_reply_inf_inited  = false;

      set_master_enabled(true);
      m_state = true;
      sql_print_information("Semi-sync replication enabled on the master.");
    }
    else
    {
      sql_print_error("Cannot allocate memory to enable semi-sync on the master.");
      result = -1;
    }
  }

  unlock();

  return result;
}


int
Repl_semi_sync_master_file_pos::enable_master()
{
  return Repl_semi_sync_master::enable_master
    ([](mysql_mutex_t *l, mysql_cond_t *c, unsigned long t) -> Active_tranx * {
      return new Active_tranx_file_pos(l, c, t);
    });
}


int
Repl_semi_sync_master_gtid::enable_master()
{
  return Repl_semi_sync_master::enable_master
    ([](mysql_mutex_t *l, mysql_cond_t *c, unsigned long t) -> Active_tranx * {
      return new Active_tranx_gtid(l, c, t);
    });
}


void Repl_semi_sync_master::disable_master()
{
  /* Must have the lock when we do enable of disable. */
  lock();

  if (get_master_enabled())
  {
    /* Switch off the semi-sync first so that waiting transaction will be
     * waken up.
     */
    switch_off();

    DBUG_ASSERT(m_active_tranxs != NULL);
    delete m_active_tranxs;
    m_active_tranxs = NULL;

    m_reply_inf_inited = false;
    m_commit_inf_inited = false;

    set_master_enabled(false);
  }

  unlock();
}

void Repl_semi_sync_master::cleanup()
{
  if (m_init_done)
  {
    mysql_mutex_destroy(&LOCK_rpl_semi_sync_master_enabled);
    mysql_mutex_destroy(&LOCK_binlog);
    mysql_cond_destroy(&COND_binlog_send);
    m_init_done= 0;
  }

  delete m_active_tranxs;
}

void Repl_semi_sync_master::create_timeout(struct timespec *out,
                                           struct timespec *start_arg)
{
  struct timespec *start_ts;
  struct timespec now_ts;
  if (!start_arg)
  {
    set_timespec(now_ts, 0);
    start_ts= &now_ts;
  }
  else
  {
    start_ts= start_arg;
  }

  long diff_secs= (long) (m_wait_timeout / TIME_THOUSAND);
  long diff_nsecs= (long) ((m_wait_timeout % TIME_THOUSAND) * TIME_MILLION);
  long nsecs= start_ts->tv_nsec + diff_nsecs;
  out->tv_sec= start_ts->tv_sec + diff_secs + nsecs / TIME_BILLION;
  out->tv_nsec= nsecs % TIME_BILLION;
}

void Repl_semi_sync_master::lock()
{
  mysql_mutex_lock(&LOCK_binlog);
}

void Repl_semi_sync_master::unlock()
{
  mysql_mutex_unlock(&LOCK_binlog);
}

void Repl_semi_sync_master::add_slave()
{
  lock();
  rpl_semi_sync_master_clients++;
  unlock();
}

void Repl_semi_sync_master::remove_slave()
{
  lock();
  DBUG_ASSERT(rpl_semi_sync_master_clients > 0);
  if (!(--rpl_semi_sync_master_clients) && !rpl_semi_sync_master_wait_no_slave
      && get_master_enabled())
  {
    /*
      Signal transactions waiting in commit_trx() that they do not have to
      wait anymore.
    */
    DBUG_ASSERT(m_active_tranxs);
    m_active_tranxs->clear_active_tranx_nodes(NULL);
  }
  unlock();
}


/*
  Check report package

  @retval 0   ok
  @retval 1   Error
  @retval -1  Slave is going down (ok)
*/

int Repl_semi_sync_master::report_reply_packet(uint32 server_id,
                                               const uchar *packet,
                                               ulong packet_len)
{
  int result= 1;                                // Assume error
  DBUG_ENTER("Repl_semi_sync_master::report_reply_packet");

  DBUG_EXECUTE_IF("semisync_corrupt_magic",
                  const_cast<uchar*>(packet)[REPLY_MAGIC_NUM_OFFSET]= 0;);
  if (unlikely(packet[REPLY_MAGIC_NUM_OFFSET] !=
               Repl_semi_sync_master::k_packet_magic_num))
  {
    if (packet[0] == COM_QUIT && packet_len == 1)
    {
      /* Slave sent COM_QUIT as part of IO thread going down */
      sql_print_information("slave IO thread has stopped");
      DBUG_RETURN(-1);
    }
    else
      sql_print_error("Read semi-sync reply magic number error. "
                      "Got magic: %u  command %u  length: %lu",
                      (uint) packet[REPLY_MAGIC_NUM_OFFSET], (uint) packet[0],
                      packet_len);
    goto l_end;
  }

  result= report_reply_packet_sub(server_id, packet, packet_len);
  if (unlikely(result))
    goto l_end;

  DBUG_RETURN(0);

l_end:
  {
    char buf[256];
    octet2hex(buf, (const unsigned char*) packet,
              MY_MIN(sizeof(buf)-1, (size_t) packet_len));
    sql_print_information("First bytes of the packet from semisync slave "
                          "server-id %d: %s", server_id, buf);

  }
  DBUG_RETURN(result);
}

int
Repl_semi_sync_master_file_pos::report_reply_packet_sub(uint32 server_id,
                                                        const uchar *packet,
                                                        ulong packet_len)
{
  char log_file_name[FN_REFLEN+1];
  my_off_t log_file_pos;
  ulong log_file_len = 0;

  if (unlikely(packet_len < REPLY_BINLOG_NAME_OFFSET))
  {
    sql_print_error("Read semi-sync reply length error: packet is too small: %lu",
                    packet_len);
    return 1;
  }

  log_file_pos = uint8korr(packet + REPLY_BINLOG_POS_OFFSET);
  log_file_len = packet_len - REPLY_BINLOG_NAME_OFFSET;
  if (unlikely(log_file_len >= FN_REFLEN))
  {
    sql_print_error("Read semi-sync reply binlog file length too large: %llu",
                    (ulonglong) log_file_pos);
    return 1;
  }
  strncpy(log_file_name, (const char*)packet + REPLY_BINLOG_NAME_OFFSET, log_file_len);
  log_file_name[log_file_len] = 0;

  DBUG_ASSERT(dirname_length(log_file_name) == 0);

  DBUG_PRINT("semisync", ("%s: Got reply(%s, %lu) from server %u",
                          "Repl_semi_sync_master::report_reply_packet",
                          log_file_name, (ulong)log_file_pos, server_id));

  rpl_semi_sync_master_get_ack++;
  report_reply_binlog_file_pos(server_id, log_file_name, log_file_pos);

  return 0;
}


int
Repl_semi_sync_master_gtid::report_reply_packet_sub(uint32 server_id,
                                                    const uchar *packet,
                                                    ulong packet_len)
{
  rpl_gtid gtid;

  if (unlikely(packet_len < REPLY_MESSAGE_GTID_LENGTH))
  {
    sql_print_error("Read semi-sync reply length error: packet is too small: %lu",
                    packet_len);
    return 1;
  }

  gtid.domain_id= uint4korr(packet + REPLY_GTID_OFFSET);
  gtid.server_id= uint4korr(packet + REPLY_GTID_OFFSET + 4);
  gtid.seq_no= uint8korr(packet + REPLY_GTID_OFFSET + 4 + 4);

  DBUG_PRINT("semisync", ("%s: Got reply GTID %u-%u-%llu from server %u",
                          "Repl_semi_sync_master::report_reply_packet",
                          gtid.domain_id, gtid.server_id,
                          (ulonglong)gtid.seq_no, server_id));

  rpl_semi_sync_master_get_ack++;
  report_reply_binlog_gtid(server_id, &gtid);

  return 0;
}


int
Repl_semi_sync_master_file_pos::report_reply_binlog_file_pos(uint32 server_id,
                                                             const char *log_file_name,
                                                             my_off_t log_file_pos)
{
  Tranx_node *tranx_entry;
  int res= 0;

  if (!(get_master_enabled()))
    return 0;

  lock();

  /* This is the real check inside the mutex. */
  if (get_master_enabled())
  {
    const rpl_gtid zero_gtid= {0, 0, 0};
    Repl_semi_sync_trx_info inf(log_file_name, log_file_pos, &zero_gtid);
    tranx_entry= m_active_tranxs->is_thd_waiter(&inf);
    if (tranx_entry)
      inf.gtid= tranx_entry->gtid;
    res= report_reply_binlog(server_id, &inf, tranx_entry);
  }

  unlock();

  return res;
}


int
Repl_semi_sync_master_gtid::report_reply_binlog_gtid(uint32 server_id,
                                                     const rpl_gtid *gtid,
                                                     bool have_gtid)
{
  int res= 0;

  if (!(get_master_enabled()))
    return 0;

  lock();

  /* This is the real check inside the mutex. */
  if (get_master_enabled())
  {
    Trans_binlog_info inf("", 0, gtid);
    Tranx_node *tranx_entry= nullptr;
    if (have_gtid)
    {
      tranx_entry= m_active_tranxs->is_thd_waiter(&inf);
      if (tranx_entry)
        inf.set(tranx_entry->log_name, tranx_entry->log_pos, gtid);
    }
    res= report_reply_binlog(server_id,
                             (have_gtid ? &inf : nullptr),
                             tranx_entry);
  }

  unlock();

  return res;

}


int
Repl_semi_sync_master::report_reply_binlog(uint32 server_id,
                                           Repl_semi_sync_trx_info *inf,
                                           Tranx_node *tranx_entry)
{
  DBUG_ENTER("Repl_semi_sync_master::report_reply_binlog");

  if (!is_on())
    /* We check to see whether we can switch semi-sync ON. */
    try_switch_on(server_id, inf);

  /* If we have multiple semi-sync slaves connected, another one might already
   * have ack'ed this one, and removed it from the list.
   */
  if (tranx_entry)
  {
    /* We got a new ack, update the replied-so-far position. */
    m_reply_inf.set(inf);
    m_reply_inf_inited = true;

    /* Remove all active transaction nodes before this point. */
    DBUG_ASSERT(m_active_tranxs != NULL);
    m_active_tranxs->clear_active_tranx_nodes(inf);

    DBUG_PRINT("semisync", ("%s: Got reply at (%s, %lu) GTID %u-%u-%llu",
                            "Repl_semi_sync_master::report_reply_binlog",
                            inf->log_file, (ulong)inf->log_pos,
                            inf->gtid.domain_id, inf->gtid.server_id,
                            (ulonglong)inf->gtid.seq_no));
  }

  DBUG_RETURN(0);
}

int Repl_semi_sync_master::wait_after_sync(const char *log_file,
                                           my_off_t log_pos,
                                           const rpl_gtid *gtid)
{
  if (!get_master_enabled())
    return 0;

  int ret= 0;
  if(log_pos &&
     wait_point() == SEMI_SYNC_MASTER_WAIT_POINT_AFTER_BINLOG_SYNC)
  {
    Repl_semi_sync_trx_info inf(log_file + dirname_length(log_file), log_pos,
                                gtid);
    ret= commit_trx(&inf);
  }

  return ret;
}


int Repl_semi_sync_master::wait_after_commit(THD* thd, bool all)
{
  if (!get_master_enabled())
    return 0;

  int ret= 0;

  bool is_real_trans=
    (all || thd->transaction->all.ha_list == 0);
  /*
    The coordinates are propagated to this point having been computed
    in report_binlog_update
  */
  Trans_binlog_info *log_info= thd->semisync_info;
  bool got_info= log_info && !log_info->is_clear();

  DBUG_ASSERT(!log_info || !log_info->log_file ||
              dirname_length(log_info->log_file) == 0);

  if (is_real_trans &&
      got_info &&
      wait_point() == SEMI_SYNC_MASTER_WAIT_POINT_AFTER_STORAGE_COMMIT)
    ret= commit_trx(log_info);

  if (is_real_trans && got_info)
    log_info->clear();

  return ret;
}

int Repl_semi_sync_master::wait_after_rollback(THD *thd, bool all)
{
  return wait_after_commit(thd, all);
}

/**
  The method runs after flush to binary log is done.
*/
int Repl_semi_sync_master::report_binlog_update(THD *trans_thd,
                                                THD *waiter_thd,
                                                const char *log_file,
                                                my_off_t log_pos,
                                                const rpl_gtid *gtid)
{
  if (get_master_enabled())
  {
    Trans_binlog_info *log_info;
    const char *file_name_without_dir= log_file + dirname_length(log_file);

    if (!(log_info= trans_thd->semisync_info))
    {
      /*
        Use my_malloc() to allocate the memory so THD can free it without
        having to know the Trans_binlog_info implementation.
      */
      void *mem= my_malloc(PSI_INSTRUMENT_ME,
                           sizeof(Trans_binlog_info), MYF(0));
      if (!mem)
        return 1;
      trans_thd->semisync_info= log_info=
        new(mem) Trans_binlog_info(file_name_without_dir, log_pos, gtid);
    }
    else
      log_info->set(file_name_without_dir, log_pos, gtid);
    return write_tranx_in_binlog(waiter_thd, log_info);
  }

  return 0;
}

int Repl_semi_sync_master::dump_start(THD* thd,
                                      const char *log_file,
                                      my_off_t log_pos,
                                      slave_connection_state *gtid_state)
{
  if (!thd->semi_sync_slave)
    return 0;

  if (ack_receiver.add_slave(thd))
  {
    sql_print_error("Failed to register slave to semi-sync ACK receiver "
                    "thread. Turning off semisync");
    thd->semi_sync_slave= 0;
    return 1;
  }

  add_slave();
  dump_start_inner(thd, log_file, log_pos, gtid_state);

  /* Mark that semi-sync net->pkt_nr is not reliable */
  thd->net.pkt_nr_can_be_reset= 1;
  return 0;
}


void
Repl_semi_sync_master_file_pos::dump_start_inner(THD* thd,
                                                 const char *log_file,
                                                 my_off_t log_pos,
                                                 slave_connection_state *gtid_state)
{
  report_reply_binlog_file_pos(thd->variables.server_id,
                               log_file + dirname_length(log_file), log_pos);
  sql_print_information("Start semi-sync binlog_dump to slave "
                        "(server_id: %ld), pos(%s, %lu)",
                        (long) thd->variables.server_id, log_file,
                        (ulong) log_pos);
}


void
Repl_semi_sync_master_gtid::dump_start_inner(THD* thd,
                                             const char *log_file,
                                             my_off_t log_pos,
                                             slave_connection_state *gtid_state)
{
  /*
    Find the latest GTID that this connecting slave has already ack'ed,
    if any.
  */
  rpl_gtid acked_gtid{0, 0, 0};
  bool any_acked= latest_gtid(gtid_state, &acked_gtid);
  report_reply_binlog_gtid(server_id, &acked_gtid, any_acked);
  sql_print_information("Start semi-sync binlog_dump to slave "
                        "(server_id: %ld), GTID %u-%u-%llu",
                        (long) thd->variables.server_id, acked_gtid.domain_id,
                        acked_gtid.server_id, (ulonglong)acked_gtid.seq_no);
}


bool
Repl_semi_sync_master_gtid::latest_gtid(slave_connection_state *gtid_state,
                                        rpl_gtid *out_gtid)
{
  bool found_any_gtid= false;
  rpl_gtid *gtid;
  Tranx_node *tranx_node;

  lock();

  if (m_commit_inf_inited)
  {
    /* Check if the slave is connecting at our current commit point. */
    gtid= gtid_state->find(m_commit_inf.gtid.domain_id);
    if (gtid &&
        gtid->server_id == m_commit_inf.gtid.server_id &&
        gtid->seq_no == m_commit_inf.gtid.seq_no)
    {
      *out_gtid= m_commit_inf.gtid;
      found_any_gtid= true;
      goto l_found;
    }
  }

  if (get_master_enabled())
  {
    /* Check if slave is connecting at any of our pending acks. */
    tranx_node= m_active_tranxs->find_latest(gtid_state);
    if (tranx_node)
    {
      *out_gtid= tranx_node->gtid;
      found_any_gtid= true;
    }
  }

l_found:
  unlock();
  return found_any_gtid;
}


void Repl_semi_sync_master::dump_end(THD* thd)
{
  if (!thd->semi_sync_slave)
    return;

  sql_print_information("Stop semi-sync binlog_dump to slave (server_id: %ld)",
                        (long) thd->variables.server_id);

  remove_slave();
  ack_receiver.remove_slave(thd);
}

int Repl_semi_sync_master::commit_trx(Repl_semi_sync_trx_info *inf)
{
  bool success= 0;
  DBUG_ENTER("Repl_semi_sync_master::commit_trx");

  if (!rpl_semi_sync_master_clients && !rpl_semi_sync_master_wait_no_slave)
  {
    lock();
    m_active_tranxs->unlink_thd_as_waiter(inf);
    unlock();
    rpl_semi_sync_master_no_transactions++;
    DBUG_RETURN(0);
  }

  if (get_master_enabled())
  {
    struct timespec start_ts;
    struct timespec abstime;
    int wait_result;
    PSI_stage_info old_stage;
    THD *thd= current_thd;
    bool aborted __attribute__((unused)) = 0;
    set_timespec(start_ts, 0);

    DEBUG_SYNC(thd, "rpl_semisync_master_commit_trx_before_lock");
    /* Acquire the mutex. */
    lock();

    /* This must be called after acquired the lock */
    THD_ENTER_COND(thd, &thd->COND_wakeup_ready, &LOCK_binlog,
                   &stage_waiting_for_semi_sync_ack_from_slave, &old_stage);

    /* This is the real check inside the mutex. */
    if (!get_master_enabled() || !is_on())
      goto l_end;

    DBUG_PRINT("semisync", ("%s: wait pos (%s, %lu) GTID %u-%u-%llu, repl(%d)",
                            "Repl_semi_sync_master::commit_trx",
                            inf->log_file, (ulong)inf->log_pos,
                            inf->gtid.domain_id, inf->gtid.server_id,
                            (ulonglong)inf->gtid.seq_no, (int)is_on()));

    while (is_on() && !(aborted= thd_killed(thd)))
    {
      /* We have to check these again as things may have changed */
      if (!rpl_semi_sync_master_clients && !rpl_semi_sync_master_wait_no_slave)
      {
        aborted= 1;
        break;
      }

      Tranx_node *tranx_entry= m_active_tranxs->is_thd_waiter(inf);
      /* In between the binlogging of this transaction and this wait, it is
       * possible that our entry in Active_tranx was removed (i.e. if a later
       * transaction already processed its ack, or if semi-sync was switched
       * off and on). It is also possible that the event was already sent to a
       * replica; however, we don't know if semi-sync was on or off at that
       * time, so an ACK may never come. So skip the wait. Note that
       * rpl_semi_sync_master_request_acks was already incremented in
       * report_binlog_update(), so to keep rpl_semi_sync_master_yes/no_tx
       * consistent with it, we check for a semi-sync restart _after_ checking
       * the reply state.
       */
      if (!tranx_entry)
      {
        DBUG_EXECUTE_IF(
          "semisync_log_skip_trx_wait",
          if (!m_reply_inf_inited ||
               m_active_tranxs->compare(inf, &m_reply_inf))
            sql_print_information(
                "Skipping semi-sync wait for transaction at pos %s, %lu "
                "GTID %u-%u-%llu. This could be because semi-sync turned off "
                "and on during the lifetime of this transaction, or simply "
                "because a later ack was received first.", inf->log_file,
                static_cast<unsigned long>(inf->log_pos),
                inf->gtid.domain_id, inf->gtid.server_id,
                (ulonglong)inf->gtid.seq_no););
        success= 1;
        break;
      }

      /*
        Mark that our THD is now valid for signalling to by the ack thread.
        It is important to ensure that we can never leave a no longer valid
        THD in the transaction list and signal it, eg. MDEV-36934. This way,
        we ensure the THD will only be signalled while this function is
        running, even in case of some incorrect error handling or similar
        that might leave a dangling THD in the list.
      */
      tranx_entry->thd_valid= true;

      /* In semi-synchronous replication, we wait until the binlog-dump
       * thread has received the reply on the relevant binlog segment from the
       * replication slave.
       *
       * Let us suspend this thread to wait on the condition;
       * when replication has progressed far enough, we will release
       * these waiting threads.
       */
      rpl_semi_sync_master_wait_sessions++;

      DBUG_PRINT("semisync", ("%s: wait %lu ms for binlog sent (%s, %lu)",
                              "Repl_semi_sync_master::commit_trx",
                              m_wait_timeout, inf->log_file,
                              (ulong)inf->log_pos));

      create_timeout(&abstime, &start_ts);
      wait_result= mysql_cond_timedwait(&thd->COND_wakeup_ready, &LOCK_binlog,
                                        &abstime);
      rpl_semi_sync_master_wait_sessions--;

      if (wait_result != 0)
      {
        /* This is a real wait timeout. */
        sql_print_warning("Timeout waiting for reply of binlog (file: %s, pos: %lu), "
                          "semi-sync up to file %s, position %lu GTID %u-%u-%llu.",
                          inf->log_file, (ulong)inf->log_pos,
                          m_reply_inf.log_file, (ulong)m_reply_inf.log_pos,
                          m_reply_inf.gtid.domain_id, m_reply_inf.gtid.server_id,
                          (ulonglong)m_reply_inf.gtid.seq_no);
        rpl_semi_sync_master_wait_timeouts++;

        /* switch semi-sync off */
        switch_off();
      }
      else
      {
        int wait_time;

        wait_time = get_wait_time(start_ts);
        if (wait_time < 0)
        {
          DBUG_PRINT("semisync", ("Replication semi-sync getWaitTime fail at "
                                  "wait position (%s, %lu)",
                                  inf->log_file, (ulong)inf->log_pos));
          rpl_semi_sync_master_timefunc_fails++;
        }
        else
        {
          rpl_semi_sync_master_trx_wait_num++;
          rpl_semi_sync_master_trx_wait_time += wait_time;

          DBUG_EXECUTE_IF("testing_cond_var_per_thd", {
            /*
              DBUG log warning to ensure we have either recieved our ACK; or
              have timed out and are awoken in an off state. Test
              rpl.rpl_semi_sync_cond_var_per_thd scans the logs to ensure this
              warning is not present.
            */
            bool valid_wakeup=
                (!get_master_enabled() || !is_on() || thd->is_killed() ||
                 tranx_entry != NULL);
            if (!valid_wakeup)
            {
              sql_print_warning(
                  "Thread awaiting semi-sync ACK was awoken before its "
                  "ACK. THD (%llu), Wait coord: (%s, %llu), ACK coord: (%s, "
                  "%llu  GTID %u-%u-%llu)",
                  thd->thread_id, inf->log_file, (ulonglong)inf->log_pos,
                  m_reply_inf.log_file, (ulonglong)m_reply_inf.log_pos,
                  m_reply_inf.gtid.domain_id, m_reply_inf.gtid.server_id,
                  (ulonglong)m_reply_inf.gtid.seq_no);
            }
          });
        }
      }
    }

    /*
      If our THD was killed (rather than awoken from an ACK) notify the
      Active_tranx cache that we are no longer waiting for the ACK, so nobody
      signals our COND var invalidly.
    */
    if (aborted)
      m_active_tranxs->unlink_thd_as_waiter(inf);

    /*
      At this point, the binlog file and position of this transaction
      must have been removed from Active_tranx.
      m_active_tranxs may be NULL if someone disabled semi sync during
      mysql_cond_timedwait
    */
    DBUG_ASSERT(aborted || !m_active_tranxs || m_active_tranxs->is_empty() ||
                !m_active_tranxs->is_thd_waiter(inf));

  l_end:
    /* Update the status counter. */
    if (success)
      rpl_semi_sync_master_yes_transactions++;
    else
      rpl_semi_sync_master_no_transactions++;

    /* The lock held will be released by thd_exit_cond, so no need to
       call unlock() here */
    THD_EXIT_COND(thd, &old_stage);
  }

  DBUG_RETURN(0);
}

/* Indicate that semi-sync replication is OFF now.
 *
 * What should we do when it is disabled?  The problem is that we want
 * the semi-sync replication enabled again when the slave catches up
 * later.  But, it is not that easy to detect that the slave has caught
 * up.  This is caused by the fact that MySQL's replication protocol is
 * asynchronous, meaning that if the master does not use the semi-sync
 * protocol, the slave would not send anything to the master.
 * Still, if the master is sending (N+1)-th event, we assume that it is
 * an indicator that the slave has received N-th event and earlier ones.
 *
 * If semi-sync is disabled, all transactions still update the wait
 * position with the last position in binlog.  But no transactions will
 * wait for confirmations and the active transaction list would not be
 * maintained.  In binlog dump thread, update_sync_header() checks whether
 * the current sending event catches up with last wait position.  If it
 * does match, semi-sync will be switched on again.
 */
void Repl_semi_sync_master::switch_off()
{
  DBUG_ENTER("Repl_semi_sync_master::switch_off");

  /* Clear the active transaction list. */
  if (m_active_tranxs)
    m_active_tranxs->clear_active_tranx_nodes(NULL);

  if (m_state)
  {
    m_state = false;


    rpl_semi_sync_master_off_times++;
    m_reply_inf_inited  = false;
    sql_print_information("Semi-sync replication switched OFF.");
  }
  DBUG_VOID_RETURN;
}

int Repl_semi_sync_master::try_switch_on(int server_id,
                                         const Repl_semi_sync_trx_info *inf)
{
  bool semi_sync_on = false;

  DBUG_ENTER("Repl_semi_sync_master::try_switch_on");

  /* If the current sending event's position is equal to the
   * current commit transaction binlog position, the slave is already
   * catching up now and we can switch semi-sync on here.
   * If m_commit_inf_inited indicates there are no recent transactions,
   * we can enable semi-sync immediately.
   */
  if (m_commit_inf_inited)
  {
    bool cmp= !inf || m_active_tranxs->compare(inf, &m_commit_inf);
    semi_sync_on= !cmp;
  }
  else
  {
    semi_sync_on = true;
  }

  if (semi_sync_on)
  {
    /* Switch semi-sync replication on. */
    m_state = true;

    if (inf)
      sql_print_information("Semi-sync replication switched ON with slave (server_id: %d) "
                            "at (%s, %lu) GTID %u-%u-%llu",
                            server_id, inf->log_file, (ulong)inf->log_pos,
                            inf->gtid.domain_id, inf->gtid.server_id,
                            (ulonglong)inf->gtid.seq_no);
    else
      sql_print_information("Semi-sync replication switched ON with slave (server_id: %d)",
                            server_id);
  }

  DBUG_RETURN(0);
}

int Repl_semi_sync_master::reserve_sync_header(String* packet)
{
  DBUG_ENTER("Repl_semi_sync_master::reserve_sync_header");

  /*
    Set the magic number and the sync status.  By default, no sync
    is required.
  */
  packet->append(reinterpret_cast<const char*>(k_sync_header),
                 sizeof(k_sync_header));
  DBUG_RETURN(0);
}

int Repl_semi_sync_master::update_sync_header(THD* thd, unsigned char *packet,
                                              const char *log_file_name,
                                              my_off_t log_file_pos,
                                              const rpl_gtid *gtid,
                                              bool* need_sync)
{
  bool sync = false;
  DBUG_ENTER("Repl_semi_sync_master::update_sync_header");

  /* If the semi-sync master is not enabled, or the slave is not a semi-sync
   * target, do not request replies from the slave.
   */
  if (!get_master_enabled() || !thd->semi_sync_slave)
  {
    *need_sync = false;
    DBUG_RETURN(0);
  }

  Repl_semi_sync_trx_info inf(log_file_name, log_file_pos, gtid);
  lock();

  /* This is the real check inside the mutex. */
  if (!get_master_enabled())
    goto l_end;

  if (is_on())
  {
    /* semi-sync is ON */
    Tranx_node *tranx_entry=
      m_active_tranxs->is_thd_waiter(&inf);
    if (!tranx_entry)
    {
      /* If we have already got the reply for the event, then we do
       * not need to sync the transaction again.
       */
      goto l_end;
    }
    sync= true;
  }
  else
  {
    if (m_commit_inf_inited)
    {
      bool cmp=  m_active_tranxs->compare(&inf, &m_commit_inf);
      sync =!cmp;
    }
    else
    {
      sync = true;
    }
  }

  DBUG_PRINT("semisync", ("%s: server(%lu), (%s, %lu) sync(%d), repl(%d)",
                          "Repl_semi_sync_master::update_sync_header",
                          thd->variables.server_id, log_file_name,
                          (ulong)log_file_pos, sync, (int)is_on()));

 l_end:
  unlock();
  *need_sync= sync;

  /*
    We do not need to clear sync flag in packet because we set it to 0 when we
    reserve the packet header.
  */
  if (sync)
    packet[2]= k_packet_flag_sync;

  DBUG_RETURN(0);
}


int
Repl_semi_sync_master_gtid::update_sync_header(THD* thd, unsigned char *packet,
                                               const char *log_file_name,
                                               my_off_t log_file_pos,
                                               const rpl_gtid *gtid,
                                               bool* need_sync)
{
  int res= Repl_semi_sync_master::update_sync_header(thd, packet, log_file_name,
                                                     log_file_pos, gtid,
                                                     need_sync);
  if (*need_sync)
    packet[2]|= k_packet_flag_gtid;
  return res;
}


int
Repl_semi_sync_master::write_tranx_in_binlog(THD *thd,
                                             const Repl_semi_sync_trx_info *inf)
{
  int result = 0;

  DBUG_ENTER("Repl_semi_sync_master::write_tranx_in_binlog");

  DEBUG_SYNC(current_thd, "semisync_at_write_tranx_in_binlog");

  lock();

  /* This is the real check inside the mutex. */
  if (!get_master_enabled())
    goto l_end;

  /* Update the current transaction commit position seen so far even
   * though semi-sync is switched off.
   * It is much better that we update m_commit_inf here, instead of
   * inside commit_trx().  This is mostly because update_sync_header()
   * will watch for m_commit_inf to decide whether to switch semi-sync
   * on. The detailed reason is explained in function update_sync_header().
   */
  if (m_commit_inf_inited)
  {
    /* Should not try to add the same position twice. */
    DBUG_ASSERT( m_active_tranxs->compare(inf, &m_commit_inf));
  }
  else
    m_commit_inf_inited = true;

  m_commit_inf.set(inf);

  if (is_on() &&
      (rpl_semi_sync_master_clients || rpl_semi_sync_master_wait_no_slave))
  {
    DBUG_ASSERT(m_active_tranxs != NULL);
    if(m_active_tranxs->insert_tranx_node(thd, inf))
    {
      /*
        if insert tranx_node failed, print a warning message
        and turn off semi-sync
      */
      sql_print_warning("Semi-sync failed to insert tranx_node for binlog "
                        "file: %s, position: %lu GTID %u-%u-%llu",
                        inf->log_file, (ulong)inf->log_pos, inf->gtid.domain_id,
                        inf->gtid.server_id, (ulonglong)inf->gtid.seq_no);
      switch_off();
    }
    else
    {
      rpl_semi_sync_master_request_ack++;

    }
  }

 l_end:
  unlock();

  DBUG_RETURN(result);
}

int Repl_semi_sync_master::flush_net(THD *thd,
                                     const char *event_buf)
{
  int      result = -1;
  NET* net= &thd->net;

  DBUG_ENTER("Repl_semi_sync_master::flush_net");

  DBUG_ASSERT((unsigned char)event_buf[1] == k_packet_magic_num);
  if ((unsigned char)event_buf[2] != k_packet_flag_sync)
  {
    /* current event does not require reply */
    result = 0;
    goto l_end;
  }

  /* We flush to make sure that the current event is sent to the network,
   * instead of being buffered in the TCP/IP stack.
   */
  if (net_flush(net))
  {
    sql_print_error("Semi-sync master failed on net_flush() "
                    "before waiting for slave reply");
    goto l_end;
  }

  /*
    We have to do a net_clear() as with semi-sync the slave_reply's are
    interleaved with data from the master and then the net->pkt_nr
    cannot be kept in sync. Better to start pkt_nr from 0 again.
  */
  net_clear(net, 0);
  net->pkt_nr++;
  net->compress_pkt_nr++;
  result = 0;
  rpl_semi_sync_master_net_wait_num++;

 l_end:
  thd->clear_error();

  DBUG_RETURN(result);
}

int Repl_semi_sync_master::after_reset_master()
{
  int result = 0;

  DBUG_ENTER("Repl_semi_sync_master::after_reset_master");

  if (rpl_semi_sync_master_enabled)
  {
    sql_print_information("Enable Semi-sync Master after reset master");
    enable_master();
  }

  lock();

  m_state = get_master_enabled() ? 1 : 0;

  m_reply_inf_inited  = false;
  m_commit_inf_inited = false;

  rpl_semi_sync_master_yes_transactions = 0;
  rpl_semi_sync_master_no_transactions = 0;
  rpl_semi_sync_master_off_times = 0;
  rpl_semi_sync_master_timefunc_fails = 0;
  rpl_semi_sync_master_wait_sessions = 0;
  rpl_semi_sync_master_wait_pos_backtraverse = 0;
  rpl_semi_sync_master_trx_wait_num = 0;
  rpl_semi_sync_master_trx_wait_time = 0;
  rpl_semi_sync_master_net_wait_num = 0;
  rpl_semi_sync_master_net_wait_time = 0;

  unlock();

  DBUG_RETURN(result);
}

int Repl_semi_sync_master::before_reset_master()
{
  int result = 0;

  DBUG_ENTER("Repl_semi_sync_master::before_reset_master");

  if (rpl_semi_sync_master_enabled)
    disable_master();

  DBUG_RETURN(result);
}

void Repl_semi_sync_master::set_export_stats()
{
  lock();

  rpl_semi_sync_master_status           = m_state;
  rpl_semi_sync_master_avg_trx_wait_time=
    ((rpl_semi_sync_master_trx_wait_num) ?
     (ulong)((double)rpl_semi_sync_master_trx_wait_time /
                     ((double)rpl_semi_sync_master_trx_wait_num)) : 0);
  rpl_semi_sync_master_avg_net_wait_time=
    ((rpl_semi_sync_master_net_wait_num) ?
     (ulong)((double)rpl_semi_sync_master_net_wait_time /
                     ((double)rpl_semi_sync_master_net_wait_num)) : 0);
  unlock();
}

void Repl_semi_sync_master::await_all_slave_replies(const char *msg)
{
  struct timespec timeout;
  int wait_result= 0;
  bool first= true;
  DBUG_ENTER("Repl_semi_sync_master::::await_all_slave_replies");

  /*
    Wait for all transactions that need ACKS to have received them; or timeout.
    If it is a timeout, the connection thread should attempt to turn off
    semi-sync and broadcast to all other waiting threads to move on.

    COND_binlog_send is only signalled after the Active_tranx cache has been
    emptied.
 */
  create_timeout(&timeout, NULL);
  lock();
  while (get_master_enabled() && is_on() && !m_active_tranxs->is_empty() && !wait_result)
  {
    if (msg && first)
    {
      first= false;
      sql_print_information("%s", msg);
    }

    wait_result=
        mysql_cond_timedwait(&COND_binlog_send, &LOCK_binlog, &timeout);
  }
  unlock();
  DBUG_VOID_RETURN;
}

/* Get the waiting time given the wait's staring time.
 *
 * Return:
 *  >= 0: the waiting time in microsecons(us)
 *   < 0: error in get time or time back traverse
 */
static int get_wait_time(const struct timespec& start_ts)
{
  ulonglong start_usecs, end_usecs;
  struct timespec end_ts;

  /* Starting time in microseconds(us). */
  start_usecs = timespec_to_usec(&start_ts);

  /* Get the wait time interval. */
  set_timespec(end_ts, 0);

  /* Ending time in microseconds(us). */
  end_usecs = timespec_to_usec(&end_ts);

  if (end_usecs < start_usecs)
    return -1;

  return (int)(end_usecs - start_usecs);
}

void semi_sync_master_deinit()
{
  if (repl_semisync_master)
    repl_semisync_master->cleanup();
  ack_receiver.cleanup();
}
