/* Copyright (C) 2009-2016 Kentoku Shiba

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

#define VP_DETAIL_VERSION "1.1.14"
#define VP_HEX_VERSION 0x0101

#if MYSQL_VERSION_ID < 50500
#define vp_my_free(A,B) my_free(A,B)
#else
#define vp_my_free(A,B) my_free(A)
#ifdef pthread_mutex_t
#undef pthread_mutex_t
#endif
#define pthread_mutex_t mysql_mutex_t
#ifdef pthread_mutex_lock
#undef pthread_mutex_lock
#endif
#define pthread_mutex_lock mysql_mutex_lock
#ifdef pthread_mutex_trylock
#undef pthread_mutex_trylock
#endif
#define pthread_mutex_trylock mysql_mutex_trylock
#ifdef pthread_mutex_unlock
#undef pthread_mutex_unlock
#endif
#define pthread_mutex_unlock mysql_mutex_unlock
#ifdef pthread_mutex_destroy
#undef pthread_mutex_destroy
#endif
#define pthread_mutex_destroy mysql_mutex_destroy
#ifdef pthread_cond_t
#undef pthread_cond_t
#endif
#define pthread_cond_t mysql_cond_t
#ifdef pthread_cond_wait
#undef pthread_cond_wait
#endif
#define pthread_cond_wait mysql_cond_wait
#ifdef pthread_cond_signal
#undef pthread_cond_signal
#endif
#define pthread_cond_signal mysql_cond_signal
#ifdef pthread_cond_broadcast
#undef pthread_cond_broadcast
#endif
#define pthread_cond_broadcast mysql_cond_broadcast
#ifdef pthread_cond_destroy
#undef pthread_cond_destroy
#endif
#define pthread_cond_destroy mysql_cond_destroy
#define my_sprintf(A,B) sprintf B
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100306
#define VP_THD_db_str(A) (A)->db.str
#define VP_THD_db_length(A) (A)->db.length
#define VP_TABLE_LIST_db_str(A) (A)->db.str
#define VP_TABLE_LIST_db_length(A) (A)->db.length
#define VP_TABLE_LIST_table_name_str(A) (A)->table_name.str
#define VP_TABLE_LIST_table_name_length(A) (A)->table_name.length
#define VP_TABLE_LIST_alias_str(A) (A)->alias.str
#define VP_TABLE_LIST_alias_length(A) (A)->alias.length
#define VP_TABLE_LIST_ALIAS_HAS_LENGTH
#define VP_field_name_str(A) (A)->field_name.str
#define VP_field_name_length(A) (A)->field_name.length
#define VP_item_name_str(A) (A)->name.str
#define VP_item_name_length(A) (A)->name.length
#else
#define VP_THD_db_str(A) (A)->db
#define VP_THD_db_length(A) (A)->db_length
#define VP_TABLE_LIST_db_str(A) (A)->db
#define VP_TABLE_LIST_db_length(A) (A)->db_length
#define VP_TABLE_LIST_table_name_str(A) (A)->table_name
#define VP_TABLE_LIST_table_name_length(A) (A)->table_name_length
#define VP_TABLE_LIST_alias_str(A) (A)->alias
#define VP_TABLE_LIST_alias_length(A) strlen((A)->alias)
#define VP_field_name_str(A) (A)->field_name
#define VP_field_name_length(A) strlen((A)->field_name)
#define VP_item_name_str(A) (A)->name
#define VP_item_name_length(A) strlen((A)->name)
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100213
#define VP_INIT_ALLOC_ROOT(A, B, C, D) \
  init_alloc_root(A, "spider", B, C, D)
#elif defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
#define VP_INIT_ALLOC_ROOT(A, B, C, D) \
  init_alloc_root(A, B, C, D)
#else
#define VP_INIT_ALLOC_ROOT(A, B, C, D) \
  init_alloc_root(A, B, C)
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100209
#define VP_create_partition_name(A,B,C,D,E,F) create_partition_name(A,B,C,D,E,F)
#define VP_create_subpartition_name(A,B,C,D,E,F) create_subpartition_name(A,B,C,D,E,F)
#else
#define VP_create_partition_name(A,B,C,D,E,F) create_partition_name(A,C,D,E,F)
#define VP_create_subpartition_name(A,B,C,D,E,F) create_subpartition_name(A,C,D,E,F)
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100200
#define VP_WITHOUT_HA_STATISTIC_INCREMENT
#define VP_HAS_NEXT_THREAD_ID
#define VP_set_next_thread_id(A)
#define VP_new_THD(A) (new THD(A))
#else
inline void VP_set_next_thread_id(THD *A)
{
  pthread_mutex_lock(&LOCK_thread_count);
  A->thread_id = (*vp_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
}
#define VP_new_THD(A) (new THD())
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100004
#define vp_user_defined_key_parts(A) (A)->user_defined_key_parts
#define vp_join_table_count(A) (A)->table_count
#define vp_get_default_part_db_type_from_partition(A) \
  plugin_data((A)->default_part_plugin, handlerton *)
#define LL(A) A ## LL
#define VP_HANDLER_HAS_HA_CLOSE
#define VP_HANDLER_HAS_COUNT_QUERY_CACHE_DEPENDANT_TABLES
#define VP_ITEM_FUNC_HAS_XOR_FUNC
#define VP_SUPPORT_MRR
#else
#define vp_user_defined_key_parts(A) (A)->key_parts
#define vp_join_table_count(A) (A)->tables
#define vp_get_default_part_db_type_from_partition(A) \
  (A)->default_part_db_type
#define VP_USE_OPEN_SKIP_TEMPORARY
#define VP_KEY_HAS_EXTRA_LENGTH
#define VP_HANDLER_HAS_HA_INDEX_READ_LAST_MAP
#define VP_HANDLER_HAS_ADD_INDEX
#define VP_HANDLER_HAS_DROP_INDEX
#define VP_TABLE_HAS_TIMESTAMP_FIELD_TYPE
#endif
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100009
#define VP_TEST(A) MY_TEST(A)
#else
#define VP_TEST(A) test(A)
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100005
#define VP_HAS_EXPLAIN_QUERY
#endif

#if defined(MARIADB_BASE_VERSION)
#define VP_HANDLER_AUTO_REPAIR_HAS_ERROR
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100100
#define VP_CREATE_FIELD_FIELDPTR_FIELDPTR_REQUIRES_THDPTR
#define VP_QCACHE_INSERT_TABLE_REQUIRES_THDPTR
#endif

#define vp_set_bit(BITMAP, BIT) \
  ((BITMAP)[(BIT) / 8] |= (1 << ((BIT) & 7)))
#define vp_clear_bit(BITMAP, BIT) \
  ((BITMAP)[(BIT) / 8] &= ~(1 << ((BIT) & 7)))
#define vp_bit_is_set(BITMAP, BIT) \
  (uint) ((BITMAP)[(BIT) / 8] & (1 << ((BIT) & 7)))

#ifndef WITHOUT_VP_BG_ACCESS
#define VP_BG_COMMAND_KILL 0
#define VP_BG_COMMAND_SELECT 1
#define VP_BG_COMMAND_INSERT 2
#define VP_BG_COMMAND_UPDATE 3
#define VP_BG_COMMAND_DELETE 4
#define VP_BG_COMMAND_UPDATE_SELECT 5

#ifndef DBUG_OFF
#define VP_DBUG_PRINT_FIELD_VALUES(table, ptr_diff) \
  { \
    char buff[MAX_FIELD_WIDTH]; \
    String str(buff, sizeof(buff), &my_charset_bin); \
    for (uint i = 0; i < table->s->fields; ++i) \
    { \
      Field *field = table->field[i]; \
      DBUG_PRINT("info", ("vp field[%u]=%p", i, field)); \
      DBUG_PRINT("info", ("vp field[%u].ptr=%p", i, field->ptr)); \
      DBUG_PRINT("info", ("vp field[%u].null_ptr=%p", i, field->null_ptr)); \
      DBUG_PRINT("info", ("vp field[%u].null_bit=%u", i, field->null_bit)); \
      field->move_field_offset(ptr_diff); \
      if (field->is_null()) \
      { \
        DBUG_PRINT("info", ("vp null")); \
      } else { \
        DBUG_PRINT("info", ("vp not null")); \
        field->val_str(&str); \
        uint length = str.length(); \
        DBUG_PRINT("info", ("vp length = %d", length)); \
        char *value = (char *) my_alloca(length + 1); \
        memcpy(value, str.ptr(), length); \
        value[length] = '\0'; \
        DBUG_PRINT("info", ("vp value = %s", value)); \
        my_afree(value); \
      } \
      field->move_field_offset(-ptr_diff); \
    } \
  }
#else
#define VP_DBUG_PRINT_FIELD_VALUES(table, ptr_diff)
#endif

typedef struct st_vp_bulk_access_info VP_BULK_ACCESS_INFO;

#ifdef VP_SUPPORT_MRR
class ha_vp;
typedef struct st_vp_key_multi_range
{
  uint id;
  uchar *key[2];
  uint length[2];
  KEY_MULTI_RANGE key_multi_range;
  range_id_t ptr;
  st_vp_key_multi_range *next;
} VP_KEY_MULTI_RANGE;

typedef struct st_vp_child_key_multi_range
{
  VP_KEY_MULTI_RANGE *vp_key_multi_range;
  st_vp_child_key_multi_range *next;
} VP_CHILD_KEY_MULTI_RANGE;

typedef struct st_vp_child_key_multi_range_hld
{
  ha_vp *vp;
  int child_table_idx;
  VP_CHILD_KEY_MULTI_RANGE *vp_child_key_multi_range;
} VP_CHILD_KEY_MULTI_RANGE_HLD;
#endif

#ifdef WITH_PARTITION_STORAGE_ENGINE
typedef struct st_vp_clone_patition_handler_share
{
  uint               use_count;
  void               **handlers;
  my_bitmap_map      *idx_read_bitmap;
  my_bitmap_map      *idx_write_bitmap;
  bool               idx_bitmap_is_set;
} VP_CLONE_PARTITION_HANDLER_SHARE;

typedef struct st_vp_patition_handler_share
{
  uint               use_count;
  TABLE              *table;
  void               *creator;
  void               **handlers;
  my_bitmap_map      *idx_init_read_bitmap;
  my_bitmap_map      *idx_init_write_bitmap;
  my_bitmap_map      *rnd_init_read_bitmap;
  my_bitmap_map      *rnd_init_write_bitmap;
  my_bitmap_map      *idx_read_bitmap;
  my_bitmap_map      *idx_write_bitmap;
  my_bitmap_map      *rnd_read_bitmap;
  my_bitmap_map      *rnd_write_bitmap;
  bool               idx_init_flg;
  bool               rnd_init_flg;
  bool               idx_bitmap_is_set;
  bool               rnd_bitmap_is_set;
  VP_CLONE_PARTITION_HANDLER_SHARE *clone_partition_handler_share;
  VP_BULK_ACCESS_INFO *current_bulk_access_info;
} VP_PARTITION_HANDLER_SHARE;

typedef struct st_vp_patition_share
{
  char               *table_name;
  uint               table_name_length;
  uint               use_count;
  pthread_mutex_t    pt_handler_mutex;
  HASH               pt_handler_hash;

/*
  volatile VP_PARTITION_HANDLER_SHARE *partition_handler_share;
*/
} VP_PARTITION_SHARE;
#endif

typedef struct st_vp_bg_base
{
  int                   table_idx;
  TABLE_LIST            *part_table;
  handler               *parent;
  uchar                 table_key_different[MAX_KEY_LENGTH];
  volatile uchar        *table_key;
  volatile key_part_map tgt_key_part_map;
  volatile int          key_idx;
  volatile int          record_idx;
  volatile bool         bg_init;
  volatile bool         bg_caller_sync_wait;
  volatile int          bg_command;
  volatile int          bg_error;
  THD                   *bg_thd;
  pthread_t             bg_thread;
  pthread_cond_t        bg_cond;
  pthread_mutex_t       bg_mutex;
  pthread_cond_t        bg_sync_cond;
  pthread_mutex_t       bg_sync_mutex;
} VP_BG_BASE;
#endif

typedef struct st_vp_correspond_key
{
  int                  table_idx;
  int                  key_idx;
  uchar                *columns_bit;
  st_vp_correspond_key *next;
  uint                 key_parts;
  st_vp_correspond_key *next_shortest;
} VP_CORRESPOND_KEY;

typedef struct st_vp_key
{
  int                key_idx;
  uchar              *columns_bit;
  VP_CORRESPOND_KEY  *correspond_key;
  VP_CORRESPOND_KEY  *shortest_correspond_key;
  st_vp_key          *key_length_next;
  st_vp_key          *key_length_prev;
} VP_KEY;

typedef struct st_vp_share
{
  char               *table_name;
  uint               table_name_length;
  uint               use_count;
  pthread_mutex_t    mutex;
  THR_LOCK           lock;

  int                table_count;
  volatile bool      init;
  volatile bool      reinit;
  pthread_mutex_t    init_mutex;
  ulong              *def_versions;
  longlong           additional_table_flags;
  int                bitmap_size;
  int                use_tables_size;
  int                *correspond_columns_p;
  int                *correspond_pt_columns_p;
  int                **correspond_columns_c_ptr;
  int                **uncorrespond_pt_columns_c_ptr;
  uchar              *correspond_columns_bit;
  uchar              *all_columns_bit;       /* all column flags are setted */
  VP_KEY             *keys;
  VP_KEY             *largest_key;
  VP_CORRESPOND_KEY  **correspond_pk;
  VP_CORRESPOND_KEY  **correspond_keys_p_ptr;
  bool               same_all_columns;
  uchar              *need_converting;
  uchar              *same_columns;
  uchar              *need_searching;
  uchar              *need_full_col_for_update;
  uchar              *pk_in_read_index;
  uchar              *select_ignore;
  uchar              *select_ignore_with_lock;
  uchar              *cpy_clm_bitmap;

  int                choose_table_mode;
  int                choose_table_mode_for_lock;
  int                multi_range_mode;
  int                str_copy_mode;
  int                pk_correspond_mode;
  int                info_src_table;
  int                auto_increment_table;
  int                table_count_mode;
  int                support_table_cache;
  int                child_binlog;
#ifndef WITHOUT_VP_BG_ACCESS
  int                bgs_mode;
  int                bgi_mode;
  int                bgu_mode;
#endif
  int                zero_record_update_mode;
  int                allow_bulk_autoinc;
  int                allow_different_column_type;

  char               *tgt_default_db_name;
  char               *tgt_table_name_list;
  char               *tgt_table_name_prefix;
  char               *tgt_table_name_suffix;
  char               *choose_ignore_table_list;
  char               *choose_ignore_table_list_for_lock;

  uint               tgt_default_db_name_length;
  uint               tgt_table_name_list_length;
  uint               tgt_table_name_prefix_length;
  uint               tgt_table_name_suffix_length;
  uint               choose_ignore_table_list_length;
  uint               choose_ignore_table_list_for_lock_length;

  char               **tgt_db_name;
  char               **tgt_table_name;
  TABLE_LIST         *part_tables;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  VP_PARTITION_SHARE *partition_share;
#endif
} VP_SHARE;

typedef struct st_vp_key_copy
{
  uchar              table_key_same[MAX_KEY_LENGTH];
  uchar              *table_key_different;
  bool               init;
  key_part_map       tgt_key_part_map;
  bool               mem_root_init;
  MEM_ROOT           mem_root;
  char               **ptr;
  int                *len;
  uchar              *null_flg;
} VP_KEY_COPY;

#if MYSQL_VERSION_ID < 50500
#else
typedef struct st_vp_child_info
{
  enum_table_ref_type child_table_ref_type;
  ulong               child_def_version;
} VP_CHILD_INFO;
#endif
