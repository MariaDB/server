/* Copyright (C) 2015 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */


#ifndef WSREP_SCHEMA_H
#define WSREP_SCHEMA_H

#include "mysqld.h"
#include "thr_lock.h" /* enum thr_lock_type */
#include "wsrep_api.h"
#include "wsrep_sr.h" /* class wsrep_SR_trx_info */

/*
  Forward decls
*/
class THD;
class TABLE;
struct st_mysql_lex_string;
typedef struct st_mysql_lex_string LEX_STRING;

class Wsrep_thd_pool;

class Wsrep_schema
{
 public:

  Wsrep_schema(Wsrep_thd_pool*);
  ~Wsrep_schema();

  /*
    Initialize wsrep schema. Storage engines must be running before
    calling this function.
  */
  int init();

  /*
    Store wsrep view info into wsrep schema.
  */
  int store_view(const wsrep_view_info_t*);

  /*
    Restore view info from stable storage.
  */
  int restore_view(const wsrep_uuid_t& node_uuid, wsrep_view_info_t**) const;

  /*
    Append transaction fragment to fragment storage.
    Starts a trx using a THD from thd_pool, does not commit.
    Should be followed by a call to update_frag_seqno(), or
    release_SR_thd() if wsrep->pre_commit() fails.
   */
  THD* append_frag(const wsrep_trx_meta_t&, uint32_t,
                   const unsigned char*, size_t);

  /*
    Update fragment sequence number and commits.
    Use in combination with append_frag().
   */
  int update_frag_seqno(THD* thd, const wsrep_trx_meta_t&);

  /*
    Rollback and release thd returned from append_frag().
   */
  void release_SR_thd(THD* thd);

  /*
    Append transaction fragment to fragment storage.
    Starts a trx using the given THD, does not commit.
   */
  int append_frag_apply(THD* thd, const wsrep_trx_meta_t&,
                        uint32_t, const unsigned char*, size_t);

  /*
    Append transaction fragment to fragment storage.
    Starts a trx using a THD from thd_pool and commits.
   */
  int append_frag_commit(const wsrep_trx_meta_t&, uint32_t,
                         const unsigned char*, size_t);

  /*
    Remove transaction from fragment storage in thd's transaction context
   */
  int remove_trx(THD* thd, wsrep_fragment_set* fragments);

  /*
    Remove transaction from fragment storage
   */
  int rollback_trx(THD* thd);

  /*
    Restore and apply all transaction fragments from fragment storage
  */
  int restore_frags();

  /*
    Replay a transaction from fragments stored in wsrep schema
   */
  int replay_trx(THD*, const wsrep_trx_meta_t&);

  /*
    Init TABLE_LIST entry for SR table
   */
  void init_SR_table(TABLE_LIST *table);

  /*
    Close wsrep schema.
  */
  void close();

 private:
  /* Non-copyable */
  Wsrep_schema(const Wsrep_schema&);
  Wsrep_schema& operator=(const Wsrep_schema&);

  Wsrep_thd_pool* thd_pool_;
};


#endif /* !WSREP_SCHEMA_H */
