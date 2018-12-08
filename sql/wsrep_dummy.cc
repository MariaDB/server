/* Copyright (C) 2014 SkySQL Ab.

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

#include "mariadb.h"
#include <sql_class.h>
#include <mysql/service_wsrep.h>

my_bool wsrep_thd_is_BF(void *, my_bool)
{ return 0; }

my_bool wsrep_trx_order_before(const void *, const void *)
{ return 0; }

enum wsrep_conflict_state wsrep_thd_conflict_state(THD *, my_bool)
{ return NO_CONFLICT; }

//int wsrep_is_wsrep_xid(const XID*)
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
{ return -1; }

bool wsrep_prepare_key(const uchar*, size_t, const uchar*, size_t, struct wsrep_buf*, size_t*)
{ return 0; }

struct wsrep *get_wsrep()
{ return 0; }

my_bool get_wsrep_certify_nonPK()
{ return 0; }

my_bool get_wsrep_debug()
{ return 0; }

my_bool get_wsrep_drupal_282555_workaround()
{ return 0; }

my_bool get_wsrep_load_data_splitting()
{ return 0; }

my_bool get_wsrep_recovery()
{ return 0; }

my_bool get_wsrep_log_conflicts()
{ return 0; }

long get_wsrep_protocol_version()
{ return 0; }

my_bool wsrep_aborting_thd_contains(THD *)
{ return 0; }

void wsrep_aborting_thd_enqueue(THD *)
{ }

bool wsrep_consistency_check(THD *)
{ return 0; }

void wsrep_lock_rollback()
{ }

my_bool wsrep_on(const void* thd)
{ return 0; }

void wsrep_post_commit(THD*, bool)
{ }

enum wsrep_trx_status wsrep_run_wsrep_commit(THD *, bool)
{ return WSREP_TRX_ERROR; }

void wsrep_thd_LOCK(const void *)
{ }

void wsrep_thd_UNLOCK(const void *)
{ }

void wsrep_thd_awake(const void *, my_bool)
{ }

const char *wsrep_thd_conflict_state_str(THD *)
{ return 0; }

enum wsrep_exec_mode wsrep_thd_exec_mode(THD *)
{ return LOCAL_STATE; }

const char *wsrep_thd_exec_mode_str(THD *)
{ return NULL; }

enum wsrep_conflict_state wsrep_thd_get_conflict_state(THD *)
{ return NO_CONFLICT; }

my_bool wsrep_thd_is_wsrep_on(const void *)
{ return 0; }

my_thread_id wsrep_thd_thread_id(const void* thd)
{ return -1; }

const char *wsrep_thd_query(const void *)
{ return 0; }

enum wsrep_query_state wsrep_thd_query_state(THD *)
{ return QUERY_IDLE; }

const char *wsrep_thd_query_state_str(THD *)
{ return 0; }

void wsrep_thd_xid(const void* thd, void* xid, size_t size)
{ }

int wsrep_thd_retry_counter(THD *)
{ return 0; }

void wsrep_thd_set_conflict_state(THD *, enum wsrep_conflict_state)
{ }

bool wsrep_thd_ignore_table(THD *)
{ return 0; }

long long wsrep_thd_trx_seqno(const void *)
{ return -1; }

struct wsrep_ws_handle* wsrep_thd_ws_handle(THD *)
{ return 0; }

my_bool wsrep_thd_is_aborting(const void *)
{ return 0; }

void wsrep_unlock_rollback()
{ }

void wsrep_set_data_home_dir(const char *)
{ }
