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
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#include <my_config.h>

#ifndef WSREP_THD_H
#define WSREP_THD_H

#ifdef WITH_WSREP

#include "sql_class.h"

int wsrep_show_bf_aborts (THD *thd, SHOW_VAR *var, char *buff,
                          enum enum_var_type scope);
void wsrep_client_rollback(THD *thd);
void wsrep_replay_sp_transaction(THD* thd);
void wsrep_replay_transaction(THD *thd);
bool wsrep_create_appliers(long threads, bool thread_count_lock=false);
void wsrep_create_rollbacker();

int  wsrep_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr,
                                my_bool signal);

/*
  PA = Parallel Applying (on the slave side)
*/
extern void  wsrep_thd_set_PA_safe(void *thd_ptr, my_bool safe);
extern my_bool  wsrep_thd_is_BF(THD *thd, my_bool sync);
extern my_bool wsrep_thd_is_wsrep(void *thd_ptr);

enum wsrep_conflict_state wsrep_thd_conflict_state(void *thd_ptr, my_bool sync);
extern "C" my_bool  wsrep_thd_is_BF_or_commit(void *thd_ptr, my_bool sync);
extern "C" my_bool  wsrep_thd_is_local(void *thd_ptr, my_bool sync);
extern "C" int  wsrep_thd_in_locking_session(void *thd_ptr);

extern void wsrep_report_bf_lock_wait(THD *thd,
                                      unsigned long long trx_id);

#else /* WITH_WSREP */

#define wsrep_thd_is_BF(T, S) (0)
#define wsrep_abort_thd(X,Y,Z) do { } while(0)
#define wsrep_create_appliers(T) do { } while(0)

#endif
#endif /* WSREP_THD_H */
