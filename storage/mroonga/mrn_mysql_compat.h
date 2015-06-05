/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2015 Kouhei Sutou <kou@clear-code.com>

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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef MRN_MYSQL_COMPAT_H_
#define MRN_MYSQL_COMPAT_H_

#include "mrn_mysql.h"

#if MYSQL_VERSION_ID >= 50604
#  define MRN_HAVE_MYSQL_TYPE_TIMESTAMP2
#  define MRN_HAVE_MYSQL_TYPE_DATETIME2
#  define MRN_HAVE_MYSQL_TYPE_TIME2
#endif

#if MYSQL_VERSION_ID < 50603
  typedef MYSQL_ERROR Sql_condition;
#endif

#if defined(MRN_MARIADB_P)
#  if MYSQL_VERSION_ID >= 50302 && MYSQL_VERSION_ID < 100000
     typedef COST_VECT Cost_estimate;
#  endif
#endif

#if MYSQL_VERSION_ID >= 50516
#  define MRN_PLUGIN_HAVE_FLAGS 1
#endif

// for MySQL < 5.5
#ifndef MY_ALL_CHARSETS_SIZE
#  define MY_ALL_CHARSETS_SIZE 256
#endif

#ifndef MRN_MARIADB_P
  typedef char *range_id_t;
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
#    define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) thd_get_error_row(thd)
#  else
#    define MRN_GET_ERROR_MESSAGE current_thd->get_stmt_da()->message()
#    if MYSQL_VERSION_ID >= 50706
#      define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) \
  thd->get_stmt_da()->current_row_for_condition()
#    else
#      define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) \
  thd->get_stmt_da()->current_row_for_warning()
#    endif
#  endif
#else
#  if MYSQL_VERSION_ID >= 50500
#    define MRN_GET_ERROR_MESSAGE current_thd->stmt_da->message()
#    define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) thd->warning_info->current_row_for_warning()
#  else
#    define MRN_GET_ERROR_MESSAGE current_thd->main_da.message()
#    define MRN_GET_CURRENT_ROW_FOR_WARNING(thd) thd->row_count
#  endif
#endif

#if MYSQL_VERSION_ID >= 50607 && !defined(MRN_MARIADB_P)
#  define MRN_ITEM_HAVE_ITEM_NAME
#endif

#if MYSQL_VERSION_ID >= 50500 && MYSQL_VERSION_ID < 100000
#  define MRN_HAVE_TABLE_DEF_CACHE
#endif

#if defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100009
#  define MRN_HAVE_TDC_ACQUIRE_SHARE
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

#if MYSQL_VERSION_ID >= 50404
#  define MRN_TABLE_SHARE_HAVE_LOCK_HA_DATA
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

#endif /* MRN_MYSQL_COMPAT_H_ */
