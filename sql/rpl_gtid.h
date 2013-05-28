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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef RPL_GTID_H
#define RPL_GTID_H

/* Definitions for MariaDB global transaction ID (GTID). */


extern const LEX_STRING rpl_gtid_slave_state_table_name;

class String;

struct rpl_gtid
{
  uint32 domain_id;
  uint32 server_id;
  uint64 seq_no;
};


enum enum_gtid_skip_type {
  GTID_SKIP_NOT, GTID_SKIP_STANDALONE, GTID_SKIP_TRANSACTION
};


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
    uint64 last_sub_id;
    uint32 domain_id;

    list_element *grab_list() { list_element *l= list; list= NULL; return l; }
    void add(list_element *l)
    {
      l->next= list;
      list= l;
      if (last_sub_id < l->sub_id)
        last_sub_id= l->sub_id;
    }
  };

  /* Mapping from domain_id to its element. */
  HASH hash;
  /* Mutex protecting access to the state. */
  mysql_mutex_t LOCK_slave_state;

  bool inited;
  bool loaded;

  rpl_slave_state();
  ~rpl_slave_state();

  void init();
  void deinit();
  void truncate_hash();
  ulong count() const { return hash.records; }
  int update(uint32 domain_id, uint32 server_id, uint64 sub_id, uint64 seq_no);
  int truncate_state_table(THD *thd);
  int record_gtid(THD *thd, const rpl_gtid *gtid, uint64 sub_id,
                  bool in_transaction, bool in_statement);
  uint64 next_subid(uint32 domain_id);
  int tostring(String *dest, rpl_gtid *extra_gtids, uint32 num_extra);
  bool domain_to_gtid(uint32 domain_id, rpl_gtid *out_gtid);
  int load(THD *thd, char *state_from_master, size_t len, bool reset,
           bool in_statement);
  bool is_empty();

  void lock() { DBUG_ASSERT(inited); mysql_mutex_lock(&LOCK_slave_state); }
  void unlock() { DBUG_ASSERT(inited); mysql_mutex_unlock(&LOCK_slave_state); }

  element *get_element(uint32 domain_id);

  void update_state_hash(uint64 sub_id, rpl_gtid *gtid);
  int record_and_update_gtid(THD *thd, Relay_log_info *rli);
};


/*
  Binlog state.
  This keeps the last GTID written to the binlog for every distinct
  (domain_id, server_id) pair.
  This will be logged at the start of the next binlog file as a
  Gtid_list_log_event; this way, it is easy to find the binlog file
  containing a gigen GTID, by simply scanning backwards from the newest
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

  rpl_binlog_state();
  ~rpl_binlog_state();

  void reset();
  void free();
  bool load(struct rpl_gtid *list, uint32 count);
  int update(const struct rpl_gtid *gtid, bool strict);
  int update_with_next_gtid(uint32 domain_id, uint32 server_id,
                             rpl_gtid *gtid);
  int alloc_element(const rpl_gtid *gtid);
  bool check_strict_sequence(uint32 domain_id, uint32 server_id, uint64 seq_no);
  int bump_seq_no_if_needed(uint32 domain_id, uint64 seq_no);
  int write_to_iocache(IO_CACHE *dest);
  int read_from_iocache(IO_CACHE *src);
  uint32 count();
  int get_gtid_list(rpl_gtid *gtid_list, uint32 list_size);
  int get_most_recent_gtid_list(rpl_gtid **list, uint32 *size);
  bool append_pos(String *str);
  rpl_gtid *find(uint32 domain_id, uint32 server_id);
  rpl_gtid *find_most_recent(uint32 domain_id);
};


/*
  Represent the GTID state that a slave connection to a master requests
  the master to start sending binlog events from.
*/
struct slave_connection_state
{
  /* Mapping from domain_id to the GTID requested for that domain. */
  HASH hash;

  slave_connection_state();
  ~slave_connection_state();

  int load(char *slave_request, size_t len);
  int load(const rpl_gtid *gtid_list, uint32 count);
  rpl_gtid *find(uint32 domain_id);
  int update(const rpl_gtid *in_gtid);
  void remove(const rpl_gtid *gtid);
  ulong count() const { return hash.records; }
  int to_string(String *out_str);
  int append_to_string(String *out_str);
};

extern bool rpl_slave_state_tostring_helper(String *dest, const rpl_gtid *gtid,
                                            bool *first);
extern int gtid_check_rpl_slave_state_table(TABLE *table);

#endif  /* RPL_GTID_H */
