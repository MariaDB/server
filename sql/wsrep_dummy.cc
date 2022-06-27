/* Copyright (C) 2014, 2021, MariaDB

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

#include "mariadb.h"
#include <sql_class.h>
#include <mysql/service_wsrep.h>

my_bool wsrep_thd_is_BF(const THD *, my_bool)
{ return 0; }

int wsrep_is_wsrep_xid(const void* xid)
{ return 0; }

long long wsrep_xid_seqno(const XID* x)
{ return -1; }

const unsigned char* wsrep_xid_uuid(const XID*)
{
    static const unsigned char uuid[16]= {0};
    return uuid;
}

bool wsrep_prepare_key_for_innodb(THD* thd, const uchar*, size_t, const uchar*, size_t, struct wsrep_buf*, size_t*)
{ return 1; }

bool wsrep_prepare_key(const uchar*, size_t, const uchar*, size_t, struct wsrep_buf*, size_t*)
{ return 0; }

struct wsrep *get_wsrep()
{ return 0; }

my_bool get_wsrep_recovery()
{ return 0; }

bool wsrep_consistency_check(THD *)
{ return 0; }

void wsrep_lock_rollback()
{ }

my_bool wsrep_on(const THD *)
{ return 0; }

void wsrep_thd_LOCK(const THD *)
{ }

void wsrep_thd_UNLOCK(const THD *)
{ }

void wsrep_thd_kill_LOCK(const THD *)
{ }

void wsrep_thd_kill_UNLOCK(const THD *)
{ }

const char *wsrep_thd_conflict_state_str(THD *)
{ return 0; }

const char *wsrep_thd_exec_mode_str(THD *)
{ return NULL; }

const char *wsrep_thd_query(const THD *)
{ return "NULL"; }

const char *wsrep_thd_query_state_str(THD *)
{ return 0; }

int wsrep_thd_retry_counter(const THD *)
{ return 0; }

bool wsrep_thd_ignore_table(THD *)
{ return 0; }

long long wsrep_thd_trx_seqno(const THD *)
{ return -1; }

my_bool wsrep_thd_is_aborting(const THD *)
{ return 0; }

void wsrep_set_data_home_dir(const char *)
{ }

my_bool wsrep_thd_is_local(const THD *)
{ return 0; }

void wsrep_thd_self_abort(THD *)
{ }

int wsrep_thd_append_key(THD *, const struct wsrep_key*, int, enum Wsrep_service_key_type)
{ return 0; }

const char* wsrep_thd_client_state_str(const THD*)
{ return 0; }

const char* wsrep_thd_client_mode_str(const THD*)
{ return 0; }

const char* wsrep_thd_transaction_state_str(const THD*)
{ return 0; }

query_id_t wsrep_thd_transaction_id(const THD *)
{ return 0; }

my_bool wsrep_thd_bf_abort(THD *, THD *, my_bool)
{ return 0; }

my_bool wsrep_thd_order_before(const THD*, const THD *)
{ return 0; }

void wsrep_handle_SR_rollback(THD*, THD*)
{ }

my_bool wsrep_thd_skip_locking(const THD*)
{ return 0;}

const char* wsrep_get_sr_table_name()
{ return 0; }

my_bool wsrep_get_debug()
{ return 0;}

void wsrep_commit_ordered(THD* )
{ }

void wsrep_log(void (*)(const char *, ...), const char *, ...)
{
}

my_bool wsrep_thd_is_applying(const THD*)
{ return 0;}

my_bool wsrep_thd_has_ignored_error(const THD*)
{ return 0;}

void wsrep_thd_set_ignored_error(THD*, my_bool)
{ }
ulong wsrep_OSU_method_get(const THD*)
{ return 0;}
bool wsrep_thd_set_wsrep_aborter(THD*, THD*)
{ return 0;}

void wsrep_report_bf_lock_wait(const THD*,
                               unsigned long long)
{}

void wsrep_thd_set_PA_unsafe(THD*)
{}

