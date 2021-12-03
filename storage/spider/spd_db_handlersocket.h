/* Copyright (C) 2012-2018 Kentoku Shiba

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

#define SPIDER_HS_CONN dena::hstcpcli_ptr
#define SPIDER_HS_CONN_CREATE dena::hstcpcli_i::create
#define SPIDER_HS_RESULT dena::hstresult
#define SPIDER_HS_SOCKARGS dena::socket_args

class spider_db_handlersocket_util: public spider_db_util
{
public:
  spider_db_handlersocket_util();
  ~spider_db_handlersocket_util();
  int append_name(
    spider_string *str,
    const char *name,
    uint name_length
  );
  int append_name_with_charset(
    spider_string *str,
    const char *name,
    uint name_length,
    CHARSET_INFO *name_charset
  );
  int append_escaped_name(
    spider_string *str,
    const char *name,
    uint name_length
  );
  int append_escaped_name_with_charset(
    spider_string *str,
    const char *name,
    uint name_length,
    CHARSET_INFO *name_charset
  );
  bool is_name_quote(
    const char head_code
  );
  int append_escaped_name_quote(
    spider_string *str
  );
  int append_column_value(
    ha_spider *spider,
    spider_string *str,
    Field *field,
    const uchar *new_ptr,
    CHARSET_INFO *access_charset
  );
  int append_trx_isolation(
    spider_string *str,
    int trx_isolation
  );
  int append_autocommit(
    spider_string *str,
    bool autocommit
  );
  int append_sql_log_off(
    spider_string *str,
    bool sql_log_off
  );
  int append_wait_timeout(
    spider_string *str,
    int wait_timeout
  );
  int append_sql_mode(
    spider_string *str,
    sql_mode_t sql_mode
  );
  int append_time_zone(
    spider_string *str,
    Time_zone *time_zone
  );
  int append_start_transaction(
    spider_string *str
  );
  int append_xa_start(
    spider_string *str,
    XID *xid
  );
  int append_lock_table_head(
    spider_string *str
  );
  int append_lock_table_body(
    spider_string *str,
    const char *db_name,
    uint db_name_length,
    CHARSET_INFO *db_name_charset,
    const char *table_name,
    uint table_name_length,
    CHARSET_INFO *table_name_charset,
    int lock_type
  );
  int append_lock_table_tail(
    spider_string *str
  );
  int append_unlock_table(
    spider_string *str
  );
  int open_item_func(
    Item_func *item_func,
    ha_spider *spider,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  );
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  int open_item_sum_func(
    Item_sum *item_sum,
    ha_spider *spider,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  );
#endif
  int append_escaped_util(
    spider_string *to,
    String *from
  );
  int append_escaped_util(
    spider_string *to,
    String *from
  );
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  int append_from_and_tables(
    ha_spider *spider,
    spider_fields *fields,
    spider_string *str,
    TABLE_LIST *table_list,
    uint table_count
  );
  int reappend_tables(
    spider_fields *fields,
    SPIDER_LINK_IDX_CHAIN *link_idx_chain,
    spider_string *str
  );
  int append_where(
    spider_string *str
  );
  int append_having(
    spider_string *str
  );
#endif
};

class spider_db_handlersocket_row: public spider_db_row
{
public:
  SPIDER_HS_STRING_REF *hs_row;
  SPIDER_HS_STRING_REF *hs_row_first;
  uint                 field_count;
  uint                 row_size;
  bool                 cloned;
  spider_db_handlersocket_row();
  ~spider_db_handlersocket_row();
  int store_to_field(
    Field *field,
    CHARSET_INFO *access_charset
  );
  int append_to_str(
    spider_string *str
  );
  int append_escaped_to_str(
    spider_string *str,
    uint dbton_id
  );
  void first();
  void next();
  bool is_null();
  int val_int();
  double val_real();
  my_decimal *val_decimal(
    my_decimal *decimal_value,
    CHARSET_INFO *access_charset
  );
  SPIDER_DB_ROW *clone();
  int store_to_tmp_table(
    TABLE *tmp_table,
    spider_string *str
  );
  uint get_byte_size();
};

class spider_db_handlersocket_result_buffer: public spider_db_result_buffer
{
public:
  SPIDER_HS_RESULT            hs_result;
  spider_db_handlersocket_result_buffer();
  ~spider_db_handlersocket_result_buffer();
  void clear();
  bool check_size(
    longlong size
  );
};

class spider_db_handlersocket_result: public spider_db_result
{
public:
  SPIDER_HS_CONN              *hs_conn_p;
  spider_db_handlersocket_row row;
  SPIDER_HS_STRING_REF        hs_row;
  uint                        field_count;
  int                         store_error_num;
  spider_db_handlersocket_result(SPIDER_DB_CONN *in_db_conn);
  ~spider_db_handlersocket_result();
  bool has_result();
  void free_result();
  SPIDER_DB_ROW *current_row();
  SPIDER_DB_ROW *fetch_row();
  SPIDER_DB_ROW *fetch_row_from_result_buffer(
    spider_db_result_buffer *spider_res_buf
  );
  SPIDER_DB_ROW *fetch_row_from_tmp_table(
    TABLE *tmp_table
  );
  int fetch_table_status(
    int mode,
    ha_statistics &stat
  );
  int fetch_table_records(
    int mode,
    ha_rows &records
  );
  int fetch_table_cardinality(
    int mode,
    TABLE *table,
    longlong *cardinality,
    uchar *cardinality_upd,
    int bitmap_size
  );
  int fetch_table_mon_status(
    int &status
  );
  longlong num_rows();
  uint num_fields();
  void move_to_pos(
    longlong pos
  );
  int get_errno();
#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
  int fetch_columns_for_discover_table_structure(
    spider_string *str,
    CHARSET_INFO *access_charset
  );
  int fetch_index_for_discover_table_structure(
    spider_string *str,
    CHARSET_INFO *access_charset
  );
  int fetch_table_for_discover_table_structure(
    spider_string *str,
    SPIDER_SHARE *spider_share,
    CHARSET_INFO *access_charset
  );
#endif
};

class spider_db_handlersocket: public spider_db_conn
{
  SPIDER_HS_CONN hs_conn;
  int            stored_error;
  uint           field_count;
public:
  DYNAMIC_ARRAY  handler_open_array;
  bool           handler_open_array_inited;
  uint           handler_open_array_id;
  const char     *handler_open_array_func_name;
  const char     *handler_open_array_file_name;
  ulong          handler_open_array_line_no;
  st_spider_db_request_key *request_key_req_first;
  st_spider_db_request_key *request_key_req_last;
  st_spider_db_request_key *request_key_snd_first;
  st_spider_db_request_key *request_key_snd_last;
  st_spider_db_request_key *request_key_reuse_first;
  st_spider_db_request_key *request_key_reuse_last;
  spider_db_handlersocket(
    SPIDER_CONN *conn
  );
  ~spider_db_handlersocket();
  int init();
  bool is_connected();
  void bg_connect();
  int connect(
    char *tgt_host,
    char *tgt_username,
    char *tgt_password,
    long tgt_port,
    char *tgt_socket,
    char *server_name,
    int connect_retry_count,
    longlong connect_retry_interval
  );
  int ping();
  void bg_disconnect();
  void disconnect();
  int set_net_timeout();
  int exec_query(
    const char *query,
    uint length,
    int quick_mode
  );
  int get_errno();
  const char *get_error();
  bool is_server_gone_error(
    int error_num
  );
  bool is_dup_entry_error(
    int error_num
  );
  bool is_xa_nota_error(
    int error_num
  );
  spider_db_result *store_result(
    spider_db_result_buffer **spider_res_buf,
    st_spider_db_request_key *request_key,
    int *error_num
  );
  spider_db_result *use_result(
    st_spider_db_request_key *request_key,
    int *error_num
  );
  int next_result();
  uint affected_rows();
  uint matched_rows();
  bool inserted_info(
    spider_db_handler *handler,
    spider_copy_info *copy_info
  );
  ulonglong last_insert_id();
  int set_character_set(
    const char *csname
  );
  int select_db(
    const char *dbname
  );
  int consistent_snapshot(
    int *need_mon
  );
  bool trx_start_in_bulk_sql();
  int start_transaction(
    int *need_mon
  );
  int commit(
    int *need_mon
  );
  int rollback(
    int *need_mon
  );
  bool xa_start_in_bulk_sql();
  int xa_start(
    XID *xid,
    int *need_mon
  );
  int xa_end(
    XID *xid,
    int *need_mon
  );
  int xa_prepare(
    XID *xid,
    int *need_mon
  );
  int xa_commit(
    XID *xid,
    int *need_mon
  );
  int xa_rollback(
    XID *xid,
    int *need_mon
  );
  bool set_trx_isolation_in_bulk_sql();
  int set_trx_isolation(
    int trx_isolation,
    int *need_mon
  );
  bool set_autocommit_in_bulk_sql();
  int set_autocommit(
    bool autocommit,
    int *need_mon
  );
  bool set_sql_log_off_in_bulk_sql();
  int set_sql_log_off(
    bool sql_log_off,
    int *need_mon
  );
  bool set_wait_timeout_in_bulk_sql();
  int set_wait_timeout(
    int wait_timeout,
    int *need_mon
  );
  bool set_sql_mode_in_bulk_sql();
  int set_sql_mode(
    sql_mode_t sql_mode,
    int *need_mon
  );
  bool set_time_zone_in_bulk_sql();
  int set_time_zone(
    Time_zone *time_zone,
    int *need_mon
  );
  int show_master_status(
    SPIDER_TRX *trx,
    SPIDER_SHARE *share,
    int all_link_idx,
    int *need_mon,
    TABLE *table,
    spider_string *str,
    int mode,
    SPIDER_DB_RESULT **res1,
    SPIDER_DB_RESULT **res2
  );
  int append_sql(
    char *sql,
    ulong sql_length,
    st_spider_db_request_key *request_key
  );
  int append_open_handler(
    uint handler_id,
    const char *db_name,
    const char *table_name,
    const char *index_name,
    const char *sql,
    st_spider_db_request_key *request_key
  );
  int append_select(
    uint handler_id,
    spider_string *sql,
    SPIDER_DB_HS_STRING_REF_BUFFER *keys,
    int limit,
    int skip,
    st_spider_db_request_key *request_key
  );
  int append_insert(
    uint handler_id,
    SPIDER_DB_HS_STRING_REF_BUFFER *upds,
    st_spider_db_request_key *request_key
  );
  int append_update(
    uint handler_id,
    spider_string *sql,
    SPIDER_DB_HS_STRING_REF_BUFFER *keys,
    SPIDER_DB_HS_STRING_REF_BUFFER *upds,
    int limit,
    int skip,
    bool increment,
    bool decrement,
    st_spider_db_request_key *request_key
  );
  int append_delete(
    uint handler_id,
    spider_string *sql,
    SPIDER_DB_HS_STRING_REF_BUFFER *keys,
    int limit,
    int skip,
    st_spider_db_request_key *request_key
  );
  void reset_request_queue();
  size_t escape_string(
    char *to,
    const char *from,
    size_t from_length
  );
  bool have_lock_table_list();
  int append_lock_tables(
    spider_string *str
  );
  int append_unlock_tables(
    spider_string *str
  );
  uint get_lock_table_hash_count();
  void reset_lock_table_hash();
  uint get_opened_handler_count();
  void reset_opened_handler();
  void set_dup_key_idx(
    ha_spider *spider,
    int link_idx
  );
  int append_request_key(
    st_spider_db_request_key *request_key
  );
  void reset_request_key_req();
  void reset_request_key_snd();
  void move_request_key_to_snd();
  int check_request_key(
    st_spider_db_request_key *request_key
  );
  bool cmp_request_key_to_snd(
    st_spider_db_request_key *request_key
  );
};

class spider_handlersocket_share: public spider_db_share
{
public:
  spider_string      *table_names_str;
  spider_string      *db_names_str;
  spider_string      *db_table_str;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type *db_table_str_hash_value;
#endif
  uint               table_nm_max_length;
  uint               db_nm_max_length;
  spider_string      *column_name_str;
  bool               same_db_table_name;
  int                first_all_link_idx;
  spider_handlersocket_share(
    st_spider_share *share
  );
  ~spider_handlersocket_share();
  int init();
  int append_table_name(
    spider_string *str,
    int all_link_idx
  );
  int create_table_names_str();
  void free_table_names_str();
  int create_column_name_str();
  void free_column_name_str();
  uint get_column_name_length(
    uint field_index
  );
  int append_column_name(
    spider_string *str,
    uint field_index
  );
  int append_column_name_with_alias(
    spider_string *str,
    uint field_index,
    const char *alias,
    uint alias_length
  );
  bool need_change_db_table_name();
#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
  int discover_table_structure(
    SPIDER_TRX *trx,
    SPIDER_SHARE *spider_share,
    spider_string *str
  );
#endif
};

class spider_handlersocket_handler: public spider_db_handler
{
  spider_string           hs_sql;
public:
  bool                    hs_adding_keys;
  SPIDER_DB_HS_STRING_REF_BUFFER hs_keys;
  SPIDER_DB_HS_STRING_REF_BUFFER hs_upds;
  SPIDER_DB_HS_STR_BUFFER hs_strs;
  uint                    hs_strs_pos;
  int                     hs_limit;
  int                     hs_skip;
  spider_handlersocket_share *handlersocket_share;
  SPIDER_LINK_FOR_HASH    *link_for_hash;
  uchar                   *minimum_select_bitmap;
  spider_handlersocket_handler(
    ha_spider *spider,
    spider_handlersocket_share *db_share
  );
  ~spider_handlersocket_handler();
  int init();
  int append_index_hint(
    spider_string *str,
    int link_idx,
    ulong sql_type
  );
  int append_table_name_with_adjusting(
    spider_string *str,
    int link_idx,
    ulong sql_type
  );
  int append_tmp_table_and_sql_for_bka(
    const key_range *start_key
  );
  int reuse_tmp_table_and_sql_for_bka();
  int append_union_table_and_sql_for_bka(
    const key_range *start_key
  );
  int reuse_union_table_and_sql_for_bka();
  int append_insert_for_recovery(
    ulong sql_type,
    int link_idx
  );
  int append_update(
    const TABLE *table,
    my_ptrdiff_t ptr_diff
  );
  int append_update(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    int link_idx
  );
  int append_delete(
    const TABLE *table,
    my_ptrdiff_t ptr_diff
  );
  int append_delete(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    int link_idx
  );
  int append_insert_part();
  int append_update_part();
  int append_delete_part();
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  int append_increment_update_set_part();
#endif
  int append_update_set_part();
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  int append_direct_update_set_part();
#endif
  int append_minimum_select_without_quote(
    spider_string *str
  );
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  int append_minimum_select_by_field_idx_list(
    spider_string *str,
    uint32 *field_idxs,
    size_t field_idxs_num
  );
  int append_dup_update_pushdown_part(
    const char *alias,
    uint alias_length
  );
  int append_update_columns_part(
    const char *alias,
    uint alias_length
  );
  int check_update_columns_part();
  int append_select_part(
    ulong sql_type
  );
#endif
  int append_table_select_part(
    ulong sql_type
  );
  int append_key_select_part(
    ulong sql_type,
    uint idx
  );
  int append_minimum_select_part(
    ulong sql_type
  );
  int append_hint_after_table_part(
    ulong sql_type
  );
  void set_where_pos(
    ulong sql_type
  );
  void set_where_to_pos(
    ulong sql_type
  );
  int check_item_type(
    Item *item
  );
  int append_values_connector_part(
    ulong sql_type
  );
  int append_values_terminator_part(
    ulong sql_type
  );
  int append_union_table_connector_part(
    ulong sql_type
  );
  int append_union_table_terminator_part(
    ulong sql_type
  );
  int append_key_column_values_part(
    const key_range *start_key,
    ulong sql_type
  );
  int append_key_column_values_with_name_part(
    const key_range *start_key,
    ulong sql_type
  );
  int append_key_where_part(
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type
  );
  int append_key_where(
    spider_string *str,
    spider_string *str_part,
    spider_string *str_part2,
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type,
    bool set_order
  );
  int append_is_null_part(
    ulong sql_type,
    KEY_PART_INFO *key_part,
    const key_range *key,
    const uchar **ptr,
    bool key_eq,
    bool tgt_final
  );
  int append_is_null(
    ulong sql_type,
    spider_string *str,
    spider_string *str_part,
    spider_string *str_part2,
    KEY_PART_INFO *key_part,
    const key_range *key,
    const uchar **ptr,
    bool key_eq,
    bool tgt_final
  );
  int append_where_terminator_part(
    ulong sql_type,
    bool set_order,
    int key_count
  );
  int append_match_where_part(
    ulong sql_type
  );
  int append_condition_part(
    const char *alias,
    uint alias_length,
    ulong sql_type,
    bool test_flg
  );
  int append_match_select_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  );
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  int append_sum_select_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  );
#endif
  void set_order_pos(
    ulong sql_type
  );
  void set_order_to_pos(
    ulong sql_type
  );
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  int append_group_by_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
#endif
  int append_key_order_for_merge_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
  int append_key_order_for_direct_order_limit_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
  int append_key_order_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
  int append_limit_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  );
  int reappend_limit_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  );
  int append_select_lock_part(
    ulong sql_type
  );
  int append_union_all_start_part(
    ulong sql_type
  );
  int append_union_all_part(
    ulong sql_type
  );
  int append_union_all_end_part(
    ulong sql_type
  );
  int append_multi_range_cnt_part(
    ulong sql_type,
    uint multi_range_cnt,
    bool with_comma
  );
  int append_multi_range_cnt_with_name_part(
    ulong sql_type,
    uint multi_range_cnt
  );
  int append_open_handler_part(
    ulong sql_type,
    uint handler_id,
    SPIDER_CONN *conn,
    int link_idx
  );
  int append_open_handler(
    spider_string *str,
    uint handler_id,
    SPIDER_CONN *conn,
    int link_idx
  );
  int append_close_handler_part(
    ulong sql_type,
    int link_idx
  );
  int append_insert_terminator_part(
    ulong sql_type
  );
  int append_insert_values_part(
    ulong sql_type
  );
  int append_insert_values(
    spider_string *str
  );
  int append_into_part(
    ulong sql_type
  );
  void set_insert_to_pos(
    ulong sql_type
  );
  int append_from_part(
    ulong sql_type,
    int link_idx
  );
  int append_delete_all_rows_part(
    ulong sql_type
  );
  int append_explain_select_part(
    key_range *start_key,
    key_range *end_key,
    ulong sql_type,
    int link_idx
  );
  bool is_sole_projection_field(
    uint16 field_index
  );
  bool is_bulk_insert_exec_period(
    bool bulk_end
  );
  bool sql_is_filled_up(
    ulong sql_type
  );
  bool sql_is_empty(
    ulong sql_type
  );
  bool support_multi_split_read();
  bool support_bulk_update();
  int bulk_tmp_table_insert();
  int bulk_tmp_table_insert(
    int link_idx
  );
  int bulk_tmp_table_end_bulk_insert();
  int bulk_tmp_table_rnd_init();
  int bulk_tmp_table_rnd_next();
  int bulk_tmp_table_rnd_end();
  bool need_copy_for_update(
    int link_idx
  );
  bool bulk_tmp_table_created();
  int mk_bulk_tmp_table_and_bulk_start();
  void rm_bulk_tmp_table();
  int insert_lock_tables_list(
    SPIDER_CONN *conn,
    int link_idx
  );
  int append_lock_tables_list(
    SPIDER_CONN *conn,
    int link_idx,
    int *appended
  );
  int realloc_sql(
    ulong *realloced
  );
  int reset_sql(
    ulong sql_type
  );
  int reset_keys(
    ulong sql_type
  );
  int reset_upds(
    ulong sql_type
  );
  int reset_strs(
    ulong sql_type
  );
  int reset_strs_pos(
    ulong sql_type
  );
  int push_back_upds(
    SPIDER_HS_STRING_REF &info
  );
  int request_buf_find(
    int link_idx
  );
  int request_buf_insert(
    int link_idx
  );
  int request_buf_update(
    int link_idx
  );
  int request_buf_delete(
    int link_idx
  );
  bool need_lock_before_set_sql_for_exec(
    ulong sql_type
  );
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  int set_sql_for_exec(
    ulong sql_type,
    int link_idx,
    SPIDER_LINK_IDX_CHAIN *link_idx_chain
  );
#endif
  int set_sql_for_exec(
    ulong sql_type,
    int link_idx
  );
  int set_sql_for_exec(
    spider_db_copy_table *tgt_ct,
    ulong sql_type
  );
  int execute_sql(
    ulong sql_type,
    SPIDER_CONN *conn,
    int quick_mode,
    int *need_mon
  );
  int reset();
  int sts_mode_exchange(
    int sts_mode
  );
  int show_table_status(
    int link_idx,
    int sts_mode,
    uint flag
  );
  int crd_mode_exchange(
    int crd_mode
  );
  int show_index(
    int link_idx,
    int crd_mode
  );
  int show_records(
    int link_idx
  );
  int show_last_insert_id(
    int link_idx,
    ulonglong &last_insert_id
  );
  ha_rows explain_select(
    key_range *start_key,
    key_range *end_key,
    int link_idx
  );
  int lock_tables(
    int link_idx
  );
  int unlock_tables(
    int link_idx
  );
  int disable_keys(
    SPIDER_CONN *conn,
    int link_idx
  );
  int enable_keys(
    SPIDER_CONN *conn,
    int link_idx
  );
  int check_table(
    SPIDER_CONN *conn,
    int link_idx,
    HA_CHECK_OPT* check_opt
  );
  int repair_table(
    SPIDER_CONN *conn,
    int link_idx,
    HA_CHECK_OPT* check_opt
  );
  int analyze_table(
    SPIDER_CONN *conn,
    int link_idx
  );
  int optimize_table(
    SPIDER_CONN *conn,
    int link_idx
  );
  int flush_tables(
    SPIDER_CONN *conn,
    int link_idx,
    bool lock
  );
  int flush_logs(
    SPIDER_CONN *conn,
    int link_idx
  );
  int insert_opened_handler(
    SPIDER_CONN *conn,
    int link_idx
  );
  int delete_opened_handler(
    SPIDER_CONN *conn,
    int link_idx
  );
  int sync_from_clone_source(
    spider_db_handler *dbton_hdl
  );
  bool support_use_handler(
    int use_handler
  );
  void minimum_select_bitmap_create();
  bool minimum_select_bit_is_set(
    uint field_index
  );
  void copy_minimum_select_bitmap(
    uchar *bitmap
  );
  int init_union_table_name_pos();
  int set_union_table_name_pos();
  int reset_union_table_name(
    spider_string *str,
    int link_idx,
    ulong sql_type
  );
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  int append_from_and_tables_part(
    spider_fields *fields,
    ulong sql_type
  );
  int reappend_tables_part(
    spider_fields *fields,
    ulong sql_type
  );
  int append_where_part(
    ulong sql_type
  );
  int append_having_part(
    ulong sql_type
  );
  int append_item_type_part(
    Item *item,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  );
  int append_list_item_select_part(
    List<Item> *select,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  );
  int append_group_by_part(
    ORDER *order,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  );
  int append_order_by_part(
    ORDER *order,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  );
#endif
};
