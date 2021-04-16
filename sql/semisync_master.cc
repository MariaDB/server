/* Copyright (C) 2007 Google Inc.
   Copyright (c) 2008, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2016, MariaDB

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

Repl_semi_sync_master repl_semisync_master;
Ack_receiver ack_receiver;

/*
  structure to save transaction log filename and position
*/
typedef struct Trans_binlog_info {
  my_off_t log_pos;
  char log_file[FN_REFLEN];
} Trans_binlog_info;

static int get_wait_time(const struct timespec& start_ts);

static ulonglong timespec_to_usec(const struct timespec *ts)
{
  return (ulonglong) ts->tv_sec * TIME_MILLION + ts->tv_nsec / TIME_THOUSAND;
}

/*******************************************************************************
 *
 * <Active_tranx> class : manage all active transaction nodes
 *
 ******************************************************************************/

Active_tranx::Active_tranx(mysql_mutex_t *lock,
                           ulong trace_level)
  : Trace(trace_level), m_allocator(max_connections),
    m_num_entries(max_connections << 1), /* Transaction hash table size
                                         * is set to double the size
                                         * of max_connections */
    m_lock(lock)
{
  /* No transactions are in the list initially. */
  m_trx_front = NULL;
  m_trx_rear  = NULL;

  /* Create the hash table to find a transaction's ending event. */
  m_trx_htb = new Tranx_node *[m_num_entries];
  for (int idx = 0; idx < m_num_entries; ++idx)
    m_trx_htb[idx] = NULL;

  sql_print_information("Semi-sync replication initialized for transactions.");
}

Active_tranx::~Active_tranx()
{
  delete [] m_trx_htb;
  m_trx_htb          = NULL;
  m_num_entries      = 0;
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

unsigned int Active_tranx::get_hash_value(const char *log_file_name,
                                          my_off_t    log_file_pos)
{
  unsigned int hash1 = calc_hash((const unsigned char *)log_file_name,
                                 strlen(log_file_name));
  unsigned int hash2 = calc_hash((const unsigned char *)(&log_file_pos),
                                 sizeof(log_file_pos));

  return (hash1 + hash2) % m_num_entries;
}

int Active_tranx::compare(const char *log_file_name1, my_off_t log_file_pos1,
                          const char *log_file_name2, my_off_t log_file_pos2)
{
  int cmp = strcmp(log_file_name1, log_file_name2);

  if (cmp != 0)
    return cmp;

  if (log_file_pos1 > log_file_pos2)
    return 1;
  else if (log_file_pos1 < log_file_pos2)
    return -1;
  return 0;
}

int Active_tranx::insert_tranx_node(const char *log_file_name,
                                    my_off_t log_file_pos)
{
  Tranx_node  *ins_node;
  int         result = 0;
  unsigned int        hash_val;

  DBUG_ENTER("Active_tranx:insert_tranx_node");

  ins_node = m_allocator.allocate_node();
  if (!ins_node)
  {
    sql_print_error("%s: transaction node allocation failed for: (%s, %lu)",
                    "Active_tranx:insert_tranx_node",
                    log_file_name, (ulong)log_file_pos);
    result = -1;
    goto l_end;
  }

  /* insert the binlog position in the active transaction list. */
  strncpy(ins_node->log_name, log_file_name, FN_REFLEN-1);
  ins_node->log_name[FN_REFLEN-1] = 0; /* make sure it ends properly */
  ins_node->log_pos = log_file_pos;

  if (!m_trx_front)
  {
    /* The list is empty. */
    m_trx_front = m_trx_rear = ins_node;
  }
  else
  {
    int cmp = compare(ins_node, m_trx_rear);
    if (cmp > 0)
    {
      /* Compare with the tail first.  If the transaction happens later in
       * binlog, then make it the new tail.
       */
      m_trx_rear->next = ins_node;
      m_trx_rear        = ins_node;
    }
    else
    {
      /* Otherwise, it is an error because the transaction should hold the
       * mysql_bin_log.LOCK_log when appending events.
       */
      sql_print_error("%s: binlog write out-of-order, tail (%s, %lu), "
                      "new node (%s, %lu)", "Active_tranx:insert_tranx_node",
                      m_trx_rear->log_name, (ulong)m_trx_rear->log_pos,
                      ins_node->log_name, (ulong)ins_node->log_pos);
      result = -1;
      goto l_end;
    }
  }

  hash_val = get_hash_value(ins_node->log_name, ins_node->log_pos);
  ins_node->hash_next = m_trx_htb[hash_val];
  m_trx_htb[hash_val]   = ins_node;

  DBUG_PRINT("semisync", ("%s: insert (%s, %lu) in entry(%u)",
                          "Active_tranx:insert_tranx_node",
                          ins_node->log_name, (ulong)ins_node->log_pos,
                          hash_val));
 l_end:

  DBUG_RETURN(result);
}

bool Active_tranx::is_tranx_end_pos(const char *log_file_name,
                                    my_off_t    log_file_pos)
{
  DBUG_ENTER("Active_tranx::is_tranx_end_pos");

  unsigned int hash_val = get_hash_value(log_file_name, log_file_pos);
  Tranx_node *entry = m_trx_htb[hash_val];

  while (entry != NULL)
  {
    if (compare(entry, log_file_name, log_file_pos) == 0)
      break;

    entry = entry->hash_next;
  }

  DBUG_PRINT("semisync", ("%s: probe (%s, %lu) in entry(%u)",
                          "Active_tranx::is_tranx_end_pos",
                          log_file_name, (ulong)log_file_pos, hash_val));

  DBUG_RETURN(entry != NULL);
}

void Active_tranx::clear_active_tranx_nodes(const char *log_file_name,
                                           my_off_t log_file_pos)
{
  Tranx_node *new_front;

  DBUG_ENTER("Active_tranx::::clear_active_tranx_nodes");

  if (log_file_name != NULL)
  {
    new_front = m_trx_front;

    while (new_front)
    {
      if (compare(new_front, log_file_name, log_file_pos) > 0)
        break;
      new_front = new_front->next;
    }
  }
  else
  {
    /* If log_file_name is NULL, clear everything. */
    new_front = NULL;
  }

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
    int n_frees = 0;
    curr_node = m_trx_front;
    while (curr_node != new_front)
    {
      next_node = curr_node->next;
      n_frees++;

      /* Remove the node from the hash table. */
      unsigned int hash_val = get_hash_value(curr_node->log_name, curr_node->log_pos);
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

  DBUG_VOID_RETURN;
}


/*******************************************************************************
 *
 * <Repl_semi_sync_master> class: the basic code layer for syncsync master.
 * <Repl_semi_sync_slave>  class: the basic code layer for syncsync slave.
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
    m_reply_file_name_inited(false),
    m_reply_file_pos(0L),
    m_wait_file_name_inited(false),
    m_wait_file_pos(0),
    m_master_enabled(false),
    m_wait_timeout(0L),
    m_state(0),
    m_wait_point(0)
{
  strcpy(m_reply_file_name, "");
  strcpy(m_wait_file_name, "");
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
    {
      result= ack_receiver.start(); /* Start the ACK thread. */
      /*
        If rpl_semi_sync_master_wait_no_slave is disabled, let's temporarily
        switch off semisync to avoid hang if there's none active slave.
      */
      if (!rpl_semi_sync_master_wait_no_slave)
        switch_off();
    }
  }
  else
  {
    disable_master();
  }

  return result;
}

int Repl_semi_sync_master::enable_master()
{
  int result = 0;

  /* Must have the lock when we do enable of disable. */
  lock();

  if (!get_master_enabled())
  {
    m_active_tranxs = new Active_tranx(&LOCK_binlog, m_trace_level);
    if (m_active_tranxs != NULL)
    {
      m_commit_file_name_inited = false;
      m_reply_file_name_inited  = false;
      m_wait_file_name_inited   = false;

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

    assert(m_active_tranxs != NULL);
    delete m_active_tranxs;
    m_active_tranxs = NULL;

    m_reply_file_name_inited = false;
    m_wait_file_name_inited  = false;
    m_commit_file_name_inited = false;

    set_master_enabled(false);
    sql_print_information("Semi-sync replication disabled on the master.");
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

void Repl_semi_sync_master::lock()
{
  mysql_mutex_lock(&LOCK_binlog);
}

void Repl_semi_sync_master::unlock()
{
  mysql_mutex_unlock(&LOCK_binlog);
}

void Repl_semi_sync_master::cond_broadcast()
{
  mysql_cond_broadcast(&COND_binlog_send);
}

int Repl_semi_sync_master::cond_timewait(struct timespec *wait_time)
{
  int wait_res;

  DBUG_ENTER("Repl_semi_sync_master::cond_timewait()");

  wait_res= mysql_cond_timedwait(&COND_binlog_send,
                                 &LOCK_binlog, wait_time);

  DBUG_RETURN(wait_res);
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
  rpl_semi_sync_master_clients--;

  /* Only switch off if semi-sync is enabled and is on */
  if (get_master_enabled() && is_on())
  {
    /* If user has chosen not to wait if no semi-sync slave available
       and the last semi-sync slave exits, turn off semi-sync on master
       immediately.
     */
    if (!rpl_semi_sync_master_wait_no_slave &&
        rpl_semi_sync_master_clients == 0)
      switch_off();
  }
  unlock();
}

int Repl_semi_sync_master::report_reply_packet(uint32 server_id,
                                               const uchar *packet,
                                               ulong packet_len)
{
  int result= -1;
  char log_file_name[FN_REFLEN+1];
  my_off_t log_file_pos;
  ulong log_file_len = 0;

  DBUG_ENTER("Repl_semi_sync_master::report_reply_packet");

  if (unlikely(packet[REPLY_MAGIC_NUM_OFFSET] !=
               Repl_semi_sync_master::k_packet_magic_num))
  {
    sql_print_error("Read semi-sync reply magic number error");
    goto l_end;
  }

  if (unlikely(packet_len < REPLY_BINLOG_NAME_OFFSET))
  {
    sql_print_error("Read semi-sync reply length error: packet is too small");
    goto l_end;
  }

  log_file_pos = uint8korr(packet + REPLY_BINLOG_POS_OFFSET);
  log_file_len = packet_len - REPLY_BINLOG_NAME_OFFSET;
  if (unlikely(log_file_len >= FN_REFLEN))
  {
    sql_print_error("Read semi-sync reply binlog file length too large");
    goto l_end;
  }
  strncpy(log_file_name, (const char*)packet + REPLY_BINLOG_NAME_OFFSET, log_file_len);
  log_file_name[log_file_len] = 0;

  DBUG_ASSERT(dirname_length(log_file_name) == 0);

  DBUG_PRINT("semisync", ("%s: Got reply(%s, %lu) from server %u",
                          "Repl_semi_sync_master::report_reply_packet",
                          log_file_name, (ulong)log_file_pos, server_id));

  rpl_semi_sync_master_get_ack++;
  report_reply_binlog(server_id, log_file_name, log_file_pos);

l_end:

  DBUG_RETURN(result);
}

int Repl_semi_sync_master::report_reply_binlog(uint32 server_id,
                                               const char *log_file_name,
                                               my_off_t log_file_pos)
{
  int   cmp;
  bool  can_release_threads = false;
  bool  need_copy_send_pos = true;

  DBUG_ENTER("Repl_semi_sync_master::report_reply_binlog");

  if (!(get_master_enabled()))
    DBUG_RETURN(0);

  lock();

  /* This is the real check inside the mutex. */
  if (!get_master_enabled())
    goto l_end;

  if (!is_on())
    /* We check to see whether we can switch semi-sync ON. */
    try_switch_on(server_id, log_file_name, log_file_pos);

  /* The position should increase monotonically, if there is only one
   * thread sending the binlog to the slave.
   * In reality, to improve the transaction availability, we allow multiple
   * sync replication slaves.  So, if any one of them get the transaction,
   * the transaction session in the primary can move forward.
   */
  if (m_reply_file_name_inited)
  {
    cmp = Active_tranx::compare(log_file_name, log_file_pos,
                               m_reply_file_name, m_reply_file_pos);

    /* If the requested position is behind the sending binlog position,
     * would not adjust sending binlog position.
     * We based on the assumption that there are multiple semi-sync slave,
     * and at least one of them shou/ld be up to date.
     * If all semi-sync slaves are behind, at least initially, the primary
     * can find the situation after the waiting timeout.  After that, some
     * slaves should catch up quickly.
     */
    if (cmp < 0)
    {
      /* If the position is behind, do not copy it. */
      need_copy_send_pos = false;
    }
  }

  if (need_copy_send_pos)
  {
    strmake_buf(m_reply_file_name, log_file_name);
    m_reply_file_pos = log_file_pos;
    m_reply_file_name_inited = true;

    /* Remove all active transaction nodes before this point. */
    assert(m_active_tranxs != NULL);
    m_active_tranxs->clear_active_tranx_nodes(log_file_name, log_file_pos);

    DBUG_PRINT("semisync", ("%s: Got reply at (%s, %lu)",
                            "Repl_semi_sync_master::report_reply_binlog",
                            log_file_name, (ulong)log_file_pos));
  }

  if (rpl_semi_sync_master_wait_sessions > 0)
  {
    /* Let us check if some of the waiting threads doing a trx
     * commit can now proceed.
     */
    cmp = Active_tranx::compare(m_reply_file_name, m_reply_file_pos,
                                m_wait_file_name, m_wait_file_pos);
    if (cmp >= 0)
    {
      /* Yes, at least one waiting thread can now proceed:
       * let us release all waiting threads with a broadcast
       */
      can_release_threads = true;
      m_wait_file_name_inited = false;
    }
  }

 l_end:
  unlock();

  if (can_release_threads)
  {
    DBUG_PRINT("semisync", ("%s: signal all waiting threads.",
                            "Repl_semi_sync_master::report_reply_binlog"));

    cond_broadcast();
  }

  DBUG_RETURN(0);
}

int Repl_semi_sync_master::wait_after_sync(const char *log_file, my_off_t log_pos)
{
  if (!get_master_enabled())
    return 0;

  int ret= 0;
  if(log_pos &&
     wait_point() == SEMI_SYNC_MASTER_WAIT_POINT_AFTER_BINLOG_SYNC)
    ret= commit_trx(log_file + dirname_length(log_file), log_pos);

  return ret;
}

int Repl_semi_sync_master::wait_after_commit(THD* thd, bool all)
{
  if (!get_master_enabled())
    return 0;

  int ret= 0;
  const char *log_file;
  my_off_t log_pos;

  bool is_real_trans=
    (all || thd->transaction->all.ha_list == 0);
  /*
    The coordinates are propagated to this point having been computed
    in report_binlog_update
  */
  Trans_binlog_info *log_info= thd->semisync_info;
  log_file= log_info && log_info->log_file[0] ? log_info->log_file : 0;
  log_pos= log_info ? log_info->log_pos : 0;

  DBUG_ASSERT(!log_file || dirname_length(log_file) == 0);

  if (is_real_trans &&
      log_pos &&
      wait_point() == SEMI_SYNC_MASTER_WAIT_POINT_AFTER_STORAGE_COMMIT)
    ret= commit_trx(log_file, log_pos);

  if (is_real_trans && log_info)
  {
    log_info->log_file[0]= 0;
    log_info->log_pos= 0;
  }

  return ret;
}

int Repl_semi_sync_master::wait_after_rollback(THD *thd, bool all)
{
  return wait_after_commit(thd, all);
}

/**
  The method runs after flush to binary log is done.
*/
int Repl_semi_sync_master::report_binlog_update(THD* thd, const char *log_file,
                                                my_off_t log_pos)
{
  if (get_master_enabled())
  {
    Trans_binlog_info *log_info;

    if (!(log_info= thd->semisync_info))
    {
      if(!(log_info= (Trans_binlog_info*)my_malloc(PSI_INSTRUMENT_ME,
                                            sizeof(Trans_binlog_info), MYF(0))))
        return 1;
      thd->semisync_info= log_info;
    }
    strcpy(log_info->log_file, log_file + dirname_length(log_file));
    log_info->log_pos = log_pos;

    return write_tranx_in_binlog(log_info->log_file, log_pos);
  }

  return 0;
}

int Repl_semi_sync_master::dump_start(THD* thd,
                                   const char *log_file,
                                   my_off_t log_pos)
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
  report_reply_binlog(thd->variables.server_id,
                      log_file + dirname_length(log_file), log_pos);
  sql_print_information("Start semi-sync binlog_dump to slave "
                        "(server_id: %ld), pos(%s, %lu)",
                        (long) thd->variables.server_id, log_file,
                        (ulong) log_pos);

  return 0;
}

void Repl_semi_sync_master::dump_end(THD* thd)
{
  if (!thd->semi_sync_slave)
    return;

  sql_print_information("Stop semi-sync binlog_dump to slave (server_id: %ld)",
                        (long) thd->variables.server_id);

  remove_slave();
  ack_receiver.remove_slave(thd);

  return;
}

int Repl_semi_sync_master::commit_trx(const char* trx_wait_binlog_name,
                                      my_off_t trx_wait_binlog_pos)
{
  DBUG_ENTER("Repl_semi_sync_master::commit_trx");

  if (get_master_enabled() && trx_wait_binlog_name)
  {
    struct timespec start_ts;
    struct timespec abstime;
    int wait_result;
    PSI_stage_info old_stage;
    THD *thd= current_thd;

    set_timespec(start_ts, 0);

    DEBUG_SYNC(thd, "rpl_semisync_master_commit_trx_before_lock");
    /* Acquire the mutex. */
    lock();

    /* This must be called after acquired the lock */
    THD_ENTER_COND(thd, &COND_binlog_send, &LOCK_binlog,
                   & stage_waiting_for_semi_sync_ack_from_slave,
                   & old_stage);

    /* This is the real check inside the mutex. */
    if (!get_master_enabled() || !is_on())
      goto l_end;

    DBUG_PRINT("semisync", ("%s: wait pos (%s, %lu), repl(%d)",
                            "Repl_semi_sync_master::commit_trx",
                            trx_wait_binlog_name, (ulong)trx_wait_binlog_pos,
                            (int)is_on()));

    while (is_on() && !thd_killed(thd))
    {
      if (m_reply_file_name_inited)
      {
        int cmp = Active_tranx::compare(m_reply_file_name, m_reply_file_pos,
                                        trx_wait_binlog_name,
                                        trx_wait_binlog_pos);
        if (cmp >= 0)
        {
          /* We have already sent the relevant binlog to the slave: no need to
           * wait here.
           */
          DBUG_PRINT("semisync", ("%s: Binlog reply is ahead (%s, %lu),",
                                  "Repl_semi_sync_master::commit_trx",
                                  m_reply_file_name,
                                  (ulong)m_reply_file_pos));
          break;
        }
      }

      /* Let us update the info about the minimum binlog position of waiting
       * threads.
       */
      if (m_wait_file_name_inited)
      {
        int cmp = Active_tranx::compare(trx_wait_binlog_name,
                                        trx_wait_binlog_pos,
                                        m_wait_file_name, m_wait_file_pos);
        if (cmp <= 0)
	{
          /* This thd has a lower position, let's update the minimum info. */
          strmake_buf(m_wait_file_name, trx_wait_binlog_name);
          m_wait_file_pos = trx_wait_binlog_pos;

          rpl_semi_sync_master_wait_pos_backtraverse++;
          DBUG_PRINT("semisync", ("%s: move back wait position (%s, %lu),",
                                  "Repl_semi_sync_master::commit_trx",
                                  m_wait_file_name, (ulong)m_wait_file_pos));
        }
      }
      else
      {
        strmake_buf(m_wait_file_name, trx_wait_binlog_name);
        m_wait_file_pos = trx_wait_binlog_pos;
        m_wait_file_name_inited = true;

        DBUG_PRINT("semisync", ("%s: init wait position (%s, %lu),",
                                "Repl_semi_sync_master::commit_trx",
                                m_wait_file_name, (ulong)m_wait_file_pos));
      }

      /* Calcuate the waiting period. */
      long diff_secs = (long) (m_wait_timeout / TIME_THOUSAND);
      long diff_nsecs = (long) ((m_wait_timeout % TIME_THOUSAND) * TIME_MILLION);
      long nsecs = start_ts.tv_nsec + diff_nsecs;
      abstime.tv_sec = start_ts.tv_sec + diff_secs + nsecs/TIME_BILLION;
      abstime.tv_nsec = nsecs % TIME_BILLION;

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
                              m_wait_timeout,
                              m_wait_file_name, (ulong)m_wait_file_pos));

      wait_result = cond_timewait(&abstime);
      rpl_semi_sync_master_wait_sessions--;

      if (wait_result != 0)
      {
        /* This is a real wait timeout. */
        sql_print_warning("Timeout waiting for reply of binlog (file: %s, pos: %lu), "
                          "semi-sync up to file %s, position %lu.",
                          trx_wait_binlog_name, (ulong)trx_wait_binlog_pos,
                          m_reply_file_name, (ulong)m_reply_file_pos);
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
                                  trx_wait_binlog_name,
                                  (ulong)trx_wait_binlog_pos));
          rpl_semi_sync_master_timefunc_fails++;
        }
        else
        {
          rpl_semi_sync_master_trx_wait_num++;
          rpl_semi_sync_master_trx_wait_time += wait_time;
        }
      }
    }

    /*
      At this point, the binlog file and position of this transaction
      must have been removed from Active_tranx.
      m_active_tranxs may be NULL if someone disabled semi sync during
      cond_timewait()
    */
    assert(thd_killed(thd) || !m_active_tranxs ||
           !m_active_tranxs->is_tranx_end_pos(trx_wait_binlog_name,
                                             trx_wait_binlog_pos));

  l_end:
    /* Update the status counter. */
    if (is_on())
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

  m_state = false;

  /* Clear the active transaction list. */
  assert(m_active_tranxs != NULL);
  m_active_tranxs->clear_active_tranx_nodes(NULL, 0);

  rpl_semi_sync_master_off_times++;
  m_wait_file_name_inited   = false;
  m_reply_file_name_inited  = false;
  sql_print_information("Semi-sync replication switched OFF.");
  cond_broadcast();                            /* wake up all waiting threads */

  DBUG_VOID_RETURN;
}

int Repl_semi_sync_master::try_switch_on(int server_id,
                                         const char *log_file_name,
                                         my_off_t log_file_pos)
{
  bool semi_sync_on = false;

  DBUG_ENTER("Repl_semi_sync_master::try_switch_on");

  /* If the current sending event's position is larger than or equal to the
   * 'largest' commit transaction binlog position, the slave is already
   * catching up now and we can switch semi-sync on here.
   * If m_commit_file_name_inited indicates there are no recent transactions,
   * we can enable semi-sync immediately.
   */
  if (m_commit_file_name_inited)
  {
    int cmp = Active_tranx::compare(log_file_name, log_file_pos,
                                   m_commit_file_name, m_commit_file_pos);
    semi_sync_on = (cmp >= 0);
  }
  else
  {
    semi_sync_on = true;
  }

  if (semi_sync_on)
  {
    /* Switch semi-sync replication on. */
    m_state = true;

    sql_print_information("Semi-sync replication switched ON with slave (server_id: %d) "
                          "at (%s, %lu)",
                          server_id, log_file_name,
                          (ulong)log_file_pos);
  }

  DBUG_RETURN(0);
}

int Repl_semi_sync_master::reserve_sync_header(String* packet)
{
  DBUG_ENTER("Repl_semi_sync_master::reserve_sync_header");

  /* Set the magic number and the sync status.  By default, no sync
   * is required.
   */
  packet->append(reinterpret_cast<const char*>(k_sync_header),
                 sizeof(k_sync_header));
  DBUG_RETURN(0);
}

int Repl_semi_sync_master::update_sync_header(THD* thd, unsigned char *packet,
                                              const char *log_file_name,
                                              my_off_t log_file_pos,
                                              bool* need_sync)
{
  int  cmp = 0;
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

  lock();

  /* This is the real check inside the mutex. */
  if (!get_master_enabled())
  {
    assert(sync == false);
    goto l_end;
  }

  if (is_on())
  {
    /* semi-sync is ON */
    sync = false;     /* No sync unless a transaction is involved. */

    if (m_reply_file_name_inited)
    {
      cmp = Active_tranx::compare(log_file_name, log_file_pos,
                                 m_reply_file_name, m_reply_file_pos);
      if (cmp <= 0)
      {
        /* If we have already got the reply for the event, then we do
         * not need to sync the transaction again.
         */
        goto l_end;
      }
    }

    if (m_wait_file_name_inited)
    {
      cmp = Active_tranx::compare(log_file_name, log_file_pos,
                                 m_wait_file_name, m_wait_file_pos);
    }
    else
    {
      cmp = 1;
    }

    /* If we are already waiting for some transaction replies which
     * are later in binlog, do not wait for this one event.
     */
    if (cmp >= 0)
    {
      /*
       * We only wait if the event is a transaction's ending event.
       */
      assert(m_active_tranxs != NULL);
      sync = m_active_tranxs->is_tranx_end_pos(log_file_name,
                                               log_file_pos);
    }
  }
  else
  {
    if (m_commit_file_name_inited)
    {
      int cmp = Active_tranx::compare(log_file_name, log_file_pos,
                                     m_commit_file_name, m_commit_file_pos);
      sync = (cmp >= 0);
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
  *need_sync= sync;

 l_end:
  unlock();

  /* We do not need to clear sync flag because we set it to 0 when we
   * reserve the packet header.
   */
  if (sync)
  {
    (packet)[2] = k_packet_flag_sync;
  }

  DBUG_RETURN(0);
}

int Repl_semi_sync_master::write_tranx_in_binlog(const char* log_file_name,
                                                 my_off_t log_file_pos)
{
  int result = 0;

  DBUG_ENTER("Repl_semi_sync_master::write_tranx_in_binlog");

  lock();

  /* This is the real check inside the mutex. */
  if (!get_master_enabled())
    goto l_end;

  /* Update the 'largest' transaction commit position seen so far even
   * though semi-sync is switched off.
   * It is much better that we update m_commit_file* here, instead of
   * inside commit_trx().  This is mostly because update_sync_header()
   * will watch for m_commit_file* to decide whether to switch semi-sync
   * on. The detailed reason is explained in function update_sync_header().
   */
  if (m_commit_file_name_inited)
  {
    int cmp = Active_tranx::compare(log_file_name, log_file_pos,
                                    m_commit_file_name, m_commit_file_pos);
    if (cmp > 0)
    {
      /* This is a larger position, let's update the maximum info. */
      strncpy(m_commit_file_name, log_file_name, FN_REFLEN-1);
      m_commit_file_name[FN_REFLEN-1] = 0; /* make sure it ends properly */
      m_commit_file_pos = log_file_pos;
    }
  }
  else
  {
    strncpy(m_commit_file_name, log_file_name, FN_REFLEN-1);
    m_commit_file_name[FN_REFLEN-1] = 0; /* make sure it ends properly */
    m_commit_file_pos = log_file_pos;
    m_commit_file_name_inited = true;
  }

  if (is_on())
  {
    assert(m_active_tranxs != NULL);
    if(m_active_tranxs->insert_tranx_node(log_file_name, log_file_pos))
    {
      /*
        if insert tranx_node failed, print a warning message
        and turn off semi-sync
      */
      sql_print_warning("Semi-sync failed to insert tranx_node for binlog file: %s, position: %lu",
                        log_file_name, (ulong)log_file_pos);
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

  assert((unsigned char)event_buf[1] == k_packet_magic_num);
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

  net_clear(net, 0);
  net->pkt_nr++;
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

  if (rpl_semi_sync_master_clients == 0 &&
      !rpl_semi_sync_master_wait_no_slave)
    m_state = 0;
  else
    m_state = get_master_enabled()? 1 : 0;

  m_wait_file_name_inited   = false;
  m_reply_file_name_inited  = false;
  m_commit_file_name_inited = false;

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

void Repl_semi_sync_master::check_and_switch()
{
  lock();
  if (get_master_enabled() && is_on())
  {
    if (!rpl_semi_sync_master_wait_no_slave
         && rpl_semi_sync_master_clients == 0)
      switch_off();
  }
  unlock();
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
  repl_semisync_master.cleanup();
  ack_receiver.cleanup();
}
