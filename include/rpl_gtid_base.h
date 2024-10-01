/* Copyright (c) 2013,2024, Kristian Nielsen and MariaDB Services Ab.

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

#ifndef RPL_GTID_BASE_H
#define RPL_GTID_BASE_H

#include "hash.h"


/* Definitions for MariaDB global transaction ID (GTID). */

struct slave_connection_state;

struct rpl_gtid
{
  uint32 domain_id;
  uint32 server_id;
  uint64 seq_no;
};

/*
  Binlog state.

  A binlog state records the last GTID written to the binlog for every
  distinct (domain_id, server_id) pair. Thus, each point in the binlog
  corresponds to a specific binlog state.

  When starting replication from a specific GTID position, the starting point
  is identified as the most recent one where the binlog state has no higher
  seq_no than the GTID position for any (domain_id, server_id) combination.

  We also remember the most recent logged GTID for every domain_id. This is
  used to know where to start when a master is changed to a slave. As a side
  effect, it also allows to skip a hash lookup in the very common case of
  logging a new GTID with same server id as last GTID.

  This base class rpl_binlog_state_base contains just be basic data operations
  to insert/update GTIDs, and is used eg. from Gtid_index_*.
*/
struct rpl_binlog_state_base
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
  my_bool initialized;

  rpl_binlog_state_base() : initialized(0) {}
  ~rpl_binlog_state_base();
  void init();
  void reset_nolock();
  void free();
  bool load_nolock(struct rpl_gtid *list, uint32 count);
  bool load_nolock(rpl_binlog_state_base *orig_state);
  int update_nolock(const struct rpl_gtid *gtid);
  int alloc_element_nolock(const rpl_gtid *gtid);
  uint32 count_nolock();
  int get_gtid_list_nolock(rpl_gtid *gtid_list, uint32 list_size);
  rpl_gtid *find_nolock(uint32 domain_id, uint32 server_id);
  bool is_before_pos(slave_connection_state *pos);
};


#endif  /* RPL_GTID_BASE_H */
