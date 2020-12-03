/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2013 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef HA_MROONGA_HPP_
#define HA_MROONGA_HPP_

#ifdef USE_PRAGMA_INTERFACE
#pragma interface
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <groonga.h>
#include "mrn_mysql_compat.h"
#include <mrn_operations.hpp>
#include <mrn_database.hpp>

#if __cplusplus >= 201402
#  define mrn_override override
#else
#  define mrn_override
#endif

#if (MYSQL_VERSION_ID >= 50514 && MYSQL_VERSION_ID < 50600)
#  define MRN_HANDLER_HAVE_FINAL_ADD_INDEX 1
#endif

#if (MYSQL_VERSION_ID >= 50603) || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_HA_RND_NEXT 1
#  define MRN_HANDLER_HAVE_HA_RND_POS 1
#  define MRN_HANDLER_HAVE_HA_INDEX_READ_MAP 1
#  define MRN_HANDLER_HAVE_HA_INDEX_READ_IDX_MAP 1
#  define MRN_HANDLER_HAVE_HA_INDEX_NEXT 1
#  define MRN_HANDLER_HAVE_HA_INDEX_PREV 1
#  define MRN_HANDLER_HAVE_HA_INDEX_FIRST 1
#  define MRN_HANDLER_HAVE_HA_INDEX_LAST 1
#  define MRN_HANDLER_HAVE_HA_INDEX_NEXT_SAME 1
#endif

#if (MYSQL_VERSION_ID >= 50604) || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_HA_CLOSE 1
#  define MRN_HANDLER_HAVE_MULTI_RANGE_READ 1
#endif

#if (MYSQL_VERSION_ID >= 50607)
#  define MRN_HANDLER_HAVE_CHECK_IF_SUPPORTED_INPLACE_ALTER 1
#  define MRN_HANDLER_HAVE_HA_PREPARE_INPLACE_ALTER_TABLE 1
#  define MRN_HANDLER_HAVE_HA_INPLACE_ALTER_TABLE 1
#  define MRN_HANDLER_HAVE_HA_COMMIT_INPLACE_ALTER_TABLE 1
#  define MRN_SUPPORT_FOREIGN_KEYS 1
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_INDEX_READ_LAST_MAP
#  if MYSQL_VERSION_ID >= 50611
#    define MRN_HANDLER_HAVE_HA_INDEX_READ_LAST_MAP
#  endif
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
#endif

#if MYSQL_VERSION_ID < 50600
#  define MRN_HANDLER_HAVE_GET_TABLESPACE_NAME
#endif

#if MYSQL_VERSION_ID >= 50607
#  define MRN_HANDLER_HAVE_SET_HA_SHARE_REF
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_BIG_TABLES
#elif defined(BIG_TABLES)
#  define MRN_BIG_TABLES
#endif

#ifdef MRN_BIG_TABLES
#  define MRN_HA_ROWS_FORMAT "llu"
#else
#  define MRN_HA_ROWS_FORMAT "lu"
#endif

#ifdef MRN_MARIADB_P
#  define MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_DETACH_CHILD
#  define MRN_HAVE_HA_EXTRA_PREPARE_FOR_FORCED_CLOSE
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80002)
#define MRN_HAVE_HA_EXTRA_SKIP_SERIALIZABLE_DD_VIEW
#define MRN_HAVE_HA_EXTRA_BEGIN_ALTER_COPY
#define MRN_HAVE_HA_EXTRA_END_ALTER_COPY
#define MRN_HAVE_HA_EXTRA_NO_AUTOINC_LOCKING
#endif

#if MYSQL_VERSION_ID >= 50607 && \
    (!defined(MRN_MARIADB_P) || MYSQL_VERSION_ID < 100008)
#  define MRN_HAVE_HA_EXTRA_EXPORT
#endif

#if MYSQL_VERSION_ID >= 50617 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_HA_EXTRA_SECONDARY_SORT_ROWID
#endif

#if MYSQL_VERSION_ID >= 50604 && !defined(MRN_MARIADB_P)
#  define MRN_TIMESTAMP_USE_TIMEVAL
#elif defined(MRN_MARIADB_P)
#  define MRN_TIMESTAMP_USE_MY_TIME_T
#else
#  define MRN_TIMESTAMP_USE_LONG
#endif

#if MYSQL_VERSION_ID < 50600 && !defined(MRN_MARIADB_P)
#  define MRN_FIELD_STORE_TIME_NEED_TYPE
#endif

#if MYSQL_VERSION_ID < 50706 || defined(MRN_MARIADB_P)
#  define MRN_HAVE_TL_WRITE_DELAYED
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_TL_WRITE_CONCURRENT_DEFAULT
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_AUTO_REPAIR_HAVE_ERROR
#endif

#if MYSQL_VERSION_ID >= 50604
#  define MRN_JOIN_TAB_HAVE_CONDITION
#endif

#if MYSQL_VERSION_ID < 50600
#  define MRN_RBR_UPDATE_NEED_ALL_COLUMNS
#endif

#if MYSQL_VERSION_ID >= 50500
#  define MRN_ROW_BASED_CHECK_IS_METHOD
#endif

#if MYSQL_VERSION_ID >= 50600
#  define MRN_HAVE_HA_REBIND_PSI
#endif

#if MYSQL_VERSION_ID >= 50612 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_POINT_XY
#endif

#if (defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100000)
#  define MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
#endif

#if (defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100010)
#  define MRN_HAVE_TDC_LOCK_TABLE_SHARE
#  if MYSQL_VERSION_ID >= 100100
#    define MRN_TABLE_SHARE_TDC_IS_POINTER
#  endif
#endif

#ifdef MRN_MARIADB_P
#  if MYSQL_VERSION_ID >= 50542 && MYSQL_VERSION_ID < 100000
#    define MRN_SUPPORT_THDVAR_SET
#  elif MYSQL_VERSION_ID >= 100017
#    define MRN_SUPPORT_THDVAR_SET
#  endif
#else
#  define MRN_SUPPORT_THDVAR_SET
#endif

#ifdef MRN_MARIADB_P
#  if MYSQL_VERSION_ID < 100000
#    define MRN_SUPPORT_PARTITION
#  endif
#else
#  define MRN_SUPPORT_PARTITION
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_FLUSH_LOGS_HAVE_BINLOG_GROUP_FLUSH
#endif

#if MYSQL_VERSION_ID < 50706 || defined(MRN_MARIADB_P)
#  define MRN_HAVE_HTON_ALTER_TABLE_FLAGS
#endif

#if MYSQL_VERSION_ID >= 50706
#  define MRN_FOREIGN_KEY_USE_CONST_STRING
#endif

#if MYSQL_VERSION_ID < 50706 || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
#endif

#if MYSQL_VERSION_ID < 50706 || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_RESET_AUTO_INCREMENT
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 50709) ||   \
  (defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100203)
#  define MRN_ALTER_INPLACE_INFO_ALTER_STORED_COLUMN_TYPE \
  ALTER_STORED_COLUMN_TYPE
#  define MRN_ALTER_INPLACE_INFO_ALTER_STORED_COLUMN_ORDER \
  ALTER_STORED_COLUMN_ORDER
#else
#  define MRN_ALTER_INPLACE_INFO_ALTER_STORED_COLUMN_TYPE \
  Alter_inplace_info::ALTER_COLUMN_TYPE
#  define MRN_ALTER_INPLACE_INFO_ALTER_STORED_COLUMN_ORDER \
  Alter_inplace_info::ALTER_COLUMN_ORDER
#endif

#if MYSQL_VERSION_ID >= 50700 && !defined(MRN_MARIADB_P)
#  define MRN_HANDLER_RECORDS_RETURN_ERROR
#endif

#if MYSQL_VERSION_ID < 80002 || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_KEYS_TO_USE_FOR_SCANNING
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80002)
#  define MRN_ST_MYSQL_PLUGIN_HAVE_CHECK_UNINSTALL
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80002)
#  define MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
#  define MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80002)
#  define MRN_HANDLERTON_CREATE_HAVE_PARTITIONED
#endif

#if defined(HAVE_PSI_INTERFACE) &&                      \
  (MYSQL_VERSION_ID < 80002 || defined(MRN_MARIADB_P))
#  define MRN_HAVE_PSI_SERVER
#endif

class ha_mroonga;

/* structs */
struct st_mrn_ft_info
{
  struct _ft_vft *please;
#ifdef HA_CAN_FULLTEXT_EXT
  struct _ft_vft_ext *could_you;
#endif
  grn_ctx *ctx;
  grn_encoding encoding;
  grn_obj *table;
  grn_obj *result;
  grn_obj *score_column;
  grn_obj key;
  grn_obj score;
  uint active_index;
  KEY *key_info;
  KEY *primary_key_info;
  grn_obj *cursor;
  grn_obj *id_accessor;
  grn_obj *key_accessor;
  ha_mroonga *mroonga;
};

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
struct ha_field_option_struct
{
  const char *groonga_type;
  const char *flags;
};

struct ha_index_option_struct
{
  const char *tokenizer;
  const char *normalizer;
  const char *token_filters;
  const char *flags;
};
#endif

/* handler class */
class ha_mroonga: public handler
{
public:
  handler   *wrap_handler;
  bool      is_clone;
  ha_mroonga *parent_for_clone;
  MEM_ROOT  *mem_root_for_clone;
  grn_obj   key_buffer;
  grn_id    record_id;
  grn_id    *key_id;
  grn_id    *del_key_id;
  MY_BITMAP multiple_column_key_bitmap;

private:
  THR_LOCK_DATA thr_lock_data;

  // for wrapper mode (TODO: need to be confirmed)
  uint      wrap_ft_init_count;
  MRN_SHARE *share;
  KEY       *wrap_key_info;
  KEY       *base_key_info;
  key_part_map pk_keypart_map;
  MEM_ROOT  mem_root;
  /// for create table and alter table
  mutable bool        analyzed_for_create;
  mutable TABLE       table_for_create;
  mutable MRN_SHARE   share_for_create;
  mutable TABLE_SHARE table_share_for_create;
  mutable MEM_ROOT    mem_root_for_create;
  mutable handler     *wrap_handler_for_create;
#ifdef MRN_HANDLER_HAVE_FINAL_ADD_INDEX
  handler_add_index *hnd_add_index;
#endif
#ifdef MRN_HANDLER_HAVE_CHECK_IF_SUPPORTED_INPLACE_ALTER
  alter_table_operations alter_handler_flags;
  KEY         *alter_key_info_buffer;
  uint        alter_key_count;
  uint        alter_index_drop_count;
  KEY         *alter_index_drop_buffer;
  uint        alter_index_add_count;
  uint        *alter_index_add_buffer;
  TABLE       *wrap_altered_table;
  KEY         *wrap_altered_table_key_info;
  TABLE_SHARE *wrap_altered_table_share;
  KEY         *wrap_altered_table_share_key_info;
#else
  KEY         *wrap_alter_key_info;
#endif
  int mrn_lock_type;

  // for groonga objects
  grn_ctx ctx_entity_;
  grn_ctx *ctx;
  grn_obj *grn_table;
  grn_obj **grn_columns;
  grn_obj **grn_column_ranges;
  grn_obj **grn_index_tables;
  grn_obj **grn_index_columns;

  // buffers
  grn_obj  encoded_key_buffer;
  grn_obj  old_value_buffer;
  grn_obj  new_value_buffer;
  grn_obj top_left_point;
  grn_obj bottom_right_point;
  grn_obj source_point;
  double top_left_longitude_in_degree;
  double bottom_right_longitude_in_degree;
  double bottom_right_latitude_in_degree;
  double top_left_latitude_in_degree;

  // for search
  grn_obj *grn_source_column_geo;
  grn_obj *cursor_geo;
  grn_table_cursor *cursor;
  grn_table_cursor *index_table_cursor;
  grn_obj *empty_value_records;
  grn_table_cursor *empty_value_records_cursor;
  grn_obj *sorted_result;
  grn_obj *matched_record_keys;
  String  *blob_buffers;

  // for error report
  uint dup_key;

  // for optimization
  bool count_skip;
  bool fast_order_limit;
  bool fast_order_limit_with_index;

  // for context
  bool ignoring_duplicated_key;
  bool inserting_with_update;
  bool fulltext_searching;
  bool ignoring_no_key_columns;
  bool replacing_;
  uint written_by_row_based_binlog;

  // for ft in where clause test
  Item_func_match *current_ft_item;

  mrn::Operations *operations_;

public:
  ha_mroonga(handlerton *hton, TABLE_SHARE *share_arg);
  ~ha_mroonga();
  const char *table_type() const;           // required
  const char *index_type(uint inx) mrn_override;
  const char **bas_ext() const;                                    // required

  ulonglong table_flags() const mrn_override;                                   // required
  ulong index_flags(uint idx, uint part, bool all_parts) const mrn_override;    // required

  // required
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info
#ifdef MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
             ,
             dd::Table *table_def
#endif
    ) mrn_override;
  // required
  int open(const char *name, int mode, uint open_options
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
           ,
           const dd::Table *table_def
#endif
    ) mrn_override;
#ifndef MRN_HANDLER_HAVE_HA_CLOSE
  int close();                                                     // required
#endif
  int info(uint flag) mrn_override;                                             // required

  uint lock_count() const mrn_override;
  THR_LOCK_DATA **store_lock(THD *thd,                             // required
                             THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) mrn_override;
  int external_lock(THD *thd, int lock_type) mrn_override;

  int rnd_init(bool scan) mrn_override;                                         // required
  int rnd_end() mrn_override;
#ifndef MRN_HANDLER_HAVE_HA_RND_NEXT
  int rnd_next(uchar *buf);                                        // required
#endif
#ifndef MRN_HANDLER_HAVE_HA_RND_POS
  int rnd_pos(uchar *buf, uchar *pos);                             // required
#endif
  void position(const uchar *record) mrn_override;                              // required
  int extra(enum ha_extra_function operation) mrn_override;
  int extra_opt(enum ha_extra_function operation, ulong cache_size) mrn_override;

  int delete_table(const char *name) mrn_override;
  int write_row(const uchar *buf) mrn_override;
  int update_row(const uchar *old_data, const uchar *new_data) mrn_override;
  int delete_row(const uchar *buf) mrn_override;

  uint max_supported_record_length()   const mrn_override;
  uint max_supported_keys()            const mrn_override;
  uint max_supported_key_parts()       const mrn_override;
  uint max_supported_key_length()      const mrn_override;
  uint max_supported_key_part_length() const mrn_override;

  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key, page_range *pages) mrn_override;
  int index_init(uint idx, bool sorted) mrn_override;
  int index_end() mrn_override;
#ifndef MRN_HANDLER_HAVE_HA_INDEX_READ_MAP
  int index_read_map(uchar * buf, const uchar * key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag);
#endif
#ifdef MRN_HANDLER_HAVE_INDEX_READ_LAST_MAP
  int index_read_last_map(uchar *buf, const uchar *key,
                          key_part_map keypart_map);
#endif
#ifndef MRN_HANDLER_HAVE_HA_INDEX_NEXT
  int index_next(uchar *buf);
#endif
#ifndef MRN_HANDLER_HAVE_HA_INDEX_PREV
  int index_prev(uchar *buf);
#endif
#ifndef MRN_HANDLER_HAVE_HA_INDEX_FIRST
  int index_first(uchar *buf);
#endif
#ifndef MRN_HANDLER_HAVE_HA_INDEX_LAST
  int index_last(uchar *buf);
#endif
  int index_next_same(uchar *buf, const uchar *key, uint keylen) mrn_override;

  int ft_init() mrn_override;
  FT_INFO *ft_init_ext(uint flags, uint inx, String *key) mrn_override;
  int ft_read(uchar *buf) mrn_override;

  const Item *cond_push(const Item *cond) mrn_override;
  void cond_pop() mrn_override;

  bool get_error_message(int error, String *buf) mrn_override;

  int reset() mrn_override;

  handler *clone(const char *name, MEM_ROOT *mem_root) mrn_override;
  uint8 table_cache_type() mrn_override;
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param,
                                      uint n_ranges, uint *bufsz,
                                      uint *flags, Cost_estimate *cost) mrn_override;
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                uint key_parts,
#endif
                                uint *bufsz, uint *flags, Cost_estimate *cost) mrn_override;
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode,
                            HANDLER_BUFFER *buf) mrn_override;
  int multi_range_read_next(range_id_t *range_info) mrn_override;
#else // MRN_HANDLER_HAVE_MULTI_RANGE_READ
  int read_multi_range_first(KEY_MULTI_RANGE **found_range_p,
                             KEY_MULTI_RANGE *ranges,
                             uint range_count,
                             bool sorted,
                             HANDLER_BUFFER *buffer);
  int read_multi_range_next(KEY_MULTI_RANGE **found_range_p);
#endif // MRN_HANDLER_HAVE_MULTI_RANGE_READ
#ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
  void start_bulk_insert(ha_rows rows, uint flags) mrn_override;
#else
  void start_bulk_insert(ha_rows rows);
#endif
  int end_bulk_insert() mrn_override;
  int delete_all_rows() mrn_override;
  int truncate() mrn_override;
  double scan_time() mrn_override;
  double read_time(uint index, uint ranges, ha_rows rows) mrn_override;
#ifdef MRN_HANDLER_HAVE_KEYS_TO_USE_FOR_SCANNING
  const key_map *keys_to_use_for_scanning() mrn_override;
#endif
  ha_rows estimate_rows_upper_bound() mrn_override;
  void update_create_info(HA_CREATE_INFO* create_info) mrn_override;
  int rename_table(const char *from, const char *to) mrn_override;
  bool is_crashed() const mrn_override;
  bool auto_repair(int error) const mrn_override;
  bool auto_repair() const;
  int disable_indexes(uint mode) mrn_override;
  int enable_indexes(uint mode) mrn_override;
  int check(THD* thd, HA_CHECK_OPT* check_opt) mrn_override;
  int repair(THD* thd, HA_CHECK_OPT* check_opt) mrn_override;
  bool check_and_repair(THD *thd) mrn_override;
  int analyze(THD* thd, HA_CHECK_OPT* check_opt) mrn_override;
  int optimize(THD* thd, HA_CHECK_OPT* check_opt) mrn_override;
  bool is_fatal_error(int error_num, uint flags=0) mrn_override;
  bool check_if_incompatible_data(HA_CREATE_INFO *create_info,
                                  uint table_changes) mrn_override;
#ifdef MRN_HANDLER_HAVE_CHECK_IF_SUPPORTED_INPLACE_ALTER
  enum_alter_inplace_result
  check_if_supported_inplace_alter(TABLE *altered_table,
                                   Alter_inplace_info *ha_alter_info) mrn_override;
#else
  alter_table_operations alter_table_flags(alter_table_operations flags);
#  ifdef MRN_HANDLER_HAVE_FINAL_ADD_INDEX
  int add_index(TABLE *table_arg, KEY *key_info, uint num_of_keys,
                handler_add_index **add);
  int final_add_index(handler_add_index *add, bool commit);
#  else
  int add_index(TABLE *table_arg, KEY *key_info, uint num_of_keys);
#  endif
  int prepare_drop_index(TABLE *table_arg, uint *key_num, uint num_of_keys);
  int final_drop_index(TABLE *table_arg);
#endif
  int update_auto_increment();
  void set_next_insert_id(ulonglong id);
  void get_auto_increment(ulonglong offset, ulonglong increment, ulonglong nb_desired_values,
                          ulonglong *first_value, ulonglong *nb_reserved_values) mrn_override;
  void restore_auto_increment(ulonglong prev_insert_id) mrn_override;
  void release_auto_increment() mrn_override;
  int check_for_upgrade(HA_CHECK_OPT *check_opt) mrn_override;
#ifdef MRN_HANDLER_HAVE_RESET_AUTO_INCREMENT
  int reset_auto_increment(ulonglong value) mrn_override;
#endif
  bool was_semi_consistent_read() mrn_override;
  void try_semi_consistent_read(bool yes) mrn_override;
  void unlock_row() mrn_override;
  int start_stmt(THD *thd, thr_lock_type lock_type) mrn_override;

protected:
#ifdef MRN_HANDLER_RECORDS_RETURN_ERROR
  int records(ha_rows *num_rows);
#else
  ha_rows records() mrn_override;
#endif
#ifdef MRN_HANDLER_HAVE_HA_RND_NEXT
  int rnd_next(uchar *buf) mrn_override;
#endif
#ifdef MRN_HANDLER_HAVE_HA_RND_POS
  int rnd_pos(uchar *buf, uchar *pos) mrn_override;
#endif
#ifdef MRN_HANDLER_HAVE_HA_INDEX_READ_MAP
  int index_read_map(uchar *buf, const uchar *key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag) mrn_override;
#endif
#ifdef MRN_HANDLER_HAVE_HA_INDEX_NEXT
  int index_next(uchar *buf) mrn_override;
#endif
#ifdef MRN_HANDLER_HAVE_HA_INDEX_PREV
  int index_prev(uchar *buf) mrn_override;
#endif
#ifdef MRN_HANDLER_HAVE_HA_INDEX_FIRST
  int index_first(uchar *buf) mrn_override;
#endif
#ifdef MRN_HANDLER_HAVE_HA_INDEX_LAST
  int index_last(uchar *buf) mrn_override;
#endif
  void change_table_ptr(TABLE *table_arg, TABLE_SHARE *share_arg) mrn_override;
  bool is_fk_defined_on_table_or_index(uint index) mrn_override;
  char *get_foreign_key_create_info() mrn_override;
#ifdef MRN_HANDLER_HAVE_GET_TABLESPACE_NAME
  char *get_tablespace_name(THD *thd, char *name, uint name_len);
#endif
  bool can_switch_engines() mrn_override;
  int get_foreign_key_list(THD *thd, List<FOREIGN_KEY_INFO> *f_key_list) mrn_override;
  int get_parent_foreign_key_list(THD *thd, List<FOREIGN_KEY_INFO> *f_key_list) mrn_override;
  uint referenced_by_foreign_key() mrn_override;
  void init_table_handle_for_HANDLER() mrn_override;
  void free_foreign_key_create_info(char* str) mrn_override;
#ifdef MRN_HAVE_HA_REBIND_PSI
  void unbind_psi() mrn_override;
  void rebind_psi() mrn_override;
#endif
  my_bool register_query_cache_table(THD *thd,
                                     const char *table_key,
                                     uint key_length,
                                     qc_engine_callback *engine_callback,
                                     ulonglong *engine_data) mrn_override;
#ifdef MRN_HANDLER_HAVE_CHECK_IF_SUPPORTED_INPLACE_ALTER
  bool prepare_inplace_alter_table(TABLE *altered_table,
                                   Alter_inplace_info *ha_alter_info) mrn_override;
  bool inplace_alter_table(TABLE *altered_table,
                           Alter_inplace_info *ha_alter_info) mrn_override;
  bool commit_inplace_alter_table(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info,
                                  bool commit) mrn_override;
#endif

private:
  void mkdir_p(const char *directory);
  ulonglong file_size(const char *path);

  bool have_unique_index();

  bool is_foreign_key_field(const char *table_name,
                            const char *field_name);

  void push_warning_unsupported_spatial_index_search(enum ha_rkey_function flag);
  void clear_cursor();
  void clear_cursor_geo();
  void clear_empty_value_records();
  void clear_search_result();
  void clear_search_result_geo();
  void clear_indexes();
  int add_wrap_hton(const char *path, handlerton *wrap_handlerton);
  void remove_related_files(const char *base_path);
  void remove_grn_obj_force(const char *name);
  int drop_index(MRN_SHARE *target_share, uint key_index);
  int drop_indexes_normal(const char *table_name, grn_obj *table);
  int drop_indexes_multiple(const char *table_name, grn_obj *table,
                            const char *index_table_name_separator);
  int drop_indexes(const char *table_name);
  bool find_column_flags(Field *field, MRN_SHARE *mrn_share, int i,
                         grn_obj_flags *column_flags);
  grn_obj *find_column_type(Field *field, MRN_SHARE *mrn_share, int i,
                            int error_code);
  grn_obj *find_tokenizer(KEY *key, MRN_SHARE *mrn_share, int i);
  grn_obj *find_tokenizer(const char *name, int name_length);
  bool have_custom_normalizer(KEY *key) const;
  grn_obj *find_normalizer(KEY *key);
  grn_obj *find_normalizer(KEY *key, const char *name);
  bool find_index_column_flags(KEY *key, grn_column_flags *index_column_flags);
  bool find_token_filters(KEY *key, grn_obj *token_filters);
  bool find_token_filters_put(grn_obj *token_filters,
                              const char *token_filter_name,
                              int token_filter_name_length);
  bool find_token_filters_fill(grn_obj *token_filters,
                               const char *token_filter_names,
                               int token_filter_name_length);
  int wrapper_get_record(uchar *buf, const uchar *key);
  int wrapper_get_next_geo_record(uchar *buf);
  int storage_get_next_record(uchar *buf);
  void geo_store_rectangle(const uchar *rectangle);
  int generic_geo_open_cursor(const uchar *key, enum ha_rkey_function find_flag);

#ifdef MRN_HANDLER_HAVE_HA_CLOSE
  int close() mrn_override;
#endif
  bool is_dry_write();
  bool is_enable_optimization();
  bool should_normalize(Field *field) const;
  void check_count_skip(key_part_map target_key_part_map);
  bool is_grn_zero_column_value(grn_obj *column, grn_obj *value);
  bool is_primary_key_field(Field *field) const;
  void check_fast_order_limit(grn_table_sort_key **sort_keys, int *n_sort_keys,
                              longlong *limit);

  long long int get_grn_time_from_timestamp_field(Field_timestamp *field);

  int generic_store_bulk_fixed_size_string(Field *field, grn_obj *buf);
  int generic_store_bulk_variable_size_string(Field *field, grn_obj *buf);
  int generic_store_bulk_integer(Field *field, grn_obj *buf);
  int generic_store_bulk_unsigned_integer(Field *field, grn_obj *buf);
  int generic_store_bulk_float(Field *field, grn_obj *buf);
  int generic_store_bulk_timestamp(Field *field, grn_obj *buf);
  int generic_store_bulk_date(Field *field, grn_obj *buf);
  int generic_store_bulk_time(Field *field, grn_obj *buf);
  int generic_store_bulk_datetime(Field *field, grn_obj *buf);
  int generic_store_bulk_year(Field *field, grn_obj *buf);
  int generic_store_bulk_new_date(Field *field, grn_obj *buf);
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
  int generic_store_bulk_datetime2(Field *field, grn_obj *buf);
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_TIME2
  int generic_store_bulk_time2(Field *field, grn_obj *buf);
#endif
  int generic_store_bulk_new_decimal(Field *field, grn_obj *buf);
  int generic_store_bulk_blob(Field *field, grn_obj *buf);
  int generic_store_bulk_geometry(Field *field, grn_obj *buf);
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
  int generic_store_bulk_json(Field *field, grn_obj *buf);
#endif
  int generic_store_bulk(Field *field, grn_obj *buf);

  void storage_store_field_string(Field *field,
                                  const char *value, uint value_length);
  void storage_store_field_integer(Field *field,
                                   const char *value, uint value_length);
  void storage_store_field_unsigned_integer(Field *field,
                                            const char *value,
                                            uint value_length);
  void storage_store_field_float(Field *field,
                                 const char *value, uint value_length);
  void storage_store_field_timestamp(Field *field,
                                     const char *value, uint value_length);
  void storage_store_field_date(Field *field,
                                const char *value, uint value_length);
  void storage_store_field_time(Field *field,
                                const char *value, uint value_length);
  void storage_store_field_datetime(Field *field,
                                    const char *value, uint value_length);
  void storage_store_field_year(Field *field,
                                const char *value, uint value_length);
  void storage_store_field_new_date(Field *field,
                                    const char *value, uint value_length);
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
  void storage_store_field_datetime2(Field *field,
                                     const char *value, uint value_length);
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_TIME2
  void storage_store_field_time2(Field *field,
                                 const char *value, uint value_length);
#endif
  void storage_store_field_blob(Field *field,
                                const char *value, uint value_length);
  void storage_store_field_geometry(Field *field,
                                    const char *value, uint value_length);
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
  void storage_store_field_json(Field *field,
                                const char *value, uint value_length);
#endif
  void storage_store_field(Field *field, const char *value, uint value_length);
  void storage_store_field_column(Field *field, bool is_primary_key,
                                  int nth_column, grn_id record_id);
  void storage_store_fields(uchar *buf, grn_id record_id);
  void storage_store_fields_for_prep_update(const uchar *old_data,
                                            const uchar *new_data,
                                            grn_id record_id);
  void storage_store_fields_by_index(uchar *buf);

  int storage_encode_key_normalize_min_sort_chars(Field *field,
                                                  uchar *buf,
                                                  uint size);
  int storage_encode_key_fixed_size_string(Field *field, const uchar *key,
                                           uchar *buf, uint *size);
  int storage_encode_key_variable_size_string(Field *field, const uchar *key,
                                              uchar *buf, uint *size);
  int storage_encode_key_timestamp(Field *field, const uchar *key,
                                   uchar *buf, uint *size);
  int storage_encode_key_time(Field *field, const uchar *key,
                              uchar *buf, uint *size);
  int storage_encode_key_year(Field *field, const uchar *key,
                              uchar *buf, uint *size);
  int storage_encode_key_datetime(Field *field, const uchar *key,
                                  uchar *buf, uint *size);
#ifdef MRN_HAVE_MYSQL_TYPE_TIMESTAMP2
  int storage_encode_key_timestamp2(Field *field, const uchar *key,
                                    uchar *buf, uint *size);
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
  int storage_encode_key_datetime2(Field *field, bool is_null, const uchar *key,
                                   uchar *buf, uint *size);
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_TIME2
  int storage_encode_key_time2(Field *field, const uchar *key,
                               uchar *buf, uint *size);
#endif
  int storage_encode_key_enum(Field *field, const uchar *key,
                              uchar *buf, uint *size);
  int storage_encode_key_set(Field *field, const uchar *key,
                             uchar *buf, uint *size);
  int storage_encode_key(Field *field, const uchar *key, uchar *buf, uint *size);
  int storage_encode_multiple_column_key(KEY *key_info,
                                         const uchar *key, uint key_length,
                                         uchar *buffer, uint *encoded_length);
  int storage_encode_multiple_column_key_range(KEY *key_info,
                                               const uchar *start,
                                               uint start_size,
                                               const uchar *end,
                                               uint end_size,
                                               uchar *min_buffer,
                                               uint *min_encoded_size,
                                               uchar *max_buffer,
                                               uint *max_encoded_size);
  int storage_encode_multiple_column_key_range(KEY *key_info,
                                               const key_range *start,
                                               const key_range *end,
                                               uchar *min_buffer,
                                               uint *min_encoded_size,
                                               uchar *max_buffer,
                                               uint *max_encoded_size);

  void set_pk_bitmap();
  int create_share_for_create() const;
  int wrapper_create(const char *name, TABLE *table,
                     HA_CREATE_INFO *info, MRN_SHARE *tmp_share);
  int storage_create(const char *name, TABLE *table,
                     HA_CREATE_INFO *info, MRN_SHARE *tmp_share);
  int wrapper_create_index_fulltext_validate(KEY *key_info);
  int wrapper_create_index_fulltext(const char *grn_table_name,
                                    int i,
                                    KEY *key_info,
                                    grn_obj **index_tables,
                                    grn_obj **index_columns,
                                    MRN_SHARE *tmp_share);
  int wrapper_create_index_geo(const char *grn_table_name,
                               int i,
                               KEY *key_info,
                               grn_obj **index_tables,
                               grn_obj **index_columns,
                               MRN_SHARE *tmp_share);
  int wrapper_create_index(const char *name, TABLE *table, MRN_SHARE *tmp_share);
  int storage_create_validate_pseudo_column(TABLE *table);
#ifdef MRN_SUPPORT_FOREIGN_KEYS
  bool storage_create_foreign_key(TABLE *table, const char *grn_table_name,
                                  Field *field, grn_obj *table_obj, int &error);
#endif
  int storage_create_validate_index(TABLE *table);
  int storage_create_index_table(TABLE *table, const char *grn_table_name,
                                 grn_obj *grn_table, MRN_SHARE *tmp_share,
                                 KEY *key_info, grn_obj **index_tables,
                                 uint i);
  int storage_create_index(TABLE *table, const char *grn_table_name,
                           grn_obj *grn_table, MRN_SHARE *tmp_share,
                           KEY *key_info, grn_obj **index_tables,
                           grn_obj **index_columns, uint i);
  int storage_create_indexes(TABLE *table, const char *grn_table_name,
                             grn_obj *grn_table, MRN_SHARE *tmp_share);
  int close_databases();
  int ensure_database_open(const char *name, mrn::Database **db=NULL);
  int ensure_database_remove(const char *name);
  int wrapper_delete_table(const char *name, handlerton *wrap_handlerton,
                           const char *table_name);
  int generic_delete_table(const char *name, const char *table_name);
  int wrapper_open(const char *name, int mode, uint open_options);
  int wrapper_open_indexes(const char *name);
  int storage_reindex();
  int storage_open(const char *name, int mode, uint open_options);
  int open_table(const char *name);
  int storage_open_columns(void);
  void storage_close_columns(void);
  int storage_open_indexes(const char *name);
  void wrapper_overwrite_index_bits();
  int wrapper_close();
  int storage_close();
  int generic_extra(enum ha_extra_function operation);
  int wrapper_extra(enum ha_extra_function operation);
  int storage_extra(enum ha_extra_function operation);
  int wrapper_extra_opt(enum ha_extra_function operation, ulong cache_size);
  int storage_extra_opt(enum ha_extra_function operation, ulong cache_size);
  int generic_reset();
  int wrapper_reset();
  int storage_reset();
  uint wrapper_lock_count() const;
  uint storage_lock_count() const;
  THR_LOCK_DATA **wrapper_store_lock(THD *thd, THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type);
  THR_LOCK_DATA **storage_store_lock(THD *thd, THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type);
  int wrapper_external_lock(THD *thd, int lock_type);
  int storage_external_lock(THD *thd, int lock_type);
#ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
  void wrapper_start_bulk_insert(ha_rows rows, uint flags);
  void storage_start_bulk_insert(ha_rows rows, uint flags);
#else
  void wrapper_start_bulk_insert(ha_rows rows);
  void storage_start_bulk_insert(ha_rows rows);
#endif
  int wrapper_end_bulk_insert();
  int storage_end_bulk_insert();
  bool wrapper_is_target_index(KEY *key_info);
  bool wrapper_have_target_index();
  int wrapper_write_row(const uchar *buf);
  int wrapper_write_row_index(const uchar *buf);
  int storage_write_row(const uchar *buf);
  int storage_write_row_multiple_column_index(const uchar *buf,
                                              grn_id record_id,
                                              KEY *key_info,
                                              grn_obj *index_column);
  int storage_write_row_multiple_column_indexes(const uchar *buf, grn_id record_id);
  int storage_write_row_unique_index(const uchar *buf,
                                     KEY *key_info,
                                     grn_obj *index_table,
                                     grn_obj *index_column,
                                     grn_id *key_id);
  int storage_write_row_unique_indexes(const uchar *buf);
  int wrapper_get_record_id(uchar *data, grn_id *record_id,
                            const char *context);
  int wrapper_update_row(const uchar *old_data, const uchar *new_data);
  int wrapper_update_row_index(const uchar *old_data,
                               const uchar *new_data);
  int storage_update_row(const uchar *old_data, const uchar *new_data);
  int storage_update_row_index(const uchar *old_data,
                               const uchar *new_data);
  int storage_update_row_unique_indexes(const uchar *new_data);
  int wrapper_delete_row(const uchar *buf);
  int wrapper_delete_row_index(const uchar *buf);
  int storage_delete_row(const uchar *buf);
  int storage_delete_row_index(const uchar *buf);
  int storage_delete_row_unique_index(grn_obj *index_table, grn_id del_key_id);
  int storage_delete_row_unique_indexes();
  int storage_prepare_delete_row_unique_index(const uchar *buf,
                                              grn_id record_id,
                                              KEY *key_info,
                                              grn_obj *index_table,
                                              grn_obj *index_column,
                                              grn_id *del_key_id);
  int storage_prepare_delete_row_unique_indexes(const uchar *buf,
                                                grn_id record_id);
  uint wrapper_max_supported_record_length() const;
  uint storage_max_supported_record_length() const;
  uint wrapper_max_supported_keys() const;
  uint storage_max_supported_keys() const;
  uint wrapper_max_supported_key_parts() const;
  uint storage_max_supported_key_parts() const;
  uint wrapper_max_supported_key_length() const;
  uint storage_max_supported_key_length() const;
  uint wrapper_max_supported_key_part_length() const;
  uint storage_max_supported_key_part_length() const;
  ulonglong wrapper_table_flags() const;
  ulonglong storage_table_flags() const;
  ulong wrapper_index_flags(uint idx, uint part, bool all_parts) const;
  ulong storage_index_flags(uint idx, uint part, bool all_parts) const;
  int wrapper_info(uint flag);
  int storage_info(uint flag);
  void storage_info_variable();
  void storage_info_variable_records();
  void storage_info_variable_data_file_length();
#ifdef MRN_HANDLER_RECORDS_RETURN_ERROR
  int wrapper_records(ha_rows *num_rows);
  int storage_records(ha_rows *num_rows);
#else
  ha_rows wrapper_records();
  ha_rows storage_records();
#endif
  int wrapper_rnd_init(bool scan);
  int storage_rnd_init(bool scan);
  int wrapper_rnd_end();
  int storage_rnd_end();
  int wrapper_rnd_next(uchar *buf);
  int storage_rnd_next(uchar *buf);
  int wrapper_rnd_pos(uchar *buf, uchar *pos);
  int storage_rnd_pos(uchar *buf, uchar *pos);
  void wrapper_position(const uchar *record);
  void storage_position(const uchar *record);
  ha_rows wrapper_records_in_range(uint key_nr, const key_range *range_min,
                                   const key_range *range_max,
                                   page_range *pages);
  ha_rows storage_records_in_range(uint key_nr, const key_range *range_min,
                                   const key_range *range_max,
                                   page_range *pages);
  ha_rows generic_records_in_range_geo(uint key_nr, const key_range *range_min,
                                       const key_range *range_max);
  int wrapper_index_init(uint idx, bool sorted);
  int storage_index_init(uint idx, bool sorted);
  int wrapper_index_end();
  int storage_index_end();
  int wrapper_index_read_map(uchar *buf, const uchar *key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag);
  int storage_index_read_map(uchar *buf, const uchar *key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag);
#ifdef MRN_HANDLER_HAVE_INDEX_READ_LAST_MAP
  int wrapper_index_read_last_map(uchar *buf, const uchar *key,
                                  key_part_map keypart_map);
  int storage_index_read_last_map(uchar *buf, const uchar *key,
                                  key_part_map keypart_map);
#endif
  int wrapper_index_next(uchar *buf);
  int storage_index_next(uchar *buf);
  int wrapper_index_prev(uchar *buf);
  int storage_index_prev(uchar *buf);
  int wrapper_index_first(uchar *buf);
  int storage_index_first(uchar *buf);
  int wrapper_index_last(uchar *buf);
  int storage_index_last(uchar *buf);
  int wrapper_index_next_same(uchar *buf, const uchar *key, uint keylen);
  int storage_index_next_same(uchar *buf, const uchar *key, uint keylen);
  int generic_ft_init();
  int wrapper_ft_init();
  int storage_ft_init();
  FT_INFO *wrapper_ft_init_ext(uint flags, uint key_nr, String *key);
  FT_INFO *storage_ft_init_ext(uint flags, uint key_nr, String *key);
  void generic_ft_init_ext_add_conditions_fast_order_limit(
      struct st_mrn_ft_info *info, grn_obj *expression);
  grn_rc generic_ft_init_ext_prepare_expression_in_boolean_mode(
    struct st_mrn_ft_info *info,
    String *key,
    grn_obj *index_column,
    grn_obj *match_columns,
    grn_obj *expression);
  grn_rc generic_ft_init_ext_prepare_expression_in_normal_mode(
    struct st_mrn_ft_info *info,
    String *key,
    grn_obj *index_column,
    grn_obj *match_columns,
    grn_obj *expression);
  struct st_mrn_ft_info *generic_ft_init_ext_select(uint flags,
                                                    uint key_nr,
                                                    String *key);
  FT_INFO *generic_ft_init_ext(uint flags, uint key_nr, String *key);
  int wrapper_ft_read(uchar *buf);
  int storage_ft_read(uchar *buf);
  const Item *wrapper_cond_push(const Item *cond);
  const Item *storage_cond_push(const Item *cond);
  void wrapper_cond_pop();
  void storage_cond_pop();
  bool wrapper_get_error_message(int error, String *buf);
  bool storage_get_error_message(int error, String *buf);
  handler *wrapper_clone(const char *name, MEM_ROOT *mem_root);
  handler *storage_clone(const char *name, MEM_ROOT *mem_root);
  uint8 wrapper_table_cache_type();
  uint8 storage_table_cache_type();
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ
  ha_rows wrapper_multi_range_read_info_const(uint keyno,
                                              RANGE_SEQ_IF *seq,
                                              void *seq_init_param,
                                              uint n_ranges,
                                              uint *bufsz,
                                              uint *flags,
                                              Cost_estimate *cost);
  ha_rows storage_multi_range_read_info_const(uint keyno,
                                              RANGE_SEQ_IF *seq,
                                              void *seq_init_param,
                                              uint n_ranges,
                                              uint *bufsz,
                                              uint *flags,
                                              Cost_estimate *cost);
  ha_rows wrapper_multi_range_read_info(uint keyno, uint n_ranges, uint keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                        uint key_parts,
#endif
                                        uint *bufsz, uint *flags,
                                        Cost_estimate *cost);
  ha_rows storage_multi_range_read_info(uint keyno, uint n_ranges, uint keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                        uint key_parts,
#endif
                                        uint *bufsz, uint *flags,
                                        Cost_estimate *cost);
  int wrapper_multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                    uint n_ranges, uint mode,
                                    HANDLER_BUFFER *buf);
  int storage_multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                    uint n_ranges, uint mode,
                                    HANDLER_BUFFER *buf);
  int wrapper_multi_range_read_next(range_id_t *range_info);
  int storage_multi_range_read_next(range_id_t *range_info);
#else // MRN_HANDLER_HAVE_MULTI_RANGE_READ
  int wrapper_read_multi_range_first(KEY_MULTI_RANGE **found_range_p,
                                     KEY_MULTI_RANGE *ranges,
                                     uint range_count,
                                     bool sorted,
                                     HANDLER_BUFFER *buffer);
  int storage_read_multi_range_first(KEY_MULTI_RANGE **found_range_p,
                                     KEY_MULTI_RANGE *ranges,
                                     uint range_count,
                                     bool sorted,
                                     HANDLER_BUFFER *buffer);
  int wrapper_read_multi_range_next(KEY_MULTI_RANGE **found_range_p);
  int storage_read_multi_range_next(KEY_MULTI_RANGE **found_range_p);
#endif // MRN_HANDLER_HAVE_MULTI_RANGE_READ
  int generic_delete_all_rows(grn_obj *target_grn_table,
                              const char *function_name);
  int wrapper_delete_all_rows();
  int storage_delete_all_rows();
  int wrapper_truncate();
  int wrapper_truncate_index();
  int storage_truncate();
  int storage_truncate_index();
  double wrapper_scan_time();
  double storage_scan_time();
  double wrapper_read_time(uint index, uint ranges, ha_rows rows);
  double storage_read_time(uint index, uint ranges, ha_rows rows);
#ifdef MRN_HANDLER_HAVE_KEYS_TO_USE_FOR_SCANNING
  const key_map *wrapper_keys_to_use_for_scanning();
  const key_map *storage_keys_to_use_for_scanning();
#endif
  ha_rows wrapper_estimate_rows_upper_bound();
  ha_rows storage_estimate_rows_upper_bound();
  void wrapper_update_create_info(HA_CREATE_INFO* create_info);
  void storage_update_create_info(HA_CREATE_INFO* create_info);
  int wrapper_rename_table(const char *from, const char *to,
                           MRN_SHARE *tmp_share,
                           const char *from_table_name,
                           const char *to_table_name);
  int wrapper_rename_index(const char *from, const char *to,
                           MRN_SHARE *tmp_share,
                           const char *from_table_name,
                           const char *to_table_name);
  int storage_rename_table(const char *from, const char *to,
                           MRN_SHARE *tmp_share,
                           const char *from_table_name,
                           const char *to_table_name);
#ifdef MRN_SUPPORT_FOREIGN_KEYS
  int storage_rename_foreign_key(MRN_SHARE *tmp_share,
                                 const char *from_table_name,
                                 const char *to_table_name);
#endif
  bool wrapper_is_crashed() const;
  bool storage_is_crashed() const;
  bool wrapper_auto_repair(int error) const;
  bool storage_auto_repair(int error) const;
  int generic_disable_index(int i, KEY *key_info);
  int wrapper_disable_indexes_mroonga(uint mode);
  int wrapper_disable_indexes(uint mode);
  int storage_disable_indexes(uint mode);
  int wrapper_enable_indexes_mroonga(uint mode);
  int wrapper_enable_indexes(uint mode);
  int storage_enable_indexes(uint mode);
  int wrapper_check(THD* thd, HA_CHECK_OPT* check_opt);
  int storage_check(THD* thd, HA_CHECK_OPT* check_opt);
  int wrapper_fill_indexes(THD *thd, KEY *key_info,
                           grn_obj **index_columns, uint n_keys);
  int wrapper_recreate_indexes(THD *thd);
  int storage_recreate_indexes(THD *thd);
  int wrapper_repair(THD* thd, HA_CHECK_OPT* check_opt);
  int storage_repair(THD* thd, HA_CHECK_OPT* check_opt);
  bool wrapper_check_and_repair(THD *thd);
  bool storage_check_and_repair(THD *thd);
  int wrapper_analyze(THD* thd, HA_CHECK_OPT* check_opt);
  int storage_analyze(THD* thd, HA_CHECK_OPT* check_opt);
  int wrapper_optimize(THD* thd, HA_CHECK_OPT* check_opt);
  int storage_optimize(THD* thd, HA_CHECK_OPT* check_opt);
  bool wrapper_is_fatal_error(int error_num, uint flags);
  bool storage_is_fatal_error(int error_num, uint flags);
  bool wrapper_is_comment_changed(TABLE *table1, TABLE *table2);
  bool wrapper_check_if_incompatible_data(HA_CREATE_INFO *create_info,
                                          uint table_changes);
  bool storage_check_if_incompatible_data(HA_CREATE_INFO *create_info,
                                          uint table_changes);
  int storage_add_index_multiple_columns(KEY *key_info, uint num_of_keys,
                                         grn_obj **index_tables,
                                         grn_obj **index_columns,
                                         bool skip_unique_key);
#ifdef MRN_HANDLER_HAVE_CHECK_IF_SUPPORTED_INPLACE_ALTER
  enum_alter_inplace_result
  wrapper_check_if_supported_inplace_alter(TABLE *altered_table,
                                           Alter_inplace_info *ha_alter_info);
  enum_alter_inplace_result
  storage_check_if_supported_inplace_alter(TABLE *altered_table,
                                           Alter_inplace_info *ha_alter_info);
  bool wrapper_prepare_inplace_alter_table(TABLE *altered_table,
                                           Alter_inplace_info *ha_alter_info);
  bool storage_prepare_inplace_alter_table(TABLE *altered_table,
                                           Alter_inplace_info *ha_alter_info);
  bool wrapper_inplace_alter_table(TABLE *altered_table,
                                   Alter_inplace_info *ha_alter_info);
  bool storage_inplace_alter_table_add_index(TABLE *altered_table,
                                             Alter_inplace_info *ha_alter_info);
  bool storage_inplace_alter_table_drop_index(TABLE *altered_table,
                                              Alter_inplace_info *ha_alter_info);
  bool storage_inplace_alter_table_add_column(TABLE *altered_table,
                                              Alter_inplace_info *ha_alter_info);
  bool storage_inplace_alter_table_drop_column(TABLE *altered_table,
                                               Alter_inplace_info *ha_alter_info);
  bool storage_inplace_alter_table_rename_column(TABLE *altered_table,
                                                 Alter_inplace_info *ha_alter_info);
  bool storage_inplace_alter_table(TABLE *altered_table,
                                   Alter_inplace_info *ha_alter_info);
  bool wrapper_commit_inplace_alter_table(TABLE *altered_table,
                                          Alter_inplace_info *ha_alter_info,
                                          bool commit);
  bool storage_commit_inplace_alter_table(TABLE *altered_table,
                                          Alter_inplace_info *ha_alter_info,
                                          bool commit);
#else
  alter_table_operations wrapper_alter_table_flags(alter_table_operations flags);
  alter_table_operations storage_alter_table_flags(alter_table_operations flags);
#  ifdef MRN_HANDLER_HAVE_FINAL_ADD_INDEX
  int wrapper_add_index(TABLE *table_arg, KEY *key_info, uint num_of_keys,
                        handler_add_index **add);
  int storage_add_index(TABLE *table_arg, KEY *key_info, uint num_of_keys,
                        handler_add_index **add);
#  else
  int wrapper_add_index(TABLE *table_arg, KEY *key_info, uint num_of_keys);
  int storage_add_index(TABLE *table_arg, KEY *key_info, uint num_of_keys);
#  endif
#  ifdef MRN_HANDLER_HAVE_FINAL_ADD_INDEX
  int wrapper_final_add_index(handler_add_index *add, bool commit);
  int storage_final_add_index(handler_add_index *add, bool commit);
#  endif
  int wrapper_prepare_drop_index(TABLE *table_arg, uint *key_num,
                                 uint num_of_keys);
  int storage_prepare_drop_index(TABLE *table_arg, uint *key_num,
                                 uint num_of_keys);
  int wrapper_final_drop_index(TABLE *table_arg);
  int storage_final_drop_index(TABLE *table_arg);
#endif
  int wrapper_update_auto_increment();
  int storage_update_auto_increment();
  void wrapper_set_next_insert_id(ulonglong id);
  void storage_set_next_insert_id(ulonglong id);
  void wrapper_get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values);
  void storage_get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values);
  void wrapper_restore_auto_increment(ulonglong prev_insert_id);
  void storage_restore_auto_increment(ulonglong prev_insert_id);
  void wrapper_release_auto_increment();
  void storage_release_auto_increment();
  int wrapper_check_for_upgrade(HA_CHECK_OPT *check_opt);
  int storage_check_for_upgrade(HA_CHECK_OPT *check_opt);
#ifdef MRN_HANDLER_HAVE_RESET_AUTO_INCREMENT
  int wrapper_reset_auto_increment(ulonglong value);
  int storage_reset_auto_increment(ulonglong value);
#endif
  bool wrapper_was_semi_consistent_read();
  bool storage_was_semi_consistent_read();
  void wrapper_try_semi_consistent_read(bool yes);
  void storage_try_semi_consistent_read(bool yes);
  void wrapper_unlock_row();
  void storage_unlock_row();
  int wrapper_start_stmt(THD *thd, thr_lock_type lock_type);
  int storage_start_stmt(THD *thd, thr_lock_type lock_type);
  void wrapper_change_table_ptr(TABLE *table_arg, TABLE_SHARE *share_arg);
  void storage_change_table_ptr(TABLE *table_arg, TABLE_SHARE *share_arg);
  bool wrapper_is_fk_defined_on_table_or_index(uint index);
  bool storage_is_fk_defined_on_table_or_index(uint index);
  char *wrapper_get_foreign_key_create_info();
  char *storage_get_foreign_key_create_info();
#ifdef MRN_HANDLER_HAVE_GET_TABLESPACE_NAME
  char *wrapper_get_tablespace_name(THD *thd, char *name, uint name_len);
  char *storage_get_tablespace_name(THD *thd, char *name, uint name_len);
#endif
  bool wrapper_can_switch_engines();
  bool storage_can_switch_engines();
  int wrapper_get_foreign_key_list(THD *thd, List<FOREIGN_KEY_INFO> *f_key_list);
  int storage_get_foreign_key_list(THD *thd, List<FOREIGN_KEY_INFO> *f_key_list);
  int wrapper_get_parent_foreign_key_list(THD *thd, List<FOREIGN_KEY_INFO> *f_key_list);
  int storage_get_parent_foreign_key_list(THD *thd, List<FOREIGN_KEY_INFO> *f_key_list);
  uint wrapper_referenced_by_foreign_key();
  uint storage_referenced_by_foreign_key();
  void wrapper_init_table_handle_for_HANDLER();
  void storage_init_table_handle_for_HANDLER();
  void wrapper_free_foreign_key_create_info(char* str);
  void storage_free_foreign_key_create_info(char* str);
  void wrapper_set_keys_in_use();
  void storage_set_keys_in_use();
#ifdef MRN_RBR_UPDATE_NEED_ALL_COLUMNS
  bool check_written_by_row_based_binlog();
#endif
#ifdef MRN_HAVE_HA_REBIND_PSI
  void wrapper_unbind_psi();
  void storage_unbind_psi();
  void wrapper_rebind();
  void storage_rebind();
#endif
  my_bool wrapper_register_query_cache_table(THD *thd,
                                             const char *table_key,
                                             uint key_length,
                                             qc_engine_callback
                                             *engine_callback,
                                             ulonglong *engine_data);
  my_bool storage_register_query_cache_table(THD *thd,
                                             const char *table_key,
                                             uint key_length,
                                             qc_engine_callback
                                             *engine_callback,
                                             ulonglong *engine_data);
};

#ifdef __cplusplus
}
#endif

#endif /* HA_MROONGA_HPP_ */
