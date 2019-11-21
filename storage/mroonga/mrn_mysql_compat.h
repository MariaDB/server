/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2017 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_MYSQL_COMPAT_H_
#define MRN_MYSQL_COMPAT_H_

#include "mrn_mysql.h"

#if MYSQL_VERSION_ID >= 50604
#  define MRN_HAVE_MYSQL_TYPE_TIMESTAMP2
#  define MRN_HAVE_MYSQL_TYPE_DATETIME2
#  define MRN_HAVE_MYSQL_TYPE_TIME2
#endif

#if MYSQL_VERSION_ID >= 50709 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_MYSQL_TYPE_JSON
#endif

#if MYSQL_VERSION_ID < 50603
  typedef MYSQL_ERROR Sql_condition;
#endif

#if defined(MRN_MARIADB_P)
#  if MYSQL_VERSION_ID < 100000
     typedef COST_VECT Cost_estimate;
#  endif
#endif

#ifndef MRN_MARIADB_P
  typedef char *range_id_t;
#endif

#if defined(MRN_MARIADB_P) || MYSQL_VERSION_ID < 80002
  typedef st_select_lex SELECT_LEX;
#endif

#if MYSQL_VERSION_ID >= 50609
#  define MRN_KEY_HAS_USER_DEFINED_KEYPARTS
#endif

#ifdef MRN_KEY_HAS_USER_DEFINED_KEYPARTS
#  define KEY_N_KEY_PARTS(key) (key)->user_defined_key_parts
#else
#  define KEY_N_KEY_PARTS(key) (key)->key_parts
#endif

#if defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100000
#  define mrn_init_alloc_root(PTR, SZ1, SZ2, FLAG) \
  init_alloc_root(PTR, SZ1, SZ2, FLAG)
#elif MYSQL_VERSION_ID >= 50706
#  define mrn_init_alloc_root(PTR, SZ1, SZ2, FLAG) \
  init_alloc_root(mrn_memory_key, PTR, SZ1, SZ2)
#else
#  define mrn_init_alloc_root(PTR, SZ1, SZ2, FLAG) \
  init_alloc_root(PTR, SZ1, SZ2)
#endif

#if MYSQL_VERSION_ID < 100002 || !defined(MRN_MARIADB_P)
#  define GTS_TABLE 0
#endif

#if MYSQL_VERSION_ID >= 50607
#  if MYSQL_VERSION_ID >= 100007 && defined(MRN_MARIADB_P)
#    define MRN_GET_ERROR_MESSAGE thd_get_error_message(current_thd)
#    define MRN_GET_ERROR_NUMBER thd_get_error_number(current_thd)
#    define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) thd_get_error_row(thd)
#  else
#    define MRN_GET_ERROR_MESSAGE current_thd->get_stmt_da()->message()
#    define MRN_GET_ERROR_NUMBER current_thd->get_stmt_da()->sql_errno()
#    if MYSQL_VERSION_ID >= 50706
#      define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) \
  thd->get_stmt_da()->current_row_for_condition()
#    else
#      define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) \
  thd->get_stmt_da()->current_row_for_warning()
#    endif
#  endif
#else
#  define MRN_GET_ERROR_MESSAGE current_thd->stmt_da->message()
#  define MRN_GET_ERROR_NUMBER current_thd->stmt_da->sql_errno()
#  define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) thd->warning_info->current_row_for_warning()
#endif

#if MYSQL_VERSION_ID >= 50607 && !defined(MRN_MARIADB_P)
#  define MRN_ITEM_HAVE_ITEM_NAME
#endif

#if MYSQL_VERSION_ID < 100000
#  define MRN_HAVE_TABLE_DEF_CACHE
#endif

#if defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100009
#  define MRN_HAVE_TDC_ACQUIRE_SHARE
#  if MYSQL_VERSION_ID < 100200
#    define MRN_TDC_ACQUIRE_SHARE_REQUIRE_KEY
#  endif
#endif

#if MYSQL_VERSION_ID >= 50613
#  define MRN_HAVE_ALTER_INFO
#endif

#if MYSQL_VERSION_ID >= 50603
#  define MRN_HAVE_GET_TABLE_DEF_KEY
#endif

#if defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100004
#  define MRN_TABLE_SHARE_HAVE_LOCK_SHARE
#endif

#ifndef TIME_FUZZY_DATE
/* For MariaDB 10. */
#  ifdef TIME_FUZZY_DATES
#    define TIME_FUZZY_DATE TIME_FUZZY_DATES
#  endif
#endif

#if MYSQL_VERSION_ID >= 100007 && defined(MRN_MARIADB_P)
#  define MRN_USE_MYSQL_DATA_HOME
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_SEVERITY_WARNING Sql_condition::SL_WARNING
#else
#  define MRN_SEVERITY_WARNING Sql_condition::WARN_LEVEL_WARN
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_PSI_MEMORY_KEY
#endif

#ifdef MRN_HAVE_PSI_MEMORY_KEY
#  define mrn_my_malloc(size, flags) \
  my_malloc(mrn_memory_key, size, flags)
#  define mrn_my_strdup(string, flags) \
  my_strdup(mrn_memory_key, string, flags)
#  define mrn_my_strndup(string, size, flags) \
  my_strndup(mrn_memory_key, string, size, flags)
#  define mrn_my_multi_malloc(flags, ...) \
  my_multi_malloc(mrn_memory_key, flags, __VA_ARGS__)
#else
#  define mrn_my_malloc(size, flags) my_malloc(size, flags)
#  define mrn_my_strdup(string, flags) my_strdup(string, flags)
#  define mrn_my_strndup(string, size, flags) \
  my_strndup(string, size, flags)
#  define mrn_my_multi_malloc(flags, ...) \
  my_multi_malloc(flags, __VA_ARGS__)
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_STRING_FREE(string) string.mem_free();
#else
#  define MRN_STRING_FREE(string) string.free();
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_THD_DB_PATH(thd) ((thd)->db().str)
#else
#  define MRN_THD_DB_PATH(thd) ((thd)->db)
#endif

#ifndef INT_MAX64
#  define INT_MAX64 LONGLONG_MAX
#endif

#ifdef UINT_MAX
#  define UINT_MAX64 UINT_MAX
#else
#  define UINT_MAX64 LONGLONG_MAX
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define mrn_my_stpmov(dst, src) my_stpmov(dst, src)
#else
#  define mrn_my_stpmov(dst, src) strmov(dst, src)
#endif

#if MYSQL_VERSION_ID >= 50607
#  if !defined(MRN_MARIADB_P)
#    define MRN_HAVE_SQL_OPTIMIZER_H
#  endif
#endif

#if MYSQL_VERSION_ID >= 50600 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_BINLOG_H
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_SPATIAL
#elif defined(HAVE_SPATIAL)
#  define MRN_HAVE_SPATIAL
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_FORMAT_STRING_LENGTH "zu"
#else
#  define MRN_FORMAT_STRING_LENGTH "u"
#endif

#ifdef MRN_MARIADB_P
#  define MRN_SUPPORT_CUSTOM_OPTIONS
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_ITEM_EQUAL_FIELDS_ITERATOR
#endif

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_SELECT_LEX_GET_WHERE_COND(select_lex) \
  ((select_lex)->where_cond())
#  define MRN_SELECT_LEX_GET_HAVING_COND(select_lex) \
  ((select_lex)->having_cond())
#  define MRN_SELECT_LEX_GET_ACTIVE_OPTIONS(select_lex) \
  ((select_lex)->active_options())
#else
#  define MRN_SELECT_LEX_GET_WHERE_COND(select_lex) \
  ((select_lex)->where)
#  define MRN_SELECT_LEX_GET_HAVING_COND(select_lex) \
  ((select_lex)->having)
#  define MRN_SELECT_LEX_GET_ACTIVE_OPTIONS(select_lex) \
  ((select_lex)->options)
#endif

#if defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100000
#  if MYSQL_VERSION_ID >= 100104
#    define mrn_init_sql_alloc(thd, mem_root)                           \
  init_sql_alloc(mem_root,                                              \
                 TABLE_ALLOC_BLOCK_SIZE,                                \
                 0,                                                     \
                 MYF(thd->slave_thread ? 0 : MY_THREAD_SPECIFIC))
#  else
#    define mrn_init_sql_alloc(thd, mem_root)           \
  init_sql_alloc(mem_root,                              \
                 TABLE_ALLOC_BLOCK_SIZE,                \
                 0,                                     \
                 MYF(0))
#  endif
#else
#  if MYSQL_VERSION_ID >= 50709
#    define mrn_init_sql_alloc(thd, mem_root)           \
  init_sql_alloc(mrn_memory_key,                        \
                 mem_root,                              \
                 TABLE_ALLOC_BLOCK_SIZE,                \
                 0)
#  else
#    define mrn_init_sql_alloc(thd, mem_root)           \
  init_sql_alloc(mem_root,                              \
                 TABLE_ALLOC_BLOCK_SIZE,                \
                 0)
#  endif
#endif

#ifdef MRN_MARIADB_P
#  define MRN_ABORT_ON_WARNING(thd) thd->abort_on_warning
#else
#  if MYSQL_VERSION_ID >= 50706
#    define MRN_ABORT_ON_WARNING(thd) thd->is_strict_mode()
#  else
#    define MRN_ABORT_ON_WARNING(thd) thd->abort_on_warning
#  endif
#endif

#define MRN_ERROR_CODE_DATA_TRUNCATE(thd)                               \
  (MRN_ABORT_ON_WARNING(thd) ? ER_WARN_DATA_OUT_OF_RANGE : WARN_DATA_TRUNCATED)

#if MYSQL_VERSION_ID >= 50709 && !defined(MRN_MARIADB_P)
#  define mrn_my_hash_init(hash,                        \
                           charset,                     \
                           default_array_elements,      \
                           key_offset,                  \
                           key_length,                  \
                           get_key,                     \
                           free_element,                \
                           flags)                       \
  my_hash_init(hash,                                    \
               charset,                                 \
               default_array_elements,                  \
               key_offset,                              \
               key_length,                              \
               get_key,                                 \
               free_element,                            \
               flags,                                   \
               mrn_memory_key)
#else
#  define mrn_my_hash_init(hash,                        \
                           charset,                     \
                           default_array_elements,      \
                           key_offset,                  \
                           key_length,                  \
                           get_key,                     \
                           free_element,                \
                           flags)                       \
  my_hash_init(hash,                                    \
               charset,                                 \
               default_array_elements,                  \
               key_offset,                              \
               key_length,                              \
               get_key,                                 \
               free_element,                            \
               flags)
#endif

#if defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100000
#  define mrn_strconvert(from_cs,               \
                         from,                  \
                         from_length,           \
                         to_cs,                 \
                         to,                    \
                         to_length,             \
                         errors)                \
  strconvert((from_cs),                         \
             (from),                            \
             (from_length),                     \
             (to_cs),                           \
             (to),                              \
             (to_length),                       \
             (errors))
#else
#  define mrn_strconvert(from_cs,               \
                         from,                  \
                         from_length,           \
                         to_cs,                 \
                         to,                    \
                         to_length,             \
                         errors)                \
  strconvert((from_cs),                         \
             (from),                            \
             (to_cs),                           \
             (to),                              \
             (to_length),                       \
             (errors))
#endif

#if MYSQL_VERSION_ID >= 50717 && !defined(MRN_MARIADB_P)
#  define mrn_is_directory_separator(c)         \
  is_directory_separator((c))
#else
#  define mrn_is_directory_separator(c)         \
  (c == FN_LIBCHAR || c == FN_LIBCHAR2)
#endif

#if ((MYSQL_VERSION_ID < 50636) || \
    (MYSQL_VERSION_ID >= 50700 && MYSQL_VERSION_ID < 50718)) && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_MYSQL_FIELD_PART_OF_KEY_NOT_CLUSTERED
#endif

#if defined(MRN_MARIADB_P) &&                                           \
  ((MYSQL_VERSION_ID >= 100207) ||                                      \
   ((MYSQL_VERSION_ID >= 100126) && (MYSQL_VERSION_ID < 100200)) ||     \
   ((MYSQL_VERSION_ID >= 100032) && (MYSQL_VERSION_ID < 100100)) ||     \
   ((MYSQL_VERSION_ID >= 50557) && (MYSQL_VERSION_ID < 100000)))
#  define mrn_create_partition_name(out,                                \
                                    out_length,                         \
                                    in1,                                \
                                    in2,                                \
                                    name_variant,                       \
                                    translate)                          \
  create_partition_name(out, out_length, in1, in2, name_variant, translate)
#  define mrn_create_subpartition_name(out,             \
                                       out_length,      \
                                       in1,             \
                                       in2,             \
                                       in3,             \
                                       name_variant)    \
  create_subpartition_name(out, out_length, in1, in2, in3, name_variant)
#else
#  define mrn_create_partition_name(out,                                \
                                    out_length,                         \
                                    in1,                                \
                                    in2,                                \
                                    name_variant,                       \
                                    translate)                          \
  (create_partition_name(out, in1, in2, name_variant, translate), 0)
#  define mrn_create_subpartition_name(out,             \
                                       out_length,      \
                                       in1,             \
                                       in2,             \
                                       in3,             \
                                       name_variant)    \
  (create_subpartition_name(out, in1, in2, in3, name_variant), 0)
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80002)
#  define ITEM_SUM_GET_NEST_LEVEL(sum_item) (sum_item)->base_select->nest_level
#  define ITEM_SUM_GET_AGGR_LEVEL(sum_item) (sum_item)->aggr_select->nest_level
#  define ITEM_SUM_GET_MAX_AGGR_LEVEL(sum_item) (sum_item)->max_aggr_level
#else
#  define ITEM_SUM_GET_NEST_LEVEL(sum_item) (sum_item)->nest_level
#  define ITEM_SUM_GET_AGGR_LEVEL(sum_item) (sum_item)->aggr_level
#  define ITEM_SUM_GET_MAX_AGGR_LEVEL(sum_item) (sum_item)->max_arg_level
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80002)
  typedef bool mrn_bool;
#else
  typedef my_bool mrn_bool;
#endif

#define MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(type, variable_name, variable_size) \
  type *variable_name =                                                 \
    (type *)mrn_my_malloc(sizeof(type) * (variable_size), MYF(MY_WME))
#define MRN_FREE_VARIABLE_LENGTH_ARRAYS(variable_name) \
  my_free(variable_name)

#if ((defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100203)) || \
  (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 50711)
#  define MRN_ALTER_INPLACE_INFO_ADD_VIRTUAL_COLUMN \
  Alter_inplace_info::ADD_VIRTUAL_COLUMN
#  define MRN_ALTER_INPLACE_INFO_ADD_STORED_BASE_COLUMN \
  Alter_inplace_info::ADD_STORED_BASE_COLUMN
#  define MRN_ALTER_INPLACE_INFO_ADD_STORED_GENERATED_COLUMN \
  Alter_inplace_info::ADD_STORED_GENERATED_COLUMN
#else
#  define MRN_ALTER_INPLACE_INFO_ADD_VIRTUAL_COLUMN 0
#  define MRN_ALTER_INPLACE_INFO_ADD_STORED_BASE_COLUMN \
  Alter_inplace_info::ADD_COLUMN
#  define MRN_ALTER_INPLACE_INFO_ADD_STORED_GENERATED_COLUMN 0
#endif

#if (defined(HA_CAN_VIRTUAL_COLUMNS) || defined(HA_GENERATED_COLUMNS))
#  define MRN_SUPPORT_GENERATED_COLUMNS
#endif

#ifdef MRN_MARIADB_P
#  if (MYSQL_VERSION_ID >= 100200)
#    define MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field) \
       (!field->stored_in_db())
#    define MRN_GENERATED_COLUMNS_FIELD_IS_STORED(field) \
       (field->vcol_info && field->vcol_info->is_stored())
#  elif (MYSQL_VERSION_ID >= 50500)
#    define MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field) \
       (!field->stored_in_db)
#    define MRN_GENERATED_COLUMNS_FIELD_IS_STORED(field) \
       (field->vcol_info && field->vcol_info->is_stored())
#  else
#    define MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field) false
#    define MRN_GENERATED_COLUMNS_FIELD_IS_STORED(field) false
#  endif
#else
#  if (MYSQL_VERSION_ID >= 50708)
#    define MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field) \
       (field->is_virtual_gcol())
#    define MRN_GENERATED_COLUMNS_FIELD_IS_STORED(field) \
       (field->is_gcol() && !field->is_virtual_gcol())
#  elif (MYSQL_VERSION_ID >= 50706)
#    define MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field) \
       (!field->stored_in_db)
#    define MRN_GENERATED_COLUMNS_FIELD_IS_STORED(field) \
       (field->gcol_info && field->gcol_info->get_field_stored())
#  else
#    define MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field) false
#    define MRN_GENERATED_COLUMNS_FIELD_IS_STORED(field) false
#  endif
#endif

#ifdef MRN_MARIADB_P
#  if (MYSQL_VERSION_ID >= 100203)
#    define MRN_GENERATED_COLUMNS_UPDATE_VIRTUAL_FIELD(table, field) \
       (table->update_virtual_field(field))
#  else
#    define MRN_GENERATED_COLUMNS_UPDATE_VIRTUAL_FIELD(table, field) \
       (field->vcol_info->expr_item->save_in_field(field, 0))
#  endif
#endif

#endif /* MRN_MYSQL_COMPAT_H_ */
