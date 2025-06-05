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

class spider_db_mbase_util: public spider_db_util
{
public:
  spider_db_mbase_util();
  virtual ~spider_db_mbase_util();
  int append_name(
    spider_string *str,
    const char *name,
    uint name_length
  ) override;
  int append_name_with_charset(
    spider_string *str,
    const char *name,
    uint name_length,
    CHARSET_INFO *name_charset
  ) override;
  int append_escaped_name(
    spider_string *str,
    const char *name,
    uint name_length
  ) override;
  int append_escaped_name_with_charset(
    spider_string *str,
    const char *name,
    uint name_length,
    CHARSET_INFO *name_charset
  ) override;
  bool is_name_quote(
    const char head_code
  ) override;
  int append_escaped_name_quote(
    spider_string *str
  ) override;
  int append_column_value(ha_spider *spider, spider_string *str, Field *field,
                          const uchar *new_ptr, bool is_like,
                          CHARSET_INFO *access_charset) override;
  int append_from_with_alias(
    spider_string *str,
    const char **table_names,
    uint *table_name_lengths,
    const char **table_aliases,
    uint *table_alias_lengths,
    uint table_count,
    int *table_name_pos,
    bool over_write
  );
  int append_trx_isolation(
    spider_string *str,
    int trx_isolation
  ) override;
  int append_autocommit(
    spider_string *str,
    bool autocommit
  ) override;
  int append_sql_log_off(
    spider_string *str,
    bool sql_log_off
  ) override;
  int append_wait_timeout(
    spider_string *str,
    int wait_timeout
  ) override;
  virtual int append_sql_mode_internal(
    spider_string *str,
    sql_mode_t sql_mode
  );
  int append_sql_mode(
    spider_string *str,
    sql_mode_t sql_mode
  ) override;
  int append_time_zone(
    spider_string *str,
    Time_zone *time_zone
  ) override;
  int append_loop_check(
    spider_string *str,
    SPIDER_CONN *conn
  ) override;
  int append_start_transaction(
    spider_string *str
  ) override;
  int append_xa_start(
    spider_string *str,
    XID *xid
  ) override;
  int append_lock_table_head(
    spider_string *str
  ) override;
  int append_lock_table_body(
    spider_string *str,
    const char *db_name,
    uint db_name_length,
    CHARSET_INFO *db_name_charset,
    const char *table_name,
    uint table_name_length,
    CHARSET_INFO *table_name_charset,
    int lock_type
  ) override;
  int append_lock_table_tail(
    spider_string *str
  ) override;
  int append_unlock_table(
    spider_string *str
  ) override;
  int open_item_func(
    Item_func *item_func,
    ha_spider *spider,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  ) override;
protected:
  int check_item_func(
    Item_func *item_func,
    ha_spider *spider,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  );
  int print_item_func(
    Item_func *item_func,
    ha_spider *spider,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  );
public:
  int open_item_sum_func(
    Item_sum *item_sum,
    ha_spider *spider,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  ) override;
  int append_escaped_util(
    spider_string *to,
    String *from
  ) override;
  int append_tables_top_down_check(
    TABLE_LIST *table_list,
    TABLE_LIST **used_table_list,
    uint *current_pos
  );
  int append_table_list(spider_fields *fields,
                        spider_string *str, TABLE_LIST *table,
                        table_map *upper_usable_tables,
                        table_map eliminated_tables);
  int append_table_array(spider_fields *fields,
                         spider_string *str, TABLE_LIST **table,
                         TABLE_LIST **end, table_map *upper_usable_tables,
                         table_map eliminated_tables);
  int append_join(spider_fields *fields, spider_string *str,
                  List<TABLE_LIST> *tables, table_map *upper_usable_tables,
                  table_map eliminated_tables);
  int append_from_and_tables(
    ha_spider *spider,
    spider_fields *fields,
    spider_string *str,
    TABLE_LIST *table_list,
    uint table_count
  ) override;
  int append_where(
    spider_string *str
  ) override;
  int append_having(
    spider_string *str
  ) override;
  bool tables_on_different_db_are_joinable() override;
  bool socket_has_default_value() override;
  bool database_has_default_value() override;
  bool default_file_has_default_value() override;
  bool host_has_default_value() override;
  bool port_has_default_value() override;
  bool append_charset_name_before_string() override;
};

class spider_db_mysql_util: public spider_db_mbase_util
{
public:
  spider_db_mysql_util();
  ~spider_db_mysql_util();
  int append_column_value(
    ha_spider *spider,
    spider_string *str,
    Field *field,
    const uchar *new_ptr,
    bool is_like,
    CHARSET_INFO *access_charset
  ) override;
};

class spider_db_mariadb_util: public spider_db_mbase_util
{
public:
  spider_db_mariadb_util();
  ~spider_db_mariadb_util();
  int append_sql_mode_internal(
    spider_string *str,
    sql_mode_t sql_mode
  ) override;
  int append_column_value(
    ha_spider *spider,
    spider_string *str,
    Field *field,
    const uchar *new_ptr,
    bool is_like,
    CHARSET_INFO *access_charset
  ) override;
};

class spider_db_mbase_row: public spider_db_row
{
public:
  MYSQL_ROW           row;
  MYSQL_ROW           row_first;
  ulong               *lengths;
  ulong               *lengths_first;
  uint                field_count;
  uint                record_size;
  bool                cloned;
  spider_db_mbase_row(
    uint dbton_id
  );
  virtual ~spider_db_mbase_row();
  int store_to_field(
    Field *field,
    CHARSET_INFO *access_charset
  ) override;
  int append_to_str(
    spider_string *str
  ) override;
  int append_escaped_to_str(
    spider_string *str,
    uint dbton_id
  ) override;
  void first() override;
  void next() override;
  bool is_null() override;
  int val_int() override;
  double val_real() override;
  my_decimal *val_decimal(
    my_decimal *decimal_value,
    CHARSET_INFO *access_charset
  ) override;
  SPIDER_DB_ROW *clone() override;
  int store_to_tmp_table(
    TABLE *tmp_table,
    spider_string *str
  ) override;
  uint get_byte_size() override;
};

class spider_db_mysql_row: public spider_db_mbase_row
{
public:
  spider_db_mysql_row();
  ~spider_db_mysql_row();
};

class spider_db_mariadb_row: public spider_db_mbase_row
{
public:
  spider_db_mariadb_row();
  ~spider_db_mariadb_row();
};

class spider_db_mbase_result: public spider_db_result
{
public:
  MYSQL_RES           *db_result;
  spider_db_mbase_row row;
  MYSQL_ROW_OFFSET    first_row;
  int                 store_error_num;
  spider_db_mbase_result(
    SPIDER_DB_CONN *in_db_conn
  );
  virtual ~spider_db_mbase_result();
  bool has_result() override;
  void free_result() override;
  SPIDER_DB_ROW *current_row() override;
  SPIDER_DB_ROW *fetch_row(MY_BITMAP *) override;
  SPIDER_DB_ROW *fetch_row_from_tmp_table(
    TABLE *tmp_table
  ) override;
  int fetch_table_status(
    int mode,
    ha_statistics &stat
  ) override;
  int fetch_simple_action(
    uint simple_action,
    uint position,
    void *param
  );
  int fetch_table_records(
    int mode,
    ha_rows &records
  ) override;
  int fetch_table_checksum(
    ha_spider *spider
  ) override;
  int fetch_table_cardinality(
    int mode,
    TABLE *table,
    longlong *cardinality,
    uchar *cardinality_upd,
    int bitmap_size
  ) override;
  int fetch_table_mon_status(
    int &status
  ) override;
  int fetch_show_master_status(
    const char **binlog_file_name,
    const char **binlog_pos
  );
  int fetch_select_binlog_gtid_pos(
    const char **gtid_pos
  );
  longlong num_rows() override;
  uint num_fields() override;
  void move_to_pos(
    longlong pos
  ) override;
  int get_errno() override;
#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
  int fetch_columns_for_discover_table_structure(
    spider_string *str,
    CHARSET_INFO *access_charset
  ) override;
  int fetch_index_for_discover_table_structure(
    spider_string *str,
    CHARSET_INFO *access_charset
  ) override;
  int fetch_table_for_discover_table_structure(
    spider_string *str,
    SPIDER_SHARE *spider_share,
    CHARSET_INFO *access_charset
  ) override;
#endif
};

class spider_db_mysql_result: public spider_db_mbase_result
{
public:
  spider_db_mysql_result(
    SPIDER_DB_CONN *in_db_conn
  );
  ~spider_db_mysql_result();
};

class spider_db_mariadb_result: public spider_db_mbase_result
{
public:
  spider_db_mariadb_result(
    SPIDER_DB_CONN *in_db_conn
  );
  ~spider_db_mariadb_result();
};

class spider_db_mbase: public spider_db_conn
{
protected:
  int                  stored_error;
  spider_db_mbase_util *spider_db_mbase_utility;
public:
  MYSQL          *db_conn;
  HASH           lock_table_hash;
  bool           lock_table_hash_inited;
  uint           lock_table_hash_id;
  const char     *lock_table_hash_func_name;
  const char     *lock_table_hash_file_name;
  ulong          lock_table_hash_line_no;
  DYNAMIC_ARRAY  handler_open_array;
  bool           handler_open_array_inited;
  uint           handler_open_array_id;
  const char     *handler_open_array_func_name;
  const char     *handler_open_array_file_name;
  ulong          handler_open_array_line_no;
  spider_db_mbase(
    SPIDER_CONN *conn,
    spider_db_mbase_util *spider_db_mbase_utility
  );
  virtual ~spider_db_mbase();
  int init() override;
  bool is_connected() override;
  void bg_connect() override;
  int connect(
    char *tgt_host,
    char *tgt_username,
    char *tgt_password,
    long tgt_port,
    char *tgt_socket,
    char *server_name,
    int connect_retry_count,
    longlong connect_retry_interval
  ) override;
  int ping() override;
  void bg_disconnect() override;
  void disconnect() override;
  int set_net_timeout() override;
  int exec_query(
    const char *query,
    uint length,
    int quick_mode
  ) override;
  int get_errno() override;
  const char *get_error() override;
  bool is_server_gone_error(
    int error_num
  ) override;
  bool is_dup_entry_error(
    int error_num
  ) override;
  bool is_xa_nota_error(
    int error_num
  ) override;
  int fetch_and_print_warnings(struct tm *l_time);
  spider_db_result *store_result(
    spider_db_result_buffer **spider_res_buf,
    st_spider_db_request_key *request_key,
    int *error_num
  ) override;
  spider_db_result *use_result(
    ha_spider *spider,
    st_spider_db_request_key *request_key,
    int *error_num
  ) override;
  int next_result() override;
  uint affected_rows() override;
  uint matched_rows() override;
  bool inserted_info(
    spider_db_handler *handler,
    ha_copy_info *copy_info
  ) override;
  ulonglong last_insert_id() override;
  int set_character_set(
    const char *csname
  ) override;
  int select_db(
    const char *dbname
  ) override;
  int consistent_snapshot(
    int *need_mon
  ) override;
  bool trx_start_in_bulk_sql() override;
  int start_transaction(
    int *need_mon
  ) override;
  int commit(
    int *need_mon
  ) override;
  int rollback(
    int *need_mon
  ) override;
  bool xa_start_in_bulk_sql() override;
  int xa_start(
    XID *xid,
    int *need_mon
  ) override;
  int xa_end(
    XID *xid,
    int *need_mon
  ) override;
  int xa_prepare(
    XID *xid,
    int *need_mon
  ) override;
  int xa_commit(
    XID *xid,
    int *need_mon
  ) override;
  int xa_rollback(
    XID *xid,
    int *need_mon
  ) override;
  bool set_trx_isolation_in_bulk_sql() override;
  int set_trx_isolation(
    int trx_isolation,
    int *need_mon
  ) override;
  bool set_autocommit_in_bulk_sql() override;
  int set_autocommit(
    bool autocommit,
    int *need_mon
  ) override;
  bool set_sql_log_off_in_bulk_sql() override;
  int set_sql_log_off(
    bool sql_log_off,
    int *need_mon
  ) override;
  bool set_wait_timeout_in_bulk_sql() override;
  int set_wait_timeout(
    int wait_timeout,
    int *need_mon
  ) override;
  bool set_sql_mode_in_bulk_sql() override;
  int set_sql_mode(
    sql_mode_t sql_mode,
    int *need_mon
  ) override;
  bool set_time_zone_in_bulk_sql() override;
  int set_time_zone(
    Time_zone *time_zone,
    int *need_mon
  ) override;
  int exec_simple_sql_with_result(
    SPIDER_TRX *trx,
    SPIDER_SHARE *share,
    const char *sql,
    uint sql_length,
    int all_link_idx,
    int *need_mon,
    SPIDER_DB_RESULT **res
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
  ) override;
  int select_binlog_gtid_pos(
    SPIDER_TRX *trx,
    SPIDER_SHARE *share,
    int all_link_idx,
    int *need_mon,
    TABLE *table,
    spider_string *str,
    const char *binlog_file_name,
    uint binlog_file_name_length,
    const char *binlog_pos,
    uint binlog_pos_length,
    SPIDER_DB_RESULT **res
  );
  size_t escape_string(
    char *to,
    const char *from,
    size_t from_length
  ) override;
  bool have_lock_table_list() override;
  int append_lock_tables(
    spider_string *str
  ) override;
  int append_unlock_tables(
    spider_string *str
  ) override;
  uint get_lock_table_hash_count() override;
  void reset_lock_table_hash() override;
  uint get_opened_handler_count() override;
  void reset_opened_handler() override;
  void set_dup_key_idx(
    ha_spider *spider,
    int link_idx
  ) override;
  bool cmp_request_key_to_snd(
    st_spider_db_request_key *request_key
  ) override;
};

class spider_db_mysql: public spider_db_mbase
{
public:
  spider_db_mysql(
    SPIDER_CONN *conn
  );
  ~spider_db_mysql();
};

class spider_db_mariadb: public spider_db_mbase
{
public:
  spider_db_mariadb(
    SPIDER_CONN *conn
  );
  ~spider_db_mariadb();
};

class spider_mbase_share: public spider_db_share
{
protected:
  spider_db_mbase_util *spider_db_mbase_utility;
public:
  spider_string      *table_select;
  int                table_select_pos;
  spider_string      *key_select;
  int                *key_select_pos;
  spider_string      *key_hint;
  spider_string      *show_table_status;
  spider_string      *show_records;
  spider_string      *show_index;
  /* The remote table names */
  spider_string      *table_names_str;
  /* The remote db names */
  spider_string      *db_names_str;
  /* fixme: this field looks useless */
  spider_string      *db_table_str;
  my_hash_value_type *db_table_str_hash_value;
  uint               table_nm_max_length;
  uint               db_nm_max_length;
  spider_string      *column_name_str;
  bool               same_db_table_name;
  int                first_all_link_idx;

  spider_mbase_share(
    st_spider_share *share,
    uint dbton_id,
    spider_db_mbase_util *spider_db_mbase_utility
  );
  virtual ~spider_mbase_share();
  int init() override;
  uint get_column_name_length(
    uint field_index
  ) override;
  int append_column_name(
    spider_string *str,
    uint field_index
  ) override;
  int append_column_name_with_alias(
    spider_string *str,
    uint field_index,
    const char *alias,
    uint alias_length
  ) override;
  int append_table_name(
    spider_string *str,
    int all_link_idx
  );
  int append_table_name_with_adjusting(
    spider_string *str,
    int all_link_idx
  );
  int append_from_with_adjusted_table_name(
    spider_string *str,
    int *table_name_pos
  );
  bool need_change_db_table_name() override;
#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
  int discover_table_structure(
    SPIDER_TRX *trx,
    SPIDER_SHARE *spider_share,
    spider_string *str
  ) override;
#endif
  bool checksum_support() override;
protected:
  int create_table_names_str();
  void free_table_names_str();
  int create_column_name_str();
  void free_column_name_str();
  int convert_key_hint_str();
  int append_show_table_status();
  void free_show_table_status();
  int append_show_records();
  void free_show_records();
  int append_show_index();
  void free_show_index();
  int append_table_select();
  int append_key_select(
    uint idx
  );
};

class spider_mysql_share: public spider_mbase_share
{
public:
  spider_mysql_share(
    st_spider_share *share
  );
  ~spider_mysql_share();
};

class spider_mariadb_share: public spider_mbase_share
{
public:
  spider_mariadb_share(
    st_spider_share *share
  );
  ~spider_mariadb_share();
};

class spider_mbase_handler: public spider_db_handler
{
protected:
  spider_db_mbase_util    *spider_db_mbase_utility;
  spider_string           sql;
  spider_string           sql_part;
  spider_string           sql_part2;
  spider_string           ha_sql;
  int                     where_pos;
  int                     order_pos;
  int                     limit_pos;
public:
  int                     table_name_pos;
protected:
  int                     ha_read_pos;
  int                     ha_next_pos;
  int                     ha_where_pos;
  int                     ha_limit_pos;
  int                     ha_table_name_pos;
  uint                    ha_sql_handler_id;
  spider_string           insert_sql;
  int                     insert_pos;
  int                     insert_table_name_pos;
  spider_string           update_sql;
  TABLE                   *upd_tmp_tbl;
  TMP_TABLE_PARAM         upd_tmp_tbl_prm;
  spider_string           tmp_sql;
  int                     tmp_sql_pos1; /* drop db nm pos at tmp_table_join */
  int                     tmp_sql_pos2; /* create db nm pos at tmp_table_join */
  int                     tmp_sql_pos3; /* insert db nm pos at tmp_table_join */
  int                     tmp_sql_pos4; /* insert val pos at tmp_table_join */
  int                     tmp_sql_pos5; /* end of drop tbl at tmp_table_join */
  spider_string           dup_update_sql;
  spider_string           *exec_sql;
  spider_string           *exec_insert_sql;
  spider_string           *exec_update_sql;
  spider_string           *exec_tmp_sql;
  spider_string           *exec_ha_sql;
  bool                    reading_from_bulk_tmp_table;
  bool                    filled_up;
  SPIDER_INT_HLD          *union_table_name_pos_first;
  SPIDER_INT_HLD          *union_table_name_pos_current;
public:
  spider_mbase_share      *mysql_share;
  SPIDER_LINK_FOR_HASH    *link_for_hash;
  uchar                   *minimum_select_bitmap;
  uchar                   direct_insert_kind;
  spider_mbase_handler(
    ha_spider *spider,
    spider_mbase_share *share,
    spider_db_mbase_util *spider_db_mbase_utility
  );
  virtual ~spider_mbase_handler();
  int init() override;
  int append_index_hint(
    spider_string *str,
    int link_idx,
    ulong sql_type
  ) override;
  int append_table_name_with_adjusting(
    spider_string *str,
    int link_idx,
    ulong sql_type
  ) override;
  int append_key_column_types(
    const key_range *start_key,
    spider_string *str
  );
  int append_key_join_columns_for_bka(
    const key_range *start_key,
    spider_string *str,
    const char **table_aliases,
    uint *table_alias_lengths
  );
  int append_tmp_table_and_sql_for_bka(
    const key_range *start_key
  ) override;
  int reuse_tmp_table_and_sql_for_bka() override;
  void create_tmp_bka_table_name(
    char *tmp_table_name,
    int *tmp_table_name_length,
    int link_idx
  );
  int append_create_tmp_bka_table(
    const key_range *start_key,
    spider_string *str,
    char *tmp_table_name,
    int tmp_table_name_length,
    int *db_name_pos,
    CHARSET_INFO *table_charset
  );
  int append_drop_tmp_bka_table(
    spider_string *str,
    char *tmp_table_name,
    int tmp_table_name_length,
    int *db_name_pos,
    int *drop_table_end_pos,
    bool with_semicolon
  );
  int append_insert_tmp_bka_table(
    const key_range *start_key,
    spider_string *str,
    char *tmp_table_name,
    int tmp_table_name_length,
    int *db_name_pos
  );
  int append_union_table_and_sql_for_bka(
    const key_range *start_key
  ) override;
  int reuse_union_table_and_sql_for_bka() override;
  int append_insert_for_recovery(
    ulong sql_type,
    int link_idx
  ) override;
  int append_update(
    const TABLE *table,
    my_ptrdiff_t ptr_diff
  ) override;
  int append_update(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    int link_idx
  ) override;
  int append_delete(
    const TABLE *table,
    my_ptrdiff_t ptr_diff
  ) override;
  int append_delete(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    int link_idx
  ) override;
  int append_insert_part() override;
  int append_insert(
    spider_string *str,
    int link_idx
  );
  int append_update_part() override;
  int append_update(
    spider_string *str,
    int link_idx
  );
  int append_delete_part() override;
  int append_delete(
    spider_string *str
  );
  int append_update_set_part() override;
  int append_update_set(
    spider_string *str
  );
  int append_direct_update_set_part() override;
  int append_direct_update_set(
    spider_string *str
  );
  int append_dup_update_pushdown_part(
    const char *alias,
    uint alias_length
  ) override;
  int append_update_columns_part(
    const char *alias,
    uint alias_length
  ) override;
  int check_update_columns_part() override;
  int append_update_columns(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_select_part(
    ulong sql_type
  ) override;
  int append_select(
    spider_string *str,
    ulong sql_type
  ) override;
  int append_table_select_part(
    ulong sql_type
  ) override;
  int append_table_select(
    spider_string *str
  );
  int append_key_select_part(
    ulong sql_type,
    uint idx
  ) override;
  int append_key_select(
    spider_string *str,
    ulong sql_type,
    uint idx
  );
  int append_minimum_select_part(
    ulong sql_type
  ) override;
  int append_minimum_select(
    spider_string *str,
    ulong sql_type
  );
  int append_table_select_with_alias(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_key_select_with_alias(
    spider_string *str,
    const KEY *key_info,
    const char *alias,
    uint alias_length
  );
  int append_minimum_select_with_alias(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_select_columns_with_alias(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_hint_after_table_part(
    ulong sql_type
  ) override;
  int append_hint_after_table(
    spider_string *str
  );
  void set_where_pos(
    ulong sql_type
  ) override;
  void set_where_to_pos(
    ulong sql_type
  ) override;
  int check_item_type(
    Item *item
  ) override;
  int append_values_connector_part(
    ulong sql_type
  ) override;
  int append_values_connector(
    spider_string *str
  );
  int append_values_terminator_part(
    ulong sql_type
  ) override;
  int append_values_terminator(
    spider_string *str
  );
  int append_union_table_connector_part(
    ulong sql_type
  ) override;
  int append_union_table_connector(
    spider_string *str
  );
  int append_union_table_terminator_part(
    ulong sql_type
  ) override;
  int append_union_table_terminator(
    spider_string *str
  );
  int append_key_column_values_part(
    const key_range *start_key,
    ulong sql_type
  ) override;
  int append_key_column_values(
    spider_string *str,
    const key_range *start_key
  );
  int append_key_column_values_with_name_part(
    const key_range *start_key,
    ulong sql_type
  ) override;
  int append_key_column_values_with_name(
    spider_string *str,
    const key_range *start_key
  );
  int append_key_where_part(
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type
  ) override;
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
  ) override;
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
  ) override;
  int append_where_terminator(
    ulong sql_type,
    spider_string *str,
    spider_string *str_part,
    spider_string *str_part2,
    bool set_order,
    int key_count
  );
  int append_match_where_part(
    ulong sql_type
  ) override;
  int append_match_where(
    spider_string *str
  );
  int append_update_where(
    spider_string *str,
    const TABLE *table,
    my_ptrdiff_t ptr_diff
  );
  int append_condition_part(
    const char *alias,
    uint alias_length,
    ulong sql_type,
    bool test_flg
  ) override;
  int append_condition(
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool start_where,
    ulong sql_type
  );
  int append_match_against_part(
    ulong sql_type,
    st_spider_ft_info *ft_info,
    const char *alias,
    uint alias_length
  );
  int append_match_against(
    spider_string *str,
    st_spider_ft_info  *ft_info,
    const char *alias,
    uint alias_length
  );
  int append_match_select_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  ) override;
  int append_match_select(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_sum_select_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  ) override;
  int append_sum_select(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  void set_order_pos(
    ulong sql_type
  ) override;
  void set_order_to_pos(
    ulong sql_type
  ) override;
  int append_group_by_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) override;
  int append_group_by(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_key_order_for_merge_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) override;
  int append_key_order_for_merge_with_alias(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_key_order_for_direct_order_limit_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) override;
  int append_key_order_for_direct_order_limit_with_alias(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_key_order_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) override;
  int append_key_order_for_handler(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_key_order_with_alias(
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  int append_limit_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  ) override;
  int reappend_limit_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  ) override;
  int append_limit(
    spider_string *str,
    longlong offset,
    longlong limit
  );
  int append_select_lock_part(
    ulong sql_type
  ) override;
  int append_select_lock(
    spider_string *str
  );
  int append_union_all_start_part(
    ulong sql_type
  ) override;
  int append_union_all_start(
    spider_string *str
  );
  int append_union_all_part(
    ulong sql_type
  ) override;
  int append_union_all(
    spider_string *str
  );
  int append_union_all_end_part(
    ulong sql_type
  ) override;
  int append_union_all_end(
    spider_string *str
  );
  int append_multi_range_cnt_part(
    ulong sql_type,
    uint multi_range_cnt,
    bool with_comma
  ) override;
  int append_multi_range_cnt(
    spider_string *str,
    uint multi_range_cnt,
    bool with_comma
  );
  int append_multi_range_cnt_with_name_part(
    ulong sql_type,
    uint multi_range_cnt
  ) override;
  int append_multi_range_cnt_with_name(
    spider_string *str,
    uint multi_range_cnt
  );
  int append_open_handler_part(
    ulong sql_type,
    uint handler_id,
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int append_open_handler(
    spider_string *str,
    uint handler_id,
    SPIDER_CONN *conn,
    int link_idx
  );
  int append_close_handler_part(
    ulong sql_type,
    int link_idx
  ) override;
  int append_close_handler(
    spider_string *str,
    int link_idx
  );
  int append_insert_terminator_part(
    ulong sql_type
  ) override;
  int append_insert_terminator(
    spider_string *str
  );
  int append_insert_values_part(
    ulong sql_type
  ) override;
  int append_insert_values(
    spider_string *str
  );
  int append_into_part(
    ulong sql_type
  ) override;
  int append_into(
    spider_string *str
  );
  void set_insert_to_pos(
    ulong sql_type
  ) override;
  int append_from_part(
    ulong sql_type,
    int link_idx
  ) override;
  int append_from(
    spider_string *str,
    ulong sql_type,
    int link_idx
  );
  int append_flush_tables_part(int link_idx, bool lock);
  int append_flush_tables(
    spider_string *str,
    int link_idx,
    bool lock
  );
  int append_optimize_table_part(int link_idx);
  int append_optimize_table(
    spider_string *str,
    int link_idx
  );
  int append_analyze_table_part(int link_idx);
  int append_analyze_table(
    spider_string *str,
    int link_idx
  );
  int append_repair_table_part(int link_idx, HA_CHECK_OPT *check_opt);
  int append_repair_table(
    spider_string *str,
    int link_idx,
    HA_CHECK_OPT* check_opt
  );
  int append_check_table_part(int link_idx, HA_CHECK_OPT *check_opt);
  int append_check_table(
    spider_string *str,
    int link_idx,
    HA_CHECK_OPT* check_opt
  );
  int append_enable_keys_part(int link_idx);
  int append_enable_keys(
    spider_string *str,
    int link_idx
  );
  int append_disable_keys_part(int link_idx);
  int append_disable_keys(
    spider_string *str,
    int link_idx
  );
  int append_delete_all_rows_part(
    ulong sql_type
  ) override;
  int append_delete_all_rows(
    spider_string *str,
    ulong sql_type
  );
  int append_truncate(
    spider_string *str,
    ulong sql_type,
    int link_idx
  );
  int append_explain_select_part(
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type,
    int link_idx
  ) override;
  int append_explain_select(
    spider_string *str,
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type,
    int link_idx
  );
  bool is_sole_projection_field(
    uint16 field_index
  ) override;
  bool is_bulk_insert_exec_period(
    bool bulk_end
  ) override;
  bool sql_is_filled_up(
    ulong sql_type
  ) override;
  bool sql_is_empty(
    ulong sql_type
  ) override;
  bool support_multi_split_read() override;
  bool support_bulk_update() override;
  int bulk_tmp_table_insert() override;
  int bulk_tmp_table_insert(
    int link_idx
  ) override;
  int bulk_tmp_table_end_bulk_insert() override;
  int bulk_tmp_table_rnd_init() override;
  int bulk_tmp_table_rnd_next() override;
  int bulk_tmp_table_rnd_end() override;
  bool need_copy_for_update(
    int link_idx
  ) override;
  bool bulk_tmp_table_created() override;
  int mk_bulk_tmp_table_and_bulk_start() override;
  void rm_bulk_tmp_table() override;
  int store_sql_to_bulk_tmp_table(
    spider_string *str,
    TABLE *tmp_table
  );
  int restore_sql_from_bulk_tmp_table(
    spider_string *str,
    TABLE *tmp_table
  );
  int insert_lock_tables_list(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int append_lock_tables_list(
    SPIDER_CONN *conn,
    int link_idx,
    int *appended
  ) override;
  int realloc_sql(
    ulong *realloced
  ) override;
  int reset_sql(
    ulong sql_type
  ) override;
  int set_sql_for_exec(
    ulong sql_type,
    int link_idx,
    SPIDER_LINK_IDX_CHAIN *link_idx_chain
  ) override;
  int set_sql_for_exec(
    ulong sql_type,
    int link_idx
  ) override;
  int set_sql_for_exec(
    spider_db_copy_table *tgt_ct,
    ulong sql_type
  ) override;
  int execute_sql(
    ulong sql_type,
    SPIDER_CONN *conn,
    int quick_mode,
    int *need_mon
  ) override;
  int reset() override;
  int sts_mode_exchange(
    int sts_mode
  ) override;
  int show_table_status(
    int link_idx,
    int sts_mode,
    uint flag
  ) override;
  int crd_mode_exchange(
    int crd_mode
  ) override;
  int show_index(
    int link_idx,
    int crd_mode
  ) override;
  int simple_action(
    uint simple_action,
    int link_idx
  );
  int show_records(
    int link_idx
  ) override;
  int checksum_table(
    int link_idx
  ) override;
  int show_last_insert_id(
    int link_idx,
    ulonglong &last_insert_id
  ) override;
  ha_rows explain_select(
    const key_range *start_key,
    const key_range *end_key,
    int link_idx
  ) override;
  int lock_tables(
    int link_idx
  ) override;
  int unlock_tables(
    int link_idx
  ) override;
  int disable_keys(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int enable_keys(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int check_table(
    SPIDER_CONN *conn,
    int link_idx,
    HA_CHECK_OPT* check_opt
  ) override;
  int repair_table(
    SPIDER_CONN *conn,
    int link_idx,
    HA_CHECK_OPT* check_opt
  ) override;
  int analyze_table(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int optimize_table(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int flush_tables(
    SPIDER_CONN *conn,
    int link_idx,
    bool lock
  ) override;
  int flush_logs(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int insert_opened_handler(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int delete_opened_handler(
    SPIDER_CONN *conn,
    int link_idx
  ) override;
  int sync_from_clone_source(
    spider_db_handler *dbton_hdl
  ) override;
  bool support_use_handler(
    int use_handler
  ) override;
  void minimum_select_bitmap_create();
  bool minimum_select_bit_is_set(
    uint field_index
  ) override;
  void copy_minimum_select_bitmap(
    uchar *bitmap
  ) override;
  int init_union_table_name_pos() override;
  int set_union_table_name_pos() override;
  int reset_union_table_name(
    spider_string *str,
    int link_idx,
    ulong sql_type
  ) override;
  int append_from_and_tables_part(
    spider_fields *fields,
    ulong sql_type
  ) override;
  int append_where_part(
    ulong sql_type
  ) override;
  int append_having_part(
    ulong sql_type
  ) override;
  int append_item_type_part(
    Item *item,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  ) override;
  int append_list_item_select_part(
    List<Item> *select,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type,
    int n_aux=0
  ) override;
  int append_list_item_select(
    List<Item> *select,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    int n_aux
  );
  int append_group_by_part(
    ORDER *order,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  ) override;
  int append_group_by(
    ORDER *order,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  );
  int append_order_by_part(
    ORDER *order,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  ) override;
  int append_order_by(
    ORDER *order,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  );
  bool check_direct_update(
    st_select_lex *select_lex,
    longlong select_limit,
    longlong offset_limit
  ) override;
  bool check_direct_delete(
    st_select_lex *select_lex,
    longlong select_limit,
    longlong offset_limit
  ) override;
};

class spider_mysql_handler: public spider_mbase_handler
{
public:
  spider_mysql_handler(
    ha_spider *spider,
    spider_mbase_share *share
  );
  ~spider_mysql_handler();
};

class spider_mariadb_handler: public spider_mbase_handler
{
public:
  spider_mariadb_handler(
    ha_spider *spider,
    spider_mbase_share *share
  );
  ~spider_mariadb_handler();
};

class spider_mbase_copy_table: public spider_db_copy_table
{
public:
  spider_mbase_share      *mysql_share;
  spider_string           sql;
  uint                    pos;
  spider_mbase_copy_table(
    spider_mbase_share *db_share
  );
  virtual ~spider_mbase_copy_table();
  int init() override;
  void set_sql_charset(
    CHARSET_INFO *cs
  ) override;
  int append_select_str() override;
  int append_insert_str(
    int insert_flg
  ) override;
  int append_table_columns(
    TABLE_SHARE *table_share
  ) override;
  int append_from_str() override;
  int append_table_name(
    int link_idx
  ) override;
  void set_sql_pos() override;
  void set_sql_to_pos() override;
  int append_copy_where(
    spider_db_copy_table *source_ct,
    KEY *key_info,
    ulong *last_row_pos,
    ulong *last_lengths
  ) override;
  int append_key_order_str(
    KEY *key_info,
    int start_pos,
    bool desc_flg
  ) override;
  int append_limit(
    longlong offset,
    longlong limit
  ) override;
  int append_into_str() override;
  int append_open_paren_str() override;
  int append_values_str() override;
  int append_select_lock_str(
    int lock_mode
  ) override;
  int exec_query(
    SPIDER_CONN *conn,
    int quick_mode,
    int *need_mon
  ) override;
  int copy_key_row(
    spider_db_copy_table *source_ct,
    Field *field,
    ulong *row_pos,
    ulong *length,
    const char *joint_str,
    const int joint_length
  );
  int copy_row(
    Field *field,
    SPIDER_DB_ROW *row
  );
  int copy_rows(
    TABLE *table,
    SPIDER_DB_ROW *row,
    ulong **last_row_pos,
    ulong **last_lengths
  ) override;
  int copy_rows(
    TABLE *table,
    SPIDER_DB_ROW *row
  ) override;
  int append_insert_terminator() override;
  int copy_insert_values(
    spider_db_copy_table *source_ct
  ) override;
};

class spider_mysql_copy_table: public spider_mbase_copy_table
{
public:
  spider_mysql_copy_table(
    spider_mbase_share *db_share
  );
  ~spider_mysql_copy_table();
};

class spider_mariadb_copy_table: public spider_mbase_copy_table
{
public:
  spider_mariadb_copy_table(
    spider_mbase_share *db_share
  );
  ~spider_mariadb_copy_table();
};
