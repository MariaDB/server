/* Copyright (C) 2009-201 Kentoku Shiba

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

#ifdef USE_PRAGMA_INTERFACE
#pragma interface
#endif

#define VP_TABLE_INFO_MAX_LEN 64
#define VP_MAX_KEY_LENGTH 16384

class ha_vp;
struct st_vp_ft_info
{
  struct _ft_vft *please;
  st_vp_ft_info *next;
  ha_vp *file;
  bool used_in_where;
  VP_CORRESPOND_KEY *target;
  FT_INFO *ft_handler;
  uint flags;
  uint inx;
  String *key;
};

typedef struct st_vp_condition
{
  COND                   *cond;
  st_vp_condition        *next;
} VP_CONDITION;

#ifdef HA_CAN_BULK_ACCESS
typedef struct st_vp_bulk_access_info
{
#ifdef WITH_PARTITION_STORAGE_ENGINE
  VP_PARTITION_HANDLER_SHARE *partition_handler_share;
  VP_CLONE_PARTITION_HANDLER_SHARE *clone_partition_handler_share;
#endif
  my_bitmap_map      *idx_init_read_bitmap;
  my_bitmap_map      *idx_init_write_bitmap;
  my_bitmap_map      *rnd_init_read_bitmap;
  my_bitmap_map      *rnd_init_write_bitmap;
  my_bitmap_map      *idx_read_bitmap;
  my_bitmap_map      *idx_write_bitmap;
  my_bitmap_map      *rnd_read_bitmap;
  my_bitmap_map      *rnd_write_bitmap;
  bool               idx_bitmap_init_flg;
  bool               rnd_bitmap_init_flg;
  bool               idx_bitmap_is_set;
  bool               rnd_bitmap_is_set;
  bool               child_keyread;
  bool               single_table;
  bool               set_used_table;
  bool               init_sel_key_init_bitmap;
  bool               init_sel_key_bitmap;
  bool               init_sel_rnd_bitmap;
  bool               init_ins_bitmap;
  uchar              **sel_key_init_child_bitmaps[2];
  uchar              **sel_key_child_bitmaps[2];
  uchar              **sel_rnd_child_bitmaps[2];
  uchar              **ins_child_bitmaps[2];
  uchar              *sel_key_init_use_tables;
  uchar              *sel_key_use_tables;
  uchar              *sel_rnd_use_tables;
  int                child_table_idx;
  int                child_key_idx;

  uint                   sequence_num;
  bool                   used;
  bool                   called;
  void                   **info;
  st_vp_bulk_access_info *next;
} VP_BULK_ACCESS_INFO;
#endif

class ha_vp: public handler
{
public:
  enum child_bitmap_state {
    CB_NO_SET, CB_SEL_KEY_INIT, CB_SEL_KEY, CB_SEL_RND,
    CB_INSERT, CB_UPDATE, CB_DELETE
  };

  THR_LOCK_DATA      lock;
  VP_SHARE           *share;
  String             *blob_buff;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  VP_PARTITION_HANDLER_SHARE *partition_handler_share;
  ha_vp              *pt_handler_share_creator;
  VP_CLONE_PARTITION_HANDLER_SHARE *clone_partition_handler_share;
#endif
  bool               is_clone;
  ha_vp              *pt_clone_source_handler;
  int                bitmap_map_size;
  my_bitmap_map      *idx_init_read_bitmap;
  my_bitmap_map      *idx_init_write_bitmap;
  my_bitmap_map      *rnd_init_read_bitmap;
  my_bitmap_map      *rnd_init_write_bitmap;
  my_bitmap_map      *idx_read_bitmap;
  my_bitmap_map      *idx_write_bitmap;
  my_bitmap_map      *rnd_read_bitmap;
  my_bitmap_map      *rnd_write_bitmap;
  bool               idx_bitmap_init_flg;
  bool               rnd_bitmap_init_flg;
  bool               idx_bitmap_is_set;
  bool               rnd_bitmap_is_set;

  uint               sql_command;
  int                lock_mode;
  enum thr_lock_type lock_type_sto;
  int                lock_type_ext;
  bool               rnd_scan;
  VP_CONDITION       *condition;
  int                store_error_num;
  bool               ft_inited;
  bool               ft_init_without_index_init;
  bool               ft_correspond_flag;
  uint               ft_init_idx;
  uint               ft_count;
  st_vp_ft_info      *ft_first;
  st_vp_ft_info      *ft_current;
  bool               use_pre_call;

  TABLE_LIST         *part_tables;
  uint               table_lock_count;
#if MYSQL_VERSION_ID < 50500
#else
  TABLE_LIST         *children_l;
  TABLE_LIST         **children_last_l;
  VP_CHILD_INFO      *children_info;
  bool               children_attached;
  bool               init_correspond_columns;
#endif
  uchar              *use_tables;
  uchar              *use_tables2;
  uchar              *use_tables3;
  uchar              *select_ignore;
  uchar              *select_ignore_with_lock;
  uchar              *update_ignore;
  uchar              *pruned_tables;
  uchar              *upd_target_tables;
  uchar              *work_bitmap;
  uchar              *work_bitmap2;
  uchar              *work_bitmap3;
  uchar              *work_bitmap4;
  bool               child_keyread;
  bool               extra_use_cmp_ref;
  bool               single_table;
  bool               update_request;
  bool               set_used_table;
  bool               bulk_insert;
  bool               init_sel_key_init_bitmap;
  bool               init_sel_key_bitmap;
  bool               init_sel_rnd_bitmap;
  bool               init_ins_bitmap;
  bool               init_upd_bitmap;
  bool               init_del_bitmap;
  bool               rnd_init_and_first;
  bool               pruned;
  bool               suppress_autoinc;
  uint               child_column_bitmap_size;
  uchar              **sel_key_init_child_bitmaps[2];
  uchar              **sel_key_child_bitmaps[2];
  uchar              **sel_rnd_child_bitmaps[2];
  uchar              **ins_child_bitmaps[2];
  uchar              **upd_child_bitmaps[2];
  uchar              **del_child_bitmaps[2];
  uchar              **add_from_child_bitmaps[2];
  uchar              **child_record0;
  uchar              **child_record1;
  uchar              *sel_key_init_use_tables;
  uchar              *sel_key_use_tables;
  uchar              *sel_rnd_use_tables;
  uchar              *key_inited_tables;
  uchar              *rnd_inited_tables;
  uchar              *ft_inited_tables;
  child_bitmap_state cb_state;
  int                child_table_idx;
  int                child_key_idx;
  uchar              *child_key;
  uint               child_key_length;
  uchar              child_key_different[MAX_KEY_LENGTH];
  uchar              child_end_key_different[MAX_KEY_LENGTH];
  key_range          child_start_key;
  key_range          child_end_key;
  KEY_MULTI_RANGE    *child_found_range;
  KEY_MULTI_RANGE    *child_multi_range_first;
#if defined(HAVE_HANDLERSOCKET)
  KEY_MULTI_RANGE    *child_multi_range;
  uchar              *child_key_buff;
#endif
  int                dup_table_idx;
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  Field              ***top_table_field_for_childs;
  uint               allocated_top_table_fields;
  bool               top_table_self;
#endif
#ifndef WITHOUT_VP_BG_ACCESS
  VP_BG_BASE         *bg_base;
#endif
  longlong           additional_table_flags;
  uint               *child_cond_count;
  uint               child_ref_length;

  uchar              *ref_buf;
  uint               ref_buf_length;

#ifdef HA_CAN_BULK_ACCESS
  bool                bulk_access_started;
  bool                bulk_access_executing;
  bool                bulk_access_pre_called;
  bool                need_bulk_access_finish;
  VP_BULK_ACCESS_INFO *bulk_access_info_first;
  VP_BULK_ACCESS_INFO *bulk_access_info_current;
  VP_BULK_ACCESS_INFO *bulk_access_info_exec_tgt;
  uchar               *bulk_access_exec_bitmap;
#endif

#ifdef VP_SUPPORT_MRR
  HANDLER_BUFFER *m_mrr_buffer;
  uint                         *m_mrr_buffer_size;
  uchar                        *m_mrr_full_buffer;
  uint                         m_mrr_full_buffer_size;
  uint                         m_mrr_new_full_buffer_size;
  uint                         *m_stock_range_seq;
  range_id_t                   *m_range_info;
  uint                         m_mrr_range_init_flags;
  uint                         m_mrr_range_length;
  VP_KEY_MULTI_RANGE           *m_mrr_range_first;
  VP_KEY_MULTI_RANGE           *m_mrr_range_current;
  uint                         *m_child_mrr_range_length;
  VP_CHILD_KEY_MULTI_RANGE     **m_child_mrr_range_first;
  VP_CHILD_KEY_MULTI_RANGE     **m_child_mrr_range_current;
  VP_CHILD_KEY_MULTI_RANGE_HLD *m_child_key_multi_range_hld;
  range_seq_t                  m_seq;
  RANGE_SEQ_IF                 *m_seq_if;
  RANGE_SEQ_IF                 m_child_seq_if;
#endif
#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
  bool handler_close;
#endif

  bool mr_init;
  MEM_ROOT mr;

  ha_vp();
  ha_vp(
    handlerton *hton,
    TABLE_SHARE *table_arg
  );
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
  int external_lock(
    THD *thd,
    int lock_type
  );
  uint lock_count() const;
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
  int reset();
  int extra(
    enum ha_extra_function operation
  );
  int extra_opt(
    enum ha_extra_function operation,
    ulong cachesize
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
#ifdef VP_HANDLER_HAS_HA_INDEX_READ_LAST_MAP
  int index_read_last_map(
    uchar *buf,
    const uchar *key,
    key_part_map keypart_map
  );
#endif
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
#ifdef VP_SUPPORT_MRR
  ha_rows multi_range_read_info_const(
    uint keyno,
    RANGE_SEQ_IF *seq,
    void *seq_init_param, 
    uint n_ranges,
    uint *bufsz,
    uint *mrr_mode,
    Cost_estimate *cost
  );
  ha_rows multi_range_read_info(
    uint keyno,
    uint n_ranges,
    uint keys,
    uint key_parts,
    uint *bufsz, 
    uint *mrr_mode,
    Cost_estimate *cost
  );
  int multi_range_read_init(
    RANGE_SEQ_IF *seq,
    void *seq_init_param,
    uint n_ranges,
    uint mrr_mode, 
    HANDLER_BUFFER *buf
  );
  int pre_multi_range_read_next(
    bool use_parallel
  );
  int multi_range_read_next(
    range_id_t *range_info
  );
  int multi_range_read_explain_info(
    uint mrr_mode,
    char *str,
    size_t size
  );
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
  int pre_index_read_map(
    const uchar *key,
    key_part_map keypart_map,
    enum ha_rkey_function find_flag,
    bool use_parallel
  );
  int pre_index_first(bool use_parallel);
  int pre_index_last(bool use_parallel);
#ifdef VP_HANDLER_HAS_HA_INDEX_READ_LAST_MAP
  int pre_index_read_last_map(
    const uchar *key,
    key_part_map keypart_map,
    bool use_parallel
  );
#endif
#ifdef VP_SUPPORT_MRR
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
  float ft_find_relevance(
    FT_INFO *handler,
    uchar *record,
    uint length
  );
  float ft_get_relevance(
    FT_INFO *handler
  );
  void ft_close_search(
    FT_INFO *handler
  );
  FT_INFO *ft_init_ext(
    uint flags,
    uint inx,
    String *key
  );
  int ft_init();
#ifdef HA_CAN_BULK_ACCESS
  int pre_ft_init();
#endif
  void ft_end();
#ifdef HA_CAN_BULK_ACCESS
  int pre_ft_end();
#endif
  int ft_read(
    uchar *buf
  );
  int info(
    uint flag
  );
  ha_rows records();
  ha_rows records_in_range(
    uint idx,
    key_range *start_key,
    key_range *end_key
  );
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
  int update_auto_increment();
  void set_next_insert_id(
    ulonglong id
  );
  void get_auto_increment(
    ulonglong offset,
    ulonglong increment,
    ulonglong nb_desired_values,
    ulonglong *first_value,
    ulonglong *nb_reserved_values
  );
  void restore_auto_increment(
    ulonglong prev_insert_id
  );
  void release_auto_increment();
  int reset_auto_increment(
    ulonglong value
  );
#ifdef VP_HANDLER_START_BULK_INSERT_HAS_FLAGS
  void start_bulk_insert(
    ha_rows rows,
    uint flags
  );
#else
  void start_bulk_insert(
    ha_rows rows
  );
#endif
  /* call from MySQL */
  int end_bulk_insert();
  /* call from MariaDB */
  int end_bulk_insert(
    bool abort
  );
  int write_row(
    uchar *buf
  );
#ifdef HA_CAN_BULK_ACCESS
  int pre_write_row(
    uchar *buf
  );
#endif
  bool start_bulk_update();
  int exec_bulk_update(
    ha_rows *dup_key_found
  );
#ifdef VP_END_BULK_UPDATE_RETURNS_INT
  int end_bulk_update();
#else
  void end_bulk_update();
#endif
#ifdef VP_UPDATE_ROW_HAS_CONST_NEW_DATA
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
#ifdef VP_MDEV_16246
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
#ifdef VP_MDEV_16246
  int direct_update_rows_init(
    List<Item> *update_fields
  );
#else
  int direct_update_rows_init();
#endif
#endif
#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  int pre_direct_update_rows_init(
    uint mode,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data
  );
#else
  int pre_direct_update_rows_init();
#endif
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  inline int direct_update_rows(ha_rows *update_rows)
  {
    return direct_update_rows(NULL, 0, FALSE, NULL, update_rows);
  }
  int direct_update_rows(
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data,
    ha_rows *update_rows
  );
#else
  int direct_update_rows(
    ha_rows *update_rows
  );
#endif
#ifdef HA_CAN_BULK_ACCESS
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
  int pre_direct_update_rows(
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    uchar *new_data,
    uint *update_rows
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
    uint *delete_rows
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
#ifdef VP_HANDLER_AUTO_REPAIR_HAS_ERROR
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
#ifdef HANDLER_HAS_TOP_TABLE_FIELDS
  int set_top_table_and_fields(
    TABLE *top_table,
    Field **top_table_field,
    uint top_table_fields,
    bool self
  );
  int set_top_table_and_fields(
    TABLE *top_table,
    Field **top_table_field,
    uint top_table_fields
  );
#ifdef HANDLER_HAS_PRUNE_PARTITIONS_FOR_CHILD
  bool prune_partitions_for_child(
    THD *thd,
    Item *pprune_cond
  );
#endif
#ifdef HANDLER_HAS_GET_NEXT_GLOBAL_FOR_CHILD
  TABLE_LIST *get_next_global_for_child();
#endif
  const COND *cond_push(
    const COND *cond
  );
  void cond_pop();
#endif
  int info_push(
    uint info_type,
    void *info
  );
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  void return_record_by_parent();
#endif
  int start_stmt(
    THD *thd,
    thr_lock_type lock_type
  );
  bool is_fatal_error(
    int error_num,
    uint flags
  );
  bool check_if_incompatible_data(
    HA_CREATE_INFO *create_info,
    uint table_changes
  );
  bool primary_key_is_clustered();
  bool can_switch_engines();
  VP_alter_table_operations alter_table_flags(
    VP_alter_table_operations flags
  );
#ifdef VP_HANDLER_HAS_ADD_INDEX
#if MYSQL_VERSION_ID < 50500 || MYSQL_VERSION_ID >= 50600
  int add_index(
    TABLE *table_arg,
    KEY *key_info,
    uint num_of_keys
  );
#else
  int add_index(
    TABLE *table_arg,
    KEY *key_info,
    uint num_of_keys,
    handler_add_index **add
  );
  int final_add_index(
    handler_add_index *add,
    bool commit
  );
#endif
#endif
#ifdef VP_HANDLER_HAS_DROP_INDEX
  int prepare_drop_index(
    TABLE *table_arg,
    uint *key_num,
    uint num_of_keys
  );
  int final_drop_index(
    TABLE *table_arg
  );
#endif
/*
  int check_for_upgrade(
    HA_CHECK_OPT *check_opt
  );
*/
  bool was_semi_consistent_read();
  void try_semi_consistent_read(
    bool yes
  );
  void unlock_row();
  void init_table_handle_for_HANDLER();
  void change_table_ptr(
    TABLE *table_arg,
    TABLE_SHARE *share_arg
  );
#if MYSQL_VERSION_ID < 50600
  char *get_tablespace_name(
    THD *thd,
    char *name,
    uint name_len
  );
#endif
  bool is_fk_defined_on_table_or_index(
    uint index
  );
  char *get_foreign_key_create_info();
  int get_foreign_key_list(
    THD *thd,
    List<FOREIGN_KEY_INFO> *f_key_list
  );
#if MYSQL_VERSION_ID < 50500
#else
  int get_parent_foreign_key_list(
    THD *thd,
    List<FOREIGN_KEY_INFO> *f_key_list
  );
#endif
  uint referenced_by_foreign_key();
  void free_foreign_key_create_info(
    char* str
  );
#ifdef VP_HANDLER_HAS_COUNT_QUERY_CACHE_DEPENDANT_TABLES
#ifdef VP_REGISTER_QUERY_CACHE_TABLE_HAS_CONST_TABLE_KEY
  my_bool register_query_cache_table(
    THD *thd,
    const char *table_key,
    uint key_length,
    qc_engine_callback *engine_callback,
    ulonglong *engine_data
  );
#else
  my_bool register_query_cache_table(
    THD *thd,
    char *table_key,
    uint key_length,
    qc_engine_callback *engine_callback,
    ulonglong *engine_data
  );
#endif
  uint count_query_cache_dependant_tables(
    uint8 *tables_type
  );
  my_bool register_query_cache_dependant_tables(
    THD *thd,
    Query_cache *cache,
    Query_cache_block_table **block,
    uint *n
  );
#else
#ifdef HTON_CAN_MERGE
  int qcache_insert(
    Query_cache *qcache,
    Query_cache_block_table *block_table,
    TABLE_COUNTER_TYPE &n
  );
  TABLE_COUNTER_TYPE qcache_table_count();
#endif
#endif

private:
  int choose_child_index(
    uint idx,
    uchar *read_set,
    uchar *write_set,
    int *table_idx,
    int *key_idx
  );
  int choose_child_ft_tables(
    uchar *read_set,
    uchar *write_set
  );
  int choose_child_tables(
    uchar *read_set,
    uchar *write_set
  );
  void clear_child_bitmap(
    int table_idx
  );
  uchar *create_child_key(
    const uchar *key_same,
    uchar *key_different,
    key_part_map keypart_map,
    uint key_length_same,
    uint *key_length
  );
  int get_child_record_by_idx(
    int table_idx,
    my_ptrdiff_t ptr_diff
  );
  int get_child_record_by_pk(
    my_ptrdiff_t ptr_diff
  );
  bool set_child_bitmap(
    uchar *bitmap,
    int table_idx,
    bool write_flg
  );
  bool add_pk_bitmap_to_child();
  void set_child_pt_bitmap();
  void set_child_record_for_update(
    my_ptrdiff_t ptr_diff,
    int record_idx,
    bool write_flg,
    bool use_table_chk
  );
  void set_child_record_for_insert(
    my_ptrdiff_t ptr_diff,
    int table_idx
  );
  int search_by_pk(
    int table_idx,
    int record_idx,
    VP_KEY_COPY *key_copy,
    my_ptrdiff_t ptr_diff,
    uchar **table_key
  );
  int search_by_pk_for_update(
    int table_idx,
    int record_idx,
    VP_KEY_COPY *vp_key_copy,
    my_ptrdiff_t ptr_diff,
    int bgu_mode
  );
  int create_child_bitmap_buff();
  void free_child_bitmap_buff();
  bool get_added_bitmap(
    uchar *added_bitmap,
    const uchar *current_bitmap,
    const uchar *pre_bitmap
  );
  void add_child_bitmap(
    int table_idx,
    uchar *bitmap
  );
  void prune_child_bitmap(
    int table_idx
  );
  void prune_child();
  int set_rnd_bitmap();
  void reset_rnd_bitmap();
  int set_rnd_bitmap_from_another(
    ha_vp *another_vp
  );
  int open_item_type(
    Item *item,
    int table_idx
  );
  int open_item_cond(
    Item_cond *item_cond,
    int table_idx
  );
  int open_item_func(
    Item_func *item_func,
    int table_idx
  );
  int open_item_ident(
    Item_ident *item_ident,
    int table_idx
  );
  int open_item_field(
    Item_field *item_field,
    int table_idx
  );
  int open_item_ref(
    Item_ref *item_ref,
    int table_idx
  );
  int open_item_row(
    Item_row *item_row,
    int table_idx
  );
  int count_condition(
    int table_idx
  );
  int create_bg_thread(
    VP_BG_BASE *base
  );
  void free_bg_thread(
    VP_BG_BASE *base
  );
  void bg_kick(
    VP_BG_BASE *base
  );
  void bg_wait(
    VP_BG_BASE *base
  );
  void init_select_column(
    bool rnd
  );
  void check_select_column(
    bool rnd
  );
  void clone_init_select_column();
  uint check_partitioned();
  void index_read_map_init(
    const uchar *key,
    key_part_map keypart_map,
    enum ha_rkey_function find_flag
  );
#ifdef VP_HANDLER_HAS_HA_INDEX_READ_LAST_MAP
  void index_read_last_map_init(
    const uchar *key,
    key_part_map keypart_map
  );
#endif
  void index_first_init();
  void index_last_init();
  void read_range_first_init(
    const key_range *start_key,
    const key_range *end_key,
    bool eq_range,
    bool sorted
  );
#ifdef VP_SUPPORT_MRR
  int multi_range_key_create_key(
    RANGE_SEQ_IF *seq,
    range_seq_t seq_it,
    int target_table_idx
  );
  int multi_range_read_next_first(
    range_id_t *range_info
  );
  int multi_range_read_next_next(
    range_id_t *range_info
  );
#else
  int read_multi_range_first_init(
    KEY_MULTI_RANGE **found_range_p,
    KEY_MULTI_RANGE *ranges,
    uint range_count,
    bool sorted,
    HANDLER_BUFFER *buffer
  );
#endif
  int rnd_next_init();
  int ft_read_init();
#ifdef HA_CAN_BULK_ACCESS
  VP_BULK_ACCESS_INFO *create_bulk_access_info();
  void delete_bulk_access_info(
    VP_BULK_ACCESS_INFO *bulk_access_info
  );
#endif
  TABLE_LIST *get_parent_table_list();
  st_select_lex *get_select_lex();
  JOIN *get_join();
#ifdef VP_HAS_EXPLAIN_QUERY
  Explain_select *get_explain_select();
#ifdef EXPLAIN_HAS_GET_UPD_DEL
  Explain_update *get_explain_upd_del();
#endif
#endif

public:
  void overwrite_index_bits();
#ifdef HANDLER_HAS_CHECK_AND_SET_BITMAP_FOR_UPDATE
  void check_and_set_bitmap_for_update(
    bool rnd
  );
#endif
};
