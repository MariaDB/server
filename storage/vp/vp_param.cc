/* Copyright (C) 2009-2019 Kentoku Shiba
   Copyright (C) 2019 MariaDB corp

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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "vp_environ.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "sql_select.h"
#endif
#include "vp_include.h"
#include "ha_vp.h"
#include "vp_table.h"

static my_bool vp_support_xa;
static MYSQL_SYSVAR_BOOL(
  support_xa,
  vp_support_xa,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "XA support",
  NULL,
  NULL,
  TRUE
);

my_bool vp_param_support_xa()
{
  DBUG_ENTER("vp_param_support_xa");
  DBUG_RETURN(vp_support_xa);
}

/*
 -1 :use table parameter
  0 :minimum
  1 :sequential
 */
static MYSQL_THDVAR_INT(
  choose_table_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of choosing to access tables", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_choose_table_mode(
  THD *thd,
  int choose_table_mode
) {
  DBUG_ENTER("vp_param_choose_table_mode");
  DBUG_RETURN(THDVAR(thd, choose_table_mode) < 0 ?
    choose_table_mode : THDVAR(thd, choose_table_mode));
}

/*
 -1 :use table parameter
  0 :minimum
  1 :sequential
 */
static MYSQL_THDVAR_INT(
  choose_table_mode_for_lock, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of choosing to access tables for lock", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_choose_table_mode_for_lock(
  THD *thd,
  int choose_table_mode_for_lock
) {
  DBUG_ENTER("vp_param_choose_table_mode_for_lock");
  DBUG_RETURN(THDVAR(thd, choose_table_mode_for_lock) < 0 ?
    choose_table_mode_for_lock : THDVAR(thd, choose_table_mode_for_lock));
}

/*
 -1 :use table parameter
  0 :separete range
  1 :multi range
 */
static MYSQL_THDVAR_INT(
  multi_range_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of choosing to access tables", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_multi_range_mode(
  THD *thd,
  int multi_range_mode
) {
  DBUG_ENTER("vp_param_multi_range_mode");
  DBUG_RETURN(THDVAR(thd, multi_range_mode) < 0 ?
    multi_range_mode : THDVAR(thd, multi_range_mode));
}

/*
 -1 :use table parameter
  0 :separete range
  1 :multi range
 */
static MYSQL_THDVAR_INT(
  child_binlog, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of choosing to access tables", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_child_binlog(
  THD *thd,
  int child_binlog
) {
  DBUG_ENTER("vp_param_child_binlog");
  DBUG_RETURN(THDVAR(thd, child_binlog) < 0 ?
    child_binlog : THDVAR(thd, child_binlog));
}

#ifndef WITHOUT_VP_BG_ACCESS
/*
 -1 :use table parameter
  0 :background search is disabled
  1 :background search is enabled
 */
static MYSQL_THDVAR_INT(
  bgs_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of background search", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_bgs_mode(
  THD *thd,
  int bgs_mode
) {
  DBUG_ENTER("vp_param_bgs_mode");
  DBUG_RETURN(THDVAR(thd, bgs_mode) < 0 ?
    bgs_mode : THDVAR(thd, bgs_mode));
}

/*
 -1 :use table parameter
  0 :background insert is disabled
  1 :background insert is enabled
 */
static MYSQL_THDVAR_INT(
  bgi_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of background insert", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_bgi_mode(
  THD *thd,
  int bgi_mode
) {
  DBUG_ENTER("vp_param_bgi_mode");
  DBUG_RETURN(THDVAR(thd, bgi_mode) < 0 ?
    bgi_mode : THDVAR(thd, bgi_mode));
}

/*
 -1 :use table parameter
  0 :background update is disabled
  1 :background update is enabled
 */
static MYSQL_THDVAR_INT(
  bgu_mode, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of background update", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_bgu_mode(
  THD *thd,
  int bgu_mode
) {
  DBUG_ENTER("vp_param_bgu_mode");
  DBUG_RETURN(THDVAR(thd, bgu_mode) < 0 ?
    bgu_mode : THDVAR(thd, bgu_mode));
}
#endif

/*
 -1 :use table parameter
  0 :exchange bulk inserting to single inserting for safety
  1 :allow bulk inserting with auto_increment
 */
static MYSQL_THDVAR_INT(
  allow_bulk_autoinc, /* name */
  PLUGIN_VAR_RQCMDARG, /* opt */
  "Mode of bulk inserting into table with auto_increment", /* comment */
  NULL, /* check */
  NULL, /* update */
  -1, /* def */
  -1, /* min */
  1, /* max */
  0 /* blk */
);

int vp_param_allow_bulk_autoinc(
  THD *thd,
  int allow_bulk_autoinc
) {
  DBUG_ENTER("vp_param_allow_bulk_autoinc");
  DBUG_RETURN(THDVAR(thd, allow_bulk_autoinc) < 0 ?
    allow_bulk_autoinc : THDVAR(thd, allow_bulk_autoinc));
}

static int vp_udf_ct_bulk_insert_interval;
/*
 -1         : The UDF parameter is adopted.
  0 or more : Milliseconds.
 */
static MYSQL_SYSVAR_INT(
  udf_ct_bulk_insert_interval,
  vp_udf_ct_bulk_insert_interval,
  PLUGIN_VAR_RQCMDARG,
  "The interval time between bulk insert and next bulk insert at coping",
  NULL,
  NULL,
  -1,
  -1,
  2147483647,
  0
);

int vp_param_udf_ct_bulk_insert_interval(
  int udf_ct_bulk_insert_interval
) {
  DBUG_ENTER("vp_param_udf_ct_bulk_insert_interval");
  DBUG_RETURN(vp_udf_ct_bulk_insert_interval < 0 ?
    udf_ct_bulk_insert_interval : vp_udf_ct_bulk_insert_interval);
}

static longlong vp_udf_ct_bulk_insert_rows;
/*
 -1,0       : The UDF parameter is adopted.
  1 or more : Number of rows.
 */
static MYSQL_SYSVAR_LONGLONG(
  udf_ct_bulk_insert_rows,
  vp_udf_ct_bulk_insert_rows,
  PLUGIN_VAR_RQCMDARG,
  "The number of rows inserted with bulk insert of one time at coping",
  NULL,
  NULL,
  -1,
  -1,
  9223372036854775807LL,
  0
);

longlong vp_param_udf_ct_bulk_insert_rows(
  longlong udf_ct_bulk_insert_rows
) {
  DBUG_ENTER("vp_param_udf_ct_bulk_insert_rows");
  DBUG_RETURN(vp_udf_ct_bulk_insert_rows <= 0 ?
    udf_ct_bulk_insert_rows : vp_udf_ct_bulk_insert_rows);
}

static char *vp_version = (char *) VP_DETAIL_VERSION;
static MYSQL_SYSVAR_STR(
  version,
  vp_version,
  PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_READONLY,
  "The version of Vertical Partitioning",
  NULL,
  NULL,
  VP_DETAIL_VERSION
);

struct st_mysql_storage_engine vp_storage_engine =
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

struct st_mysql_sys_var* vp_system_variables[] = {
  MYSQL_SYSVAR(support_xa),
  MYSQL_SYSVAR(choose_table_mode),
  MYSQL_SYSVAR(choose_table_mode_for_lock),
  MYSQL_SYSVAR(multi_range_mode),
  MYSQL_SYSVAR(child_binlog),
#ifndef WITHOUT_VP_BG_ACCESS
  MYSQL_SYSVAR(bgs_mode),
  MYSQL_SYSVAR(bgi_mode),
  MYSQL_SYSVAR(bgu_mode),
#endif
  MYSQL_SYSVAR(allow_bulk_autoinc),
  MYSQL_SYSVAR(udf_ct_bulk_insert_interval),
  MYSQL_SYSVAR(udf_ct_bulk_insert_rows),
  MYSQL_SYSVAR(version),
  NULL
};

mysql_declare_plugin(vp)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &vp_storage_engine,
  "VP",
  "Kentoku Shiba",
  "Vertical Partitioning Storage Engine",
  PLUGIN_LICENSE_GPL,
  vp_db_init,
  vp_db_done,
  VP_HEX_VERSION,
  NULL,
  vp_system_variables,
  NULL,
#if MYSQL_VERSION_ID >= 50600
  0,
#endif
}
mysql_declare_plugin_end;

#ifdef MARIADB_BASE_VERSION
maria_declare_plugin(vp)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &vp_storage_engine,
  "VP",
  "Kentoku Shiba",
  "Vertical Partitioning Storage Engine",
  PLUGIN_LICENSE_GPL,
  vp_db_init,
  vp_db_done,
  VP_HEX_VERSION,
  NULL,
  vp_system_variables,
  VP_DETAIL_VERSION,
  MariaDB_PLUGIN_MATURITY_BETA
}
maria_declare_plugin_end;
#endif
