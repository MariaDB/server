/*
  Copyright(C) 2009-2017 Brazil

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

#pragma once

#include <stdarg.h>
#include <sys/types.h>
#include <stdint.h>
#include <string.h>

#ifdef  __cplusplus
extern "C" {
#endif

#ifndef GRN_API
# if defined(_WIN32) || defined(_WIN64)
#  define GRN_API __declspec(dllimport)
# else
#  define GRN_API
# endif /* defined(_WIN32) || defined(_WIN64) */
#endif /* GRN_API */

typedef unsigned int grn_id;
typedef uint8_t grn_bool;

#define GRN_ID_NIL                     (0x00)
#define GRN_ID_MAX                     (0x3fffffff)

#define GRN_TRUE                       (1)
#define GRN_FALSE                      (0)

typedef enum {
  GRN_SUCCESS = 0,
  GRN_END_OF_DATA = 1,
  GRN_UNKNOWN_ERROR = -1,
  GRN_OPERATION_NOT_PERMITTED = -2,
  GRN_NO_SUCH_FILE_OR_DIRECTORY = -3,
  GRN_NO_SUCH_PROCESS = -4,
  GRN_INTERRUPTED_FUNCTION_CALL = -5,
  GRN_INPUT_OUTPUT_ERROR = -6,
  GRN_NO_SUCH_DEVICE_OR_ADDRESS = -7,
  GRN_ARG_LIST_TOO_LONG = -8,
  GRN_EXEC_FORMAT_ERROR = -9,
  GRN_BAD_FILE_DESCRIPTOR = -10,
  GRN_NO_CHILD_PROCESSES = -11,
  GRN_RESOURCE_TEMPORARILY_UNAVAILABLE = -12,
  GRN_NOT_ENOUGH_SPACE = -13,
  GRN_PERMISSION_DENIED = -14,
  GRN_BAD_ADDRESS = -15,
  GRN_RESOURCE_BUSY = -16,
  GRN_FILE_EXISTS = -17,
  GRN_IMPROPER_LINK = -18,
  GRN_NO_SUCH_DEVICE = -19,
  GRN_NOT_A_DIRECTORY = -20,
  GRN_IS_A_DIRECTORY = -21,
  GRN_INVALID_ARGUMENT = -22,
  GRN_TOO_MANY_OPEN_FILES_IN_SYSTEM = -23,
  GRN_TOO_MANY_OPEN_FILES = -24,
  GRN_INAPPROPRIATE_I_O_CONTROL_OPERATION = -25,
  GRN_FILE_TOO_LARGE = -26,
  GRN_NO_SPACE_LEFT_ON_DEVICE = -27,
  GRN_INVALID_SEEK = -28,
  GRN_READ_ONLY_FILE_SYSTEM = -29,
  GRN_TOO_MANY_LINKS = -30,
  GRN_BROKEN_PIPE = -31,
  GRN_DOMAIN_ERROR = -32,
  GRN_RESULT_TOO_LARGE = -33,
  GRN_RESOURCE_DEADLOCK_AVOIDED = -34,
  GRN_NO_MEMORY_AVAILABLE = -35,
  GRN_FILENAME_TOO_LONG = -36,
  GRN_NO_LOCKS_AVAILABLE = -37,
  GRN_FUNCTION_NOT_IMPLEMENTED = -38,
  GRN_DIRECTORY_NOT_EMPTY = -39,
  GRN_ILLEGAL_BYTE_SEQUENCE = -40,
  GRN_SOCKET_NOT_INITIALIZED = -41,
  GRN_OPERATION_WOULD_BLOCK = -42,
  GRN_ADDRESS_IS_NOT_AVAILABLE = -43,
  GRN_NETWORK_IS_DOWN = -44,
  GRN_NO_BUFFER = -45,
  GRN_SOCKET_IS_ALREADY_CONNECTED = -46,
  GRN_SOCKET_IS_NOT_CONNECTED = -47,
  GRN_SOCKET_IS_ALREADY_SHUTDOWNED = -48,
  GRN_OPERATION_TIMEOUT = -49,
  GRN_CONNECTION_REFUSED = -50,
  GRN_RANGE_ERROR = -51,
  GRN_TOKENIZER_ERROR = -52,
  GRN_FILE_CORRUPT = -53,
  GRN_INVALID_FORMAT = -54,
  GRN_OBJECT_CORRUPT = -55,
  GRN_TOO_MANY_SYMBOLIC_LINKS = -56,
  GRN_NOT_SOCKET = -57,
  GRN_OPERATION_NOT_SUPPORTED = -58,
  GRN_ADDRESS_IS_IN_USE = -59,
  GRN_ZLIB_ERROR = -60,
  GRN_LZ4_ERROR = -61,
/* Just for backward compatibility. We'll remove it at 5.0.0. */
#define GRN_LZO_ERROR GRN_LZ4_ERROR
  GRN_STACK_OVER_FLOW = -62,
  GRN_SYNTAX_ERROR = -63,
  GRN_RETRY_MAX = -64,
  GRN_INCOMPATIBLE_FILE_FORMAT = -65,
  GRN_UPDATE_NOT_ALLOWED = -66,
  GRN_TOO_SMALL_OFFSET = -67,
  GRN_TOO_LARGE_OFFSET = -68,
  GRN_TOO_SMALL_LIMIT = -69,
  GRN_CAS_ERROR = -70,
  GRN_UNSUPPORTED_COMMAND_VERSION = -71,
  GRN_NORMALIZER_ERROR = -72,
  GRN_TOKEN_FILTER_ERROR = -73,
  GRN_COMMAND_ERROR = -74,
  GRN_PLUGIN_ERROR = -75,
  GRN_SCORER_ERROR = -76,
  GRN_CANCEL = -77,
  GRN_WINDOW_FUNCTION_ERROR = -78,
  GRN_ZSTD_ERROR = -79
} grn_rc;

GRN_API grn_rc grn_init(void);
GRN_API grn_rc grn_fin(void);

GRN_API const char *grn_get_global_error_message(void);

typedef enum {
  GRN_ENC_DEFAULT = 0,
  GRN_ENC_NONE,
  GRN_ENC_EUC_JP,
  GRN_ENC_UTF8,
  GRN_ENC_SJIS,
  GRN_ENC_LATIN1,
  GRN_ENC_KOI8R
} grn_encoding;

typedef enum {
  GRN_COMMAND_VERSION_DEFAULT = 0,
  GRN_COMMAND_VERSION_1,
  GRN_COMMAND_VERSION_2,
  GRN_COMMAND_VERSION_3
} grn_command_version;

#define GRN_COMMAND_VERSION_MIN GRN_COMMAND_VERSION_1
#define GRN_COMMAND_VERSION_STABLE GRN_COMMAND_VERSION_1
#define GRN_COMMAND_VERSION_MAX GRN_COMMAND_VERSION_3

typedef enum {
  GRN_LOG_NONE = 0,
  GRN_LOG_EMERG,
  GRN_LOG_ALERT,
  GRN_LOG_CRIT,
  GRN_LOG_ERROR,
  GRN_LOG_WARNING,
  GRN_LOG_NOTICE,
  GRN_LOG_INFO,
  GRN_LOG_DEBUG,
  GRN_LOG_DUMP
} grn_log_level;

GRN_API const char *grn_log_level_to_string(grn_log_level level);
GRN_API grn_bool grn_log_level_parse(const char *string, grn_log_level *level);

/* query log flags */
#define GRN_QUERY_LOG_NONE        (0x00)
#define GRN_QUERY_LOG_COMMAND     (0x01<<0)
#define GRN_QUERY_LOG_RESULT_CODE (0x01<<1)
#define GRN_QUERY_LOG_DESTINATION (0x01<<2)
#define GRN_QUERY_LOG_CACHE       (0x01<<3)
#define GRN_QUERY_LOG_SIZE        (0x01<<4)
#define GRN_QUERY_LOG_SCORE       (0x01<<5)
#define GRN_QUERY_LOG_ALL\
  (GRN_QUERY_LOG_COMMAND |\
   GRN_QUERY_LOG_RESULT_CODE |\
   GRN_QUERY_LOG_DESTINATION |\
   GRN_QUERY_LOG_CACHE |\
   GRN_QUERY_LOG_SIZE |\
   GRN_QUERY_LOG_SCORE)
#define GRN_QUERY_LOG_DEFAULT     GRN_QUERY_LOG_ALL

typedef enum {
  GRN_CONTENT_NONE = 0,
  GRN_CONTENT_TSV,
  GRN_CONTENT_JSON,
  GRN_CONTENT_XML,
  GRN_CONTENT_MSGPACK,
  GRN_CONTENT_GROONGA_COMMAND_LIST
} grn_content_type;

typedef struct _grn_obj grn_obj;
typedef struct _grn_ctx grn_ctx;

#define GRN_CTX_MSGSIZE                (0x80)
#define GRN_CTX_FIN                    (0xff)

typedef union {
  int int_value;
  grn_id id;
  void *ptr;
} grn_user_data;

typedef grn_obj *grn_proc_func(grn_ctx *ctx, int nargs, grn_obj **args,
                               grn_user_data *user_data);

struct _grn_ctx {
  grn_rc rc;
  int flags;
  grn_encoding encoding;
  unsigned char ntrace;
  unsigned char errlvl;
  unsigned char stat;
  unsigned int seqno;
  unsigned int subno;
  unsigned int seqno2;
  unsigned int errline;
  grn_user_data user_data;
  grn_ctx *prev;
  grn_ctx *next;
  const char *errfile;
  const char *errfunc;
  struct _grn_ctx_impl *impl;
  void *trace[16];
  char errbuf[GRN_CTX_MSGSIZE];
};

#define GRN_CTX_USER_DATA(ctx) (&((ctx)->user_data))

/* Deprecated since 4.0.3. Don't use it. */
#define GRN_CTX_USE_QL                 (0x03)
/* Deprecated since 4.0.3. Don't use it. */
#define GRN_CTX_BATCH_MODE             (0x04)
#define GRN_CTX_PER_DB                 (0x08)

GRN_API grn_rc grn_ctx_init(grn_ctx *ctx, int flags);
GRN_API grn_rc grn_ctx_fin(grn_ctx *ctx);
GRN_API grn_ctx *grn_ctx_open(int flags);
GRN_API grn_rc grn_ctx_close(grn_ctx *ctx);
GRN_API grn_rc grn_ctx_set_finalizer(grn_ctx *ctx, grn_proc_func *func);

GRN_API grn_rc grn_ctx_push_temporary_open_space(grn_ctx *ctx);
GRN_API grn_rc grn_ctx_pop_temporary_open_space(grn_ctx *ctx);
GRN_API grn_rc grn_ctx_merge_temporary_open_space(grn_ctx *ctx);

GRN_API grn_encoding grn_get_default_encoding(void);
GRN_API grn_rc grn_set_default_encoding(grn_encoding encoding);

#define GRN_CTX_GET_ENCODING(ctx) ((ctx)->encoding)
#define GRN_CTX_SET_ENCODING(ctx,enc) \
  ((ctx)->encoding = (enc == GRN_ENC_DEFAULT) ? grn_get_default_encoding() : enc)

GRN_API const char *grn_get_version(void);
GRN_API const char *grn_get_package(void);
GRN_API const char *grn_get_package_label(void);

GRN_API grn_command_version grn_get_default_command_version(void);
GRN_API grn_rc grn_set_default_command_version(grn_command_version version);
GRN_API grn_command_version grn_ctx_get_command_version(grn_ctx *ctx);
GRN_API grn_rc grn_ctx_set_command_version(grn_ctx *ctx, grn_command_version version);
GRN_API long long int grn_ctx_get_match_escalation_threshold(grn_ctx *ctx);
GRN_API grn_rc grn_ctx_set_match_escalation_threshold(grn_ctx *ctx, long long int threshold);
GRN_API long long int grn_get_default_match_escalation_threshold(void);
GRN_API grn_rc grn_set_default_match_escalation_threshold(long long int threshold);

GRN_API int grn_get_lock_timeout(void);
GRN_API grn_rc grn_set_lock_timeout(int timeout);

/* grn_encoding */

GRN_API const char *grn_encoding_to_string(grn_encoding encoding);
GRN_API grn_encoding grn_encoding_parse(const char *name);

/* obj */

typedef uint16_t grn_obj_flags;
typedef uint32_t grn_table_flags;
typedef uint32_t grn_column_flags;

/* flags for grn_obj_flags and grn_table_flags */

#define GRN_OBJ_FLAGS_MASK             (0xffff)

#define GRN_OBJ_TABLE_TYPE_MASK        (0x07)
#define GRN_OBJ_TABLE_HASH_KEY         (0x00)
#define GRN_OBJ_TABLE_PAT_KEY          (0x01)
#define GRN_OBJ_TABLE_DAT_KEY          (0x02)
#define GRN_OBJ_TABLE_NO_KEY           (0x03)

#define GRN_OBJ_KEY_MASK               (0x07<<3)
#define GRN_OBJ_KEY_UINT               (0x00<<3)
#define GRN_OBJ_KEY_INT                (0x01<<3)
#define GRN_OBJ_KEY_FLOAT              (0x02<<3)
#define GRN_OBJ_KEY_GEO_POINT          (0x03<<3)

#define GRN_OBJ_KEY_WITH_SIS           (0x01<<6)
#define GRN_OBJ_KEY_NORMALIZE          (0x01<<7)

#define GRN_OBJ_COLUMN_TYPE_MASK       (0x07)
#define GRN_OBJ_COLUMN_SCALAR          (0x00)
#define GRN_OBJ_COLUMN_VECTOR          (0x01)
#define GRN_OBJ_COLUMN_INDEX           (0x02)

#define GRN_OBJ_COMPRESS_MASK          (0x07<<4)
#define GRN_OBJ_COMPRESS_NONE          (0x00<<4)
#define GRN_OBJ_COMPRESS_ZLIB          (0x01<<4)
#define GRN_OBJ_COMPRESS_LZ4           (0x02<<4)
/* Just for backward compatibility. We'll remove it at 5.0.0. */
#define GRN_OBJ_COMPRESS_LZO           GRN_OBJ_COMPRESS_LZ4
#define GRN_OBJ_COMPRESS_ZSTD          (0x03<<4)

#define GRN_OBJ_WITH_SECTION           (0x01<<7)
#define GRN_OBJ_WITH_WEIGHT            (0x01<<8)
#define GRN_OBJ_WITH_POSITION          (0x01<<9)
#define GRN_OBJ_RING_BUFFER            (0x01<<10)

#define GRN_OBJ_UNIT_MASK              (0x0f<<8)
#define GRN_OBJ_UNIT_DOCUMENT_NONE     (0x00<<8)
#define GRN_OBJ_UNIT_DOCUMENT_SECTION  (0x01<<8)
#define GRN_OBJ_UNIT_DOCUMENT_POSITION (0x02<<8)
#define GRN_OBJ_UNIT_SECTION_NONE      (0x03<<8)
#define GRN_OBJ_UNIT_SECTION_POSITION  (0x04<<8)
#define GRN_OBJ_UNIT_POSITION_NONE     (0x05<<8)
#define GRN_OBJ_UNIT_USERDEF_DOCUMENT  (0x06<<8)
#define GRN_OBJ_UNIT_USERDEF_SECTION   (0x07<<8)
#define GRN_OBJ_UNIT_USERDEF_POSITION  (0x08<<8)

/* Don't use (0x01<<12) because it's used internally. */

#define GRN_OBJ_NO_SUBREC              (0x00<<13)
#define GRN_OBJ_WITH_SUBREC            (0x01<<13)

#define GRN_OBJ_KEY_VAR_SIZE           (0x01<<14)

#define GRN_OBJ_TEMPORARY              (0x00<<15)
#define GRN_OBJ_PERSISTENT             (0x01<<15)

/* flags only for grn_table_flags */

#define GRN_OBJ_KEY_LARGE              (0x01<<16)

/* flags only for grn_column_flags */

#define GRN_OBJ_INDEX_SMALL            (0x01<<16)
#define GRN_OBJ_INDEX_MEDIUM           (0x01<<17)

/* obj types */

#define GRN_VOID                       (0x00)
#define GRN_BULK                       (0x02)
#define GRN_PTR                        (0x03)
#define GRN_UVECTOR                    (0x04) /* vector of fixed size data especially grn_id */
#define GRN_PVECTOR                    (0x05) /* vector of grn_obj* */
#define GRN_VECTOR                     (0x06) /* vector of arbitrary data */
#define GRN_MSG                        (0x07)
#define GRN_QUERY                      (0x08)
#define GRN_ACCESSOR                   (0x09)
#define GRN_SNIP                       (0x0b)
#define GRN_PATSNIP                    (0x0c)
#define GRN_STRING                     (0x0d)
#define GRN_CURSOR_TABLE_HASH_KEY      (0x10)
#define GRN_CURSOR_TABLE_PAT_KEY       (0x11)
#define GRN_CURSOR_TABLE_DAT_KEY       (0x12)
#define GRN_CURSOR_TABLE_NO_KEY        (0x13)
#define GRN_CURSOR_COLUMN_INDEX        (0x18)
#define GRN_CURSOR_COLUMN_GEO_INDEX    (0x1a)
#define GRN_CURSOR_CONFIG              (0x1f)
#define GRN_TYPE                       (0x20)
#define GRN_PROC                       (0x21)
#define GRN_EXPR                       (0x22)
#define GRN_TABLE_HASH_KEY             (0x30)
#define GRN_TABLE_PAT_KEY              (0x31)
#define GRN_TABLE_DAT_KEY              (0x32)
#define GRN_TABLE_NO_KEY               (0x33)
#define GRN_DB                         (0x37)
#define GRN_COLUMN_FIX_SIZE            (0x40)
#define GRN_COLUMN_VAR_SIZE            (0x41)
#define GRN_COLUMN_INDEX               (0x48)

typedef struct _grn_section grn_section;
typedef struct _grn_obj_header grn_obj_header;

struct _grn_section {
  unsigned int offset;
  unsigned int length;
  unsigned int weight;
  grn_id domain;
};

struct _grn_obj_header {
  unsigned char type;
  unsigned char impl_flags;
  grn_obj_flags flags;
  grn_id domain;
};

struct _grn_obj {
  grn_obj_header header;
  union {
    struct {
      char *head;
      char *curr;
      char *tail;
    } b;
    struct {
      grn_obj *body;
      grn_section *sections;
      int n_sections;
    } v;
  } u;
};

#define GRN_OBJ_REFER                  (0x01<<0)
#define GRN_OBJ_OUTPLACE               (0x01<<1)
#define GRN_OBJ_OWN                    (0x01<<5)

#define GRN_OBJ_INIT(obj,obj_type,obj_flags,obj_domain) do { \
  (obj)->header.type = (obj_type);\
  (obj)->header.impl_flags = (obj_flags);\
  (obj)->header.flags = 0;\
  (obj)->header.domain = (obj_domain);\
  (obj)->u.b.head = NULL;\
  (obj)->u.b.curr = NULL;\
  (obj)->u.b.tail = NULL;\
} while (0)

#define GRN_OBJ_FIN(ctx,obj) (grn_obj_close((ctx), (obj)))

GRN_API grn_rc grn_ctx_use(grn_ctx *ctx, grn_obj *db);
GRN_API grn_obj *grn_ctx_db(grn_ctx *ctx);
GRN_API grn_obj *grn_ctx_get(grn_ctx *ctx, const char *name, int name_size);
GRN_API grn_rc grn_ctx_get_all_tables(grn_ctx *ctx, grn_obj *tables_buffer);
GRN_API grn_rc grn_ctx_get_all_types(grn_ctx *ctx, grn_obj *types_buffer);
GRN_API grn_rc grn_ctx_get_all_tokenizers(grn_ctx *ctx,
                                          grn_obj *tokenizers_buffer);
GRN_API grn_rc grn_ctx_get_all_normalizers(grn_ctx *ctx,
                                           grn_obj *normalizers_buffer);
GRN_API grn_rc grn_ctx_get_all_token_filters(grn_ctx *ctx,
                                             grn_obj *token_filters_buffer);

typedef enum {
  GRN_DB_VOID = 0,
  GRN_DB_DB,
  GRN_DB_OBJECT,
  GRN_DB_BOOL,
  GRN_DB_INT8,
  GRN_DB_UINT8,
  GRN_DB_INT16,
  GRN_DB_UINT16,
  GRN_DB_INT32,
  GRN_DB_UINT32,
  GRN_DB_INT64,
  GRN_DB_UINT64,
  GRN_DB_FLOAT,
  GRN_DB_TIME,
  GRN_DB_SHORT_TEXT,
  GRN_DB_TEXT,
  GRN_DB_LONG_TEXT,
  GRN_DB_TOKYO_GEO_POINT,
  GRN_DB_WGS84_GEO_POINT
} grn_builtin_type;

typedef enum {
  GRN_DB_MECAB = 64,
  GRN_DB_DELIMIT,
  GRN_DB_UNIGRAM,
  GRN_DB_BIGRAM,
  GRN_DB_TRIGRAM
} grn_builtin_tokenizer;

GRN_API grn_obj *grn_ctx_at(grn_ctx *ctx, grn_id id);
GRN_API grn_bool grn_ctx_is_opened(grn_ctx *ctx, grn_id id);

GRN_API grn_rc grn_plugin_register(grn_ctx *ctx, const char *name);
GRN_API grn_rc grn_plugin_unregister(grn_ctx *ctx, const char *name);
GRN_API grn_rc grn_plugin_register_by_path(grn_ctx *ctx, const char *path);
GRN_API grn_rc grn_plugin_unregister_by_path(grn_ctx *ctx, const char *path);
GRN_API const char *grn_plugin_get_system_plugins_dir(void);
GRN_API const char *grn_plugin_get_suffix(void);
GRN_API const char *grn_plugin_get_ruby_suffix(void);
GRN_API grn_rc grn_plugin_get_names(grn_ctx *ctx, grn_obj *names);

typedef struct {
  const char *name;
  unsigned int name_size;
  grn_obj value;
} grn_expr_var;

typedef grn_rc (*grn_plugin_func)(grn_ctx *ctx);

typedef enum {
  GRN_PROC_INVALID = 0,
  GRN_PROC_TOKENIZER,
  GRN_PROC_COMMAND,
  GRN_PROC_FUNCTION,
  GRN_PROC_HOOK,
  GRN_PROC_NORMALIZER,
  GRN_PROC_TOKEN_FILTER,
  GRN_PROC_SCORER,
  GRN_PROC_WINDOW_FUNCTION
} grn_proc_type;

GRN_API grn_obj *grn_proc_create(grn_ctx *ctx,
                                 const char *name, int name_size, grn_proc_type type,
                                 grn_proc_func *init, grn_proc_func *next, grn_proc_func *fin,
                                 unsigned int nvars, grn_expr_var *vars);
GRN_API grn_obj *grn_proc_get_info(grn_ctx *ctx, grn_user_data *user_data,
                                   grn_expr_var **vars, unsigned int *nvars, grn_obj **caller);
GRN_API grn_proc_type grn_proc_get_type(grn_ctx *ctx, grn_obj *proc);

typedef grn_obj grn_table_cursor;

typedef struct {
  grn_id rid;
  uint32_t sid;
  uint32_t pos;
  uint32_t tf;
  uint32_t weight;
  uint32_t rest;
} grn_posting;

typedef enum {
  GRN_OP_PUSH = 0,
  GRN_OP_POP,
  GRN_OP_NOP,
  GRN_OP_CALL,
  GRN_OP_INTERN,
  GRN_OP_GET_REF,
  GRN_OP_GET_VALUE,
  GRN_OP_AND,
  GRN_OP_AND_NOT,
  /* Deprecated. Just for backward compatibility. */
#define GRN_OP_BUT GRN_OP_AND_NOT
  GRN_OP_OR,
  GRN_OP_ASSIGN,
  GRN_OP_STAR_ASSIGN,
  GRN_OP_SLASH_ASSIGN,
  GRN_OP_MOD_ASSIGN,
  GRN_OP_PLUS_ASSIGN,
  GRN_OP_MINUS_ASSIGN,
  GRN_OP_SHIFTL_ASSIGN,
  GRN_OP_SHIFTR_ASSIGN,
  GRN_OP_SHIFTRR_ASSIGN,
  GRN_OP_AND_ASSIGN,
  GRN_OP_XOR_ASSIGN,
  GRN_OP_OR_ASSIGN,
  GRN_OP_JUMP,
  GRN_OP_CJUMP,
  GRN_OP_COMMA,
  GRN_OP_BITWISE_OR,
  GRN_OP_BITWISE_XOR,
  GRN_OP_BITWISE_AND,
  GRN_OP_BITWISE_NOT,
  GRN_OP_EQUAL,
  GRN_OP_NOT_EQUAL,
  GRN_OP_LESS,
  GRN_OP_GREATER,
  GRN_OP_LESS_EQUAL,
  GRN_OP_GREATER_EQUAL,
  GRN_OP_IN,
  GRN_OP_MATCH,
  GRN_OP_NEAR,
  GRN_OP_NEAR2,
  GRN_OP_SIMILAR,
  GRN_OP_TERM_EXTRACT,
  GRN_OP_SHIFTL,
  GRN_OP_SHIFTR,
  GRN_OP_SHIFTRR,
  GRN_OP_PLUS,
  GRN_OP_MINUS,
  GRN_OP_STAR,
  GRN_OP_SLASH,
  GRN_OP_MOD,
  GRN_OP_DELETE,
  GRN_OP_INCR,
  GRN_OP_DECR,
  GRN_OP_INCR_POST,
  GRN_OP_DECR_POST,
  GRN_OP_NOT,
  GRN_OP_ADJUST,
  GRN_OP_EXACT,
  GRN_OP_LCP,
  GRN_OP_PARTIAL,
  GRN_OP_UNSPLIT,
  GRN_OP_PREFIX,
  GRN_OP_SUFFIX,
  GRN_OP_GEO_DISTANCE1,
  GRN_OP_GEO_DISTANCE2,
  GRN_OP_GEO_DISTANCE3,
  GRN_OP_GEO_DISTANCE4,
  GRN_OP_GEO_WITHINP5,
  GRN_OP_GEO_WITHINP6,
  GRN_OP_GEO_WITHINP8,
  GRN_OP_OBJ_SEARCH,
  GRN_OP_EXPR_GET_VAR,
  GRN_OP_TABLE_CREATE,
  GRN_OP_TABLE_SELECT,
  GRN_OP_TABLE_SORT,
  GRN_OP_TABLE_GROUP,
  GRN_OP_JSON_PUT,
  GRN_OP_GET_MEMBER,
  GRN_OP_REGEXP,
  GRN_OP_FUZZY
} grn_operator;

GRN_API grn_obj *grn_obj_column(grn_ctx *ctx, grn_obj *table,
                                const char *name, unsigned int name_size);

/*-------------------------------------------------------------
 * API for column
 */

#define GRN_COLUMN_NAME_ID            "_id"
#define GRN_COLUMN_NAME_ID_LEN        (sizeof(GRN_COLUMN_NAME_ID) - 1)
#define GRN_COLUMN_NAME_KEY           "_key"
#define GRN_COLUMN_NAME_KEY_LEN       (sizeof(GRN_COLUMN_NAME_KEY) - 1)
#define GRN_COLUMN_NAME_VALUE         "_value"
#define GRN_COLUMN_NAME_VALUE_LEN     (sizeof(GRN_COLUMN_NAME_VALUE) - 1)
#define GRN_COLUMN_NAME_SCORE         "_score"
#define GRN_COLUMN_NAME_SCORE_LEN     (sizeof(GRN_COLUMN_NAME_SCORE) - 1)
#define GRN_COLUMN_NAME_NSUBRECS      "_nsubrecs"
#define GRN_COLUMN_NAME_NSUBRECS_LEN  (sizeof(GRN_COLUMN_NAME_NSUBRECS) - 1)
#define GRN_COLUMN_NAME_MAX           "_max"
#define GRN_COLUMN_NAME_MAX_LEN       (sizeof(GRN_COLUMN_NAME_MAX) - 1)
#define GRN_COLUMN_NAME_MIN           "_min"
#define GRN_COLUMN_NAME_MIN_LEN       (sizeof(GRN_COLUMN_NAME_MIN) - 1)
#define GRN_COLUMN_NAME_SUM           "_sum"
#define GRN_COLUMN_NAME_SUM_LEN       (sizeof(GRN_COLUMN_NAME_SUM) - 1)
#define GRN_COLUMN_NAME_AVG           "_avg"
#define GRN_COLUMN_NAME_AVG_LEN       (sizeof(GRN_COLUMN_NAME_AVG) - 1)

GRN_API grn_obj *grn_column_create(grn_ctx *ctx, grn_obj *table,
                                   const char *name, unsigned int name_size,
                                   const char *path, grn_column_flags flags, grn_obj *type);

#define GRN_COLUMN_OPEN_OR_CREATE(ctx,table,name,name_size,path,flags,type,column) \
  (((column) = grn_obj_column((ctx), (table), (name), (name_size))) ||\
   ((column) = grn_column_create((ctx), (table), (name), (name_size), (path), (flags), (type))))

GRN_API grn_rc grn_column_index_update(grn_ctx *ctx, grn_obj *column,
                                       grn_id id, unsigned int section,
                                       grn_obj *oldvalue, grn_obj *newvalue);
GRN_API grn_obj *grn_column_table(grn_ctx *ctx, grn_obj *column);
GRN_API grn_rc grn_column_truncate(grn_ctx *ctx, grn_obj *column);

/*-------------------------------------------------------------
 * API for db, table and/or column
 */

typedef enum {
  GRN_INFO_ENCODING = 0,
  GRN_INFO_SOURCE,
  GRN_INFO_DEFAULT_TOKENIZER,
  GRN_INFO_ELEMENT_SIZE,
  GRN_INFO_CURR_MAX,
  GRN_INFO_MAX_ELEMENT_SIZE,
  GRN_INFO_SEG_SIZE,
  GRN_INFO_CHUNK_SIZE,
  GRN_INFO_MAX_SECTION,
  GRN_INFO_HOOK_LOCAL_DATA,
  GRN_INFO_ELEMENT_A,
  GRN_INFO_ELEMENT_CHUNK,
  GRN_INFO_ELEMENT_CHUNK_SIZE,
  GRN_INFO_ELEMENT_BUFFER_FREE,
  GRN_INFO_ELEMENT_NTERMS,
  GRN_INFO_ELEMENT_NTERMS_VOID,
  GRN_INFO_ELEMENT_SIZE_IN_CHUNK,
  GRN_INFO_ELEMENT_POS_IN_CHUNK,
  GRN_INFO_ELEMENT_SIZE_IN_BUFFER,
  GRN_INFO_ELEMENT_POS_IN_BUFFER,
  GRN_INFO_ELEMENT_ESTIMATE_SIZE,
  GRN_INFO_NGRAM_UNIT_SIZE,
  /*
  GRN_INFO_VERSION,
  GRN_INFO_CONFIGURE_OPTIONS,
  GRN_INFO_CONFIG_PATH,
  */
  GRN_INFO_PARTIAL_MATCH_THRESHOLD,
  GRN_INFO_II_SPLIT_THRESHOLD,
  GRN_INFO_SUPPORT_ZLIB,
  GRN_INFO_SUPPORT_LZ4,
/* Just for backward compatibility. We'll remove it at 5.0.0. */
#define GRN_INFO_SUPPORT_LZO GRN_INFO_SUPPORT_LZ4
  GRN_INFO_NORMALIZER,
  GRN_INFO_TOKEN_FILTERS,
  GRN_INFO_SUPPORT_ZSTD,
  GRN_INFO_SUPPORT_ARROW
} grn_info_type;

GRN_API grn_obj *grn_obj_get_info(grn_ctx *ctx, grn_obj *obj, grn_info_type type, grn_obj *valuebuf);
GRN_API grn_rc grn_obj_set_info(grn_ctx *ctx, grn_obj *obj, grn_info_type type, grn_obj *value);
GRN_API grn_obj *grn_obj_get_element_info(grn_ctx *ctx, grn_obj *obj, grn_id id,
                                          grn_info_type type, grn_obj *value);
GRN_API grn_rc grn_obj_set_element_info(grn_ctx *ctx, grn_obj *obj, grn_id id,
                                        grn_info_type type, grn_obj *value);

GRN_API grn_obj *grn_obj_get_value(grn_ctx *ctx, grn_obj *obj, grn_id id, grn_obj *value);
GRN_API int grn_obj_get_values(grn_ctx *ctx, grn_obj *obj, grn_id offset, void **values);

#define GRN_COLUMN_EACH(ctx,column,id,value,block) do {\
  int _n;\
  grn_id id = 1;\
  while ((_n = grn_obj_get_values(ctx, column, id, (void **)&value)) > 0) {\
    for (; _n; _n--, id++, value++) {\
      block\
    }\
  }\
} while (0)

#define GRN_OBJ_SET_MASK               (0x07)
#define GRN_OBJ_SET                    (0x01)
#define GRN_OBJ_INCR                   (0x02)
#define GRN_OBJ_DECR                   (0x03)
#define GRN_OBJ_APPEND                 (0x04)
#define GRN_OBJ_PREPEND                (0x05)
#define GRN_OBJ_GET                    (0x01<<4)
#define GRN_OBJ_COMPARE                (0x01<<5)
#define GRN_OBJ_LOCK                   (0x01<<6)
#define GRN_OBJ_UNLOCK                 (0x01<<7)

GRN_API grn_rc grn_obj_set_value(grn_ctx *ctx, grn_obj *obj, grn_id id, grn_obj *value, int flags);
GRN_API grn_rc grn_obj_remove(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_rc grn_obj_remove_dependent(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_rc grn_obj_remove_force(grn_ctx *ctx,
                                    const char *name,
                                    int name_size);
GRN_API grn_rc grn_obj_rename(grn_ctx *ctx, grn_obj *obj,
                              const char *name, unsigned int name_size);
GRN_API grn_rc grn_table_rename(grn_ctx *ctx, grn_obj *table,
                                const char *name, unsigned int name_size);

GRN_API grn_rc grn_column_rename(grn_ctx *ctx, grn_obj *column,
                                 const char *name, unsigned int name_size);

GRN_API grn_rc grn_obj_close(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_rc grn_obj_reinit(grn_ctx *ctx, grn_obj *obj, grn_id domain, unsigned char flags);
GRN_API void grn_obj_unlink(grn_ctx *ctx, grn_obj *obj);

GRN_API grn_user_data *grn_obj_user_data(grn_ctx *ctx, grn_obj *obj);

GRN_API grn_rc grn_obj_set_finalizer(grn_ctx *ctx, grn_obj *obj, grn_proc_func *func);

GRN_API const char *grn_obj_path(grn_ctx *ctx, grn_obj *obj);
GRN_API int grn_obj_name(grn_ctx *ctx, grn_obj *obj, char *namebuf, int buf_size);

GRN_API int grn_column_name(grn_ctx *ctx, grn_obj *obj, char *namebuf, int buf_size);

GRN_API grn_id grn_obj_get_range(grn_ctx *ctx, grn_obj *obj);

#define GRN_OBJ_GET_DOMAIN(obj) \
  ((obj)->header.type == GRN_TABLE_NO_KEY ? GRN_ID_NIL : (obj)->header.domain)

GRN_API int grn_obj_expire(grn_ctx *ctx, grn_obj *obj, int threshold);
GRN_API int grn_obj_check(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_rc grn_obj_lock(grn_ctx *ctx, grn_obj *obj, grn_id id, int timeout);
GRN_API grn_rc grn_obj_unlock(grn_ctx *ctx, grn_obj *obj, grn_id id);
GRN_API grn_rc grn_obj_clear_lock(grn_ctx *ctx, grn_obj *obj);
GRN_API unsigned int grn_obj_is_locked(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_rc grn_obj_flush(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_rc grn_obj_flush_recursive(grn_ctx *ctx, grn_obj *obj);
GRN_API int grn_obj_defrag(grn_ctx *ctx, grn_obj *obj, int threshold);

GRN_API grn_obj *grn_obj_db(grn_ctx *ctx, grn_obj *obj);

GRN_API grn_id grn_obj_id(grn_ctx *ctx, grn_obj *obj);

/* Flags for grn_fuzzy_search_optarg::flags. */
#define GRN_TABLE_FUZZY_SEARCH_WITH_TRANSPOSITION                  (0x01)

typedef struct _grn_fuzzy_search_optarg grn_fuzzy_search_optarg;

struct _grn_fuzzy_search_optarg {
  unsigned int max_distance;
  unsigned int max_expansion;
  unsigned int prefix_match_size;
  int flags;
};

#define GRN_MATCH_INFO_GET_MIN_RECORD_ID                           (0x01)

typedef struct _grn_match_info grn_match_info;

struct _grn_match_info {
  int flags;
  grn_id min;
};

typedef struct _grn_search_optarg grn_search_optarg;

struct _grn_search_optarg {
  grn_operator mode;
  int similarity_threshold;
  int max_interval;
  int *weight_vector;
  int vector_size;
  grn_obj *proc;
  int max_size;
  grn_obj *scorer;
  grn_obj *scorer_args_expr;
  unsigned int scorer_args_expr_offset;
  grn_fuzzy_search_optarg fuzzy;
  grn_match_info match_info;
};

GRN_API grn_rc grn_obj_search(grn_ctx *ctx, grn_obj *obj, grn_obj *query,
                              grn_obj *res, grn_operator op, grn_search_optarg *optarg);

typedef grn_rc grn_selector_func(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                                 int nargs, grn_obj **args,
                                 grn_obj *res, grn_operator op);

GRN_API grn_rc grn_proc_set_selector(grn_ctx *ctx, grn_obj *proc,
                                     grn_selector_func selector);
GRN_API grn_rc grn_proc_set_selector_operator(grn_ctx *ctx,
                                              grn_obj *proc,
                                              grn_operator selector_op);
GRN_API grn_operator grn_proc_get_selector_operator(grn_ctx *ctx,
                                                    grn_obj *proc);

GRN_API grn_rc grn_proc_set_is_stable(grn_ctx *ctx,
                                      grn_obj *proc,
                                      grn_bool is_stable);
GRN_API grn_bool grn_proc_is_stable(grn_ctx *ctx, grn_obj *proc);

/*-------------------------------------------------------------
 * grn_vector
*/

GRN_API unsigned int grn_vector_size(grn_ctx *ctx, grn_obj *vector);

GRN_API grn_rc grn_vector_add_element(grn_ctx *ctx, grn_obj *vector,
                                      const char *str, unsigned int str_len,
                                      unsigned int weight, grn_id domain);

GRN_API unsigned int grn_vector_get_element(grn_ctx *ctx, grn_obj *vector,
                                            unsigned int offset, const char **str,
                                            unsigned int *weight, grn_id *domain);
GRN_API unsigned int grn_vector_pop_element(grn_ctx *ctx, grn_obj *vector,
                                            const char **str,
                                            unsigned int *weight,
                                            grn_id *domain);

/*-------------------------------------------------------------
 * grn_uvector
*/

GRN_API unsigned int grn_uvector_size(grn_ctx *ctx, grn_obj *uvector);
GRN_API unsigned int grn_uvector_element_size(grn_ctx *ctx, grn_obj *uvector);

GRN_API grn_rc grn_uvector_add_element(grn_ctx *ctx, grn_obj *vector,
                                       grn_id id, unsigned int weight);

GRN_API grn_id grn_uvector_get_element(grn_ctx *ctx, grn_obj *uvector,
                                       unsigned int offset,
                                       unsigned int *weight);

/*-------------------------------------------------------------
 * API for hook
 */

GRN_API int grn_proc_call_next(grn_ctx *ctx, grn_obj *exec_info, grn_obj *in, grn_obj *out);
GRN_API void *grn_proc_get_ctx_local_data(grn_ctx *ctx, grn_obj *exec_info);
GRN_API void *grn_proc_get_hook_local_data(grn_ctx *ctx, grn_obj *exec_info);

typedef enum {
  GRN_HOOK_SET = 0,
  GRN_HOOK_GET,
  GRN_HOOK_INSERT,
  GRN_HOOK_DELETE,
  GRN_HOOK_SELECT
} grn_hook_entry;

GRN_API grn_rc grn_obj_add_hook(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry,
                                int offset, grn_obj *proc, grn_obj *data);
GRN_API int grn_obj_get_nhooks(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry);
GRN_API grn_obj *grn_obj_get_hook(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry,
                                  int offset, grn_obj *data);
GRN_API grn_rc grn_obj_delete_hook(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry, int offset);

GRN_API grn_obj *grn_obj_open(grn_ctx *ctx, unsigned char type, grn_obj_flags flags, grn_id domain);

/* Deprecated since 5.0.1. Use grn_column_find_index_data() instead. */
GRN_API int grn_column_index(grn_ctx *ctx, grn_obj *column, grn_operator op,
                             grn_obj **indexbuf, int buf_size, int *section);

/* @since 5.0.1. */
typedef struct _grn_index_datum {
  grn_obj *index;
  unsigned int section;
} grn_index_datum;

/* @since 5.0.1. */
GRN_API unsigned int grn_column_find_index_data(grn_ctx *ctx, grn_obj *column,
                                                grn_operator op,
                                                grn_index_datum *index_data,
                                                unsigned int n_index_data);
/* @since 5.1.2. */
GRN_API uint32_t grn_column_get_all_index_data(grn_ctx *ctx,
                                               grn_obj *column,
                                               grn_index_datum *index_data,
                                               uint32_t n_index_data);

GRN_API grn_rc grn_obj_delete_by_id(grn_ctx *ctx, grn_obj *db, grn_id id, grn_bool removep);
GRN_API grn_rc grn_obj_path_by_id(grn_ctx *ctx, grn_obj *db, grn_id id, char *buffer);

/* geo */

typedef struct {
  int latitude;
  int longitude;
} grn_geo_point;

GRN_API grn_rc grn_geo_select_in_rectangle(grn_ctx *ctx,
                                           grn_obj *index,
                                           grn_obj *top_left_point,
                                           grn_obj *bottom_right_point,
                                           grn_obj *res,
                                           grn_operator op);
GRN_API unsigned int grn_geo_estimate_size_in_rectangle(grn_ctx *ctx,
                                                        grn_obj *index,
                                                        grn_obj *top_left_point,
                                                        grn_obj *bottom_right_point);
/* Deprecated since 4.0.8. Use grn_geo_estimate_size_in_rectangle() instead. */
GRN_API int grn_geo_estimate_in_rectangle(grn_ctx *ctx,
                                          grn_obj *index,
                                          grn_obj *top_left_point,
                                          grn_obj *bottom_right_point);
GRN_API grn_obj *grn_geo_cursor_open_in_rectangle(grn_ctx *ctx,
                                                  grn_obj *index,
                                                  grn_obj *top_left_point,
                                                  grn_obj *bottom_right_point,
                                                  int offset,
                                                  int limit);
GRN_API grn_posting *grn_geo_cursor_next(grn_ctx *ctx, grn_obj *cursor);


/* query & snippet */

#ifndef GRN_QUERY_AND
#define GRN_QUERY_AND '+'
#endif /* GRN_QUERY_AND */
#ifndef GRN_QUERY_AND_NOT
# ifdef GRN_QUERY_BUT
   /* Deprecated. Just for backward compatibility. */
#  define GRN_QUERY_AND_NOT GRN_QUERY_BUT
# else
#  define GRN_QUERY_AND_NOT '-'
# endif /* GRN_QUERY_BUT */
#endif /* GRN_QUERY_AND_NOT */
#ifndef GRN_QUERY_ADJ_INC
#define GRN_QUERY_ADJ_INC '>'
#endif /* GRN_QUERY_ADJ_POS2 */
#ifndef GRN_QUERY_ADJ_DEC
#define GRN_QUERY_ADJ_DEC '<'
#endif /* GRN_QUERY_ADJ_POS1 */
#ifndef GRN_QUERY_ADJ_NEG
#define GRN_QUERY_ADJ_NEG '~'
#endif /* GRN_QUERY_ADJ_NEG */
#ifndef GRN_QUERY_PREFIX
#define GRN_QUERY_PREFIX '*'
#endif /* GRN_QUERY_PREFIX */
#ifndef GRN_QUERY_PARENL
#define GRN_QUERY_PARENL '('
#endif /* GRN_QUERY_PARENL */
#ifndef GRN_QUERY_PARENR
#define GRN_QUERY_PARENR ')'
#endif /* GRN_QUERY_PARENR */
#ifndef GRN_QUERY_QUOTEL
#define GRN_QUERY_QUOTEL '"'
#endif /* GRN_QUERY_QUOTEL */
#ifndef GRN_QUERY_QUOTER
#define GRN_QUERY_QUOTER '"'
#endif /* GRN_QUERY_QUOTER */
#ifndef GRN_QUERY_ESCAPE
#define GRN_QUERY_ESCAPE '\\'
#endif /* GRN_QUERY_ESCAPE */
#ifndef GRN_QUERY_COLUMN
#define GRN_QUERY_COLUMN ':'
#endif /* GRN_QUERY_COLUMN */

typedef struct _grn_snip_mapping grn_snip_mapping;

struct _grn_snip_mapping {
  void *dummy;
};

#define GRN_SNIP_NORMALIZE             (0x01<<0)
#define GRN_SNIP_COPY_TAG              (0x01<<1)
#define GRN_SNIP_SKIP_LEADING_SPACES   (0x01<<2)

#define GRN_SNIP_MAPPING_HTML_ESCAPE   ((grn_snip_mapping *)-1)

GRN_API grn_obj *grn_snip_open(grn_ctx *ctx, int flags, unsigned int width,
                               unsigned int max_results,
                               const char *defaultopentag, unsigned int defaultopentag_len,
                               const char *defaultclosetag, unsigned int defaultclosetag_len,
                               grn_snip_mapping *mapping);
GRN_API grn_rc grn_snip_add_cond(grn_ctx *ctx, grn_obj *snip,
                                 const char *keyword, unsigned int keyword_len,
                                 const char *opentag, unsigned int opentag_len,
                                 const char *closetag, unsigned int closetag_len);
GRN_API grn_rc grn_snip_set_normalizer(grn_ctx *ctx, grn_obj *snip,
                                       grn_obj *normalizer);
GRN_API grn_obj *grn_snip_get_normalizer(grn_ctx *ctx, grn_obj *snip);
GRN_API grn_rc grn_snip_exec(grn_ctx *ctx, grn_obj *snip,
                             const char *string, unsigned int string_len,
                             unsigned int *nresults, unsigned int *max_tagged_len);
GRN_API grn_rc grn_snip_get_result(grn_ctx *ctx, grn_obj *snip, const unsigned int index,
                                   char *result, unsigned int *result_len);

/* log */

#define GRN_LOG_TIME                   (0x01<<0)
#define GRN_LOG_TITLE                  (0x01<<1)
#define GRN_LOG_MESSAGE                (0x01<<2)
#define GRN_LOG_LOCATION               (0x01<<3)
#define GRN_LOG_PID                    (0x01<<4)

/* Deprecated since 2.1.2. Use grn_logger instead. */
typedef struct _grn_logger_info grn_logger_info;

/* Deprecated since 2.1.2. Use grn_logger instead. */
struct _grn_logger_info {
  grn_log_level max_level;
  int flags;
  void (*func)(int, const char *, const char *, const char *, const char *, void *);
  void *func_arg;
};

/* Deprecated since 2.1.2. Use grn_logger_set() instead. */
GRN_API grn_rc grn_logger_info_set(grn_ctx *ctx, const grn_logger_info *info);

typedef struct _grn_logger grn_logger;

struct _grn_logger {
  grn_log_level max_level;
  int flags;
  void *user_data;
  void (*log)(grn_ctx *ctx, grn_log_level level,
              const char *timestamp, const char *title, const char *message,
              const char *location, void *user_data);
  void (*reopen)(grn_ctx *ctx, void *user_data);
  void (*fin)(grn_ctx *ctx, void *user_data);
};

GRN_API grn_rc grn_logger_set(grn_ctx *ctx, const grn_logger *logger);

GRN_API void grn_logger_set_max_level(grn_ctx *ctx, grn_log_level max_level);
GRN_API grn_log_level grn_logger_get_max_level(grn_ctx *ctx);

#ifdef __GNUC__
# define GRN_ATTRIBUTE_PRINTF(fmt_pos) \
  __attribute__ ((format(printf, fmt_pos, fmt_pos + 1)))
#else
# define GRN_ATTRIBUTE_PRINTF(fmt_pos)
#endif /* __GNUC__ */

#if defined(__clang__)
# if __has_attribute(__alloc_size__)
#  define HAVE_ALLOC_SIZE_ATTRIBUTE
# endif /* __has_attribute(__alloc_size__) */
#elif defined(__GNUC__) && \
  ((__GNUC__ >= 5) || (__GNUC__ > 4 && __GNUC_MINOR__ >= 3))
# define HAVE_ALLOC_SIZE_ATTRIBUTE
#endif /* __clang__ */

#ifdef HAVE_ALLOC_SIZE_ATTRIBUTE
# define GRN_ATTRIBUTE_ALLOC_SIZE(size) \
  __attribute__ ((alloc_size(size)))
# define GRN_ATTRIBUTE_ALLOC_SIZE_N(n, size) \
  __attribute__ ((alloc_size(n, size)))
#else
# define GRN_ATTRIBUTE_ALLOC_SIZE(size)
# define GRN_ATTRIBUTE_ALLOC_SIZE_N(n, size)
#endif /* HAVE_ALLOC_SIZE_ATTRIBUTE */

GRN_API void grn_logger_put(grn_ctx *ctx, grn_log_level level,
                            const char *file, int line, const char *func, const char *fmt, ...) GRN_ATTRIBUTE_PRINTF(6);
GRN_API void grn_logger_putv(grn_ctx *ctx,
                             grn_log_level level,
                             const char *file,
                             int line,
                             const char *func,
                             const char *fmt,
                             va_list ap);
GRN_API void grn_logger_reopen(grn_ctx *ctx);

GRN_API grn_bool grn_logger_pass(grn_ctx *ctx, grn_log_level level);

#ifndef GRN_LOG_DEFAULT_LEVEL
# define GRN_LOG_DEFAULT_LEVEL GRN_LOG_NOTICE
#endif /* GRN_LOG_DEFAULT_LEVEL */

GRN_API void grn_default_logger_set_max_level(grn_log_level level);
GRN_API grn_log_level grn_default_logger_get_max_level(void);
GRN_API void grn_default_logger_set_flags(int flags);
GRN_API int grn_default_logger_get_flags(void);
GRN_API void grn_default_logger_set_path(const char *path);
GRN_API const char *grn_default_logger_get_path(void);
GRN_API void grn_default_logger_set_rotate_threshold_size(off_t threshold);
GRN_API off_t grn_default_logger_get_rotate_threshold_size(void);

#define GRN_LOG(ctx,level,...) do {\
  if (grn_logger_pass(ctx, level)) {\
    grn_logger_put(ctx, (level), __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__); \
  }\
} while (0)

typedef struct _grn_query_logger grn_query_logger;

struct _grn_query_logger {
  unsigned int flags;
  void *user_data;
  void (*log)(grn_ctx *ctx, unsigned int flag,
              const char *timestamp, const char *info, const char *message,
              void *user_data);
  void (*reopen)(grn_ctx *ctx, void *user_data);
  void (*fin)(grn_ctx *ctx, void *user_data);
};

GRN_API grn_bool grn_query_log_flags_parse(const char *string,
                                           int string_size,
                                           unsigned int *flags);

GRN_API grn_rc grn_query_logger_set(grn_ctx *ctx, const grn_query_logger *logger);
GRN_API void grn_query_logger_set_flags(grn_ctx *ctx, unsigned int flags);
GRN_API void grn_query_logger_add_flags(grn_ctx *ctx, unsigned int flags);
GRN_API void grn_query_logger_remove_flags(grn_ctx *ctx, unsigned int flags);
GRN_API unsigned int grn_query_logger_get_flags(grn_ctx *ctx);

GRN_API void grn_query_logger_put(grn_ctx *ctx, unsigned int flag,
                                  const char *mark,
                                  const char *format, ...) GRN_ATTRIBUTE_PRINTF(4);
GRN_API void grn_query_logger_reopen(grn_ctx *ctx);

GRN_API grn_bool grn_query_logger_pass(grn_ctx *ctx, unsigned int flag);

GRN_API void grn_default_query_logger_set_flags(unsigned int flags);
GRN_API unsigned int grn_default_query_logger_get_flags(void);
GRN_API void grn_default_query_logger_set_path(const char *path);
GRN_API const char *grn_default_query_logger_get_path(void);
GRN_API void grn_default_query_logger_set_rotate_threshold_size(off_t threshold);
GRN_API off_t grn_default_query_logger_get_rotate_threshold_size(void);

#define GRN_QUERY_LOG(ctx, flag, mark, format, ...) do {\
  if (grn_query_logger_pass(ctx, flag)) {\
    grn_query_logger_put(ctx, (flag), (mark), format, __VA_ARGS__);\
  }\
} while (0)

/* grn_bulk */

#define GRN_BULK_BUFSIZE (sizeof(grn_obj) - sizeof(grn_obj_header))
/* This assumes that GRN_BULK_BUFSIZE is less than 32 (= 0x20). */
#define GRN_BULK_BUFSIZE_MAX 0x1f
#define GRN_BULK_SIZE_IN_FLAGS(flags) ((flags) & GRN_BULK_BUFSIZE_MAX)
#define GRN_BULK_OUTP(bulk) ((bulk)->header.impl_flags & GRN_OBJ_OUTPLACE)
#define GRN_BULK_REWIND(bulk) do {\
  if ((bulk)->header.type == GRN_VECTOR) {\
    grn_obj *_body = (bulk)->u.v.body;\
    if (_body) {\
      if (GRN_BULK_OUTP(_body)) {\
        (_body)->u.b.curr = (_body)->u.b.head;\
      } else {\
        (_body)->header.flags &= ~GRN_BULK_BUFSIZE_MAX;\
      }\
    }\
    (bulk)->u.v.n_sections = 0;\
  } else {\
    if (GRN_BULK_OUTP(bulk)) {\
      (bulk)->u.b.curr = (bulk)->u.b.head;\
    } else {\
      (bulk)->header.flags &= ~GRN_BULK_BUFSIZE_MAX;\
    }\
  }\
} while (0)
#define GRN_BULK_INCR_LEN(bulk,len) do {\
  if (GRN_BULK_OUTP(bulk)) {\
    (bulk)->u.b.curr += (len);\
  } else {\
    (bulk)->header.flags += (grn_obj_flags)(len);\
  }\
} while (0)
#define GRN_BULK_WSIZE(bulk) \
  (GRN_BULK_OUTP(bulk)\
   ? ((bulk)->u.b.tail - (bulk)->u.b.head)\
   : GRN_BULK_BUFSIZE)
#define GRN_BULK_REST(bulk) \
  (GRN_BULK_OUTP(bulk)\
   ? ((bulk)->u.b.tail - (bulk)->u.b.curr)\
   : GRN_BULK_BUFSIZE - GRN_BULK_SIZE_IN_FLAGS((bulk)->header.flags))
#define GRN_BULK_VSIZE(bulk) \
  (GRN_BULK_OUTP(bulk)\
   ? ((bulk)->u.b.curr - (bulk)->u.b.head)\
   : GRN_BULK_SIZE_IN_FLAGS((bulk)->header.flags))
#define GRN_BULK_EMPTYP(bulk) \
  (GRN_BULK_OUTP(bulk)\
   ? ((bulk)->u.b.curr == (bulk)->u.b.head)\
   : !(GRN_BULK_SIZE_IN_FLAGS((bulk)->header.flags)))
#define GRN_BULK_HEAD(bulk) \
  (GRN_BULK_OUTP(bulk)\
   ? ((bulk)->u.b.head)\
   : (char *)&((bulk)->u.b.head))
#define GRN_BULK_CURR(bulk) \
  (GRN_BULK_OUTP(bulk)\
   ? ((bulk)->u.b.curr)\
   : (char *)&((bulk)->u.b.head) + GRN_BULK_SIZE_IN_FLAGS((bulk)->header.flags))
#define GRN_BULK_TAIL(bulk) \
  (GRN_BULK_OUTP(bulk)\
   ? ((bulk)->u.b.tail)\
   : (char *)&((bulk)[1]))

GRN_API grn_rc grn_bulk_reinit(grn_ctx *ctx, grn_obj *bulk, unsigned int size);
GRN_API grn_rc grn_bulk_resize(grn_ctx *ctx, grn_obj *bulk, unsigned int newsize);
GRN_API grn_rc grn_bulk_write(grn_ctx *ctx, grn_obj *bulk,
                              const char *str, unsigned int len);
GRN_API grn_rc grn_bulk_write_from(grn_ctx *ctx, grn_obj *bulk,
                                   const char *str, unsigned int from, unsigned int len);
GRN_API grn_rc grn_bulk_reserve(grn_ctx *ctx, grn_obj *bulk, unsigned int len);
GRN_API grn_rc grn_bulk_space(grn_ctx *ctx, grn_obj *bulk, unsigned int len);
GRN_API grn_rc grn_bulk_truncate(grn_ctx *ctx, grn_obj *bulk, unsigned int len);
GRN_API grn_rc grn_bulk_fin(grn_ctx *ctx, grn_obj *bulk);

/* grn_text */

GRN_API grn_rc grn_text_itoa(grn_ctx *ctx, grn_obj *bulk, int i);
GRN_API grn_rc grn_text_itoa_padded(grn_ctx *ctx, grn_obj *bulk, int i, char ch, unsigned int len);
GRN_API grn_rc grn_text_lltoa(grn_ctx *ctx, grn_obj *bulk, long long int i);
GRN_API grn_rc grn_text_ftoa(grn_ctx *ctx, grn_obj *bulk, double d);
GRN_API grn_rc grn_text_itoh(grn_ctx *ctx, grn_obj *bulk, int i, unsigned int len);
GRN_API grn_rc grn_text_itob(grn_ctx *ctx, grn_obj *bulk, grn_id id);
GRN_API grn_rc grn_text_lltob32h(grn_ctx *ctx, grn_obj *bulk, long long int i);
GRN_API grn_rc grn_text_benc(grn_ctx *ctx, grn_obj *bulk, unsigned int v);
GRN_API grn_rc grn_text_esc(grn_ctx *ctx, grn_obj *bulk, const char *s, unsigned int len);
GRN_API grn_rc grn_text_urlenc(grn_ctx *ctx, grn_obj *buf,
                               const char *str, unsigned int len);
GRN_API const char *grn_text_urldec(grn_ctx *ctx, grn_obj *buf,
                                    const char *s, const char *e, char d);
GRN_API grn_rc grn_text_escape_xml(grn_ctx *ctx, grn_obj *buf,
                                   const char *s, unsigned int len);
GRN_API grn_rc grn_text_time2rfc1123(grn_ctx *ctx, grn_obj *bulk, int sec);
GRN_API grn_rc grn_text_printf(grn_ctx *ctx, grn_obj *bulk,
                               const char *format, ...) GRN_ATTRIBUTE_PRINTF(3);
GRN_API grn_rc grn_text_vprintf(grn_ctx *ctx, grn_obj *bulk,
                                const char *format, va_list args);

GRN_API void grn_ctx_recv_handler_set(grn_ctx *,
                                      void (*func)(grn_ctx *, int, void *),
                                      void *func_arg);

/* various values exchanged via grn_obj */

#define GRN_OBJ_DO_SHALLOW_COPY        (GRN_OBJ_REFER|GRN_OBJ_OUTPLACE)
#define GRN_OBJ_VECTOR                 (0x01<<7)

#define GRN_OBJ_MUTABLE(obj) ((obj) && (obj)->header.type <= GRN_VECTOR)

#define GRN_VALUE_FIX_SIZE_INIT(obj,flags,domain)\
  GRN_OBJ_INIT((obj), ((flags) & GRN_OBJ_VECTOR) ? GRN_UVECTOR : GRN_BULK,\
               ((flags) & GRN_OBJ_DO_SHALLOW_COPY), (domain))
#define GRN_VALUE_VAR_SIZE_INIT(obj,flags,domain)\
  GRN_OBJ_INIT((obj), ((flags) & GRN_OBJ_VECTOR) ? GRN_VECTOR : GRN_BULK,\
               ((flags) & GRN_OBJ_DO_SHALLOW_COPY), (domain))

#define GRN_VOID_INIT(obj) GRN_OBJ_INIT((obj), GRN_VOID, 0, GRN_DB_VOID)
#define GRN_TEXT_INIT(obj,flags) \
  GRN_VALUE_VAR_SIZE_INIT(obj, flags, GRN_DB_TEXT)
#define GRN_SHORT_TEXT_INIT(obj,flags) \
  GRN_VALUE_VAR_SIZE_INIT(obj, flags, GRN_DB_SHORT_TEXT)
#define GRN_LONG_TEXT_INIT(obj,flags) \
  GRN_VALUE_VAR_SIZE_INIT(obj, flags, GRN_DB_LONG_TEXT)
#define GRN_TEXT_SET_REF(obj,str,len) do {\
  (obj)->u.b.head = (char *)(str);\
  (obj)->u.b.curr = (char *)(str) + (len);\
} while (0)
#define GRN_TEXT_SET(ctx,obj,str,len) do {\
  if ((obj)->header.impl_flags & GRN_OBJ_REFER) {\
    GRN_TEXT_SET_REF((obj), (str), (len));\
  } else {\
    grn_bulk_write_from((ctx), (obj), (const char *)(str), 0, (unsigned int)(len));\
  }\
} while (0)
#define GRN_TEXT_PUT(ctx,obj,str,len) \
  grn_bulk_write((ctx), (obj), (const char *)(str), (unsigned int)(len))
#define GRN_TEXT_PUTC(ctx,obj,c) do {\
  char _c = (c); grn_bulk_write((ctx), (obj), &_c, 1);\
} while (0)

#define GRN_TEXT_PUTS(ctx,obj,str) GRN_TEXT_PUT((ctx), (obj), (str), strlen(str))
#define GRN_TEXT_SETS(ctx,obj,str) GRN_TEXT_SET((ctx), (obj), (str), strlen(str))
#define GRN_TEXT_VALUE(obj) GRN_BULK_HEAD(obj)
#define GRN_TEXT_LEN(obj) GRN_BULK_VSIZE(obj)

#define GRN_TEXT_EQUAL_CSTRING(bulk, string)\
  (GRN_TEXT_LEN(bulk) == strlen(string) &&\
   memcmp(GRN_TEXT_VALUE(bulk), string, GRN_TEXT_LEN(bulk)) == 0)

#define GRN_BOOL_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_BOOL)
#define GRN_INT8_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_INT8)
#define GRN_UINT8_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_UINT8)
#define GRN_INT16_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_INT16)
#define GRN_UINT16_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_UINT16)
#define GRN_INT32_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_INT32)
#define GRN_UINT32_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_UINT32)
#define GRN_INT64_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_INT64)
#define GRN_UINT64_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_UINT64)
#define GRN_FLOAT_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_FLOAT)
#define GRN_TIME_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_TIME)
#define GRN_RECORD_INIT GRN_VALUE_FIX_SIZE_INIT
#define GRN_PTR_INIT(obj,flags,domain)\
  GRN_OBJ_INIT((obj), ((flags) & GRN_OBJ_VECTOR) ? GRN_PVECTOR : GRN_PTR,\
               ((flags) & (GRN_OBJ_DO_SHALLOW_COPY | GRN_OBJ_OWN)),\
               (domain))
#define GRN_TOKYO_GEO_POINT_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_TOKYO_GEO_POINT)
#define GRN_WGS84_GEO_POINT_INIT(obj,flags) \
  GRN_VALUE_FIX_SIZE_INIT(obj, flags, GRN_DB_WGS84_GEO_POINT)

#define GRN_BOOL_SET(ctx,obj,val) do {\
  unsigned char _val = (unsigned char)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(unsigned char));\
} while (0)
#define GRN_INT8_SET(ctx,obj,val) do {\
  signed char _val = (signed char)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(signed char));\
} while (0)
#define GRN_UINT8_SET(ctx,obj,val) do {\
  unsigned char _val = (unsigned char)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(unsigned char));\
} while (0)
#define GRN_INT16_SET(ctx,obj,val) do {\
  signed short _val = (signed short)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(signed short));\
} while (0)
#define GRN_UINT16_SET(ctx,obj,val) do {\
  unsigned short _val = (unsigned short)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(unsigned short));\
} while (0)
#define GRN_INT32_SET(ctx,obj,val) do {\
  int _val = (int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(int));\
} while (0)
#define GRN_UINT32_SET(ctx,obj,val) do {\
  unsigned int _val = (unsigned int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(unsigned int));\
} while (0)
#define GRN_INT64_SET(ctx,obj,val) do {\
  long long int _val = (long long int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(long long int));\
} while (0)
#define GRN_UINT64_SET(ctx,obj,val) do {\
  long long unsigned int _val = (long long unsigned int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(long long unsigned int));\
} while (0)
#define GRN_FLOAT_SET(ctx,obj,val) do {\
  double _val = (double)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(double));\
} while (0)
#define GRN_TIME_SET GRN_INT64_SET
#define GRN_RECORD_SET(ctx,obj,val) do {\
  grn_id _val = (grn_id)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(grn_id));\
} while (0)
#define GRN_PTR_SET(ctx,obj,val) do {\
  grn_obj *_val = (grn_obj *)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(grn_obj *));\
} while (0)

#define GRN_GEO_DEGREE2MSEC(degree)\
  ((int)((degree) * 3600 * 1000 + ((degree) > 0 ? 0.5 : -0.5)))
#define GRN_GEO_MSEC2DEGREE(msec)\
  ((((int)(msec)) / 3600.0) * 0.001)

#define GRN_GEO_POINT_SET(ctx,obj,_latitude,_longitude) do {\
  grn_geo_point _val;\
  _val.latitude = (int)(_latitude);\
  _val.longitude = (int)(_longitude);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val, 0, sizeof(grn_geo_point));\
} while (0)

#define GRN_BOOL_SET_AT(ctx,obj,offset,val) do {\
  unsigned char _val = (unsigned char)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset), sizeof(unsigned char));\
} while (0)
#define GRN_INT8_SET_AT(ctx,obj,offset,val) do {\
  signed char _val = (signed char)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(signed char), sizeof(signed char));\
} while (0)
#define GRN_UINT8_SET_AT(ctx,obj,offset,val) do { \
  unsigned char _val = (unsigned char)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(unsigned char), sizeof(unsigned char));\
} while (0)
#define GRN_INT16_SET_AT(ctx,obj,offset,val) do {\
  signed short _val = (signed short)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(signed short), sizeof(signed short));\
} while (0)
#define GRN_UINT16_SET_AT(ctx,obj,offset,val) do { \
  unsigned short _val = (unsigned short)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(unsigned short), sizeof(unsigned short));\
} while (0)
#define GRN_INT32_SET_AT(ctx,obj,offset,val) do {\
  int _val = (int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(int), sizeof(int));\
} while (0)
#define GRN_UINT32_SET_AT(ctx,obj,offset,val) do { \
  unsigned int _val = (unsigned int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(unsigned int), sizeof(unsigned int));\
} while (0)
#define GRN_INT64_SET_AT(ctx,obj,offset,val) do {\
  long long int _val = (long long int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(long long int), sizeof(long long int));\
} while (0)
#define GRN_UINT64_SET_AT(ctx,obj,offset,val) do {\
  long long unsigned int _val = (long long unsigned int)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(long long unsigned int),\
                      sizeof(long long unsigned int));\
} while (0)
#define GRN_FLOAT_SET_AT(ctx,obj,offset,val) do {\
  double _val = (double)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(double), sizeof(double));\
} while (0)
#define GRN_TIME_SET_AT GRN_INT64_SET_AT
#define GRN_RECORD_SET_AT(ctx,obj,offset,val) do {\
  grn_id _val = (grn_id)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(grn_id), sizeof(grn_id));\
} while (0)
#define GRN_PTR_SET_AT(ctx,obj,offset,val) do {\
  grn_obj *_val = (grn_obj *)(val);\
  grn_bulk_write_from((ctx), (obj), (char *)&_val,\
                      (offset) * sizeof(grn_obj *), sizeof(grn_obj *));\
} while (0)

#define GRN_BOOL_VALUE(obj) (*((unsigned char *)GRN_BULK_HEAD(obj)))
#define GRN_INT8_VALUE(obj) (*((signed char *)GRN_BULK_HEAD(obj)))
#define GRN_UINT8_VALUE(obj) (*((unsigned char *)GRN_BULK_HEAD(obj)))
#define GRN_INT16_VALUE(obj) (*((signed short *)GRN_BULK_HEAD(obj)))
#define GRN_UINT16_VALUE(obj) (*((unsigned short *)GRN_BULK_HEAD(obj)))
#define GRN_INT32_VALUE(obj) (*((int *)GRN_BULK_HEAD(obj)))
#define GRN_UINT32_VALUE(obj) (*((unsigned int *)GRN_BULK_HEAD(obj)))
#define GRN_INT64_VALUE(obj) (*((long long int *)GRN_BULK_HEAD(obj)))
#define GRN_UINT64_VALUE(obj) (*((long long unsigned int *)GRN_BULK_HEAD(obj)))
#define GRN_FLOAT_VALUE(obj) (*((double *)GRN_BULK_HEAD(obj)))
#define GRN_TIME_VALUE GRN_INT64_VALUE
#define GRN_RECORD_VALUE(obj) (*((grn_id *)GRN_BULK_HEAD(obj)))
#define GRN_PTR_VALUE(obj) (*((grn_obj **)GRN_BULK_HEAD(obj)))
#define GRN_GEO_POINT_VALUE(obj,_latitude,_longitude) do {\
  grn_geo_point *_val = (grn_geo_point *)GRN_BULK_HEAD(obj);\
  _latitude = _val->latitude;\
  _longitude = _val->longitude;\
} while (0)

#define GRN_BOOL_VALUE_AT(obj,offset) (((unsigned char *)GRN_BULK_HEAD(obj))[offset])
#define GRN_INT8_VALUE_AT(obj,offset) (((signed char *)GRN_BULK_HEAD(obj))[offset])
#define GRN_UINT8_VALUE_AT(obj,offset) (((unsigned char *)GRN_BULK_HEAD(obj))[offset])
#define GRN_INT16_VALUE_AT(obj,offset) (((signed short *)GRN_BULK_HEAD(obj))[offset])
#define GRN_UINT16_VALUE_AT(obj,offset) (((unsigned short *)GRN_BULK_HEAD(obj))[offset])
#define GRN_INT32_VALUE_AT(obj,offset) (((int *)GRN_BULK_HEAD(obj))[offset])
#define GRN_UINT32_VALUE_AT(obj,offset) (((unsigned int *)GRN_BULK_HEAD(obj))[offset])
#define GRN_INT64_VALUE_AT(obj,offset) (((long long int *)GRN_BULK_HEAD(obj))[offset])
#define GRN_UINT64_VALUE_AT(obj,offset) (((long long unsigned int *)GRN_BULK_HEAD(obj))[offset])
#define GRN_FLOAT_VALUE_AT(obj,offset) (((double *)GRN_BULK_HEAD(obj))[offset])
#define GRN_TIME_VALUE_AT GRN_INT64_VALUE_AT
#define GRN_RECORD_VALUE_AT(obj,offset) (((grn_id *)GRN_BULK_HEAD(obj))[offset])
#define GRN_PTR_VALUE_AT(obj,offset) (((grn_obj **)GRN_BULK_HEAD(obj))[offset])

#define GRN_BOOL_PUT(ctx,obj,val) do {\
  unsigned char _val = (unsigned char)(val);\
  grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(unsigned char));\
} while (0)
#define GRN_INT8_PUT(ctx,obj,val) do {\
  signed char _val = (signed char)(val); grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(signed char));\
} while (0)
#define GRN_UINT8_PUT(ctx,obj,val) do {\
  unsigned char _val = (unsigned char)(val);\
  grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(unsigned char));\
} while (0)
#define GRN_INT16_PUT(ctx,obj,val) do {\
  signed short _val = (signed short)(val); grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(signed short));\
} while (0)
#define GRN_UINT16_PUT(ctx,obj,val) do {\
  unsigned short _val = (unsigned short)(val);\
  grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(unsigned short));\
} while (0)
#define GRN_INT32_PUT(ctx,obj,val) do {\
  int _val = (int)(val); grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(int));\
} while (0)
#define GRN_UINT32_PUT(ctx,obj,val) do {\
  unsigned int _val = (unsigned int)(val);\
  grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(unsigned int));\
} while (0)
#define GRN_INT64_PUT(ctx,obj,val) do {\
  long long int _val = (long long int)(val);\
  grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(long long int));\
} while (0)
#define GRN_UINT64_PUT(ctx,obj,val) do {\
  long long unsigned int _val = (long long unsigned int)(val);\
  grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(long long unsigned int));\
} while (0)
#define GRN_FLOAT_PUT(ctx,obj,val) do {\
  double _val = (double)(val); grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(double));\
} while (0)
#define GRN_TIME_PUT GRN_INT64_PUT
#define GRN_RECORD_PUT(ctx,obj,val) do {\
  grn_id _val = (grn_id)(val); grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(grn_id));\
} while (0)
#define GRN_PTR_PUT(ctx,obj,val) do {\
  grn_obj *_val = (grn_obj *)(val);\
  grn_bulk_write((ctx), (obj), (char *)&_val, sizeof(grn_obj *));\
} while (0)

#define GRN_BULK_POP(obj, value, type, default) do {\
    if ((size_t) GRN_BULK_VSIZE(obj) >= sizeof(type)) {  \
    GRN_BULK_INCR_LEN((obj), -(sizeof(type)));\
    value = *(type *)(GRN_BULK_CURR(obj));\
  } else {\
    value = default;\
  }\
} while (0)
#define GRN_BOOL_POP(obj, value) GRN_BULK_POP(obj, value, unsigned char, 0)
#define GRN_INT8_POP(obj, value) GRN_BULK_POP(obj, value, int8_t, 0)
#define GRN_UINT8_POP(obj, value) GRN_BULK_POP(obj, value, uint8_t, 0)
#define GRN_INT16_POP(obj, value) GRN_BULK_POP(obj, value, int16_t, 0)
#define GRN_UINT16_POP(obj, value) GRN_BULK_POP(obj, value, uint16_t, 0)
#define GRN_INT32_POP(obj, value) GRN_BULK_POP(obj, value, int32_t, 0)
#define GRN_UINT32_POP(obj, value) GRN_BULK_POP(obj, value, uint32_t, 0)
#define GRN_INT64_POP(obj, value) GRN_BULK_POP(obj, value, int64_t, 0)
#define GRN_UINT64_POP(obj, value) GRN_BULK_POP(obj, value, uint64_t, 0)
#define GRN_FLOAT_POP(obj, value) GRN_BULK_POP(obj, value, double, 0.0)
#define GRN_TIME_POP GRN_INT64_POP
#define GRN_RECORD_POP(obj, value) GRN_BULK_POP(obj, value, grn_id, GRN_ID_NIL)
#define GRN_PTR_POP(obj, value) GRN_BULK_POP(obj, value, grn_obj *, NULL)

/* grn_str: deprecated. use grn_string instead. */

typedef struct {
  const char *orig;
  char *norm;
  short *checks;
  unsigned char *ctypes;
  int flags;
  unsigned int orig_blen;
  unsigned int norm_blen;
  unsigned int length;
  grn_encoding encoding;
} grn_str;

#define GRN_STR_REMOVEBLANK            (0x01<<0)
#define GRN_STR_WITH_CTYPES            (0x01<<1)
#define GRN_STR_WITH_CHECKS            (0x01<<2)
#define GRN_STR_NORMALIZE              GRN_OBJ_KEY_NORMALIZE

GRN_API grn_str *grn_str_open(grn_ctx *ctx, const char *str, unsigned int str_len,
                              int flags);
GRN_API grn_rc grn_str_close(grn_ctx *ctx, grn_str *nstr);

/* grn_string */

#define GRN_STRING_REMOVE_BLANK               (0x01<<0)
#define GRN_STRING_WITH_TYPES                 (0x01<<1)
#define GRN_STRING_WITH_CHECKS                (0x01<<2)
#define GRN_STRING_REMOVE_TOKENIZED_DELIMITER (0x01<<3)

#define GRN_NORMALIZER_AUTO ((grn_obj *)1)

#define GRN_CHAR_BLANK 0x80
#define GRN_CHAR_IS_BLANK(c) ((c) & (GRN_CHAR_BLANK))
#define GRN_CHAR_TYPE(c) ((c) & 0x7f)

typedef enum {
  GRN_CHAR_NULL = 0,
  GRN_CHAR_ALPHA,
  GRN_CHAR_DIGIT,
  GRN_CHAR_SYMBOL,
  GRN_CHAR_HIRAGANA,
  GRN_CHAR_KATAKANA,
  GRN_CHAR_KANJI,
  GRN_CHAR_OTHERS
} grn_char_type;

GRN_API grn_obj *grn_string_open(grn_ctx *ctx,
                                 const char *string,
                                 unsigned int length_in_bytes,
                                 grn_obj *normalizer, int flags);
GRN_API grn_rc grn_string_get_original(grn_ctx *ctx, grn_obj *string,
                                       const char **original,
                                       unsigned int *length_in_bytes);
GRN_API int grn_string_get_flags(grn_ctx *ctx, grn_obj *string);
GRN_API grn_rc grn_string_get_normalized(grn_ctx *ctx, grn_obj *string,
                                         const char **normalized,
                                         unsigned int *length_in_bytes,
                                         unsigned int *n_characters);
GRN_API grn_rc grn_string_set_normalized(grn_ctx *ctx, grn_obj *string,
                                         char *normalized,
                                         unsigned int length_in_bytes,
                                         unsigned int n_characters);
GRN_API const short *grn_string_get_checks(grn_ctx *ctx, grn_obj *string);
GRN_API grn_rc grn_string_set_checks(grn_ctx *ctx,
                                     grn_obj *string,
                                     short *checks);
GRN_API const unsigned char *grn_string_get_types(grn_ctx *ctx, grn_obj *string);
GRN_API grn_rc grn_string_set_types(grn_ctx *ctx,
                                    grn_obj *string,
                                    unsigned char *types);
GRN_API grn_encoding grn_string_get_encoding(grn_ctx *ctx, grn_obj *string);


GRN_API int grn_charlen(grn_ctx *ctx, const char *str, const char *end);

GRN_API grn_rc grn_ctx_push(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_obj *grn_ctx_pop(grn_ctx *ctx);

GRN_API int grn_obj_columns(grn_ctx *ctx, grn_obj *table,
                            const char *str, unsigned int str_size, grn_obj *res);

GRN_API grn_rc grn_load(grn_ctx *ctx, grn_content_type input_type,
                        const char *table, unsigned int table_len,
                        const char *columns, unsigned int columns_len,
                        const char *values, unsigned int values_len,
                        const char *ifexists, unsigned int ifexists_len,
                        const char *each, unsigned int each_len);

#define GRN_CTX_MORE                    (0x01<<0)
#define GRN_CTX_TAIL                    (0x01<<1)
#define GRN_CTX_HEAD                    (0x01<<2)
#define GRN_CTX_QUIET                   (0x01<<3)
#define GRN_CTX_QUIT                    (0x01<<4)

GRN_API grn_rc grn_ctx_connect(grn_ctx *ctx, const char *host, int port, int flags);
GRN_API unsigned int grn_ctx_send(grn_ctx *ctx, const char *str, unsigned int str_len, int flags);
GRN_API unsigned int grn_ctx_recv(grn_ctx *ctx, char **str, unsigned int *str_len, int *flags);

typedef struct _grn_ctx_info grn_ctx_info;

struct _grn_ctx_info {
  int fd;
  unsigned int com_status;
  grn_obj *outbuf;
  unsigned char stat;
};

GRN_API grn_rc grn_ctx_info_get(grn_ctx *ctx, grn_ctx_info *info);

GRN_API grn_rc grn_set_segv_handler(void);
GRN_API grn_rc grn_set_int_handler(void);
GRN_API grn_rc grn_set_term_handler(void);


typedef struct _grn_table_delete_optarg grn_table_delete_optarg;

struct _grn_table_delete_optarg {
  int flags;
  int (*func)(grn_ctx *ctx, grn_obj *, grn_id, void *);
  void *func_arg;
};

struct _grn_table_scan_hit {
  grn_id id;
  unsigned int offset;
  unsigned int length;
};

typedef struct {
  int64_t tv_sec;
  int32_t tv_nsec;
} grn_timeval;

#ifdef __cplusplus
}
#endif
