/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB Corporation.
   Copyright (c) 2023, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA
*/

#ifndef STATUS_VAR_H
#define STATUS_VAR_H

#include "sql_cmd.h"                            /* SQLCOM_END */

class SQL_CATALOG;

/**
  Per thread status variables.
  Must be long/ulong up to last_system_status_var so that
  add_to_status/add_diff_to_status can work.
*/

typedef struct system_status_var
{
  ulong column_compressions;
  ulong column_decompressions;
  ulong com_stat[(uint) SQLCOM_END];
  ulong com_create_tmp_table;
  ulong com_drop_tmp_table;
  ulong com_other;

  ulong com_stmt_prepare;
  ulong com_stmt_reprepare;
  ulong com_stmt_execute;
  ulong com_stmt_send_long_data;
  ulong com_stmt_fetch;
  ulong com_stmt_reset;
  ulong com_stmt_close;

  ulong com_register_slave;
  ulong created_tmp_disk_tables_;
  ulong created_tmp_tables_;
  ulong ha_commit_count;
  ulong ha_delete_count;
  ulong ha_read_first_count;
  ulong ha_read_last_count;
  ulong ha_read_key_count;
  ulong ha_read_next_count;
  ulong ha_read_prev_count;
  ulong ha_read_retry_count;
  ulong ha_read_rnd_count;
  ulong ha_read_rnd_next_count;
  ulong ha_read_rnd_deleted_count;

  /*
    This number doesn't include calls to the default implementation and
    calls made by range access. The intent is to count only calls made by
    BatchedKeyAccess.
  */
  ulong ha_mrr_init_count;
  ulong ha_mrr_key_refills_count;
  ulong ha_mrr_rowid_refills_count;

  ulong ha_rollback_count;
  ulong ha_update_count;
  ulong ha_write_count;
  /* The following are for internal temporary tables */
  ulong ha_tmp_update_count;
  ulong ha_tmp_write_count;
  ulong ha_tmp_delete_count;
  ulong ha_prepare_count;
  ulong ha_icp_attempts;
  ulong ha_icp_match;
  ulong ha_discover_count;
  ulong ha_savepoint_count;
  ulong ha_savepoint_rollback_count;
  ulong ha_external_lock_count;

  ulong opened_tables;
  ulong opened_shares;
  ulong opened_views;               /* +1 opening a view */

  ulong select_full_join_count_;
  ulong select_full_range_join_count_;
  ulong select_range_count_;
  ulong select_range_check_count_;
  ulong select_scan_count_;
  ulong update_scan_count;
  ulong delete_scan_count;
  ulong executed_triggers;
  ulong long_query_count;
  ulong filesort_merge_passes_;
  ulong filesort_range_count_;
  ulong filesort_rows_;
  ulong filesort_scan_count_;
  ulong filesort_pq_sorts_;
  ulong optimizer_join_prefixes_check_calls;

  /* Features used */
  ulong feature_custom_aggregate_functions; /* +1 when custom aggregate
                                            functions are used */
  ulong feature_dynamic_columns;    /* +1 when creating a dynamic column */
  ulong feature_fulltext;	    /* +1 when MATCH is used */
  ulong feature_gis;                /* +1 opening table with GIS features */
  ulong feature_invisible_columns;  /* +1 opening table with invisible column */
  ulong feature_json;		    /* +1 when JSON function is used */
  ulong feature_locale;		    /* +1 when LOCALE is set */
  ulong feature_subquery;	    /* +1 when subqueries are used */
  ulong feature_system_versioning;  /* +1 opening table WITH SYSTEM VERSIONING */
  ulong feature_application_time_periods;
                                    /* +1 opening a table with application-time period */
  ulong feature_insert_returning;   /* +1 when INSERT...RETURNING is used */
  ulong feature_timezone;	    /* +1 when XPATH is used */
  ulong feature_trigger;	    /* +1 opening a table with triggers */
  ulong feature_xml;		    /* +1 when XPATH is used */
  ulong feature_window_functions;   /* +1 when window functions are used */
  ulong feature_into_outfile;       /* +1 when INTO OUTFILE is used */
  ulong feature_into_variable;      /* +1 when INTO VARIABLE is used */

  /* From MASTER_GTID_WAIT usage */
  ulong master_gtid_wait_timeouts;          /* Number of timeouts */
  ulong master_gtid_wait_time;              /* Time in microseconds */
  ulong master_gtid_wait_count;

  ulong empty_queries;
  ulong access_denied_errors;
  ulong lost_connections;
  ulong max_statement_time_exceeded;
  /*
   Number of times where column info was not
   sent with prepared statement metadata.
  */
  ulong skip_metadata_count;

  /*
    Number of statements sent from the client
  */
  ulong questions;
  /*
    IMPORTANT!
    SEE last_system_status_var DEFINITION BELOW.
    Below 'last_system_status_var' are all variables that cannot be handled
    automatically by add_to_status()/add_diff_to_status().
  */
  ulonglong bytes_received;
  ulonglong bytes_sent;
  ulonglong rows_read;
  ulonglong rows_sent;
  ulonglong rows_tmp_read;
  ulonglong binlog_bytes_written;
  ulonglong table_open_cache_hits;
  ulonglong table_open_cache_misses;
  ulonglong table_open_cache_overflows;
  ulonglong send_metadata_skips;
  double last_query_cost;
  double cpu_time, busy_time;
  uint32 threads_running;
  /* Don't initialize */
  /* Memory used for thread local storage */
  int64 max_local_memory_used;
  volatile int64 local_memory_used;
  /* Memory allocated for global usage */
  volatile int64 global_memory_used;
} STATUS_VAR;

/*
  This is used for 'SHOW STATUS'. It must be updated to the last ulong
  variable in system_status_var which is makes sense to add to the global
  counter
*/

#define last_system_status_var questions
#define last_cleared_system_status_var local_memory_used

/** Number of contiguous global status variables */
constexpr int COUNT_GLOBAL_STATUS_VARS= int(offsetof(STATUS_VAR,
                                                     last_system_status_var) /
                                            sizeof(ulong)) + 1;

/*
  Global status variables
*/

extern ulong feature_files_opened_with_delayed_keys, feature_check_constraint;

void add_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var);

void add_diff_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var,
                        STATUS_VAR *dec_var);

void calc_sum_of_all_status_if_needed2(STATUS_VAR *to, SQL_CATALOG *catalog);
static inline void calc_sum_of_all_status_if_needed(STATUS_VAR *to,
                                                    SQL_CATALOG *catalog)
{
  if (to->local_memory_used == 0)
    calc_sum_of_all_status_if_needed2(to, catalog);
}
#endif /* STATUS_VAR_H */
