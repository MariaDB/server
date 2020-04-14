/* Copyright (C) 2008-2019 Kentoku Shiba
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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#define SPIDER_DB_WRAPPER_STR "mysql"
#define SPIDER_DB_WRAPPER_LEN (sizeof(SPIDER_DB_WRAPPER_STR) - 1)
#define SPIDER_DB_PK_NAME_STR "PRIMARY"
#define SPIDER_DB_PK_NAME_LEN (sizeof(SPIDER_DB_PK_NAME_STR) - 1)
#define SPIDER_DB_UNIQUE_NAME_STR "UNIQUE"
#define SPIDER_DB_UNIQUE_NAME_LEN (sizeof(SPIDER_DB_UNIQUE_NAME_STR) - 1)
#define SPIDER_DB_KEY_NAME_STR "KEY"
#define SPIDER_DB_KEY_NAME_LEN (sizeof(SPIDER_DB_KEY_NAME_STR) - 1)
#define SPIDER_DB_SEQUENCE_NAME_STR ""
#define SPIDER_DB_SEQUENCE_NAME_LEN (sizeof(SPIDER_DB_SEQUENCE_NAME_STR) - 1)

#define SPIDER_DB_TABLE_LOCK_READ_LOCAL         0
#define SPIDER_DB_TABLE_LOCK_READ               1
#define SPIDER_DB_TABLE_LOCK_LOW_PRIORITY_WRITE 2
#define SPIDER_DB_TABLE_LOCK_WRITE              3

#define SPIDER_DB_INSERT_REPLACE       (1 << 0)
#define SPIDER_DB_INSERT_IGNORE        (1 << 1)
#define SPIDER_DB_INSERT_LOW_PRIORITY  (1 << 2)
#define SPIDER_DB_INSERT_HIGH_PRIORITY (1 << 3)
#define SPIDER_DB_INSERT_DELAYED       (1 << 4)

#define SPIDER_SQL_OPEN_PAREN_STR "("
#define SPIDER_SQL_OPEN_PAREN_LEN (sizeof(SPIDER_SQL_OPEN_PAREN_STR) - 1)
#define SPIDER_SQL_CLOSE_PAREN_STR ")"
#define SPIDER_SQL_CLOSE_PAREN_LEN (sizeof(SPIDER_SQL_CLOSE_PAREN_STR) - 1)
#define SPIDER_SQL_COMMA_STR ","
#define SPIDER_SQL_COMMA_LEN (sizeof(SPIDER_SQL_COMMA_STR) - 1)
#define SPIDER_SQL_UNION_ALL_STR ")union all("
#define SPIDER_SQL_UNION_ALL_LEN (sizeof(SPIDER_SQL_UNION_ALL_STR) - 1)
#define SPIDER_SQL_NULL_STR "null"
#define SPIDER_SQL_NULL_LEN (sizeof(SPIDER_SQL_NULL_STR) - 1)
#define SPIDER_SQL_GT_STR " > "
#define SPIDER_SQL_GT_LEN (sizeof(SPIDER_SQL_GT_STR) - 1)
#define SPIDER_SQL_GTEQUAL_STR " >= "
#define SPIDER_SQL_GTEQUAL_LEN (sizeof(SPIDER_SQL_GTEQUAL_STR) - 1)
#define SPIDER_SQL_LT_STR " < "
#define SPIDER_SQL_LT_LEN (sizeof(SPIDER_SQL_LT_STR) - 1)
#define SPIDER_SQL_LTEQUAL_STR " <= "
#define SPIDER_SQL_LTEQUAL_LEN (sizeof(SPIDER_SQL_LTEQUAL_STR) - 1)

#define SPIDER_SQL_ID_STR "id"
#define SPIDER_SQL_ID_LEN (sizeof(SPIDER_SQL_ID_STR) - 1)
#define SPIDER_SQL_TMP_BKA_ENGINE_STR "memory"
#define SPIDER_SQL_TMP_BKA_ENGINE_LEN (sizeof(SPIDER_SQL_TMP_BKA_ENGINE_STR) - 1)

#define SPIDER_SQL_INSERT_STR "insert "
#define SPIDER_SQL_INSERT_LEN (sizeof(SPIDER_SQL_INSERT_STR) - 1)
#define SPIDER_SQL_REPLACE_STR "replace "
#define SPIDER_SQL_REPLACE_LEN (sizeof(SPIDER_SQL_REPLACE_STR) - 1)
#define SPIDER_SQL_SELECT_STR "select "
#define SPIDER_SQL_SELECT_LEN (sizeof(SPIDER_SQL_SELECT_STR) - 1)
#define SPIDER_SQL_UPDATE_STR "update "
#define SPIDER_SQL_UPDATE_LEN (sizeof(SPIDER_SQL_UPDATE_STR) - 1)
#define SPIDER_SQL_DELETE_STR "delete "
#define SPIDER_SQL_DELETE_LEN (sizeof(SPIDER_SQL_DELETE_STR) - 1)
#define SPIDER_SQL_DISTINCT_STR "distinct "
#define SPIDER_SQL_DISTINCT_LEN (sizeof(SPIDER_SQL_DISTINCT_STR) - 1)
#define SPIDER_SQL_HIGH_PRIORITY_STR "high_priority "
#define SPIDER_SQL_HIGH_PRIORITY_LEN (sizeof(SPIDER_SQL_HIGH_PRIORITY_STR) - 1)
#define SPIDER_SQL_LOW_PRIORITY_STR "low_priority "
#define SPIDER_SQL_LOW_PRIORITY_LEN (sizeof(SPIDER_SQL_LOW_PRIORITY_STR) - 1)
#define SPIDER_SQL_SQL_DELAYED_STR "delayed "
#define SPIDER_SQL_SQL_DELAYED_LEN (sizeof(SPIDER_SQL_SQL_DELAYED_STR) - 1)
#define SPIDER_SQL_SQL_IGNORE_STR "ignore "
#define SPIDER_SQL_SQL_IGNORE_LEN (sizeof(SPIDER_SQL_SQL_IGNORE_STR) - 1)
#define SPIDER_SQL_FROM_STR " from "
#define SPIDER_SQL_FROM_LEN (sizeof(SPIDER_SQL_FROM_STR) - 1)
#define SPIDER_SQL_WHERE_STR " where "
#define SPIDER_SQL_WHERE_LEN (sizeof(SPIDER_SQL_WHERE_STR) - 1)
#define SPIDER_SQL_OR_STR " or "
#define SPIDER_SQL_OR_LEN (sizeof(SPIDER_SQL_OR_STR) - 1)
#define SPIDER_SQL_ORDER_STR " order by "
#define SPIDER_SQL_ORDER_LEN (sizeof(SPIDER_SQL_ORDER_STR) - 1)
#define SPIDER_SQL_DESC_STR " desc"
#define SPIDER_SQL_DESC_LEN (sizeof(SPIDER_SQL_DESC_STR) - 1)
#define SPIDER_SQL_LIMIT_STR " limit "
#define SPIDER_SQL_LIMIT_LEN (sizeof(SPIDER_SQL_LIMIT_STR) - 1)
#define SPIDER_SQL_INTO_STR "into "
#define SPIDER_SQL_INTO_LEN (sizeof(SPIDER_SQL_INTO_STR) - 1)
#define SPIDER_SQL_VALUES_STR "values"
#define SPIDER_SQL_VALUES_LEN (sizeof(SPIDER_SQL_VALUES_STR) - 1)
#define SPIDER_SQL_SHARED_LOCK_STR " lock in share mode"
#define SPIDER_SQL_SHARED_LOCK_LEN (sizeof(SPIDER_SQL_SHARED_LOCK_STR) - 1)
#define SPIDER_SQL_FOR_UPDATE_STR " for update"
#define SPIDER_SQL_FOR_UPDATE_LEN (sizeof(SPIDER_SQL_FOR_UPDATE_STR) - 1)

#define SPIDER_SQL_SQL_ALTER_TABLE_STR "alter table "
#define SPIDER_SQL_SQL_ALTER_TABLE_LEN (sizeof(SPIDER_SQL_SQL_ALTER_TABLE_STR) - 1)
#define SPIDER_SQL_SQL_DISABLE_KEYS_STR " disable keys"
#define SPIDER_SQL_SQL_DISABLE_KEYS_LEN (sizeof(SPIDER_SQL_SQL_DISABLE_KEYS_STR) - 1)
#define SPIDER_SQL_SQL_ENABLE_KEYS_STR " enable keys"
#define SPIDER_SQL_SQL_ENABLE_KEYS_LEN (sizeof(SPIDER_SQL_SQL_ENABLE_KEYS_STR) - 1)
#define SPIDER_SQL_SQL_CHECK_TABLE_STR "check table "
#define SPIDER_SQL_SQL_CHECK_TABLE_LEN (sizeof(SPIDER_SQL_SQL_CHECK_TABLE_STR) - 1)
#define SPIDER_SQL_SQL_ANALYZE_STR "analyze "
#define SPIDER_SQL_SQL_ANALYZE_LEN (sizeof(SPIDER_SQL_SQL_ANALYZE_STR) - 1)
#define SPIDER_SQL_SQL_OPTIMIZE_STR "optimize "
#define SPIDER_SQL_SQL_OPTIMIZE_LEN (sizeof(SPIDER_SQL_SQL_OPTIMIZE_STR) - 1)
#define SPIDER_SQL_SQL_REPAIR_STR "repair "
#define SPIDER_SQL_SQL_REPAIR_LEN (sizeof(SPIDER_SQL_SQL_REPAIR_STR) - 1)
#define SPIDER_SQL_SQL_TABLE_STR "table "
#define SPIDER_SQL_SQL_TABLE_LEN (sizeof(SPIDER_SQL_SQL_TABLE_STR) - 1)
#define SPIDER_SQL_SQL_QUICK_STR " quick"
#define SPIDER_SQL_SQL_QUICK_LEN (sizeof(SPIDER_SQL_SQL_QUICK_STR) - 1)
#define SPIDER_SQL_SQL_FAST_STR " fast"
#define SPIDER_SQL_SQL_FAST_LEN (sizeof(SPIDER_SQL_SQL_FAST_STR) - 1)
#define SPIDER_SQL_SQL_MEDIUM_STR " medium"
#define SPIDER_SQL_SQL_MEDIUM_LEN (sizeof(SPIDER_SQL_SQL_MEDIUM_STR) - 1)
#define SPIDER_SQL_SQL_EXTENDED_STR " extended"
#define SPIDER_SQL_SQL_EXTENDED_LEN (sizeof(SPIDER_SQL_SQL_EXTENDED_STR) - 1)
#define SPIDER_SQL_SQL_LOCAL_STR "local "
#define SPIDER_SQL_SQL_LOCAL_LEN (sizeof(SPIDER_SQL_SQL_LOCAL_STR) - 1)
#define SPIDER_SQL_SQL_USE_FRM_STR " use_frm"
#define SPIDER_SQL_SQL_USE_FRM_LEN (sizeof(SPIDER_SQL_SQL_USE_FRM_STR) - 1)
#define SPIDER_SQL_TRUNCATE_TABLE_STR "truncate table "
#define SPIDER_SQL_TRUNCATE_TABLE_LEN (sizeof(SPIDER_SQL_TRUNCATE_TABLE_STR) - 1)
#define SPIDER_SQL_EXPLAIN_SELECT_STR "explain select 1 "
#define SPIDER_SQL_EXPLAIN_SELECT_LEN sizeof(SPIDER_SQL_EXPLAIN_SELECT_STR) - 1
#define SPIDER_SQL_FLUSH_LOGS_STR "flush logs"
#define SPIDER_SQL_FLUSH_LOGS_LEN sizeof(SPIDER_SQL_FLUSH_LOGS_STR) - 1
#define SPIDER_SQL_FLUSH_TABLES_STR "flush tables"
#define SPIDER_SQL_FLUSH_TABLES_LEN sizeof(SPIDER_SQL_FLUSH_TABLES_STR) - 1
#define SPIDER_SQL_WITH_READ_LOCK_STR " with read lock"
#define SPIDER_SQL_WITH_READ_LOCK_LEN sizeof(SPIDER_SQL_WITH_READ_LOCK_STR) - 1
#define SPIDER_SQL_DUPLICATE_KEY_UPDATE_STR " on duplicate key update "
#define SPIDER_SQL_DUPLICATE_KEY_UPDATE_LEN (sizeof(SPIDER_SQL_DUPLICATE_KEY_UPDATE_STR) - 1)
#define SPIDER_SQL_HANDLER_STR "handler "
#define SPIDER_SQL_HANDLER_LEN (sizeof(SPIDER_SQL_HANDLER_STR) - 1)
#define SPIDER_SQL_OPEN_STR " open "
#define SPIDER_SQL_OPEN_LEN (sizeof(SPIDER_SQL_OPEN_STR) - 1)
#define SPIDER_SQL_CLOSE_STR " close "
#define SPIDER_SQL_CLOSE_LEN (sizeof(SPIDER_SQL_CLOSE_STR) - 1)
#define SPIDER_SQL_READ_STR " read "
#define SPIDER_SQL_READ_LEN (sizeof(SPIDER_SQL_READ_STR) - 1)
#define SPIDER_SQL_FIRST_STR " first "
#define SPIDER_SQL_FIRST_LEN (sizeof(SPIDER_SQL_FIRST_STR) - 1)
#define SPIDER_SQL_NEXT_STR " next  "
#define SPIDER_SQL_NEXT_LEN (sizeof(SPIDER_SQL_NEXT_STR) - 1)
#define SPIDER_SQL_PREV_STR " prev  "
#define SPIDER_SQL_PREV_LEN (sizeof(SPIDER_SQL_PREV_STR) - 1)
#define SPIDER_SQL_LAST_STR " last  "
#define SPIDER_SQL_LAST_LEN (sizeof(SPIDER_SQL_LAST_STR) - 1)
#define SPIDER_SQL_AS_STR "as "
#define SPIDER_SQL_AS_LEN (sizeof(SPIDER_SQL_AS_STR) - 1)
#define SPIDER_SQL_WITH_QUERY_EXPANSION_STR " with query expansion"
#define SPIDER_SQL_WITH_QUERY_EXPANSION_LEN (sizeof(SPIDER_SQL_WITH_QUERY_EXPANSION_STR) - 1)
#define SPIDER_SQL_IN_BOOLEAN_MODE_STR " in boolean mode"
#define SPIDER_SQL_IN_BOOLEAN_MODE_LEN (sizeof(SPIDER_SQL_IN_BOOLEAN_MODE_STR) - 1)
#define SPIDER_SQL_MATCH_STR "match("
#define SPIDER_SQL_MATCH_LEN (sizeof(SPIDER_SQL_MATCH_STR) - 1)
#define SPIDER_SQL_AGAINST_STR ")against("
#define SPIDER_SQL_AGAINST_LEN (sizeof(SPIDER_SQL_AGAINST_STR) - 1)
#define SPIDER_SQL_IS_NULL_STR " is null"
#define SPIDER_SQL_IS_NULL_LEN (sizeof(SPIDER_SQL_IS_NULL_STR) - 1)
#define SPIDER_SQL_IS_NOT_NULL_STR " is not null"
#define SPIDER_SQL_IS_NOT_NULL_LEN (sizeof(SPIDER_SQL_IS_NOT_NULL_STR) - 1)
#define SPIDER_SQL_NOT_NULL_STR " not null"
#define SPIDER_SQL_NOT_NULL_LEN (sizeof(SPIDER_SQL_NOT_NULL_STR) - 1)
#define SPIDER_SQL_DEFAULT_STR " default "
#define SPIDER_SQL_DEFAULT_LEN (sizeof(SPIDER_SQL_DEFAULT_STR) - 1)
#define SPIDER_SQL_SPACE_STR " "
#define SPIDER_SQL_SPACE_LEN (sizeof(SPIDER_SQL_SPACE_STR) - 1)
#define SPIDER_SQL_ONE_STR "1"
#define SPIDER_SQL_ONE_LEN sizeof(SPIDER_SQL_ONE_STR) - 1
#define SPIDER_SQL_SQL_CACHE_STR "sql_cache "
#define SPIDER_SQL_SQL_CACHE_LEN (sizeof(SPIDER_SQL_SQL_CACHE_STR) - 1)
#define SPIDER_SQL_SQL_NO_CACHE_STR "sql_no_cache "
#define SPIDER_SQL_SQL_NO_CACHE_LEN (sizeof(SPIDER_SQL_SQL_NO_CACHE_STR) - 1)
#define SPIDER_SQL_SQL_QUICK_MODE_STR "quick "
#define SPIDER_SQL_SQL_QUICK_MODE_LEN (sizeof(SPIDER_SQL_SQL_QUICK_MODE_STR) - 1)
#define SPIDER_SQL_SET_STR " set "
#define SPIDER_SQL_SET_LEN (sizeof(SPIDER_SQL_SET_STR) - 1)
#define SPIDER_SQL_UNDERSCORE_STR "_"
#define SPIDER_SQL_UNDERSCORE_LEN sizeof(SPIDER_SQL_UNDERSCORE_STR) - 1
#define SPIDER_SQL_PF_EQUAL_STR " <=> "
#define SPIDER_SQL_PF_EQUAL_LEN (sizeof(SPIDER_SQL_PF_EQUAL_STR) - 1)
#define SPIDER_SQL_GROUP_STR " group by "
#define SPIDER_SQL_GROUP_LEN (sizeof(SPIDER_SQL_GROUP_STR) - 1)
#define SPIDER_SQL_HAVING_STR " having "
#define SPIDER_SQL_HAVING_LEN (sizeof(SPIDER_SQL_HAVING_STR) - 1)
#define SPIDER_SQL_PLUS_STR " + "
#define SPIDER_SQL_PLUS_LEN (sizeof(SPIDER_SQL_PLUS_STR) - 1)
#define SPIDER_SQL_MINUS_STR " - "
#define SPIDER_SQL_MINUS_LEN (sizeof(SPIDER_SQL_MINUS_STR) - 1)

#define SPIDER_SQL_YEAR_STR "year"
#define SPIDER_SQL_YEAR_LEN (sizeof(SPIDER_SQL_YEAR_STR) - 1)
#define SPIDER_SQL_QUARTER_STR "quarter"
#define SPIDER_SQL_QUARTER_LEN (sizeof(SPIDER_SQL_QUARTER_STR) - 1)
#define SPIDER_SQL_MONTH_STR "month"
#define SPIDER_SQL_MONTH_LEN (sizeof(SPIDER_SQL_MONTH_STR) - 1)
#define SPIDER_SQL_WEEK_STR "week"
#define SPIDER_SQL_WEEK_LEN (sizeof(SPIDER_SQL_WEEK_STR) - 1)
#define SPIDER_SQL_DAY_STR "day"
#define SPIDER_SQL_DAY_LEN (sizeof(SPIDER_SQL_DAY_STR) - 1)
#define SPIDER_SQL_HOUR_STR "hour"
#define SPIDER_SQL_HOUR_LEN (sizeof(SPIDER_SQL_HOUR_STR) - 1)
#define SPIDER_SQL_MINUTE_STR "minute"
#define SPIDER_SQL_MINUTE_LEN (sizeof(SPIDER_SQL_MINUTE_STR) - 1)
#define SPIDER_SQL_SECOND_STR "second"
#define SPIDER_SQL_SECOND_LEN (sizeof(SPIDER_SQL_SECOND_STR) - 1)
#define SPIDER_SQL_MICROSECOND_STR "microsecond"
#define SPIDER_SQL_MICROSECOND_LEN (sizeof(SPIDER_SQL_MICROSECOND_STR) - 1)

#define SPIDER_SQL_SHOW_RECORDS_STR "select count(*) from "
#define SPIDER_SQL_SHOW_RECORDS_LEN sizeof(SPIDER_SQL_SHOW_RECORDS_STR) - 1
#define SPIDER_SQL_SHOW_INDEX_STR "show index from "
#define SPIDER_SQL_SHOW_INDEX_LEN sizeof(SPIDER_SQL_SHOW_INDEX_STR) - 1
#define SPIDER_SQL_SELECT_STATISTICS_STR "select `column_name`,`cardinality` from `information_schema`.`statistics` where `table_schema` = "
#define SPIDER_SQL_SELECT_STATISTICS_LEN sizeof(SPIDER_SQL_SELECT_STATISTICS_STR) - 1
#define SPIDER_SQL_MAX_STR "max"
#define SPIDER_SQL_MAX_LEN sizeof(SPIDER_SQL_MAX_STR) - 1

#define SPIDER_SQL_DROP_TMP_STR "drop temporary table if exists "
#define SPIDER_SQL_DROP_TMP_LEN (sizeof(SPIDER_SQL_DROP_TMP_STR) - 1)
#define SPIDER_SQL_CREATE_TMP_STR "create temporary table "
#define SPIDER_SQL_CREATE_TMP_LEN (sizeof(SPIDER_SQL_CREATE_TMP_STR) - 1)
#define SPIDER_SQL_TMP_BKA_STR "tmp_spider_bka_"
#define SPIDER_SQL_TMP_BKA_LEN (sizeof(SPIDER_SQL_TMP_BKA_STR) - 1)
#define SPIDER_SQL_ENGINE_STR ")engine="
#define SPIDER_SQL_ENGINE_LEN (sizeof(SPIDER_SQL_ENGINE_STR) - 1)
#define SPIDER_SQL_DEF_CHARSET_STR " default charset="
#define SPIDER_SQL_DEF_CHARSET_LEN (sizeof(SPIDER_SQL_DEF_CHARSET_STR) - 1)
#define SPIDER_SQL_ID_TYPE_STR " bigint"
#define SPIDER_SQL_ID_TYPE_LEN (sizeof(SPIDER_SQL_ID_TYPE_STR) - 1)

#define SPIDER_SQL_COLUMN_NAME_STR "`column_name`"
#define SPIDER_SQL_COLUMN_NAME_LEN sizeof(SPIDER_SQL_COLUMN_NAME_STR) - 1

#define SPIDER_SQL_A_DOT_STR "a."
#define SPIDER_SQL_A_DOT_LEN (sizeof(SPIDER_SQL_A_DOT_STR) - 1)
#define SPIDER_SQL_B_DOT_STR "b."
#define SPIDER_SQL_B_DOT_LEN (sizeof(SPIDER_SQL_B_DOT_STR) - 1)
#define SPIDER_SQL_A_STR "a"
#define SPIDER_SQL_A_LEN (sizeof(SPIDER_SQL_A_STR) - 1)
#define SPIDER_SQL_B_STR "b"
#define SPIDER_SQL_B_LEN (sizeof(SPIDER_SQL_B_STR) - 1)

#define SPIDER_SQL_INDEX_IGNORE_STR " IGNORE INDEX "
#define SPIDER_SQL_INDEX_IGNORE_LEN (sizeof(SPIDER_SQL_INDEX_IGNORE_STR) - 1)
#define SPIDER_SQL_INDEX_USE_STR " USE INDEX "
#define SPIDER_SQL_INDEX_USE_LEN (sizeof(SPIDER_SQL_INDEX_USE_STR) - 1)
#define SPIDER_SQL_INDEX_FORCE_STR " FORCE INDEX "
#define SPIDER_SQL_INDEX_FORCE_LEN (sizeof(SPIDER_SQL_INDEX_FORCE_STR) - 1)

#define SPIDER_SQL_INT_LEN 20
#define SPIDER_SQL_HANDLER_CID_LEN 6
#define SPIDER_SQL_HANDLER_CID_FORMAT "t%05u"
#define SPIDER_UDF_PING_TABLE_PING_ONLY                (1 << 0)
#define SPIDER_UDF_PING_TABLE_USE_WHERE                (1 << 1)
#define SPIDER_UDF_PING_TABLE_USE_ALL_MONITORING_NODES (1 << 2)

int spider_db_connect(
  const SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int link_idx
);

int spider_db_ping_internal(
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int all_link_idx,
  int *need_mon
);

int spider_db_ping(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);

void spider_db_disconnect(
  SPIDER_CONN *conn
);

int spider_db_conn_queue_action(
  SPIDER_CONN *conn
);

int spider_db_before_query(
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_db_query(
  SPIDER_CONN *conn,
  const char *query,
  uint length,
  int quick_mode,
  int *need_mon
);

int spider_db_errorno(
  SPIDER_CONN *conn
);

int spider_db_set_trx_isolation(
  SPIDER_CONN *conn,
  int trx_isolation,
  int *need_mon
);

int spider_db_set_names_internal(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  SPIDER_CONN *conn,
  int all_link_idx,
  int *need_mon
);

int spider_db_set_names(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);

int spider_db_query_with_set_names(
  ulong sql_type,
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);

int spider_db_query_for_bulk_update(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx,
  ha_rows *dup_key_found
);

size_t spider_db_real_escape_string(
  SPIDER_CONN *conn,
  char *to,
  const char *from,
  size_t from_length
);

int spider_db_consistent_snapshot(
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_db_start_transaction(
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_db_commit(
  SPIDER_CONN *conn
);

int spider_db_rollback(
  SPIDER_CONN *conn
);

int spider_db_append_hex_string(
  spider_string *str,
  uchar *hex_ptr,
  int hex_ptr_length
);

void spider_db_append_xid_str(
  spider_string *tmp_str,
  XID *xid
);

int spider_db_xa_end(
  SPIDER_CONN *conn,
  XID *xid
);

int spider_db_xa_prepare(
  SPIDER_CONN *conn,
  XID *xid
);

int spider_db_xa_commit(
  SPIDER_CONN *conn,
  XID *xid
);

int spider_db_xa_rollback(
  SPIDER_CONN *conn,
  XID *xid
);

int spider_db_lock_tables(
  ha_spider *spider,
  int link_idx
);

int spider_db_unlock_tables(
  ha_spider *spider,
  int link_idx
);

int spider_db_append_name_with_quote_str(
  spider_string *str,
  const char *name,
  uint dbton_id
);

int spider_db_append_name_with_quote_str(
  spider_string *str,
  LEX_CSTRING &name,
  uint dbton_id
);

int spider_db_append_name_with_quote_str_internal(
  spider_string *str,
  const char *name,
  int length,
  uint dbton_id
);

int spider_db_append_name_with_quote_str_internal(
  spider_string *str,
  const char *name,
  int length,
  CHARSET_INFO *cs,
  uint dbton_id
);

int spider_db_append_select(
  ha_spider *spider
);

int spider_db_append_select_columns(
  ha_spider *spider
);

int spider_db_append_null_value(
  spider_string *str,
  KEY_PART_INFO *key_part,
  const uchar **ptr
);

int spider_db_append_key_columns(
  const key_range *start_key,
  ha_spider *spider,
  spider_string *str
);

int spider_db_append_key_hint(
  spider_string *str,
  char *hint_str
);

int spider_db_append_key_where_internal(
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  const key_range *start_key,
  const key_range *end_key,
  ha_spider *spider,
  bool set_order,
  ulong sql_type,
  uint dbton_id
);

int spider_db_append_key_where(
  const key_range *start_key,
  const key_range *end_key,
  ha_spider *spider
);

int spider_db_append_charset_name_before_string(
  spider_string *str,
  CHARSET_INFO *cs
);

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_db_refetch_for_item_sum_funcs(
  ha_spider *spider
);

int spider_db_fetch_for_item_sum_funcs(
  SPIDER_DB_ROW *row,
  ha_spider *spider
);

int spider_db_fetch_for_item_sum_func(
  SPIDER_DB_ROW *row,
  Item_sum *item_sum,
  ha_spider *spider
);
#endif

int spider_db_append_match_fetch(
  ha_spider *spider,
  st_spider_ft_info *ft_first,
  st_spider_ft_info *ft_current,
  SPIDER_DB_ROW *row
);

int spider_db_append_match_where(
  ha_spider *spider
);

int spider_db_append_hint_after_table(
  ha_spider *spider,
  spider_string *str,
  spider_string *hint
);

int spider_db_append_show_records(
  SPIDER_SHARE *share
);

void spider_db_free_show_records(
  SPIDER_SHARE *share
);

int spider_db_append_show_index(
  SPIDER_SHARE *share
);

void spider_db_free_show_index(
  SPIDER_SHARE *share
);

void spider_db_append_handler_next(
  ha_spider *spider
);

void spider_db_get_row_from_tmp_tbl_rec(
  SPIDER_RESULT *current,
  SPIDER_DB_ROW **row
);

int spider_db_get_row_from_tmp_tbl(
  SPIDER_RESULT *current,
  SPIDER_DB_ROW **row
);

int spider_db_get_row_from_tmp_tbl_pos(
  SPIDER_POSITION *pos,
  SPIDER_DB_ROW **row
);

int spider_db_fetch_row(
  SPIDER_SHARE *share,
  Field *field,
  SPIDER_DB_ROW *row,
  my_ptrdiff_t ptr_diff
);

int spider_db_fetch_table(
  ha_spider *spider,
  uchar *buf,
  TABLE *table,
  SPIDER_RESULT_LIST *result_list
);

int spider_db_fetch_key(
  ha_spider *spider,
  uchar *buf,
  TABLE *table,
  const KEY *key_info,
  SPIDER_RESULT_LIST *result_list
);

int spider_db_fetch_minimum_columns(
  ha_spider *spider,
  uchar *buf,
  TABLE *table,
  SPIDER_RESULT_LIST *result_list
);

void spider_db_free_one_result_for_start_next(
  ha_spider *spider
);

void spider_db_free_one_result(
  SPIDER_RESULT_LIST *result_list,
  SPIDER_RESULT *result
);

void spider_db_free_one_quick_result(
  SPIDER_RESULT *result
);

int spider_db_free_result(
  ha_spider *spider,
  bool final
);

int spider_db_store_result(
  ha_spider *spider,
  int link_idx,
  TABLE *table
);

void spider_db_discard_result(
  ha_spider *spider,
  int link_idx,
  SPIDER_CONN *conn
);

void spider_db_discard_multiple_result(
  ha_spider *spider,
  int link_idx,
  SPIDER_CONN *conn
);

#ifdef HA_CAN_BULK_ACCESS
int spider_db_bulk_store_result(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx,
  bool discard_result
);
#endif

int spider_db_fetch(
  uchar *buf,
  ha_spider *spider,
  TABLE *table
);

int spider_db_seek_prev(
  uchar *buf,
  ha_spider *spider,
  TABLE *table
);

int spider_db_seek_next(
  uchar *buf,
  ha_spider *spider,
  int link_idx,
  TABLE *table
);

int spider_db_seek_last(
  uchar *buf,
  ha_spider *spider,
  int link_idx,
  TABLE *table
);

int spider_db_seek_first(
  uchar *buf,
  ha_spider *spider,
  TABLE *table
);

void spider_db_set_pos_to_first_row(
  SPIDER_RESULT_LIST *result_list
);

void spider_db_create_position(
  ha_spider *spider,
  SPIDER_POSITION *pos
);

int spider_db_seek_tmp(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table
);

int spider_db_seek_tmp_table(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table
);

int spider_db_seek_tmp_key(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table,
  const KEY *key_info
);

int spider_db_seek_tmp_minimum_columns(
  uchar *buf,
  SPIDER_POSITION *pos,
  ha_spider *spider,
  TABLE *table
);

int spider_db_show_table_status(
  ha_spider *spider,
  int link_idx,
  int sts_mode,
  uint flag
);

int spider_db_simple_action(
  uint simple_action,
  spider_db_handler *db_handler,
  int link_idx
);

int spider_db_simple_action(
  uint simple_action,
  ha_spider *spider,
  int link_idx,
  bool pre_call
);

void spider_db_set_cardinarity(
  ha_spider *spider,
  TABLE *table
);

int spider_db_show_index(
  ha_spider *spider,
  int link_idx,
  TABLE *table,
  int crd_mode
);

ha_rows spider_db_explain_select(
  const key_range *start_key,
  const key_range *end_key,
  ha_spider *spider,
  int link_idx
);

int spider_db_bulk_insert_init(
  ha_spider *spider,
  const TABLE *table
);

int spider_db_bulk_insert(
  ha_spider *spider,
  TABLE *table,
  ha_copy_info *copy_info,
  bool bulk_end
);

#ifdef HA_CAN_BULK_ACCESS
int spider_db_bulk_bulk_insert(
  ha_spider *spider
);
#endif

int spider_db_update_auto_increment(
  ha_spider *spider,
  int link_idx
);

int spider_db_bulk_update_size_limit(
  ha_spider *spider,
  TABLE *table
);

int spider_db_bulk_update_end(
  ha_spider *spider,
  ha_rows *dup_key_found
);

int spider_db_bulk_update(
  ha_spider *spider,
  TABLE *table,
  my_ptrdiff_t ptr_diff
);

int spider_db_update(
  ha_spider *spider,
  TABLE *table,
  const uchar *old_data
);

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int spider_db_direct_update(
  ha_spider *spider,
  TABLE *table,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  ha_rows *update_rows,
  ha_rows *found_rows
);
#else
int spider_db_direct_update(
  ha_spider *spider,
  TABLE *table,
  ha_rows *update_rows,
  ha_rows *found_rows
);
#endif
#endif

#ifdef HA_CAN_BULK_ACCESS
int spider_db_bulk_direct_update(
  ha_spider *spider,
  ha_rows *update_rows
);
#endif

int spider_db_bulk_delete(
  ha_spider *spider,
  TABLE *table,
  my_ptrdiff_t ptr_diff
);

int spider_db_delete(
  ha_spider *spider,
  TABLE *table,
  const uchar *buf
);

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
int spider_db_direct_delete(
  ha_spider *spider,
  TABLE *table,
  KEY_MULTI_RANGE *ranges,
  uint range_count,
  ha_rows *delete_rows
);
#else
int spider_db_direct_delete(
  ha_spider *spider,
  TABLE *table,
  ha_rows *delete_rows
);
#endif
#endif

int spider_db_delete_all_rows(
  ha_spider *spider
);

int spider_db_disable_keys(
  ha_spider *spider
);

int spider_db_enable_keys(
  ha_spider *spider
);

int spider_db_check_table(
  ha_spider *spider,
  HA_CHECK_OPT* check_opt
);

int spider_db_repair_table(
  ha_spider *spider,
  HA_CHECK_OPT* check_opt
);

int spider_db_analyze_table(
  ha_spider *spider
);

int spider_db_optimize_table(
  ha_spider *spider
);

int spider_db_flush_tables(
  ha_spider *spider,
  bool lock
);

int spider_db_flush_logs(
  ha_spider *spider
);

Field *spider_db_find_field_in_item_list(
  Item **item_list,
  uint item_count,
  uint start_item,
  spider_string *str,
  const char *func_name,
  int func_name_length
);

int spider_db_print_item_type(
  Item *item,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_print_item_type_default(
  Item *item,
  ha_spider *spider,
  spider_string *str
);

int spider_db_open_item_cond(
  Item_cond *item_cond,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_func(
  Item_func *item_func,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_db_open_item_sum_func(
  Item_sum *item_sum,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);
#endif

int spider_db_open_item_ident(
  Item_ident *item_ident,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_field(
  Item_field *item_field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_ref(
  Item_ref *item_ref,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_row(
  Item_row *item_row,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_string(
  Item *item,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_int(
  Item *item,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_cache(
  Item_cache *item_cache,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_open_item_insert_value(
  Item_insert_value *item_insert_value,
  Field *field,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);

int spider_db_append_condition(
  ha_spider *spider,
  const char *alias,
  uint alias_length,
  bool test_flg
);

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_db_append_update_columns(
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  uint dbton_id,
  bool use_fields,
  spider_fields *fields
);
#endif

uint spider_db_check_ft_idx(
  Item_func *item_func,
  ha_spider *spider
);

int spider_db_udf_fetch_row(
  SPIDER_TRX *trx,
  Field *field,
  SPIDER_DB_ROW *row,
  ulong *length
);

int spider_db_udf_fetch_table(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  TABLE *table,
  SPIDER_DB_RESULT *result,
  uint set_on,
  uint set_off
);

int spider_db_udf_direct_sql_connect(
  const SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_CONN *conn
);

int spider_db_udf_direct_sql_ping(
  SPIDER_DIRECT_SQL *direct_sql
);

int spider_db_udf_direct_sql(
  SPIDER_DIRECT_SQL *direct_sql
);

int spider_db_udf_direct_sql_select_db(
  SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_CONN *conn
);

int spider_db_udf_direct_sql_set_names(
  SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_TRX *trx,
  SPIDER_CONN *conn
);

int spider_db_udf_check_and_set_set_names(
  SPIDER_TRX *trx
);

int spider_db_udf_append_set_names(
  SPIDER_TRX *trx
);

void spider_db_udf_free_set_names(
  SPIDER_TRX *trx
);

int spider_db_udf_ping_table(
  SPIDER_TABLE_MON_LIST *table_mon_list,
  SPIDER_SHARE *share,
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  char *where_clause,
  uint where_clause_length,
  bool ping_only,
  bool use_where,
  longlong limit
);

int spider_db_udf_ping_table_append_mon_next(
  spider_string *str,
  char *child_table_name,
  uint child_table_name_length,
  int link_id,
  char *where_clause,
  uint where_clause_length,
  longlong first_sid,
  int full_mon_count,
  int current_mon_count,
  int success_count,
  int fault_count,
  int flags,
  longlong limit
);

int spider_db_udf_ping_table_append_select(
  spider_string *str,
  SPIDER_SHARE *share,
  SPIDER_TRX *trx,
  spider_string *where_str,
  bool use_where,
  longlong limit,
  uint dbton_id
);

int spider_db_udf_ping_table_mon_next(
  THD *thd,
  SPIDER_TABLE_MON *table_mon,
  SPIDER_CONN *conn,
  SPIDER_MON_TABLE_RESULT *mon_table_result,
  char *child_table_name,
  uint child_table_name_length,
  int link_id,
  char *where_clause,
  uint where_clause_length,
  longlong first_sid,
  int full_mon_count,
  int current_mon_count,
  int success_count,
  int fault_count,
  int flags,
  longlong limit
);

int spider_db_udf_copy_tables(
  SPIDER_COPY_TABLES *copy_tables,
  ha_spider *spider,
  TABLE *table,
  longlong bulk_insert_rows
);

int spider_db_open_handler(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);

#ifdef HA_CAN_BULK_ACCESS
int spider_db_bulk_open_handler(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);
#endif

int spider_db_close_handler(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx,
  uint tgt_conn_kind
);

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
void spider_db_hs_request_buf_reset(
  SPIDER_CONN *conn
);
#endif

bool spider_db_conn_is_network_error(
  int error_num
);
