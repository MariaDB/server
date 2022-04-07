/* Copyright (C) 2008-2019 Kentoku Shiba
   Copyright (C) 2019-2022 MariaDB corp

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

#ifdef USE_PRAGMA_INTERFACE
#pragma interface
#endif

#define SPIDER_CONNECT_INFO_MAX_LEN 64
#define SPIDER_CONNECT_INFO_PATH_MAX_LEN FN_REFLEN
#define SPIDER_LONGLONG_LEN 20
#define SPIDER_MAX_KEY_LENGTH 16384

#define SPIDER_SET_CONNS_PARAM(param_name, param_val, conns, link_statuses, conn_link_idx, link_count, link_status) \
  for ( \
    roop_count = spider_conn_link_idx_next(link_statuses, \
      conn_link_idx, -1, link_count, link_status); \
    roop_count < link_count; \
    roop_count = spider_conn_link_idx_next(link_statuses, \
      conn_link_idx, roop_count, link_count, link_status) \
  ) { \
    if (conns[roop_count]) \
      conns[roop_count]->param_name = param_val; \
  }

class ha_spider;
struct st_spider_ft_info
{
  struct _ft_vft *please;
  st_spider_ft_info *next;
  ha_spider *file;
  uint target;
  bool used_in_where;
  float score;
  uint flags;
  uint inx;
  String *key;
};

class ha_spider final : public handler
{
public:
  SPIDER_SHARE       *share;
  ulonglong          spider_thread_id;
  ulonglong          trx_conn_adjustment;
  uint               mem_calc_id;
  const char         *mem_calc_func_name;
  const char         *mem_calc_file_name;
  ulong              mem_calc_line_no;
  uint               sql_kinds;
  uint               *sql_kind;
  ulonglong          *connection_ids;
  uint               conn_kinds;
  uint               *conn_kind;
  char               *conn_keys_first_ptr;
  char               **conn_keys;
  SPIDER_CONN        **conns;
  /* for active-standby mode */
  uint               *conn_link_idx;
  uchar              *conn_can_fo;
  void               **quick_targets;
  int                *need_mons;
  query_id_t         search_link_query_id;
  int                search_link_idx;
  int                result_link_idx;
  SPIDER_RESULT_LIST result_list;
  spider_string      *blob_buff;
  SPIDER_POSITION    *pushed_pos;
  SPIDER_POSITION    pushed_pos_buf;
  SPIDER_PARTITION_HANDLER *partition_handler;
  bool                wide_handler_owner = FALSE;
  SPIDER_WIDE_HANDLER *wide_handler = NULL;

  bool               is_clone;
  ha_spider          *pt_clone_source_handler;
  ha_spider          *pt_clone_last_searcher;
  bool               use_index_merge;

  bool               init_index_handler;
  bool               init_rnd_handler;

  bool               da_status;
  bool               use_spatial_index;

  uint                  idx_for_direct_join;
  bool                  use_fields;
  spider_fields         *fields;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *result_link_idx_chain;

  /* for mrr */
  bool               mrr_with_cnt;
  uint               multi_range_cnt;
  uint               multi_range_hit_point;
  int                multi_range_num;
  bool               have_second_range;
  KEY_MULTI_RANGE    mrr_second_range;
  spider_string      *mrr_key_buff;
  range_id_t         *multi_range_keys;

  char               *append_tblnm_alias;
  uint               append_tblnm_alias_length;

  ha_spider          *next;

  bool               dml_inited;
  bool               rnd_scan_and_first;
  bool               use_pre_call;
  bool               use_pre_action;
  bool               pre_bitmap_checked;
  bool               bulk_insert;
  bool               info_auto_called;
#ifdef HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
  bool               auto_inc_temporary;
#endif
  int                bulk_size;
  int                direct_dup_insert;
  int                store_error_num;
  uint               dup_key_idx;
  int                select_column_mode;
  bool               pk_update;
  bool               force_auto_increment;
  int                bka_mode;
  int                error_mode;
  ulonglong          store_last_insert_id;

  ulonglong          *db_request_id;
  uchar              *db_request_phase;
  uchar              *m_handler_opened;
  uint               *m_handler_id;
  char               **m_handler_cid;
  bool               do_direct_update;
  uint               direct_update_kinds;
  spider_index_rnd_init prev_index_rnd_init;
  SPIDER_ITEM_HLD    *direct_aggregate_item_first;
  SPIDER_ITEM_HLD    *direct_aggregate_item_current;
  ha_rows            table_rows;
  ha_checksum        checksum_val;
  bool               checksum_null;
  uint               action_flags;

  /* for fulltext search */
  bool               ft_init_and_first;
  uint               ft_init_idx;
  uint               ft_count;
  bool               ft_init_without_index_init;
  st_spider_ft_info  *ft_first;
  st_spider_ft_info  *ft_current;

  /* for dbton */
  spider_db_handler  **dbton_handler;

  /* for direct limit offset */
  longlong direct_select_offset;
  longlong direct_current_offset;
  longlong direct_select_limit;

  ha_spider();
  ha_spider(
    handlerton *hton,
    TABLE_SHARE *table_arg
  );
  virtual ~ha_spider();
  handler *clone(
    const char *name,
    MEM_ROOT *mem_root
  );
  const char **bas_ext() const;
  int open(
    const char* name,
    int mode,
    uint test_if_locked
  );
  int close();
  int check_access_kind_for_connection(
    THD *thd,
    bool write_request
  );
  void check_access_kind(
    THD *thd
  );
  THR_LOCK_DATA **store_lock(
    THD *thd,
    THR_LOCK_DATA **to,
    enum thr_lock_type lock_type
  );
  int external_lock(
    THD *thd,
    int lock_type
  );
  int start_stmt(
    THD *thd,
    thr_lock_type lock_type
  );
  int reset();
  int extra(
    enum ha_extra_function operation
  );
  int index_init(
    uint idx,
    bool sorted
  );
  int index_end();
  int index_read_map(
    uchar *buf,
    const uchar *key,
    key_part_map keypart_map,
    enum ha_rkey_function find_flag
  );
  int index_read_last_map(
    uchar *buf,
    const uchar *key,
    key_part_map keypart_map
  );
  int index_next(
    uchar *buf
  );
  int index_prev(
    uchar *buf
  );
  int index_first(
    uchar *buf
  );
  int index_last(
    uchar *buf
  );
  int index_next_same(
    uchar *buf,
    const uchar *key,
    uint keylen
  );
  int read_range_first(
    const key_range *start_key,
    const key_range *end_key,
    bool eq_range,
    bool sorted
  );
  int read_range_next();
  void reset_no_where_cond();
  bool check_no_where_cond();
  ha_rows multi_range_read_info_const(
    uint keyno,
    RANGE_SEQ_IF *seq,
    void *seq_init_param,
    uint n_ranges,
    uint *bufsz,
    uint *flags,
    Cost_estimate *cost
  );
  ha_rows multi_range_read_info(
    uint keyno,
    uint n_ranges,
    uint keys,
    uint key_parts,
    uint *bufsz,
    uint *flags,
    Cost_estimate *cost
  );
  int multi_range_read_init(
    RANGE_SEQ_IF *seq,
    void *seq_init_param,
    uint n_ranges,
    uint mode,
    HANDLER_BUFFER *buf
  );
  int multi_range_read_next(
    range_id_t *range_info
  );
  int multi_range_read_next_first(
    range_id_t *range_info
  );
  int multi_range_read_next_next(
    range_id_t *range_info
  );
  int rnd_init(
    bool scan
  );
  int rnd_end();
  int rnd_next(
    uchar *buf
  );
  void position(
    const uchar *record
  );
  int rnd_pos(
    uchar *buf,
    uchar *pos
  );
  int cmp_ref(
    const uchar *ref1,
    const uchar *ref2
  );
  int ft_init();
  void ft_end();
  FT_INFO *ft_init_ext(
    uint flags,
    uint inx,
    String *key
  );
  int ft_read(
    uchar *buf
  );
  int pre_index_read_map(
    const uchar *key,
    key_part_map keypart_map,
    enum ha_rkey_function find_flag,
    bool use_parallel
  );
  int pre_index_first(bool use_parallel);
  int pre_index_last(bool use_parallel);
  int pre_index_read_last_map(
    const uchar *key,
    key_part_map keypart_map,
    bool use_parallel
  );
  int pre_multi_range_read_next(
    bool use_parallel
  );
  int pre_read_range_first(
    const key_range *start_key,
    const key_range *end_key,
    bool eq_range,
    bool sorted,
    bool use_parallel
  );
  int pre_ft_read(bool use_parallel);
  int pre_rnd_next(bool use_parallel);
  int info(
    uint flag
  );
  ha_rows records_in_range(
    uint inx,
    const key_range *start_key,
    const key_range *end_key,
    page_range *pages
  );
  int check_crd();
  int pre_records();
  ha_rows records();
  int pre_calculate_checksum();
  int calculate_checksum();
  const char *table_type() const;
  ulonglong table_flags() const;
  ulong table_flags_for_partition();
  const char *index_type(
    uint key_number
  );
  ulong index_flags(
    uint idx,
    uint part,
    bool all_parts
  ) const;
  uint max_supported_record_length() const;
  uint max_supported_keys() const;
  uint max_supported_key_parts() const;
  uint max_supported_key_length() const;
  uint max_supported_key_part_length() const;
  uint8 table_cache_type();
  bool need_info_for_auto_inc();
#ifdef HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
  bool can_use_for_auto_inc_init();
#endif
  int update_auto_increment();
  void get_auto_increment(
    ulonglong offset,
    ulonglong increment,
    ulonglong nb_desired_values,
    ulonglong *first_value,
    ulonglong *nb_reserved_values
  );
  int reset_auto_increment(
    ulonglong value
  );
  void release_auto_increment();
  void start_bulk_insert(
    ha_rows rows,
    uint flags
  );
  int end_bulk_insert();
  int write_row(
    const uchar *buf
  );
  void direct_update_init(
    THD *thd,
    bool hs_request
  );
  bool start_bulk_update();
  int exec_bulk_update(
    ha_rows *dup_key_found
  );
  int end_bulk_update();
#ifdef SPIDER_UPDATE_ROW_HAS_CONST_NEW_DATA
  int bulk_update_row(
    const uchar *old_data,
    const uchar *new_data,
    ha_rows *dup_key_found
  );
  int update_row(
    const uchar *old_data,
    const uchar *new_data
  );
#else
  int bulk_update_row(
    const uchar *old_data,
    uchar *new_data,
    ha_rows *dup_key_found
  );
  int update_row(
    const uchar *old_data,
    uchar *new_data
  );
#endif
  bool check_direct_update_sql_part(
    st_select_lex *select_lex,
    longlong select_limit,
    longlong offset_limit
  );
#ifdef SPIDER_MDEV_16246
  int direct_update_rows_init(
    List<Item> *update_fields
  );
#else
  int direct_update_rows_init();
#endif
  int direct_update_rows(
    ha_rows *update_rows,
    ha_rows *found_row
  );
  bool start_bulk_delete();
  int end_bulk_delete();
  int delete_row(
    const uchar *buf
  );
  bool check_direct_delete_sql_part(
    st_select_lex *select_lex,
    longlong select_limit,
    longlong offset_limit
  );
  int direct_delete_rows_init();
  int direct_delete_rows(
    ha_rows *delete_rows
  );
  int delete_all_rows();
  int truncate();
  double scan_time();
  double read_time(
    uint index,
    uint ranges,
    ha_rows rows
  );
  const key_map *keys_to_use_for_scanning();
  ha_rows estimate_rows_upper_bound();
  void print_error(
    int error,
    myf errflag
  );
  bool get_error_message(
    int error,
    String *buf
  );
  int create(
    const char *name,
    TABLE *form,
    HA_CREATE_INFO *info
  );
  void update_create_info(
    HA_CREATE_INFO* create_info
  );
  int rename_table(
    const char *from,
    const char *to
  );
  int delete_table(
    const char *name
  );
  bool is_crashed() const;
#ifdef SPIDER_HANDLER_AUTO_REPAIR_HAS_ERROR
  bool auto_repair(int error) const;
#else
  bool auto_repair() const;
#endif
  int disable_indexes(
    uint mode
  );
  int enable_indexes(
    uint mode
  );
  int check(
    THD* thd,
    HA_CHECK_OPT* check_opt
  );
  int repair(
    THD* thd,
    HA_CHECK_OPT* check_opt
  );
  bool check_and_repair(
    THD *thd
  );
  int analyze(
    THD* thd,
    HA_CHECK_OPT* check_opt
  );
  int optimize(
    THD* thd,
    HA_CHECK_OPT* check_opt
  );
  bool is_fatal_error(
    int error_num,
    uint flags
  );
  Field *field_exchange(
    Field *field
  );
  const COND *cond_push(
    const COND* cond
  );
  void cond_pop();
  int info_push(
    uint info_type,
    void *info
  );
  void return_record_by_parent();
  TABLE *get_table();
  void set_ft_discard_bitmap();
  void set_searched_bitmap();
  void set_clone_searched_bitmap();
  void set_searched_bitmap_from_item_list();
  void set_select_column_mode();
  void check_select_column(bool rnd);
  bool check_and_start_bulk_update(
    spider_bulk_upd_start bulk_upd_start
  );
  int check_and_end_bulk_update(
    spider_bulk_upd_start bulk_upd_start
  );
  uint check_partitioned();
  void check_direct_order_limit();
  void check_distinct_key_query();
  bool is_sole_projection_field(
    uint16 field_index
  );
  int check_ha_range_eof();
  int drop_tmp_tables();
  bool handler_opened(
    int link_idx,
    uint tgt_conn_kind
  );
  void set_handler_opened(
    int link_idx
  );
  void clear_handler_opened(
    int link_idx,
    uint tgt_conn_kind
  );
  int close_opened_handler(
    int link_idx,
    bool release_conn
  );
  int index_handler_init();
  int rnd_handler_init();
  void set_error_mode();
  void backup_error_status();
  int check_error_mode(
    int error_num
  );
  int check_error_mode_eof(
    int error_num
  );
  int index_read_map_internal(
    uchar *buf,
    const uchar *key,
    key_part_map keypart_map,
    enum ha_rkey_function find_flag
  );
  int index_read_last_map_internal(
    uchar *buf,
    const uchar *key,
    key_part_map keypart_map
  );
  int index_first_internal(uchar *buf);
  int index_last_internal(uchar *buf);
  int read_range_first_internal(
    uchar *buf,
    const key_range *start_key,
    const key_range *end_key,
    bool eq_range,
    bool sorted
  );
  int ft_read_internal(uchar *buf);
  int rnd_next_internal(uchar *buf);
  void check_pre_call(
    bool use_parallel
  );
  void check_insert_dup_update_pushdown();
  void sync_from_clone_source_base(
    ha_spider *spider
  );
  void set_first_link_idx();
  void reset_first_link_idx();
  int reset_sql_sql(
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
  int append_insert_sql_part();
  int append_update_sql_part();
  int append_update_set_sql_part();
  int append_direct_update_set_sql_part();
  int append_dup_update_pushdown_sql_part(
    const char *alias,
    uint alias_length
  );
  int append_update_columns_sql_part(
    const char *alias,
    uint alias_length
  );
  int check_update_columns_sql_part();
  int append_delete_sql_part();
  int append_select_sql_part(
    ulong sql_type
  );
  int append_table_select_sql_part(
    ulong sql_type
  );
  int append_key_select_sql_part(
    ulong sql_type,
    uint idx
  );
  int append_minimum_select_sql_part(
    ulong sql_type
  );
  int append_from_sql_part(
    ulong sql_type
  );
  int append_hint_after_table_sql_part(
    ulong sql_type
  );
  void set_where_pos_sql(
    ulong sql_type
  );
  void set_where_to_pos_sql(
    ulong sql_type
  );
  int check_item_type_sql(
    Item *item
  );
  int append_values_connector_sql_part(
    ulong sql_type
  );
  int append_values_terminator_sql_part(
    ulong sql_type
  );
  int append_union_table_connector_sql_part(
    ulong sql_type
  );
  int append_union_table_terminator_sql_part(
    ulong sql_type
  );
  int append_key_column_values_sql_part(
    const key_range *start_key,
    ulong sql_type
  );
  int append_key_column_values_with_name_sql_part(
    const key_range *start_key,
    ulong sql_type
  );
  int append_key_where_sql_part(
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type
  );
  int append_match_where_sql_part(
    ulong sql_type
  );
  int append_condition_sql_part(
    const char *alias,
    uint alias_length,
    ulong sql_type,
    bool test_flg
  );
  int append_sum_select_sql_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  );
  int append_match_select_sql_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  );
  void set_order_pos_sql(
    ulong sql_type
  );
  void set_order_to_pos_sql(
    ulong sql_type
  );
  int append_group_by_sql_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
  int append_key_order_for_merge_with_alias_sql_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
  int append_key_order_for_direct_order_limit_with_alias_sql_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
  int append_key_order_with_alias_sql_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
  int append_limit_sql_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  );
  int reappend_limit_sql_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  );
  int append_insert_terminator_sql_part(
    ulong sql_type
  );
  int append_insert_values_sql_part(
    ulong sql_type
  );
  int append_into_sql_part(
    ulong sql_type
  );
  void set_insert_to_pos_sql(
    ulong sql_type
  );
  bool is_bulk_insert_exec_period(
    bool bulk_end
  );
  int append_select_lock_sql_part(
    ulong sql_type
  );
  int append_union_all_start_sql_part(
    ulong sql_type
  );
  int append_union_all_sql_part(
    ulong sql_type
  );
  int append_union_all_end_sql_part(
    ulong sql_type
  );
  int append_multi_range_cnt_sql_part(
    ulong sql_type,
    uint multi_range_cnt,
    bool with_comma
  );
  int append_multi_range_cnt_with_name_sql_part(
    ulong sql_type,
    uint multi_range_cnt
  );
  int append_delete_all_rows_sql_part(
    ulong sql_type
  );
  int append_update_sql(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    bool bulk
  );
  int append_delete_sql(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    bool bulk
  );
  bool sql_is_filled_up(
    ulong sql_type
  );
  bool sql_is_empty(
    ulong sql_type
  );
  bool support_multi_split_read_sql();
  bool support_bulk_update_sql();
  int bulk_tmp_table_insert();
  int bulk_tmp_table_end_bulk_insert();
  int bulk_tmp_table_rnd_init();
  int bulk_tmp_table_rnd_next();
  int bulk_tmp_table_rnd_end();
  int mk_bulk_tmp_table_and_bulk_start();
  void rm_bulk_tmp_table();
  bool bulk_tmp_table_created();
  int print_item_type(
    Item *item,
    spider_string *str,
    const char *alias,
    uint alias_length
  );
  bool support_use_handler_sql(
    int use_handler
  );
  int init_union_table_name_pos_sql();
  int set_union_table_name_pos_sql();
  int append_lock_tables_list();
  int lock_tables();
  int dml_init();
};
