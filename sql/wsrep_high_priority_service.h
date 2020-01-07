/* Copyright 2018 Codership Oy <info@codership.com>

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

#ifndef WSREP_HIGH_PRIORITY_SERVICE_H
#define WSREP_HIGH_PRIORITY_SERVICE_H

#include "wsrep/high_priority_service.hpp"
#include "my_global.h"
#include "sql_error.h" /* Diagnostics area */
#include "sql_class.h" /* rpl_group_info */

class THD;
class Relay_log_info;
class Wsrep_server_service;

class Wsrep_high_priority_service :
  public wsrep::high_priority_service,
  public wsrep::high_priority_context
{
public:
  Wsrep_high_priority_service(THD*);
  ~Wsrep_high_priority_service();
  int start_transaction(const wsrep::ws_handle&,
                        const wsrep::ws_meta&);
  int next_fragment(const wsrep::ws_meta&);
  const wsrep::transaction& transaction() const;
  int adopt_transaction(const wsrep::transaction&);
  int apply_write_set(const wsrep::ws_meta&, const wsrep::const_buffer&,
                      wsrep::mutable_buffer&) = 0;
  int append_fragment_and_commit(const wsrep::ws_handle&,
                                 const wsrep::ws_meta&,
                                 const wsrep::const_buffer&,
                                 const wsrep::xid&);
  int remove_fragments(const wsrep::ws_meta&);
  int commit(const wsrep::ws_handle&, const wsrep::ws_meta&);
  int rollback(const wsrep::ws_handle&, const wsrep::ws_meta&);
  int apply_toi(const wsrep::ws_meta&, const wsrep::const_buffer&,
                wsrep::mutable_buffer&);
  void store_globals();
  void reset_globals();
  void switch_execution_context(wsrep::high_priority_service&);
  int log_dummy_write_set(const wsrep::ws_handle&,
                          const wsrep::ws_meta&,
                          wsrep::mutable_buffer&);
  void adopt_apply_error(wsrep::mutable_buffer&);

  virtual bool check_exit_status() const = 0;
  void debug_crash(const char*);
protected:
  friend Wsrep_server_service;
  THD* m_thd;
  Relay_log_info*   m_rli;
  rpl_group_info*   m_rgi;
  struct shadow
  {
    ulonglong      option_bits;
    uint           server_status;
    struct st_vio* vio;
    ulong          tx_isolation;
    char*          db;
    size_t         db_length;
    //struct timeval user_time;
    my_hrtime_t user_time;
    longlong       row_count_func;
    bool           wsrep_applier;
  } m_shadow;
};

class Wsrep_applier_service : public Wsrep_high_priority_service
{
public:
  Wsrep_applier_service(THD*);
  ~Wsrep_applier_service();
  int apply_write_set(const wsrep::ws_meta&, const wsrep::const_buffer&,
                      wsrep::mutable_buffer&);
  int apply_nbo_begin(const wsrep::ws_meta&, const wsrep::const_buffer& data,
                      wsrep::mutable_buffer& err);
  void after_apply();
  bool is_replaying() const { return false; }
  bool check_exit_status() const;
};

class Wsrep_replayer_service : public Wsrep_high_priority_service
{
public:
  Wsrep_replayer_service(THD* replayer_thd, THD* orig_thd);
  ~Wsrep_replayer_service();
  int apply_write_set(const wsrep::ws_meta&, const wsrep::const_buffer&,
                      wsrep::mutable_buffer&);
  int apply_nbo_begin(const wsrep::ws_meta&, const wsrep::const_buffer& data,
                      wsrep::mutable_buffer& err)
  {
    DBUG_ASSERT(0); /* DDL should never cause replaying */
    return 0;
  }
  void after_apply() { }
  bool is_replaying() const { return true; }
  void replay_status(enum wsrep::provider::status status)
  { m_replay_status = status; }
  enum wsrep::provider::status replay_status() const
  { return m_replay_status; }
  /* Replayer should never be forced to exit */
  bool check_exit_status() const { return false; }
private:
  THD* m_orig_thd;
  struct da_shadow
  {
    enum Diagnostics_area::enum_diagnostics_status status;
    ulonglong affected_rows;
    ulonglong last_insert_id;
    char message[MYSQL_ERRMSG_SIZE];
    da_shadow()
      : status()
      , affected_rows()
      , last_insert_id()
      , message()
    { }
  } m_da_shadow;
  enum wsrep::provider::status m_replay_status;
};

#endif /* WSREP_HIGH_PRIORITY_SERVICE_H */
