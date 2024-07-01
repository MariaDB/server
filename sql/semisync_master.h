/* Copyright (C) 2007 Google Inc.
   Copyright (c) 2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */


#ifndef SEMISYNC_MASTER_H
#define SEMISYNC_MASTER_H

#include "semisync.h"
#include "semisync_master_ack_receiver.h"

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_LOCK_rpl_semi_sync_master_enabled;
extern PSI_mutex_key key_LOCK_binlog;
extern PSI_cond_key key_COND_binlog_send;
#endif

struct Tranx_node {
  char              log_name[FN_REFLEN];
  my_off_t          log_pos;
  THD               *thd;                   /* The thread awaiting an ACK */
  struct Tranx_node *next;            /* the next node in the sorted list */
  struct Tranx_node *hash_next;    /* the next node during hash collision */
};

/**
  @class Tranx_node_allocator

  This class provides memory allocating and freeing methods for
  Tranx_node. The main target is performance.

  @section ALLOCATE How to allocate a node
    The pointer of the first node after 'last_node' in current_block is
    returned. current_block will move to the next free Block when all nodes of
    it are in use. A new Block is allocated and is put into the rear of the
    Block link table if no Block is free.

    The list starts up empty (ie, there is no allocated Block).

    After some nodes are freed, there probably are some free nodes before
    the sequence of the allocated nodes, but we do not reuse it. It is better
    to keep the allocated nodes are in the sequence, for it is more efficient
    for allocating and freeing Tranx_node.

  @section FREENODE How to free nodes
    There are two methods for freeing nodes. They are free_all_nodes and
    free_nodes_before.

    'A Block is free' means all of its nodes are free.
    @subsection free_nodes_before
    As all allocated nodes are in the sequence, 'Before one node' means all
    nodes before given node in the same Block and all Blocks before the Block
    which containing the given node. As such, all Blocks before the given one
    ('node') are free Block and moved into the rear of the Block link table.
    The Block containing the given 'node', however, is not. For at least the
    given 'node' is still in use. This will waste at most one Block, but it is
    more efficient.
 */
#define BLOCK_TRANX_NODES 16
class Tranx_node_allocator
{
public:
  /**
    @param reserved_nodes
      The number of reserved Tranx_nodes. It is used to set 'reserved_blocks'
      which can contain at least 'reserved_nodes' number of Tranx_nodes.  When
      freeing memory, we will reserve at least reserved_blocks of Blocks not
      freed.
   */
  Tranx_node_allocator(uint reserved_nodes) :
    reserved_blocks(reserved_nodes/BLOCK_TRANX_NODES +
                  (reserved_nodes%BLOCK_TRANX_NODES > 1 ? 2 : 1)),
    first_block(NULL), last_block(NULL),
    current_block(NULL), last_node(-1), block_num(0) {}

  ~Tranx_node_allocator()
  {
    Block *block= first_block;
    while (block != NULL)
    {
      Block *next= block->next;
      free_block(block);
      block= next;
    }
  }

  /**
    The pointer of the first node after 'last_node' in current_block is
    returned. current_block will move to the next free Block when all nodes of
    it are in use. A new Block is allocated and is put into the rear of the
    Block link table if no Block is free.

    @return Return a Tranx_node *, or NULL if an error occurred.
   */
  Tranx_node *allocate_node()
  {
    Tranx_node *trx_node;
    Block *block= current_block;

    if (last_node == BLOCK_TRANX_NODES-1)
    {
      current_block= current_block->next;
      last_node= -1;
    }

    if (current_block == NULL && allocate_block())
    {
      current_block= block;
      if (current_block)
        last_node= BLOCK_TRANX_NODES-1;
      return NULL;
    }

    trx_node= &(current_block->nodes[++last_node]);
    trx_node->log_name[0] = '\0';
    trx_node->log_pos= 0;
    trx_node->next= 0;
    trx_node->hash_next= 0;
    return trx_node;
  }

  /**
    All nodes are freed.

    @return Return 0, or 1 if an error occurred.
   */
  int free_all_nodes()
  {
    current_block= first_block;
    last_node= -1;
    free_blocks();
    return 0;
  }

  /**
    All Blocks before the given 'node' are free Block and moved into the rear
    of the Block link table.

    @param node All nodes before 'node' will be freed

    @return Return 0, or 1 if an error occurred.
   */
  int free_nodes_before(Tranx_node* node)
  {
    Block *block;
    Block *prev_block= NULL;

    block= first_block;
    while (block != current_block->next)
    {
      /* Find the Block containing the given node */
      if (&(block->nodes[0]) <= node && &(block->nodes[BLOCK_TRANX_NODES]) >= node)
      {
        /* All Blocks before the given node are put into the rear */
        if (first_block != block)
        {
          last_block->next= first_block;
          first_block= block;
          last_block= prev_block;
          last_block->next= NULL;
          free_blocks();
        }
        return 0;
      }
      prev_block= block;
      block= block->next;
    }

    /* Node does not find should never happen */
    DBUG_ASSERT(0);
    return 1;
  }

private:
  uint reserved_blocks;

 /**
   A sequence memory which contains BLOCK_TRANX_NODES Tranx_nodes.

   BLOCK_TRANX_NODES The number of Tranx_nodes which are in a Block.

   next Every Block has a 'next' pointer which points to the next Block.
        These linking Blocks constitute a Block link table.
  */
  struct Block {
    Block *next;
    Tranx_node nodes[BLOCK_TRANX_NODES];
  };

  /**
    The 'first_block' is the head of the Block link table;
   */
  Block *first_block;
  /**
    The 'last_block' is the rear of the Block link table;
   */
  Block *last_block;

  /**
    current_block always points the Block in the Block link table in
    which the last allocated node is. The Blocks before it are all in use
    and the Blocks after it are all free.
   */
  Block *current_block;

  /**
    It always points to the last node which has been allocated in the
    current_block.
   */
  int last_node;

  /**
    How many Blocks are in the Block link table.
   */
  uint block_num;

  /**
    Allocate a block and then assign it to current_block.
  */
  int allocate_block()
  {
    Block *block= (Block *)my_malloc(PSI_INSTRUMENT_ME, sizeof(Block), MYF(0));
    if (block)
    {
      block->next= NULL;

      if (first_block == NULL)
        first_block= block;
      else
        last_block->next= block;

      /* New Block is always put into the rear */
      last_block= block;
      /* New Block is always the current_block */
      current_block= block;
      ++block_num;
      return 0;
    }
    return 1;
  }

  /**
    Free a given Block.
    @param block The Block will be freed.
   */
  void free_block(Block *block)
  {
    my_free(block);
    --block_num;
  }


  /**
    If there are some free Blocks and the total number of the Blocks in the
    Block link table is larger than the 'reserved_blocks', Some free Blocks
    will be freed until the total number of the Blocks is equal to the
    'reserved_blocks' or there is only one free Block behind the
    'current_block'.
   */
  void free_blocks()
  {
    if (current_block == NULL || current_block->next == NULL)
      return;

    /* One free Block is always kept behind the current block */
    Block *block= current_block->next->next;
    while (block_num > reserved_blocks && block != NULL)
    {
      Block *next= block->next;
      free_block(block);
      block= next;
    }
    current_block->next->next= block;
    if (block == NULL)
      last_block= current_block->next;
  }
};

/**
  Function pointer type to run on the contents of an Active_tranx node.

  Return 0 for success, 1 for error.

  Note Repl_semi_sync_master::LOCK_binlog is not guaranteed to be held for
  its invocation. See the context in which it is called to know.
*/

typedef int (*active_tranx_action)(THD *trx_thd, const char *log_file_name,
                                   my_off_t trx_log_file_pos);

/**
   This class manages memory for active transaction list.

   We record each active transaction with a Tranx_node, each session
   can have only one open transaction. Because of EVENT, the total
   active transaction nodes can exceed the maximum allowed
   connections.
*/
class Active_tranx
  :public Trace {
private:

  Tranx_node_allocator m_allocator;
  /* These two record the active transaction list in sort order. */
  Tranx_node       *m_trx_front, *m_trx_rear;

  Tranx_node      **m_trx_htb;        /* A hash table on active transactions. */

  int              m_num_entries;              /* maximum hash table entries */
  mysql_mutex_t *m_lock;                                     /* mutex lock */
  mysql_cond_t  *m_cond_empty;    /* signalled when cleared all Tranx_node */

  inline void assert_lock_owner();

  inline unsigned int calc_hash(const unsigned char *key, size_t length);
  unsigned int get_hash_value(const char *log_file_name, my_off_t log_file_pos);

  int compare(const char *log_file_name1, my_off_t log_file_pos1,
              const Tranx_node *node2) {
    return compare(log_file_name1, log_file_pos1,
                   node2->log_name, node2->log_pos);
  }
  int compare(const Tranx_node *node1,
              const char *log_file_name2, my_off_t log_file_pos2) {
    return compare(node1->log_name, node1->log_pos,
                   log_file_name2, log_file_pos2);
  }
  int compare(const Tranx_node *node1, const Tranx_node *node2) {
    return compare(node1->log_name, node1->log_pos,
                   node2->log_name, node2->log_pos);
  }

public:
  Active_tranx(mysql_mutex_t *lock, mysql_cond_t *cond,
               unsigned long trace_level);
  ~Active_tranx();

  /* Insert an active transaction node with the specified position.
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int insert_tranx_node(THD *thd_to_wait, const char *log_file_name,
                        my_off_t log_file_pos);

  /* Clear the active transaction nodes until(inclusive) the specified
   * position.
   * If log_file_name is NULL, everything will be cleared: the sorted
   * list and the hash table will be reset to empty.
   *
   * The pre_delete_hook parameter is a function pointer that will be invoked
   * for each Active_tranx node, in order, from m_trx_front to m_trx_rear,
   * e.g. to signal their wakeup condition. Repl_semi_sync_binlog::LOCK_binlog
   * is held while this is invoked.
   */
  void clear_active_tranx_nodes(const char *log_file_name,
                                my_off_t log_file_pos,
                                active_tranx_action pre_delete_hook);

  /* Unlinks a thread from a Tranx_node, so it will not be referenced/signalled
   * if it is separately killed. Note that this keeps the Tranx_node itself in
   * the cache so it can still be awaited by await_all_slave_replies(), e.g.
   * as is done by SHUTDOWN WAIT FOR ALL SLAVES.
   */
  void unlink_thd_as_waiter(const char *log_file_name, my_off_t log_file_pos);

#ifndef DBUG_OFF
  /* Uses DBUG_ASSERT statements to ensure that the argument thd_to_check
   * matches the thread of the respective Tranx_node::thd of the passed in
   * log_file_name and log_file_pos.
   */
  void assert_thd_is_waiter(THD *thd_to_check, const char *log_file_name,
                            my_off_t log_file_pos);
#endif

  /* Given a position, check to see whether the position is an active
   * transaction's ending position by probing the hash table.
   */
  bool is_tranx_end_pos(const char *log_file_name, my_off_t log_file_pos);

  /* Given two binlog positions, compare which one is bigger based on
   * (file_name, file_position).
   */
  static int compare(const char *log_file_name1, my_off_t log_file_pos1,
                     const char *log_file_name2, my_off_t log_file_pos2);


  /* Check if there are no transactions actively awaiting ACKs. Returns true
   * if the internal linked list has no entries, false otherwise.
   */
  bool is_empty() { return m_trx_front == NULL; }

};

/**
   The extension class for the master of semi-synchronous replication
*/
class Repl_semi_sync_master
  :public Repl_semi_sync_base {
  Active_tranx    *m_active_tranxs;  /* active transaction list: the list will
                                      be cleared when semi-sync switches off. */

  /* True when init_object has been called */
  bool m_init_done;

  /* This cond variable is signaled when enough binlog has been sent to slave,
   * so that a waiting trx can return the 'ok' to the client for a commit.
   */
  mysql_cond_t  COND_binlog_send;

  /* Mutex that protects the following state variables and the active
   * transaction list.
   * Under no cirumstances we can acquire mysql_bin_log.LOCK_log if we are
   * already holding m_LOCK_binlog because it can cause deadlocks.
   */
  mysql_mutex_t LOCK_binlog;

  /* This is set to true when m_reply_file_name contains meaningful data. */
  bool            m_reply_file_name_inited;

  /* The binlog name up to which we have received replies from any slaves. */
  char            m_reply_file_name[FN_REFLEN];

  /* The position in that file up to which we have the reply from any slaves. */
  my_off_t        m_reply_file_pos;

  /* This is set to true when we know the 'smallest' wait position. */
  bool            m_wait_file_name_inited;

  /* NULL, or the 'smallest' filename that a transaction is waiting for
   * slave replies.
   */
  char            m_wait_file_name[FN_REFLEN];

  /* The smallest position in that file that a trx is waiting for: the trx
   * can proceed and send an 'ok' to the client when the master has got the
   * reply from the slave indicating that it already got the binlog events.
   */
  my_off_t        m_wait_file_pos;

  /* This is set to true when we know the 'largest' transaction commit
   * position in the binlog file.
   * We always maintain the position no matter whether semi-sync is switched
   * on switched off.  When a transaction wait timeout occurs, semi-sync will
   * switch off.  Binlog-dump thread can use the three fields to detect when
   * slaves catch up on replication so that semi-sync can switch on again.
   */
  bool            m_commit_file_name_inited;

  /* The 'largest' binlog filename that a commit transaction is seeing.       */
  char            m_commit_file_name[FN_REFLEN];

  /* The 'largest' position in that file that a commit transaction is seeing. */
  my_off_t        m_commit_file_pos;

  /* All global variables which can be set by parameters. */
  volatile bool            m_master_enabled;      /* semi-sync is enabled on the master */
  unsigned long           m_wait_timeout;      /* timeout period(ms) during tranx wait */

  bool            m_state;                    /* whether semi-sync is switched */

  /*Waiting for ACK before/after innodb commit*/
  ulong m_wait_point;

  void lock();
  void unlock();

  /* Is semi-sync replication on? */
  bool is_on() {
    return (m_state);
  }

  void set_master_enabled(bool enabled) {
    m_master_enabled = enabled;
  }

  /* Switch semi-sync off because of timeout in transaction waiting. */
  void switch_off();

  /* Switch semi-sync on when slaves catch up. */
  int try_switch_on(int server_id,
                    const char *log_file_name, my_off_t log_file_pos);

 public:
  Repl_semi_sync_master();
  ~Repl_semi_sync_master() = default;

  void cleanup();

  bool get_master_enabled() {
    return m_master_enabled;
  }
  void set_trace_level(unsigned long trace_level) {
    m_trace_level = trace_level;
    if (m_active_tranxs)
      m_active_tranxs->m_trace_level = trace_level;
  }

  /* Set the transaction wait timeout period, in milliseconds. */
  void set_wait_timeout(unsigned long wait_timeout) {
    m_wait_timeout = wait_timeout;
  }

  /*
    Calculates a timeout that is m_wait_timeout after start_arg and saves it
    in out. If start_arg is NULL, the timeout is m_wait_timeout after the
    current system time.
  */
  void create_timeout(struct timespec *out, struct timespec *start_arg);

  /*
    Blocks the calling thread until the ack_receiver either receives ACKs for
    all transactions awaiting ACKs, or times out (from
    rpl_semi_sync_master_timeout).

    If info_msg is provided, it will be output via sql_print_information when
    there are transactions awaiting ACKs; info_msg is not output if there are
    no transasctions to await.
  */
  void await_all_slave_replies(const char *msg);

  /*set the ACK point, after binlog sync or after transaction commit*/
  void set_wait_point(unsigned long ack_point)
  {
    m_wait_point = ack_point;
  }

  ulong wait_point() //no cover line
  {
    return m_wait_point; //no cover line
  }

  /* Initialize this class after MySQL parameters are initialized. this
   * function should be called once at bootstrap time.
   */
  int init_object();

  /* Enable the object to enable semi-sync replication inside the master. */
  int enable_master();

  /* Disable the object to disable semi-sync replication inside the master. */
  void disable_master();

  /* Add a semi-sync replication slave */
  void add_slave();
    
  /* Remove a semi-sync replication slave */
  void remove_slave();

  /* It parses a reply packet and call report_reply_binlog to handle it. */
  int report_reply_packet(uint32 server_id, const uchar *packet,
                        ulong packet_len);

  /* In semi-sync replication, reports up to which binlog position we have
   * received replies from the slave indicating that it already get the events.
   *
   * Input:
   *  server_id     - (IN)  master server id number
   *  log_file_name - (IN)  binlog file name
   *  end_offset    - (IN)  the offset in the binlog file up to which we have
   *                        the replies from the slave
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int report_reply_binlog(uint32 server_id,
                          const char* log_file_name,
                          my_off_t end_offset);

  /* Commit a transaction in the final step.  This function is called from
   * InnoDB before returning from the low commit.  If semi-sync is switch on,
   * the function will wait to see whether binlog-dump thread get the reply for
   * the events of the transaction.  Remember that this is not a direct wait,
   * instead, it waits to see whether the binlog-dump thread has reached the
   * point.  If the wait times out, semi-sync status will be switched off and
   * all other transaction would not wait either.
   *
   * Input:  (the transaction events' ending binlog position)
   *  trx_wait_binlog_name - (IN)  ending position's file name
   *  trx_wait_binlog_pos  - (IN)  ending position's file offset
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int commit_trx(const char* trx_wait_binlog_name,
                 my_off_t trx_wait_binlog_pos);

  /*Wait for ACK after writing/sync binlog to file*/
  int wait_after_sync(const char* log_file, my_off_t log_pos);

  /*Wait for ACK after commting the transaction*/
  int wait_after_commit(THD* thd, bool all);

  /*Wait after the transaction is rollback*/
  int wait_after_rollback(THD *thd, bool all);
  /* Store the current binlog position in m_active_tranxs. This position should
   * be acked by slave.
   *
   * Inputs:
   *   trans_thd  Thread of the transaction which is executing the
   *              transaction.
   *   waiter_thd Thread that will wait for the ACK from the replica,
   *              which depends on the semi-sync wait point. If AFTER_SYNC,
   *              and also using binlog group commit, this will be the leader
   *              thread of the binlog commit. Otherwise, it is the thread that
   *              is executing the transaction, i.e. the same as trans_thd.
   *   log_file   Name of the binlog file that the transaction is written into
   *   log_pos    Offset within the binlog file that the transaction is written
   *              at
   */
  int report_binlog_update(THD *trans_thd, THD *waiter_thd,
                           const char *log_file, my_off_t log_pos);

  int dump_start(THD* thd,
                  const char *log_file,
                  my_off_t log_pos);

  void dump_end(THD* thd);

  /* Reserve space in the replication event packet header:
   *  . slave semi-sync off: 1 byte - (0)
   *  . slave semi-sync on:  3 byte - (0, 0xef, 0/1}
   *
   * Input:
   *  packet   - (IN)  the header buffer
   *
   * Return:
   *  size of the bytes reserved for header
   */
  int reserve_sync_header(String* packet);

  /* Update the sync bit in the packet header to indicate to the slave whether
   * the master will wait for the reply of the event.  If semi-sync is switched
   * off and we detect that the slave is catching up, we switch semi-sync on.
   * 
   * Input:
   *  THD           - (IN)  current dump thread
   *  packet        - (IN)  the packet containing the replication event
   *  log_file_name - (IN)  the event ending position's file name
   *  log_file_pos  - (IN)  the event ending position's file offset
   *  need_sync     - (IN)  identify if flush_net is needed to call.
   *  server_id     - (IN)  master server id number
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int update_sync_header(THD* thd, unsigned char *packet,
                         const char *log_file_name,
                         my_off_t log_file_pos,
                         bool* need_sync);

  /* Called when a transaction finished writing binlog events.
   *  . update the 'largest' transactions' binlog event position
   *  . insert the ending position in the active transaction list if
   *    semi-sync is on
   *
   * Input:  (the transaction events' ending binlog position)
   *  THD           - (IN)  thread that will wait for an ACK. This can be the
   *                        binlog leader thread when using wait_point
   *                        AFTER_SYNC with binlog group commit. In all other
   *                        cases, this is the user thread executing the
   *                        transaction.
   *  log_file_name - (IN)  transaction ending position's file name
   *  log_file_pos  - (IN)  transaction ending position's file offset
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int write_tranx_in_binlog(THD *thd, const char *log_file_name,
                            my_off_t log_file_pos);

  /* Read the slave's reply so that we know how much progress the slave makes
   * on receive replication events.
   */
  int flush_net(THD* thd, const char *event_buf);

  /* Export internal statistics for semi-sync replication. */
  void set_export_stats();

  /* 'reset master' command is issued from the user and semi-sync need to
   * go off for that.
   */
  int after_reset_master();

  /*called before reset master*/
  int before_reset_master();

  mysql_mutex_t LOCK_rpl_semi_sync_master_enabled;
};

enum rpl_semi_sync_master_wait_point_t {
  SEMI_SYNC_MASTER_WAIT_POINT_AFTER_BINLOG_SYNC,
  SEMI_SYNC_MASTER_WAIT_POINT_AFTER_STORAGE_COMMIT,
};

extern Repl_semi_sync_master repl_semisync_master;
extern Ack_receiver ack_receiver;

/* System and status variables for the master component */
extern my_bool rpl_semi_sync_master_enabled;
extern my_bool rpl_semi_sync_master_status;
extern ulong rpl_semi_sync_master_wait_point;
extern ulong rpl_semi_sync_master_clients;
extern ulong rpl_semi_sync_master_timeout;
extern ulong rpl_semi_sync_master_trace_level;
extern ulong rpl_semi_sync_master_yes_transactions;
extern ulong rpl_semi_sync_master_no_transactions;
extern ulong rpl_semi_sync_master_off_times;
extern ulong rpl_semi_sync_master_wait_timeouts;
extern ulong rpl_semi_sync_master_timefunc_fails;
extern ulong rpl_semi_sync_master_num_timeouts;
extern ulong rpl_semi_sync_master_wait_sessions;
extern ulong rpl_semi_sync_master_wait_pos_backtraverse;
extern ulong rpl_semi_sync_master_avg_trx_wait_time;
extern ulong rpl_semi_sync_master_avg_net_wait_time;
extern ulonglong rpl_semi_sync_master_net_wait_num;
extern ulonglong rpl_semi_sync_master_trx_wait_num;
extern ulonglong rpl_semi_sync_master_net_wait_time;
extern ulonglong rpl_semi_sync_master_trx_wait_time;
extern unsigned long long rpl_semi_sync_master_request_ack;
extern unsigned long long rpl_semi_sync_master_get_ack;

/*
  This indicates whether we should keep waiting if no semi-sync slave
  is available.
     0           : stop waiting if detected no avaialable semi-sync slave.
     1 (default) : keep waiting until timeout even no available semi-sync slave.
*/
extern char rpl_semi_sync_master_wait_no_slave;
extern Repl_semi_sync_master repl_semisync_master;

extern PSI_stage_info stage_waiting_for_semi_sync_ack_from_slave;
extern PSI_stage_info stage_reading_semi_sync_ack;
extern PSI_stage_info stage_waiting_for_semi_sync_slave;

void semi_sync_master_deinit();

#endif /* SEMISYNC_MASTER_H */
