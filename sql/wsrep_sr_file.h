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

#include "wsrep_priv.h"

#include "wsrep_sr.h"
#include "wsrep_thd.h"
#include <map>
#include <string>
#include <list>

class SR_storage;
class SR_file;
class Wsrep_schema;

typedef struct node_trx {
  uint8_t data[24];
} node_trx_t;
typedef std::map<node_trx_t, bool> trxs_t;

class SR_storage_file: public SR_storage {
  enum read_mode {
    FILTER,
    POPULATE
  };

 private:

  //wsrep_uuid_t cluster_uuid_;
  std::string dir_;
  size_t size_limit_;

  std::list<SR_file*> files_;

  SR_file *curr_file_;

  int max_file_order();
  std::string *new_name(int order);
  SR_file *append_file();
  void remove_file(SR_file *file);

  void read_trxs_from_file(
                           std::string file,
                           trxs_t *trxs,
                           THD *thd,
                           enum read_mode mode);

 public:
  SR_storage_file(const char *dir, size_t limit, const char* cluster_uuid_str);

  int init(const char *cluster_uuid_str, Wsrep_schema*);

  THD* append_frag(THD*         thd,
                   uint32_t     flags,
                   const uchar* buf,
                   size_t       buf_len);

  void append_frag_apply(THD*         thd,
                         uint32_t     flags,
                         const uchar* buf,
                         size_t       buf_len)
  {
      append_frag(thd, flags, buf, buf_len);
  }

  void append_frag_commit(THD*         thd,
                          uint32_t     flags,
                          const uchar* buf,
                          size_t       buf_len)
  {
      append_frag(thd, flags, buf, buf_len);
  }

  void update_frag_seqno (THD* thd, THD* orig_THD)  {};

  void release_SR_thd(THD* thd) {}

  void remove_trx( THD *thd );

  void remove_trx( wsrep_SR_trx_info *trx ) {
    remove_trx(trx->get_THD());
  }

  void rollback_trx( THD *thd );

  void rollback_trx( wsrep_SR_trx_info *trx ) {
    rollback_trx(trx->get_THD());
  }

  void trx_done( THD *thd ) { }

  int replay_trx(THD* thd, const wsrep_trx_meta_t&);

  int restore( THD *thd );

  void prepare_for_open_tables(THD *, TABLE_LIST **) { }

  void close();
};
