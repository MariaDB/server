/* Copyright (C) 2008-2020 Kentoku Shiba
   Copyright (C) 2019-2020 MariaDB corp

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

#include "hs_compat.h"
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#include "hstcpcli.hpp"
#endif

#define SPIDER_DBTON_SIZE 15

#ifndef SIZEOF_STORED_DOUBLE
#define SIZEOF_STORED_DOUBLE 8
#endif

#define SPIDER_DB_WRAPPER_MYSQL "mysql"
#define SPIDER_DB_WRAPPER_MARIADB "mariadb"

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100204
#define PLUGIN_VAR_CAN_MEMALLOC
/*
#define ITEM_FUNC_CASE_PARAMS_ARE_PUBLIC
#define HASH_UPDATE_WITH_HASH_VALUE
*/
#else
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
#define HANDLER_HAS_DIRECT_UPDATE_ROWS_WITH_HS
#endif
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
#define SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
#define SPIDER_HAS_APPEND_FOR_SINGLE_QUOTE
#define SPIDER_HAS_SHOW_SIMPLE_FUNC
#define SPIDER_HAS_JT_HASH_INDEX_MERGE
#define SPIDER_HAS_EXPR_CACHE_ITEM
#else
#define SPIDER_NEED_CHECK_CONDITION_AT_CHECKING_DIRECT_ORDER_LIMIT
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100007
#define SPIDER_ITEM_HAS_CMP_TYPE
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100004
#define SPIDER_HAS_TIME_STATUS
#define SPIDER_HAS_DECIMAL_OPERATION_RESULTS_VALUE_TYPE
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100014
#define SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100100
#define SPIDER_ITEM_STRING_WITHOUT_SET_STR_WITH_COPY_AND_THDPTR
#endif
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100108
#define SPIDER_HAS_GROUP_BY_HANDLER
#endif

#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100200
#define SPIDER_ORDER_HAS_ENUM_ORDER
#endif

#if defined(MARIADB_BASE_VERSION)
#define SPIDER_ITEM_GEOFUNC_NAME_HAS_MBR
#define SPIDER_HANDLER_AUTO_REPAIR_HAS_ERROR
#endif

class spider_db_conn;
typedef spider_db_conn SPIDER_DB_CONN;
class spider_db_result;
typedef spider_db_result SPIDER_DB_RESULT;
class spider_db_row;
typedef spider_db_row SPIDER_DB_ROW;
class spider_db_result_buffer;
typedef spider_db_result_buffer SPIDER_DB_RESULT_BUFFER;
struct st_spider_conn;
typedef st_spider_conn SPIDER_CONN;
struct st_spider_result;
typedef st_spider_result SPIDER_RESULT;

#define SPIDER_SQL_SEMICOLON_STR ";"
#define SPIDER_SQL_SEMICOLON_LEN sizeof(SPIDER_SQL_SEMICOLON_STR) - 1
#define SPIDER_SQL_COLON_STR ":"
#define SPIDER_SQL_COLON_LEN sizeof(SPIDER_SQL_COLON_STR) - 1
#define SPIDER_SQL_VALUE_QUOTE_STR "'"
#define SPIDER_SQL_VALUE_QUOTE_LEN (sizeof(SPIDER_SQL_VALUE_QUOTE_STR) - 1)

#define SPIDER_SQL_DOT_STR "."
#define SPIDER_SQL_DOT_LEN (sizeof(SPIDER_SQL_DOT_STR) - 1)
#define SPIDER_SQL_HYPHEN_STR "-"
#define SPIDER_SQL_HYPHEN_LEN (sizeof(SPIDER_SQL_HYPHEN_STR) - 1)

#define SPIDER_SQL_EQUAL_STR " = "
#define SPIDER_SQL_EQUAL_LEN (sizeof(SPIDER_SQL_EQUAL_STR) - 1)
#define SPIDER_SQL_AND_STR " and "
#define SPIDER_SQL_AND_LEN (sizeof(SPIDER_SQL_AND_STR) - 1)
#define SPIDER_SQL_BETWEEN_STR " between "
#define SPIDER_SQL_BETWEEN_LEN (sizeof(SPIDER_SQL_BETWEEN_STR) - 1)

#define SPIDER_SQL_TABLE_NAME_STR "`table_name`"
#define SPIDER_SQL_TABLE_NAME_LEN sizeof(SPIDER_SQL_TABLE_NAME_STR) - 1

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#define SPIDER_SQL_HS_EQUAL_STR "="
#define SPIDER_SQL_HS_EQUAL_LEN (sizeof(SPIDER_SQL_HS_EQUAL_STR) - 1)
#define SPIDER_SQL_HS_GT_STR ">"
#define SPIDER_SQL_HS_GT_LEN (sizeof(SPIDER_SQL_HS_GT_STR) - 1)
#define SPIDER_SQL_HS_GTEQUAL_STR ">="
#define SPIDER_SQL_HS_GTEQUAL_LEN (sizeof(SPIDER_SQL_HS_GTEQUAL_STR) - 1)
#define SPIDER_SQL_HS_LT_STR "<"
#define SPIDER_SQL_HS_LT_LEN (sizeof(SPIDER_SQL_HS_LT_STR) - 1)
#define SPIDER_SQL_HS_INSERT_STR "+"
#define SPIDER_SQL_HS_INSERT_LEN (sizeof(SPIDER_SQL_HS_INSERT_STR) - 1)
#define SPIDER_SQL_HS_UPDATE_STR "U"
#define SPIDER_SQL_HS_UPDATE_LEN (sizeof(SPIDER_SQL_HS_UPDATE_STR) - 1)
#define SPIDER_SQL_HS_DELETE_STR "D"
#define SPIDER_SQL_HS_DELETE_LEN (sizeof(SPIDER_SQL_HS_DELETE_STR) - 1)
#define SPIDER_SQL_HS_INCREMENT_STR "+"
#define SPIDER_SQL_HS_INCREMENT_LEN (sizeof(SPIDER_SQL_HS_INCREMENT_STR) - 1)
#define SPIDER_SQL_HS_DECREMENT_STR "-"
#define SPIDER_SQL_HS_DECREMENT_LEN (sizeof(SPIDER_SQL_HS_DECREMENT_STR) - 1)
#endif
#define SPIDER_SQL_HS_LTEQUAL_STR "<="
#define SPIDER_SQL_HS_LTEQUAL_LEN (sizeof(SPIDER_SQL_HS_LTEQUAL_STR) - 1)

#ifdef ITEM_FUNC_CASE_PARAMS_ARE_PUBLIC
#define SPIDER_SQL_CASE_STR "case "
#define SPIDER_SQL_CASE_LEN (sizeof(SPIDER_SQL_CASE_STR) - 1)
#define SPIDER_SQL_WHEN_STR " when "
#define SPIDER_SQL_WHEN_LEN (sizeof(SPIDER_SQL_WHEN_STR) - 1)
#define SPIDER_SQL_THEN_STR " then "
#define SPIDER_SQL_THEN_LEN (sizeof(SPIDER_SQL_THEN_STR) - 1)
#define SPIDER_SQL_ELSE_STR " else "
#define SPIDER_SQL_ELSE_LEN (sizeof(SPIDER_SQL_ELSE_STR) - 1)
#define SPIDER_SQL_END_STR " end"
#define SPIDER_SQL_END_LEN (sizeof(SPIDER_SQL_END_STR) - 1)
#endif

#define SPIDER_SQL_USING_STR " using "
#define SPIDER_SQL_USING_LEN (sizeof(SPIDER_SQL_USING_STR) - 1)
#define SPIDER_SQL_MBR_STR "mbr"
#define SPIDER_SQL_MBR_LEN (sizeof(SPIDER_SQL_MBR_STR) - 1)
#define SPIDER_SQL_MBR_EQUAL_STR "mbrequal("
#define SPIDER_SQL_MBR_EQUAL_LEN (sizeof(SPIDER_SQL_MBR_EQUAL_STR) - 1)
#define SPIDER_SQL_MBR_CONTAIN_STR "mbrcontains("
#define SPIDER_SQL_MBR_CONTAIN_LEN (sizeof(SPIDER_SQL_MBR_CONTAIN_STR) - 1)
#define SPIDER_SQL_MBR_INTERSECT_STR "mbrintersects("
#define SPIDER_SQL_MBR_INTERSECT_LEN (sizeof(SPIDER_SQL_MBR_INTERSECT_STR) - 1)
#define SPIDER_SQL_MBR_WITHIN_STR "mbrwithin("
#define SPIDER_SQL_MBR_WITHIN_LEN (sizeof(SPIDER_SQL_MBR_WITHIN_STR) - 1)
#define SPIDER_SQL_MBR_DISJOINT_STR "mbrdisjoint("
#define SPIDER_SQL_MBR_DISJOINT_LEN (sizeof(SPIDER_SQL_MBR_DISJOINT_STR) - 1)
#define SPIDER_SQL_NOT_BETWEEN_STR "not between"
#define SPIDER_SQL_NOT_BETWEEN_LEN (sizeof(SPIDER_SQL_NOT_BETWEEN_STR) - 1)
#define SPIDER_SQL_TO_FLOAT_STR "/* create function to_float(a decimal(20,6)) returns float return a */ to_float("
#define SPIDER_SQL_TO_FLOAT_LEN (sizeof(SPIDER_SQL_TO_FLOAT_STR) - 1)
#define SPIDER_SQL_IN_STR "in("
#define SPIDER_SQL_IN_LEN (sizeof(SPIDER_SQL_IN_STR) - 1)
#define SPIDER_SQL_NOT_IN_STR "not in("
#define SPIDER_SQL_NOT_IN_LEN (sizeof(SPIDER_SQL_NOT_IN_STR) - 1)
#define SPIDER_SQL_NOT_LIKE_STR "not like"
#define SPIDER_SQL_NOT_LIKE_LEN (sizeof(SPIDER_SQL_NOT_LIKE_STR) - 1)
#define SPIDER_SQL_AS_CHAR_STR " as char"
#define SPIDER_SQL_AS_CHAR_LEN (sizeof(SPIDER_SQL_AS_CHAR_STR) - 1)
#define SPIDER_SQL_CAST_STR "cast("
#define SPIDER_SQL_CAST_LEN (sizeof(SPIDER_SQL_CAST_STR) - 1)
#define SPIDER_SQL_AS_DATETIME_STR " as datetime"
#define SPIDER_SQL_AS_DATETIME_LEN (sizeof(SPIDER_SQL_AS_DATETIME_STR) - 1)
#define SPIDER_SQL_AS_DECIMAL_STR " as decimal"
#define SPIDER_SQL_AS_DECIMAL_LEN (sizeof(SPIDER_SQL_AS_DECIMAL_STR) - 1)
#define SPIDER_SQL_AS_SIGNED_STR " as signed"
#define SPIDER_SQL_AS_SIGNED_LEN (sizeof(SPIDER_SQL_AS_SIGNED_STR) - 1)
#define SPIDER_SQL_AS_UNSIGNED_STR " as unsigned"
#define SPIDER_SQL_AS_UNSIGNED_LEN (sizeof(SPIDER_SQL_AS_UNSIGNED_STR) - 1)
#define SPIDER_SQL_AS_DATE_STR " as date"
#define SPIDER_SQL_AS_DATE_LEN (sizeof(SPIDER_SQL_AS_DATE_STR) - 1)
#define SPIDER_SQL_AS_TIME_STR " as time"
#define SPIDER_SQL_AS_TIME_LEN (sizeof(SPIDER_SQL_AS_TIME_STR) - 1)
#define SPIDER_SQL_AS_BINARY_STR " as binary"
#define SPIDER_SQL_AS_BINARY_LEN (sizeof(SPIDER_SQL_AS_BINARY_STR) - 1)
#define SPIDER_SQL_AS_FLOAT_STR " as float"
#define SPIDER_SQL_AS_FLOAT_LEN (sizeof(SPIDER_SQL_AS_FLOAT_STR) - 1)
#define SPIDER_SQL_IS_TRUE_STR " is true"
#define SPIDER_SQL_IS_TRUE_LEN (sizeof(SPIDER_SQL_IS_TRUE_STR) - 1)
#define SPIDER_SQL_IS_NOT_TRUE_STR " is not true"
#define SPIDER_SQL_IS_NOT_TRUE_LEN (sizeof(SPIDER_SQL_IS_NOT_TRUE_STR) - 1)
#define SPIDER_SQL_IS_FALSE_STR " is false"
#define SPIDER_SQL_IS_FALSE_LEN (sizeof(SPIDER_SQL_IS_FALSE_STR) - 1)
#define SPIDER_SQL_IS_NOT_FALSE_STR " is not false"
#define SPIDER_SQL_IS_NOT_FALSE_LEN (sizeof(SPIDER_SQL_IS_NOT_FALSE_STR) - 1)
#define SPIDER_SQL_NULL_CHAR_STR ""
#define SPIDER_SQL_NULL_CHAR_LEN (sizeof(SPIDER_SQL_NULL_CHAR_STR) - 1)
#define SPIDER_SQL_CREATE_TABLE_STR "create table "
#define SPIDER_SQL_CREATE_TABLE_LEN (sizeof(SPIDER_SQL_CREATE_TABLE_STR) - 1)
#define SPIDER_SQL_DEFAULT_CHARSET_STR " default charset "
#define SPIDER_SQL_DEFAULT_CHARSET_LEN (sizeof(SPIDER_SQL_DEFAULT_CHARSET_STR) - 1)
#define SPIDER_SQL_CHARACTER_SET_STR " character set "
#define SPIDER_SQL_CHARACTER_SET_LEN (sizeof(SPIDER_SQL_CHARACTER_SET_STR) - 1)
#define SPIDER_SQL_COLLATE_STR " collate "
#define SPIDER_SQL_COLLATE_LEN (sizeof(SPIDER_SQL_COLLATE_STR) - 1)
#define SPIDER_SQL_COMMENT_STR " comment "
#define SPIDER_SQL_COMMENT_LEN (sizeof(SPIDER_SQL_COMMENT_STR) - 1)
#define SPIDER_SQL_CONNECTION_STR " connection "
#define SPIDER_SQL_CONNECTION_LEN (sizeof(SPIDER_SQL_CONNECTION_STR) - 1)
#define SPIDER_SQL_LCL_NAME_QUOTE_STR "`"
#define SPIDER_SQL_LCL_NAME_QUOTE_LEN (sizeof(SPIDER_SQL_LCL_NAME_QUOTE_STR) - 1)
#define SPIDER_SQL_MIN_STR "min"
#define SPIDER_SQL_MIN_LEN (sizeof(SPIDER_SQL_MIN_STR) - 1)

#define SPIDER_SQL_LOP_CHK_PRM_PRF_STR "spider_lc_"
#define SPIDER_SQL_LOP_CHK_PRM_PRF_LEN (sizeof(SPIDER_SQL_LOP_CHK_PRM_PRF_STR) - 1)

#define SPIDER_CONN_KIND_MYSQL (1 << 0)
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#define SPIDER_CONN_KIND_HS_READ (1 << 2)
#define SPIDER_CONN_KIND_HS_WRITE (1 << 3)
#endif

#define SPIDER_SQL_KIND_SQL (1 << 0)
#define SPIDER_SQL_KIND_HANDLER (1 << 1)
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#define SPIDER_SQL_KIND_HS (1 << 2)
#endif

#define SPIDER_SQL_TYPE_SELECT_SQL (1 << 0)
#define SPIDER_SQL_TYPE_INSERT_SQL (1 << 1)
#define SPIDER_SQL_TYPE_UPDATE_SQL (1 << 2)
#define SPIDER_SQL_TYPE_DELETE_SQL (1 << 3)
#define SPIDER_SQL_TYPE_BULK_UPDATE_SQL (1 << 4)
#define SPIDER_SQL_TYPE_TMP_SQL (1 << 5)
#define SPIDER_SQL_TYPE_DROP_TMP_TABLE_SQL (1 << 6)
#define SPIDER_SQL_TYPE_OTHER_SQL (1 << 7)
#define SPIDER_SQL_TYPE_HANDLER (1 << 8)
#define SPIDER_SQL_TYPE_SELECT_HS (1 << 9)
#define SPIDER_SQL_TYPE_INSERT_HS (1 << 10)
#define SPIDER_SQL_TYPE_UPDATE_HS (1 << 11)
#define SPIDER_SQL_TYPE_DELETE_HS (1 << 12)
#define SPIDER_SQL_TYPE_OTHER_HS (1 << 13)

enum spider_bulk_upd_start {
  SPD_BU_NOT_START,
  SPD_BU_START_BY_INDEX_OR_RND_INIT,
  SPD_BU_START_BY_BULK_INIT
};

enum spider_index_rnd_init {
  SPD_NONE,
  SPD_INDEX,
  SPD_RND
};

struct st_spider_ft_info;
struct st_spider_result;
typedef struct st_spider_transaction SPIDER_TRX;
typedef struct st_spider_share SPIDER_SHARE;
class ha_spider;
class spider_db_copy_table;
class spider_db_handler;

class spider_string
{
public:
  bool mem_calc_inited;
  String str;
  uint id;
  const char *func_name;
  const char *file_name;
  ulong line_no;
  uint32 current_alloc_mem;
  spider_string *next;

  spider_string();
  spider_string(
    uint32 length_arg
  );
  spider_string(
    const char *str,
    CHARSET_INFO *cs
  );
  spider_string(
    const char *str,
    uint32 len,
    CHARSET_INFO *cs
  );
  spider_string(
    char *str,
    uint32 len,
    CHARSET_INFO *cs
  );
  spider_string(
    const String &str
  );
  ~spider_string();
  void init_mem_calc(
    uint id,
    const char *func_name,
    const char *file_name,
    ulong line_no
  );
  void mem_calc();
  String *get_str();
  void set_charset(
    CHARSET_INFO *charset_arg
  );
  CHARSET_INFO *charset() const;
  uint32 length() const;
  uint32 alloced_length() const;
  char &operator [] (
    uint32 i
  ) const;
  void length(
    uint32 len
  );
  bool is_empty() const;
  const char *ptr() const;
  char *c_ptr();
  char *c_ptr_quick();
  char *c_ptr_safe();
  LEX_STRING lex_string() const;
  void set(
    String &str,
    uint32 offset,
    uint32 arg_length
  );
  void set(
    char *str,
    uint32 arg_length,
    CHARSET_INFO *cs
  );
  void set(
    const char *str,
    uint32 arg_length,
    CHARSET_INFO *cs
  );
  bool set_ascii(
    const char *str,
    uint32 arg_length
  );
  void set_quick(
    char *str,
    uint32 arg_length,
    CHARSET_INFO *cs
  );
  bool set_int(
    longlong num,
    bool unsigned_flag,
    CHARSET_INFO *cs
  );
  bool set(
    longlong num,
    CHARSET_INFO *cs
  );
  bool set(
    ulonglong num,
    CHARSET_INFO *cs
  );
  bool set_real(
    double num,
    uint decimals,
    CHARSET_INFO *cs
  );
  void chop();
  void free();
  bool alloc(
    uint32 arg_length
  );
  bool real_alloc(
    uint32 arg_length
  );
  bool realloc(
    uint32 arg_length
  );
  void shrink(
    uint32 arg_length
  );
  bool is_alloced();
  spider_string& operator = (
    const String &s
  );
  bool copy();
  bool copy(
    const spider_string &s
  );
  bool copy(
    const String &s
  );
  bool copy(
    const char *s,
    uint32 arg_length,
    CHARSET_INFO *cs
  );
  bool needs_conversion(
    uint32 arg_length,
    CHARSET_INFO *cs_from,
    CHARSET_INFO *cs_to,
    uint32 *offset
  );
  bool copy_aligned(
    const char *s,
    uint32 arg_length,
    uint32 offset,
    CHARSET_INFO *cs
  );
  bool set_or_copy_aligned(
    const char *s,
    uint32 arg_length,
    CHARSET_INFO *cs
  );
  bool copy(
    const char *s,
    uint32 arg_length,
    CHARSET_INFO *csfrom,
    CHARSET_INFO *csto,
    uint *errors
  );
  bool append(
    const spider_string &s
  );
  bool append(
    const String &s
  );
  bool append(
    const char *s
  );
  bool append(
    LEX_STRING *ls
  );
  bool append(
    const char *s,
    uint32 arg_length
  );
  bool append(
    const char *s,
    uint32 arg_length,
    CHARSET_INFO *cs
  );
  bool append_ulonglong(
    ulonglong val
  );
  bool append(
    IO_CACHE *file,
    uint32 arg_length
  );
  bool append_with_prefill(
    const char *s,
    uint32 arg_length,
    uint32 full_length,
    char fill_char
  );
  int strstr(
    const String &search,
    uint32 offset = 0
  );
  int strrstr(
    const String &search,
    uint32 offset = 0
  );
  bool replace(
    uint32 offset,
    uint32 arg_length,
    const char *to,
    uint32 length
  );
  bool replace(
    uint32 offset,
    uint32 arg_length,
    const String &to
  );
  inline bool append(
    char chr
  );
  bool fill(
    uint32 max_length,
    char fill
  );
  void strip_sp();
  uint32 numchars();
  int charpos(
    int i,
    uint32 offset=0
  );
  int reserve(
    uint32 space_needed
  );
  int reserve(
    uint32 space_needed,
    uint32 grow_by
  );
  void q_append(
    const char c
  );
  void q_append(
    const uint32 n
  );
  void q_append(
    double d
  );
  void q_append(
    double *d
  );
  void q_append(
    const char *data,
    uint32 data_len
  );
  void write_at_position(
    int position,
    uint32 value
  );
  void qs_append(
    const char *str,
    uint32 len
  );
  void qs_append(
    double d
  );
  void qs_append(
    double *d
  );
  void qs_append(
    const char c
  );
  void qs_append(
    int i
  );
  void qs_append(
    uint i
  );
  char *prep_append(
    uint32 arg_length,
    uint32 step_alloc
  );
  bool append(
    const char *s,
    uint32 arg_length,
    uint32 step_alloc
  );
  void append_escape_string(
    const char *st,
    uint len
  );
  void append_escape_string(
    const char *st,
    uint len,
    CHARSET_INFO *cs
  );
  bool append_for_single_quote(
    const char *st,
    uint len
  );
  bool append_for_single_quote(
    const String *s
  );
  bool append_for_single_quote(
    const char *st
  );
  void print(
    String *print
  );
  void swap(
    spider_string &s
  );
  bool uses_buffer_owned_by(
    const String *s
  ) const;
  bool is_ascii() const;
};

typedef struct spider_table_link_idx_holder SPIDER_TABLE_LINK_IDX_HOLDER;
typedef struct spider_table_holder SPIDER_TABLE_HOLDER;

typedef struct spider_link_idx_holder
{
  spider_table_link_idx_holder *table_link_idx_holder;
  int link_idx;
  int link_status;
  spider_link_idx_holder *next_table;
  spider_link_idx_holder *next;
} SPIDER_LINK_IDX_HOLDER;

typedef struct spider_link_idx_chain
{
  SPIDER_CONN *conn;
  spider_link_idx_holder *link_idx_holder;
  spider_link_idx_holder *current_link_idx_holder;
  int link_status;
  spider_link_idx_chain *next;
} SPIDER_LINK_IDX_CHAIN;

typedef struct spider_table_link_idx_holder
{
  spider_table_holder *table_holder;
  spider_link_idx_holder *first_link_idx_holder;
  spider_link_idx_holder *last_link_idx_holder;
  spider_link_idx_holder *current_link_idx_holder;
  uint link_idx_holder_count;
} SPIDER_TABLE_LINK_IDX_HOLDER;

typedef struct spider_conn_holder
{
  SPIDER_CONN *conn;
  spider_table_link_idx_holder *table_link_idx_holder;
  uint link_idx_holder_count_max;
  bool checked_for_same_conn;
  long access_balance;
  spider_conn_holder *prev;
  spider_conn_holder *next;
} SPIDER_CONN_HOLDER;

typedef struct spider_table_holder
{
  TABLE *table;
  ha_spider *spider;
  spider_string *alias;
} SPIDER_TABLE_HOLDER;

typedef struct spider_field_holder
{
  Field *field;
  ha_spider *spider;
  spider_string *alias;
  spider_field_holder *next;
} SPIDER_FIELD_HOLDER;

typedef struct spider_field_chain
{
  spider_field_holder *field_holder;
  spider_field_chain *next;
} SPIDER_FIELD_CHAIN;

class spider_fields
{
  uint dbton_count;
  uint current_dbton_num;
  uint dbton_ids[SPIDER_DBTON_SIZE];
  uint table_count;
  uint current_table_num;
  SPIDER_TABLE_HOLDER *table_holder;
  SPIDER_LINK_IDX_CHAIN *first_link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *last_link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *current_link_idx_chain;
  SPIDER_LINK_IDX_CHAIN *first_ok_link_idx_chain;
  SPIDER_CONN_HOLDER *first_conn_holder;
  SPIDER_CONN_HOLDER *last_conn_holder;
  SPIDER_CONN_HOLDER *current_conn_holder;
  SPIDER_FIELD_HOLDER *first_field_holder;
  SPIDER_FIELD_HOLDER *last_field_holder;
  SPIDER_FIELD_HOLDER *current_field_holder;
  SPIDER_FIELD_CHAIN *first_field_chain;
  SPIDER_FIELD_CHAIN *last_field_chain;
  SPIDER_FIELD_CHAIN *current_field_chain;
  Field **first_field_ptr;
  Field **current_field_ptr;
public:
  spider_fields();
  virtual ~spider_fields();
  void add_dbton_id(
    uint dbton_id_arg
  );
  void set_pos_to_first_dbton_id();
  uint get_next_dbton_id();
  int make_link_idx_chain(
    int link_status
  );
  SPIDER_LINK_IDX_CHAIN *create_link_idx_chain();
  void set_pos_to_first_link_idx_chain();
  SPIDER_LINK_IDX_CHAIN *get_next_link_idx_chain();
  SPIDER_LINK_IDX_HOLDER *get_dup_link_idx_holder(
    SPIDER_TABLE_LINK_IDX_HOLDER *table_link_idx_holder,
    SPIDER_LINK_IDX_HOLDER *current
  );
  bool check_link_ok_chain();
  bool is_first_link_ok_chain(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
  );
  int get_ok_link_idx();
  void set_first_link_idx();
  int add_link_idx(
    SPIDER_CONN_HOLDER *conn_holder_arg,
    ha_spider *spider_arg,
    int link_idx
  );
  SPIDER_LINK_IDX_HOLDER *create_link_idx_holder();
  void set_pos_to_first_table_on_link_idx_chain(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
  );
  SPIDER_LINK_IDX_HOLDER *get_next_table_on_link_idx_chain(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
  );
  SPIDER_CONN_HOLDER *add_conn(
    SPIDER_CONN *conn_arg,
    long access_balance
  );
  SPIDER_CONN_HOLDER *create_conn_holder();
  void set_pos_to_first_conn_holder();
  SPIDER_CONN_HOLDER *get_next_conn_holder();
  bool has_conn_holder();
  void clear_conn_holder_from_conn();
  bool check_conn_same_conn(
    SPIDER_CONN *conn_arg
  );
  bool remove_conn_if_not_checked();
  void check_support_dbton(
    uchar *dbton_bitmap
  );
  void choose_a_conn();
  void free_conn_holder(
    SPIDER_CONN_HOLDER *conn_holder_arg
  );
  SPIDER_TABLE_HOLDER *add_table(
    ha_spider *spider_arg
  );
  bool all_query_fields_are_query_table_members();
  int create_table_holder(
    uint table_count_arg
  );
  void set_pos_to_first_table_holder();
  SPIDER_TABLE_HOLDER *get_next_table_holder();
  SPIDER_TABLE_HOLDER *get_table_holder(TABLE *table);
  uint get_table_count();
  int add_field(Field *field_arg);
  SPIDER_FIELD_HOLDER *create_field_holder();
  void set_pos_to_first_field_holder();
  SPIDER_FIELD_HOLDER *get_next_field_holder();
  SPIDER_FIELD_CHAIN *create_field_chain();
  void set_pos_to_first_field_chain();
  SPIDER_FIELD_CHAIN *get_next_field_chain();
  void set_field_ptr(Field **field_arg);
  Field **get_next_field_ptr();
  int ping_table_mon_from_table(
    SPIDER_LINK_IDX_CHAIN *link_idx_chain
  );
};

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#define SPIDER_HS_UINT32_INFO dena::uint32_info
#define SPIDER_HS_STRING_REF dena::string_ref
#ifndef HANDLERSOCKET_MYSQL_UTIL
#define SPIDER_HS_VECTOR std::vector
class spider_db_hs_string_ref_buffer
{
  SPIDER_HS_VECTOR<SPIDER_HS_STRING_REF> hs_conds;
public:
  spider_db_hs_string_ref_buffer();
  ~spider_db_hs_string_ref_buffer();
  int init();
  void clear();
  int push_back(
    SPIDER_HS_STRING_REF &cond
  );
  SPIDER_HS_STRING_REF *ptr();
  uint size();
};
#else
class spider_db_hs_string_ref_buffer
{
  bool                    hs_da_init;
  DYNAMIC_ARRAY           hs_conds;
  uint                    hs_conds_id;
  const char              *hs_conds_func_name;
  const char              *hs_conds_file_name;
  ulong                   hs_conds_line_no;
public:
  spider_db_hs_string_ref_buffer();
  ~spider_db_hs_string_ref_buffer();
  int init();
  void clear();
  int push_back(
    SPIDER_HS_STRING_REF &cond
  );
  SPIDER_HS_STRING_REF *ptr();
  uint size();
};
#endif

class spider_db_hs_str_buffer
{
  bool                    hs_da_init;
  DYNAMIC_ARRAY           hs_conds;
  uint                    hs_conds_id;
  const char              *hs_conds_func_name;
  const char              *hs_conds_file_name;
  ulong                   hs_conds_line_no;
public:
  spider_db_hs_str_buffer();
  ~spider_db_hs_str_buffer();
  int init();
  void clear();
  spider_string *add(
    uint *strs_pos,
    const char *str,
    uint str_len
  );
};

#define SPIDER_DB_HS_STRING_REF_BUFFER spider_db_hs_string_ref_buffer
#define SPIDER_DB_HS_STR_BUFFER spider_db_hs_str_buffer
#endif

struct st_spider_db_request_key
{
  ulonglong                spider_thread_id;
  query_id_t               query_id;
  void                     *handler;
  ulonglong                request_id;
  st_spider_db_request_key *next;
};

class spider_db_util
{
public:
  uint dbton_id;
  spider_db_util() {}
  virtual ~spider_db_util() {}
  virtual int append_name(
    spider_string *str,
    const char *name,
    uint name_length
  ) = 0;
  virtual int append_name_with_charset(
    spider_string *str,
    const char *name,
    uint name_length,
    CHARSET_INFO *name_charset
  ) = 0;
  virtual int append_escaped_name(
    spider_string *str,
    const char *name,
    uint name_length
  ) = 0;
  virtual int append_escaped_name_with_charset(
    spider_string *str,
    const char *name,
    uint name_length,
    CHARSET_INFO *name_charset
  ) = 0;
  virtual bool is_name_quote(
    const char head_code
  ) = 0;
  virtual int append_escaped_name_quote(
    spider_string *str
  ) = 0;
  virtual int append_column_value(
    ha_spider *spider,
    spider_string *str,
    Field *field,
    const uchar *new_ptr,
    CHARSET_INFO *access_charset
  ) = 0;
  virtual int append_trx_isolation(
    spider_string *str,
    int trx_isolation
  ) = 0;
  virtual int append_autocommit(
    spider_string *str,
    bool autocommit
  ) = 0;
  virtual int append_sql_log_off(
    spider_string *str,
    bool sql_log_off
  ) = 0;
  virtual int append_wait_timeout(
    spider_string *str,
    int wait_timeout
  ) = 0;
  virtual int append_sql_mode(
    spider_string *str,
    sql_mode_t sql_mode
  ) = 0;
  virtual int append_time_zone(
    spider_string *str,
    Time_zone *time_zone
  ) = 0;
  virtual int append_loop_check(
    spider_string *str,
    SPIDER_CONN *conn
  );
  virtual int append_start_transaction(
    spider_string *str
  ) = 0;
  virtual int append_xa_start(
    spider_string *str,
    XID *xid
  ) = 0;
  virtual int append_lock_table_head(
    spider_string *str
  ) = 0;
  virtual int append_lock_table_body(
    spider_string *str,
    const char *db_name,
    uint db_name_length,
    CHARSET_INFO *db_name_charset,
    const char *table_name,
    uint table_name_length,
    CHARSET_INFO *table_name_charset,
    int lock_type
  ) = 0;
  virtual int append_lock_table_tail(
    spider_string *str
  ) = 0;
  virtual int append_unlock_table(
    spider_string *str
  ) = 0;
  virtual int open_item_func(
    Item_func *item_func,
    ha_spider *spider,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  ) = 0;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  virtual int open_item_sum_func(
    Item_sum *item_sum,
    ha_spider *spider,
    spider_string *str,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields
  ) = 0;
#endif
  virtual int append_escaped_util(
    spider_string *to,
    String *from
  ) = 0;
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  virtual int append_from_and_tables(
    ha_spider *spider,
    spider_fields *fields,
    spider_string *str,
    TABLE_LIST *table_list,
    uint table_count
  ) = 0;
  virtual int reappend_tables(
    spider_fields *fields,
    SPIDER_LINK_IDX_CHAIN *link_idx_chain,
    spider_string *str
  ) = 0;
  virtual int append_where(
    spider_string *str
  ) = 0;
  virtual int append_having(
    spider_string *str
  ) = 0;
#endif
  virtual bool tables_on_different_db_are_joinable();
  virtual bool socket_has_default_value();
  virtual bool database_has_default_value();
  virtual bool default_file_has_default_value();
  virtual bool host_has_default_value();
  virtual bool port_has_default_value();
  virtual bool append_charset_name_before_string();
  virtual uint limit_mode();
};

class spider_db_row
{
public:
  uint dbton_id;
  SPIDER_DB_ROW *next_pos;
  spider_db_row(uint in_dbton_id) : dbton_id(in_dbton_id), next_pos(NULL) {}
  virtual ~spider_db_row() {}
  virtual int store_to_field(
    Field *field,
    CHARSET_INFO *access_charset
  ) = 0;
  virtual int append_to_str(
    spider_string *str
  ) = 0;
  virtual int append_escaped_to_str(
    spider_string *str,
    uint dbton_id
  ) = 0;
  virtual void first() = 0;
  virtual void next() = 0;
  virtual bool is_null() = 0;
  virtual int val_int() = 0;
  virtual double val_real() = 0;
  virtual my_decimal *val_decimal(
    my_decimal *decimal_value,
    CHARSET_INFO *access_charset
  ) = 0;
  virtual SPIDER_DB_ROW *clone() = 0;
  virtual int store_to_tmp_table(
    TABLE *tmp_table,
    spider_string *str
  ) = 0;
  virtual uint get_byte_size() = 0;
};

class spider_db_result_buffer
{
public:
  spider_db_result_buffer() {}
  virtual ~spider_db_result_buffer() {}
  virtual void clear() = 0;
  virtual bool check_size(
    longlong size
  ) = 0;
};

class spider_db_result
{
public:
  SPIDER_DB_CONN *db_conn;
  uint           dbton_id;
  spider_db_result(SPIDER_DB_CONN *in_db_conn);
  virtual ~spider_db_result() {}
  virtual void set_limit(longlong value) {}
  virtual bool has_result() = 0;
  virtual void free_result() = 0;
  virtual SPIDER_DB_ROW *current_row() = 0;
  virtual SPIDER_DB_ROW *fetch_row() = 0;
  virtual SPIDER_DB_ROW *fetch_row_from_result_buffer(
    spider_db_result_buffer *spider_res_buf
  ) = 0;
  virtual SPIDER_DB_ROW *fetch_row_from_tmp_table(
    TABLE *tmp_table
  ) = 0;
  virtual int fetch_table_status(
    int mode,
    ha_statistics &stat
  ) = 0;
  virtual int fetch_table_records(
    int mode,
    ha_rows &records
  ) = 0;
#ifdef HA_HAS_CHECKSUM_EXTENDED
  virtual int fetch_table_checksum(
    ha_spider *spider
  );
#endif
  virtual int fetch_table_cardinality(
    int mode,
    TABLE *table,
    longlong *cardinality,
    uchar *cardinality_upd,
    int bitmap_size
  ) = 0;
  virtual int fetch_table_mon_status(
    int &status
  ) = 0;
  virtual longlong num_rows() = 0;
  virtual uint num_fields() = 0;
  virtual void move_to_pos(
    longlong pos
  ) = 0;
  virtual int get_errno() = 0;
#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
  virtual int fetch_columns_for_discover_table_structure(
    spider_string *str,
    CHARSET_INFO *access_charset
  ) = 0;
  virtual int fetch_index_for_discover_table_structure(
    spider_string *str,
    CHARSET_INFO *access_charset
  ) = 0;
  virtual int fetch_table_for_discover_table_structure(
    spider_string *str,
    SPIDER_SHARE *spider_share,
    CHARSET_INFO *access_charset
  ) = 0;
#endif
  virtual uint limit_mode();
};

class spider_db_conn
{
public:
  SPIDER_CONN    *conn;
  uint           dbton_id;
  spider_db_conn(
    SPIDER_CONN *in_conn
  );
  virtual ~spider_db_conn() {}
  virtual int init() = 0;
  virtual void set_limit(longlong value) {}
  virtual bool is_connected() = 0;
  virtual void bg_connect() = 0;
  virtual int connect(
    char *tgt_host,
    char *tgt_username,
    char *tgt_password,
    long tgt_port,
    char *tgt_socket,
    char *server_name,
    int connect_retry_count,
    longlong connect_retry_interval
  ) = 0;
  virtual int ping() = 0;
  virtual void bg_disconnect() = 0;
  virtual void disconnect() = 0;
  virtual int set_net_timeout() = 0;
  virtual int exec_query(
    const char *query,
    uint length,
    int quick_mode
  ) = 0;
  virtual int get_errno() = 0;
  virtual const char *get_error() = 0;
  virtual bool is_server_gone_error(
    int error_num
  ) = 0;
  virtual bool is_dup_entry_error(
    int error_num
  ) = 0;
  virtual bool is_xa_nota_error(
    int error_num
  ) = 0;
  virtual spider_db_result *store_result(
    spider_db_result_buffer **spider_res_buf,
    st_spider_db_request_key *request_key,
    int *error_num
  ) = 0;
  virtual spider_db_result *use_result(
    ha_spider *spider,
    st_spider_db_request_key *request_key,
    int *error_num
  ) = 0;
  virtual int next_result() = 0;
  virtual uint affected_rows() = 0;
  virtual uint matched_rows() = 0;
  virtual bool inserted_info(
    spider_db_handler *handler,
    ha_copy_info *copy_info
  ) = 0;
  virtual ulonglong last_insert_id() = 0;
  virtual int set_character_set(
    const char *csname
  ) = 0;
  virtual int select_db(
    const char *dbname
  ) = 0;
  virtual int consistent_snapshot(
    int *need_mon
  ) = 0;
  virtual bool trx_start_in_bulk_sql() = 0;
  virtual int start_transaction(
    int *need_mon
  ) = 0;
  virtual int commit(
    int *need_mon
  ) = 0;
  virtual int rollback(
    int *need_mon
  ) = 0;
  virtual bool xa_start_in_bulk_sql() = 0;
  virtual int xa_start(
    XID *xid,
    int *need_mon
  ) = 0;
  virtual int xa_end(
    XID *xid,
    int *need_mon
  ) = 0;
  virtual int xa_prepare(
    XID *xid,
    int *need_mon
  ) = 0;
  virtual int xa_commit(
    XID *xid,
    int *need_mon
  ) = 0;
  virtual int xa_rollback(
    XID *xid,
    int *need_mon
  ) = 0;
  virtual bool set_trx_isolation_in_bulk_sql() = 0;
  virtual int set_trx_isolation(
    int trx_isolation,
    int *need_mon
  ) = 0;
  virtual bool set_autocommit_in_bulk_sql() = 0;
  virtual int set_autocommit(
    bool autocommit,
    int *need_mon
  ) = 0;
  virtual bool set_sql_log_off_in_bulk_sql() = 0;
  virtual int set_sql_log_off(
    bool sql_log_off,
    int *need_mon
  ) = 0;
  virtual bool set_wait_timeout_in_bulk_sql() = 0;
  virtual int set_wait_timeout(
    int wait_timeout,
    int *need_mon
  ) = 0;
  virtual bool set_sql_mode_in_bulk_sql() = 0;
  virtual int set_sql_mode(
    sql_mode_t sql_mode,
    int *need_mon
  ) = 0;
  virtual bool set_time_zone_in_bulk_sql() = 0;
  virtual int set_time_zone(
    Time_zone *time_zone,
    int *need_mon
  ) = 0;
  virtual bool set_loop_check_in_bulk_sql();
  virtual int set_loop_check(
    int *need_mon
  );
  virtual int fin_loop_check();
  virtual int show_master_status(
    SPIDER_TRX *trx,
    SPIDER_SHARE *share,
    int all_link_idx,
    int *need_mon,
    TABLE *table,
    spider_string *str,
    int mode,
    SPIDER_DB_RESULT **res1,
    SPIDER_DB_RESULT **res2
  ) = 0;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  virtual int append_sql(
    char *sql,
    ulong sql_length,
    st_spider_db_request_key *request_key
  ) = 0;
  virtual int append_open_handler(
    uint handler_id,
    const char *db_name,
    const char *table_name,
    const char *index_name,
    const char *sql,
    st_spider_db_request_key *request_key
  ) = 0;
  virtual int append_select(
    uint handler_id,
    spider_string *sql,
    SPIDER_DB_HS_STRING_REF_BUFFER *keys,
    int limit,
    int skip,
    st_spider_db_request_key *request_key
  ) = 0;
  virtual int append_insert(
    uint handler_id,
    SPIDER_DB_HS_STRING_REF_BUFFER *upds,
    st_spider_db_request_key *request_key
  ) = 0;
  virtual int append_update(
    uint handler_id,
    spider_string *sql,
    SPIDER_DB_HS_STRING_REF_BUFFER *keys,
    SPIDER_DB_HS_STRING_REF_BUFFER *upds,
    int limit,
    int skip,
    bool increment,
    bool decrement,
    st_spider_db_request_key *request_key
  ) = 0;
  virtual int append_delete(
    uint handler_id,
    spider_string *sql,
    SPIDER_DB_HS_STRING_REF_BUFFER *keys,
    int limit,
    int skip,
    st_spider_db_request_key *request_key
  ) = 0;
  virtual void reset_request_queue() = 0;
#endif
  virtual size_t escape_string(
    char *to,
    const char *from,
    size_t from_length
  ) = 0;
  virtual bool have_lock_table_list() = 0;
  virtual int append_lock_tables(
    spider_string *str
  ) = 0;
  virtual int append_unlock_tables(
    spider_string *str
  ) = 0;
  virtual uint get_lock_table_hash_count() = 0;
  virtual void reset_lock_table_hash() = 0;
  virtual uint get_opened_handler_count() = 0;
  virtual void reset_opened_handler() = 0;
  virtual void set_dup_key_idx(
    ha_spider *spider,
    int link_idx
  ) = 0;
  virtual bool cmp_request_key_to_snd(
    st_spider_db_request_key *request_key
  ) = 0;
  virtual uint limit_mode();
};

class spider_db_share
{
protected:
  uint               mem_calc_id;
  const char         *mem_calc_func_name;
  const char         *mem_calc_file_name;
  ulong              mem_calc_line_no;
public:
  uint dbton_id;
  st_spider_share *spider_share;
  spider_db_share(
    st_spider_share *share,
    uint dbton_id
  ) : dbton_id(dbton_id), spider_share(share) {}
  virtual ~spider_db_share() {}
  virtual int init() = 0;
  virtual uint get_column_name_length(
    uint field_index
  ) = 0;
  virtual int append_column_name(
    spider_string *str,
    uint field_index
  ) = 0;
  virtual int append_column_name_with_alias(
    spider_string *str,
    uint field_index,
    const char *alias,
    uint alias_length
  ) = 0;
  virtual bool need_change_db_table_name() = 0;
#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
  virtual int discover_table_structure(
    SPIDER_TRX *trx,
    SPIDER_SHARE *spider_share,
    spider_string *str
  ) = 0;
#endif
#ifdef HA_HAS_CHECKSUM_EXTENDED
  virtual bool checksum_support();
#endif
};

class spider_db_handler
{
protected:
  uint               mem_calc_id;
  const char         *mem_calc_func_name;
  const char         *mem_calc_file_name;
  ulong              mem_calc_line_no;
public:
  uint dbton_id;
  ha_spider *spider;
  spider_db_share *db_share;
  int first_link_idx;
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
#endif
  bool strict_group_by;
  bool no_where_cond;
  spider_db_handler(ha_spider *spider, spider_db_share *db_share) :
    dbton_id(db_share->dbton_id), spider(spider), db_share(db_share),
    first_link_idx(-1) {}
  virtual ~spider_db_handler() {}
  virtual int init() = 0;
  virtual int append_index_hint(
    spider_string *str,
    int link_idx,
    ulong sql_type
    ) = 0;
  virtual int append_table_name_with_adjusting(
    spider_string *str,
    int link_idx,
    ulong sql_type
  ) = 0;
  virtual int append_tmp_table_and_sql_for_bka(
    const key_range *start_key
  ) = 0;
  virtual int reuse_tmp_table_and_sql_for_bka() = 0;
  virtual int append_union_table_and_sql_for_bka(
    const key_range *start_key
  ) = 0;
  virtual int reuse_union_table_and_sql_for_bka() = 0;
  virtual int append_insert_for_recovery(
    ulong sql_type,
    int link_idx
  ) = 0;
  virtual int append_update(
    const TABLE *table,
    my_ptrdiff_t ptr_diff
  ) = 0;
  virtual int append_update(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    int link_idx
  ) = 0;
  virtual int append_delete(
    const TABLE *table,
    my_ptrdiff_t ptr_diff
  ) = 0;
  virtual int append_delete(
    const TABLE *table,
    my_ptrdiff_t ptr_diff,
    int link_idx
  ) = 0;
  virtual int append_insert_part() = 0;
  virtual int append_update_part() = 0;
  virtual int append_delete_part() = 0;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  virtual int append_increment_update_set_part() = 0;
#endif
#endif
  virtual int append_update_set_part() = 0;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  virtual int append_direct_update_set_part() = 0;
  virtual int append_dup_update_pushdown_part(
    const char *alias,
    uint alias_length
  ) = 0;
  virtual int append_update_columns_part(
    const char *alias,
    uint alias_length
  ) = 0;
  virtual int check_update_columns_part() = 0;
#endif
  virtual int append_select_part(
    ulong sql_type
  ) = 0;
  virtual int append_select(
    spider_string *str,
    ulong sql_type
  ) = 0;
  virtual int append_table_select_part(
    ulong sql_type
  ) = 0;
  virtual int append_key_select_part(
    ulong sql_type,
    uint idx
  ) = 0;
  virtual int append_minimum_select_part(
    ulong sql_type
  ) = 0;
  virtual int append_hint_after_table_part(
    ulong sql_type
  ) = 0;
  virtual void set_where_pos(
    ulong sql_type
  ) = 0;
  virtual void set_where_to_pos(
    ulong sql_type
  ) = 0;
  virtual int check_item_type(
    Item *item
  ) = 0;
  virtual int append_values_connector_part(
    ulong sql_type
  ) = 0;
  virtual int append_values_terminator_part(
    ulong sql_type
  ) = 0;
  virtual int append_union_table_connector_part(
    ulong sql_type
  ) = 0;
  virtual int append_union_table_terminator_part(
    ulong sql_type
  ) = 0;
  virtual int append_key_column_values_part(
    const key_range *start_key,
    ulong sql_type
  ) = 0;
  virtual int append_key_column_values_with_name_part(
    const key_range *start_key,
    ulong sql_type
  ) = 0;
  virtual int append_key_where_part(
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type
  ) = 0;
  virtual int append_is_null_part(
    ulong sql_type,
    KEY_PART_INFO *key_part,
    const key_range *key,
    const uchar **ptr,
    bool key_eq,
    bool tgt_final
  ) = 0;
  virtual int append_where_terminator_part(
    ulong sql_type,
    bool set_order,
    int key_count
  ) = 0;
  virtual int append_match_where_part(
    ulong sql_type
  ) = 0;
  virtual int append_condition_part(
    const char *alias,
    uint alias_length,
    ulong sql_type,
    bool test_flg
  ) = 0;
  virtual int append_match_select_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  ) = 0;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  virtual int append_sum_select_part(
    ulong sql_type,
    const char *alias,
    uint alias_length
  ) = 0;
#endif
  virtual void set_order_pos(
    ulong sql_type
  ) = 0;
  virtual void set_order_to_pos(
    ulong sql_type
  ) = 0;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  virtual int append_group_by_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) = 0;
#endif
  virtual int append_key_order_for_merge_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) = 0;
  virtual int append_key_order_for_direct_order_limit_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) = 0;
  virtual int append_key_order_with_alias_part(
    const char *alias,
    uint alias_length,
    ulong sql_type
  ) = 0;
  virtual int append_limit_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  ) = 0;
  virtual int reappend_limit_part(
    longlong offset,
    longlong limit,
    ulong sql_type
  ) = 0;
  virtual int append_select_lock_part(
    ulong sql_type
  ) = 0;
  virtual int append_union_all_start_part(
    ulong sql_type
  ) = 0;
  virtual int append_union_all_part(
    ulong sql_type
  ) = 0;
  virtual int append_union_all_end_part(
    ulong sql_type
  ) = 0;
  virtual int append_multi_range_cnt_part(
    ulong sql_type,
    uint multi_range_cnt,
    bool with_comma
  ) = 0;
  virtual int append_multi_range_cnt_with_name_part(
    ulong sql_type,
    uint multi_range_cnt
  ) = 0;
  virtual int append_open_handler_part(
    ulong sql_type,
    uint handler_id,
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int append_close_handler_part(
    ulong sql_type,
    int link_idx
  ) = 0;
  virtual int append_insert_terminator_part(
    ulong sql_type
  ) = 0;
  virtual int append_insert_values_part(
    ulong sql_type
  ) = 0;
  virtual int append_into_part(
    ulong sql_type
  ) = 0;
  virtual void set_insert_to_pos(
    ulong sql_type
  ) = 0;
  virtual int append_from_part(
    ulong sql_type,
    int link_idx
  ) = 0;
  virtual int append_delete_all_rows_part(
    ulong sql_type
  ) = 0;
  virtual int append_explain_select_part(
    const key_range *start_key,
    const key_range *end_key,
    ulong sql_type,
    int link_idx
  ) = 0;
  virtual bool is_sole_projection_field(
    uint16 field_index
  ) = 0;
  virtual bool is_bulk_insert_exec_period(
    bool bulk_end
  ) = 0;
  virtual bool sql_is_filled_up(
    ulong sql_type
  ) = 0;
  virtual bool sql_is_empty(
    ulong sql_type
  ) = 0;
  virtual bool support_multi_split_read() = 0;
  virtual bool support_bulk_update() = 0;
  virtual int bulk_tmp_table_insert() = 0;
  virtual int bulk_tmp_table_insert(
    int link_idx
  ) = 0;
  virtual int bulk_tmp_table_end_bulk_insert() = 0;
  virtual int bulk_tmp_table_rnd_init() = 0;
  virtual int bulk_tmp_table_rnd_next() = 0;
  virtual int bulk_tmp_table_rnd_end() = 0;
  virtual bool need_copy_for_update(
    int link_idx
  ) = 0;
  virtual bool bulk_tmp_table_created() = 0;
  virtual int mk_bulk_tmp_table_and_bulk_start() = 0;
  virtual void rm_bulk_tmp_table() = 0;
  virtual int insert_lock_tables_list(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int append_lock_tables_list(
    SPIDER_CONN *conn,
    int link_idx,
    int *appended
  ) = 0;
  virtual int realloc_sql(
    ulong *realloced
  ) = 0;
  virtual int reset_sql(
    ulong sql_type
  ) = 0;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  virtual int reset_keys(
    ulong sql_type
  ) = 0;
  virtual int reset_upds(
    ulong sql_type
  ) = 0;
  virtual int reset_strs(
    ulong sql_type
  ) = 0;
  virtual int reset_strs_pos(
    ulong sql_type
  ) = 0;
  virtual int push_back_upds(
    SPIDER_HS_STRING_REF &info
  ) = 0;
#endif
  virtual bool need_lock_before_set_sql_for_exec(
    ulong sql_type
  ) = 0;
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  virtual int set_sql_for_exec(
    ulong sql_type,
    int link_idx,
    SPIDER_LINK_IDX_CHAIN *link_idx_chain
  ) = 0;
#endif
  virtual int set_sql_for_exec(
    ulong sql_type,
    int link_idx
  ) = 0;
  virtual int set_sql_for_exec(
    spider_db_copy_table *tgt_ct,
    ulong sql_type
  ) = 0;
  virtual int execute_sql(
    ulong sql_type,
    SPIDER_CONN *conn,
    int quick_mode,
    int *need_mon
  ) = 0;
  virtual int reset() = 0;
  virtual int sts_mode_exchange(
    int sts_mode
  ) = 0;
  virtual int show_table_status(
    int link_idx,
    int sts_mode,
    uint flag
  ) = 0;
  virtual int crd_mode_exchange(
    int crd_mode
  ) = 0;
  virtual int show_index(
    int link_idx,
    int crd_mode
  ) = 0;
  virtual int show_records(
    int link_idx
  ) = 0;
#ifdef HA_HAS_CHECKSUM_EXTENDED
  virtual int checksum_table(
    int link_idx
  );
#endif
  virtual int show_last_insert_id(
    int link_idx,
    ulonglong &last_insert_id
  ) = 0;
  virtual ha_rows explain_select(
    const key_range *start_key,
    const key_range *end_key,
    int link_idx
  ) = 0;
  virtual int lock_tables(
    int link_idx
  ) = 0;
  virtual int unlock_tables(
    int link_idx
  ) = 0;
  virtual int disable_keys(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int enable_keys(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int check_table(
    SPIDER_CONN *conn,
    int link_idx,
    HA_CHECK_OPT* check_opt
  ) = 0;
  virtual int repair_table(
    SPIDER_CONN *conn,
    int link_idx,
    HA_CHECK_OPT* check_opt
  ) = 0;
  virtual int analyze_table(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int optimize_table(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int flush_tables(
    SPIDER_CONN *conn,
    int link_idx,
    bool lock
  ) = 0;
  virtual int flush_logs(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int insert_opened_handler(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int delete_opened_handler(
    SPIDER_CONN *conn,
    int link_idx
  ) = 0;
  virtual int sync_from_clone_source(
    spider_db_handler *dbton_hdl
  ) = 0;
  virtual bool support_use_handler(
    int use_handler
  ) = 0;
  virtual bool minimum_select_bit_is_set(
    uint field_index
  ) = 0;
  virtual void copy_minimum_select_bitmap(
    uchar *bitmap
  ) = 0;
  virtual int init_union_table_name_pos() = 0;
  virtual int set_union_table_name_pos() = 0;
  virtual int reset_union_table_name(
    spider_string *str,
    int link_idx,
    ulong sql_type
  ) = 0;
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  virtual int append_from_and_tables_part(
    spider_fields *fields,
    ulong sql_type
  ) = 0;
  virtual int reappend_tables_part(
    spider_fields *fields,
    ulong sql_type
  ) = 0;
  virtual int append_where_part(
    ulong sql_type
  ) = 0;
  virtual int append_having_part(
    ulong sql_type
  ) = 0;
  virtual int append_item_type_part(
    Item *item,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  ) = 0;
  virtual int append_list_item_select_part(
    List<Item> *select,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  ) = 0;
  virtual int append_group_by_part(
    ORDER *order,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  ) = 0;
  virtual int append_order_by_part(
    ORDER *order,
    const char *alias,
    uint alias_length,
    bool use_fields,
    spider_fields *fields,
    ulong sql_type
  ) = 0;
#endif
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  virtual bool check_direct_update(
    st_select_lex *select_lex,
    longlong select_limit,
    longlong offset_limit
  );
  virtual bool check_direct_delete(
    st_select_lex *select_lex,
    longlong select_limit,
    longlong offset_limit
  );
#endif
};

class spider_db_copy_table
{
public:
  uint dbton_id;
  spider_db_share *db_share;
  spider_db_copy_table(spider_db_share *db_share) :
    dbton_id(db_share->dbton_id), db_share(db_share) {}
  virtual ~spider_db_copy_table() {}
  virtual int init() = 0;
  virtual void set_sql_charset(
    CHARSET_INFO *cs
  ) = 0;
  virtual int append_select_str() = 0;
  virtual int append_insert_str(
    int insert_flg
  ) = 0;
  virtual int append_table_columns(
    TABLE_SHARE *table_share
  ) = 0;
  virtual int append_from_str() = 0;
  virtual int append_table_name(
    int link_idx
  ) = 0;
  virtual void set_sql_pos() = 0;
  virtual void set_sql_to_pos() = 0;
  virtual int append_copy_where(
    spider_db_copy_table *source_ct,
    KEY *key_info,
    ulong *last_row_pos,
    ulong *last_lengths
  ) = 0;
  virtual int append_key_order_str(
    KEY *key_info,
    int start_pos,
    bool desc_flg
  ) = 0;
  virtual int append_limit(
    longlong offset,
    longlong limit
  ) = 0;
  virtual int append_into_str() = 0;
  virtual int append_open_paren_str() = 0;
  virtual int append_values_str() = 0;
  virtual int append_select_lock_str(
    int lock_mode
  ) = 0;
  virtual int exec_query(
    SPIDER_CONN *conn,
    int quick_mode,
    int *need_mon
  ) = 0;
  virtual int copy_rows(
    TABLE *table,
    SPIDER_DB_ROW *row,
    ulong **last_row_pos,
    ulong **last_lengths
  ) = 0;
  virtual int copy_rows(
    TABLE *table,
    SPIDER_DB_ROW *row
  ) = 0;
  virtual int append_insert_terminator() = 0;
  virtual int copy_insert_values(
    spider_db_copy_table *source_ct
  ) = 0;
};

enum spider_db_access_type
{
  SPIDER_DB_ACCESS_TYPE_SQL,
  SPIDER_DB_ACCESS_TYPE_NOSQL
};

#define SPIDER_MATURITY_UNKNOWN      0
#define SPIDER_MATURITY_EXPERIMENTAL 1
#define SPIDER_MATURITY_ALPHA        2
#define SPIDER_MATURITY_BETA         3
#define SPIDER_MATURITY_GAMMA        4
#define SPIDER_MATURITY_STABLE       5

static const LEX_CSTRING maturity_name[] =
{
  { STRING_WITH_LEN("Unknown") },
  { STRING_WITH_LEN("Experimental") },
  { STRING_WITH_LEN("Alpha") },
  { STRING_WITH_LEN("Beta") },
  { STRING_WITH_LEN("Gamma") },
  { STRING_WITH_LEN("Stable") }
};

typedef struct st_spider_dbton
{
  uint dbton_id;
  const char *wrapper;
  enum spider_db_access_type db_access_type;
  int (*init)();
  int (*deinit)();
  spider_db_share *(*create_db_share)(st_spider_share *share);
  spider_db_handler *(*create_db_handler)(ha_spider *spider,
    spider_db_share *db_share);
  spider_db_copy_table *(*create_db_copy_table)(
    spider_db_share *db_share);
  SPIDER_DB_CONN *(*create_db_conn)(SPIDER_CONN *conn);
  bool (*support_direct_join)();
  spider_db_util *db_util;
  const char *descr;
  const char *version_info;
  unsigned int maturity;
} SPIDER_DBTON;

typedef struct st_spider_position
{
  SPIDER_DB_ROW          *row;
  uint                   pos_mode;
  bool                   use_position;
  bool                   mrr_with_cnt;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  bool                   direct_aggregate;
#endif
  uint                   sql_kind;
  uchar                  *position_bitmap;
  st_spider_ft_info      *ft_first;
  st_spider_ft_info      *ft_current;
  my_off_t               tmp_tbl_pos;
  SPIDER_RESULT          *result;
} SPIDER_POSITION;

typedef struct st_spider_condition
{
  COND                   *cond;
  st_spider_condition    *next;
} SPIDER_CONDITION;

typedef struct st_spider_result
{
  uint                 dbton_id;
  SPIDER_DB_RESULT     *result;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile
#endif
    st_spider_result   *prev;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile
#endif
    st_spider_result   *next;
  SPIDER_POSITION      *first_position; /* for quick mode */
  int                  pos_page_size; /* for quick mode */
  longlong             record_num;
  bool                 finish_flg;
  bool                 use_position;
  bool                 first_pos_use_position;
  bool                 tmp_tbl_use_position;
  uint                 field_count; /* for quick mode */
  TABLE                *result_tmp_tbl;
  TMP_TABLE_PARAM      result_tmp_tbl_prm;
  THD                  *result_tmp_tbl_thd;
  uint                 result_tmp_tbl_inited;
  SPIDER_DB_ROW        *tmp_tbl_row;
} SPIDER_RESULT;

typedef struct st_spider_result_list
{
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile
#endif
    SPIDER_RESULT        *first;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile
#endif
    SPIDER_RESULT        *last;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile
#endif
    SPIDER_RESULT         *current;
  KEY                     *key_info;
  int                     key_order;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  ulonglong               hs_upd_rows;
  SPIDER_DB_RESULT        *hs_result;
  SPIDER_DB_RESULT_BUFFER *hs_result_buf;
  bool                    hs_has_result;
  SPIDER_DB_CONN          *hs_conn;
#endif
#ifdef HA_CAN_BULK_ACCESS
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uchar                   *hs_r_bulk_open_index;
  uchar                   *hs_w_bulk_open_index;
#endif
#endif
  spider_string           *sqls;
  int                     ha_read_kind;
  bool                    have_sql_kind_backup;
  uint                    *sql_kind_backup;
  uint                    sql_kinds_backup;
  bool                    use_union;
  bool                    use_both_key;
  const key_range         *end_key;
  spider_string           *insert_sqls;
  spider_string           *update_sqls;
  TABLE                   **upd_tmp_tbls;
  TMP_TABLE_PARAM         *upd_tmp_tbl_prms;
  bool                    tmp_table_join;
  uchar                   *tmp_table_join_first;
  bool                    tmp_tables_created;
  uchar                   *tmp_table_created;
  bool                    tmp_table_join_break_after_get_next;
  key_part_map            tmp_table_join_key_part_map;
  spider_string           *tmp_sqls;
  bool                    tmp_reuse_sql;
  bool                    sorted;
  bool                    desc_flg;
  longlong                current_row_num;
  longlong                record_num;
  bool                    finish_flg;
  longlong                limit_num;
  longlong                internal_offset;
  longlong                internal_limit;
  longlong                split_read;
  int                     multi_split_read;
  int                     max_order;
  int                     quick_mode;
  longlong                quick_page_size;
  longlong                quick_page_byte;
  int                     low_mem_read;
  int                     bulk_update_mode;
  int                     bulk_update_size;
  spider_bulk_upd_start   bulk_update_start;
  bool                    check_direct_order_limit;
  bool                    direct_order_limit;
  /* the limit_offeset, without where condition */
  bool                    direct_limit_offset;
  bool                    direct_distinct;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
  bool                    direct_aggregate;
  bool                    snap_mrr_with_cnt;
  bool                    snap_direct_aggregate;
  SPIDER_DB_ROW           *snap_row;
#endif
  bool                    in_cmp_ref;
  bool                    set_split_read;
  bool                    insert_dup_update_pushdown;
  longlong                split_read_base;
  double                  semi_split_read;
  longlong                semi_split_read_limit;
  longlong                semi_split_read_base;
  longlong                first_read;
  longlong                second_read;
  int                     set_split_read_count;
  int                     *casual_read;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  /* 0:nomal 1:store 2:store end */
  volatile
#endif
    int                   quick_phase;
  bool                    keyread;
  TABLE                   *table;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  volatile int            bgs_error;
  bool                    bgs_error_with_message;
  char                    bgs_error_msg[MYSQL_ERRMSG_SIZE];
  volatile bool           bgs_working;
  /* 0:not use bg 1:first read 2:second read 3:after second read */
  volatile int            bgs_phase;
  volatile longlong       bgs_first_read;
  volatile longlong       bgs_second_read;
  volatile longlong       bgs_split_read;
  volatile
#endif
    SPIDER_RESULT         *bgs_current;
  SPIDER_DB_ROW           *tmp_pos_row_first;
} SPIDER_RESULT_LIST;
