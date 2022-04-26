/* Copyright (c) 2013, Kristian Nielsen and MariaDB Services Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef RPL_GTID_H
#define RPL_GTID_H

#include "hash.h"
#include "queues.h"


/* Definitions for MariaDB global transaction ID (GTID). */


extern const LEX_STRING rpl_gtid_slave_state_table_name;

class String;
#define PARAM_GTID(G) G.domain_id, G.server_id, G.seq_no

struct rpl_gtid
{
  uint32 domain_id;
  uint32 server_id;
  uint64 seq_no;
};

inline bool operator==(const rpl_gtid& lhs, const rpl_gtid& rhs)
{
  return
    lhs.domain_id == rhs.domain_id &&
    lhs.server_id == rhs.server_id &&
    lhs.seq_no    == rhs.seq_no;
};

enum enum_gtid_skip_type {
  GTID_SKIP_NOT, GTID_SKIP_STANDALONE, GTID_SKIP_TRANSACTION
};


/*
  Structure to keep track of threads waiting in MASTER_GTID_WAIT().

  Since replication is (mostly) single-threaded, we want to minimise the
  performance impact on that from MASTER_GTID_WAIT(). To achieve this, we
  are careful to keep the common lock between replication threads and
  MASTER_GTID_WAIT threads held for as short as possible. We keep only
  a single thread waiting to be notified by the replication threads; this
  thread then handles all the (potentially heavy) lifting of dealing with
  all current waiting threads.
*/
struct gtid_waiting {
  /* Elements in the hash, basically a priority queue for each domain. */
  struct hash_element {
    QUEUE queue;
    uint32 domain_id;
  };
  /* A priority queue to handle waiters in one domain in seq_no order. */
  struct queue_element {
    uint64 wait_seq_no;
    THD *thd;
    int queue_idx;
    /*
      do_small_wait is true if we have responsibility for ensuring that there
      is a small waiter.
    */
    bool do_small_wait;
    /*
      The flag `done' is set when the wait is completed (either due to reaching
      the position waited for, or due to timeout or kill). The queue_element
      is in the queue if and only if `done' is true.
    */
    bool done;
  };

  mysql_mutex_t LOCK_gtid_waiting;
  HASH hash;

  void init();
  void destroy();
  hash_element *get_entry(uint32 domain_id);
  int wait_for_pos(THD *thd, String *gtid_str, longlong timeout_us);
  void promote_new_waiter(gtid_waiting::hash_element *he);
  int wait_for_gtid(THD *thd, rpl_gtid *wait_gtid, struct timespec *wait_until);
  void process_wait_hash(uint64 wakeup_seq_no, gtid_waiting::hash_element *he);
  int register_in_wait_queue(THD *thd, rpl_gtid *wait_gtid, hash_element *he,
                             queue_element *elem);
  void remove_from_wait_queue(hash_element *he, queue_element *elem);
};


class Relay_log_info;
struct rpl_group_info;
class Gtid_list_log_event;

/*
  Replication slave state.

  For every independent replication stream (identified by domain_id), this
  remembers the last gtid applied on the slave within this domain.

  Since events are always committed in-order within a single domain, this is
  sufficient to maintain the state of the replication slave.
*/
struct rpl_slave_state
{
  /* Elements in the list of GTIDs kept for each domain_id. */
  struct list_element
  {
    struct list_element *next;
    uint64 sub_id;
    uint64 seq_no;
    uint32 server_id;
  };

  /* Elements in the HASH that hold the state for one domain_id. */
  struct element
  {
    struct list_element *list;
    uint32 domain_id;
    /* Highest seq_no seen so far in this domain. */
    uint64 highest_seq_no;
    /*
      If this is non-NULL, then it is the waiter responsible for the small
      wait in MASTER_GTID_WAIT().
    */
    gtid_waiting::queue_element *gtid_waiter;
    /*
      If gtid_waiter is non-NULL, then this is the seq_no that its
      MASTER_GTID_WAIT() is waiting on. When we reach this seq_no, we need to
      signal the waiter on COND_wait_gtid.
    */
    uint64 min_wait_seq_no;
    mysql_cond_t COND_wait_gtid;

    /*
      For --gtid-ignore-duplicates. The Relay_log_info that currently owns
      this domain, and the number of worker threads that are active in it.

      The idea is that only one of multiple master connections is allowed to
      actively apply events for a given domain. Other connections must either
      discard the events (if the seq_no in GTID shows they have already been
      applied), or wait to see if the current owner will apply it.
    */
    const Relay_log_info *owner_rli;
    uint32 owner_count;
    mysql_cond_t COND_gtid_ignore_duplicates;

    list_element *grab_list() { list_element *l= list; list= NULL; return l; }
    void add(list_element *l)
    {
      l->next= list;
      list= l;
    }
  };

  /* Mapping from domain_id to its element. */
  HASH hash;
  /* Mutex protecting access to the state. */
  mysql_mutex_t LOCK_slave_state;
  /* Auxiliary buffer to sort gtid list. */
  DYNAMIC_ARRAY gtid_sort_array;

  uint64 last_sub_id;
  bool loaded;

  rpl_slave_state();
  ~rpl_slave_state();

  void truncate_hash();
  ulong count() const { return hash.records; }
  int update(uint32 domain_id, uint32 server_id, uint64 sub_id,
             uint64 seq_no, rpl_group_info *rgi);
  int truncate_state_table(THD *thd);
  int record_gtid(THD *thd, const rpl_gtid *gtid, uint64 sub_id,
                  rpl_group_info *rgi, bool in_statement);
  uint64 next_sub_id(uint32 domain_id);
  int iterate(int (*cb)(rpl_gtid *, void *), void *data,
              rpl_gtid *extra_gtids, uint32 num_extra,
              bool sort);
  int tostring(String *dest, rpl_gtid *extra_gtids, uint32 num_extra);
  bool domain_to_gtid(uint32 domain_id, rpl_gtid *out_gtid);
  int load(THD *thd, char *state_from_master, size_t len, bool reset,
           bool in_statement);
  bool is_empty();

  element *get_element(uint32 domain_id);
  int put_back_list(uint32 domain_id, list_element *list);

  void update_state_hash(uint64 sub_id, rpl_gtid *gtid, rpl_group_info *rgi);
  int record_and_update_gtid(THD *thd, struct rpl_group_info *rgi);
  int check_duplicate_gtid(rpl_gtid *gtid, rpl_group_info *rgi);
  void release_domain_owner(rpl_group_info *rgi);
};


/*
  Binlog state.
  This keeps the last GTID written to the binlog for every distinct
  (domain_id, server_id) pair.
  This will be logged at the start of the next binlog file as a
  Gtid_list_log_event; this way, it is easy to find the binlog file
  containing a given GTID, by simply scanning backwards from the newest
  one until a lower seq_no is found in the Gtid_list_log_event at the
  start of a binlog for the given domain_id and server_id.

  We also remember the last logged GTID for every domain_id. This is used
  to know where to start when a master is changed to a slave. As a side
  effect, it also allows to skip a hash lookup in the very common case of
  logging a new GTID with same server id as last GTID.
*/
struct rpl_binlog_state
{
  struct element {
    uint32 domain_id;
    HASH hash;                /* Containing all server_id for one domain_id */
    /* The most recent entry in the hash. */
    rpl_gtid *last_gtid;
    /* Counter to allocate next seq_no for this domain. */
    uint64 seq_no_counter;

    int update_element(const rpl_gtid *gtid);
  };
  /* Mapping from domain_id to collection of elements. */
  HASH hash;
  /* Mutex protecting access to the state. */
  mysql_mutex_t LOCK_binlog_state;
  my_bool initialized;

  /* Auxiliary buffer to sort gtid list. */
  DYNAMIC_ARRAY gtid_sort_array;

   rpl_binlog_state() :initialized(0) {}
  ~rpl_binlog_state();

  void init();
  void reset_nolock();
  void reset();
  void free();
  bool load(struct rpl_gtid *list, uint32 count);
  bool load(rpl_slave_state *slave_pos);
  int update_nolock(const struct rpl_gtid *gtid, bool strict);
  int update(const struct rpl_gtid *gtid, bool strict);
  int update_with_next_gtid(uint32 domain_id, uint32 server_id,
                             rpl_gtid *gtid);
  int alloc_element_nolock(const rpl_gtid *gtid);
  bool check_strict_sequence(uint32 domain_id, uint32 server_id, uint64 seq_no);
  int bump_seq_no_if_needed(uint32 domain_id, uint64 seq_no);
  int write_to_iocache(IO_CACHE *dest);
  int read_from_iocache(IO_CACHE *src);
  uint32 count();
  int get_gtid_list(rpl_gtid *gtid_list, uint32 list_size);
  int get_most_recent_gtid_list(rpl_gtid **list, uint32 *size);
  bool append_pos(String *str);
  bool append_state(String *str);
  rpl_gtid *find_nolock(uint32 domain_id, uint32 server_id);
  rpl_gtid *find(uint32 domain_id, uint32 server_id);
  rpl_gtid *find_most_recent(uint32 domain_id);
  const char* drop_domain(DYNAMIC_ARRAY *ids, Gtid_list_log_event *glev, char*);
};


/*
  Represent the GTID state that a slave connection to a master requests
  the master to start sending binlog events from.
*/
struct slave_connection_state
{
  struct entry {
    rpl_gtid gtid;
    uint32 flags;
  };
  static const uint32 START_OWN_SLAVE_POS= 0x1;
  static const uint32 START_ON_EMPTY_DOMAIN= 0x2;

  /* Mapping from domain_id to the entry with GTID requested for that domain. */
  HASH hash;

  /* Auxiliary buffer to sort gtid list. */
  DYNAMIC_ARRAY gtid_sort_array;

  slave_connection_state();
  ~slave_connection_state();

  void reset() { my_hash_reset(&hash); }
  int load(char *slave_request, size_t len);
  int load(const rpl_gtid *gtid_list, uint32 count);
  int load(rpl_slave_state *state, rpl_gtid *extra_gtids, uint32 num_extra);
  rpl_gtid *find(uint32 domain_id);
  entry *find_entry(uint32 domain_id);
  int update(const rpl_gtid *in_gtid);
  void remove(const rpl_gtid *gtid);
  void remove_if_present(const rpl_gtid *in_gtid);
  ulong count() const { return hash.records; }
  int to_string(String *out_str);
  int append_to_string(String *out_str);
  int get_gtid_list(rpl_gtid *gtid_list, uint32 list_size);
  bool is_pos_reached();
};


extern bool rpl_slave_state_tostring_helper(String *dest, const rpl_gtid *gtid,
                                            bool *first);
extern int gtid_check_rpl_slave_state_table(TABLE *table);
extern rpl_gtid *gtid_parse_string_to_list(const char *p, size_t len,
                                           uint32 *out_len);

#endif  /* RPL_GTID_H */
