/* Copyright (C) 2008-2015 Kentoku Shiba

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#define MYSQL_SERVER 1
#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#endif
#include <my_getopt.h>
#include "spd_err.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_table.h"
#include "spd_trx.h"

extern struct st_mysql_plugin spider_i_s_alloc_mem;
#ifdef MARIADB_BASE_VERSION
extern struct st_maria_plugin spider_i_s_alloc_mem_maria;
#endif

extern volatile ulonglong spider_mon_table_cache_version;
extern volatile ulonglong spider_mon_table_cache_version_req;

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
static int spider_direct_update(THD *thd, SHOW_VAR *var, char *buff)
{
  int error_num = 0;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_direct_update");
  var->type = SHOW_LONGLONG;
  if ((trx = spider_get_trx(thd, TRUE, &error_num)))
    var->value = (char *) &trx->direct_update_count;
  DBUG_RETURN(error_num);
}

static int spider_direct_delete(THD *thd, SHOW_VAR *var, char *buff)
{
  int error_num = 0;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_direct_delete");
  var->type = SHOW_LONGLONG;
  if ((trx = spider_get_trx(thd, TRUE, &error_num)))
    var->value = (char *) &trx->direct_delete_count;
  DBUG_RETURN(error_num);
}
#endif

static int spider_direct_order_limit(THD *thd, SHOW_VAR *var, char *buff)
{
  int error_num = 0;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_direct_order_limit");
  var->type = SHOW_LONGLONG;
  if ((trx = spider_get_trx(thd, TRUE, &error_num)))
    var->value = (char *) &trx->direct_order_limit_count;
  DBUG_RETURN(error_num);
}

static int spider_direct_aggregate(THD *thd, SHOW_VAR *var, char *buff)
{
  int error_num = 0;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_direct_aggregate");
  var->type = SHOW_LONGLONG;
  if ((trx = spider_get_trx(thd, TRUE, &error_num)))
    var->value = (char *) &trx->direct_aggregate_count;
  DBUG_RETURN(error_num);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
static int spider_hs_result_free(THD *thd, SHOW_VAR *var, char *buff)
{
  int error_num = 0;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_hs_result_free");
  var->type = SHOW_LONGLONG;
  if ((trx = spider_get_trx(thd, TRUE, &error_num)))
    var->value = (char *) &trx->hs_result_free_count;
  DBUG_RETURN(error_num);
}
#endif

struct st_mysql_show_var spider_status_variables[] =
{
  {"Spider_mon_table_cache_version",
    (char *) &spider_mon_table_cache_version, SHOW_LONGLONG},
  {"Spider_mon_table_cache_version_req",
    (char *) &spider_mon_table_cache_version_req, SHOW_LONGLONG},
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#ifdef SPIDER_HAS_SHOW_SIMPLE_FUNC
  {"Spider_direct_update", (char *) &spider_direct_update, SHOW_SIMPLE_FUNC},
  {"Spider_direct_delete", (char *) &spider_direct_delete, SHOW_SIMPLE_FUNC},
#else
  {"Spider_direct_update", (char *) &spider_direct_update, SHOW_FUNC},
  {"Spider_direct_delete", (char *) &spider_direct_delete, SHOW_FUNC},
#endif
#endif
#ifdef SPIDER_HAS_SHOW_SIMPLE_FUNC
  {"Spider_direct_order_limit",
    (char *) &spider_direct_order_limit, SHOW_SIMPLE_FUNC},
  {"Spider_direct_aggregate",
    (char *) &spider_direct_aggregate, SHOW_SIMPLE_FUNC},
#else
  {"Spider_direct_order_limit",
    (char *) &spider_direct_order_limit, SHOW_FUNC},
  {"Spider_direct_aggregate",
    (char *) &spider_direct_aggregate, SHOW_FUNC},
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef SPIDER_HAS_SHOW_SIMPLE_FUNC
  {"Spider_hs_result_free", (char *) &spider_hs_result_free, SHOW_SIMPLE_FUNC},
#else
  {"Spider_hs_result_free", (char *) &spider_hs_result_free, SHOW_FUNC},
#endif
#endif
  {NullS, NullS, SHOW_LONG}
};

typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_int_t, int);
#if MYSQL_VERSION_ID < 50500
extern bool throw_bounds_warning(THD *thd, bool fixed, bool unsignd,
  const char *name, long long val);
#else
extern bool throw_bounds_warning(THD *thd, const char *name, bool fixed,
  bool is_unsignd, longlong v);
#endif

static my_bool spider_support_xa;
static MYSQL_SYSVAR_BOOL(
  support_xa,
  spider_support_xa,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "XA support",
  NULL,
  NULL,
  TRUE
);

my_bool spider_param_support_xa()
{
  DBUG_ENTER("spider_param_support_xa");
  DBUG_RETURN(spider_support_xa);
}

static my_bool spider_connect_mutex;
static MYSQL_SYSVAR_BOOL(
  connect_mutex,
  spider_connect_mutex,
  PLUGIN_VAR_OPCMDARG,
  "Use mutex at connecting",
  NULL,
  NULL,
  FALSE
);

my_bool spider_param_connect_mutex()
{
  DBUG_ENTER("spider_param_connect_mutex");
  DBUG_RETURN(spider_connect_mutex);
}

static uint spider_connect_error_interval;
/*
  0-: interval
 */
static MYSQL_SYSVAR_UINT(
  connect_error_interval,
  spider_connect_error_interval,
  PLUGIN_VAR_RQCMDARG,
  "Return same error code until interval passes if connection is failed",
  NULL,
  NULL,
  1,
  0,
  4294967295U,
  0
);

uint spider_param_connect_error_interval()
{
  DBUG_ENTER("spider_param_connect_error_interval");
  DBUG_RETURN(spider_connect_error_interval);
}

static uint spider_table_init_error_interval;
/*
  0-: interval
 */
static MYSQL_SYSVAR_UINT(
  table_init_error_interval,
  spider_table_init_error_interval,
  PLUGIN_VAR_RQCMDARG,
  "Return same error code until interval passes if table init is failed",
  NULL,
  NULL,
  1,
  0,
  4294967295U,
  0
);

uint spider_param_table_init_error_interval()
{
  DBUG_ENTER("spider_param_table_init_error_interval");
  DBUG_RETURN(spider_table_init_error_interval);
}

static int spider_use_table_charset;
/*
 -1 :use table parameter
  0 :use utf8
  1 :use table charset
 */
static MYSQL_SYSVAR_INT(
  use_table_charset,
  spider_use_table_charset,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Use table charset for remote access",
  NULL,
  NULL,
  -1,
  -1,
  1,
  0
);

int spider_param_use_table_charset(
  int use_table_charset
) {
  DBUG_ENTER("spider_param_use_table_charset");
  DBUG_RETURN(spider_use_table_charset == -1 ?
    use_table_charset : spider_use_table_charset);
}

/*
  0: no recycle
  1: recycle in instance
  2: recycle in thread
 */
static MYSQL_THDVAR_UINT(
  conn_recycle_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Connection recycle mode", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  0, /* min */
  2, /* max */
  0 /* blk */
);

uint spider_param_conn_recycle_mode(
  THD *thd
) {
  DBUG_ENTER("spider_param_conn_recycle_mode");
  DBUG_RETURN(THDVAR(thd, conn_recycle_mode));
}

/*
  0: weak
  1: strict
 */
static MYSQL_THDVAR_UINT(
  conn_recycle_strict, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Strict connection recycle", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  0, /* min */
  1, /* max */
  0 /* blk */
);

uint spider_param_conn_recycle_strict(
  THD *thd
) {
  DBUG_ENTER("spider_param_conn_recycle_strict");
  DBUG_RETURN(THDVAR(thd, conn_recycle_strict));
}

/*
  FALSE: no sync
  TRUE:  sync
 */
static MYSQL_THDVAR_BOOL(
  sync_trx_isolation, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Sync transaction isolation level", /* comment */
  NULL, /* check */
  NULL, /* update */
  TRUE /* def */
);

bool spider_param_sync_trx_isolation(
  THD *thd
) {
  DBUG_ENTER("spider_param_sync_trx_isolation");
  DBUG_RETURN(THDVAR(thd, sync_trx_isolation));
}

/*
  FALSE: no use
  TRUE:  use
 */
static MYSQL_THDVAR_BOOL(
  use_consistent_snapshot, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Use start transaction with consistent snapshot", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_use_consistent_snapshot(
  THD *thd
) {
  DBUG_ENTER("spider_param_use_consistent_snapshot");
  DBUG_RETURN(THDVAR(thd, use_consistent_snapshot));
}

/*
  FALSE: off
  TRUE:  on
 */
static MYSQL_THDVAR_BOOL(
  internal_xa, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Use inner xa transaction", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_internal_xa(
  THD *thd
) {
  DBUG_ENTER("spider_param_internal_xa");
  DBUG_RETURN(THDVAR(thd, internal_xa));
}

/*
  0 :err when use a spider table
  1 :err when start trx
  2 :start trx with snapshot on remote server(not use xa)
  3 :start xa on remote server(not use trx with snapshot)
 */
static MYSQL_THDVAR_UINT(
  internal_xa_snapshot, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Action of inner xa and snapshot both using", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  0, /* min */
  3, /* max */
  0 /* blk */
);

uint spider_param_internal_xa_snapshot(
  THD *thd
) {
  DBUG_ENTER("spider_param_internal_xa_snapshot");
  DBUG_RETURN(THDVAR(thd, internal_xa_snapshot));
}

/*
  0 :off
  1 :continue prepare, commit, rollback if xid not found return
  2 :continue prepare, commit, rollback if all error return
 */
static MYSQL_THDVAR_UINT(
  force_commit, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Force prepare, commit, rollback mode", /* comment */
  NULL, /* check */
  NULL, /* update */
  1, /* def */
  0, /* min */
  2, /* max */
  0 /* blk */
);

uint spider_param_force_commit(
  THD *thd
) {
  DBUG_ENTER("spider_param_force_commit");
  DBUG_RETURN(THDVAR(thd, force_commit));
}

/*
 -1 :use table parameter
  0-:offset
 */
static MYSQL_THDVAR_LONGLONG(
  internal_offset, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Internal offset", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_internal_offset(
  THD *thd,
  longlong internal_offset
) {
  DBUG_ENTER("spider_param_internal_offset");
  DBUG_RETURN(THDVAR(thd, internal_offset) < 0 ?
    internal_offset : THDVAR(thd, internal_offset));
}

/*
 -1 :use table parameter
  0-:limit
 */
static MYSQL_THDVAR_LONGLONG(
  internal_limit, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Internal limit", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_internal_limit(
  THD *thd,
  longlong internal_limit
) {
  DBUG_ENTER("spider_param_internal_limit");
  DBUG_RETURN(THDVAR(thd, internal_limit) < 0 ?
    internal_limit : THDVAR(thd, internal_limit));
}

/*
 -1 :use table parameter
  0-:number of rows at a select
 */
static MYSQL_THDVAR_LONGLONG(
  split_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Number of rows at a select", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_split_read(
  THD *thd,
  longlong split_read
) {
  DBUG_ENTER("spider_param_split_read");
  DBUG_RETURN(THDVAR(thd, split_read) < 0 ?
    split_read : THDVAR(thd, split_read));
}

/*
  -1 :use table parameter
   0 :doesn't use "offset" and "limit" for "split_read"
   1-:magnification
 */
static MYSQL_THDVAR_INT(
  semi_split_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Use offset and limit parameter in SQL for split_read parameter.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

double spider_param_semi_split_read(
  THD *thd,
  double semi_split_read
) {
  DBUG_ENTER("spider_param_semi_split_read");
  DBUG_RETURN(THDVAR(thd, semi_split_read) < 0 ?
    semi_split_read : THDVAR(thd, semi_split_read));
}

/*
 -1 :use table parameter
  0-:the limit value
 */
static MYSQL_THDVAR_LONGLONG(
  semi_split_read_limit, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "The limit value for semi_split_read", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_semi_split_read_limit(
  THD *thd,
  longlong semi_split_read_limit
) {
  DBUG_ENTER("spider_param_semi_split_read_limit");
  DBUG_RETURN(THDVAR(thd, semi_split_read_limit) < 0 ?
    semi_split_read_limit : THDVAR(thd, semi_split_read_limit));
}

/*
 -1 :use table parameter
  0 :no alloc
  1-:alloc size
 */
static MYSQL_THDVAR_INT(
  init_sql_alloc_size, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Initial sql string alloc size", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_init_sql_alloc_size(
  THD *thd,
  int init_sql_alloc_size
) {
  DBUG_ENTER("spider_param_init_sql_alloc_size");
  DBUG_RETURN(THDVAR(thd, init_sql_alloc_size) < 0 ?
    init_sql_alloc_size : THDVAR(thd, init_sql_alloc_size));
}

/*
 -1 :use table parameter
  0 :off
  1 :on
 */
static MYSQL_THDVAR_INT(
  reset_sql_alloc, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Reset sql string alloc after execute", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_reset_sql_alloc(
  THD *thd,
  int reset_sql_alloc
) {
  DBUG_ENTER("spider_param_reset_sql_alloc");
  DBUG_RETURN(THDVAR(thd, reset_sql_alloc) < 0 ?
    reset_sql_alloc : THDVAR(thd, reset_sql_alloc));
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
/*
 -1 :use table parameter
  0-:result free size for handlersocket
 */
static MYSQL_THDVAR_LONGLONG(
  hs_result_free_size, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Result free size for handlersocket", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_hs_result_free_size(
  THD *thd,
  longlong hs_result_free_size
) {
  DBUG_ENTER("spider_param_hs_result_free_size");
  DBUG_RETURN(THDVAR(thd, hs_result_free_size) < 0 ?
    hs_result_free_size : THDVAR(thd, hs_result_free_size));
}
#endif

/*
 -1 :use table parameter
  0 :off
  1 :on
 */
static MYSQL_THDVAR_INT(
  multi_split_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Sprit read mode for multi range", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_multi_split_read(
  THD *thd,
  int multi_split_read
) {
  DBUG_ENTER("spider_param_multi_split_read");
  DBUG_RETURN(THDVAR(thd, multi_split_read) < 0 ?
    multi_split_read : THDVAR(thd, multi_split_read));
}

/*
 -1 :use table parameter
  0-:max order columns
 */
static MYSQL_THDVAR_INT(
  max_order, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Max columns for order by", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  32767, /* max */
  0 /* blk */
);

int spider_param_max_order(
  THD *thd,
  int max_order
) {
  DBUG_ENTER("spider_param_max_order");
  DBUG_RETURN(THDVAR(thd, max_order) < 0 ?
    max_order : THDVAR(thd, max_order));
}

/*
 -1 :off
  0 :read uncommitted
  1 :read committed
  2 :repeatable read
  3 :serializable
 */
static MYSQL_THDVAR_INT(
  semi_trx_isolation, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Transaction isolation level during execute a sql", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  3, /* max */
  0 /* blk */
);

int spider_param_semi_trx_isolation(
  THD *thd
) {
  DBUG_ENTER("spider_param_semi_trx_isolation");
  DBUG_RETURN(THDVAR(thd, semi_trx_isolation));
}

static int spider_param_semi_table_lock_check(
  MYSQL_THD thd,
  struct st_mysql_sys_var *var,
  void *save,
  struct st_mysql_value *value
) {
  int error_num;
  SPIDER_TRX *trx;
  my_bool fixed;
  long long tmp;
  struct my_option options;
  DBUG_ENTER("spider_param_semi_table_lock_check");
  if (!(trx = spider_get_trx((THD *) thd, TRUE, &error_num)))
    DBUG_RETURN(error_num);
  if (trx->locked_connections)
  {
    my_message(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM,
      ER_SPIDER_ALTER_BEFORE_UNLOCK_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM);
  }
  value->val_int(value, &tmp);
  options.sub_size = 0;
  options.var_type = GET_INT;
  options.def_value = ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->def_val;
  options.min_value = ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->min_val;
  options.max_value = ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->max_val;
  options.block_size =
    (long) ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->blk_sz;
  options.arg_type = REQUIRED_ARG;
  *((int *) save) = (int) getopt_ll_limit_value(tmp, &options, &fixed);
#if MYSQL_VERSION_ID < 50500
  DBUG_RETURN(throw_bounds_warning(thd, fixed, FALSE,
    ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->name, (long long) tmp));
#else
  DBUG_RETURN(throw_bounds_warning(thd,
    ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->name, fixed, FALSE,
    (longlong) tmp));
#endif
}

/*
  0 :off
  1 :on
 */
static MYSQL_THDVAR_INT(
  semi_table_lock, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Table lock during execute a sql", /* comment */
  &spider_param_semi_table_lock_check, /* check */
  NULL, /* update */
  1, /* def */
  0, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_semi_table_lock(
  THD *thd,
  int semi_table_lock
) {
  DBUG_ENTER("spider_param_semi_table_lock");
  DBUG_RETURN((semi_table_lock & THDVAR(thd, semi_table_lock)));
}

static int spider_param_semi_table_lock_connection_check(
  MYSQL_THD thd,
  struct st_mysql_sys_var *var,
  void *save,
  struct st_mysql_value *value
) {
  int error_num;
  SPIDER_TRX *trx;
  my_bool fixed;
  long long tmp;
  struct my_option options;
  DBUG_ENTER("spider_param_semi_table_lock_connection_check");
  if (!(trx = spider_get_trx((THD *) thd, TRUE, &error_num)))
    DBUG_RETURN(error_num);
  if (trx->locked_connections)
  {
    my_message(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM,
      ER_SPIDER_ALTER_BEFORE_UNLOCK_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_ALTER_BEFORE_UNLOCK_NUM);
  }
  value->val_int(value, &tmp);
  options.sub_size = 0;
  options.var_type = GET_INT;
  options.def_value = ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->def_val;
  options.min_value = ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->min_val;
  options.max_value = ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->max_val;
  options.block_size =
    (long) ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->blk_sz;
  options.arg_type = REQUIRED_ARG;
  *((int *) save) = (int) getopt_ll_limit_value(tmp, &options, &fixed);
#if MYSQL_VERSION_ID < 50500
  DBUG_RETURN(throw_bounds_warning(thd, fixed, FALSE,
    ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->name, (long long) tmp));
#else
  DBUG_RETURN(throw_bounds_warning(thd,
    ((MYSQL_SYSVAR_NAME(thdvar_int_t) *) var)->name, fixed, FALSE,
    (longlong) tmp));
#endif
}

/*
 -1 :off
  0 :use same connection
  1 :use different connection
 */
static MYSQL_THDVAR_INT(
  semi_table_lock_connection, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Use different connection if semi_table_lock is enabled", /* comment */
  &spider_param_semi_table_lock_connection_check, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_semi_table_lock_connection(
  THD *thd,
  int semi_table_lock_connection
) {
  DBUG_ENTER("spider_param_semi_table_lock_connection");
  DBUG_RETURN(THDVAR(thd, semi_table_lock_connection) == -1 ?
    semi_table_lock_connection : THDVAR(thd, semi_table_lock_connection));
}

/*
  0-:block_size
 */
static MYSQL_THDVAR_UINT(
  block_size, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Index block size", /* comment */
  NULL, /* check */
  NULL, /* update */
  16384, /* def */
  0, /* min */
  4294967295U, /* max */
  0 /* blk */
);

uint spider_param_block_size(
  THD *thd
) {
  DBUG_ENTER("spider_param_block_size");
  DBUG_RETURN(THDVAR(thd, block_size));
}

/*
 -1 :use table parameter
  0 :off
  1 :lock in share mode
  2 :for update
 */
static MYSQL_THDVAR_INT(
  selupd_lock_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Lock for select with update", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_selupd_lock_mode(
  THD *thd,
  int selupd_lock_mode
) {
  DBUG_ENTER("spider_param_selupd_lock_mode");
  DBUG_RETURN(THDVAR(thd, selupd_lock_mode) == -1 ?
    selupd_lock_mode : THDVAR(thd, selupd_lock_mode));
}

/*
  FALSE: no sync
  TRUE:  sync
 */
static MYSQL_THDVAR_BOOL(
  sync_autocommit, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Sync autocommit", /* comment */
  NULL, /* check */
  NULL, /* update */
  TRUE /* def */
);

bool spider_param_sync_autocommit(
  THD *thd
) {
  DBUG_ENTER("spider_param_sync_autocommit");
  DBUG_RETURN(THDVAR(thd, sync_autocommit));
}

/*
  FALSE: no sync
  TRUE:  sync
 */
static MYSQL_THDVAR_BOOL(
  sync_time_zone, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Sync time_zone", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_sync_time_zone(
  THD *thd
) {
  DBUG_ENTER("spider_param_sync_time_zone");
  DBUG_RETURN(THDVAR(thd, sync_time_zone));
}

/*
  FALSE: not use
  TRUE:  use
 */
static MYSQL_THDVAR_BOOL(
  use_default_database, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Use default database", /* comment */
  NULL, /* check */
  NULL, /* update */
  TRUE /* def */
);

bool spider_param_use_default_database(
  THD *thd
) {
  DBUG_ENTER("spider_param_use_default_database");
  DBUG_RETURN(THDVAR(thd, use_default_database));
}

/*
-1 :don't know or does not matter; don't send 'SET SQL_LOG_OFF' statement
 0 :do send 'SET SQL_LOG_OFF 0' statement to data nodes
 1 :do send 'SET SQL_LOG_OFF 1' statement to data nodes
*/
static MYSQL_THDVAR_INT(
  internal_sql_log_off,                                  /* name */
  PLUGIN_VAR_RQCMDARG,                                   /* opt */
  "Manage SQL_LOG_OFF mode statement to the data nodes", /* comment */
  NULL,                                                  /* check */
  NULL,                                                  /* update */
  -1,                                                    /* default */
  -1,                                                    /* min */
  1,                                                     /* max */
  0                                                      /* blk */
);

int spider_param_internal_sql_log_off(
  THD *thd
) {
  DBUG_ENTER("spider_param_internal_sql_log_off");
  DBUG_RETURN(THDVAR(thd, internal_sql_log_off));
}

/*
 -1 :use table parameter
  0-:bulk insert size
 */
static MYSQL_THDVAR_INT(
  bulk_size, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Bulk insert size", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_bulk_size(
  THD *thd,
  int bulk_size
) {
  DBUG_ENTER("spider_param_bulk_size");
  DBUG_RETURN(THDVAR(thd, bulk_size) < 0 ?
    bulk_size : THDVAR(thd, bulk_size));
}

/*
 -1 :use table parameter
  0 : Send "update" and "delete" statements one by one.
  1 : Send collected multiple "update" and "delete" statements.
      (Collected statements are sent one by one)
  2 : Send collected multiple "update" and "delete" statements.
      (Collected statements are sent together)
 */
static MYSQL_THDVAR_INT(
  bulk_update_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "The mode of bulk updating and deleting", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_bulk_update_mode(
  THD *thd,
  int bulk_update_mode
) {
  DBUG_ENTER("spider_param_bulk_update_mode");
  DBUG_RETURN(THDVAR(thd, bulk_update_mode) == -1 ?
    bulk_update_mode : THDVAR(thd, bulk_update_mode));
}

/*
 -1 :use table parameter
  0-:bulk update size
 */
static MYSQL_THDVAR_INT(
  bulk_update_size, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Bulk update size", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_bulk_update_size(
  THD *thd,
  int bulk_update_size
) {
  DBUG_ENTER("spider_param_bulk_update_size");
  DBUG_RETURN(THDVAR(thd, bulk_update_size) == -1 ?
    bulk_update_size : THDVAR(thd, bulk_update_size));
}

/*
 -1 :use table parameter
  0 :off
  1 :on
 */
static MYSQL_THDVAR_INT(
  internal_optimize, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Execute optimize to remote server", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_internal_optimize(
  THD *thd,
  int internal_optimize
) {
  DBUG_ENTER("spider_param_internal_optimize");
  DBUG_RETURN(THDVAR(thd, internal_optimize) == -1 ?
    internal_optimize : THDVAR(thd, internal_optimize));
}

/*
 -1 :use table parameter
  0 :off
  1 :on
 */
static MYSQL_THDVAR_INT(
  internal_optimize_local, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Execute optimize to remote server with local", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_internal_optimize_local(
  THD *thd,
  int internal_optimize_local
) {
  DBUG_ENTER("spider_param_internal_optimize_local");
  DBUG_RETURN(THDVAR(thd, internal_optimize_local) == -1 ?
    internal_optimize_local : THDVAR(thd, internal_optimize_local));
}

/*
  FALSE: off
  TRUE:  on
 */
static MYSQL_THDVAR_BOOL(
  use_flash_logs, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Execute flush logs to remote server", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_use_flash_logs(
  THD *thd
) {
  DBUG_ENTER("spider_param_use_flash_logs");
  DBUG_RETURN(THDVAR(thd, use_flash_logs));
}

/*
  0 :off
  1 :flush tables with read lock
  2 :flush tables another connection
 */
static MYSQL_THDVAR_INT(
  use_snapshot_with_flush_tables, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Execute optimize to remote server with local", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  0, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_use_snapshot_with_flush_tables(
  THD *thd
) {
  DBUG_ENTER("spider_param_use_snapshot_with_flush_tables");
  DBUG_RETURN(THDVAR(thd, use_snapshot_with_flush_tables));
}

/*
  FALSE: off
  TRUE:  on
 */
static MYSQL_THDVAR_BOOL(
  use_all_conns_snapshot, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "When start trx with snapshot, it send to all connections", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_use_all_conns_snapshot(
  THD *thd
) {
  DBUG_ENTER("spider_param_use_all_conns_snapshot");
  DBUG_RETURN(THDVAR(thd, use_all_conns_snapshot));
}

/*
  FALSE: off
  TRUE:  on
 */
static MYSQL_THDVAR_BOOL(
  lock_exchange, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Exchange select lock to lock tables", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_lock_exchange(
  THD *thd
) {
  DBUG_ENTER("spider_param_lock_exchange");
  DBUG_RETURN(THDVAR(thd, lock_exchange));
}

/*
  FALSE: off
  TRUE:  on
 */
static MYSQL_THDVAR_BOOL(
  internal_unlock, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Unlock tables for using connections in sql", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_internal_unlock(
  THD *thd
) {
  DBUG_ENTER("spider_param_internal_unlock");
  DBUG_RETURN(THDVAR(thd, internal_unlock));
}

/*
  FALSE: off
  TRUE:  on
 */
static MYSQL_THDVAR_BOOL(
  semi_trx, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Take a transaction during execute a sql", /* comment */
  NULL, /* check */
  NULL, /* update */
  TRUE /* def */
);

bool spider_param_semi_trx(
  THD *thd
) {
  DBUG_ENTER("spider_param_semi_trx");
  DBUG_RETURN(THDVAR(thd, semi_trx));
}

/*
 -1 :use table parameter
  0-:seconds of timeout
 */
static MYSQL_THDVAR_INT(
  connect_timeout, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Wait timeout of connecting to remote server", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_connect_timeout(
  THD *thd,
  int connect_timeout
) {
  DBUG_ENTER("spider_param_connect_timeout");
  if (thd)
    DBUG_RETURN(THDVAR(thd, connect_timeout) == -1 ?
      connect_timeout : THDVAR(thd, connect_timeout));
  DBUG_RETURN(connect_timeout);
}

/*
 -1 :use table parameter
  0-:seconds of timeout
 */
static MYSQL_THDVAR_INT(
  net_read_timeout, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Wait timeout of receiving data from remote server", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_net_read_timeout(
  THD *thd,
  int net_read_timeout
) {
  DBUG_ENTER("spider_param_net_read_timeout");
  if (thd)
    DBUG_RETURN(THDVAR(thd, net_read_timeout) == -1 ?
      net_read_timeout : THDVAR(thd, net_read_timeout));
  DBUG_RETURN(net_read_timeout);
}

/*
 -1 :use table parameter
  0-:seconds of timeout
 */
static MYSQL_THDVAR_INT(
  net_write_timeout, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Wait timeout of sending data to remote server", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_net_write_timeout(
  THD *thd,
  int net_write_timeout
) {
  DBUG_ENTER("spider_param_net_write_timeout");
  if (thd)
    DBUG_RETURN(THDVAR(thd, net_write_timeout) == -1 ?
      net_write_timeout : THDVAR(thd, net_write_timeout));
  DBUG_RETURN(net_write_timeout);
}

/*
 -1 :use table parameter
  0 :It acquires it collectively.
  1 :Acquisition one by one.If it discontinues once, and it will need
     it later, it retrieves it again when there is interrupt on the way.
  2 :Acquisition one by one.Interrupt is waited for until end of getting
     result when there is interrupt on the way.
 */
static MYSQL_THDVAR_INT(
  quick_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "The retrieval result from a remote server is acquired by acquisition one by one", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  3, /* max */
  0 /* blk */
);

int spider_param_quick_mode(
  THD *thd,
  int quick_mode
) {
  DBUG_ENTER("spider_param_quick_mode");
  DBUG_RETURN(THDVAR(thd, quick_mode) < 0 ?
    quick_mode : THDVAR(thd, quick_mode));
}

/*
 -1 :use table parameter
  0-:number of records
 */
static MYSQL_THDVAR_LONGLONG(
  quick_page_size, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Number of records in a page when acquisition one by one", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_quick_page_size(
  THD *thd,
  longlong quick_page_size
) {
  DBUG_ENTER("spider_param_quick_page_size");
  DBUG_RETURN(THDVAR(thd, quick_page_size) < 0 ?
    quick_page_size : THDVAR(thd, quick_page_size));
}

/*
 -1 :use table parameter
  0 :It doesn't use low memory mode.
  1 :It uses low memory mode.
 */
static MYSQL_THDVAR_INT(
  low_mem_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Use low memory mode when SQL(SELECT) internally issued to a remote server is executed and get a result list", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_low_mem_read(
  THD *thd,
  int low_mem_read
) {
  DBUG_ENTER("spider_param_low_mem_read");
  DBUG_RETURN(THDVAR(thd, low_mem_read) < 0 ?
    low_mem_read : THDVAR(thd, low_mem_read));
}

/*
 -1 :use table parameter
  0 :Use index columns if select statement can solve by using index,
     otherwise use all columns.
  1 :Use columns that are judged necessary.
 */
static MYSQL_THDVAR_INT(
  select_column_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "The mode of using columns at select clause", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_select_column_mode(
  THD *thd,
  int select_column_mode
) {
  DBUG_ENTER("spider_param_select_column_mode");
  DBUG_RETURN(THDVAR(thd, select_column_mode) == -1 ?
    select_column_mode : THDVAR(thd, select_column_mode));
}

#ifndef WITHOUT_SPIDER_BG_SEARCH
/*
 -1 :use table parameter
  0 :background search is disabled
  1 :background search is used if search with no lock
  2 :background search is used if search with no lock or shared lock
  3 :background search is used regardless of the lock
 */
static MYSQL_THDVAR_INT(
  bgs_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of background search", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  3, /* max */
  0 /* blk */
);

int spider_param_bgs_mode(
  THD *thd,
  int bgs_mode
) {
  DBUG_ENTER("spider_param_bgs_mode");
  DBUG_RETURN(THDVAR(thd, bgs_mode) < 0 ?
    bgs_mode : THDVAR(thd, bgs_mode));
}

/*
 -1 :use table parameter
  0 :records is gotten usually
  1-:number of records
 */
static MYSQL_THDVAR_LONGLONG(
  bgs_first_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Number of first read records when background search is used", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_bgs_first_read(
  THD *thd,
  longlong bgs_first_read
) {
  DBUG_ENTER("spider_param_bgs_first_read");
  DBUG_RETURN(THDVAR(thd, bgs_first_read) < 0 ?
    bgs_first_read : THDVAR(thd, bgs_first_read));
}

/*
 -1 :use table parameter
  0 :records is gotten usually
  1-:number of records
 */
static MYSQL_THDVAR_LONGLONG(
  bgs_second_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Number of second read records when background search is used", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_bgs_second_read(
  THD *thd,
  longlong bgs_second_read
) {
  DBUG_ENTER("spider_param_bgs_second_read");
  DBUG_RETURN(THDVAR(thd, bgs_second_read) < 0 ?
    bgs_second_read : THDVAR(thd, bgs_second_read));
}
#endif

/*
 -1 :use table parameter
  0 :records is gotten usually
  1-:number of records
 */
static MYSQL_THDVAR_LONGLONG(
  first_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Number of first read records", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_first_read(
  THD *thd,
  longlong first_read
) {
  DBUG_ENTER("spider_param_first_read");
  DBUG_RETURN(THDVAR(thd, first_read) < 0 ?
    first_read : THDVAR(thd, first_read));
}

/*
 -1 :use table parameter
  0 :records is gotten usually
  1-:number of records
 */
static MYSQL_THDVAR_LONGLONG(
  second_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Number of second read records", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_second_read(
  THD *thd,
  longlong second_read
) {
  DBUG_ENTER("spider_param_second_read");
  DBUG_RETURN(THDVAR(thd, second_read) < 0 ?
    second_read : THDVAR(thd, second_read));
}

/*
 -1 :use table parameter
  0 :always get the newest information
  1-:interval
 */
static MYSQL_THDVAR_INT(
  crd_interval, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Interval of cardinality confirmation.(second)", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

double spider_param_crd_interval(
  THD *thd,
  double crd_interval
) {
  DBUG_ENTER("spider_param_crd_interval");
  DBUG_RETURN(THDVAR(thd, crd_interval) == -1 ?
    crd_interval : THDVAR(thd, crd_interval));
}

/*
 -1 :use table parameter
  0 :use table parameter
  1 :use show command
  2 :use information schema
  3 :use explain
 */
static MYSQL_THDVAR_INT(
  crd_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of cardinality confirmation.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  3, /* max */
  0 /* blk */
);

int spider_param_crd_mode(
  THD *thd,
  int crd_mode
) {
  DBUG_ENTER("spider_param_crd_mode");
  DBUG_RETURN(THDVAR(thd, crd_mode) <= 0 ?
    crd_mode : THDVAR(thd, crd_mode));
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
/*
 -1 :use table parameter
  0 :No synchronization.
  1 :Cardinality is synchronized when opening a table.
     Then no synchronization.
  2 :Synchronization.
 */
static MYSQL_THDVAR_INT(
  crd_sync, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Cardinality synchronization in partitioned table.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_crd_sync(
  THD *thd,
  int crd_sync
) {
  DBUG_ENTER("spider_param_crd_sync");
  DBUG_RETURN(THDVAR(thd, crd_sync) == -1 ?
    crd_sync : THDVAR(thd, crd_sync));
}
#endif

/*
 -1 :use table parameter
  0 :The crd_weight is used as a fixed value.
  1 :The crd_weight is used as an addition value.
  2 :The crd_weight is used as a multiplication value.
 */
static MYSQL_THDVAR_INT(
  crd_type, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Type of cardinality calculation.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_crd_type(
  THD *thd,
  int crd_type
) {
  DBUG_ENTER("spider_param_crd_type");
  DBUG_RETURN(THDVAR(thd, crd_type) == -1 ?
    crd_type : THDVAR(thd, crd_type));
}

/*
 -1 :use table parameter
  0-:weight
 */
static MYSQL_THDVAR_INT(
  crd_weight, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Weight coefficient to calculate effectiveness of index from cardinality of column.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

double spider_param_crd_weight(
  THD *thd,
  double crd_weight
) {
  DBUG_ENTER("spider_param_crd_weight");
  DBUG_RETURN(THDVAR(thd, crd_weight) == -1 ?
    crd_weight : THDVAR(thd, crd_weight));
}

#ifndef WITHOUT_SPIDER_BG_SEARCH
/*
 -1 :use table parameter
  0 :Background confirmation is disabled
  1 :Background confirmation is enabled
 */
static MYSQL_THDVAR_INT(
  crd_bg_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of cardinality confirmation at background.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_crd_bg_mode(
  THD *thd,
  int crd_bg_mode
) {
  DBUG_ENTER("spider_param_crd_bg_mode");
  DBUG_RETURN(THDVAR(thd, crd_bg_mode) == -1 ?
    crd_bg_mode : THDVAR(thd, crd_bg_mode));
}
#endif

/*
 -1 :use table parameter
  0 :always get the newest information
  1-:interval
 */
static MYSQL_THDVAR_INT(
  sts_interval, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Interval of table state confirmation.(second)", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2147483647, /* max */
  0 /* blk */
);

double spider_param_sts_interval(
  THD *thd,
  double sts_interval
) {
  DBUG_ENTER("spider_param_sts_interval");
  DBUG_RETURN(THDVAR(thd, sts_interval) == -1 ?
    sts_interval : THDVAR(thd, sts_interval));
}

/*
 -1 :use table parameter
  0 :use table parameter
  1 :use show command
  2 :use information schema
 */
static MYSQL_THDVAR_INT(
  sts_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of table state confirmation.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_sts_mode(
  THD *thd,
  int sts_mode
) {
  DBUG_ENTER("spider_param_sts_mode");
  DBUG_RETURN(THDVAR(thd, sts_mode) <= 0 ?
    sts_mode : THDVAR(thd, sts_mode));
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
/*
 -1 :use table parameter
  0 :No synchronization.
  1 :Table state is synchronized when opening a table.
     Then no synchronization.
  2 :Synchronization.
 */
static MYSQL_THDVAR_INT(
  sts_sync, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Table state synchronization in partitioned table.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_sts_sync(
  THD *thd,
  int sts_sync
) {
  DBUG_ENTER("spider_param_sts_sync");
  DBUG_RETURN(THDVAR(thd, sts_sync) == -1 ?
    sts_sync : THDVAR(thd, sts_sync));
}
#endif

#ifndef WITHOUT_SPIDER_BG_SEARCH
/*
 -1 :use table parameter
  0 :Background confirmation is disabled
  1 :Background confirmation is enabled
 */
static MYSQL_THDVAR_INT(
  sts_bg_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of table state confirmation at background.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_sts_bg_mode(
  THD *thd,
  int sts_bg_mode
) {
  DBUG_ENTER("spider_param_sts_bg_mode");
  DBUG_RETURN(THDVAR(thd, sts_bg_mode) == -1 ?
    sts_bg_mode : THDVAR(thd, sts_bg_mode));
}
#endif

/*
  0 :always ping
  1-:interval
 */
static MYSQL_THDVAR_INT(
  ping_interval_at_trx_start, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Ping interval at transaction start", /* comment */
  NULL, /* check */
  NULL, /* update */
  3600, /* def */
  0, /* min */
  2147483647, /* max */
  0 /* blk */
);

double spider_param_ping_interval_at_trx_start(
  THD *thd
) {
  DBUG_ENTER("spider_param_ping_interval_at_trx_start");
  DBUG_RETURN(THDVAR(thd, ping_interval_at_trx_start));
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
/*
  0 :always ping
  1-:interval
 */
static MYSQL_THDVAR_INT(
  hs_ping_interval, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Ping interval for handlersocket", /* comment */
  NULL, /* check */
  NULL, /* update */
  30, /* def */
  0, /* min */
  2147483647, /* max */
  0 /* blk */
);

double spider_param_hs_ping_interval(
  THD *thd
) {
  DBUG_ENTER("spider_param_hs_ping_interval");
  DBUG_RETURN(THDVAR(thd, hs_ping_interval));
}
#endif

/*
 -1 :use table parameter
  0 :normal mode
  1 :quick mode
  2 :set 0 value
 */
static MYSQL_THDVAR_INT(
  auto_increment_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of auto increment.", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  3, /* max */
  0 /* blk */
);

int spider_param_auto_increment_mode(
  THD *thd,
  int auto_increment_mode
) {
  DBUG_ENTER("spider_param_auto_increment_mode");
  DBUG_RETURN(THDVAR(thd, auto_increment_mode) == -1 ?
    auto_increment_mode : THDVAR(thd, auto_increment_mode));
}

/*
  FALSE: off
  TRUE:  on
 */
static MYSQL_THDVAR_BOOL(
  same_server_link, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Permit one to link same server's table", /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_same_server_link(
  THD *thd
) {
  DBUG_ENTER("spider_param_same_server_link");
  DBUG_RETURN(THDVAR(thd, same_server_link));
}

/*
  FALSE: transmits
  TRUE:  don't transmit
 */
static MYSQL_THDVAR_BOOL(
  local_lock_table, /* name */
  PLUGIN_VAR_OPCMDARG, /* opt */
  "Remote server transmission when lock tables is executed at local",
    /* comment */
  NULL, /* check */
  NULL, /* update */
  FALSE /* def */
);

bool spider_param_local_lock_table(
  THD *thd
) {
  DBUG_ENTER("spider_param_local_lock_table");
  DBUG_RETURN(THDVAR(thd, local_lock_table));
}

/*
 -1 :use table parameter
  0 :don't transmit
  1 :transmits
 */
static MYSQL_THDVAR_INT(
  use_pushdown_udf, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Remote server transmission existence when UDF is used at condition and \"engine_condition_pushdown=1\"", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_use_pushdown_udf(
  THD *thd,
  int use_pushdown_udf
) {
  DBUG_ENTER("spider_param_use_pushdown_udf");
  DBUG_RETURN(THDVAR(thd, use_pushdown_udf) == -1 ?
    use_pushdown_udf : THDVAR(thd, use_pushdown_udf));
}

/*
 -1 :use table parameter
  0 :duplicate check on local server
  1 :avoid duplicate check on local server
 */
static MYSQL_THDVAR_INT(
  direct_dup_insert, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Execute \"REPLACE\" and \"INSERT IGNORE\" on remote server and avoid duplicate check on local server", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_direct_dup_insert(
  THD *thd,
  int direct_dup_insert
) {
  DBUG_ENTER("spider_param_direct_dup_insert");
  DBUG_RETURN(THDVAR(thd, direct_dup_insert) < 0 ?
    direct_dup_insert : THDVAR(thd, direct_dup_insert));
}

static uint spider_udf_table_lock_mutex_count;
/*
  1-: mutex count
 */
static MYSQL_SYSVAR_UINT(
  udf_table_lock_mutex_count,
  spider_udf_table_lock_mutex_count,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Mutex count of table lock for Spider UDFs",
  NULL,
  NULL,
  20,
  1,
  4294967295U,
  0
);

uint spider_param_udf_table_lock_mutex_count()
{
  DBUG_ENTER("spider_param_udf_table_lock_mutex_count");
  DBUG_RETURN(spider_udf_table_lock_mutex_count);
}

static uint spider_udf_table_mon_mutex_count;
/*
  1-: mutex count
 */
static MYSQL_SYSVAR_UINT(
  udf_table_mon_mutex_count,
  spider_udf_table_mon_mutex_count,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Mutex count of table mon for Spider UDFs",
  NULL,
  NULL,
  20,
  1,
  4294967295U,
  0
);

uint spider_param_udf_table_mon_mutex_count()
{
  DBUG_ENTER("spider_param_udf_table_mon_mutex_count");
  DBUG_RETURN(spider_udf_table_mon_mutex_count);
}

/*
  1-:number of rows
 */
static MYSQL_THDVAR_LONGLONG(
  udf_ds_bulk_insert_rows, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Number of rows for bulk inserting", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_udf_ds_bulk_insert_rows(
  THD *thd,
  longlong udf_ds_bulk_insert_rows
) {
  DBUG_ENTER("spider_param_udf_ds_bulk_insert_rows");
  DBUG_RETURN(THDVAR(thd, udf_ds_bulk_insert_rows) <= 0 ?
    udf_ds_bulk_insert_rows : THDVAR(thd, udf_ds_bulk_insert_rows));
}

/*
 -1 :use table parameter
  0 :drop records
  1 :insert last table
  2 :insert first table and loop again
 */
static MYSQL_THDVAR_INT(
  udf_ds_table_loop_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Table loop mode if the number of tables in table list are less than the number of result sets", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_udf_ds_table_loop_mode(
  THD *thd,
  int udf_ds_table_loop_mode
) {
  DBUG_ENTER("spider_param_udf_ds_table_loop_mode");
  DBUG_RETURN(THDVAR(thd, udf_ds_table_loop_mode) == -1 ?
    udf_ds_table_loop_mode : THDVAR(thd, udf_ds_table_loop_mode));
}

static char *spider_remote_access_charset;
/*
 */
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
static MYSQL_SYSVAR_STR(
  remote_access_charset,
  spider_remote_access_charset,
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Set remote access charset at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#else
#ifdef PLUGIN_VAR_CAN_MEMALLOC
static MYSQL_SYSVAR_STR(
  remote_access_charset,
  spider_remote_access_charset,
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Set remote access charset at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#else
static MYSQL_SYSVAR_STR(
  remote_access_charset,
  spider_remote_access_charset,
  PLUGIN_VAR_RQCMDARG,
  "Set remote access charset at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#endif
#endif

char *spider_param_remote_access_charset()
{
  DBUG_ENTER("spider_param_remote_access_charset");
  DBUG_RETURN(spider_remote_access_charset);
}

static int spider_remote_autocommit;
/*
 -1 :don't set
  0 :autocommit = 0
  1 :autocommit = 1
 */
static MYSQL_SYSVAR_INT(
  remote_autocommit,
  spider_remote_autocommit,
  PLUGIN_VAR_RQCMDARG,
  "Set autocommit mode at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  -1,
  -1,
  1,
  0
);

int spider_param_remote_autocommit()
{
  DBUG_ENTER("spider_param_remote_autocommit");
  DBUG_RETURN(spider_remote_autocommit);
}

static char *spider_remote_time_zone;
/*
 */
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
static MYSQL_SYSVAR_STR(
  remote_time_zone,
  spider_remote_time_zone,
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Set remote time_zone at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#else
#ifdef PLUGIN_VAR_CAN_MEMALLOC
static MYSQL_SYSVAR_STR(
  remote_time_zone,
  spider_remote_time_zone,
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Set remote time_zone at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#else
static MYSQL_SYSVAR_STR(
  remote_time_zone,
  spider_remote_time_zone,
  PLUGIN_VAR_RQCMDARG,
  "Set remote time_zone at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#endif
#endif

char *spider_param_remote_time_zone()
{
  DBUG_ENTER("spider_param_remote_time_zone");
  DBUG_RETURN(spider_remote_time_zone);
}

static int spider_remote_sql_log_off;
/*
 -1 :don't know the value on all data nodes, or does not matter
  0 :sql_log_off = 0 on all data nodes
  1 :sql_log_off = 1 on all data nodes
 */
static MYSQL_SYSVAR_INT(
  remote_sql_log_off,
  spider_remote_sql_log_off,
  PLUGIN_VAR_RQCMDARG,
  "Set SQL_LOG_OFF mode on connecting for improved performance of connection, if you know",
  NULL,
  NULL,
  -1,
  -1,
  1,
  0
);

int spider_param_remote_sql_log_off()
{
  DBUG_ENTER("spider_param_remote_sql_log_off");
  DBUG_RETURN(spider_remote_sql_log_off);
}

static int spider_remote_trx_isolation;
/*
 -1 :don't set
  0 :READ UNCOMMITTED
  1 :READ COMMITTED
  2 :REPEATABLE READ
  3 :SERIALIZABLE
 */
static MYSQL_SYSVAR_INT(
  remote_trx_isolation,
  spider_remote_trx_isolation,
  PLUGIN_VAR_RQCMDARG,
  "Set transaction isolation level at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  -1,
  -1,
  3,
  0
);

int spider_param_remote_trx_isolation()
{
  DBUG_ENTER("spider_param_remote_trx_isolation");
  DBUG_RETURN(spider_remote_trx_isolation);
}

static char *spider_remote_default_database;
/*
 */
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
static MYSQL_SYSVAR_STR(
  remote_default_database,
  spider_remote_default_database,
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Set remote database at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#else
#ifdef PLUGIN_VAR_CAN_MEMALLOC
static MYSQL_SYSVAR_STR(
  remote_default_database,
  spider_remote_default_database,
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Set remote database at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#else
static MYSQL_SYSVAR_STR(
  remote_default_database,
  spider_remote_default_database,
  PLUGIN_VAR_RQCMDARG,
  "Set remote database at connecting for improvement performance of connection if you know",
  NULL,
  NULL,
  NULL
);
#endif
#endif

char *spider_param_remote_default_database()
{
  DBUG_ENTER("spider_param_remote_default_database");
  DBUG_RETURN(spider_remote_default_database);
}

/*
  0-:connect retry interval (micro second)
 */
static MYSQL_THDVAR_LONGLONG(
  connect_retry_interval, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Connect retry interval", /* comment */
  NULL, /* check */
  NULL, /* update */
  1000, /* def */
  0, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_connect_retry_interval(
  THD *thd
) {
  DBUG_ENTER("spider_param_connect_retry_interval");
  if (thd)
    DBUG_RETURN(THDVAR(thd, connect_retry_interval));
  DBUG_RETURN(0);
}

/*
  0-:connect retry count
 */
static MYSQL_THDVAR_INT(
  connect_retry_count, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Connect retry count", /* comment */
  NULL, /* check */
  NULL, /* update */
  1000, /* def */
  0, /* min */
  2147483647, /* max */
  0 /* blk */
);

int spider_param_connect_retry_count(
  THD *thd
) {
  DBUG_ENTER("spider_param_connect_retry_count");
  if (thd)
    DBUG_RETURN(THDVAR(thd, connect_retry_count));
  DBUG_RETURN(0);
}

/*
 */
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
static MYSQL_THDVAR_STR(
  bka_engine, /* name */
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Temporary table's engine for BKA", /* comment */
  NULL, /* check */
  NULL, /* update */
  NULL /* def */
);
#else
#ifdef PLUGIN_VAR_CAN_MEMALLOC
static MYSQL_THDVAR_STR(
  bka_engine, /* name */
  PLUGIN_VAR_MEMALLOC |
  PLUGIN_VAR_RQCMDARG,
  "Temporary table's engine for BKA", /* comment */
  NULL, /* check */
  NULL, /* update */
  NULL /* def */
);
#else
static MYSQL_THDVAR_STR(
  bka_engine, /* name */
  PLUGIN_VAR_RQCMDARG,
  "Temporary table's engine for BKA", /* comment */
  NULL, /* check */
  NULL, /* update */
  NULL /* def */
);
#endif
#endif

char *spider_param_bka_engine(
  THD *thd,
  char *bka_engine
) {
  DBUG_ENTER("spider_param_bka_engine");
  DBUG_RETURN(THDVAR(thd, bka_engine) ?
    THDVAR(thd, bka_engine) : bka_engine);
}

/*
 -1 :use table parameter
  0 :use union all
  1 :use temporary table
 */
static MYSQL_THDVAR_INT(
  bka_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of BKA for Spider", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  2, /* max */
  0 /* blk */
);

int spider_param_bka_mode(
  THD *thd,
  int bka_mode
) {
  DBUG_ENTER("spider_param_bka_mode");
  DBUG_RETURN(THDVAR(thd, bka_mode) == -1 ?
    bka_mode : THDVAR(thd, bka_mode));
}

static int spider_udf_ct_bulk_insert_interval;
/*
 -1         : The UDF parameter is adopted.
  0 or more : Milliseconds.
 */
static MYSQL_SYSVAR_INT(
  udf_ct_bulk_insert_interval,
  spider_udf_ct_bulk_insert_interval,
  PLUGIN_VAR_RQCMDARG,
  "The interval time between bulk insert and next bulk insert at coping",
  NULL,
  NULL,
  -1,
  -1,
  2147483647,
  0
);

int spider_param_udf_ct_bulk_insert_interval(
  int udf_ct_bulk_insert_interval
) {
  DBUG_ENTER("spider_param_udf_ct_bulk_insert_interval");
  DBUG_RETURN(spider_udf_ct_bulk_insert_interval < 0 ?
    udf_ct_bulk_insert_interval : spider_udf_ct_bulk_insert_interval);
}

static longlong spider_udf_ct_bulk_insert_rows;
/*
 -1,0       : The UDF parameter is adopted.
  1 or more : Number of rows.
 */
static MYSQL_SYSVAR_LONGLONG(
  udf_ct_bulk_insert_rows,
  spider_udf_ct_bulk_insert_rows,
  PLUGIN_VAR_RQCMDARG,
  "The number of rows inserted with bulk insert of one time at coping",
  NULL,
  NULL,
  -1,
  -1,
  9223372036854775807LL,
  0
);

longlong spider_param_udf_ct_bulk_insert_rows(
  longlong udf_ct_bulk_insert_rows
) {
  DBUG_ENTER("spider_param_udf_ct_bulk_insert_rows");
  DBUG_RETURN(spider_udf_ct_bulk_insert_rows <= 0 ?
    udf_ct_bulk_insert_rows : spider_udf_ct_bulk_insert_rows);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
/*
  0: no recycle
  1: recycle in instance
  2: recycle in thread
 */
static MYSQL_THDVAR_UINT(
  hs_r_conn_recycle_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Handlersocket connection recycle mode", /* comment */
  NULL, /* check */
  NULL, /* update */
  2, /* def */
  0, /* min */
  2, /* max */
  0 /* blk */
);

uint spider_param_hs_r_conn_recycle_mode(
  THD *thd
) {
  DBUG_ENTER("spider_param_hs_r_conn_recycle_mode");
  DBUG_RETURN(THDVAR(thd, hs_r_conn_recycle_mode));
}

/*
  0: weak
  1: strict
 */
static MYSQL_THDVAR_UINT(
  hs_r_conn_recycle_strict, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Strict handlersocket connection recycle", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  0, /* min */
  1, /* max */
  0 /* blk */
);

uint spider_param_hs_r_conn_recycle_strict(
  THD *thd
) {
  DBUG_ENTER("spider_param_hs_r_conn_recycle_strict");
  DBUG_RETURN(THDVAR(thd, hs_r_conn_recycle_strict));
}

/*
  0: no recycle
  1: recycle in instance
  2: recycle in thread
 */
static MYSQL_THDVAR_UINT(
  hs_w_conn_recycle_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Handlersocket connection recycle mode", /* comment */
  NULL, /* check */
  NULL, /* update */
  2, /* def */
  0, /* min */
  2, /* max */
  0 /* blk */
);

uint spider_param_hs_w_conn_recycle_mode(
  THD *thd
) {
  DBUG_ENTER("spider_param_hs_w_conn_recycle_mode");
  DBUG_RETURN(THDVAR(thd, hs_w_conn_recycle_mode));
}

/*
  0: weak
  1: strict
 */
static MYSQL_THDVAR_UINT(
  hs_w_conn_recycle_strict, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Strict handlersocket connection recycle", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  0, /* min */
  1, /* max */
  0 /* blk */
);

uint spider_param_hs_w_conn_recycle_strict(
  THD *thd
) {
  DBUG_ENTER("spider_param_hs_w_conn_recycle_strict");
  DBUG_RETURN(THDVAR(thd, hs_w_conn_recycle_strict));
}

/*
 -1 :use table parameter
  0 :not use
  1 :use handlersocket
 */
static MYSQL_THDVAR_INT(
  use_hs_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Use handlersocket for reading", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_use_hs_read(
  THD *thd,
  int use_hs_read
) {
  DBUG_ENTER("spider_param_use_hs_read");
  DBUG_RETURN(THDVAR(thd, use_hs_read) == -1 ?
    use_hs_read : THDVAR(thd, use_hs_read));
}

/*
 -1 :use table parameter
  0 :not use
  1 :use handlersocket
 */
static MYSQL_THDVAR_INT(
  use_hs_write, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Use handlersocket for writing", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_use_hs_write(
  THD *thd,
  int use_hs_write
) {
  DBUG_ENTER("spider_param_use_hs_write");
  DBUG_RETURN(THDVAR(thd, use_hs_write) == -1 ?
    use_hs_write : THDVAR(thd, use_hs_write));
}
#endif

/*
 -1 :use table parameter
  0 :not use
  1 :use handler
 */
static MYSQL_THDVAR_INT(
  use_handler, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Use handler for reading", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  3, /* max */
  0 /* blk */
);

int spider_param_use_handler(
  THD *thd,
  int use_handler
) {
  DBUG_ENTER("spider_param_use_handler");
  DBUG_RETURN(THDVAR(thd, use_handler) == -1 ?
    use_handler : THDVAR(thd, use_handler));
}

/*
 -1 :use table parameter
  0 :return error if error
  1 :return 0 record if error
 */
static MYSQL_THDVAR_INT(
  error_read_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Read error mode if error", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_error_read_mode(
  THD *thd,
  int error_read_mode
) {
  DBUG_ENTER("spider_param_error_read_mode");
  DBUG_RETURN(THDVAR(thd, error_read_mode) == -1 ?
    error_read_mode : THDVAR(thd, error_read_mode));
}

/*
 -1 :use table parameter
  0 :return error if error
  1 :return 0 record if error
 */
static MYSQL_THDVAR_INT(
  error_write_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Write error mode if error", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_error_write_mode(
  THD *thd,
  int error_write_mode
) {
  DBUG_ENTER("spider_param_error_write_mode");
  DBUG_RETURN(THDVAR(thd, error_write_mode) == -1 ?
    error_write_mode : THDVAR(thd, error_write_mode));
}

/*
 -1 :use table parameter
  0 :not skip
  1 :skip
 */
static MYSQL_THDVAR_INT(
  skip_default_condition, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Skip generating internal default condition", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_skip_default_condition(
  THD *thd,
  int skip_default_condition
) {
  DBUG_ENTER("spider_param_skip_default_condition");
  DBUG_RETURN(THDVAR(thd, skip_default_condition) == -1 ?
    skip_default_condition : THDVAR(thd, skip_default_condition));
}

/*
 -1 :use table parameter
  0 :not send directly
  1-:send directly
 */
static MYSQL_THDVAR_LONGLONG(
  direct_order_limit, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Send 'ORDER BY' and 'LIMIT' to remote server directly", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  9223372036854775807LL, /* max */
  0 /* blk */
);

longlong spider_param_direct_order_limit(
  THD *thd,
  longlong direct_order_limit
) {
  DBUG_ENTER("spider_param_direct_order_limit");
  DBUG_RETURN(THDVAR(thd, direct_order_limit) == -1 ?
    direct_order_limit : THDVAR(thd, direct_order_limit));
}

/*
 -1 :use table parameter
  0 :writable
  1 :read only
 */
static MYSQL_THDVAR_INT(
  read_only_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Read only", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_read_only_mode(
  THD *thd,
  int read_only_mode
) {
  DBUG_ENTER("spider_param_read_only_mode");
  DBUG_RETURN(THDVAR(thd, read_only_mode) == -1 ?
    read_only_mode : THDVAR(thd, read_only_mode));
}

#ifdef HA_CAN_BULK_ACCESS
static int spider_bulk_access_free;
/*
 -1 :use table parameter
  0 :in reset
  1 :in close
 */
static MYSQL_SYSVAR_INT(
  bulk_access_free,
  spider_bulk_access_free,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Free mode of bulk access resources",
  NULL,
  NULL,
  -1,
  -1,
  1,
  0
);

int spider_param_bulk_access_free(
  int bulk_access_free
) {
  DBUG_ENTER("spider_param_bulk_access_free");
  DBUG_RETURN(spider_bulk_access_free == -1 ?
    bulk_access_free : spider_bulk_access_free);
}
#endif

#if MYSQL_VERSION_ID < 50500
#else
/*
 -1 :use UDF parameter
  0 :can not use
  1 :can use
 */
static MYSQL_THDVAR_INT(
  udf_ds_use_real_table, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Use real table for temporary table list", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_udf_ds_use_real_table(
  THD *thd,
  int udf_ds_use_real_table
) {
  DBUG_ENTER("spider_param_udf_ds_use_real_table");
  DBUG_RETURN(THDVAR(thd, udf_ds_use_real_table) == -1 ?
    udf_ds_use_real_table : THDVAR(thd, udf_ds_use_real_table));
}
#endif

static my_bool spider_general_log;
static MYSQL_SYSVAR_BOOL(
  general_log,
  spider_general_log,
  PLUGIN_VAR_OPCMDARG,
  "Log query to remote server in general log",
  NULL,
  NULL,
  FALSE
);

my_bool spider_param_general_log()
{
  DBUG_ENTER("spider_param_general_log");
  DBUG_RETURN(spider_general_log);
}

static uint spider_log_result_errors;
/*
  0: no log
  1: log error
  2: log warning summary
  3: log warning
  4: log info
 */
static MYSQL_SYSVAR_UINT(
  log_result_errors,
  spider_log_result_errors,
  PLUGIN_VAR_RQCMDARG,
  "Log error from remote server in error log",
  NULL,
  NULL,
  0,
  0,
  4,
  0
);

uint spider_param_log_result_errors()
{
  DBUG_ENTER("spider_param_log_result_errors");
  DBUG_RETURN(spider_log_result_errors);
}

static uint spider_log_result_error_with_sql;
/*
  0: no log
  1: log spider sql at logging result errors
  2: log user sql at logging result errors
  3: log both sql at logging result errors
 */
static MYSQL_SYSVAR_UINT(
  log_result_error_with_sql,
  spider_log_result_error_with_sql,
  PLUGIN_VAR_RQCMDARG,
  "Log sql at logging result errors",
  NULL,
  NULL,
  0,
  0,
  3,
  0
);

uint spider_param_log_result_error_with_sql()
{
  DBUG_ENTER("spider_param_log_result_error_with_sql");
  DBUG_RETURN(spider_log_result_error_with_sql);
}

static char *spider_version = (char *) SPIDER_DETAIL_VERSION;
static MYSQL_SYSVAR_STR(
  version,
  spider_version,
  PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_READONLY,
  "The version of Spider",
  NULL,
  NULL,
  SPIDER_DETAIL_VERSION
);

/*
  0: server_id + thread_id
  1: server_id + thread_id + query_id
 */
static MYSQL_THDVAR_UINT(
  internal_xa_id_type, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "The type of internal_xa id", /* comment */
  NULL, /* check */
  NULL, /* update */
  0, /* def */
  0, /* min */
  1, /* max */
  0 /* blk */
);

uint spider_param_internal_xa_id_type(
  THD *thd
) {
  DBUG_ENTER("spider_param_internal_xa_id_type");
  DBUG_RETURN(THDVAR(thd, internal_xa_id_type));
}

/*
 -1 :use table parameter
  0 :OFF
  1 :automatic channel
  2-63 :use custom channel
 */
static MYSQL_THDVAR_INT(
  casual_read, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Read casually if it is possible", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  63, /* max */
  0 /* blk */
);

int spider_param_casual_read(
  THD *thd,
  int casual_read
) {
  DBUG_ENTER("spider_param_casual_read");
  DBUG_RETURN(THDVAR(thd, casual_read) == -1 ?
    casual_read : THDVAR(thd, casual_read));
}

static my_bool spider_dry_access;
static MYSQL_SYSVAR_BOOL(
  dry_access,
  spider_dry_access,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "dry access",
  NULL,
  NULL,
  FALSE
);

my_bool spider_param_dry_access()
{
  DBUG_ENTER("spider_param_dry_access");
  DBUG_RETURN(spider_dry_access);
}

/*
 -1 :use table parameter
  0 :fast
  1 :correct delete row number
 */
static MYSQL_THDVAR_INT(
  delete_all_rows_type, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "The type of delete_all_rows", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_delete_all_rows_type(
  THD *thd,
  int delete_all_rows_type
) {
  DBUG_ENTER("spider_param_delete_all_rows_type");
  DBUG_RETURN(THDVAR(thd, delete_all_rows_type) == -1 ?
    delete_all_rows_type : THDVAR(thd, delete_all_rows_type));
}

/*
 -1 :use table parameter
  0 :compact
  1 :add original table name
 */
static MYSQL_THDVAR_INT(
  bka_table_name_type, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "The type of temporary table name for bka", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int spider_param_bka_table_name_type(
  THD *thd,
  int bka_table_name_type
) {
  DBUG_ENTER("spider_param_bka_table_name_type");
  DBUG_RETURN(THDVAR(thd, bka_table_name_type) == -1 ?
    bka_table_name_type : THDVAR(thd, bka_table_name_type));
}

static struct st_mysql_storage_engine spider_storage_engine =
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

static struct st_mysql_sys_var* spider_system_variables[] = {
  MYSQL_SYSVAR(support_xa),
  MYSQL_SYSVAR(table_init_error_interval),
  MYSQL_SYSVAR(use_table_charset),
  MYSQL_SYSVAR(conn_recycle_mode),
  MYSQL_SYSVAR(conn_recycle_strict),
  MYSQL_SYSVAR(sync_trx_isolation),
  MYSQL_SYSVAR(use_consistent_snapshot),
  MYSQL_SYSVAR(internal_xa),
  MYSQL_SYSVAR(internal_xa_snapshot),
  MYSQL_SYSVAR(force_commit),
  MYSQL_SYSVAR(internal_offset),
  MYSQL_SYSVAR(internal_limit),
  MYSQL_SYSVAR(split_read),
  MYSQL_SYSVAR(semi_split_read),
  MYSQL_SYSVAR(semi_split_read_limit),
  MYSQL_SYSVAR(init_sql_alloc_size),
  MYSQL_SYSVAR(reset_sql_alloc),
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  MYSQL_SYSVAR(hs_result_free_size),
#endif
  MYSQL_SYSVAR(multi_split_read),
  MYSQL_SYSVAR(max_order),
  MYSQL_SYSVAR(semi_trx_isolation),
  MYSQL_SYSVAR(semi_table_lock),
  MYSQL_SYSVAR(semi_table_lock_connection),
  MYSQL_SYSVAR(block_size),
  MYSQL_SYSVAR(selupd_lock_mode),
  MYSQL_SYSVAR(sync_autocommit),
  MYSQL_SYSVAR(sync_time_zone),
  MYSQL_SYSVAR(use_default_database),
  MYSQL_SYSVAR(internal_sql_log_off),
  MYSQL_SYSVAR(bulk_size),
  MYSQL_SYSVAR(bulk_update_mode),
  MYSQL_SYSVAR(bulk_update_size),
  MYSQL_SYSVAR(internal_optimize),
  MYSQL_SYSVAR(internal_optimize_local),
  MYSQL_SYSVAR(use_flash_logs),
  MYSQL_SYSVAR(use_snapshot_with_flush_tables),
  MYSQL_SYSVAR(use_all_conns_snapshot),
  MYSQL_SYSVAR(lock_exchange),
  MYSQL_SYSVAR(internal_unlock),
  MYSQL_SYSVAR(semi_trx),
  MYSQL_SYSVAR(connect_timeout),
  MYSQL_SYSVAR(net_read_timeout),
  MYSQL_SYSVAR(net_write_timeout),
  MYSQL_SYSVAR(quick_mode),
  MYSQL_SYSVAR(quick_page_size),
  MYSQL_SYSVAR(low_mem_read),
  MYSQL_SYSVAR(select_column_mode),
#ifndef WITHOUT_SPIDER_BG_SEARCH
  MYSQL_SYSVAR(bgs_mode),
  MYSQL_SYSVAR(bgs_first_read),
  MYSQL_SYSVAR(bgs_second_read),
#endif
  MYSQL_SYSVAR(first_read),
  MYSQL_SYSVAR(second_read),
  MYSQL_SYSVAR(crd_interval),
  MYSQL_SYSVAR(crd_mode),
#ifdef WITH_PARTITION_STORAGE_ENGINE
  MYSQL_SYSVAR(crd_sync),
#endif
  MYSQL_SYSVAR(crd_type),
  MYSQL_SYSVAR(crd_weight),
#ifndef WITHOUT_SPIDER_BG_SEARCH
  MYSQL_SYSVAR(crd_bg_mode),
#endif
  MYSQL_SYSVAR(sts_interval),
  MYSQL_SYSVAR(sts_mode),
#ifdef WITH_PARTITION_STORAGE_ENGINE
  MYSQL_SYSVAR(sts_sync),
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
  MYSQL_SYSVAR(sts_bg_mode),
#endif
  MYSQL_SYSVAR(ping_interval_at_trx_start),
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  MYSQL_SYSVAR(hs_ping_interval),
#endif
  MYSQL_SYSVAR(auto_increment_mode),
  MYSQL_SYSVAR(same_server_link),
  MYSQL_SYSVAR(local_lock_table),
  MYSQL_SYSVAR(use_pushdown_udf),
  MYSQL_SYSVAR(direct_dup_insert),
  MYSQL_SYSVAR(udf_table_lock_mutex_count),
  MYSQL_SYSVAR(udf_table_mon_mutex_count),
  MYSQL_SYSVAR(udf_ds_bulk_insert_rows),
  MYSQL_SYSVAR(udf_ds_table_loop_mode),
  MYSQL_SYSVAR(remote_access_charset),
  MYSQL_SYSVAR(remote_autocommit),
  MYSQL_SYSVAR(remote_time_zone),
  MYSQL_SYSVAR(remote_sql_log_off),
  MYSQL_SYSVAR(remote_trx_isolation),
  MYSQL_SYSVAR(remote_default_database),
  MYSQL_SYSVAR(connect_retry_interval),
  MYSQL_SYSVAR(connect_retry_count),
  MYSQL_SYSVAR(connect_mutex),
  MYSQL_SYSVAR(bka_engine),
  MYSQL_SYSVAR(bka_mode),
  MYSQL_SYSVAR(udf_ct_bulk_insert_interval),
  MYSQL_SYSVAR(udf_ct_bulk_insert_rows),
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  MYSQL_SYSVAR(hs_r_conn_recycle_mode),
  MYSQL_SYSVAR(hs_r_conn_recycle_strict),
  MYSQL_SYSVAR(hs_w_conn_recycle_mode),
  MYSQL_SYSVAR(hs_w_conn_recycle_strict),
  MYSQL_SYSVAR(use_hs_read),
  MYSQL_SYSVAR(use_hs_write),
#endif
  MYSQL_SYSVAR(use_handler),
  MYSQL_SYSVAR(error_read_mode),
  MYSQL_SYSVAR(error_write_mode),
  MYSQL_SYSVAR(skip_default_condition),
  MYSQL_SYSVAR(direct_order_limit),
  MYSQL_SYSVAR(read_only_mode),
#ifdef HA_CAN_BULK_ACCESS
  MYSQL_SYSVAR(bulk_access_free),
#endif
#if MYSQL_VERSION_ID < 50500
#else
  MYSQL_SYSVAR(udf_ds_use_real_table),
#endif
  MYSQL_SYSVAR(general_log),
  MYSQL_SYSVAR(log_result_errors),
  MYSQL_SYSVAR(log_result_error_with_sql),
  MYSQL_SYSVAR(version),
  MYSQL_SYSVAR(internal_xa_id_type),
  MYSQL_SYSVAR(casual_read),
  MYSQL_SYSVAR(dry_access),
  MYSQL_SYSVAR(delete_all_rows_type),
  MYSQL_SYSVAR(bka_table_name_type),
  MYSQL_SYSVAR(connect_error_interval),
  NULL
};

mysql_declare_plugin(spider)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &spider_storage_engine,
  "SPIDER",
  "Kentoku Shiba",
  "Spider storage engine",
  PLUGIN_LICENSE_GPL,
  spider_db_init,
  spider_db_done,
  SPIDER_HEX_VERSION,
  spider_status_variables,
  spider_system_variables,
  NULL,
#if MYSQL_VERSION_ID >= 50600
  0,
#endif
},
spider_i_s_alloc_mem
mysql_declare_plugin_end;

#ifdef MARIADB_BASE_VERSION
maria_declare_plugin(spider)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &spider_storage_engine,
  "SPIDER",
  "Kentoku Shiba",
  "Spider storage engine",
  PLUGIN_LICENSE_GPL,
  spider_db_init,
  spider_db_done,
  SPIDER_HEX_VERSION,
  spider_status_variables,
  spider_system_variables,
  SPIDER_DETAIL_VERSION,
  MariaDB_PLUGIN_MATURITY_GAMMA
},
spider_i_s_alloc_mem_maria
maria_declare_plugin_end;
#endif
