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

class ha_spider: public handler
{
public:
  THR_LOCK_DATA      lock;
  SPIDER_SHARE       *share;
  SPIDER_TRX         *trx;
  ulonglong          spider_thread_id;
  ulonglong          trx_conn_adjustment;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  ulonglong          trx_hs_r_conn_adjustment;
  ulonglong          trx_hs_w_conn_adjustment;
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char               **hs_r_conn_keys;
  SPIDER_CONN        **hs_r_conns;
  ulonglong          *hs_r_conn_ages;
  char               **hs_w_conn_keys;
  SPIDER_CONN        **hs_w_conns;
  ulonglong          *hs_w_conn_ages;
#endif
  /* for active-standby mode */
  uint               *conn_link_idx;
  uchar              *conn_can_fo;
  void               **quick_targets;
  int                *need_mons;
  query_id_t         search_link_query_id;
  int                search_link_idx;
  int                result_link_idx;
  SPIDER_RESULT_LIST result_list;
  SPIDER_CONDITION   *condition;
  spider_string      *blob_buff;
  uchar              *searched_bitmap;
  uchar              *ft_discard_bitmap;
  bool               position_bitmap_init;
  uchar              *position_bitmap;
  SPIDER_POSITION    *pushed_pos;
  SPIDER_POSITION    pushed_pos_buf;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  SPIDER_PARTITION_HANDLER_SHARE *partition_handler_share;
  ha_spider          *pt_handler_share_creator;
#endif
#ifdef HA_CAN_BULK_ACCESS
  int                pre_direct_init_result;
  bool               is_bulk_access_clone;
  bool               synced_from_clone_source;
  bool               bulk_access_started;
  bool               bulk_access_executing;
  bool               bulk_access_pre_called;
  SPIDER_BULK_ACCESS_LINK *bulk_access_link_first;
  SPIDER_BULK_ACCESS_LINK *bulk_access_link_current;
  SPIDER_BULK_ACCESS_LINK *bulk_access_link_exec_tgt;
/*
  bool               init_ha_mem_root;
  MEM_ROOT           ha_mem_root;
*/
  ulonglong          external_lock_cnt;
#endif
  bool               is_clone;
  bool               clone_bitmap_init;
  ha_spider          *pt_clone_source_handler;
  ha_spider          *pt_clone_last_searcher;
  bool               use_index_merge;

  bool               init_index_handler;
  bool               init_rnd_handler;

  bool               da_status;
  bool               use_spatial_index;

#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  uint                  idx_for_direct_join;
  bool                  use_fields;
  spider_fields         *fields;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *result_link_idx_chain;
#endif

  /* for mrr */
  bool               mrr_with_cnt;
  uint               multi_range_cnt;
  uint               multi_range_hit_point;
#ifdef HA_MRR_USE_DEFAULT_IMPL
  int                multi_range_num;
  bool               have_second_range;
  KEY_MULTI_RANGE    mrr_second_range;
  spider_string      *mrr_key_buff;
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
  range_id_t         *multi_range_keys;
#else
  char               **multi_range_keys;
#endif
#else
  KEY_MULTI_RANGE    *multi_range_ranges;
#endif

  char               *append_tblnm_alias;
  uint               append_tblnm_alias_length;

  ha_spider          *next;

  bool               rnd_scan_and_first;
  bool               quick_mode;
  bool               keyread;
  bool               ignore_dup_key;
  bool               write_can_replace;
  bool               insert_with_update;
  bool               low_priority;
  bool               high_priority;
  bool               insert_delayed;
  bool               use_pre_call;
  bool               use_pre_action;
  bool               pre_bitmap_checked;
  enum thr_lock_type lock_type;
  int                lock_mode;
  uint               sql_command;
  int                selupd_lock_mode;
  bool               bulk_insert;
#ifdef HANDLER_HAS_NEED_INFO_FOR_AUTO_INC
  bool               info_auto_called;
#endif
#ifdef HANDLER_HAS_CAN_USE_FOR_AUTO_INC_INIT
  bool               auto_inc_temporary;
#endif
  int                bulk_size;
  int                direct_dup_insert;
  int                store_error_num;
  uint               dup_key_idx;
  int                select_column_mode;
  bool               update_request;
  bool               pk_update;
  bool               force_auto_increment;
  int                bka_mode;
  bool               cond_check;
  int                cond_check_error;
  int                error_mode;
  ulonglong          store_last_insert_id;

  ulonglong          *db_request_id;
  uchar              *db_request_phase;
  uchar              *m_handler_opened;
  uint               *m_handler_id;
  char               **m_handler_cid;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uchar              *r_handler_opened;
  uint               *r_handler_id;
  uint               *r_handler_index;
  uchar              *w_handler_opened;
  uint               *w_handler_id;
  uint               *w_handler_index;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  uchar              *do_hs_direct_update;
  uint32             **hs_r_ret_fields;
  uint32             **hs_w_ret_fields;
  size_t             *hs_r_ret_fields_num;
  size_t             *hs_w_ret_fields_num;
  uint32             *hs_pushed_ret_fields;
  size_t             hs_pushed_ret_fields_num;
  size_t             hs_pushed_ret_fields_size;
  size_t             hs_pushed_lcl_fields_num;
  uchar              *tmp_column_bitmap;
  bool               hs_increment;
  bool               hs_decrement;
  uint32             hs_pushed_strref_num;
#endif
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  bool               do_direct_update;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  bool               maybe_do_hs_direct_update;
#endif
  uint               direct_update_kinds;
  List<Item>         *direct_update_fields;
  List<Item>         *direct_update_values;
#endif
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
  longlong           info_limit;
#endif
  spider_index_rnd_init prev_index_rnd_init;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  SPIDER_ITEM_HLD    *direct_aggregate_item_first;
  SPIDER_ITEM_HLD    *direct_aggregate_item_current;
#endif
  ha_rows            table_rows;
#ifdef HA_HAS_CHECKSUM_EXTENDED
  ha_checksum        checksum_val;
  bool               checksum_null;
  uint               action_flags;
#endif

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
  int check_access_kind(
    THD *thd,
    bool write_request
  );
#ifdef HA_CAN_BULK_ACCESS
  int additional_lock(
    THD *thd,
    enum thr_lock_type lock_type
  );
#endif
  THR_LOCK_DATA **store_lock(
    THD *thd,
    THR_LOCK_DATA **to,
    enum thr_lock_type lock_type
  );
  int external_lock(
    THD *thd,
    int lock_type
  );
  int reset();
  int extra(
    enum ha_extra_function operation
  );
  int index_init(
    uint idx,
    bool sorted
  );
#ifdef HA_CAN_BULK_ACCESS
  int pre_index_init(
    uint idx,
    bool sorted
  );
#endif
  int index_end();
#ifdef HA_CAN_BULK_ACCESS
  int pre_index_end();
#endif
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
#ifdef HA_MRR_USE_DEFAULT_IMPL
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
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
#else
  ha_rows multi_range_read_info_const(
    uint keyno,
    RANGE_SEQ_IF *seq,
    void *seq_init_param,
    uint n_ranges,
    uint *bufsz,
    uint *flags,
    COST_VECT *cost
  );
  ha_rows multi_range_read_info(
    uint keyno,
    uint n_ranges,
    uint keys,
    uint key_parts,
    uint *bufsz,
    uint *flags,
    COST_VECT *cost
  );
#endif
  int multi_range_read_init(
    RANGE_SEQ_IF *seq,
    void *seq_init_param,
    uint n_ranges,
    uint mode,
    HANDLER_BUFFER *buf
  );
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
  int multi_range_read_next(
    range_id_t *range_info
  );
  int multi_range_read_next_first(
    range_id_t *range_info
  );
  int multi_range_read_next_next(
    range_id_t *range_info
  );
#else
  int multi_range_read_next(
    char **range_info
  );
  int multi_range_read_next_first(
    char **range_info
  );
  int multi_range_read_next_next(
    char **range_info
  );
#endif
#else
  int read_multi_range_first(
    KEY_MULTI_RANGE **found_range_p,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    HANDLER_BUFFER *buffer
  );
  int read_multi_range_next(
    KEY_MULTI_RANGE **found_range_p
  );
#endif
  int rnd_init(
    bool scan
  );
#ifdef HA_CAN_BULK_ACCESS
  int pre_rnd_init(
    bool scan
  );
#endif
  int rnd_end();
#ifdef HA_CAN_BULK_ACCESS
  int pre_rnd_end();
#endif
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
#ifdef HA_MRR_USE_DEFAULT_IMPL
  int pre_multi_range_read_next(
    bool use_parallel
  );
#else
  int pre_read_multi_range_first(
    KEY_MULTI_RANGE **found_range_p,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    HANDLER_BUFFER *buffer,
    bool use_parallel
  );
#endif
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
    key_range *start_key,
    key_range *end_key
  );
  int check_crd();
  int pre_records();
  ha_rows records();
#ifdef HA_HAS_CHECKSUM_EXTENDED
  int pre_calculate_checksum();
  int calculate_checksum();
#endif
  const char *table_type() const;
  ulonglong table_flags() const;
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
#ifdef HANDLER_HAS_NEED_INFO_FOR_AUTO_INC
  bool need_info_for_auto_inc();
#endif
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
#ifdef SPIDER_HANDLER_START_BULK_INSERT_HAS_FLAGS
  void start_bulk_insert(
    ha_rows rows,
    uint flags
  );
#else
  void start_bulk_insert(
    ha_rows rows
  );
#endif
  int end_bulk_insert();
  int write_row(
    const uchar *buf
  );
#ifdef HA_CAN_BULK_ACCESS
  int pre_write_row(
    uchar *buf
  );
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  void direct_update_init(
    THD *thd,
    bool hs_request
  );
#endif
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
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
#ifdef SPIDER_MDEV_16246
  inline int direct_update_rows_init(
    List<Item> *update_fields
  ) {
    return direct_update_rows_init(update_fields, 2, NULL, 0, FALSE, NULL);
  }
  int direct_update_rows_init(
    List<Item> *update_fields,
    uint mode,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data
  );
#else
  inline int direct_update_rows_init()
  {
    return direct_update_rows_init(2, NULL, 0, FALSE, NULL);
  }
  int direct_update_rows_init(
    uint mode,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data
  );
#endif
#else
#ifdef SPIDER_MDEV_16246
  int direct_update_rows_init(
    List<Item> *update_fields
  );
#else
  int direct_update_rows_init();
#endif
#endif
#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
#ifdef SPIDER_MDEV_16246
  inline int pre_direct_update_rows_init(
    List<Item> *update_fields
  ) {
    return pre_direct_update_rows_init(update_fields, 2, NULL, 0, FALSE, NULL);
  }
  int pre_direct_update_rows_init(
    List<Item> *update_fields,
    uint mode,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data
  );
#else
  inline int pre_direct_update_rows_init()
  {
    return pre_direct_update_rows_init(2, NULL, 0, FALSE, NULL);
  }
  int pre_direct_update_rows_init(
    uint mode,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data
  );
#endif
#else
#ifdef SPIDER_MDEV_16246
  int pre_direct_update_rows_init(
    List<Item> *update_fields
  );
#else
  int pre_direct_update_rows_init();
#endif
#endif
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  inline int direct_update_rows(ha_rows *update_rows, ha_rows *found_rows)
  {
    return direct_update_rows(NULL, 0, FALSE, NULL, update_rows, found_rows);
  }
  int direct_update_rows(
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data,
    ha_rows *update_rows,
    ha_rows *found_row
  );
#else
  int direct_update_rows(
    ha_rows *update_rows,
    ha_rows *found_row
  );
#endif
#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  inline int pre_direct_update_rows()
  {
    ha_rows update_rows;
    ha_rows found_rows;

    return pre_direct_update_rows(NULL, 0, FALSE, NULL, &update_rows,
      &found_rows);
  }
  int pre_direct_update_rows(
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data,
    ha_rows *update_rows,
    ha_rows *found_row
  );
#else
  int pre_direct_update_rows();
#endif
#endif
#endif
  bool start_bulk_delete();
  int end_bulk_delete();
  int delete_row(
    const uchar *buf
  );
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  inline int direct_delete_rows_init()
  {
    return direct_delete_rows_init(2, NULL, 0, FALSE);
  }
  int direct_delete_rows_init(
    uint mode,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted
  );
#else
  int direct_delete_rows_init();
#endif
#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  inline int pre_direct_delete_rows_init()
  {
    return pre_direct_delete_rows_init(2, NULL, 0, FALSE);
  }
  int pre_direct_delete_rows_init(
    uint mode,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted
  );
#else
  int pre_direct_delete_rows_init();
#endif
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  inline int direct_delete_rows(ha_rows *delete_rows)
  {
    return direct_delete_rows(NULL, 0, FALSE, delete_rows);
  }
  int direct_delete_rows(
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    ha_rows *delete_rows
  );
#else
  int direct_delete_rows(
    ha_rows *delete_rows
  );
#endif
#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  inline int pre_direct_delete_rows()
  {
    ha_rows delete_rows;

    return pre_direct_delete_rows(NULL, 0, FALSE, &delete_rows);
  }
  int pre_direct_delete_rows(
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    ha_rows *delete_rows
  );
#else
  int pre_direct_delete_rows();
#endif
#endif
#endif
  int delete_all_rows();
  int truncate();
  double scan_time();
  double read_time(
    uint index,
    uint ranges,
    ha_rows rows
  );
#ifdef HA_CAN_BULK_ACCESS
  void bulk_req_exec();
#endif
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
  Field *get_top_table_field(
    uint16 field_index
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
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  void return_record_by_parent();
#endif
  TABLE *get_table();
  TABLE *get_top_table();
  void set_ft_discard_bitmap();
  void set_searched_bitmap();
  void set_clone_searched_bitmap();
  void set_searched_bitmap_from_item_list();
  void set_select_column_mode();
#ifdef WITH_PARTITION_STORAGE_ENGINE
  void check_select_column(bool rnd);
#endif
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
#ifdef HA_MRR_USE_DEFAULT_IMPL
#else
  int read_multi_range_first_internal(
    uchar *buf,
    KEY_MULTI_RANGE **found_range_p,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    HANDLER_BUFFER *buffer
  );
#endif
  int ft_read_internal(uchar *buf);
  int rnd_next_internal(uchar *buf);
  void check_pre_call(
    bool use_parallel
  );
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  void check_insert_dup_update_pushdown();
#endif
#ifdef HA_CAN_BULK_ACCESS
  SPIDER_BULK_ACCESS_LINK *create_bulk_access_link();
  void delete_bulk_access_link(
    SPIDER_BULK_ACCESS_LINK *bulk_access_link
  );
  int sync_from_clone_source(
    ha_spider *spider
  );
#endif
  void sync_from_clone_source_base(
    ha_spider *spider
  );
  void set_first_link_idx();
  void reset_first_link_idx();
  int reset_sql_sql(
    ulong sql_type
  );
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  int reset_hs_sql(
    ulong sql_type
  );
  int reset_hs_keys(
    ulong sql_type
  );
  int reset_hs_upds(
    ulong sql_type
  );
  int reset_hs_strs(
    ulong sql_type
  );
  int reset_hs_strs_pos(
    ulong sql_type
  );
  int push_back_hs_upds(
    SPIDER_HS_STRING_REF &info
  );
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  int append_increment_update_set_sql_part();
#endif
#endif
  int append_update_set_sql_part();
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  int append_direct_update_set_sql_part();
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  int append_direct_update_set_hs_part();
#endif
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  int append_dup_update_pushdown_sql_part(
    const char *alias,
    uint alias_length
  );
  int append_update_columns_sql_part(
    const char *alias,
    uint alias_length
  );
  int check_update_columns_sql_part();
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  int append_key_where_hs_part(
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type
  );
#endif
  int append_match_where_sql_part(
    ulong sql_type
  );
  int append_condition_sql_part(
    const char *alias,
    uint alias_length,
    ulong sql_type,
    bool test_flg
  );
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  int append_sum_select_sql_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  );
#endif
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
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  int append_group_by_sql_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  );
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  int append_limit_hs_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  );
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  int append_insert_values_hs_part(
    ulong sql_type
  );
#endif
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
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  bool support_bulk_access_hs() const;
#endif
  int init_union_table_name_pos_sql();
  int set_union_table_name_pos_sql();
};
