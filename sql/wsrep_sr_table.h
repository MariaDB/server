/* Copyright (C) 2013 Codership Oy <info@codership.com>

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


#include "wsrep_sr.h"

class Wsrep_schema;

class SR_storage_table : public SR_storage
{
 public:
  SR_storage_table();
  ~SR_storage_table();
  int init(const char *cluster_uuid_str, Wsrep_schema*);

  THD* append_frag(THD*         thd,
                   uint32_t     flags,
                   const uchar* buf,
                   size_t       buf_len);

  void update_frag_seqno(THD* thd, THD* orig_THD);

  void release_SR_thd(THD* thd);

  void append_frag_apply(THD*         thd,
                         uint32_t     flags,
                         const uchar* buf,
                         size_t       buf_len);

  void append_frag_commit(THD*         thd,
                          uint32_t     flags,
                          const uchar* buf,
                          size_t       buf_len);

  void remove_trx( THD *thd );

  void remove_trx( wsrep_SR_trx_info *trx );

  void rollback_trx( THD *thd );

  void rollback_trx( wsrep_SR_trx_info *trx );

  void trx_done( THD *thd );

  int replay_trx(THD* thd, const wsrep_trx_meta_t& meta);

  int restore( THD *thd );

  void prepare_for_open_tables(THD *thd, TABLE_LIST **table_list);

  void close();

 private:
  Wsrep_schema* wsrep_schema_;
};
