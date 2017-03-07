/* Copyright 2013-2016 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef WSREP_SR_H
#define WSREP_SR_H

#include "wsrep_mysqld.h"

#include <sys/types.h>
#include <vector>
#include <map>
#include <queue>
#include "sql_class.h"

extern class SR_storage_file *wsrep_SR_store_file;
extern class SR_storage_table *wsrep_SR_store_table;
extern class SR_storage *wsrep_SR_store;

class wsrep_SR_rollback_event {

 private:
  THD* thd_;
  my_thread_id thread_id_;
  wsrep_trx_meta_t trx_meta_;
  wsrep_ws_handle_t ws_handle_;

 public:
  wsrep_SR_rollback_event(THD *thd)
  {
    thd_ = thd;
    thread_id_ = thd->thread_id;
    trx_meta_  = thd->wsrep_trx_meta;
    ws_handle_ = thd->wsrep_ws_handle;
  }
 public:
  my_thread_id get_thread_id() { return thread_id_; }
  wsrep_ws_handle_t get_ws_handle() { return ws_handle_; }
  wsrep_trx_meta_t get_trx_meta() { return trx_meta_; }
};

class Wsrep_SR_rollback_queue {
 private:

  std::map<my_thread_id, class wsrep_SR_rollback_event*>  map_;
  mysql_mutex_t mutex_;

 public:

  Wsrep_SR_rollback_queue ()
  {
    mysql_mutex_init(key_LOCK_wsrep_SR_pool,
                     &mutex_, MY_MUTEX_INIT_FAST);
  }

  ~Wsrep_SR_rollback_queue ()
  {
    /* check if there are pending rollbacks */
    (void) mysql_mutex_destroy(&mutex_);
  }

  void append_SR_rollback(THD *thd);
  void send_SR_rollbacks(THD *thd);

};

class wsrep_SR_trx_info
{
private:
  THD *thd_; /* THD processing SR transaction */
  my_thread_id applier_thread_; /* thread currently applying, -1 if idle */

 public:
  ~wsrep_SR_trx_info ()
  {
    WSREP_DEBUG("wsrep_SR_trx_info destructor: %lld",
                (thd_) ? thd_->thread_id : -1);
    remove(NULL, true);
  }

  my_thread_id get_applier_thread() { return applier_thread_; }
  void set_applier_thread(my_thread_id thread_id) { applier_thread_= thread_id; }

  void remove(THD* caller, bool persistent);
  void cleanup();

  wsrep_SR_trx_info(THD *thd) {
    WSREP_DEBUG("wsrep_SR_trx_info construtor: %lld", (thd) ? thd->thread_id : -1);
    thd_ = thd;
    applier_thread_ = 0;
  }
  THD* get_THD() const { return thd_; }
  void set_THD(THD *thd) { thd_ = thd; }

  void append_fragment(const wsrep_trx_meta_t *trx) {
    thd_->wsrep_SR_fragments.push_back(*trx);
  }
};

class SR_pool
{
  /* protected by LOCK_wsrep_SR_pool mutex */
  // ideally we want to change std::map to std::unordered_map here
  typedef std::map<uint64_t, wsrep_SR_trx_info*> trx_pool_t;
  typedef std::map<wsrep_uuid_t, trx_pool_t>     src_pool_t;
 private:
  src_pool_t pool_;
 public:
  SR_pool() { }

  ~SR_pool () {
    WSREP_DEBUG("SR_pool destructor");
  }

  wsrep_SR_trx_info*  find(const wsrep_uuid_t& nodeID,
                           const uint64_t& trxID) const;
  wsrep_SR_trx_info* add(const wsrep_uuid_t& nodeID, const uint64_t& trxID,
                         THD *thd);
  void remove(THD* caller, const wsrep_uuid_t& nodeID, const uint64_t& trxID,
              bool persistent);
  bool remove(THD* caller, THD *thd, bool persistent);
  void trimToNodes (THD* caller, const wsrep_member_info_t nodes[], int nodeCount);
};

class Wsrep_schema;

class SR_storage
{
public:
  SR_storage()
      :
    cluster_uuid_(),
    restored_(false)
  { }
  virtual ~SR_storage() {}

  virtual int init(const char* cluster_uuid_str, Wsrep_schema*) = 0;

  virtual THD* append_frag (THD*         thd,
                            uint32_t     flags,
                            const uchar* buf,
                            size_t       buf_len) = 0;

  virtual void update_frag_seqno (THD* thd, THD* orig_THD) = 0;

  virtual void release_SR_thd(THD* thd) = 0;

  virtual void append_frag_apply (THD*         thd,
                                  uint32_t     flags,
                                  const uchar* buf,
                                  size_t       buf_len) = 0;

  virtual void append_frag_commit (THD*         thd,
                                   uint32_t     flags,
                                   const uchar* buf,
                                   size_t       buf_len) = 0;

  virtual void remove_trx ( THD* thd ) = 0;

  virtual void remove_trx ( wsrep_SR_trx_info* trx ) = 0;

  virtual void rollback_trx ( THD* thd ) = 0;

  virtual void rollback_trx ( wsrep_SR_trx_info* trx ) = 0;

  virtual void trx_done ( THD* thd ) = 0;

  virtual int replay_trx(THD* thd, const wsrep_trx_meta_t& meta) = 0;

  virtual int restore( THD *thd ) = 0;

  virtual void prepare_for_open_tables(THD *thd, TABLE_LIST **table_list) = 0;

  virtual void close() = 0;

protected:
  wsrep_uuid_t cluster_uuid_;
  bool restored_;
};

/* functions for appliers */
extern SR_pool *sr_pool;

void wsrep_close_SR_transactions(THD *thd);
void wsrep_init_SR_pool();
void wsrep_restore_SR_trxs(THD *thd);
void trim_SR_pool(THD* thd, const wsrep_member_info_t nodes[], int nodeCount);
bool wsrep_abort_SR_THD(THD* thd, THD* victim_thd);

/*
  Remove SR fragments from SR storage. This happens in the thd
  transaction context, fragment removal will be committed
  at the same time as THD transaction is committed.
 */
void wsrep_remove_SR_fragments(THD *thd);

/*
  Rollback SR trx. Removes fragments from SR storage non-transactionally,
  so it can be used outside of THD transaction context.
 */
void wsrep_rollback_SR_trx(THD *thd);

/*
  Prepare SR trx info for local transaction. Transfers SR ownership
  to SR trx info handle.
 */
void wsrep_prepare_SR_trx_info_for_rollback(THD *thd);

static inline bool wsrep_may_produce_SR_step(const THD *thd)
{
  switch (thd->lex->sql_command)
  {
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_LOAD:
  case SQLCOM_COMMIT:
  case SQLCOM_ROLLBACK_TO_SAVEPOINT:
  case SQLCOM_SAVEPOINT:
    return true;
  default:
    return false;
  }
}

void wsrep_prepare_SR_for_open_tables(THD *thd, TABLE_LIST **table_list);
void wsrep_handle_SR_rollback(void *BF_thd_ptr, void *victim_thd_ptr);

extern Wsrep_SR_rollback_queue* wsrep_SR_rollback_queue;

#endif /* WSREP_SR_H */
