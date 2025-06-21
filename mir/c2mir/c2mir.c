/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* C to MIR compiler.  It is a four pass compiler:
   o preprocessor pass generating tokens
   o parsing pass generating AST
   o context pass checking context constraints and augmenting AST
   o generation pass producing MIR

   The compiler implements C11 standard w/o C11 optional features:
   atomic, complex, variable size arrays. */

#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <setjmp.h>
#include <math.h>
#include <wchar.h>
#include "mir-alloc.h"
#include "mir.h"
#include "time.h"

#include "c2mir.h"

#if defined(__x86_64__) || defined(_M_AMD64)
#include "x86_64/cx86_64.h"
#elif defined(__aarch64__)
#include "aarch64/caarch64.h"
#elif defined(__PPC64__)
#include "ppc64/cppc64.h"
#elif defined(__s390x__)
#include "s390x/cs390x.h"
#elif defined(__riscv)
#include "riscv64/criscv64.h"
#else
#error "undefined or unsupported generation target for C"
#endif

#define SWAP(a1, a2, t) \
  do {                  \
    t = a1;             \
    a1 = a2;            \
    a2 = t;             \
  } while (0)

typedef enum {
  C_alloc_error,
  C_unfinished_comment,
  C_out_of_range_number,
  C_invalid_char_constant,
  C_no_string_end,
  C_invalid_str_constant,
  C_invalid_char,
} C_error_code_t;

DEF_VARR (char);

typedef struct pos {
  const char *fname;
  int lno, ln_pos;
} pos_t;

static const pos_t no_pos = {NULL, -1, -1};

typedef struct c2m_ctx *c2m_ctx_t;

typedef struct stream {
  FILE *f;                        /* the current file, NULL for top-level or string stream */
  const char *fname;              /* NULL only for preprocessor string stream */
  int (*getc_func) (c2m_ctx_t);   /* get function for top-level or string stream */
  VARR (char) * ln;               /* stream current line in reverse order */
  pos_t pos;                      /* includes file name used for reports */
  fpos_t fpos;                    /* file pos to resume file stream */
  const char *start, *curr;       /* non NULL only for string stream  */
  int ifs_length_at_stream_start; /* length of ifs at the stream start */
} *stream_t;

DEF_VARR (stream_t);

typedef const char *char_ptr_t;
DEF_VARR (char_ptr_t);

typedef void *void_ptr_t;
DEF_VARR (void_ptr_t);

typedef struct {
  const char *s;
  size_t len;
} str_t;

typedef struct {
  str_t str;
  size_t key, flags;
} tab_str_t;

DEF_HTAB (tab_str_t);

typedef struct token *token_t;
DEF_VARR (token_t);

typedef struct node *node_t;

enum symbol_mode { S_REGULAR, S_TAG, S_LABEL };

DEF_VARR (node_t);

typedef struct {
  enum symbol_mode mode;
  node_t id;
  node_t scope;
  node_t def_node, aux_node;
  VARR (node_t) * defs;
} symbol_t;

DEF_HTAB (symbol_t);

struct init_object {
  struct type *container_type;
  int field_designator_p;
  union {
    mir_llong curr_index;
    node_t curr_member;
  } u;
};

typedef struct init_object init_object_t;
DEF_VARR (init_object_t);

typedef struct pre_ctx *pre_ctx_t;
typedef struct parse_ctx *parse_ctx_t;
typedef struct check_ctx *check_ctx_t;
typedef struct gen_ctx *gen_ctx_t;

DEF_VARR (pos_t);

struct c2m_ctx {
  MIR_context_t ctx;
  struct c2mir_options *options;
  jmp_buf env; /* put it first as it might need 16-byte malloc allignment */
  VARR (char_ptr_t) * headers;
  VARR (char_ptr_t) * system_headers;
  const char **header_dirs, **system_header_dirs;
  void (*error_func) (c2m_ctx_t, C_error_code_t code, const char *message);
  VARR (void_ptr_t) * reg_memory;
  VARR (stream_t) * streams; /* stack of streams */
  stream_t cs, eof_s;        /* current stream and stream corresponding the last EOF */
  HTAB (tab_str_t) * str_tab;
  HTAB (tab_str_t) * str_key_tab;
  str_t empty_str;
  unsigned curr_uid;
  int (*c_getc) (void *); /* c2mir interface get function */
  void *c_getc_data;
  unsigned n_errors, n_warnings;
  VARR (char) * symbol_text, *temp_string;
  VARR (token_t) * recorded_tokens, *buffered_tokens;
  node_t top_scope;
  HTAB (symbol_t) * symbol_tab;
  VARR (pos_t) * node_positions;
  VARR (node_t) * call_nodes;
  VARR (node_t) * containing_anon_members;
  VARR (init_object_t) * init_object_path;
  char temp_str_buff[50];
  struct pre_ctx *pre_ctx;
  struct parse_ctx *parse_ctx;
  struct check_ctx *check_ctx;
  struct gen_ctx *gen_ctx;
};

typedef struct c2m_ctx *c2m_ctx_t;

#define c2m_options c2m_ctx->options
#define headers c2m_ctx->headers
#define system_headers c2m_ctx->system_headers
#define header_dirs c2m_ctx->header_dirs
#define system_header_dirs c2m_ctx->system_header_dirs
#define error_func c2m_ctx->error_func
#define reg_memory c2m_ctx->reg_memory
#define str_tab c2m_ctx->str_tab
#define streams c2m_ctx->streams
#define cs c2m_ctx->cs
#define eof_s c2m_ctx->eof_s
#define str_key_tab c2m_ctx->str_key_tab
#define empty_str c2m_ctx->empty_str
#define curr_uid c2m_ctx->curr_uid
#define c_getc c2m_ctx->c_getc
#define c_getc_data c2m_ctx->c_getc_data
#define n_errors c2m_ctx->n_errors
#define n_warnings c2m_ctx->n_warnings
#define symbol_text c2m_ctx->symbol_text
#define temp_string c2m_ctx->temp_string
#define recorded_tokens c2m_ctx->recorded_tokens
#define buffered_tokens c2m_ctx->buffered_tokens
#define top_scope c2m_ctx->top_scope
#define symbol_tab c2m_ctx->symbol_tab
#define node_positions c2m_ctx->node_positions
#define call_nodes c2m_ctx->call_nodes
#define containing_anon_members c2m_ctx->containing_anon_members
#define init_object_path c2m_ctx->init_object_path
#define temp_str_buff c2m_ctx->temp_str_buff

static inline c2m_ctx_t *c2m_ctx_loc (MIR_context_t ctx) {
  return (c2m_ctx_t *) ((void **) ctx + 1);
}

static void alloc_error (c2m_ctx_t c2m_ctx, const char *message) {
  error_func (c2m_ctx, C_alloc_error, message);
}

static const int max_nested_includes = 32;

#define MIR_VARR_ERROR alloc_error
#define MIR_HTAB_ERROR MIR_VARR_ERROR

#define FALSE 0
#define TRUE 1

#include "mir-varr.h"
#include "mir-dlist.h"
#include "mir-hash.h"
#include "mir-htab.h"

static mir_size_t round_size (mir_size_t size, mir_size_t round) {
  return (size + round - 1) / round * round;
}

/* Some abbreviations: */
#define NL_HEAD(list) DLIST_HEAD (node_t, list)
#define NL_TAIL(list) DLIST_TAIL (node_t, list)
#define NL_LENGTH(list) DLIST_LENGTH (node_t, list)
#define NL_NEXT(el) DLIST_NEXT (node_t, el)
#define NL_PREV(el) DLIST_PREV (node_t, el)
#define NL_REMOVE(list, el) DLIST_REMOVE (node_t, list, el)
#define NL_APPEND(list, el) DLIST_APPEND (node_t, list, el)
#define NL_PREPEND(list, el) DLIST_PREPEND (node_t, list, el)
#define NL_EL(list, n) DLIST_EL (node_t, list, n)

enum basic_type {
  TP_UNDEF,
  TP_VOID,
  /* Integer types: the first should be BOOL and the last should be
     ULLONG.  The order is important -- do not change it.  */
  TP_BOOL,
  TP_CHAR,
  TP_SCHAR,
  TP_UCHAR,
  TP_SHORT,
  TP_USHORT,
  TP_INT,
  TP_UINT,
  TP_LONG,
  TP_ULONG,
  TP_LLONG,
  TP_ULLONG,
  TP_FLOAT,
  TP_DOUBLE,
  TP_LDOUBLE,
};

struct type_qual {
  unsigned int const_p : 1, restrict_p : 1, volatile_p : 1, atomic_p : 1; /* Type qualifiers */
};

static const struct type_qual zero_type_qual = {0, 0, 0, 0};

struct arr_type {
  unsigned int static_p : 1;
  struct type *el_type;
  struct type_qual ind_type_qual;
  node_t size;
};

struct func_type {
  unsigned int dots_p : 1;
  struct type *ret_type;
  node_t param_list; /* w/o N_DOTS */
  MIR_item_t proto_item;
};

enum type_mode {
  TM_UNDEF,
  TM_BASIC,
  TM_ENUM,
  TM_PTR,
  TM_STRUCT,
  TM_UNION,
  TM_ARR,
  TM_FUNC,
};

struct type {
  node_t pos_node;       /* set up and used only for checking type correctness */
  struct type *arr_type; /* NULL or array type before its adjustment */
  MIR_alias_t antialias; /* it can be non-zero only for pointers */
  struct type_qual type_qual;
  enum type_mode mode;
  char func_type_before_adjustment_p;
  char unnamed_anon_struct_union_member_type_p;
  int align; /* type align, undefined if < 0  */
  /* Raw type size (w/o alignment type itself requirement but with
     element alignment requirements), undefined if mir_size_max.  */
  mir_size_t raw_size;
  union {
    enum basic_type basic_type; /* also integer type */
    node_t tag_type;            /* struct/union/enum */
    struct type *ptr_type;
    struct arr_type *arr_type;
    struct func_type *func_type;
  } u;
};

/*!*/ static struct type VOID_TYPE
  = {.raw_size = MIR_SIZE_MAX, .align = -1, .mode = TM_BASIC, .u = {.basic_type = TP_VOID}};

static void set_type_layout (c2m_ctx_t c2m_ctx, struct type *type);

static mir_size_t raw_type_size (c2m_ctx_t c2m_ctx, struct type *type) {
  if (type->raw_size == MIR_SIZE_MAX) set_type_layout (c2m_ctx, type);
  if (n_errors != 0 && type->raw_size == MIR_SIZE_MAX) {
    /* Use safe values for programs with errors: */
    type->raw_size = 0;
    type->align = 1;
  }
  assert (type->raw_size != MIR_SIZE_MAX);
  return type->raw_size;
}

typedef struct {
  const char *name, *content;
} string_include_t;

#if defined(__x86_64__) || defined(_M_AMD64)
#include "x86_64/cx86_64-code.c"
#elif defined(__aarch64__)
#include "aarch64/caarch64-code.c"
#elif defined(__PPC64__)
#include "ppc64/cppc64-code.c"
#elif defined(__s390x__)
#include "s390x/cs390x-code.c"
#elif defined(__riscv)
#include "riscv64/criscv64-code.c"
#else
#error "undefined or unsupported generation target for C"
#endif

static inline MIR_alloc_t c2m_alloc (c2m_ctx_t c2m_ctx) {
  return MIR_get_alloc (c2m_ctx->ctx);
}

static void *reg_malloc (c2m_ctx_t c2m_ctx, size_t s) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  void *mem = MIR_malloc (alloc, s);

  if (mem == NULL) alloc_error (c2m_ctx, "no memory");
  VARR_PUSH (void_ptr_t, reg_memory, mem);
  return mem;
}

static void reg_memory_pop (c2m_ctx_t c2m_ctx, size_t mark) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  while (VARR_LENGTH (void_ptr_t, reg_memory) > mark)
    MIR_free (alloc, VARR_POP (void_ptr_t, reg_memory));
}

static size_t MIR_UNUSED reg_memory_mark (c2m_ctx_t c2m_ctx) {
  return VARR_LENGTH (void_ptr_t, reg_memory);
}
static void reg_memory_finish (c2m_ctx_t c2m_ctx) {
  reg_memory_pop (c2m_ctx, 0);
  VARR_DESTROY (void_ptr_t, reg_memory);
}

static void reg_memory_init (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  VARR_CREATE (void_ptr_t, reg_memory, alloc, 4096);
}

static int char_is_signed_p (void) { return MIR_CHAR_MAX == MIR_SCHAR_MAX; }

enum str_flag { FLAG_EXT = 1, FLAG_C89, FLAG_EXT89 };

static int str_eq (tab_str_t str1, tab_str_t str2, void *arg MIR_UNUSED) {
  return str1.str.len == str2.str.len && memcmp (str1.str.s, str2.str.s, str1.str.len) == 0;
}
static htab_hash_t str_hash (tab_str_t str, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash (str.str.s, str.str.len, 0x42);
}
static int str_key_eq (tab_str_t str1, tab_str_t str2, void *arg MIR_UNUSED) {
  return str1.key == str2.key;
}
static htab_hash_t str_key_hash (tab_str_t str, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash64 (str.key, 0x24);
}

static str_t uniq_cstr (c2m_ctx_t c2m_ctx, const char *str);

static void str_init (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  HTAB_CREATE (tab_str_t, str_tab, alloc, 1000, str_hash, str_eq, NULL);
  HTAB_CREATE (tab_str_t, str_key_tab, alloc, 200, str_key_hash, str_key_eq, NULL);
  empty_str = uniq_cstr (c2m_ctx, "");
}

static int str_exists_p (c2m_ctx_t c2m_ctx, const char *s, size_t len, tab_str_t *tab_str) {
  tab_str_t el, str;

  str.str.s = s;
  str.str.len = len;
  if (!HTAB_DO (tab_str_t, str_tab, str, HTAB_FIND, el)) return FALSE;
  *tab_str = el;
  return TRUE;
}

static tab_str_t str_add (c2m_ctx_t c2m_ctx, const char *s, size_t len, size_t key, size_t flags,
                          int key_p) {
  char *heap_s;
  tab_str_t el, str;

  if (str_exists_p (c2m_ctx, s, len, &el)) return el;
  heap_s = reg_malloc (c2m_ctx, len);
  memcpy (heap_s, s, len);
  str.str.s = heap_s;
  str.str.len = len;
  str.key = key;
  str.flags = flags;
  HTAB_DO (tab_str_t, str_tab, str, HTAB_INSERT, el);
  if (key_p) HTAB_DO (tab_str_t, str_key_tab, str, HTAB_INSERT, el);
  return str;
}

static const char *str_find_by_key (c2m_ctx_t c2m_ctx, size_t key) {
  tab_str_t el, str;

  str.key = key;
  if (!HTAB_DO (tab_str_t, str_key_tab, str, HTAB_FIND, el)) return NULL;
  return el.str.s;
}

static void str_finish (c2m_ctx_t c2m_ctx) {
  HTAB_DESTROY (tab_str_t, str_tab);
  HTAB_DESTROY (tab_str_t, str_key_tab);
}

static void *c2mir_calloc (c2m_ctx_t c2m_ctx, size_t size) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  void *res = MIR_calloc (alloc, 1, size);

  if (res == NULL) (*MIR_get_error_func (c2m_ctx->ctx)) (MIR_alloc_error, "no memory");
  return res;
}

void c2mir_init (MIR_context_t ctx) {
  MIR_alloc_t alloc = MIR_get_alloc (ctx);
  struct c2m_ctx **c2m_ctx_ptr = c2m_ctx_loc (ctx), *c2m_ctx;

  *c2m_ctx_ptr = c2m_ctx = MIR_calloc (alloc, 1, sizeof (struct c2m_ctx));
  if (c2m_ctx == NULL) (*MIR_get_error_func (ctx)) (MIR_alloc_error, "no memory");

  c2m_ctx->ctx = ctx;
  reg_memory_init (c2m_ctx);
  str_init (c2m_ctx);
}

void c2mir_finish (MIR_context_t ctx) {
  struct c2m_ctx **c2m_ctx_ptr = c2m_ctx_loc (ctx), *c2m_ctx = *c2m_ctx_ptr;

  str_finish (c2m_ctx);
  reg_memory_finish (c2m_ctx);
  free (c2m_ctx);
  *c2m_ctx_ptr = NULL;
}

/* New Page */

/* ------------------------- Parser Start ------------------------------ */

/* Parser is manually written parser with back-tracing to keep original
   grammar close to C11 standard grammar as possible.  It has a
   rudimentary syntax error recovery based on stop symbols ';' and
   '}'.  The input is parse tokens and the output is the following AST
   nodes (the AST root is transl_unit):

const : N_I | N_L | N_LL | N_U | N_UL | N_ULL | N_F | N_D | N_LD
      | N_CH | N_CH16 | N_CH32 | N_STR | N_STR16 | N_STR32
expr : const | N_ID | N_LABEL_ADDR (N_ID) | N_ADD (expr)
     | N_SUB (expr) | N_ADD (expr, expr) | N_SUB (expr, expr)
     | N_MUL (expr, expr) | N_DIV (expr, expr) | N_MOD (expr, expr)
     | N_LSH (expr, expr) | N_RSH (expr, expr)
     | N_NOT (expr) | N_BITWISE_NOT (expr)
     | N_INC (expr) | N_DEC (expr) | N_POST_INC (expr)| N_POST_DEC (expr)
     | N_ALIGNOF (type_name?) | N_SIZEOF (type_name) | N_EXPR_SIZEOF (expr)
     | N_CAST (type_name, expr) | N_COMMA (expr, expr) | N_ANDAND (expr, expr)
     | N_OROR (expr, expr) | N_EQ (expr, expr) | N_NE (expr, expr)
     | N_LT (expr, expr) | N_LE (expr, expr) | N_GT (expr, expr) | N_GE (expr, expr)
     | N_AND (expr, expr) | N_OR (expr, expr) | N_XOR (expr, expr)
     | N_ASSIGN (expr, expr) | N_ADD_ASSIGN (expr, expr) | N_SUB_ASSIGN (expr, expr)
     | N_MUL_ASSIGN (expr, expr) | N_DIV_ASSIGN (expr, expr) | N_MOD_ASSIGN (expr, expr)
     | N_LSH_ASSIGN (expr, expr) | N_RSH_ASSIGN (expr, expr)
     | N_AND_ASSIGN (expr, expr) | N_OR_ASSIGN (expr, expr) | N_XOR_ASSIGN (expr, expr)
     | N_DEREF (expr) | | N_ADDR (expr) | N_IND (expr, expr) | N_FIELD (expr, N_ID)
     | N_DEREF_FIELD (expr, N_ID) | N_COND (expr, expr, expr)
     | N_COMPOUND_LITERAL (type_name, initializer) | N_CALL (expr, N_LIST:(expr)*)
     | N_GENERIC (expr, N_LIST:(N_GENERIC_ASSOC (type_name?, expr))+ )
     | N_STMTEXPR (compound_stmt)
label: N_CASE(expr) | N_CASE(expr,expr) | N_DEFAULT | N_LABEL(N_ID)
stmt: compound_stmt | N_IF(N_LIST:(label)*, expr, stmt, stmt?)
    | N_SWITCH(N_LIST:(label)*, expr, stmt) | (N_WHILE|N_DO) (N_LIST:(label)*, expr, stmt)
    | N_FOR(N_LIST:(label)*,(N_LIST: declaration+ | expr)?, expr?, expr?, stmt)
    | N_GOTO(N_LIST:(label)*, N_ID) | N_INDIRECT_GOTO(N_LIST:(label)*, expr)
    | (N_CONTINUE|N_BREAK) (N_LIST:(label)*)
    | N_RETURN(N_LIST:(label)*, expr?) | N_EXPR(N_LIST:(label)*, expr)
compound_stmt: N_BLOCK(N_LIST:(label)*, N_LIST:(declaration | stmt)*)
asm: N_ASM(N_STR | N_STR16 | N_STR32)
attr_arg: const | N_ID
attr: N_ATTR(N_ID, NLIST:(attr_arg)*)
attrs: N_LIST:(attrs)*
declaration: N_SPEC_DECL(N_SHARE(declaration_specs), declarator?, attrs?, asm?, initializer?)
           | st_assert
st_assert: N_ST_ASSERT(const_expr, N_STR | N_STR16 | N_STR32)
declaration_specs: N_LIST:(align_spec|sc_spec|type_qual|func_spec|type_spec|attr)*
align_spec: N_ALIGNAS(type_name|const_expr)
sc_spec: N_TYPEDEF|N_EXTERN|N_STATIC|N_AUTO|N_REGISTER|N_THREAD_LOCAL
type_qual: N_CONST|N_RESTRICT|N_VOLATILE|N_ATOMIC
func_spec: N_INLINE|N_NO_RETURN
type_spec: N_VOID|N_CHAR|N_SHORT|N_INT|N_LONG|N_FLOAT|N_DOUBLE|N_SIGNED|N_UNSIGNED|N_BOOL
         | (N_STRUCT|N_UNION) (N_ID?, struct_declaration_list?)
         | N_ENUM(N_ID?, N_LIST?: N_ENUM_COST(N_ID, const_expr?)*) | typedef_name
struct_declaration_list: N_LIST: struct_declaration*
struct_declaration: st_assert | N_MEMBER(N_SHARE(spec_qual_list), declarator?, attrs?, const_expr?)
spec_qual_list: N_LIST:(type_qual|type_spec)*
declarator: the same as direct declarator
direct_declarator: N_DECL(N_ID,
                          N_LIST:(N_POINTER(type_qual_list) | N_FUNC(id_list|parameter_list)
                                            | N_ARR(N_STATIC?, type_qual_list,
                                                    (assign_expr|N_STAR)?))*)
pointer: N_LIST: N_POINTER(type_qual_list)*
type_qual_list : N_LIST: type_qual*
parameter_type_list: N_LIST:(N_SPEC_DECL(declaration_specs, declarator, attrs?, ignore, ignore)
                             | N_TYPE(declaration_specs, abstract_declarator))+ [N_DOTS]
id_list: N_LIST: N_ID*
initializer: assign_expr | initialize_list
initializer_list: N_LIST: N_INIT(N_LIST:(const_expr | N_FIELD_ID (N_ID))* initializer)*
type_name: N_TYPE(spec_qual_list, abstract_declarator)
abstract_declarator: the same as abstract direct declarator
abstract_direct_declarator: N_DECL(ignore,
                                   N_LIST:(N_POINTER(type_qual_list) | N_FUNC(parameter_list)
                                           | N_ARR(N_STATIC?, type_qual_list,
                                                   (assign_expr|N_STAR)?))*)
typedef_name: N_ID
transl_unit: N_MODULE(N_LIST:(declaration
                              | N_FUNC_DEF(declaration_specs, declarator,
                                           N_LIST: declaration*, compound_stmt))*)

Here ? means it can be N_IGNORE, * means 0 or more elements in the list, + means 1 or more.

*/

#define REP_SEP ,
#define T_EL(t) T_##t
typedef enum {
  T_NUMBER = 256,
  REP8 (T_EL, CH, STR, ID, ASSIGN, DIVOP, ADDOP, SH, CMP),
  REP8 (T_EL, EQNE, ANDAND, OROR, INCDEC, ARROW, UNOP, DOTS, BOOL),
  REP8 (T_EL, COMPLEX, ALIGNOF, ALIGNAS, ATOMIC, GENERIC, NO_RETURN, STATIC_ASSERT, THREAD_LOCAL),
  REP8 (T_EL, THREAD, AUTO, BREAK, CASE, CHAR, CONST, CONTINUE, DEFAULT),
  REP8 (T_EL, DO, DOUBLE, ELSE, ENUM, EXTERN, FLOAT, FOR, GOTO),
  REP8 (T_EL, IF, INLINE, INT, LONG, REGISTER, RESTRICT, RETURN, SHORT),
  REP8 (T_EL, SIGNED, SIZEOF, STATIC, STRUCT, SWITCH, TYPEDEF, TYPEOF, UNION),
  REP5 (T_EL, UNSIGNED, VOID, VOLATILE, WHILE, EOFILE),
  /* tokens existing in preprocessor only: */
  T_HEADER,         /* include header */
  T_NO_MACRO_IDENT, /* ??? */
  T_DBLNO,          /* ## */
  T_PLM,
  T_RDBLNO, /* placemarker, ## in replacement list */
  T_BOA,    /* begin of argument */
  T_EOA,
  T_EOR, /* end of argument and macro replacement */
  T_EOP, /* end of processing */
  T_EOU, /* end of translation unit */
} token_code_t;

static token_code_t FIRST_KW = T_BOOL, LAST_KW = T_WHILE;

#define NODE_EL(n) N_##n

typedef enum {
  REP8 (NODE_EL, IGNORE, I, L, LL, U, UL, ULL, F),
  REP8 (NODE_EL, D, LD, CH, CH16, CH32, STR, STR16, STR32),
  REP5 (NODE_EL, ID, COMMA, ANDAND, OROR, STMTEXPR),
  REP8 (NODE_EL, EQ, NE, LT, LE, GT, GE, ASSIGN, BITWISE_NOT),
  REP8 (NODE_EL, NOT, AND, AND_ASSIGN, OR, OR_ASSIGN, XOR, XOR_ASSIGN, LSH),
  REP8 (NODE_EL, LSH_ASSIGN, RSH, RSH_ASSIGN, ADD, ADD_ASSIGN, SUB, SUB_ASSIGN, MUL),
  REP8 (NODE_EL, MUL_ASSIGN, DIV, DIV_ASSIGN, MOD, MOD_ASSIGN, IND, FIELD, ADDR),
  REP8 (NODE_EL, DEREF, DEREF_FIELD, COND, INC, DEC, POST_INC, POST_DEC, ALIGNOF),
  REP8 (NODE_EL, SIZEOF, EXPR_SIZEOF, CAST, COMPOUND_LITERAL, CALL, GENERIC, GENERIC_ASSOC, IF),
  REP8 (NODE_EL, SWITCH, WHILE, DO, FOR, GOTO, INDIRECT_GOTO, CONTINUE, BREAK),
  REP8 (NODE_EL, RETURN, EXPR, BLOCK, CASE, DEFAULT, LABEL, LABEL_ADDR, LIST),
  REP8 (NODE_EL, SPEC_DECL, SHARE, TYPEDEF, EXTERN, STATIC, AUTO, REGISTER, THREAD_LOCAL),
  REP8 (NODE_EL, DECL, VOID, CHAR, SHORT, INT, LONG, FLOAT, DOUBLE),
  REP8 (NODE_EL, SIGNED, UNSIGNED, BOOL, STRUCT, UNION, ENUM, ENUM_CONST, MEMBER),
  REP8 (NODE_EL, CONST, RESTRICT, VOLATILE, ATOMIC, INLINE, NO_RETURN, ALIGNAS, FUNC),
  REP8 (NODE_EL, STAR, POINTER, DOTS, ARR, INIT, FIELD_ID, TYPE, ST_ASSERT),
  REP4 (NODE_EL, FUNC_DEF, MODULE, ASM, ATTR),
} node_code_t;

#undef REP_SEP

DEF_DLIST_LINK (node_t);
DEF_DLIST_TYPE (node_t);

struct node {
  node_code_t code;
  unsigned uid;
  void *attr; /* used a scope for parser and as an attribute after */
  DLIST_LINK (node_t) op_link;
  union {
    str_t s;
    mir_char ch;
    mir_long l;
    mir_llong ll;
    mir_ulong ul; /* includes CH16 and CH32 */
    mir_ullong ull;
    mir_float f;
    mir_double d;
    mir_ldouble ld;
    DLIST (node_t) ops;
  } u;
};

static pos_t get_node_pos (c2m_ctx_t c2m_ctx, node_t n) {
  return VARR_GET (pos_t, node_positions, n->uid);
}

#define POS(n) get_node_pos (c2m_ctx, n)

static void set_node_pos (c2m_ctx_t c2m_ctx, node_t n, pos_t pos) {
  while (n->uid >= VARR_LENGTH (pos_t, node_positions)) VARR_PUSH (pos_t, node_positions, no_pos);
  VARR_SET (pos_t, node_positions, n->uid, pos);
}

DEF_DLIST_CODE (node_t, op_link);

struct token {
  int code : 16; /* token_code_t and EOF */
  int processed_p : 16;
  pos_t pos;
  node_code_t node_code;
  node_t node;
  const char *repr;
};

static node_t add_pos (c2m_ctx_t c2m_ctx, node_t n, pos_t p) {
  if (POS (n).lno < 0) set_node_pos (c2m_ctx, n, p);
  return n;
}

static node_t op_append (c2m_ctx_t c2m_ctx, node_t n, node_t op) {
  NL_APPEND (n->u.ops, op);
  return add_pos (c2m_ctx, n, POS (op));
}

static node_t op_prepend (c2m_ctx_t c2m_ctx, node_t n, node_t op) {
  NL_PREPEND (n->u.ops, op);
  return add_pos (c2m_ctx, n, POS (op));
}

static void op_flat_append (c2m_ctx_t c2m_ctx, node_t n, node_t op) {
  if (op->code != N_LIST) {
    op_append (c2m_ctx, n, op);
    return;
  }
  for (node_t next_el, el = NL_HEAD (op->u.ops); el != NULL; el = next_el) {
    next_el = NL_NEXT (el);
    NL_REMOVE (op->u.ops, el);
    op_append (c2m_ctx, n, el);
  }
}

static node_t new_node (c2m_ctx_t c2m_ctx, node_code_t nc) {
  node_t n = reg_malloc (c2m_ctx, sizeof (struct node));

  n->code = nc;
  n->uid = curr_uid++;
  DLIST_INIT (node_t, n->u.ops);
  n->attr = NULL;
  set_node_pos (c2m_ctx, n, no_pos);
  return n;
}

static node_t copy_node_with_pos (c2m_ctx_t c2m_ctx, node_t n, pos_t pos) {
  node_t r = new_node (c2m_ctx, n->code);

  set_node_pos (c2m_ctx, r, pos);
  r->u = n->u;
  return r;
}

static node_t copy_node (c2m_ctx_t c2m_ctx, node_t n) {
  return copy_node_with_pos (c2m_ctx, n, POS (n));
}

static node_t new_pos_node (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p) {
  return add_pos (c2m_ctx, new_node (c2m_ctx, nc), p);
}
static node_t new_node1 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1) {
  return op_append (c2m_ctx, new_node (c2m_ctx, nc), op1);
}
static node_t new_pos_node1 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1) {
  return add_pos (c2m_ctx, new_node1 (c2m_ctx, nc, op1), p);
}
static node_t new_node2 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1, node_t op2) {
  return op_append (c2m_ctx, new_node1 (c2m_ctx, nc, op1), op2);
}
static node_t new_pos_node2 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1, node_t op2) {
  return add_pos (c2m_ctx, new_node2 (c2m_ctx, nc, op1, op2), p);
}
static node_t new_node3 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1, node_t op2, node_t op3) {
  return op_append (c2m_ctx, new_node2 (c2m_ctx, nc, op1, op2), op3);
}
static node_t new_pos_node3 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1, node_t op2,
                             node_t op3) {
  return add_pos (c2m_ctx, new_node3 (c2m_ctx, nc, op1, op2, op3), p);
}
static node_t new_node4 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1, node_t op2, node_t op3,
                         node_t op4) {
  return op_append (c2m_ctx, new_node3 (c2m_ctx, nc, op1, op2, op3), op4);
}
static node_t new_pos_node4 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1, node_t op2,
                             node_t op3, node_t op4) {
  return add_pos (c2m_ctx, new_node4 (c2m_ctx, nc, op1, op2, op3, op4), p);
}
static node_t new_node5 (c2m_ctx_t c2m_ctx, node_code_t nc, node_t op1, node_t op2, node_t op3,
                         node_t op4, node_t op5) {
  return op_append (c2m_ctx, new_node4 (c2m_ctx, nc, op1, op2, op3, op4), op5);
}
static node_t new_pos_node5 (c2m_ctx_t c2m_ctx, node_code_t nc, pos_t p, node_t op1, node_t op2,
                             node_t op3, node_t op4, node_t op5) {
  return add_pos (c2m_ctx, new_node5 (c2m_ctx, nc, op1, op2, op3, op4, op5), p);
}
static node_t new_ch_node (c2m_ctx_t c2m_ctx, int ch, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_CH, p);
  n->u.ch = ch;
  return n;
}
static node_t new_ch16_node (c2m_ctx_t c2m_ctx, mir_ulong ch, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_CH16, p);
  n->u.ul = ch;
  return n;
}
static node_t new_ch32_node (c2m_ctx_t c2m_ctx, mir_ulong ch, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_CH32, p);
  n->u.ul = ch;
  return n;
}
static node_t new_i_node (c2m_ctx_t c2m_ctx, long l, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_I, p);
  n->u.l = l;
  return n;
}
static node_t new_l_node (c2m_ctx_t c2m_ctx, long l, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_L, p);
  n->u.l = l;
  return n;
}
static node_t new_ll_node (c2m_ctx_t c2m_ctx, long long ll, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_LL, p);
  n->u.ll = ll;
  return n;
}
static node_t new_u_node (c2m_ctx_t c2m_ctx, unsigned long ul, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_U, p);
  n->u.ul = ul;
  return n;
}
static node_t new_ul_node (c2m_ctx_t c2m_ctx, unsigned long ul, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_UL, p);
  n->u.ul = ul;
  return n;
}
static node_t new_ull_node (c2m_ctx_t c2m_ctx, unsigned long long ull, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_ULL, p);
  n->u.ull = ull;
  return n;
}
static node_t new_f_node (c2m_ctx_t c2m_ctx, float f, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_F, p);
  n->u.f = f;
  return n;
}
static node_t new_d_node (c2m_ctx_t c2m_ctx, double d, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_D, p);
  n->u.d = d;
  return n;
}
static node_t new_ld_node (c2m_ctx_t c2m_ctx, long double ld, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, N_LD, p);
  n->u.ld = ld;
  return n;
}
static node_t new_str_node (c2m_ctx_t c2m_ctx, node_code_t nc, str_t s, pos_t p) {
  node_t n = new_pos_node (c2m_ctx, nc, p);
  n->u.s = s;
  return n;
}

static node_t get_op (node_t n, int nop) {
  n = NL_HEAD (n->u.ops);
  for (; nop > 0; nop--) n = NL_NEXT (n);
  return n;
}

static str_t uniq_cstr (c2m_ctx_t c2m_ctx, const char *str) {
  return str_add (c2m_ctx, str, strlen (str) + 1, T_STR, 0, FALSE).str;
}
static str_t uniq_str (c2m_ctx_t c2m_ctx, const char *str, size_t len) {
  return str_add (c2m_ctx, str, len, T_STR, 0, FALSE).str;
}

static token_t new_token (c2m_ctx_t c2m_ctx, pos_t pos, const char *repr, int token_code,
                          node_code_t node_code) {
  token_t token = reg_malloc (c2m_ctx, sizeof (struct token));

  token->code = token_code;
  token->processed_p = FALSE;
  token->pos = pos;
  token->repr = repr;
  token->node_code = node_code;
  token->node = NULL;
  return token;
}

static token_t copy_token (c2m_ctx_t c2m_ctx, token_t t, pos_t pos) {
  token_t token = new_token (c2m_ctx, pos, t->repr, t->code, t->node_code);

  if (t->node != NULL) token->node = copy_node_with_pos (c2m_ctx, t->node, pos);
  return token;
}

static token_t new_token_wo_uniq_repr (c2m_ctx_t c2m_ctx, pos_t pos, const char *repr,
                                       int token_code, node_code_t node_code) {
  return new_token (c2m_ctx, pos, uniq_cstr (c2m_ctx, repr).s, token_code, node_code);
}

static token_t new_node_token (c2m_ctx_t c2m_ctx, pos_t pos, const char *repr, int token_code,
                               node_t node) {
  token_t token = new_token_wo_uniq_repr (c2m_ctx, pos, repr, token_code, N_IGNORE);

  token->node = node;
  return token;
}

static void print_pos (FILE *f, pos_t pos, int col_p) {
  if (pos.lno < 0) return;
  fprintf (f, "%s:%d", pos.fname, pos.lno);
  if (col_p) fprintf (f, ":%d: ", pos.ln_pos);
}

static const char *get_token_name (c2m_ctx_t c2m_ctx, int token_code) {
  const char *s;

  switch (token_code) {
  case T_NUMBER: return "number";
  case T_CH: return "char constant";
  case T_STR: return "string";
  case T_ID: return "identifier";
  case T_ASSIGN: return "assign op";
  case T_DIVOP: return "/ or %";
  case T_ADDOP: return "+ or -";
  case T_SH: return "shift op";
  case T_CMP: return "comparison op";
  case T_EQNE: return "equality op";
  case T_ANDAND: return "&&";
  case T_OROR: return "||";
  case T_INCDEC: return "++ or --";
  case T_ARROW: return "->";
  case T_UNOP: return "unary op";
  case T_DOTS: return "...";
  default:
    if ((s = str_find_by_key (c2m_ctx, token_code)) != NULL) return s;
    if (isprint (token_code))
      sprintf (temp_str_buff, "%c", token_code);
    else
      sprintf (temp_str_buff, "%d", token_code);
    return temp_str_buff;
  }
}

static void error (c2m_ctx_t c2m_ctx, pos_t pos, const char *format, ...) {
  va_list args;
  FILE *f;

  if ((f = c2m_options->message_file) == NULL) return;
  n_errors++;
  va_start (args, format);
  print_pos (f, pos, TRUE);
  vfprintf (f, format, args);
  va_end (args);
  fprintf (f, "\n");
}

static void warning (c2m_ctx_t c2m_ctx, pos_t pos, const char *format, ...) {
  va_list args;
  FILE *f;

  if ((f = c2m_options->message_file) == NULL) return;
  n_warnings++;
  if (!c2m_options->ignore_warnings_p) {
    va_start (args, format);
    print_pos (f, pos, TRUE);
    fprintf (f, "warning -- ");
    vfprintf (f, format, args);
    va_end (args);
    fprintf (f, "\n");
  }
}

#define TAB_STOP 8

static void init_streams (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  cs = eof_s = NULL;
  VARR_CREATE (stream_t, streams, alloc, 32);
}

static void free_stream (stream_t s) {
  VARR_DESTROY (char, s->ln);
  free (s);
}

static void finish_streams (c2m_ctx_t c2m_ctx) {
  if (eof_s != NULL) free_stream (eof_s);
  if (streams == NULL) return;
  while (VARR_LENGTH (stream_t, streams) != 0) free_stream (VARR_POP (stream_t, streams));
  VARR_DESTROY (stream_t, streams);
}

static stream_t new_stream (MIR_alloc_t alloc, FILE *f, const char *fname, int (*getc_func) (c2m_ctx_t)) {
  stream_t s = MIR_malloc (alloc, sizeof (struct stream));

  VARR_CREATE (char, s->ln, alloc, 128);
  s->f = f;
  s->fname = s->pos.fname = fname;
  s->pos.lno = 0;
  s->pos.ln_pos = 0;
  s->ifs_length_at_stream_start = 0;
  s->start = s->curr = NULL;
  s->getc_func = getc_func;
  return s;
}

static void add_stream (c2m_ctx_t c2m_ctx, FILE *f, const char *fname,
                        int (*getc_func) (c2m_ctx_t)) {
  assert (fname != NULL);
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  if (cs != NULL && cs->f != NULL && cs->f != stdin) {
    fgetpos (cs->f, &cs->fpos);
    fclose (cs->f);
    cs->f = NULL;
  }
  cs = new_stream (alloc, f, fname, getc_func);
  VARR_PUSH (stream_t, streams, cs);
}

static int str_getc (c2m_ctx_t c2m_ctx) {
  if (*cs->curr == '\0') return EOF;
  return *cs->curr++;
}

static void add_string_stream (c2m_ctx_t c2m_ctx, const char *pos_fname, const char *str) {
  add_stream (c2m_ctx, NULL, pos_fname, str_getc);
  cs->start = cs->curr = str;
}

static int string_stream_p (stream_t s) { return s->getc_func != NULL; }

static void change_stream_pos (c2m_ctx_t c2m_ctx, pos_t pos) { cs->pos = pos; }

static void remove_trigraphs (c2m_ctx_t c2m_ctx) {
  int len = (int) VARR_LENGTH (char, cs->ln);
  char *addr = VARR_ADDR (char, cs->ln);
  int i, start, to, ch;

  for (i = to = 0; i < len; i++, to++) {
    addr[to] = addr[i];
    for (start = i; i < len && addr[i] == '?'; i++, to++) addr[to] = addr[i];
    if (i >= len) break;
    if (i < start + 2) {
      addr[to] = addr[i];
      continue;
    }
    switch (addr[i]) {
    case '=': ch = '#'; break;
    case '(': ch = '['; break;
    case '/': ch = '\\'; break;
    case ')': ch = ']'; break;
    case '\'': ch = '^'; break;
    case '<': ch = '{'; break;
    case '!': ch = '|'; break;
    case '>': ch = '}'; break;
    case '-': ch = '~'; break;
    default: addr[to] = addr[i]; continue;
    }
    to -= 2;
    addr[to] = ch;
  }
  VARR_TRUNC (char, cs->ln, to);
}

static int ln_get (c2m_ctx_t c2m_ctx) {
  if (cs->f == NULL) return cs->getc_func (c2m_ctx); /* top level */
  return fgetc (cs->f);
}

static char *reverse (VARR (char) * v) {
  char *addr = VARR_ADDR (char, v);
  int i, j, temp, last = (int) VARR_LENGTH (char, v) - 1;

  if (last >= 0 && addr[last] == '\0') last--;
  for (i = last, j = 0; i > j; i--, j++) SWAP (addr[i], addr[j], temp);
  return addr;
}

static int get_line (c2m_ctx_t c2m_ctx) { /* translation phase 1 and 2 */
  int c, eof_p = 0;

  VARR_TRUNC (char, cs->ln, 0);
  for (c = ln_get (c2m_ctx); c != EOF && c != '\n'; c = ln_get (c2m_ctx))
    VARR_PUSH (char, cs->ln, c);
  if (VARR_LENGTH (char, cs->ln) != 0 && VARR_LAST (char, cs->ln) == '\r') VARR_POP (char, cs->ln);
  eof_p = c == EOF;
  if (eof_p) {
    if (VARR_LENGTH (char, cs->ln) == 0) return FALSE;
    if (c != '\n')
      (c2m_options->pedantic_p ? error : warning) (c2m_ctx, cs->pos, "no end of line at file end");
  }
  remove_trigraphs (c2m_ctx);
  VARR_PUSH (char, cs->ln, '\n');
  reverse (cs->ln);
  return TRUE;
}

static int cs_get (c2m_ctx_t c2m_ctx) {
  size_t len = VARR_LENGTH (char, cs->ln);

  for (;;) {
    if (len == 2 && VARR_GET (char, cs->ln, 1) == '\\') {
      assert (VARR_GET (char, cs->ln, 0) == '\n');
    } else if (len > 0) {
      cs->pos.ln_pos++;
      return VARR_POP (char, cs->ln);
    }
    if (cs->fname == NULL || !get_line (c2m_ctx)) return EOF;
    len = VARR_LENGTH (char, cs->ln);
    assert (len > 0);
    cs->pos.ln_pos = 0;
    cs->pos.lno++;
  }
}

static void cs_unget (c2m_ctx_t c2m_ctx, int c) {
  cs->pos.ln_pos--;
  VARR_PUSH (char, cs->ln, c);
}

static void set_string_stream (c2m_ctx_t c2m_ctx, const char *str, pos_t pos,
                               void (*transform) (const char *, VARR (char) *)) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  /* read from string str */
  cs = new_stream (alloc, NULL, NULL, NULL);
  VARR_PUSH (stream_t, streams, cs);
  cs->pos = pos;
  if (transform != NULL) {
    transform (str, cs->ln);
  } else {
    for (; *str != '\0'; str++) VARR_PUSH (char, cs->ln, *str);
  }
}

static void remove_string_stream (c2m_ctx_t c2m_ctx) {
  assert (cs->f == NULL && cs->f == NULL);
  free_stream (VARR_POP (stream_t, streams));
  cs = VARR_LAST (stream_t, streams);
}

#define MAX_UTF8 0x1FFFFF

/* We use UTF-32 for 32-bit wchars and UTF-16 for 16-bit wchar (LE/BE
   depending on endianess of the target), UTF-8 for anything else. */
static void push_str_char (VARR (char) * temp, uint64_t ch, int type) {
  int i, len = 0;

  switch (type) {
  case ' ':
    if (ch <= 0xFF) {
      VARR_PUSH (char, temp, (char) ch);
      return;
    }
    /* Fall through */
  case '8':
    if (ch <= 0x7F) {
      VARR_PUSH (char, temp, (char) ch);
    } else if (ch <= 0x7FF) {
      VARR_PUSH (char, temp, (char) (0xC0 | (ch >> 6)));
      VARR_PUSH (char, temp, (char) (0x80 | (ch & 0x3F)));
    } else if (ch <= 0xFFFF) {
      VARR_PUSH (char, temp, (char) (0xE0 | (ch >> 12)));
      VARR_PUSH (char, temp, (char) (0x80 | ((ch >> 6) & 0x3F)));
      VARR_PUSH (char, temp, (char) (0x80 | (ch & 0x3F)));
    } else {
      assert (ch <= MAX_UTF8);
      VARR_PUSH (char, temp, (char) (0xF0 | (ch >> 18)));
      VARR_PUSH (char, temp, (char) (0x80 | ((ch >> 12) & 0x3F)));
      VARR_PUSH (char, temp, (char) (0x80 | ((ch >> 6) & 0x3F)));
      VARR_PUSH (char, temp, (char) (0x80 | (ch & 0x3F)));
    }
    return;
  case 'L':
    if (sizeof (mir_wchar) == 4) goto U;
    /* Fall through */
  case 'u': len = 2; break;
  case 'U':
  U:
    len = 4;
    break;
  default: assert (FALSE);
  }
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  for (i = 0; i < len; i++) VARR_PUSH (char, temp, (ch >> i * 8) & 0xff);
#else
  for (i = len - 1; i >= 0; i--) VARR_PUSH (char, temp, (ch >> i * 8) & 0xff);
#endif
}

static int pre_skip_if_part_p (c2m_ctx_t c2m_ctx);

static void set_string_val (c2m_ctx_t c2m_ctx, token_t t, VARR (char) * temp, int type) {
  int i, str_len;
  int64_t curr_c, last_c = -1;
  uint64_t max_char = (type == 'u'   ? UINT16_MAX
                       : type == 'U' ? UINT32_MAX
                       : type == 'L' ? MIR_WCHAR_MAX
                                     : MIR_UCHAR_MAX);
  int start = type == ' ' ? 0 : type == '8' ? 2 : 1;
  int string_p = t->repr[start] == '"';
  const char *str;

  assert (t->code == T_STR || t->code == T_CH);
  str = t->repr;
  VARR_TRUNC (char, temp, 0);
  str_len = (int) strlen (str);
  assert (str_len >= start + 2 && (str[start] == '"' || str[start] == '\'')
          && str[start] == str[str_len - 1]);
  for (i = start + 1; i < str_len - 1; i++) {
    if (!string_p && last_c >= 0 && !pre_skip_if_part_p (c2m_ctx))
      error (c2m_ctx, t->pos, "multibyte character");
    last_c = curr_c = (unsigned char) str[i];
    if (curr_c != '\\') {
      push_str_char (temp, curr_c, type);
      continue;
    }
    last_c = curr_c = str[++i];
    switch (curr_c) {
    case 'a': last_c = curr_c = '\a'; break;
    case 'b': last_c = curr_c = '\b'; break;
    case 'n': last_c = curr_c = '\n'; break;
    case 'f': last_c = curr_c = '\f'; break;
    case 'r': last_c = curr_c = '\r'; break;
    case 't': last_c = curr_c = '\t'; break;
    case 'v': last_c = curr_c = '\v'; break;
    case '\\':
    case '\'':
    case '\?':
    case '\"': break;
    case 'e':
      if (!pre_skip_if_part_p (c2m_ctx))
        (c2m_options->pedantic_p ? error : warning) (c2m_ctx, t->pos,
                                                     "non-standard escape sequence \\e");
      last_c = curr_c = '\033';
      break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7': {
      uint64_t v = curr_c - '0';

      curr_c = str[++i];
      if (!isdigit ((int) curr_c) || curr_c == '8' || curr_c == '9') {
        i--;
      } else {
        v = v * 8 + curr_c - '0';
        curr_c = str[++i];
        if (!isdigit ((int) curr_c) || curr_c == '8' || curr_c == '9')
          i--;
        else
          v = v * 8 + curr_c - '0';
      }
      last_c = curr_c = v;
      break;
    }
    case 'x':
    case 'X': {
      int first_p = TRUE;
      uint64_t v = 0;

      for (i++;; i++) {
        curr_c = str[i];
        if (!isxdigit ((int) curr_c)) break;
        first_p = FALSE;
        if (v <= UINT32_MAX) {
          v *= 16;
          v += (isdigit ((int) curr_c)   ? curr_c - '0'
                : islower ((int) curr_c) ? curr_c - 'a' + 10
                                         : curr_c - 'A' + 10);
        }
      }
      if (first_p) {
        if (!pre_skip_if_part_p (c2m_ctx))
          error (c2m_ctx, t->pos, "wrong hexadecimal char %c", curr_c);
      } else if (v > max_char) {
        if (!pre_skip_if_part_p (c2m_ctx))
          (c2m_options->pedantic_p ? error : warning) (c2m_ctx, t->pos,
                                                       "too big hexadecimal char 0x%x", v);
        curr_c = max_char;
      }
      last_c = curr_c = v;
      i--;
      break;
    }
    case 'u':
    case 'U': {
      int n, start_c = (int) curr_c, digits_num = curr_c == 'u' ? 4 : 8;
      uint64_t v = 0;

      for (i++, n = 0; n < digits_num; i++, n++) {
        curr_c = str[i];
        if (!isxdigit ((int) curr_c)) break;
        v *= 16;
        v += (isdigit ((int) curr_c)   ? curr_c - '0'
              : islower ((int) curr_c) ? curr_c - 'a' + 10
                                       : curr_c - 'A' + 10);
      }
      last_c = curr_c = v;
      if (n < digits_num) {
        if (!pre_skip_if_part_p (c2m_ctx))
          error (c2m_ctx, t->pos, "unfinished \\%c<hex-digits>", start_c);
      } else if (v > max_char && (!string_p || (type != ' ' && type != '8') || v > MAX_UTF8)) {
        if (!pre_skip_if_part_p (c2m_ctx))
          (c2m_options->pedantic_p ? error : warning) (c2m_ctx, t->pos,
                                                       "too big universal char 0x%lx in \\%c",
                                                       (unsigned long) v, start_c);
        last_c = curr_c = max_char;
      } else if ((0xD800 <= v && v <= 0xDFFF)
                 || (v < 0xA0 && v != 0x24 && v != 0x40 && v != 0x60)) {
        if (!pre_skip_if_part_p (c2m_ctx)) {
          error (c2m_ctx, t->pos, "usage of reserved value 0x%lx in \\%c", (unsigned long) v,
                 start_c);
          curr_c = -1;
        }
      }
      if (n < digits_num) i--;
      break;
    }
    default:
      if (!pre_skip_if_part_p (c2m_ctx)) {
        error (c2m_ctx, t->pos, "wrong escape char 0x%x", curr_c);
        curr_c = -1;
      }
    }
    if (!string_p || curr_c >= 0) push_str_char (temp, curr_c, type);
  }
  push_str_char (temp, '\0', type);
  if (string_p)
    t->node->u.s = uniq_str (c2m_ctx, VARR_ADDR (char, temp), VARR_LENGTH (char, temp));
  else if (last_c < 0) {
    if (!pre_skip_if_part_p (c2m_ctx)) error (c2m_ctx, t->pos, "empty char constant");
  } else if (type == 'U' || type == 'u' || type == 'L') {
    t->node->u.ul = (mir_ulong) last_c;
  } else {
    t->node->u.ch = (mir_char) last_c;
  }
}

static token_t new_id_token (c2m_ctx_t c2m_ctx, pos_t pos, const char *id_str) {
  token_t token;
  str_t str = uniq_cstr (c2m_ctx, id_str);

  token = new_token (c2m_ctx, pos, str.s, T_ID, N_IGNORE);
  token->node = new_str_node (c2m_ctx, N_ID, str, pos);
  return token;
}

static token_t get_next_pptoken_1 (c2m_ctx_t c2m_ctx, int header_p) {
  int start_c, curr_c, nl_p, comment_char, wide_type;
  pos_t pos;

  if (cs->fname != NULL && VARR_LENGTH (token_t, buffered_tokens) != 0)
    return VARR_POP (token_t, buffered_tokens);
  VARR_TRUNC (char, symbol_text, 0);
  for (;;) {
    curr_c = cs_get (c2m_ctx);
    /* Process sequence of white spaces/comments: */
    for (comment_char = -1, nl_p = FALSE;; curr_c = cs_get (c2m_ctx)) {
      switch (curr_c) {
      case '\t':
        cs->pos.ln_pos = (int) round_size ((mir_size_t) cs->pos.ln_pos, TAB_STOP);
        /* fall through */
      case ' ':
      case '\f':
      case '\r':
      case '\v': break;
      case '\n':
        if (comment_char < 0) {
          nl_p = TRUE;
          pos = cs->pos;
        } else if (comment_char == '/') {
          comment_char = -1;
          nl_p = TRUE;
          pos = cs->pos;
        }
        cs->pos.ln_pos = 0;
        break;
      case '/':
        if (comment_char >= 0) break;
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '/' || curr_c == '*') {
          VARR_PUSH (char, symbol_text, '/');
          comment_char = curr_c;
          break;
        }
        cs_unget (c2m_ctx, curr_c);
        curr_c = '/';
        goto end_ws;
      case '*':
        if (comment_char < 0) goto end_ws;
        if (comment_char != '*') break;
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '/') {
          comment_char = -1;
          VARR_PUSH (char, symbol_text, '*');
        } else {
          cs_unget (c2m_ctx, curr_c);
          curr_c = '*';
        }
        break;
      default:
        if (comment_char < 0) goto end_ws;
        if (curr_c == EOF) {
          error_func (c2m_ctx, C_unfinished_comment, "unfinished comment");
          goto end_ws;
        }
        break;
      }
      VARR_PUSH (char, symbol_text, curr_c);
    }
  end_ws:
    if (VARR_LENGTH (char, symbol_text) != 0) {
      cs_unget (c2m_ctx, curr_c);
      VARR_PUSH (char, symbol_text, '\0');
      return new_token_wo_uniq_repr (c2m_ctx, nl_p ? pos : cs->pos, VARR_ADDR (char, symbol_text),
                                     nl_p ? '\n' : ' ', N_IGNORE);
    }
    if (header_p && (curr_c == '<' || curr_c == '\"')) {
      int stop;

      pos = cs->pos;
      VARR_TRUNC (char, temp_string, 0);
      for (stop = curr_c == '<' ? '>' : '\"';;) {
        VARR_PUSH (char, symbol_text, curr_c);
        curr_c = cs_get (c2m_ctx);
        VARR_PUSH (char, temp_string, curr_c);
        if (curr_c == stop || curr_c == '\n' || curr_c == EOF) break;
      }
      if (curr_c == stop) {
        VARR_PUSH (char, symbol_text, curr_c);
        VARR_PUSH (char, symbol_text, '\0');
        VARR_POP (char, temp_string);
        VARR_PUSH (char, temp_string, '\0');
        return new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_HEADER,
                               new_str_node (c2m_ctx, N_STR,
                                             uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)),
                                             pos));
      } else {
        VARR_PUSH (char, symbol_text, curr_c);
        for (size_t i = 0; i < VARR_LENGTH (char, symbol_text); i++)
          cs_unget (c2m_ctx, VARR_GET (char, symbol_text, i));
        curr_c = (stop == '>' ? '<' : '\"');
      }
    }
    switch (start_c = curr_c) {
    case '\\':
      curr_c = cs_get (c2m_ctx);
      assert (curr_c != '\n');
      cs_unget (c2m_ctx, curr_c);
      return new_token (c2m_ctx, cs->pos, "\\", '\\', N_IGNORE);
    case '~': return new_token (c2m_ctx, cs->pos, "~", T_UNOP, N_BITWISE_NOT);
    case '+':
    case '-':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == start_c) {
        if (start_c == '+')
          return new_token (c2m_ctx, pos, "++", T_INCDEC, N_INC);
        else
          return new_token (c2m_ctx, pos, "--", T_INCDEC, N_DEC);
      } else if (curr_c == '=') {
        if (start_c == '+')
          return new_token (c2m_ctx, pos, "+=", T_ASSIGN, N_ADD_ASSIGN);
        else
          return new_token (c2m_ctx, pos, "-=", T_ASSIGN, N_SUB_ASSIGN);
      } else if (start_c == '-' && curr_c == '>') {
        return new_token (c2m_ctx, pos, "->", T_ARROW, N_DEREF_FIELD);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '+')
          return new_token (c2m_ctx, pos, "+", T_ADDOP, N_ADD);
        else
          return new_token (c2m_ctx, pos, "-", T_ADDOP, N_SUB);
      }
      assert (FALSE);
    case '=':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        return new_token (c2m_ctx, pos, "==", T_EQNE, N_EQ);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, "=", '=', N_ASSIGN);
      }
      assert (FALSE);
    case '<':
    case '>':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == start_c) {
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '=') {
          if (start_c == '<')
            return new_token (c2m_ctx, pos, "<<=", T_ASSIGN, N_LSH_ASSIGN);
          else
            return new_token (c2m_ctx, pos, ">>=", T_ASSIGN, N_RSH_ASSIGN);
        } else {
          cs_unget (c2m_ctx, curr_c);
          if (start_c == '<')
            return new_token (c2m_ctx, pos, "<<", T_SH, N_LSH);
          else
            return new_token (c2m_ctx, pos, ">>", T_SH, N_RSH);
        }
      } else if (curr_c == '=') {
        if (start_c == '<')
          return new_token (c2m_ctx, pos, "<=", T_CMP, N_LE);
        else
          return new_token (c2m_ctx, pos, ">=", T_CMP, N_GE);
      } else if (start_c == '<' && curr_c == ':') {
        return new_token (c2m_ctx, pos, "<:", '[', N_IGNORE);
      } else if (start_c == '<' && curr_c == '%') {
        return new_token (c2m_ctx, pos, "<%", '{', N_IGNORE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '<')
          return new_token (c2m_ctx, pos, "<", T_CMP, N_LT);
        else
          return new_token (c2m_ctx, pos, ">", T_CMP, N_GT);
      }
      assert (FALSE);
    case '*':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        return new_token (c2m_ctx, pos, "*=", T_ASSIGN, N_MUL_ASSIGN);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, "*", '*', N_MUL);
      }
      assert (FALSE);
    case '/':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      assert (curr_c != '/' && curr_c != '*');
      if (curr_c == '=') return new_token (c2m_ctx, pos, "/=", T_ASSIGN, N_DIV_ASSIGN);
      assert (curr_c != '*' && curr_c != '/'); /* we already processed comments */
      cs_unget (c2m_ctx, curr_c);
      return new_token (c2m_ctx, pos, "/", T_DIVOP, N_DIV);
    case '%':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        return new_token (c2m_ctx, pos, "%=", T_ASSIGN, N_MOD_ASSIGN);
      } else if (curr_c == '>') {
        return new_token (c2m_ctx, pos, "%>", '}', N_IGNORE);
      } else if (curr_c == ':') {
        curr_c = cs_get (c2m_ctx);
        if (curr_c != '%') {
          cs_unget (c2m_ctx, curr_c);
          return new_token (c2m_ctx, pos, "%:", '#', N_IGNORE);
        } else {
          curr_c = cs_get (c2m_ctx);
          if (curr_c == ':')
            return new_token (c2m_ctx, pos, "%:%:", T_DBLNO, N_IGNORE);
          else {
            cs_unget (c2m_ctx, '%');
            cs_unget (c2m_ctx, curr_c);
            return new_token (c2m_ctx, pos, "%:", '#', N_IGNORE);
          }
        }
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, "%", T_DIVOP, N_MOD);
      }
      assert (FALSE);
    case '&':
    case '|':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        if (start_c == '&')
          return new_token (c2m_ctx, pos, "&=", T_ASSIGN, N_AND_ASSIGN);
        else
          return new_token (c2m_ctx, pos, "|=", T_ASSIGN, N_OR_ASSIGN);
      } else if (curr_c == start_c) {
        if (start_c == '&')
          return new_token (c2m_ctx, pos, "&&", T_ANDAND, N_ANDAND);
        else
          return new_token (c2m_ctx, pos, "||", T_OROR, N_OROR);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '&')
          return new_token (c2m_ctx, pos, "&", start_c, N_AND);
        else
          return new_token (c2m_ctx, pos, "|", start_c, N_OR);
      }
      assert (FALSE);
    case '^':
    case '!':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '=') {
        if (start_c == '^')
          return new_token (c2m_ctx, pos, "^=", T_ASSIGN, N_XOR_ASSIGN);
        else
          return new_token (c2m_ctx, pos, "!=", T_EQNE, N_NE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        if (start_c == '^')
          return new_token (c2m_ctx, pos, "^", '^', N_XOR);
        else
          return new_token (c2m_ctx, pos, "!", T_UNOP, N_NOT);
      }
      assert (FALSE);
    case ';': return new_token (c2m_ctx, cs->pos, ";", curr_c, N_IGNORE);
    case '?': return new_token (c2m_ctx, cs->pos, "?", curr_c, N_IGNORE);
    case '(': return new_token (c2m_ctx, cs->pos, "(", curr_c, N_IGNORE);
    case ')': return new_token (c2m_ctx, cs->pos, ")", curr_c, N_IGNORE);
    case '{': return new_token (c2m_ctx, cs->pos, "{", curr_c, N_IGNORE);
    case '}': return new_token (c2m_ctx, cs->pos, "}", curr_c, N_IGNORE);
    case ']': return new_token (c2m_ctx, cs->pos, "]", curr_c, N_IGNORE);
    case EOF: {
      pos = cs->pos;
      if (eof_s != NULL) free_stream (eof_s);
      if (eof_s != cs && cs->f != stdin && cs->f != NULL) {
        fclose (cs->f);
        cs->f = NULL;
      }
      eof_s = VARR_LENGTH (stream_t, streams) == 0 ? NULL : VARR_POP (stream_t, streams);
      if (VARR_LENGTH (stream_t, streams) == 0) {
        return new_token (c2m_ctx, pos, "<EOU>", T_EOU, N_IGNORE);
      }
      cs = VARR_LAST (stream_t, streams);
      if (cs->f == NULL && cs->fname != NULL && !string_stream_p (cs)) {
        if ((cs->f = fopen (cs->fname, "rb")) == NULL) {
          if (c2m_options->message_file != NULL)
            fprintf (c2m_options->message_file, "cannot reopen file %s -- good bye\n", cs->fname);
          longjmp (c2m_ctx->env, 1);  // ???
        }
        fsetpos (cs->f, &cs->fpos);
      }
      return new_token (c2m_ctx, cs->pos, "<EOF>", T_EOFILE, N_IGNORE);
    }
    case ':':
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '>') {
        return new_token (c2m_ctx, cs->pos, ":>", ']', N_IGNORE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, cs->pos, ":", ':', N_IGNORE);
      }
    case '#':
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '#') {
        return new_token (c2m_ctx, cs->pos, "##", T_DBLNO, N_IGNORE);
      } else {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, cs->pos, "#", '#', N_IGNORE);
      }
    case ',': return new_token (c2m_ctx, cs->pos, ",", ',', N_COMMA);
    case '[': return new_token (c2m_ctx, cs->pos, "[", '[', N_IND);
    case '.':
      pos = cs->pos;
      curr_c = cs_get (c2m_ctx);
      if (curr_c == '.') {
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '.') {
          return new_token (c2m_ctx, pos, "...", T_DOTS, N_IGNORE);
        } else {
          cs_unget (c2m_ctx, '.');
          cs_unget (c2m_ctx, curr_c);
          return new_token (c2m_ctx, pos, ".", '.', N_FIELD);
        }
      } else if (!isdigit (curr_c)) {
        cs_unget (c2m_ctx, curr_c);
        return new_token (c2m_ctx, pos, ".", '.', N_FIELD);
      }
      cs_unget (c2m_ctx, curr_c);
      curr_c = '.';
      /* falls through */
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
      pos = cs->pos;
      VARR_TRUNC (char, symbol_text, 0);
      for (;;) {
        VARR_PUSH (char, symbol_text, curr_c);
        curr_c = cs_get (c2m_ctx);
        if (curr_c == 'e' || curr_c == 'E' || curr_c == 'p' || curr_c == 'P') {
          int c = cs_get (c2m_ctx);

          if (c == '+' || c == '-') {
            VARR_PUSH (char, symbol_text, curr_c);
            curr_c = c;
          } else {
            cs_unget (c2m_ctx, c);
          }
        } else if (!isdigit (curr_c) && !isalpha (curr_c) && curr_c != '_' && curr_c != '.')
          break;
      }
      VARR_PUSH (char, symbol_text, '\0');
      cs_unget (c2m_ctx, curr_c);
      return new_token_wo_uniq_repr (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_NUMBER,
                                     N_IGNORE);
    }
    case '\'':
    case '\"':
      wide_type = ' ';
    literal: {
      token_t t;
      int stop = curr_c;

      pos = cs->pos;
      VARR_PUSH (char, symbol_text, curr_c);
      for (curr_c = cs_get (c2m_ctx); curr_c != stop && curr_c != '\n' && curr_c != EOF;
           curr_c = cs_get (c2m_ctx)) {
        if (curr_c == '\0') {
          warning (c2m_ctx, pos, "null character in %s literal ignored",
                   stop == '"' ? "string" : "char");
        } else {
          VARR_PUSH (char, symbol_text, curr_c);
        }
        if (curr_c != '\\') continue;
        curr_c = cs_get (c2m_ctx);
        if (curr_c == '\n' || curr_c == EOF) break;
        if (curr_c == '\0') {
          warning (c2m_ctx, pos, "null character in %s literal ignored",
                   stop == '"' ? "string" : "char");
        } else {
          VARR_PUSH (char, symbol_text, curr_c);
        }
      }
      VARR_PUSH (char, symbol_text, curr_c);
      if (curr_c == stop) {
        if (stop == '\'' && VARR_LENGTH (char, symbol_text) == 1)
          error (c2m_ctx, pos, "empty character");
      } else {
        if (curr_c == '\n') cs_unget (c2m_ctx, '\n');
        error (c2m_ctx, pos, "unterminated %s", stop == '"' ? "string" : "char");
        VARR_PUSH (char, symbol_text, stop);
      }
      VARR_PUSH (char, symbol_text, '\0');
      if (wide_type == 'U' || (sizeof (mir_wchar) == 4 && wide_type == 'L')) {
        t = (stop == '\"' ? new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_STR,
                                            new_str_node (c2m_ctx, N_STR32, empty_str, pos))
                          : new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_CH,
                                            new_ch32_node (c2m_ctx, ' ', pos)));
      } else if (wide_type == 'u' || wide_type == 'L') {
        t = (stop == '\"' ? new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_STR,
                                            new_str_node (c2m_ctx, N_STR16, empty_str, pos))
                          : new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_CH,
                                            new_ch16_node (c2m_ctx, ' ', pos)));
      } else {
        t = (stop == '\"' ? new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_STR,
                                            new_str_node (c2m_ctx, N_STR, empty_str, pos))
                          : new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_CH,
                                            new_ch_node (c2m_ctx, ' ', pos)));
      }
      set_string_val (c2m_ctx, t, symbol_text, wide_type);
      return t;
    }
    default:
      if (isalpha (curr_c) || curr_c == '_') {
        if (curr_c == 'L' || curr_c == 'u' || curr_c == 'U') {
          wide_type = curr_c;
          if ((curr_c = cs_get (c2m_ctx)) == '\"' || curr_c == '\'') {
            VARR_PUSH (char, symbol_text, wide_type);
            goto literal;
          } else if (wide_type == 'u' && curr_c == '8') {
            wide_type = '8';
            if ((curr_c = cs_get (c2m_ctx)) == '\"') {
              VARR_PUSH (char, symbol_text, 'u');
              VARR_PUSH (char, symbol_text, '8');
              goto literal;
            }
            cs_unget (c2m_ctx, curr_c);
            curr_c = '8';
          }
          cs_unget (c2m_ctx, curr_c);
          curr_c = wide_type;
        }
        pos = cs->pos;
        do {
          VARR_PUSH (char, symbol_text, curr_c);
          curr_c = cs_get (c2m_ctx);
        } while (isalnum (curr_c) || curr_c == '_');
        cs_unget (c2m_ctx, curr_c);
        VARR_PUSH (char, symbol_text, '\0');
        return new_id_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text));
      } else {
        VARR_PUSH (char, symbol_text, curr_c);
        VARR_PUSH (char, symbol_text, '\0');
        return new_token_wo_uniq_repr (c2m_ctx, cs->pos, VARR_ADDR (char, symbol_text), curr_c,
                                       N_IGNORE);
      }
    }
  }
}

static token_t get_next_pptoken (c2m_ctx_t c2m_ctx) { return get_next_pptoken_1 (c2m_ctx, FALSE); }

static token_t get_next_include_pptoken (c2m_ctx_t c2m_ctx) {
  return get_next_pptoken_1 (c2m_ctx, TRUE);
}

#ifdef C2MIR_PREPRO_DEBUG
static const char *get_token_str (token_t t) {
  switch (t->code) {
  case T_EOFILE: return "EOF";
  case T_DBLNO: return "DBLNO";
  case T_PLM: return "PLM";
  case T_RDBLNO: return "RDBLNO";
  case T_BOA: return "BOA";
  case T_EOA: return "EOA";
  case T_EOR: return "EOR";
  case T_EOP: return "EOP";
  case T_EOU: return "EOU";
  default: return t->repr;
  }
}
#endif

static void unget_next_pptoken (c2m_ctx_t c2m_ctx, token_t t) {
  VARR_PUSH (token_t, buffered_tokens, t);
}

static const char *stringify (const char *str, VARR (char) * to) {
  VARR_TRUNC (char, to, 0);
  VARR_PUSH (char, to, '"');
  for (; *str != '\0'; str++) {
    if (*str == '\"' || *str == '\\') VARR_PUSH (char, to, '\\');
    VARR_PUSH (char, to, *str);
  }
  VARR_PUSH (char, to, '"');
  return VARR_ADDR (char, to);
}

static void destringify (const char *repr, VARR (char) * to) {
  int i, repr_len = (int) strlen (repr);

  VARR_TRUNC (char, to, 0);
  if (repr_len == 0) return;
  i = repr[0] == '"' ? 1 : 0;
  if (i == 1 && repr_len == 1) return;
  if (repr[repr_len - 1] == '"') repr_len--;
  for (; i < repr_len; i++)
    if (repr[i] != '\\' || i + 1 >= repr_len || (repr[i + 1] != '\\' && repr[i + 1] != '"'))
      VARR_PUSH (char, to, repr[i]);
}

/* TS - vector, T defines position for empty vector */
static token_t token_stringify (c2m_ctx_t c2m_ctx, token_t t, VARR (token_t) * ts) {
  if (VARR_LENGTH (token_t, ts) != 0) t = VARR_GET (token_t, ts, 0);
  t = new_node_token (c2m_ctx, t->pos, "", T_STR, new_str_node (c2m_ctx, N_STR, empty_str, t->pos));
  VARR_TRUNC (char, temp_string, 0);
  for (const char *s = t->repr; *s != 0; s++) VARR_PUSH (char, temp_string, *s);
  VARR_PUSH (char, temp_string, '"');
  for (size_t i = 0; i < VARR_LENGTH (token_t, ts); i++)
    if (VARR_GET (token_t, ts, i)->code == ' ' || VARR_GET (token_t, ts, i)->code == '\n') {
      VARR_PUSH (char, temp_string, ' ');
    } else {
      for (const char *s = VARR_GET (token_t, ts, i)->repr; *s != 0; s++) {
        int c = VARR_LENGTH (token_t, ts) == i + 1 ? '\0' : VARR_GET (token_t, ts, i + 1)->repr[0];

        /* It is an implementation defined behaviour analogous GCC/Clang (see set_string_val): */
        if (*s == '\"'
            || (*s == '\\' && c != '\\' && c != 'a' && c != 'b' && c != 'f' && c != 'n' && c != 'r'
                && c != 'v' && c != 't' && c != '?' && c != 'e' && !('0' <= c && c <= '7')
                && c != 'x' && c != 'X'))
          VARR_PUSH (char, temp_string, '\\');
        VARR_PUSH (char, temp_string, *s);
      }
    }
  VARR_PUSH (char, temp_string, '"');
  VARR_PUSH (char, temp_string, '\0');
  t->repr = uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
  set_string_val (c2m_ctx, t, temp_string, ' ');
  return t;
}

static node_t get_int_node_from_repr (c2m_ctx_t c2m_ctx, const char *repr, char **stop, int base,
                                      int uns_p, int long_p, int llong_p, pos_t pos) {
  mir_ullong ull = strtoull (repr, stop, base);

  if (llong_p) {
    if (!uns_p && (base == 10 || ull <= MIR_LLONG_MAX)) return new_ll_node (c2m_ctx, ull, pos);
    return new_ull_node (c2m_ctx, ull, pos);
  }
  if (long_p) {
    if (!uns_p && ull <= MIR_LONG_MAX) return new_l_node (c2m_ctx, (long) ull, pos);
    if (ull <= MIR_ULONG_MAX) return new_ul_node (c2m_ctx, (unsigned long) ull, pos);
    if (!uns_p && (base == 10 || ull <= MIR_LLONG_MAX)) return new_ll_node (c2m_ctx, ull, pos);
    return new_ull_node (c2m_ctx, ull, pos);
  }
  if (uns_p) {
    if (ull <= MIR_UINT_MAX) return new_u_node (c2m_ctx, (unsigned long) ull, pos);
    if (ull <= MIR_ULONG_MAX) return new_ul_node (c2m_ctx, (unsigned long) ull, pos);
    return new_ull_node (c2m_ctx, ull, pos);
  }
  if (ull <= MIR_INT_MAX) return new_i_node (c2m_ctx, (long) ull, pos);
  if (base != 10 && ull <= MIR_UINT_MAX) return new_u_node (c2m_ctx, (unsigned long) ull, pos);
  if (ull <= MIR_LONG_MAX) return new_l_node (c2m_ctx, (long) ull, pos);
  if (ull <= MIR_ULONG_MAX) return new_ul_node (c2m_ctx, (unsigned long) ull, pos);
  if (base == 10 || ull <= MIR_LLONG_MAX) return new_ll_node (c2m_ctx, ull, pos);
  return new_ull_node (c2m_ctx, ull, pos);
}

static token_t pptoken2token (c2m_ctx_t c2m_ctx, token_t t, int id2kw_p) {
  assert (t->code != T_HEADER && t->code != T_BOA && t->code != T_EOA && t->code != T_EOR
          && t->code != T_EOP && t->code != T_EOFILE && t->code != T_EOU && t->code != T_PLM
          && t->code != T_RDBLNO);
  if (t->code == T_NO_MACRO_IDENT) t->code = T_ID;
  if (t->code == T_ID && id2kw_p) {
    tab_str_t str = str_add (c2m_ctx, t->repr, strlen (t->repr) + 1, T_STR, 0, FALSE);

    if (str.key != T_STR) {
      t->code = (int) str.key;
      t->node_code = N_IGNORE;
      t->node = NULL;
    }
    return t;
  } else if (t->code == ' ' || t->code == '\n') {
    return NULL;
  } else if (t->code == T_NUMBER) {
    int i, base = 10, float_p = FALSE, double_p = FALSE, ldouble_p = FALSE;
    int uns_p = FALSE, long_p = FALSE, llong_p = FALSE;
    const char *repr = t->repr, *start = t->repr;
    char *stop;
    int last = (int) strlen (repr) - 1;

    assert (last >= 0);
    if (repr[0] == '0' && (repr[1] == 'x' || repr[1] == 'X')) {
      base = 16;
    } else if (repr[0] == '0' && (repr[1] == 'b' || repr[1] == 'B')) {
      (c2m_options->pedantic_p ? error : warning) (c2m_ctx, t->pos,
                                                   "binary number is not a standard: %s", t->repr);
      base = 2;
      start += 2;
    } else if (repr[0] == '0') {
      base = 8;
    }
    for (i = 0; i <= last; i++) {
      if (repr[i] == '.') {
        double_p = TRUE;
      } else if (repr[i] == 'p' || repr[i] == 'P') {
        double_p = TRUE;
      } else if ((repr[i] == 'e' || repr[i] == 'E') && base != 16) {
        double_p = TRUE;
      }
    }
    if (last >= 2
        && (strcmp (&repr[last - 2], "LLU") == 0 || strcmp (&repr[last - 2], "ULL") == 0
            || strcmp (&repr[last - 2], "llu") == 0 || strcmp (&repr[last - 2], "ull") == 0
            || strcmp (&repr[last - 2], "LLu") == 0 || strcmp (&repr[last - 2], "uLL") == 0
            || strcmp (&repr[last - 2], "llU") == 0 || strcmp (&repr[last - 2], "Ull") == 0)) {
      llong_p = uns_p = TRUE;
      last -= 3;
    } else if (last >= 1
               && (strcmp (&repr[last - 1], "LL") == 0 || strcmp (&repr[last - 1], "ll") == 0)) {
      llong_p = TRUE;
      last -= 2;
    } else if (last >= 1
               && (strcmp (&repr[last - 1], "LU") == 0 || strcmp (&repr[last - 1], "UL") == 0
                   || strcmp (&repr[last - 1], "lu") == 0 || strcmp (&repr[last - 1], "ul") == 0
                   || strcmp (&repr[last - 1], "Lu") == 0 || strcmp (&repr[last - 1], "uL") == 0
                   || strcmp (&repr[last - 1], "lU") == 0 || strcmp (&repr[last - 1], "Ul") == 0)) {
      long_p = uns_p = TRUE;
      last -= 2;
    } else if (strcmp (&repr[last], "L") == 0 || strcmp (&repr[last], "l") == 0) {
      long_p = TRUE;
      last--;
    } else if (strcmp (&repr[last], "U") == 0 || strcmp (&repr[last], "u") == 0) {
      uns_p = TRUE;
      last--;
    } else if (double_p && (strcmp (&repr[last], "F") == 0 || strcmp (&repr[last], "f") == 0)) {
      float_p = TRUE;
      double_p = FALSE;
      last--;
    }
    if (double_p) {
      if (uns_p || llong_p) {
        error (c2m_ctx, t->pos, "wrong number: %s", repr);
      } else if (long_p) {
        ldouble_p = TRUE;
        double_p = FALSE;
      }
    }
    errno = 0;
    if (float_p) {
      t->node = new_f_node (c2m_ctx, strtof (start, &stop), t->pos);
    } else if (double_p) {
      t->node = new_d_node (c2m_ctx, strtod (start, &stop), t->pos);
    } else if (ldouble_p) {
      t->node = new_ld_node (c2m_ctx, strtold (start, &stop), t->pos);
    } else {
      t->node
        = get_int_node_from_repr (c2m_ctx, start, &stop, base, uns_p, long_p, llong_p, t->pos);
    }
    if (stop != &repr[last + 1]) {
      if (c2m_options->message_file != NULL)
        fprintf (c2m_options->message_file, "%s:%s:%s\n", repr, stop, &repr[last + 1]);
      error (c2m_ctx, t->pos, "wrong number: %s", t->repr);
    } else if (errno) {
      if (float_p || double_p || ldouble_p) {
        warning (c2m_ctx, t->pos, "number %s is out of range -- using IEEE infinity", t->repr);
      } else {
        (c2m_options->pedantic_p ? error : warning) (c2m_ctx, t->pos, "number %s is out of range",
                                                     t->repr);
      }
    }
  }
  return t;
}

/* --------------------------- Preprocessor -------------------------------- */

typedef struct macro {          /* macro definition: */
  token_t id;                   /* T_ID */
  VARR (token_t) * params;      /* (T_ID)* [N_DOTS], NULL means no params */
  VARR (token_t) * replacement; /* token*, NULL means a standard macro */
  int ignore_p;
} *macro_t;

DEF_VARR (macro_t);
DEF_HTAB (macro_t);

typedef struct ifstate {
  int skip_p, true_p, else_p; /* ??? flags that we are in a else part and in a false part */
  pos_t if_pos;               /* pos for #if and last #else, #elif */
} *ifstate_t;

DEF_VARR (ifstate_t);

typedef VARR (token_t) * token_arr_t;

DEF_VARR (token_arr_t);

typedef struct macro_call {
  macro_t macro;
  pos_t pos;
  /* Var array of arguments, each arg is var array of tokens, NULL for args absence: */
  VARR (token_arr_t) * args;
  int repl_pos;                 /* position in macro replacement */
  VARR (token_t) * repl_buffer; /* LIST:(token nodes)* */
} *macro_call_t;

DEF_VARR (macro_call_t);

struct pre_ctx {
  VARR (char_ptr_t) * once_include_files;
  VARR (token_t) * temp_tokens;
  HTAB (macro_t) * macro_tab;
  VARR (macro_t) * macros;
  VARR (ifstate_t) * ifs; /* stack of ifstates */
  int no_out_p;           /* don't output lexs -- put them into buffer */
  int skip_if_part_p;
  token_t if_id; /* last processed token #if or #elif: used for error messages */
  char date_str[50], time_str[50], date_str_repr[50], time_str_repr[50];
  VARR (token_t) * output_buffer;
  VARR (macro_call_t) * macro_call_stack;
  VARR (token_t) * pre_expr;
  token_t pre_last_token;
  pos_t actual_pre_pos;
  unsigned long pptokens_num;
  void (*pre_out_token_func) (c2m_ctx_t c2m_ctx, token_t);
};

#define once_include_files pre_ctx->once_include_files
#define temp_tokens pre_ctx->temp_tokens
#define macro_tab pre_ctx->macro_tab
#define macros pre_ctx->macros
#define ifs pre_ctx->ifs
#define no_out_p pre_ctx->no_out_p
#define skip_if_part_p pre_ctx->skip_if_part_p
#define if_id pre_ctx->if_id
#define date_str pre_ctx->date_str
#define time_str pre_ctx->time_str
#define date_str_repr pre_ctx->date_str_repr
#define time_str_repr pre_ctx->time_str_repr
#define output_buffer pre_ctx->output_buffer
#define macro_call_stack pre_ctx->macro_call_stack
#define pre_expr pre_ctx->pre_expr
#define pre_last_token pre_ctx->pre_last_token
#define actual_pre_pos pre_ctx->actual_pre_pos
#define pptokens_num pre_ctx->pptokens_num
#define pre_out_token_func pre_ctx->pre_out_token_func

static int pre_skip_if_part_p (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  return pre_ctx != NULL && skip_if_part_p;
}

/* It is a token based prerpocessor.
   It is input preprocessor tokens and output is (parser) tokens */

static void add_to_temp_string (c2m_ctx_t c2m_ctx, const char *str) {
  size_t i, len;

  if ((len = VARR_LENGTH (char, temp_string)) != 0
      && VARR_GET (char, temp_string, len - 1) == '\0') {
    VARR_POP (char, temp_string);
  }
  len = strlen (str);
  for (i = 0; i < len; i++) VARR_PUSH (char, temp_string, str[i]);
  VARR_PUSH (char, temp_string, '\0');
}

static int macro_eq (macro_t macro1, macro_t macro2, void *arg MIR_UNUSED) {
  return macro1->id->repr == macro2->id->repr;
}

static htab_hash_t macro_hash (macro_t macro, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash (macro->id->repr, strlen (macro->id->repr), 0x42);
}

static macro_t new_macro (c2m_ctx_t c2m_ctx, token_t id, VARR (token_t) * params,
                          VARR (token_t) * replacement);

static void new_std_macro (c2m_ctx_t c2m_ctx, const char *id_str) {
  new_macro (c2m_ctx, new_id_token (c2m_ctx, no_pos, id_str), NULL, NULL);
}

static void init_macros (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  VARR (token_t) * params;

  VARR_CREATE (macro_t, macros, alloc, 2048);
  HTAB_CREATE (macro_t, macro_tab, alloc, 2048, macro_hash, macro_eq, NULL);
  /* Standard macros : */
  new_std_macro (c2m_ctx, "__DATE__");
  new_std_macro (c2m_ctx, "__TIME__");
  new_std_macro (c2m_ctx, "__FILE__");
  new_std_macro (c2m_ctx, "__LINE__");
  if (!c2m_options->pedantic_p) {
    VARR_CREATE (token_t, params, alloc, 1);
    VARR_PUSH (token_t, params, new_id_token (c2m_ctx, no_pos, "$"));
    new_macro (c2m_ctx, new_id_token (c2m_ctx, no_pos, "__has_include"), params, NULL);
    VARR_CREATE (token_t, params, alloc, 1);
    VARR_PUSH (token_t, params, new_id_token (c2m_ctx, no_pos, "$"));
    new_macro (c2m_ctx, new_id_token (c2m_ctx, no_pos, "__has_builtin"), params, NULL);
  }
}

static macro_t new_macro (c2m_ctx_t c2m_ctx, token_t id, VARR (token_t) * params,
                          VARR (token_t) * replacement) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  macro_t tab_m, m = malloc (sizeof (struct macro));

  m->id = id;
  m->params = params;
  m->replacement = replacement;
  m->ignore_p = FALSE;
  assert (!HTAB_DO (macro_t, macro_tab, m, HTAB_FIND, tab_m));
  HTAB_DO (macro_t, macro_tab, m, HTAB_INSERT, tab_m);
  VARR_PUSH (macro_t, macros, m);
  return m;
}

static void finish_macros (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  if (macros != NULL) {
    while (VARR_LENGTH (macro_t, macros) != 0) {
      macro_t m = VARR_POP (macro_t, macros);

      if (m->params != NULL) VARR_DESTROY (token_t, m->params);
      if (m->replacement != NULL) VARR_DESTROY (token_t, m->replacement);
      free (m);
    }
    VARR_DESTROY (macro_t, macros);
  }
  if (macro_tab != NULL) HTAB_DESTROY (macro_t, macro_tab);
}

static macro_call_t new_macro_call (MIR_alloc_t alloc, macro_t m, pos_t pos) {
  macro_call_t mc = malloc (sizeof (struct macro_call));

  mc->macro = m;
  mc->pos = pos;
  mc->repl_pos = 0;
  mc->args = NULL;
  VARR_CREATE (token_t, mc->repl_buffer, alloc, 64);
  return mc;
}

static void free_macro_call (macro_call_t mc) {
  VARR_DESTROY (token_t, mc->repl_buffer);
  if (mc->args != NULL) {
    while (VARR_LENGTH (token_arr_t, mc->args) != 0) {
      VARR (token_t) *arg = VARR_POP (token_arr_t, mc->args);
      VARR_DESTROY (token_t, arg);
    }
    VARR_DESTROY (token_arr_t, mc->args);
  }
  free (mc);
}

static ifstate_t new_ifstate (int skip_p, int true_p, int else_p, pos_t if_pos) {
  ifstate_t ifstate = malloc (sizeof (struct ifstate));

  ifstate->skip_p = skip_p;
  ifstate->true_p = true_p;
  ifstate->else_p = else_p;
  ifstate->if_pos = if_pos;
  return ifstate;
}

static void pop_ifstate (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  ifstate_t ifstate = VARR_POP (ifstate_t, ifs);
  free (ifstate);
}

static void pre_init (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx;
  time_t t, time_loc;
  struct tm *tm, tm_loc MIR_UNUSED;

  c2m_ctx->pre_ctx = pre_ctx = c2mir_calloc (c2m_ctx, sizeof (struct pre_ctx));
  no_out_p = skip_if_part_p = FALSE;
  t = time (&time_loc);
#if defined(_WIN32)
  tm = localtime (&t);
#else
  tm = localtime_r (&t, &tm_loc);
#endif
  if (tm == NULL) {
    strcpy (date_str_repr, "\"Unknown date\"");
    strcpy (time_str_repr, "\"Unknown time\"");
  } else {
    strftime (date_str_repr, sizeof (date_str), "\"%b %d %Y\"", tm);
    strftime (time_str_repr, sizeof (time_str), "\"%H:%M:%S\"", tm);
  }
  strcpy (date_str, date_str_repr + 1);
  date_str[strlen (date_str) - 1] = '\0';
  strcpy (time_str, time_str_repr + 1);
  time_str[strlen (time_str) - 1] = '\0';
  VARR_CREATE (char_ptr_t, once_include_files, alloc, 64);
  VARR_CREATE (token_t, temp_tokens, alloc, 128);
  VARR_CREATE (token_t, output_buffer, alloc, 2048);
  init_macros (c2m_ctx);
  VARR_CREATE (ifstate_t, ifs, alloc, 512);
  VARR_CREATE (macro_call_t, macro_call_stack, alloc, 512);
}

static void pre_finish (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx;

  if (c2m_ctx == NULL || (pre_ctx = c2m_ctx->pre_ctx) == NULL) return;
  if (once_include_files != NULL) VARR_DESTROY (char_ptr_t, once_include_files);
  if (temp_tokens != NULL) VARR_DESTROY (token_t, temp_tokens);
  if (output_buffer != NULL) VARR_DESTROY (token_t, output_buffer);
  finish_macros (c2m_ctx);
  if (ifs != NULL) {
    while (VARR_LENGTH (ifstate_t, ifs) != 0) pop_ifstate (c2m_ctx);
    VARR_DESTROY (ifstate_t, ifs);
  }
  if (macro_call_stack != NULL) {
    while (VARR_LENGTH (macro_call_t, macro_call_stack) != 0)
      free_macro_call (VARR_POP (macro_call_t, macro_call_stack));
    VARR_DESTROY (macro_call_t, macro_call_stack);
  }
  free (c2m_ctx->pre_ctx);
}

static void add_include_stream (c2m_ctx_t c2m_ctx, const char *fname, const char *content,
                                pos_t err_pos) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  FILE *f;

  for (size_t i = 0; i < VARR_LENGTH (char_ptr_t, once_include_files); i++)
    if (strcmp (fname, VARR_GET (char_ptr_t, once_include_files, i)) == 0) return;
  assert (fname != NULL);
  if (content == NULL && (f = fopen (fname, "rb")) == NULL) {
    if (c2m_options->message_file != NULL)
      error (c2m_ctx, err_pos, "error in opening file %s", fname);
    longjmp (c2m_ctx->env, 1);  // ???
  }
  if (content == NULL)
    add_stream (c2m_ctx, f, fname, NULL);
  else
    add_string_stream (c2m_ctx, fname, content);
  cs->ifs_length_at_stream_start = (int) VARR_LENGTH (ifstate_t, ifs);
}

static void skip_nl (c2m_ctx_t c2m_ctx, token_t t,
                     VARR (token_t) * buffer) { /* skip until new line */
  if (t == NULL) t = get_next_pptoken (c2m_ctx);
  for (; t->code != '\n' && t->code != T_EOU; t = get_next_pptoken (c2m_ctx))  // ??>
    if (buffer != NULL) VARR_PUSH (token_t, buffer, t);
  unget_next_pptoken (c2m_ctx, t);
}

static const char *varg = "__VA_ARGS__";

static int find_param (VARR (token_t) * params, const char *name) {
  size_t len = VARR_LENGTH (token_t, params);
  token_t param;

  if (strcmp (name, varg) == 0 && len != 0 && VARR_LAST (token_t, params)->code == T_DOTS)
    return (int) len - 1;
  for (size_t i = 0; i < len; i++) {
    param = VARR_GET (token_t, params, i);
    if (strcmp (param->repr, name) == 0) return (int) i;
  }
  return -1;
}

static int params_eq_p (VARR (token_t) * params1, VARR (token_t) * params2) {
  token_t param1, param2;

  if (params1 == NULL || params2 == NULL) return params1 == params2;
  if (VARR_LENGTH (token_t, params1) != VARR_LENGTH (token_t, params2)) return FALSE;
  for (size_t i = 0; i < VARR_LENGTH (token_t, params1); i++) {
    param1 = VARR_GET (token_t, params1, i);
    param2 = VARR_GET (token_t, params2, i);
    if (strcmp (param1->repr, param2->repr) != 0) return FALSE;
  }
  return TRUE;
}

static int replacement_eq_p (VARR (token_t) * r1, VARR (token_t) * r2) {
  token_t el1, el2;

  if (VARR_LENGTH (token_t, r1) != VARR_LENGTH (token_t, r2)) return FALSE;
  for (size_t i = 0; i < VARR_LENGTH (token_t, r1); i++) {
    el1 = VARR_GET (token_t, r1, i);
    el2 = VARR_GET (token_t, r2, i);

    if (el1->code == ' ' && el2->code == ' ') continue;
    if (el1->node_code != el2->node_code) return FALSE;
    if (strcmp (el1->repr, el2->repr) != 0) return FALSE;
  }
  return TRUE;
}

static void define (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  VARR (token_t) * repl, *params;
  token_t id, t;
  const char *name;
  macro_t m;
  struct macro macro_struct;

  t = get_next_pptoken (c2m_ctx);                      // ???
  if (t->code == ' ') t = get_next_pptoken (c2m_ctx);  // ??
  if (t->code != T_ID) {
    error (c2m_ctx, t->pos, "no ident after #define: %s", t->repr);
    skip_nl (c2m_ctx, t, NULL);
    return;
  }
  id = t;
  t = get_next_pptoken (c2m_ctx);
  VARR_CREATE (token_t, repl, alloc, 64);
  params = NULL;
  if (t->code == '(') {
    VARR_CREATE (token_t, params, alloc, 16);
    t = get_next_pptoken (c2m_ctx); /* skip '(' */
    if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
    if (t->code != ')') {
      for (;;) {
        if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
        if (t->code == T_ID) {
          if (find_param (params, t->repr) >= 0)
            error (c2m_ctx, t->pos, "repeated macro parameter %s", t->repr);
          VARR_PUSH (token_t, params, t);
        } else if (t->code == T_DOTS) {
          VARR_PUSH (token_t, params, t);
        } else {
          error (c2m_ctx, t->pos, "macro parameter is expected");
          break;
        }
        t = get_next_pptoken (c2m_ctx);
        if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
        if (t->code == ')') break;
        if (VARR_LAST (token_t, params)->code == T_DOTS) {
          error (c2m_ctx, t->pos, "... is not the last parameter");
          break;
        }
        if (t->code == T_DOTS) continue;
        if (t->code != ',') {
          error (c2m_ctx, t->pos, "missed ,");
          continue;
        }
        t = get_next_pptoken (c2m_ctx);
      }
    }
    for (; t->code != '\n' && t->code != ')';) t = get_next_pptoken (c2m_ctx);
    if (t->code == ')') t = get_next_pptoken (c2m_ctx);
  }
  if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
  for (; t->code != '\n'; t = get_next_pptoken (c2m_ctx)) {
    if (t->code == T_DBLNO) {
      if (VARR_LENGTH (token_t, repl) == 0) {
        error (c2m_ctx, t->pos, "## at the beginning of a macro expansion");
        continue;
      }
      t->code = T_RDBLNO;
    }
    VARR_PUSH (token_t, repl, t);
  }
  unget_next_pptoken (c2m_ctx, t);
  if (VARR_LENGTH (token_t, repl) != 0 && (t = VARR_LAST (token_t, repl))->code == T_RDBLNO) {
    VARR_POP (token_t, repl);
    error (c2m_ctx, t->pos, "## at the end of a macro expansion");
  }
  name = id->repr;
  macro_struct.id = id;
  if (!HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, m)) {
    if (strcmp (name, "defined") == 0) {
      error (c2m_ctx, id->pos, "macro definition of %s", name);
    } else {
      new_macro (c2m_ctx, id, params, repl);
      params = NULL;
    }
  } else if (m->replacement == NULL) {
    error (c2m_ctx, id->pos, "standard macro %s redefinition", name);
  } else {
    if (!params_eq_p (m->params, params) || !replacement_eq_p (m->replacement, repl)) {
      if (c2m_options->pedantic_p) {
        error (c2m_ctx, id->pos, "different macro redefinition of %s", name);
        error (c2m_ctx, m->id->pos, "previous definition of %s", m->id->repr);
      } else {
        VARR (token_t) * temp;
        warning (c2m_ctx, id->pos, "different macro redefinition of %s", name);
        warning (c2m_ctx, m->id->pos, "previous definition of %s", m->id->repr);
        SWAP (m->params, params, temp);
        SWAP (m->replacement, repl, temp);
      }
    }
    VARR_DESTROY (token_t, repl);
  }
  if (params != NULL) VARR_DESTROY (token_t, params);
}

#ifdef C2MIR_PREPRO_DEBUG
static void print_output_buffer (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  fprintf (stderr, "output buffer:");
  for (size_t i = 0; i < (int) VARR_LENGTH (token_t, output_buffer); i++) {
    fprintf (stderr, " <%s>", get_token_str (VARR_GET (token_t, output_buffer, i)));
  }
  fprintf (stderr, "\n");
}
#endif

static void push_back (c2m_ctx_t c2m_ctx, VARR (token_t) * tokens) {
#ifdef C2MIR_PREPRO_DEBUG
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  fprintf (stderr,
           "# push back (macro call depth %d):", VARR_LENGTH (macro_call_t, macro_call_stack));
#endif
  for (int i = (int) VARR_LENGTH (token_t, tokens) - 1; i >= 0; i--) {
#ifdef C2MIR_PREPRO_DEBUG
    fprintf (stderr, " <%s>", get_token_str (VARR_GET (token_t, tokens, i)));
#endif
    unget_next_pptoken (c2m_ctx, VARR_GET (token_t, tokens, i));
  }
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "\n");
  print_output_buffer (c2m_ctx);
#endif
}

static void copy_and_push_back (c2m_ctx_t c2m_ctx, VARR (token_t) * tokens, pos_t pos) {
#ifdef C2MIR_PREPRO_DEBUG
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  fprintf (stderr, "# copy & push back (macro call depth %d):",
           VARR_LENGTH (macro_call_t, macro_call_stack));
#endif
  for (int i = (int) VARR_LENGTH (token_t, tokens) - 1; i >= 0; i--) {
#ifdef C2MIR_PREPRO_DEBUG
    fprintf (stderr, " <%s>", get_token_str (VARR_GET (token_t, tokens, i)));
#endif
    unget_next_pptoken (c2m_ctx, copy_token (c2m_ctx, VARR_GET (token_t, tokens, i), pos));
  }
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "\n");
  print_output_buffer (c2m_ctx);
#endif
}

static int file_found_p (const char *name) {
  FILE *f;

  if ((f = fopen (name, "r")) == NULL) return FALSE;
  fclose (f);
  return TRUE;
}

static const char *get_full_name (c2m_ctx_t c2m_ctx, const char *base, const char *name,
                                  int dir_base_p) {
  const char *str, *last, *last2, *slash = "/", *slash2 = NULL;
  size_t len;

  VARR_TRUNC (char, temp_string, 0);
  if (base == NULL || *base == '\0') {
    assert (name != NULL && name[0] != '\0');
    return name;
  }
#ifdef _WIN32
  slash2 = "\\";
#endif
  if (dir_base_p) {
    len = strlen (base);
    assert (len > 0);
    add_to_temp_string (c2m_ctx, base);
    if (base[len - 1] != slash[0]) add_to_temp_string (c2m_ctx, slash);
  } else {
    last = strrchr (base, slash[0]);
    last2 = slash2 != NULL ? strrchr (base, slash2[0]) : NULL;
    if (last2 != NULL && (last == NULL || last2 > last)) last = last2;
    if (last != NULL) {
      for (str = base; str <= last; str++) VARR_PUSH (char, temp_string, *str);
      VARR_PUSH (char, temp_string, '\0');
    } else {
      add_to_temp_string (c2m_ctx, ".");
      add_to_temp_string (c2m_ctx, slash);
    }
  }
  add_to_temp_string (c2m_ctx, name);
  return VARR_ADDR (char, temp_string);
}

static const char *get_include_fname (c2m_ctx_t c2m_ctx, token_t t, const char **content) {
  const char *fullname, *name;

  *content = NULL;
  assert (t->code == T_STR || t->code == T_HEADER);
  if ((name = t->node->u.s.s)[0] != '/') {
    if (t->repr[0] == '"') {
      /* Search relative to the current source dir */
      if (cs->fname != NULL) {
        fullname = get_full_name (c2m_ctx, cs->fname, name, FALSE);
        if (file_found_p (fullname)) return uniq_cstr (c2m_ctx, fullname).s;
      }
      for (size_t i = 0; header_dirs[i] != NULL; i++) {
        fullname = get_full_name (c2m_ctx, header_dirs[i], name, TRUE);
        if (file_found_p (fullname)) return uniq_cstr (c2m_ctx, fullname).s;
      }
    }
    for (size_t i = 0; i < sizeof (standard_includes) / sizeof (string_include_t); i++)
      if (standard_includes[i].name != NULL && strcmp (name, standard_includes[i].name) == 0) {
        *content = standard_includes[i].content;
        return name;
      }
    for (size_t i = 0; system_header_dirs[i] != NULL; i++) {
      fullname = get_full_name (c2m_ctx, system_header_dirs[i], name, TRUE);
      if (file_found_p (fullname)) return uniq_cstr (c2m_ctx, fullname).s;
    }
  }
  return name;
}

static int digits_p (const char *str) {
  while ('0' <= *str && *str <= '9') str++;
  return *str == '\0';
}

static pos_t check_line_directive_args (c2m_ctx_t c2m_ctx, VARR (token_t) * buffer) {
  size_t i, len = VARR_LENGTH (token_t, buffer);
  token_t *buffer_arr = VARR_ADDR (token_t, buffer);
  const char *fname;
  pos_t pos;
  int lno;
  unsigned long long l;

  if (len == 0) return no_pos;
  i = buffer_arr[0]->code == ' ' ? 1 : 0;
  fname = buffer_arr[i]->pos.fname;
  if (i >= len || buffer_arr[i]->code != T_NUMBER) return no_pos;
  if (!digits_p (buffer_arr[i]->repr)) return no_pos;
  errno = 0;
  l = strtoll (buffer_arr[i]->repr, NULL, 10);
  lno = (int) l;
  if (errno || l > ((1ul << 31) - 1))
    error (c2m_ctx, buffer_arr[i]->pos, "#line with too big value: %s", buffer_arr[i]->repr);
  i++;
  if (i < len && buffer_arr[i]->code == ' ') i++;
  if (i < len && buffer_arr[i]->code == T_STR) {
    fname = buffer_arr[i]->node->u.s.s;
    i++;
  }
  if (i == len) {
    pos.fname = fname;
    pos.lno = lno;
    pos.ln_pos = 0;
    return pos;
  }
  return no_pos;
}

static void check_pragma (c2m_ctx_t c2m_ctx, token_t t, VARR (token_t) * tokens) {
  token_t *tokens_arr = VARR_ADDR (token_t, tokens);
  size_t i, tokens_len = VARR_LENGTH (token_t, tokens);

  i = 0;
  if (i < tokens_len && tokens_arr[i]->code == ' ') i++;
#ifdef _WIN32
  if (i + 1 == tokens_len && tokens_arr[i]->code == T_ID
      && strcmp (tokens_arr[i]->repr, "once") == 0) {
    pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
    VARR_PUSH (char_ptr_t, once_include_files, cs->fname);
    return;
  }
#endif
  if (i >= tokens_len || tokens_arr[i]->code != T_ID || strcmp (tokens_arr[i]->repr, "STDC") != 0) {
    warning (c2m_ctx, t->pos, "unknown pragma");
    return;
  }
  i++;
  if (i < tokens_len && tokens_arr[i]->code == ' ') i++;
  if (i >= tokens_len || tokens_arr[i]->code != T_ID) {
    error (c2m_ctx, t->pos, "wrong STDC pragma");
    return;
  }
  if (strcmp (tokens_arr[i]->repr, "FP_CONTRACT") != 0
      && strcmp (tokens_arr[i]->repr, "FENV_ACCESS") != 0
      && strcmp (tokens_arr[i]->repr, "CX_LIMITED_RANGE") != 0) {
    error (c2m_ctx, t->pos, "unknown STDC pragma %s", tokens_arr[i]->repr);
    return;
  }
  i++;
  if (i < tokens_len && tokens_arr[i]->code == ' ') i++;
  if (i >= tokens_len || tokens_arr[i]->code != T_ID) {
    error (c2m_ctx, t->pos, "wrong STDC pragma value");
    return;
  }
  if (strcmp (tokens_arr[i]->repr, "ON") != 0 && strcmp (tokens_arr[i]->repr, "OFF") != 0
      && strcmp (tokens_arr[i]->repr, "DEFAULT") != 0) {
    error (c2m_ctx, t->pos, "unknown STDC pragma value", tokens_arr[i]->repr);
    return;
  }
  i++;
  if (i < tokens_len && (tokens_arr[i]->code == ' ' || tokens_arr[i]->code == '\n')) i++;
  if (i < tokens_len) error (c2m_ctx, t->pos, "garbage at STDC pragma end");
}

static void pop_macro_call (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  macro_call_t mc;

  mc = VARR_POP (macro_call_t, macro_call_stack);
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "finish call of macro %s\n", mc->macro->id->repr);
#endif
  mc->macro->ignore_p = FALSE;
  free_macro_call (mc);
}

static void find_args (c2m_ctx_t c2m_ctx, macro_call_t mc) { /* we have just read a parenthesis */
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  macro_t m;
  token_t t;
  int va_p, level = 0;
  size_t params_len;
  VARR (token_arr_t) * args;
  VARR (token_t) * arg, *temp_arr;

  m = mc->macro;
  VARR_CREATE (token_arr_t, args, alloc, 16);
  VARR_CREATE (token_t, arg, alloc, 16);
  params_len = VARR_LENGTH (token_t, m->params);
  va_p = params_len == 1 && VARR_GET (token_t, m->params, 0)->code == T_DOTS;
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "# finding args of macro %s call:\n#    arg 0:", m->id->repr);
#endif
  for (int newln_p = FALSE;; newln_p = t->code == '\n') {
    t = get_next_pptoken (c2m_ctx);
#ifdef C2MIR_PREPRO_DEBUG
    fprintf (stderr, " <%s>%s", get_token_str (t), t->processed_p ? "*" : "");
#endif
    if (t->code == T_EOR) {
      t = get_next_pptoken (c2m_ctx);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, " <%s>", get_token_str (t), t->processed_p ? "*" : "");
#endif
      pop_macro_call (c2m_ctx);
    }
    if (t->code == T_EOFILE || t->code == T_EOU || t->code == T_EOR || t->code == T_BOA
        || t->code == T_EOA || (newln_p && t->code == '#'))
      break;
    if (level == 0 && t->code == ')') break;
    if (level == 0 && !va_p && t->code == ',') {
      VARR_PUSH (token_arr_t, args, arg);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "\n#    arg %d:", VARR_LENGTH (token_arr_t, args));
#endif
      VARR_CREATE (token_t, arg, alloc, 16);
      if (VARR_LENGTH (token_arr_t, args) == params_len - 1
          && strcmp (VARR_GET (token_t, m->params, params_len - 1)->repr, "...") == 0)
        va_p = 1;
    } else {
      VARR_PUSH (token_t, arg, t);
      if (t->code == ')')
        level--;
      else if (t->code == '(')
        level++;
    }
  }
#ifdef C2MIR_PREPRO_DEBUG
  fprintf (stderr, "\n");
#endif
  if (t->code != ')') {
    error (c2m_ctx, t->pos, "unfinished call of macro %s", m->id->repr);
#ifdef C2MIR_PREPRO_DEBUG
    fprintf (stderr, "# push back <%s>%s\n", get_token_str (t), t->processed_p ? "*" : "");
#endif
    unget_next_pptoken (c2m_ctx, t);
  }
  VARR_PUSH (token_arr_t, args, arg);
  if (params_len == 0 && VARR_LENGTH (token_arr_t, args) == 1) {
    token_arr_t arr = VARR_GET (token_arr_t, args, 0);

    if (VARR_LENGTH (token_t, arr) == 0
        || (VARR_LENGTH (token_t, arr) == 1 && VARR_GET (token_t, arr, 0)->code == ' ')) {
      temp_arr = VARR_POP (token_arr_t, args);
      VARR_DESTROY (token_t, temp_arr);
      mc->args = args;
      return;
    }
  }
  if (VARR_LENGTH (token_arr_t, args) > params_len) {
    arg = VARR_GET (token_arr_t, args, params_len);
    if (VARR_LENGTH (token_t, arg) != 0) t = VARR_GET (token_t, arg, 0);
    while (VARR_LENGTH (token_arr_t, args) > params_len) {
      temp_arr = VARR_POP (token_arr_t, args);
      VARR_DESTROY (token_t, temp_arr);
    }
    error (c2m_ctx, t->pos, "too many args for call of macro %s", m->id->repr);
  } else if (VARR_LENGTH (token_arr_t, args) < params_len) {
    for (; VARR_LENGTH (token_arr_t, args) < params_len;) {
      VARR_CREATE (token_t, arg, alloc, 16);
      VARR_PUSH (token_arr_t, args, arg);
    }
    error (c2m_ctx, t->pos, "not enough args for call of macro %s", m->id->repr);
  }
  mc->args = args;
}

static token_t token_concat (c2m_ctx_t c2m_ctx, token_t t1, token_t t2) {
  token_t t, next;

  VARR_TRUNC (char, temp_string, 0);
  add_to_temp_string (c2m_ctx, t1->repr);
  add_to_temp_string (c2m_ctx, t2->repr);
  reverse (temp_string);
  set_string_stream (c2m_ctx, VARR_ADDR (char, temp_string), t1->pos, NULL);
  t = get_next_pptoken (c2m_ctx);
  next = get_next_pptoken (c2m_ctx);
  while (next->code == T_EOU) next = get_next_pptoken (c2m_ctx);
  if (next->code != T_EOFILE) {
    error (c2m_ctx, t1->pos, "wrong result of ##: %s", reverse (temp_string));
    remove_string_stream (c2m_ctx);
  }
  return t;
}

static void add_token (VARR (token_t) * to, token_t t) {
  if ((t->code != ' ' && t->code != '\n') || VARR_LENGTH (token_t, to) == 0
      || (VARR_LAST (token_t, to)->code != ' ' && VARR_LAST (token_t, to)->code != '\n'))
    VARR_PUSH (token_t, to, t);
}

static void add_arg_tokens (VARR (token_t) * to, VARR (token_t) * from) {
  int start;

  for (start = (int) VARR_LENGTH (token_t, from) - 1; start >= 0; start--)
    if (VARR_GET (token_t, from, start)->code == T_BOA) break;
  assert (start >= 0);
  for (size_t i = start + 1; i < VARR_LENGTH (token_t, from); i++)
    add_token (to, VARR_GET (token_t, from, i));
  VARR_TRUNC (token_t, from, start);
}

static void add_tokens (VARR (token_t) * to, VARR (token_t) * from) {
  for (size_t i = 0; i < VARR_LENGTH (token_t, from); i++)
    add_token (to, VARR_GET (token_t, from, i));
}

static void del_tokens (VARR (token_t) * tokens, int from, int len) {
  int diff, tokens_len = (int) VARR_LENGTH (token_t, tokens);
  token_t *addr = VARR_ADDR (token_t, tokens);

  if (len < 0) len = tokens_len - from;
  assert (from + len <= tokens_len);
  if ((diff = tokens_len - from - len) > 0)
    memmove (addr + from, addr + from + len, diff * sizeof (token_t));
  VARR_TRUNC (token_t, tokens, tokens_len - len);
}

static VARR (token_t) * do_concat (c2m_ctx_t c2m_ctx, VARR (token_t) * tokens) {
  int i, j, k, empty_j_p, empty_k_p, len = (int) VARR_LENGTH (token_t, tokens);
  token_t t;

  for (i = len - 1; i >= 0; i--)
    if ((t = VARR_GET (token_t, tokens, i))->code == T_RDBLNO) {
      j = i + 1;
      k = i - 1;
      assert (k >= 0 && j < len);
      if (VARR_GET (token_t, tokens, j)->code == ' ' || VARR_GET (token_t, tokens, j)->code == '\n')
        j++;
      if (VARR_GET (token_t, tokens, k)->code == ' ' || VARR_GET (token_t, tokens, k)->code == '\n')
        k--;
      assert (k >= 0 && j < len);
      empty_j_p = VARR_GET (token_t, tokens, j)->code == T_PLM;
      empty_k_p = VARR_GET (token_t, tokens, k)->code == T_PLM;
      if (empty_j_p || empty_k_p) {
        if (!empty_j_p)
          j--;
        else if (j + 1 < len
                 && (VARR_GET (token_t, tokens, j + 1)->code == ' '
                     || VARR_GET (token_t, tokens, j + 1)->code == '\n'))
          j++;
        if (!empty_k_p)
          k++;
        else if (k != 0
                 && (VARR_GET (token_t, tokens, k - 1)->code == ' '
                     || VARR_GET (token_t, tokens, k - 1)->code == '\n'))
          k--;
        if (!empty_j_p || !empty_k_p) {
          del_tokens (tokens, k, j - k + 1);
        } else {
          del_tokens (tokens, k, j - k);
          t = new_token (c2m_ctx, t->pos, "", ' ', N_IGNORE);
          VARR_SET (token_t, tokens, k, t);
        }
      } else {
        t = token_concat (c2m_ctx, VARR_GET (token_t, tokens, k), VARR_GET (token_t, tokens, j));
        del_tokens (tokens, k + 1, j - k);
        VARR_SET (token_t, tokens, k, t);
      }
      i = k;
      len = (int) VARR_LENGTH (token_t, tokens);
    }
  for (i = len - 1; i >= 0; i--) VARR_GET (token_t, tokens, i)->processed_p = TRUE;
  return tokens;
}

static void process_replacement (c2m_ctx_t c2m_ctx, macro_call_t mc) {
  macro_t m;
  token_t t, *m_repl;
  VARR (token_t) * arg;
  int i, m_repl_len, sharp_pos, copy_p;

  m = mc->macro;
  sharp_pos = -1;
  m_repl = VARR_ADDR (token_t, m->replacement);
  m_repl_len = (int) VARR_LENGTH (token_t, m->replacement);
  for (;;) {
    if (mc->repl_pos >= m_repl_len) {
      t = get_next_pptoken (c2m_ctx);
      unget_next_pptoken (c2m_ctx, t);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "# push back <%s>\n", get_token_str (t));
#endif
      unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOR, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "# push back <EOR>: mc=%lx\n", mc);
#endif
      push_back (c2m_ctx, do_concat (c2m_ctx, mc->repl_buffer));
      m->ignore_p = TRUE;
      return;
    }
    t = m_repl[mc->repl_pos++];
    copy_p = TRUE;
    if (t->code == T_ID) {
      i = find_param (m->params, t->repr);
      if (i >= 0) {
        arg = VARR_GET (token_arr_t, mc->args, i);
        if (sharp_pos >= 0) {
          del_tokens (mc->repl_buffer, sharp_pos, -1);
          if (VARR_LENGTH (token_t, arg) != 0
              && (VARR_GET (token_t, arg, 0)->code == ' '
                  || VARR_GET (token_t, arg, 0)->code == '\n'))
            del_tokens (arg, 0, 1);
          if (VARR_LENGTH (token_t, arg) != 0
              && (VARR_LAST (token_t, arg)->code == ' ' || VARR_LAST (token_t, arg)->code == '\n'))
            VARR_POP (token_t, arg);
          t = token_stringify (c2m_ctx, mc->macro->id, arg);
          copy_p = FALSE;
        } else if ((mc->repl_pos >= 2 && m_repl[mc->repl_pos - 2]->code == T_RDBLNO)
                   || (mc->repl_pos >= 3 && m_repl[mc->repl_pos - 2]->code == ' '
                       && m_repl[mc->repl_pos - 3]->code == T_RDBLNO)
                   || (mc->repl_pos < m_repl_len && m_repl[mc->repl_pos]->code == T_RDBLNO)
                   || (mc->repl_pos + 1 < m_repl_len && m_repl[mc->repl_pos + 1]->code == T_RDBLNO
                       && m_repl[mc->repl_pos]->code == ' ')) {
          if (VARR_LENGTH (token_t, arg) == 0
              || (VARR_LENGTH (token_t, arg) == 1
                  && (VARR_GET (token_t, arg, 0)->code == ' '
                      || VARR_GET (token_t, arg, 0)->code == '\n'))) {
            t = new_token (c2m_ctx, t->pos, "", T_PLM, N_IGNORE);
            copy_p = FALSE;
          } else {
            add_tokens (mc->repl_buffer, arg);
            continue;
          }
        } else {
          unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOA, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
          fprintf (stderr, "# push back <EOA> for macro %s call\n", mc->macro->id->repr);
#endif
          copy_and_push_back (c2m_ctx, arg, mc->pos);
          unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_BOA, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
          fprintf (stderr, "# push back <BOA> for macro %s call\n", mc->macro->id->repr);
#endif
          return;
        }
      }
    } else if (t->code == '#') {
      sharp_pos = (int) VARR_LENGTH (token_t, mc->repl_buffer);
    } else if (t->code != ' ') {
      sharp_pos = -1;
    }
    if (copy_p) t = copy_token (c2m_ctx, t, mc->pos);
    add_token (mc->repl_buffer, t);
  }
}

static void prepare_pragma_string (const char *repr, VARR (char) * to) {
  destringify (repr, to);
  reverse (to);
}

static int process_pragma (c2m_ctx_t c2m_ctx, token_t t) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  token_t t1, t2;

  if (strcmp (t->repr, "_Pragma") != 0) return FALSE;
  VARR_TRUNC (token_t, temp_tokens, 0);
  t1 = get_next_pptoken (c2m_ctx);
  VARR_PUSH (token_t, temp_tokens, t1);
  if (t1->code == ' ' || t1->code == '\n') {
    t1 = get_next_pptoken (c2m_ctx);
    VARR_PUSH (token_t, temp_tokens, t1);
  }
  if (t1->code != '(') {
    push_back (c2m_ctx, temp_tokens);
    return FALSE;
  }
  t1 = get_next_pptoken (c2m_ctx);
  VARR_PUSH (token_t, temp_tokens, t1);
  if (t1->code == ' ' || t1->code == '\n') {
    t1 = get_next_pptoken (c2m_ctx);
    VARR_PUSH (token_t, temp_tokens, t1);
  }
  if (t1->code != T_STR) {
    push_back (c2m_ctx, temp_tokens);
    return FALSE;
  }
  t2 = t1;
  t1 = get_next_pptoken (c2m_ctx);
  VARR_PUSH (token_t, temp_tokens, t1);
  if (t1->code == ' ' || t1->code == '\n') {
    t1 = get_next_pptoken (c2m_ctx);
    VARR_PUSH (token_t, temp_tokens, t1);
  }
  if (t1->code != ')') {
    push_back (c2m_ctx, temp_tokens);
    return FALSE;
  }
  set_string_stream (c2m_ctx, t2->repr, t2->pos, prepare_pragma_string);
  VARR_TRUNC (token_t, temp_tokens, 0);
  for (t1 = get_next_pptoken (c2m_ctx); t1->code != T_EOFILE; t1 = get_next_pptoken (c2m_ctx))
    VARR_PUSH (token_t, temp_tokens, t1);
  check_pragma (c2m_ctx, t2, temp_tokens);
  return TRUE;
}

static void flush_buffer (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;

  for (size_t i = 0; i < VARR_LENGTH (token_t, output_buffer); i++)
    pre_out_token_func (c2m_ctx, VARR_GET (token_t, output_buffer, i));
  VARR_TRUNC (token_t, output_buffer, 0);
}

static void out_token (c2m_ctx_t c2m_ctx, token_t t) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;

  if (no_out_p || VARR_LENGTH (macro_call_t, macro_call_stack) != 0) {
    VARR_PUSH (token_t, output_buffer, t);
    return;
  }
  flush_buffer (c2m_ctx);
  pre_out_token_func (c2m_ctx, t);
}

struct val {
  int uns_p;
  union {
    mir_llong i_val;
    mir_ullong u_val;
  } u;
};

static void move_tokens (VARR (token_t) * to, VARR (token_t) * from) {
  VARR_TRUNC (token_t, to, 0);
  for (size_t i = 0; i < VARR_LENGTH (token_t, from); i++)
    VARR_PUSH (token_t, to, VARR_GET (token_t, from, i));
  VARR_TRUNC (token_t, from, 0);
}

static void reverse_move_tokens (VARR (token_t) * to, VARR (token_t) * from) {
  VARR_TRUNC (token_t, to, 0);
  while (VARR_LENGTH (token_t, from) != 0) VARR_PUSH (token_t, to, VARR_POP (token_t, from));
}

static void transform_to_header (c2m_ctx_t c2m_ctx, VARR (token_t) * buffer) {
  size_t i, j, k;
  token_t t;
  pos_t pos;

  for (i = 0; i < VARR_LENGTH (token_t, buffer) && VARR_GET (token_t, buffer, i)->code == ' '; i++)
    ;
  if (i >= VARR_LENGTH (token_t, buffer)) return;
  if ((t = VARR_GET (token_t, buffer, i))->node_code != N_LT) return;
  pos = t->pos;
  for (j = i + 1;
       j < VARR_LENGTH (token_t, buffer) && VARR_GET (token_t, buffer, j)->node_code != N_GT; j++)
    ;
  if (j >= VARR_LENGTH (token_t, buffer)) return;
  VARR_TRUNC (char, symbol_text, 0);
  VARR_TRUNC (char, temp_string, 0);
  VARR_PUSH (char, symbol_text, '<');
  for (k = i + 1; k < j; k++) {
    t = VARR_GET (token_t, buffer, k);
    for (const char *s = t->repr; *s != 0; s++) {
      VARR_PUSH (char, symbol_text, *s);
      VARR_PUSH (char, temp_string, *s);
    }
  }
  VARR_PUSH (char, symbol_text, '>');
  VARR_PUSH (char, symbol_text, '\0');
  VARR_PUSH (char, temp_string, '\0');
  del_tokens (buffer, (int) i, (int) (j - i));
  t = new_node_token (c2m_ctx, pos, VARR_ADDR (char, symbol_text), T_HEADER,
                      new_str_node (c2m_ctx, N_STR,
                                    uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)), pos));
  VARR_SET (token_t, buffer, i, t);
}

static void processing (c2m_ctx_t c2m_ctx, int ignore_directive_p);

static struct val eval_expr (c2m_ctx_t c2m_ctx, VARR (token_t) * buffer, token_t if_token);

static const char *get_header_name (c2m_ctx_t c2m_ctx, VARR (token_t) * buffer, pos_t err_pos,
                                    const char **content) {
  size_t i;

  *content = NULL;
  transform_to_header (c2m_ctx, buffer);
  i = 0;
  if (VARR_LENGTH (token_t, buffer) != 0 && VARR_GET (token_t, buffer, 0)->code == ' ') i++;
  if (i != VARR_LENGTH (token_t, buffer) - 1
      || (VARR_GET (token_t, buffer, i)->code != T_STR
          && VARR_GET (token_t, buffer, i)->code != T_HEADER)) {
    error (c2m_ctx, err_pos, "wrong #include");
    return NULL;
  }
  return get_include_fname (c2m_ctx, VARR_GET (token_t, buffer, i), content);
}

static void process_directive (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  token_t t, t1;
  int true_p;
  VARR (token_t) * temp_buffer;
  pos_t pos;
  struct macro macro;
  macro_t tab_macro;
  const char *name;

  t = get_next_pptoken (c2m_ctx);
  if (t->code == '\n') return;
  if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
  if (t->code != T_ID) {
    if (!skip_if_part_p) error (c2m_ctx, t->pos, "wrong directive name %s", t->repr);
    skip_nl (c2m_ctx, NULL, NULL);
    return;
  }
  VARR_CREATE (token_t, temp_buffer, alloc, 64);
  if (strcmp (t->repr, "ifdef") == 0 || strcmp (t->repr, "ifndef") == 0) {
    t1 = t;
    if (VARR_LENGTH (ifstate_t, ifs) != 0 && VARR_LAST (ifstate_t, ifs)->skip_p) {
      skip_if_part_p = true_p = TRUE;
      skip_nl (c2m_ctx, NULL, NULL);
    } else {
      t = get_next_pptoken (c2m_ctx);
      skip_if_part_p = FALSE;
      if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
      if (t->code != T_ID) {
        error (c2m_ctx, t->pos, "wrong #%s", t1->repr);
      } else {
        macro.id = t;
        skip_if_part_p = HTAB_DO (macro_t, macro_tab, &macro, HTAB_FIND, tab_macro);
      }
      t = get_next_pptoken (c2m_ctx);
      if (t->code != '\n') {
        error (c2m_ctx, t1->pos, "garbage at the end of #%s", t1->repr);
        skip_nl (c2m_ctx, NULL, NULL);
      }
      if (strcmp (t1->repr, "ifdef") == 0) skip_if_part_p = !skip_if_part_p;
      true_p = !skip_if_part_p;
    }
    VARR_PUSH (ifstate_t, ifs, new_ifstate (skip_if_part_p, true_p, FALSE, t1->pos));
  } else if (strcmp (t->repr, "endif") == 0 || strcmp (t->repr, "else") == 0) {
    t1 = t;
    t = get_next_pptoken (c2m_ctx);
    if (t->code != '\n') {
      error (c2m_ctx, t1->pos, "garbage at the end of #%s", t1->repr);
      skip_nl (c2m_ctx, NULL, NULL);
    }
    if ((int) VARR_LENGTH (ifstate_t, ifs) <= cs->ifs_length_at_stream_start)
      error (c2m_ctx, t1->pos, "unmatched #%s", t1->repr);
    else if (strcmp (t1->repr, "endif") == 0) {
      pop_ifstate (c2m_ctx);
      skip_if_part_p = VARR_LENGTH (ifstate_t, ifs) == 0 ? 0 : VARR_LAST (ifstate_t, ifs)->skip_p;
    } else if (VARR_LAST (ifstate_t, ifs)->else_p) {
      error (c2m_ctx, t1->pos, "repeated #else");
      VARR_LAST (ifstate_t, ifs)->skip_p = 1;
      skip_if_part_p = TRUE;
    } else {
      skip_if_part_p = VARR_LAST (ifstate_t, ifs)->true_p;
      VARR_LAST (ifstate_t, ifs)->true_p = TRUE;
      VARR_LAST (ifstate_t, ifs)->skip_p = skip_if_part_p;
      VARR_LAST (ifstate_t, ifs)->else_p = FALSE;
    }
  } else if (strcmp (t->repr, "if") == 0 || strcmp (t->repr, "elif") == 0) {
    if_id = t;
    if (strcmp (t->repr, "elif") == 0 && VARR_LENGTH (ifstate_t, ifs) == 0) {
      error (c2m_ctx, t->pos, "#elif without #if");
    } else if (strcmp (t->repr, "elif") == 0 && VARR_LAST (ifstate_t, ifs)->else_p) {
      error (c2m_ctx, t->pos, "#elif after #else");
      skip_if_part_p = TRUE;
    } else if (strcmp (t->repr, "if") == 0 && VARR_LENGTH (ifstate_t, ifs) != 0
               && VARR_LAST (ifstate_t, ifs)->skip_p) {
      skip_if_part_p = true_p = TRUE;
      skip_nl (c2m_ctx, NULL, NULL);
      VARR_PUSH (ifstate_t, ifs, new_ifstate (skip_if_part_p, true_p, FALSE, t->pos));
    } else if (strcmp (t->repr, "elif") == 0 && VARR_LAST (ifstate_t, ifs)->true_p) {
      VARR_LAST (ifstate_t, ifs)->skip_p = skip_if_part_p = TRUE;
      skip_nl (c2m_ctx, NULL, NULL);
    } else {
      struct val val;

      skip_if_part_p = FALSE; /* for eval expr */
      skip_nl (c2m_ctx, NULL, temp_buffer);
      val = eval_expr (c2m_ctx, temp_buffer, t);
      true_p = val.uns_p ? val.u.u_val != 0 : val.u.i_val != 0;
      skip_if_part_p = !true_p;
      if (strcmp (t->repr, "if") == 0)
        VARR_PUSH (ifstate_t, ifs, new_ifstate (skip_if_part_p, true_p, FALSE, t->pos));
      else {
        VARR_LAST (ifstate_t, ifs)->skip_p = skip_if_part_p;
        VARR_LAST (ifstate_t, ifs)->true_p = true_p;
      }
    }
  } else if (skip_if_part_p) {
    skip_nl (c2m_ctx, NULL, NULL);
  } else if (strcmp (t->repr, "define") == 0) {
    define (c2m_ctx);
  } else if (strcmp (t->repr, "include") == 0) {
    const char *content;

    t = get_next_include_pptoken (c2m_ctx);
    if (t->code == ' ') t = get_next_include_pptoken (c2m_ctx);
    t1 = get_next_pptoken (c2m_ctx);
    if ((t->code == T_STR || t->code == T_HEADER) && t1->code == '\n')
      name = get_include_fname (c2m_ctx, t, &content);
    else {
      VARR_PUSH (token_t, temp_buffer, t);
      skip_nl (c2m_ctx, t1, temp_buffer);
      unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOP, N_IGNORE));
      push_back (c2m_ctx, temp_buffer);
      if (n_errors != 0) VARR_TRUNC (macro_call_t, macro_call_stack, 0); /* can be non-empty */
      assert (VARR_LENGTH (macro_call_t, macro_call_stack) == 0 && !no_out_p);
      no_out_p = TRUE;
      processing (c2m_ctx, TRUE);
      no_out_p = FALSE;
      move_tokens (temp_buffer, output_buffer);
      if ((name = get_header_name (c2m_ctx, temp_buffer, t->pos, &content)) == NULL) {
        error (c2m_ctx, t->pos, "wrong #include");
        goto ret;
      }
    }
    if ((int) VARR_LENGTH (stream_t, streams) >= max_nested_includes + 1) {
      error (c2m_ctx, t->pos, "more %d include levels", VARR_LENGTH (stream_t, streams) - 1);
      goto ret;
    }
    add_include_stream (c2m_ctx, name, content, t->pos);
  } else if (strcmp (t->repr, "line") == 0) {
    skip_nl (c2m_ctx, NULL, temp_buffer);
    unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOP, N_IGNORE));
    push_back (c2m_ctx, temp_buffer);
    if (n_errors != 0) VARR_TRUNC (macro_call_t, macro_call_stack, 0); /* can be non-empty */
    assert (VARR_LENGTH (macro_call_t, macro_call_stack) == 0 && !no_out_p);
    no_out_p = TRUE;
    processing (c2m_ctx, TRUE);
    no_out_p = 0;
    move_tokens (temp_buffer, output_buffer);
    pos = check_line_directive_args (c2m_ctx, temp_buffer);
    if (pos.lno < 0) {
      error (c2m_ctx, t->pos, "wrong #line");
    } else {
      change_stream_pos (c2m_ctx, pos);
    }
  } else if (strcmp (t->repr, "error") == 0) {
    VARR_TRUNC (char, temp_string, 0);
    add_to_temp_string (c2m_ctx, "#error");
    for (t1 = get_next_pptoken (c2m_ctx); t1->code != '\n'; t1 = get_next_pptoken (c2m_ctx))
      add_to_temp_string (c2m_ctx, t1->repr);
    error (c2m_ctx, t->pos, "%s", VARR_ADDR (char, temp_string));
  } else if (!c2m_options->pedantic_p && strcmp (t->repr, "warning") == 0) {
    VARR_TRUNC (char, temp_string, 0);
    add_to_temp_string (c2m_ctx, "#warning");
    for (t1 = get_next_pptoken (c2m_ctx); t1->code != '\n'; t1 = get_next_pptoken (c2m_ctx))
      add_to_temp_string (c2m_ctx, t1->repr);
    warning (c2m_ctx, t->pos, "%s", VARR_ADDR (char, temp_string));
  } else if (strcmp (t->repr, "pragma") == 0) {
    skip_nl (c2m_ctx, NULL, temp_buffer);
    check_pragma (c2m_ctx, t, temp_buffer);
  } else if (strcmp (t->repr, "undef") == 0) {
    t = get_next_pptoken (c2m_ctx);
    if (t->code == ' ') t = get_next_pptoken (c2m_ctx);
    if (t->code == '\n') {
      error (c2m_ctx, t->pos, "no ident after #undef");
      goto ret;
    }
    if (t->code != T_ID) {
      error (c2m_ctx, t->pos, "no ident after #undef");
      skip_nl (c2m_ctx, t, NULL);
      goto ret;
    }
    if (strcmp (t->repr, "defined") == 0) {
      error (c2m_ctx, t->pos, "#undef of %s", t->repr);
    } else {
      macro.id = t;
      if (HTAB_DO (macro_t, macro_tab, &macro, HTAB_FIND, tab_macro)) {
        if (tab_macro->replacement == NULL)
          error (c2m_ctx, t->pos, "#undef of standard macro %s", t->repr);
        else
          HTAB_DO (macro_t, macro_tab, &macro, HTAB_DELETE, tab_macro);
      }
    }
  }
ret:
  VARR_DESTROY (token_t, temp_buffer);
}

static int pre_match (c2m_ctx_t c2m_ctx, int c, pos_t *pos, node_code_t *node_code, node_t *node) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  token_t t;

  if (VARR_LENGTH (token_t, pre_expr) == 0) return FALSE;
  t = VARR_LAST (token_t, pre_expr);
  if (t->code != c) return FALSE;
  if (pos != NULL) *pos = t->pos;
  if (node_code != NULL) *node_code = t->node_code;
  if (node != NULL) *node = t->node;
  VARR_POP (token_t, pre_expr);
  return TRUE;
}

static node_t pre_cond_expr (c2m_ctx_t c2m_ctx);

/* Expressions: */
static node_t pre_primary_expr (c2m_ctx_t c2m_ctx) {
  node_t r, n;

  if (pre_match (c2m_ctx, T_CH, NULL, NULL, &r)) return r;
  if (pre_match (c2m_ctx, T_NUMBER, NULL, NULL, &n)) {
    if (!pre_match (c2m_ctx, '(', NULL, NULL, NULL)) return n;
    if (!pre_match (c2m_ctx, ')', NULL, NULL, NULL)) {
      for (;;) {
        if ((r = pre_cond_expr (c2m_ctx)) == NULL) return NULL;
        if (pre_match (c2m_ctx, ')', NULL, NULL, NULL)) break;
        if (!pre_match (c2m_ctx, ',', NULL, NULL, NULL)) return NULL;
      }
    }
    return new_pos_node (c2m_ctx, N_IGNORE, POS (n)); /* error only during evaluation */
  }
  if (pre_match (c2m_ctx, '(', NULL, NULL, NULL)) {
    if ((r = pre_cond_expr (c2m_ctx)) == NULL) return NULL;
    if (pre_match (c2m_ctx, ')', NULL, NULL, NULL)) return r;
  }
  return NULL;
}

static node_t pre_unary_expr (c2m_ctx_t c2m_ctx) {
  node_t r;
  node_code_t code;
  pos_t pos;

  if (!pre_match (c2m_ctx, T_UNOP, &pos, &code, NULL)
      && !pre_match (c2m_ctx, T_ADDOP, &pos, &code, NULL))
    return pre_primary_expr (c2m_ctx);
  if ((r = pre_unary_expr (c2m_ctx)) == NULL) return r;
  r = new_pos_node1 (c2m_ctx, code, pos, r);
  return r;
}

static node_t pre_left_op (c2m_ctx_t c2m_ctx, int token, int token2,
                           node_t (*f) (c2m_ctx_t c2m_ctx)) {
  node_code_t code;
  node_t r, n;
  pos_t pos;

  if ((r = f (c2m_ctx)) == NULL) return r;
  while (pre_match (c2m_ctx, token, &pos, &code, NULL)
         || (token2 >= 0 && pre_match (c2m_ctx, token2, &pos, &code, NULL))) {
    n = new_pos_node1 (c2m_ctx, code, pos, r);
    if ((r = f (c2m_ctx)) == NULL) return r;
    op_append (c2m_ctx, n, r);
    r = n;
  }
  return r;
}

static node_t pre_mul_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_DIVOP, '*', pre_unary_expr);
}
static node_t pre_add_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_ADDOP, -1, pre_mul_expr);
}
static node_t pre_sh_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_SH, -1, pre_add_expr);
}
static node_t pre_rel_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_CMP, -1, pre_sh_expr);
}
static node_t pre_eq_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_EQNE, -1, pre_rel_expr);
}
static node_t pre_and_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, '&', -1, pre_eq_expr);
}
static node_t pre_xor_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, '^', -1, pre_and_expr);
}
static node_t pre_or_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, '|', -1, pre_xor_expr);
}
static node_t pre_land_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_ANDAND, -1, pre_or_expr);
}
static node_t pre_lor_expr (c2m_ctx_t c2m_ctx) {
  return pre_left_op (c2m_ctx, T_OROR, -1, pre_land_expr);
}

static node_t pre_cond_expr (c2m_ctx_t c2m_ctx) {
  node_t r, n;
  pos_t pos;

  if ((r = pre_lor_expr (c2m_ctx)) == NULL) return r;
  if (!pre_match (c2m_ctx, '?', &pos, NULL, NULL)) return r;
  n = new_pos_node1 (c2m_ctx, N_COND, pos, r);
  if ((r = pre_cond_expr (c2m_ctx)) == NULL) return r;
  op_append (c2m_ctx, n, r);
  if (!pre_match (c2m_ctx, ':', NULL, NULL, NULL)) return NULL;
  if ((r = pre_cond_expr (c2m_ctx)) == NULL) return r;
  op_append (c2m_ctx, n, r);
  return n;
}

static node_t parse_pre_expr (c2m_ctx_t c2m_ctx, VARR (token_t) * expr) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  node_t r;
  token_t t;

  pre_expr = expr;
  t = VARR_LAST (token_t, expr);
  if ((r = pre_cond_expr (c2m_ctx)) != NULL && VARR_LENGTH (token_t, expr) == 0) return r;
  if (VARR_LENGTH (token_t, expr) != 0) t = VARR_POP (token_t, expr);
  error (c2m_ctx, t->pos, "wrong preprocessor expression");
  return NULL;
}

static void replace_defined (c2m_ctx_t c2m_ctx, VARR (token_t) * expr_buffer) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  size_t i, j, k, len;
  token_t t, id;
  const char *res;
  struct macro macro_struct;
  macro_t tab_macro;

  for (i = 0; i < VARR_LENGTH (token_t, expr_buffer); i++) {
    /* Change defined ident and defined (ident) */
    t = VARR_GET (token_t, expr_buffer, i);
    if (t->code == T_ID && strcmp (t->repr, "defined") == 0) {
      j = i + 1;
      len = VARR_LENGTH (token_t, expr_buffer);
      if (j < len && VARR_GET (token_t, expr_buffer, j)->code == ' ') j++;
      if (j >= len) continue;
      if ((id = VARR_GET (token_t, expr_buffer, j))->code == T_ID) {
        macro_struct.id = id;
        res = HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, tab_macro) ? "1" : "0";
        VARR_SET (token_t, expr_buffer, i,
                  new_token (c2m_ctx, t->pos, res, T_NUMBER, N_IGNORE));  // ???
        del_tokens (expr_buffer, (int) i + 1, (int) (j - i));
        continue;
      }
      if (j >= len || VARR_GET (token_t, expr_buffer, j)->code != '(') continue;
      j++;
      if (j < len && VARR_GET (token_t, expr_buffer, j)->code == ' ') j++;
      if (j >= len || VARR_GET (token_t, expr_buffer, j)->code != T_ID) continue;
      k = j;
      j++;
      if (j < len && VARR_GET (token_t, expr_buffer, j)->code == ' ') j++;
      if (j >= len || VARR_GET (token_t, expr_buffer, j)->code != ')') continue;
      macro_struct.id = VARR_GET (token_t, expr_buffer, k);
      res = HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, tab_macro) ? "1" : "0";
      VARR_SET (token_t, expr_buffer, i,
                new_token (c2m_ctx, t->pos, res, T_NUMBER, N_IGNORE));  // ???
      del_tokens (expr_buffer, (int) i + 1, (int) (j - i));
    }
  }
}

static struct val eval (c2m_ctx_t c2m_ctx, node_t tree);

static struct val eval_expr (c2m_ctx_t c2m_ctx, VARR (token_t) * expr_buffer, token_t if_token) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  size_t i, j;
  token_t t, ppt;
  VARR (token_t) * temp_buffer;
  node_t tree;

  replace_defined (c2m_ctx, expr_buffer);
  if (VARR_LENGTH (macro_call_t, macro_call_stack) != 0)
    error (c2m_ctx, if_token->pos, "#if/#elif inside a macro call");
  assert (VARR_LENGTH (token_t, output_buffer) == 0 && !no_out_p);
  /* macro substitution */
  unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, if_token->pos, "", T_EOP, N_IGNORE));
  push_back (c2m_ctx, expr_buffer);
  no_out_p = TRUE;
  processing (c2m_ctx, TRUE);
  replace_defined (c2m_ctx, output_buffer);
  no_out_p = FALSE;
  reverse_move_tokens (expr_buffer, output_buffer);
  VARR_CREATE (token_t, temp_buffer, alloc, VARR_LENGTH (token_t, expr_buffer));
  for (i = j = 0; i < VARR_LENGTH (token_t, expr_buffer); i++) {
    int change_p = TRUE;

    /* changing PP tokens to tokens and idents to "0" */
    ppt = VARR_GET (token_t, expr_buffer, i);
    t = pptoken2token (c2m_ctx, ppt, FALSE);
    if (t == NULL || t->code == ' ' || t->code == '\n') continue;
    if (t->code == T_NUMBER
        && (t->node->code == N_F || t->node->code == N_D || t->node->code == N_LD)) {
      error (c2m_ctx, ppt->pos, "floating point in #if/#elif: %s", ppt->repr);
    } else if (t->code == T_STR) {
      error (c2m_ctx, ppt->pos, "string in #if/#elif: %s", ppt->repr);
    } else if (t->code != T_ID) {
      change_p = FALSE;
    }
    if (change_p)
      t = new_node_token (c2m_ctx, ppt->pos, "0", T_NUMBER, new_ll_node (c2m_ctx, 0, ppt->pos));
    VARR_PUSH (token_t, temp_buffer, t);
  }
  no_out_p = TRUE;
  if (VARR_LENGTH (token_t, temp_buffer) != 0) {
    tree = parse_pre_expr (c2m_ctx, temp_buffer);
  } else {
    error (c2m_ctx, if_token->pos, "empty preprocessor expression");
    tree = NULL;
  }
  no_out_p = FALSE;
  VARR_DESTROY (token_t, temp_buffer);
  if (tree == NULL) {
    struct val val;

    val.uns_p = FALSE;
    val.u.i_val = 0;
    return val;
  }
  return eval (c2m_ctx, tree);
}

static int eval_binop_operands (c2m_ctx_t c2m_ctx, node_t tree, struct val *v1, struct val *v2) {
  *v1 = eval (c2m_ctx, NL_HEAD (tree->u.ops));
  *v2 = eval (c2m_ctx, NL_EL (tree->u.ops, 1));
  if (v1->uns_p && !v2->uns_p) {
    v2->uns_p = TRUE;
    v2->u.u_val = v2->u.i_val;
  } else if (!v1->uns_p && v2->uns_p) {
    v1->uns_p = TRUE;
    v1->u.u_val = v1->u.i_val;
  }
  return v1->uns_p;
}

static struct val eval (c2m_ctx_t c2m_ctx, node_t tree) {
  int cond;
  struct val res, v1, v2;

#define UNOP(op)                                \
  do {                                          \
    v1 = eval (c2m_ctx, NL_HEAD (tree->u.ops)); \
    res = v1;                                   \
    if (res.uns_p)                              \
      res.u.u_val = op res.u.u_val;             \
    else                                        \
      res.u.i_val = op res.u.i_val;             \
  } while (0)

#define BINOP(op)                                              \
  do {                                                         \
    res.uns_p = eval_binop_operands (c2m_ctx, tree, &v1, &v2); \
    if (res.uns_p)                                             \
      res.u.u_val = v1.u.u_val op v2.u.u_val;                  \
    else                                                       \
      res.u.i_val = v1.u.i_val op v2.u.i_val;                  \
  } while (0)

  switch (tree->code) {
  case N_IGNORE:
    error (c2m_ctx, POS (tree), "wrong preprocessor expression");
    res.uns_p = FALSE;
    res.u.i_val = 0;
    break;
  case N_CH:
    res.uns_p = !char_is_signed_p () || MIR_CHAR_MAX > MIR_INT_MAX;
    if (res.uns_p)
      res.u.u_val = tree->u.ch;
    else
      res.u.i_val = tree->u.ch;
    break;
  case N_CH16:
  case N_CH32:
    res.uns_p = TRUE;
    res.u.u_val = tree->u.ul;
    break;
  case N_I:
  case N_L:
    res.uns_p = FALSE;
    res.u.i_val = tree->u.l;
    break;
  case N_LL:
    res.uns_p = FALSE;
    res.u.i_val = tree->u.ll;
    break;
  case N_U:
  case N_UL:
    res.uns_p = TRUE;
    res.u.u_val = tree->u.ul;
    break;
  case N_ULL:
    res.uns_p = TRUE;
    res.u.u_val = tree->u.ull;
    break;
  case N_BITWISE_NOT: UNOP (~); break;
  case N_NOT: UNOP (!); break;
  case N_EQ: BINOP (==); break;
  case N_NE: BINOP (!=); break;
  case N_LT: BINOP (<); break;
  case N_LE: BINOP (<=); break;
  case N_GT: BINOP (>); break;
  case N_GE: BINOP (>=); break;
  case N_ADD:
    if (NL_EL (tree->u.ops, 1) == NULL) {
      UNOP (+);
    } else {
      BINOP (+);
    }
    break;
  case N_SUB:
    if (NL_EL (tree->u.ops, 1) == NULL) {
      UNOP (-);
    } else {
      BINOP (-);
    }
    break;
  case N_AND: BINOP (&); break;
  case N_OR: BINOP (|); break;
  case N_XOR: BINOP (^); break;
  case N_LSH: BINOP (<<); break;
  case N_RSH: BINOP (>>); break;
  case N_MUL: BINOP (*); break;
  case N_DIV:
  case N_MOD: {
    int zero_p;

    res.uns_p = eval_binop_operands (c2m_ctx, tree, &v1, &v2);
    if (res.uns_p) {
      res.u.u_val = ((zero_p = v2.u.u_val == 0) ? 1
                     : tree->code == N_DIV      ? v1.u.u_val / v2.u.u_val
                                                : v1.u.u_val % v2.u.u_val);
    } else {
      res.u.i_val = ((zero_p = v2.u.i_val == 0) ? 1
                     : tree->code == N_DIV      ? v1.u.i_val / v2.u.i_val
                                                : v1.u.i_val % v2.u.i_val);
    }
    if (zero_p)
      error (c2m_ctx, POS (tree), "division (%s) by zero in preporocessor",
             tree->code == N_DIV ? "/" : "%");
    break;
  }
  case N_ANDAND:
  case N_OROR:
    v1 = eval (c2m_ctx, NL_HEAD (tree->u.ops));
    cond = v1.uns_p ? v1.u.u_val != 0 : v1.u.i_val != 0;
    if (tree->code == N_ANDAND ? cond : !cond) {
      v2 = eval (c2m_ctx, NL_EL (tree->u.ops, 1));
      cond = v2.uns_p ? v2.u.u_val != 0 : v2.u.i_val != 0;
    }
    res.uns_p = FALSE;
    res.u.i_val = cond;
    break;
  case N_COND:
    v1 = eval (c2m_ctx, NL_HEAD (tree->u.ops));
    cond = v1.uns_p ? v1.u.u_val != 0 : v1.u.i_val != 0;
    res = eval (c2m_ctx, NL_EL (tree->u.ops, cond ? 1 : 2));
    break;
  default:
    res.uns_p = FALSE;
    res.u.i_val = 0;
    assert (FALSE);
  }
  return res;
}

static macro_call_t try_param_macro_call (c2m_ctx_t c2m_ctx, macro_t m, token_t macro_id) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  macro_call_t mc;
  token_t t1 = get_next_pptoken (c2m_ctx), t2 = NULL;

  while (t1->code == T_EOR) {
    pop_macro_call (c2m_ctx);
    t1 = get_next_pptoken (c2m_ctx);
  }
  if (t1->code == ' ' || t1->code == '\n') {
    t2 = t1;
    t1 = get_next_pptoken (c2m_ctx);
  }
  if (t1->code != '(') { /* no args: it is not a macro call */
    unget_next_pptoken (c2m_ctx, t1);
    if (t2 != NULL) unget_next_pptoken (c2m_ctx, t2);
    out_token (c2m_ctx, macro_id);
    return NULL;
  }
  mc = new_macro_call (alloc, m, macro_id->pos);
  find_args (c2m_ctx, mc);
  VARR_PUSH (macro_call_t, macro_call_stack, mc);
  return mc;
}

#define ADD_OVERFLOW "__builtin_add_overflow"
#define SUB_OVERFLOW "__builtin_sub_overflow"
#define MUL_OVERFLOW "__builtin_mul_overflow"
#define EXPECT "__builtin_expect"
#define JCALL "__builtin_jcall"
#define JRET "__builtin_jret"
#define PROP_SET "__builtin_prop_set"
#define PROP_EQ "__builtin_prop_eq"
#define PROP_NE "__builtin_prop_ne"

static void processing (c2m_ctx_t c2m_ctx, int ignore_directive_p) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  token_t t;
  struct macro macro_struct;
  macro_t m;
  macro_call_t mc;
  int newln_p;

  for (newln_p = TRUE;;) { /* Main loop. */
    t = get_next_pptoken (c2m_ctx);
    if (t->code == T_EOP) return; /* end of processing */
    if (newln_p && !ignore_directive_p && t->code == '#') {
      process_directive (c2m_ctx);
      continue;
    }
    if (t->code == '\n') {
      newln_p = TRUE;
      out_token (c2m_ctx, t);
      continue;
    } else if (t->code == ' ') {
      out_token (c2m_ctx, t);
      continue;
    } else if (t->code == T_EOFILE || t->code == T_EOU) {
      if ((int) VARR_LENGTH (ifstate_t, ifs)
          > (eof_s == NULL ? 0 : eof_s->ifs_length_at_stream_start)) {
        error (c2m_ctx, VARR_LAST (ifstate_t, ifs)->if_pos, "unfinished #if");
      }
      if (t->code == T_EOU) return;
      while ((int) VARR_LENGTH (ifstate_t, ifs) > eof_s->ifs_length_at_stream_start)
        pop_ifstate (c2m_ctx);
      skip_if_part_p = VARR_LENGTH (ifstate_t, ifs) == 0 ? 0 : VARR_LAST (ifstate_t, ifs)->skip_p;
      newln_p = TRUE;
      continue;
    } else if (skip_if_part_p) {
      skip_nl (c2m_ctx, t, NULL);
      newln_p = TRUE;
      continue;
    }
    newln_p = FALSE;
    if (t->code == T_EOR) {  // finish macro call
      pop_macro_call (c2m_ctx);
      continue;
    } else if (t->code == T_EOA) { /* arg end: add the result to repl_buffer */
      mc = VARR_LAST (macro_call_t, macro_call_stack);
      add_arg_tokens (mc->repl_buffer, output_buffer);
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "adding processed arg to output buffer\n");
#endif
      process_replacement (c2m_ctx, mc);
      continue;
    } else if (t->code != T_ID) {
      out_token (c2m_ctx, t);
      continue;
    }
    macro_struct.id = t;
    if (!HTAB_DO (macro_t, macro_tab, &macro_struct, HTAB_FIND, m)) {
      if (!process_pragma (c2m_ctx, t)) out_token (c2m_ctx, t);
      continue;
    }
    if (m->replacement == NULL) { /* standard macro */
      if (strcmp (t->repr, "__STDC__") == 0) {
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, "1", T_NUMBER,
                                            new_i_node (c2m_ctx, 1, t->pos)));
      } else if (strcmp (t->repr, "__STDC_HOSTED__") == 0) {
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, "1", T_NUMBER,
                                            new_i_node (c2m_ctx, 1, t->pos)));
      } else if (strcmp (t->repr, "__STDC_VERSION__") == 0) {
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, "201112L", T_NUMBER,
                                            new_l_node (c2m_ctx, 201112, t->pos)));  // ???
      } else if (strcmp (t->repr, "__FILE__") == 0) {
        stringify (t->pos.fname, temp_string);
        VARR_PUSH (char, temp_string, '\0');
        t = new_node_token (c2m_ctx, t->pos, VARR_ADDR (char, temp_string), T_STR,
                            new_str_node (c2m_ctx, N_STR, empty_str, t->pos));
        set_string_val (c2m_ctx, t, temp_string, ' ');
        out_token (c2m_ctx, t);
      } else if (strcmp (t->repr, "__LINE__") == 0) {
        char str[50];

        sprintf (str, "%d", t->pos.lno);
        out_token (c2m_ctx, new_node_token (c2m_ctx, t->pos, str, T_NUMBER,
                                            new_i_node (c2m_ctx, t->pos.lno, t->pos)));
      } else if (strcmp (t->repr, "__DATE__") == 0) {
        t = new_node_token (c2m_ctx, t->pos, date_str_repr, T_STR,
                            new_str_node (c2m_ctx, N_STR, uniq_cstr (c2m_ctx, date_str), t->pos));
        out_token (c2m_ctx, t);
      } else if (strcmp (t->repr, "__TIME__") == 0) {
        t = new_node_token (c2m_ctx, t->pos, time_str_repr, T_STR,
                            new_str_node (c2m_ctx, N_STR, uniq_cstr (c2m_ctx, time_str), t->pos));
        out_token (c2m_ctx, t);
      } else if (strcmp (t->repr, "__has_include") == 0) {
        int res;
        VARR (token_t) * arg;
        const char *name, *content;
        FILE *f;

        if ((mc = try_param_macro_call (c2m_ctx, m, t)) != NULL) {
          unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOR, N_IGNORE));
          if (VARR_LENGTH (token_arr_t, mc->args) != 1) {
            res = 0;
          } else {
            arg = VARR_LAST (token_arr_t, mc->args);
            if ((name = get_header_name (c2m_ctx, arg, t->pos, &content)) != NULL) {
              res = content != NULL || ((f = fopen (name, "r")) != NULL && !fclose (f)) ? 1 : 0;
            } else {
              error (c2m_ctx, t->pos, "wrong arg of predefined __has_include");
              res = 0;
            }
          }
          m->ignore_p = TRUE;
          unget_next_pptoken (c2m_ctx, new_node_token (c2m_ctx, t->pos, res ? "1" : "0", T_NUMBER,
                                                       new_i_node (c2m_ctx, res, t->pos)));
        }
      } else if (strcmp (t->repr, "__has_builtin") == 0) {
        int res;
        size_t i, len;
        VARR (token_t) * arg;

        res = 0;
        if ((mc = try_param_macro_call (c2m_ctx, m, t)) != NULL) {
          unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOR, N_IGNORE));
          if (VARR_LENGTH (token_arr_t, mc->args) != 1) {
            error (c2m_ctx, t->pos, "wrong number of args for __has_builtin");
          } else {
            arg = VARR_LAST (token_arr_t, mc->args);
            len = VARR_LENGTH (token_t, arg);
            i = 0;
            if (i < len && VARR_GET (token_t, arg, i)->code == ' ') i++;
            if (i >= len || (t = VARR_GET (token_t, arg, i))->code != T_ID) {
              error (c2m_ctx, t->pos, "__has_builtin requires identifier");
            } else {
              i++;
              if (i < len && VARR_GET (token_t, arg, i)->code == ' ') i++;
              if (i != len)
                error (c2m_ctx, t->pos, "garbage after identifier in __has_builtin");
              else
                res = (strcmp (t->repr, ADD_OVERFLOW) == 0 || strcmp (t->repr, SUB_OVERFLOW) == 0
                       || strcmp (t->repr, MUL_OVERFLOW) == 0 || strcmp (t->repr, EXPECT) == 0
                       || strcmp (t->repr, JCALL) == 0 || strcmp (t->repr, JRET) == 0
                       || strcmp (t->repr, PROP_SET) == 0 || strcmp (t->repr, PROP_EQ) == 0
                       || strcmp (t->repr, PROP_NE) == 0);
            }
          }
          m->ignore_p = TRUE;
          unget_next_pptoken (c2m_ctx, new_node_token (c2m_ctx, t->pos, res ? "1" : "0", T_NUMBER,
                                                       new_i_node (c2m_ctx, res, t->pos)));
        }
      } else {
        assert (FALSE);
      }
      continue;
    }
    if (m->ignore_p) {
      t->code = T_NO_MACRO_IDENT;
      out_token (c2m_ctx, t);
      continue;
    }
    if (m->params == NULL) { /* macro without parameters */
      unget_next_pptoken (c2m_ctx, new_token (c2m_ctx, t->pos, "", T_EOR, N_IGNORE));
#ifdef C2MIR_PREPRO_DEBUG
      fprintf (stderr, "# push back <EOR>\n");
#endif
      mc = new_macro_call (alloc, m, t->pos);
      add_tokens (mc->repl_buffer, m->replacement);
      copy_and_push_back (c2m_ctx, do_concat (c2m_ctx, mc->repl_buffer), mc->pos);
      m->ignore_p = TRUE;
      VARR_PUSH (macro_call_t, macro_call_stack, mc);
    } else if ((mc = try_param_macro_call (c2m_ctx, m, t)) != NULL) { /* macro with parameters */
      process_replacement (c2m_ctx, mc);
    }
  }
}

static void pre_text_out (c2m_ctx_t c2m_ctx, token_t t) { /* NULL means end of output */
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  int i;
  FILE *f = c2m_options->prepro_output_file;

  if (t == NULL && pre_last_token != NULL && pre_last_token->code == '\n') {
    fprintf (f, "\n");
    return;
  }
  if (t->code == '\n') {
    pre_last_token = t;
    return;
  }
  if (actual_pre_pos.fname != t->pos.fname || actual_pre_pos.lno != t->pos.lno) {
    if (actual_pre_pos.fname == t->pos.fname && actual_pre_pos.lno < t->pos.lno
        && actual_pre_pos.lno + 4 >= t->pos.lno) {
      for (; actual_pre_pos.lno != t->pos.lno; actual_pre_pos.lno++) fprintf (f, "\n");
    } else {
      if (pre_last_token != NULL) fprintf (f, "\n");
      fprintf (f, "#line %d", t->pos.lno);
      if (actual_pre_pos.fname != t->pos.fname) {
        stringify (t->pos.fname, temp_string);
        VARR_PUSH (char, temp_string, '\0');
        fprintf (f, " %s", VARR_ADDR (char, temp_string));
      }
      fprintf (f, "\n");
    }
    for (i = 0; i < t->pos.ln_pos - 1; i++) fprintf (f, " ");
    actual_pre_pos = t->pos;
  }
  fprintf (f, "%s", t->code == ' ' ? " " : t->repr);
  pre_last_token = t;
}

static void pre_out (c2m_ctx_t c2m_ctx, token_t t) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;

  if (t == NULL) {
    t = new_token (c2m_ctx, pre_last_token == NULL ? no_pos : pre_last_token->pos, "<EOF>",
                   T_EOFILE, N_IGNORE);
  } else {
    assert (t->code != T_EOU && t->code != EOF);
    pre_last_token = t;
    if ((t = pptoken2token (c2m_ctx, t, TRUE)) == NULL) return;
  }
  if (t->code == T_STR && VARR_LENGTH (token_t, recorded_tokens) != 0
      && VARR_LAST (token_t, recorded_tokens)->code == T_STR) { /* concat strings */
    token_t last_t = VARR_POP (token_t, recorded_tokens);
    int type = ' ', last_t_quot_off = 0, t_quot_off = 0, err_p = FALSE;
    const char *s;

    VARR_TRUNC (char, temp_string, 0);
    if (last_t->repr[0] == 'u' && last_t->repr[1] == '8') {
      err_p = t->repr[0] != '\"' && (t->repr[0] != 'u' || t->repr[1] != '8');
      last_t_quot_off = 2;
    } else if (last_t->repr[0] == 'L' || last_t->repr[0] == 'u' || last_t->repr[0] == 'U') {
      err_p = t->repr[0] != '\"' && (t->repr[0] != last_t->repr[0] || t->repr[1] == '8');
      last_t_quot_off = 1;
    }
    if (t->repr[0] == 'u' && t->repr[1] == '8') {
      err_p = last_t->repr[0] != '\"' && (last_t->repr[0] != 'u' || last_t->repr[1] != '8');
      t_quot_off = 2;
    } else if (t->repr[0] == 'L' || t->repr[0] == 'u' || t->repr[0] == 'U') {
      err_p = last_t->repr[0] != '\"' && (t->repr[0] != last_t->repr[0] || last_t->repr[1] == '8');
      t_quot_off = 1;
    }
    if (err_p) error (c2m_ctx, t->pos, "concatenation of different type string literals");
    if (sizeof (mir_wchar) == 4 && (last_t->repr[0] == 'L' || t->repr[0] == 'L')) {
      type = 'L';
    } else if (last_t->repr[0] == 'U' || t->repr[0] == 'U') {
      type = 'U';
    } else if (last_t->repr[0] == 'L' || t->repr[0] == 'L') {
      type = 'L';
    } else if ((last_t->repr[0] == 'u' && last_t->repr[1] == '8')
               || (t->repr[0] == 'u' && t->repr[1] == '8')) {
      VARR_PUSH (char, temp_string, 'u');
      type = '8';
    } else if ((last_t->repr[0] == 'u' || t->repr[0] == 'u')) {
      type = 'u';
    }
    if (type != ' ') VARR_PUSH (char, temp_string, type);
    for (s = last_t->repr + last_t_quot_off; *s != 0; s++) VARR_PUSH (char, temp_string, *s);
    assert (VARR_LAST (char, temp_string) == '"');
    VARR_POP (char, temp_string);
    for (s = t->repr + t_quot_off + 1; *s != 0; s++) VARR_PUSH (char, temp_string, *s);
    t = last_t;
    assert (VARR_LAST (char, temp_string) == '"');
    VARR_PUSH (char, temp_string, '\0');
    t->repr = uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
    set_string_val (c2m_ctx, t, temp_string, type);
  }
  VARR_PUSH (token_t, recorded_tokens, t);
}

static void common_pre_out (c2m_ctx_t c2m_ctx, token_t t) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;

  pptokens_num++;
  (c2m_options->prepro_only_p ? pre_text_out : pre_out) (c2m_ctx, t);
}

static void pre (c2m_ctx_t c2m_ctx) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;

  pre_last_token = NULL;
  actual_pre_pos.fname = NULL;
  actual_pre_pos.lno = 0;
  actual_pre_pos.ln_pos = 0;
  pre_out_token_func = common_pre_out;
  pptokens_num = 0;
  VARR_TRUNC (char_ptr_t, once_include_files, 0);
  if (!c2m_options->no_prepro_p) {
    processing (c2m_ctx, FALSE);
  } else {
    for (;;) {
      token_t t = get_next_pptoken (c2m_ctx);

      if (t->code == T_EOFILE || t->code == T_EOU) break;
      pre_out_token_func (c2m_ctx, t);
    }
  }
  pre_out_token_func (c2m_ctx, NULL);
  if (c2m_options->verbose_p && c2m_options->message_file != NULL)
    fprintf (c2m_options->message_file, "    preprocessor tokens -- %lu, parse tokens -- %lu\n",
             pptokens_num, (unsigned long) VARR_LENGTH (token_t, recorded_tokens));
}

/* ------------------------- Preprocessor End ------------------------------ */

typedef struct {
  node_t id, scope;
  int typedef_p;
} tpname_t;

DEF_HTAB (tpname_t);

struct parse_ctx {
  int record_level;
  size_t next_token_index;
  token_t curr_token;
  node_t curr_scope;
  HTAB (tpname_t) * tpname_tab;
};

#define record_level parse_ctx->record_level
#define next_token_index parse_ctx->next_token_index
#define next_token_index parse_ctx->next_token_index
#define curr_token parse_ctx->curr_token
#define curr_scope parse_ctx->curr_scope
#define tpname_tab parse_ctx->tpname_tab

static struct node err_struct;
static const node_t err_node = &err_struct;

static void read_token (c2m_ctx_t c2m_ctx) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  curr_token = VARR_GET (token_t, recorded_tokens, next_token_index);
  next_token_index++;
}

static size_t record_start (c2m_ctx_t c2m_ctx) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  assert (next_token_index > 0 && record_level >= 0);
  record_level++;
  return next_token_index - 1;
}

static void record_stop (c2m_ctx_t c2m_ctx, size_t mark, int restore_p) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  assert (record_level > 0);
  record_level--;
  if (!restore_p) return;
  next_token_index = mark;
  read_token (c2m_ctx);
}

static void syntax_error (c2m_ctx_t c2m_ctx, const char *expected_name) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  FILE *f;

  if ((f = c2m_options->message_file) == NULL) return;
  print_pos (f, curr_token->pos, TRUE);
  fprintf (f, "syntax error on %s", get_token_name (c2m_ctx, curr_token->code));
  fprintf (f, " (expected '%s'):", expected_name);
#if 0
  {
    static const int context_len = 5;

    for (int i = 0; i < context_len && curr_token->code != T_EOFILE; i++) {
      fprintf (f, " %s", curr_token->repr);
    }
  }
#endif
  fprintf (f, "\n");
  n_errors++;
}

static int tpname_eq (tpname_t tpname1, tpname_t tpname2, void *arg MIR_UNUSED) {
  return tpname1.id->u.s.s == tpname2.id->u.s.s && tpname1.scope == tpname2.scope;
}

static htab_hash_t tpname_hash (tpname_t tpname, void *arg MIR_UNUSED) {
  return (htab_hash_t) (mir_hash_finish (
    mir_hash_step (mir_hash_step (mir_hash_init (0x42), (uint64_t) tpname.id->u.s.s),
                   (uint64_t) tpname.scope)));
}

static void tpname_init (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  HTAB_CREATE (tpname_t, tpname_tab, alloc, 1000, tpname_hash, tpname_eq, NULL);
}

static int tpname_find (c2m_ctx_t c2m_ctx, node_t id, node_t scope, tpname_t *res) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  int found_p;
  tpname_t el, tpname;

  tpname.id = id;
  tpname.scope = scope;
  found_p = HTAB_DO (tpname_t, tpname_tab, tpname, HTAB_FIND, el);
  if (res != NULL && found_p) *res = el;
  return found_p;
}

static tpname_t tpname_add (c2m_ctx_t c2m_ctx, node_t id, node_t scope, int typedef_p) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  tpname_t el, tpname;

  tpname.id = id;
  tpname.scope = scope;
  tpname.typedef_p = typedef_p;
  if (HTAB_DO (tpname_t, tpname_tab, tpname, HTAB_FIND, el)) return el;
  HTAB_DO (tpname_t, tpname_tab, tpname, HTAB_INSERT, el);
  return el;
}

static void tpname_finish (c2m_ctx_t c2m_ctx) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  if (tpname_tab != NULL) HTAB_DESTROY (tpname_t, tpname_tab);
}

#define P(f)                                                 \
  do {                                                       \
    if ((r = (f) (c2m_ctx, no_err_p)) == err_node) return r; \
  } while (0)
#define PA(f, a)                                                \
  do {                                                          \
    if ((r = (f) (c2m_ctx, no_err_p, a)) == err_node) return r; \
  } while (0)
#define PTFAIL(t)                                                               \
  do {                                                                          \
    if (record_level == 0) syntax_error (c2m_ctx, get_token_name (c2m_ctx, t)); \
    return err_node;                                                            \
  } while (0)

#define PT(t)               \
  do {                      \
    if (!M (t)) PTFAIL (t); \
  } while (0)

#define PTP(t, pos)               \
  do {                            \
    if (!MP (t, pos)) PTFAIL (t); \
  } while (0)

#define PTN(t)                  \
  do {                          \
    if (!MN (t, r)) PTFAIL (t); \
  } while (0)

#define PE(f, l)                                           \
  do {                                                     \
    if ((r = (f) (c2m_ctx, no_err_p)) == err_node) goto l; \
  } while (0)
#define PAE(f, a, l)                                          \
  do {                                                        \
    if ((r = (f) (c2m_ctx, no_err_p, a)) == err_node) goto l; \
  } while (0)
#define PTE(t, pos, l)        \
  do {                        \
    if (!MP (t, pos)) goto l; \
  } while (0)

typedef node_t (*nonterm_func_t) (c2m_ctx_t c2m_ctx, int);
typedef node_t (*nonterm_arg_func_t) (c2m_ctx_t c2m_ctx, int, node_t);

#define D(f) static node_t f (c2m_ctx_t c2m_ctx, int no_err_p MIR_UNUSED)
#define DA(f) static node_t f (c2m_ctx_t c2m_ctx, int no_err_p, node_t arg)

#define C(c) (curr_token->code == c)

static int match (c2m_ctx_t c2m_ctx, int c, pos_t *pos, node_code_t *node_code, node_t *node) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  if (curr_token->code != c) return FALSE;
  if (pos != NULL) *pos = curr_token->pos;
  if (node_code != NULL) *node_code = curr_token->node_code;
  if (node != NULL) *node = curr_token->node;
  read_token (c2m_ctx);
  return TRUE;
}

#define M(c) match (c2m_ctx, c, NULL, NULL, NULL)
#define MP(c, pos) match (c2m_ctx, c, &(pos), NULL, NULL)
#define MC(c, pos, code) match (c2m_ctx, c, &(pos), &(code), NULL)
#define MN(c, node) match (c2m_ctx, c, NULL, NULL, &(node))

static node_t try_f (c2m_ctx_t c2m_ctx, nonterm_func_t f) {
  size_t mark = record_start (c2m_ctx);
  node_t r = (f) (c2m_ctx, TRUE);

  record_stop (c2m_ctx, mark, r == err_node);
  return r;
}

static node_t try_arg_f (c2m_ctx_t c2m_ctx, nonterm_arg_func_t f, node_t arg) {
  size_t mark = record_start (c2m_ctx);
  node_t r = (f) (c2m_ctx, TRUE, arg);

  record_stop (c2m_ctx, mark, r == err_node);
  return r;
}

#define TRY(f) try_f (c2m_ctx, f)
#define TRY_A(f, arg) try_arg_f (c2m_ctx, f, arg)

D (compound_stmt);

/* Expressions: */
D (type_name);
D (expr);
D (assign_expr);
D (initializer_list);

D (par_type_name) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r;

  PT ('(');
  P (type_name);
  PT (')');
  return r;
}

D (primary_expr) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r, n, op, gn, list;
  pos_t pos;

  if (MN (T_ID, r) || MN (T_NUMBER, r) || MN (T_CH, r) || MN (T_STR, r)) {
    return r;
  } else if (MP (T_ANDAND, pos)) {
    PTN (T_ID);
    return new_pos_node1 (c2m_ctx, N_LABEL_ADDR, pos, r);
  } else if (M ('(')) {
    if (C ('{')) {
      P (compound_stmt);
      r = new_node1 (c2m_ctx, N_STMTEXPR, r);
    } else {
      P (expr);
    }
    if (M (')')) return r;
  } else if (MP (T_GENERIC, pos)) {
    PT ('(');
    P (assign_expr);
    PT (',');
    list = new_node (c2m_ctx, N_LIST);
    n = new_pos_node2 (c2m_ctx, N_GENERIC, pos, r, list);
    for (;;) { /* generic-assoc-list , generic-association */
      if (MP (T_DEFAULT, pos)) {
        op = new_node (c2m_ctx, N_IGNORE);
      } else {
        P (type_name);
        op = r;
        pos = POS (op);
      }
      PT (':');
      P (assign_expr);
      gn = new_pos_node2 (c2m_ctx, N_GENERIC_ASSOC, pos, op, r);
      op_append (c2m_ctx, list, gn);
      if (!M (',')) break;
    }
    PT (')');
    return n;
  }
  return err_node;
}

DA (post_expr_part) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r, n, op, list;
  node_code_t code;
  pos_t pos;

  r = arg;
  for (;;) {
    if (MC (T_INCDEC, pos, code)) {
      code = code == N_INC ? N_POST_INC : N_POST_DEC;
      op = r;
      r = NULL;
    } else if (MC ('.', pos, code) || MC (T_ARROW, pos, code)) {
      op = r;
      if (!MN (T_ID, r)) return err_node;
    } else if (MC ('[', pos, code)) {
      op = r;
      P (expr);
      PT (']');
    } else if (!MP ('(', pos)) {
      break;
    } else {
      op = r;
      r = NULL;
      code = N_CALL;
      list = new_node (c2m_ctx, N_LIST);
      if (!C (')')) {
        for (;;) {
          P (assign_expr);
          op_append (c2m_ctx, list, r);
          if (!M (',')) break;
        }
      }
      r = list;
      PT (')');
    }
    n = new_pos_node1 (c2m_ctx, code, pos, op);
    if (r != NULL) op_append (c2m_ctx, n, r);
    r = n;
  }
  return r;
}

D (post_expr) {
  node_t r;

  P (primary_expr);
  PA (post_expr_part, r);
  return r;
}

D (unary_expr) {
  node_t r, t;
  node_code_t code;
  pos_t pos;

  if ((r = TRY (par_type_name)) != err_node) {
    t = r;
    if (!MP ('{', pos)) {
      P (unary_expr);
      r = new_node2 (c2m_ctx, N_CAST, t, r);
    } else {
      P (initializer_list);
      if (!M ('}')) return err_node;
      r = new_pos_node2 (c2m_ctx, N_COMPOUND_LITERAL, pos, t, r);
      PA (post_expr_part, r);
    }
    return r;
  } else if (MP (T_SIZEOF, pos)) {
    if ((r = TRY (par_type_name)) != err_node) {
      r = new_pos_node1 (c2m_ctx, N_SIZEOF, pos, r);
      return r;
    }
    code = N_EXPR_SIZEOF;
  } else if (MP (T_ALIGNOF, pos)) {
    if ((r = TRY (par_type_name)) != err_node) {
      r = new_pos_node1 (c2m_ctx, N_ALIGNOF, pos, r);
    } else {
      P (unary_expr); /* error recovery */
      r = new_pos_node1 (c2m_ctx, N_ALIGNOF, pos, new_node (c2m_ctx, N_IGNORE));
    }
    return r;
  } else if (!MC (T_INCDEC, pos, code) && !MC (T_UNOP, pos, code) && !MC (T_ADDOP, pos, code)
             && !MC ('*', pos, code) && !MC ('&', pos, code)) {
    P (post_expr);
    return r;
  } else if (code == N_AND) {
    code = N_ADDR;
  } else if (code == N_MUL) {
    code = N_DEREF;
  }
  P (unary_expr);
  r = new_pos_node1 (c2m_ctx, code, pos, r);
  return r;
}

static node_t left_op (c2m_ctx_t c2m_ctx, int no_err_p, int token, int token2, nonterm_func_t f) {
  node_code_t code;
  node_t r, n;
  pos_t pos;

  P (f);
  while (MC (token, pos, code) || (token2 >= 0 && MC (token2, pos, code))) {
    n = new_pos_node1 (c2m_ctx, code, pos, r);
    P (f);
    op_append (c2m_ctx, n, r);
    r = n;
  }
  return r;
}

static node_t right_op (c2m_ctx_t c2m_ctx, int no_err_p, int token, int token2, nonterm_func_t left,
                        nonterm_func_t right) {
  node_code_t code;
  node_t r, n;
  pos_t pos;

  P (left);
  if (MC (token, pos, code) || (token2 >= 0 && MC (token2, pos, code))) {
    n = new_pos_node1 (c2m_ctx, code, pos, r);
    P (right);
    op_append (c2m_ctx, n, r);
    r = n;
  }
  return r;
}

D (mul_expr) { return left_op (c2m_ctx, no_err_p, T_DIVOP, '*', unary_expr); }
D (add_expr) { return left_op (c2m_ctx, no_err_p, T_ADDOP, -1, mul_expr); }
D (sh_expr) { return left_op (c2m_ctx, no_err_p, T_SH, -1, add_expr); }
D (rel_expr) { return left_op (c2m_ctx, no_err_p, T_CMP, -1, sh_expr); }
D (eq_expr) { return left_op (c2m_ctx, no_err_p, T_EQNE, -1, rel_expr); }
D (and_expr) { return left_op (c2m_ctx, no_err_p, '&', -1, eq_expr); }
D (xor_expr) { return left_op (c2m_ctx, no_err_p, '^', -1, and_expr); }
D (or_expr) { return left_op (c2m_ctx, no_err_p, '|', -1, xor_expr); }
D (land_expr) { return left_op (c2m_ctx, no_err_p, T_ANDAND, -1, or_expr); }
D (lor_expr) { return left_op (c2m_ctx, no_err_p, T_OROR, -1, land_expr); }

D (cond_expr) {
  node_t r, n;
  pos_t pos;

  P (lor_expr);
  if (!MP ('?', pos)) return r;
  n = new_pos_node1 (c2m_ctx, N_COND, pos, r);
  P (expr);
  op_append (c2m_ctx, n, r);
  if (!M (':')) return err_node;
  P (cond_expr);
  op_append (c2m_ctx, n, r);
  return n;
}

#define const_expr cond_expr

D (assign_expr) { return right_op (c2m_ctx, no_err_p, T_ASSIGN, '=', cond_expr, assign_expr); }
D (expr) { return right_op (c2m_ctx, no_err_p, ',', -1, assign_expr, expr); }

/* Declarations: */
D (attr_spec);
DA (declaration_specs);
D (sc_spec);
DA (type_spec);
D (struct_declaration_list);
D (struct_declaration);
D (spec_qual_list);
D (type_qual);
D (func_spec);
D (align_spec);
D (declarator);
D (direct_declarator);
D (pointer);
D (type_qual_list);
D (param_type_list);
D (id_list);
D (abstract_declarator);
D (direct_abstract_declarator);
D (typedef_name);
D (initializer);
D (st_assert);

D (asm_spec) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r, id;

  PTN (T_ID);
  if (strcmp (r->u.s.s, "__asm") != 0 && strcmp (r->u.s.s, "asm") != 0) PTFAIL (T_ID);
  id = r;
  PT ('(');
  PTN (T_STR);
  PT (')');
  return new_pos_node1 (c2m_ctx, N_ASM, POS (id), r);
}

static node_t try_attr_spec (c2m_ctx_t c2m_ctx, pos_t pos, node_t *asm_part) {
  node_t r;

  if (c2m_options->pedantic_p) return NULL;
  if (asm_part != NULL) {
    *asm_part = NULL;
    if ((r = TRY (asm_spec)) != err_node) {
      if (c2m_options->pedantic_p)
        error (c2m_ctx, pos, "asm is not implemented");
      else {
        /*warning (c2m_ctx, pos, "asm is not implemented -- ignoring it")*/
      }
      *asm_part = r;
    }
  }
  if ((r = TRY (attr_spec)) != err_node) {
    if (c2m_options->pedantic_p)
      error (c2m_ctx, pos, "GCC attributes are not implemented");
    else {
      /*warning (c2m_ctx, pos, "GCC attributes are not implemented -- ignoring them")*/
    }
  }
  return r;
}

D (declaration) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  int typedef_p;
  node_t op, list, decl, spec, r, attrs, asm_part = NULL;
  pos_t pos, last_pos;

  if (C (T_STATIC_ASSERT)) {
    P (st_assert);
  } else if (MP (';', pos)) {
    r = new_node (c2m_ctx, N_LIST);
    if (curr_scope == top_scope && c2m_options->pedantic_p)
      warning (c2m_ctx, pos, "extra ; outside of a function");
  } else {
    try_attr_spec (c2m_ctx, curr_token->pos, NULL);
    PA (declaration_specs, curr_scope == top_scope ? (node_t) 1 : NULL);
    spec = r;
    last_pos = POS (spec);
    list = new_node (c2m_ctx, N_LIST);
    if (C (';')) {
      op_append (c2m_ctx, list,
                 new_node5 (c2m_ctx, N_SPEC_DECL, spec, new_node (c2m_ctx, N_IGNORE),
                            new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_IGNORE),
                            new_node (c2m_ctx, N_IGNORE)));
    } else { /* init-declarator-list */
      for (op = NL_HEAD (spec->u.ops); op != NULL; op = NL_NEXT (op))
        if (op->code == N_TYPEDEF) break;
      typedef_p = op != NULL;
      for (;;) { /* init-declarator */
        P (declarator);
        decl = r;
        last_pos = POS (decl);
        assert (decl->code == N_DECL);
        op = NL_HEAD (decl->u.ops);
        tpname_add (c2m_ctx, op, curr_scope, typedef_p);
        attrs = try_attr_spec (c2m_ctx, last_pos, &asm_part);
        if (attrs == err_node) attrs = new_node (c2m_ctx, N_IGNORE);
        if (asm_part == NULL) asm_part = new_node (c2m_ctx, N_IGNORE);
        if (M ('=')) {
          P (initializer);
        } else {
          r = new_node (c2m_ctx, N_IGNORE);
        }
        op_append (c2m_ctx, list,
                   new_pos_node5 (c2m_ctx, N_SPEC_DECL, POS (decl),
                                  new_node1 (c2m_ctx, N_SHARE, spec), decl, attrs, asm_part, r));
        if (!M (',')) break;
      }
    }
    r = list;
    PT (';');
  }
  return r;
}

D (attr) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r, res, list;

  if (C (')') || C (',')) /* empty */
    return NULL;
  if (FIRST_KW <= (token_code_t) curr_token->code && (token_code_t) curr_token->code <= LAST_KW) {
    token_t kw = curr_token;
    PT (curr_token->code);
    r = new_str_node (c2m_ctx, N_ID, uniq_cstr (c2m_ctx, kw->repr), kw->pos);
  } else {
    PTN (T_ID);
  }
  list = new_node (c2m_ctx, N_LIST);
  res = new_node2 (c2m_ctx, N_ATTR, r, list);
  if (C ('(')) {
    PT ('(');
    while (!C (')')) {
      if (C (T_NUMBER) || C (T_CH) || C (T_STR))
        PTN (curr_token->code);
      else
        PTN (T_ID);
      op_append (c2m_ctx, list, r);
      if (!C (')')) PT (',');
    }
    PT (')');
  }
  return res;
}

D (attr_spec) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, r;

  PTN (T_ID);
  /* libc can define empty __attribute__ for non-GNU C compiler -- define also __mirc_attribute__ */
  if (strcmp (r->u.s.s, "__attribute__") != 0 && strcmp (r->u.s.s, "__mirc_attribute__") != 0)
    PTFAIL (T_ID);
  PT ('(');
  PT ('(');
  list = new_node (c2m_ctx, N_LIST);
  for (;;) {
    P (attr);
    op_append (c2m_ctx, list, r);
    if (C (')')) break;
    PT (',');
  }
  PT (')');
  PT (')');
  return list;
}

DA (declaration_specs) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, r, prev_type_spec = NULL;
  int first_p;
  pos_t pos = curr_token->pos, spec_pos;

  list = new_node (c2m_ctx, N_LIST);
  for (first_p = arg == NULL;; first_p = FALSE) {
    spec_pos = curr_token->pos;
    if (C (T_ALIGNAS)) {
      P (align_spec);
    } else if ((r = TRY (sc_spec)) != err_node) {
    } else if ((r = TRY (type_qual)) != err_node) {
    } else if ((r = TRY (func_spec)) != err_node) {
    } else if (first_p) {
      PA (type_spec, prev_type_spec);
      prev_type_spec = r;
    } else if ((r = TRY_A (type_spec, prev_type_spec)) != err_node) {
      prev_type_spec = r;
    } else if ((r = try_attr_spec (c2m_ctx, spec_pos, FALSE)) != err_node && r != NULL) {
      continue; /* ignore attrs for declaration specs (type attrs) */
    } else
      break;
    op_append (c2m_ctx, list, r);
  }
  if (prev_type_spec == NULL && arg != NULL) {
    if (c2m_options->pedantic_p) warning (c2m_ctx, pos, "type defaults to int");
    r = new_pos_node (c2m_ctx, N_INT, pos);
    op_append (c2m_ctx, list, r);
  }
  return list;
}

D (sc_spec) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r;
  pos_t pos;

  if (MP (T_TYPEDEF, pos)) {
    r = new_pos_node (c2m_ctx, N_TYPEDEF, pos);
  } else if (MP (T_EXTERN, pos)) {
    r = new_pos_node (c2m_ctx, N_EXTERN, pos);
  } else if (MP (T_STATIC, pos)) {
    r = new_pos_node (c2m_ctx, N_STATIC, pos);
  } else if (MP (T_AUTO, pos)) {
    r = new_pos_node (c2m_ctx, N_AUTO, pos);
  } else if (MP (T_REGISTER, pos)) {
    r = new_pos_node (c2m_ctx, N_REGISTER, pos);
  } else if (MP (T_THREAD_LOCAL, pos)) {
    if (c2m_options->pedantic_p)
      error (c2m_ctx, pos, "Thread local is not implemented");
    else
      warning (c2m_ctx, pos,
               "Thread local is not implemented -- program might not work as assumed");
    r = new_pos_node (c2m_ctx, N_THREAD_LOCAL, pos);
  } else {
    if (record_level == 0) syntax_error (c2m_ctx, "a storage specifier");
    return err_node;
  }
  return r;
}

DA (type_spec) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t op1, op2, op3, op4, r;
  int struct_p, id_p = FALSE;
  pos_t pos;

  if (MP (T_VOID, pos)) {
    r = new_pos_node (c2m_ctx, N_VOID, pos);
  } else if (MP (T_CHAR, pos)) {
    r = new_pos_node (c2m_ctx, N_CHAR, pos);
  } else if (MP (T_SHORT, pos)) {
    r = new_pos_node (c2m_ctx, N_SHORT, pos);
  } else if (MP (T_INT, pos)) {
    r = new_pos_node (c2m_ctx, N_INT, pos);
  } else if (MP (T_LONG, pos)) {
    r = new_pos_node (c2m_ctx, N_LONG, pos);
  } else if (MP (T_FLOAT, pos)) {
    r = new_pos_node (c2m_ctx, N_FLOAT, pos);
  } else if (MP (T_DOUBLE, pos)) {
    r = new_pos_node (c2m_ctx, N_DOUBLE, pos);
  } else if (MP (T_SIGNED, pos)) {
    r = new_pos_node (c2m_ctx, N_SIGNED, pos);
  } else if (MP (T_UNSIGNED, pos)) {
    r = new_pos_node (c2m_ctx, N_UNSIGNED, pos);
  } else if (MP (T_BOOL, pos)) {
    r = new_pos_node (c2m_ctx, N_BOOL, pos);
  } else if (MP (T_COMPLEX, pos)) {
    if (record_level == 0) error (c2m_ctx, pos, "complex numbers are not supported");
    return err_node;
  } else if (MP (T_ATOMIC, pos)) { /* atomic-type-specifier */
    PT ('(');
    P (type_name);
    PT (')');
    error (c2m_ctx, pos, "Atomic types are not supported");
  } else if ((struct_p = MP (T_STRUCT, pos)) || MP (T_UNION, pos)) {
    /* struct-or-union-specifier, struct-or-union */
    if (!MN (T_ID, op1)) {
      op1 = new_node (c2m_ctx, N_IGNORE);
    } else {
      id_p = TRUE;
    }
    if (M ('{')) {
      if (!C ('}') && !M (';')) {
        P (struct_declaration_list);
      } else {
        (c2m_options->pedantic_p ? error : warning) (c2m_ctx, pos, "empty struct/union");
        r = new_node (c2m_ctx, N_LIST);
      }
      PT ('}');
    } else if (!id_p) {
      return err_node;
    } else {
      r = new_node (c2m_ctx, N_IGNORE);
    }
    r = new_pos_node2 (c2m_ctx, struct_p ? N_STRUCT : N_UNION, pos, op1, r);
  } else if (MP (T_ENUM, pos)) { /* enum-specifier */
    if (!MN (T_ID, op1)) {
      op1 = new_node (c2m_ctx, N_IGNORE);
    } else {
      id_p = TRUE;
    }
    op2 = new_node (c2m_ctx, N_LIST);
    if (M ('{')) { /* enumerator-list */
      for (;;) {   /* enumerator */
        PTN (T_ID);
        op3 = r; /* enumeration-constant */
        if (!M ('=')) {
          op4 = new_node (c2m_ctx, N_IGNORE);
        } else {
          P (const_expr);
          op4 = r;
        }
        op_append (c2m_ctx, op2, new_node2 (c2m_ctx, N_ENUM_CONST, op3, op4));
        if (!M (',')) break;
        if (C ('}')) break;
      }
      PT ('}');
    } else if (!id_p) {
      return err_node;
    } else {
      op2 = new_node (c2m_ctx, N_IGNORE);
    }
    r = new_pos_node2 (c2m_ctx, N_ENUM, pos, op1, op2);
  } else if (arg == NULL) {
    P (typedef_name);
  } else {
    r = err_node;
  }
  return r;
}

D (struct_declaration_list) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r, res, el, next_el;

  res = new_node (c2m_ctx, N_LIST);
  for (;;) {
    P (struct_declaration);
    if (r->code != N_LIST) {
      op_append (c2m_ctx, res, r);
    } else {
      for (el = NL_HEAD (r->u.ops); el != NULL; el = next_el) {
        next_el = NL_NEXT (el);
        NL_REMOVE (r->u.ops, el);
        op_append (c2m_ctx, res, el);
      }
    }
    if (C ('}')) break;
  }
  return res;
}

D (struct_declaration) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, spec, op, r, attrs;

  if (C (T_STATIC_ASSERT)) {
    P (st_assert);
  } else {
    P (spec_qual_list);
    spec = r;
    list = new_node (c2m_ctx, N_LIST);
    if (M (';')) {
      op = new_pos_node4 (c2m_ctx, N_MEMBER, POS (spec), new_node1 (c2m_ctx, N_SHARE, spec),
                          new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_IGNORE),
                          new_node (c2m_ctx, N_IGNORE));
      op_append (c2m_ctx, list, op);
    } else {     /* struct-declarator-list */
      for (;;) { /* struct-declarator */
        if (!C (':')) {
          P (declarator);
          attrs = try_attr_spec (c2m_ctx, curr_token->pos, NULL);
          op = r;
        } else {
          attrs = err_node;
          op = new_node (c2m_ctx, N_IGNORE);
        }
        if (attrs == err_node) attrs = new_node (c2m_ctx, N_IGNORE);
        if (M (':')) {
          P (const_expr);
        } else {
          r = new_node (c2m_ctx, N_IGNORE);
        }
        op = new_pos_node4 (c2m_ctx, N_MEMBER, POS (op), new_node1 (c2m_ctx, N_SHARE, spec), op,
                            attrs, r);
        op_append (c2m_ctx, list, op);
        if (!M (',')) break;
      }
      PT (';');
    }
    r = list;
  }
  return r;
}

D (spec_qual_list) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, op, r, arg = NULL;
  int first_p;

  list = new_node (c2m_ctx, N_LIST);
  for (first_p = TRUE;; first_p = FALSE) {
    if (C (T_CONST) || C (T_RESTRICT) || C (T_VOLATILE) || C (T_ATOMIC)) {
      P (type_qual);
      op = r;
    } else if ((op = TRY_A (type_spec, arg)) != err_node) {
      arg = op;
    } else if (first_p) {
      return err_node;
    } else {
      break;
    }
    op_append (c2m_ctx, list, op);
  }
  return list;
}

D (type_qual) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r;
  pos_t pos;

  if (MP (T_CONST, pos)) {
    r = new_pos_node (c2m_ctx, N_CONST, pos);
  } else if (MP (T_RESTRICT, pos)) {
    r = new_pos_node (c2m_ctx, N_RESTRICT, pos);
  } else if (MP (T_VOLATILE, pos)) {
    r = new_pos_node (c2m_ctx, N_VOLATILE, pos);
  } else if (MP (T_ATOMIC, pos)) {
    r = new_pos_node (c2m_ctx, N_ATOMIC, pos);
  } else {
    if (record_level == 0) syntax_error (c2m_ctx, "a type qualifier");
    r = err_node;
  }
  return r;
}

D (func_spec) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r;
  pos_t pos;

  if (MP (T_INLINE, pos)) {
    r = new_pos_node (c2m_ctx, N_INLINE, pos);
  } else if (MP (T_NO_RETURN, pos)) {
    r = new_pos_node (c2m_ctx, N_NO_RETURN, pos);
  } else {
    if (record_level == 0) syntax_error (c2m_ctx, "a function specifier");
    r = err_node;
  }
  return r;
}

D (align_spec) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r;
  pos_t pos;

  PTP (T_ALIGNAS, pos);
  PT ('(');
  if ((r = TRY (type_name)) != err_node) {
  } else {
    P (const_expr);
  }
  PT (')');
  r = new_pos_node1 (c2m_ctx, N_ALIGNAS, pos, r);
  return r;
}

D (declarator) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, p = NULL, r, el, next_el;

  if (C ('*')) {
    P (pointer);
    p = r;
  }
  P (direct_declarator);
  if (p != NULL) {
    list = NL_NEXT (NL_HEAD (r->u.ops));
    assert (list->code == N_LIST);
    for (el = NL_HEAD (p->u.ops); el != NULL; el = next_el) {
      next_el = NL_NEXT (el);
      NL_REMOVE (p->u.ops, el);
      op_append (c2m_ctx, list, el);
    }
  }
  return r;
}

D (direct_declarator) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, tql, ae, res, r;
  pos_t pos, static_pos;

  if (MN (T_ID, r)) {
    res = new_node2 (c2m_ctx, N_DECL, r, new_node (c2m_ctx, N_LIST));
  } else if (M ('(')) {
    P (declarator);
    res = r;
    PT (')');
  } else {
    return err_node;
  }
  list = NL_NEXT (NL_HEAD (res->u.ops));
  assert (list->code == N_LIST);
  for (;;) {
    if (MP ('(', pos)) {
      if ((r = TRY (param_type_list)) != err_node) {
      } else {
        P (id_list);
      }
      PT (')');
      op_append (c2m_ctx, list, new_pos_node1 (c2m_ctx, N_FUNC, pos, r));
    } else if (M ('[')) {
      int static_p = FALSE;

      if (MP (T_STATIC, static_pos)) {
        static_p = TRUE;
      }
      if (!C (T_CONST) && !C (T_RESTRICT) && !C (T_VOLATILE) && !C (T_ATOMIC)) {
        tql = new_node (c2m_ctx, N_LIST);
      } else {
        P (type_qual_list);
        tql = r;
        if (!static_p && M (T_STATIC)) {
          static_p = TRUE;
        }
      }
      if (static_p) {
        P (assign_expr);
        ae = r;
      } else if (MP ('*', pos)) {
        ae = new_pos_node (c2m_ctx, N_STAR, pos);
      } else if (!C (']')) {
        P (assign_expr);
        ae = r;
      } else {
        ae = new_node (c2m_ctx, N_IGNORE);
      }
      PT (']');
      op_append (c2m_ctx, list,
                 new_node3 (c2m_ctx, N_ARR,
                            static_p ? new_pos_node (c2m_ctx, N_STATIC, static_pos)
                                     : new_node (c2m_ctx, N_IGNORE),
                            tql, ae));
    } else
      break;
  }
  return res;
}

D (pointer) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t op, r;
  pos_t pos;

  PTP ('*', pos);
  if (C (T_CONST) || C (T_RESTRICT) || C (T_VOLATILE) || C (T_ATOMIC)) {
    P (type_qual_list);
  } else {
    r = new_node (c2m_ctx, N_LIST);
  }
  op = new_pos_node1 (c2m_ctx, N_POINTER, pos, r);
  if (C ('*')) {
    P (pointer);
  } else {
    r = new_node (c2m_ctx, N_LIST);
  }
  op_append (c2m_ctx, r, op);
  return r;
}

D (type_qual_list) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, r;

  list = new_node (c2m_ctx, N_LIST);
  for (;;) {
    P (type_qual);
    op_append (c2m_ctx, list, r);
    if (!C (T_CONST) && !C (T_RESTRICT) && !C (T_VOLATILE) && !C (T_ATOMIC)) break;
  }
  return list;
}

D (param_type_abstract_declarator) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r = err_node;

  P (abstract_declarator);
  if (C (',') || C (')')) return r;
  return err_node;
}

D (param_type_list) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, attrs, op1, op2, r = err_node;
  int comma_p;
  pos_t pos;

  list = new_node (c2m_ctx, N_LIST);
  if (C (')')) return list;
  for (;;) { /* parameter-list, parameter-declaration */
    PA (declaration_specs, NULL);
    op1 = r;
    if (C (',') || C (')')) {
      r = new_node2 (c2m_ctx, N_TYPE, op1,
                     new_node2 (c2m_ctx, N_DECL, new_node (c2m_ctx, N_IGNORE),
                                new_node (c2m_ctx, N_LIST)));
    } else if ((op2 = TRY (param_type_abstract_declarator)) != err_node) {
      /* Try param_type_abstract_declarator first for possible func
         type case ("<res_type> (<typedef_name>)") which can conflict with declarator ("<res_type>
         (<new decl identifier>)")  */
      r = new_node2 (c2m_ctx, N_TYPE, op1, op2);
    } else {
      P (declarator);
      attrs = try_attr_spec (c2m_ctx, curr_token->pos, NULL);
      if (attrs == err_node) attrs = new_node (c2m_ctx, N_IGNORE);
      r = new_pos_node5 (c2m_ctx, N_SPEC_DECL, POS (op2), op1, r, attrs,
                         new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_IGNORE));
    }
    op_append (c2m_ctx, list, r);
    comma_p = FALSE;
    if (!M (',')) break;
    comma_p = TRUE;
    if (C (T_DOTS)) break;
  }
  if (comma_p) {
    PTP (T_DOTS, pos);
    op_append (c2m_ctx, list, new_pos_node (c2m_ctx, N_DOTS, pos));
  }
  return list;
}

D (id_list) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, r;

  list = new_node (c2m_ctx, N_LIST);
  if (C (')')) return list;
  for (;;) {
    PTN (T_ID);
    op_append (c2m_ctx, list, r);
    if (!M (',')) break;
  }
  return list;
}

D (abstract_declarator) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, p = NULL, r, el, next_el;

  if (C ('*')) {
    P (pointer);
    p = r;
    if ((r = TRY (direct_abstract_declarator)) == err_node)
      r = new_pos_node2 (c2m_ctx, N_DECL, POS (p), new_node (c2m_ctx, N_IGNORE),
                         new_node (c2m_ctx, N_LIST));
  } else {
    P (direct_abstract_declarator);
  }
  if (p != NULL) {
    list = NL_NEXT (NL_HEAD (r->u.ops));
    assert (list->code == N_LIST);
    for (el = NL_HEAD (p->u.ops); el != NULL; el = next_el) {
      next_el = NL_NEXT (el);
      NL_REMOVE (p->u.ops, el);
      op_append (c2m_ctx, list, el);
    }
  }
  return r;
}

D (par_abstract_declarator) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r;

  PT ('(');
  P (abstract_declarator);
  PT (')');
  return r;
}

D (direct_abstract_declarator) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t res, list, tql, ae, r;
  pos_t pos, pos2 = no_pos;

  if ((res = TRY (par_abstract_declarator)) != err_node) {
    if (!C ('(') && !C ('[')) return res;
  } else {
    res = new_node2 (c2m_ctx, N_DECL, new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_LIST));
  }
  list = NL_NEXT (NL_HEAD (res->u.ops));
  assert (list->code == N_LIST);
  for (;;) {
    if (MP ('(', pos)) {
      P (param_type_list);
      PT (')');
      op_append (c2m_ctx, list, new_pos_node1 (c2m_ctx, N_FUNC, pos, r));
    } else {
      PTP ('[', pos);
      if (MP ('*', pos2)) {
        r = new_pos_node3 (c2m_ctx, N_ARR, pos, new_node (c2m_ctx, N_IGNORE),
                           new_node (c2m_ctx, N_IGNORE), new_pos_node (c2m_ctx, N_STAR, pos2));
      } else {
        int static_p = FALSE;

        if (MP (T_STATIC, pos2)) {
          static_p = TRUE;
        }
        if (!C (T_CONST) && !C (T_RESTRICT) && !C (T_VOLATILE) && !C (T_ATOMIC)) {
          tql = new_node (c2m_ctx, N_LIST);
        } else {
          P (type_qual_list);
          tql = r;
          if (!static_p && M (T_STATIC)) {
            static_p = TRUE;
          }
        }
        if (!C (']')) {
          P (assign_expr);
          ae = r;
        } else {
          ae = new_node (c2m_ctx, N_IGNORE);
        }
        r = new_pos_node3 (c2m_ctx, N_ARR, pos,
                           static_p ? new_pos_node (c2m_ctx, N_STATIC, pos2)
                                    : new_node (c2m_ctx, N_IGNORE),
                           tql, ae);
      }
      PT (']');
      op_append (c2m_ctx, list, r);
    }
    if (!C ('(') && !C ('[')) break;
  }
  add_pos (c2m_ctx, res, POS (list));
  return res;
}

D (typedef_name) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t scope, r;
  tpname_t tpn;

  PTN (T_ID);
  for (scope = curr_scope;; scope = scope->attr) {
    if (tpname_find (c2m_ctx, r, scope, &tpn)) {
      if (!tpn.typedef_p) break;
      return r;
    }
    if (scope == NULL) break;
  }
  return err_node;
}

D (initializer) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r;

  if (!M ('{')) {
    P (assign_expr);
  } else {
    P (initializer_list);
    if (M (',')) {
    }
    PT ('}');
  }
  return r;
}

D (initializer_list) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, list2, r;
  int first_p;

  list = new_node (c2m_ctx, N_LIST);
  if (C ('}')) {
    (c2m_options->pedantic_p ? error : warning) (c2m_ctx, curr_token->pos,
                                                 "empty initializer list");
    return list;
  }
  for (;;) { /* designation */
    list2 = new_node (c2m_ctx, N_LIST);
    for (first_p = TRUE;; first_p = FALSE) { /* designator-list, designator */
      if (M ('[')) {
        P (const_expr);
        PT (']');
      } else if (M ('.')) {
        PTN (T_ID);
        r = new_node1 (c2m_ctx, N_FIELD_ID, r);
      } else
        break;
      op_append (c2m_ctx, list2, r);
    }
    if (!first_p) {
      PT ('=');
    }
    P (initializer);
    op_append (c2m_ctx, list, new_node2 (c2m_ctx, N_INIT, list2, r));
    if (!M (',')) break;
    if (C ('}')) break;
  }
  return list;
}

D (type_name) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t op, r;

  P (spec_qual_list);
  op = r;
  if (!C (')') && !C (':')) {
    P (abstract_declarator);
  } else {
    r = new_pos_node2 (c2m_ctx, N_DECL, POS (op), new_node (c2m_ctx, N_IGNORE),
                       new_node (c2m_ctx, N_LIST));
  }
  return new_node2 (c2m_ctx, N_TYPE, op, r);
}

D (st_assert) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t op1, r;
  pos_t pos;

  PTP (T_STATIC_ASSERT, pos);
  PT ('(');
  P (const_expr);
  op1 = r;
  PT (',');
  PTN (T_STR);
  PT (')');
  PT (';');
  r = new_pos_node2 (c2m_ctx, N_ST_ASSERT, pos, op1, r);
  return r;
}

/* Statements: */

D (label) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t r, n;
  pos_t pos;

  if (MP (T_CASE, pos)) {
    P (expr);
    n = new_pos_node1 (c2m_ctx, N_CASE, pos, r);
    if (M (T_DOTS)) {
      P (expr);
      op_append (c2m_ctx, n, r);
    }
    r = n;
  } else if (MP (T_DEFAULT, pos)) {
    r = new_pos_node (c2m_ctx, N_DEFAULT, pos);
  } else {
    PTN (T_ID);
    r = new_node1 (c2m_ctx, N_LABEL, r);
  }
  PT (':');
  return r;
}

D (stmt) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t l, n, op1, op2, op3, r;
  pos_t pos;

  l = new_node (c2m_ctx, N_LIST);
  while ((op1 = TRY (label)) != err_node) {
    op_append (c2m_ctx, l, op1);
  }
  if (C ('{')) {
    P (compound_stmt);
    if (NL_HEAD (l->u.ops) != NULL) { /* replace empty label list */
      assert (NL_HEAD (r->u.ops)->code == N_LIST && NL_HEAD (NL_HEAD (r->u.ops)->u.ops) == NULL);
      NL_REMOVE (r->u.ops, NL_HEAD (r->u.ops));
      NL_PREPEND (r->u.ops, l);
    }
  } else if (MP (T_IF, pos)) { /* selection-statement */
    PT ('(');
    P (expr);
    op1 = r;
    PT (')');
    P (stmt);
    op2 = r;
    if (!M (T_ELSE)) {
      r = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (stmt);
    }
    r = new_pos_node4 (c2m_ctx, N_IF, pos, l, op1, op2, r);
  } else if (MP (T_SWITCH, pos)) { /* selection-statement */
    PT ('(');
    P (expr);
    op1 = r;
    PT (')');
    P (stmt);
    r = new_pos_node3 (c2m_ctx, N_SWITCH, pos, l, op1, r);
  } else if (MP (T_WHILE, pos)) { /* iteration-statement */
    PT ('(');
    P (expr);
    op1 = r;
    PT (')');
    P (stmt);
    r = new_pos_node3 (c2m_ctx, N_WHILE, pos, l, op1, r);
  } else if (M (T_DO)) { /* iteration-statement */
    P (stmt);
    op1 = r;
    PTP (T_WHILE, pos);
    PT ('(');
    P (expr);
    PT (')');
    PT (';');
    r = new_pos_node3 (c2m_ctx, N_DO, pos, l, r, op1);
  } else if (MP (T_FOR, pos)) { /* iteration-statement */
    PT ('(');
    n = new_pos_node (c2m_ctx, N_FOR, pos);
    n->attr = curr_scope;
    curr_scope = n;
    if ((r = TRY (declaration)) != err_node) {
      op1 = r;
      curr_scope = n->attr;
    } else {
      curr_scope = n->attr;
      if (!M (';')) {
        P (expr);
        op1 = r;
        PT (';');
      } else {
        op1 = new_node (c2m_ctx, N_IGNORE);
      }
    }
    if (M (';')) {
      op2 = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
      op2 = r;
      PT (';');
    }
    if (C (')')) {
      op3 = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
      op3 = r;
    }
    PT (')');
    P (stmt);
    op_append (c2m_ctx, n, l);
    op_append (c2m_ctx, n, op1);
    op_append (c2m_ctx, n, op2);
    op_append (c2m_ctx, n, op3);
    op_append (c2m_ctx, n, r);
    r = n;
  } else if (MP (T_GOTO, pos)) { /* jump-statement */
    int indirect_p = FALSE;
    if (!M ('*')) {
      PTN (T_ID);
    } else {
      indirect_p = TRUE;
      P (expr);
    }
    PT (';');
    r = new_pos_node2 (c2m_ctx, indirect_p ? N_INDIRECT_GOTO : N_GOTO, pos, l, r);
  } else if (MP (T_CONTINUE, pos)) { /* continue-statement */
    PT (';');
    r = new_pos_node1 (c2m_ctx, N_CONTINUE, pos, l);
  } else if (MP (T_BREAK, pos)) { /* break-statement */
    PT (';');
    r = new_pos_node1 (c2m_ctx, N_BREAK, pos, l);
  } else if (MP (T_RETURN, pos)) { /* return-statement */
    if (M (';')) {
      r = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
      PT (';');
    }
    r = new_pos_node2 (c2m_ctx, N_RETURN, pos, l, r);
  } else { /* expression-statement */
    if (C (';')) {
      r = new_node (c2m_ctx, N_IGNORE);
    } else {
      P (expr);
    }
    PT (';');
    r = new_pos_node2 (c2m_ctx, N_EXPR, POS (r), l, r);
  }
  return r;
}

static void error_recovery (c2m_ctx_t c2m_ctx, int par_lev, const char *expected) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  syntax_error (c2m_ctx, expected);
  if (c2m_options->debug_p) fprintf (stderr, "error recovery: skipping");
  for (;;) {
    if (curr_token->code == T_EOFILE || (par_lev == 0 && curr_token->code == ';')) break;
    if (curr_token->code == '{') {
      par_lev++;
    } else if (curr_token->code == '}') {
      if (--par_lev <= 0) break;
    }
    if (c2m_options->debug_p)
      fprintf (stderr, " %s(%d:%d)", get_token_name (c2m_ctx, curr_token->code),
               curr_token->pos.lno, curr_token->pos.ln_pos);
    read_token (c2m_ctx);
  }
  if (c2m_options->debug_p) fprintf (stderr, " %s\n", get_token_name (c2m_ctx, curr_token->code));
  if (curr_token->code != T_EOFILE) read_token (c2m_ctx);
}

D (compound_stmt) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t n, list, r;
  pos_t pos;

  PTE ('{', pos, err0);
  list = new_node (c2m_ctx, N_LIST);
  n = new_pos_node2 (c2m_ctx, N_BLOCK, pos, new_node (c2m_ctx, N_LIST), list);
  n->attr = curr_scope;
  curr_scope = n;
  while (!C ('}') && !C (T_EOFILE)) { /* block-item-list, block_item */
    if ((r = TRY (declaration)) != err_node) {
    } else {
      PE (stmt, err1);
    }
    op_flat_append (c2m_ctx, list, r);
    continue;
  err1:
    error_recovery (c2m_ctx, 1, "<statement>");
  }
  curr_scope = n->attr;
  if (C (T_EOFILE)) {
    error (c2m_ctx, pos, "unfinished compound statement");
    return err_node;
  }
  PT ('}');
  return n;
err0:
  error_recovery (c2m_ctx, 0, "{");
  return err_node;
}

D (transl_unit) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;
  node_t list, ds, d, dl, r, func, param_list, p, par_declarator, id;

  // curr_token->code = ';'; /* for error recovery */
  read_token (c2m_ctx);
  list = new_node (c2m_ctx, N_LIST);
  while (!C (T_EOFILE)) { /* external-declaration */
    if ((r = TRY (declaration)) != err_node) {
    } else {
      PAE (declaration_specs, (node_t) 1, err);
      ds = r;
      PE (declarator, err);
      d = r;
      dl = new_node (c2m_ctx, N_LIST);
      d->attr = curr_scope;
      curr_scope = d;
      while (!C ('{')) { /* declaration-list */
        PE (declaration, decl_err);
        op_flat_append (c2m_ctx, dl, r);
      }
      func = NL_HEAD (NL_EL (d->u.ops, 1)->u.ops);
      if (func == NULL || func->code != N_FUNC) {
        id = NL_HEAD (d->u.ops);
        error (c2m_ctx, POS (id), "non-function declaration %s before '{'", id->u.s.s);
      } else {
        param_list = NL_HEAD (func->u.ops);
        for (p = NL_HEAD (param_list->u.ops); p != NULL; p = NL_NEXT (p)) {
          if (p->code == N_ID) {
            tpname_add (c2m_ctx, p, curr_scope, FALSE);
          } else if (p->code == N_SPEC_DECL) {
            par_declarator = NL_EL (p->u.ops, 1);
            id = NL_HEAD (par_declarator->u.ops);
            tpname_add (c2m_ctx, id, curr_scope, FALSE);
          }
        }
      }
      P (compound_stmt);
      r = new_pos_node4 (c2m_ctx, N_FUNC_DEF, POS (d), ds, d, dl, r);
      curr_scope = d->attr;
    }
    op_flat_append (c2m_ctx, list, r);
    continue;
  decl_err:
    curr_scope = d->attr;
  err:
    error_recovery (c2m_ctx, 0, "<declarator>");
  }
  return new_node1 (c2m_ctx, N_MODULE, list);
}

static void fatal_error (c2m_ctx_t c2m_ctx, C_error_code_t code MIR_UNUSED, const char *message) {
  if (c2m_options->message_file != NULL) fprintf (c2m_options->message_file, "%s\n", message);
  longjmp (c2m_ctx->env, 1);
}

static void kw_add (c2m_ctx_t c2m_ctx, const char *name, token_code_t tc, size_t flags) {
  str_add (c2m_ctx, name, strlen (name) + 1, tc, flags, TRUE);
}

static void parse_init (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  parse_ctx_t parse_ctx;

  c2m_ctx->parse_ctx = parse_ctx = c2mir_calloc (c2m_ctx, sizeof (struct parse_ctx));
  curr_scope = NULL;
  error_func = fatal_error;
  record_level = 0;
  curr_uid = 0;
  init_streams (c2m_ctx);
  VARR_CREATE (token_t, recorded_tokens, alloc, 32);
  VARR_CREATE (token_t, buffered_tokens, alloc, 32);
  pre_init (c2m_ctx);
  kw_add (c2m_ctx, "_Bool", T_BOOL, 0);
  kw_add (c2m_ctx, "_Complex", T_COMPLEX, 0);
  kw_add (c2m_ctx, "_Alignas", T_ALIGNAS, 0);
  kw_add (c2m_ctx, "_Alignof", T_ALIGNOF, 0);
  kw_add (c2m_ctx, "_Atomic", T_ATOMIC, 0);
  kw_add (c2m_ctx, "_Generic", T_GENERIC, 0);
  kw_add (c2m_ctx, "_Noreturn", T_NO_RETURN, 0);
  kw_add (c2m_ctx, "_Static_assert", T_STATIC_ASSERT, 0);
  kw_add (c2m_ctx, "_Thread_local", T_THREAD_LOCAL, 0);
  kw_add (c2m_ctx, "auto", T_AUTO, 0);
  kw_add (c2m_ctx, "break", T_BREAK, 0);
  kw_add (c2m_ctx, "case", T_CASE, 0);
  kw_add (c2m_ctx, "char", T_CHAR, 0);
  kw_add (c2m_ctx, "const", T_CONST, 0);
  kw_add (c2m_ctx, "continue", T_CONTINUE, 0);
  kw_add (c2m_ctx, "default", T_DEFAULT, 0);
  kw_add (c2m_ctx, "do", T_DO, 0);
  kw_add (c2m_ctx, "double", T_DOUBLE, 0);
  kw_add (c2m_ctx, "else", T_ELSE, 0);
  kw_add (c2m_ctx, "enum", T_ENUM, 0);
  kw_add (c2m_ctx, "extern", T_EXTERN, 0);
  kw_add (c2m_ctx, "float", T_FLOAT, 0);
  kw_add (c2m_ctx, "for", T_FOR, 0);
  kw_add (c2m_ctx, "goto", T_GOTO, 0);
  kw_add (c2m_ctx, "if", T_IF, 0);
  kw_add (c2m_ctx, "inline", T_INLINE, FLAG_EXT89);
  kw_add (c2m_ctx, "int", T_INT, 0);
  kw_add (c2m_ctx, "long", T_LONG, 0);
  kw_add (c2m_ctx, "register", T_REGISTER, 0);
  kw_add (c2m_ctx, "restrict", T_RESTRICT, FLAG_C89);
  kw_add (c2m_ctx, "return", T_RETURN, 0);
  kw_add (c2m_ctx, "short", T_SHORT, 0);
  kw_add (c2m_ctx, "signed", T_SIGNED, 0);
  kw_add (c2m_ctx, "sizeof", T_SIZEOF, 0);
  kw_add (c2m_ctx, "static", T_STATIC, 0);
  kw_add (c2m_ctx, "struct", T_STRUCT, 0);
  kw_add (c2m_ctx, "switch", T_SWITCH, 0);
  kw_add (c2m_ctx, "typedef", T_TYPEDEF, 0);
  kw_add (c2m_ctx, "typeof", T_TYPEOF, FLAG_EXT);
  kw_add (c2m_ctx, "union", T_UNION, 0);
  kw_add (c2m_ctx, "unsigned", T_UNSIGNED, 0);
  kw_add (c2m_ctx, "void", T_VOID, 0);
  kw_add (c2m_ctx, "volatile", T_VOLATILE, 0);
  kw_add (c2m_ctx, "while", T_WHILE, 0);
  kw_add (c2m_ctx, "__restrict", T_RESTRICT, FLAG_EXT);
  kw_add (c2m_ctx, "__restrict__", T_RESTRICT, FLAG_EXT);
  kw_add (c2m_ctx, "__inline", T_INLINE, FLAG_EXT);
  kw_add (c2m_ctx, "__inline__", T_INLINE, FLAG_EXT);
  tpname_init (c2m_ctx);
}

static void add_standard_includes (c2m_ctx_t c2m_ctx) {
  const char *str, *name;

  for (size_t i = 0; i < sizeof (standard_includes) / sizeof (string_include_t); i++) {
    if ((name = standard_includes[i].name) != NULL) continue;
    str = standard_includes[i].content;
    add_string_stream (c2m_ctx, "<environment>", str);
  }
}

static node_t parse (c2m_ctx_t c2m_ctx) {
  parse_ctx_t parse_ctx = c2m_ctx->parse_ctx;

  next_token_index = 0;
  return transl_unit (c2m_ctx, FALSE);
}

static void parse_finish (c2m_ctx_t c2m_ctx) {
  if (c2m_ctx == NULL || c2m_ctx->parse_ctx == NULL) return;
  if (recorded_tokens != NULL) VARR_DESTROY (token_t, recorded_tokens);
  if (buffered_tokens != NULL) VARR_DESTROY (token_t, buffered_tokens);
  pre_finish (c2m_ctx);
  tpname_finish (c2m_ctx);
  finish_streams (c2m_ctx);
  free (c2m_ctx->parse_ctx);
}

#undef P
#undef PT
#undef PTP
#undef PTN
#undef PE
#undef PTE
#undef D
#undef M
#undef MP
#undef MC
#undef MN
#undef TRY
#undef C

/* ------------------------- Parser End ------------------------------ */
/* New Page */
/* ---------------------- Context Checker Start ---------------------- */

/* The context checker is AST traversing pass which checks C11
   constraints.  It also augmenting AST nodes by type and layout
   information.  Here are the created node attributes:

 1. expr nodes have attribute "struct expr", N_ID not expr context has NULL attribute.
 2. N_SWITCH has attribute "struct switch_attr"
 3. N_SPEC_DECL (only with ID), N_MEMBER, N_FUNC_DEF have attribute "struct decl"
 4. N_GOTO has attribute node_t (target stmt)
 5. N_STRUCT, N_UNION have attribute "struct node_scope" if they have a decl list
 6. N_MODULE, N_BLOCK, N_FOR, N_FUNC have attribute "struct node_scope"
 7. declaration_specs or spec_qual_list N_LISTs have attribute "struct decl_spec",
    but as a part of N_COMPOUND_LITERAL have attribute "struct decl"
 8. N_ENUM has attribute "struct enum_type"
 9. N_ENUM_CONST has attribute "struct enum_value"
10. N_CASE and N_DEFAULT have attribute "struct case_attr"

*/

typedef struct decl *decl_t;
DEF_VARR (decl_t);

typedef struct case_attr *case_t;
DEF_HTAB (case_t);

#undef curr_scope

struct check_ctx {
  node_t curr_scope;
  VARR (node_t) * label_uses;
  node_t func_block_scope;
  unsigned curr_func_scope_num;
  unsigned char in_params_p, jump_ret_p;
  node_t curr_unnamed_anon_struct_union_member;
  node_t curr_switch;
  VARR (decl_t) * func_decls_for_allocation;
  VARR (node_t) * possible_incomplete_decls;
  node_t n_i1_node;
  HTAB (case_t) * case_tab;
  node_t curr_func_def, curr_loop, curr_loop_switch;
  mir_size_t curr_call_arg_area_offset;
  VARR (node_t) * context_stack;
};

#define curr_scope check_ctx->curr_scope
#define label_uses check_ctx->label_uses
#define func_block_scope check_ctx->func_block_scope
#define curr_func_scope_num check_ctx->curr_func_scope_num
#define in_params_p check_ctx->in_params_p
#define jump_ret_p check_ctx->jump_ret_p
#define curr_unnamed_anon_struct_union_member check_ctx->curr_unnamed_anon_struct_union_member
#define curr_switch check_ctx->curr_switch
#define func_decls_for_allocation check_ctx->func_decls_for_allocation
#define possible_incomplete_decls check_ctx->possible_incomplete_decls
#define n_i1_node check_ctx->n_i1_node
#define case_tab check_ctx->case_tab
#define curr_func_def check_ctx->curr_func_def
#define curr_loop check_ctx->curr_loop
#define curr_loop_switch check_ctx->curr_loop_switch
#define curr_call_arg_area_offset check_ctx->curr_call_arg_area_offset
#define context_stack check_ctx->context_stack

static int supported_alignment_p (mir_llong align MIR_UNUSED) { return TRUE; }  // ???

static int symbol_eq (symbol_t s1, symbol_t s2, void *arg MIR_UNUSED) {
  return s1.mode == s2.mode && s1.id->u.s.s == s2.id->u.s.s && s1.scope == s2.scope;
}

static htab_hash_t symbol_hash (symbol_t s, void *arg MIR_UNUSED) {
  return (htab_hash_t) (mir_hash_finish (
    mir_hash_step (mir_hash_step (mir_hash_step (mir_hash_init (0x42), (uint64_t) s.mode),
                                  (uint64_t) s.id->u.s.s),
                   (uint64_t) s.scope)));
}

static void symbol_clear (symbol_t sym, void *arg MIR_UNUSED) { VARR_DESTROY (node_t, sym.defs); }

static void symbol_init (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  HTAB_CREATE_WITH_FREE_FUNC (symbol_t, symbol_tab, alloc, 5000, symbol_hash, symbol_eq, symbol_clear,
                              NULL);
}

static int symbol_find (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                        symbol_t *res) {
  int found_p;
  symbol_t el, symbol;

  symbol.mode = mode;
  symbol.id = id;
  symbol.scope = scope;
  found_p = HTAB_DO (symbol_t, symbol_tab, symbol, HTAB_FIND, el);
  if (res != NULL && found_p) *res = el;
  return found_p;
}

static void symbol_insert (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                           node_t def_node, node_t aux_node) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  symbol_t el, symbol;

  symbol.mode = mode;
  symbol.id = id;
  symbol.scope = scope;
  symbol.def_node = def_node;
  symbol.aux_node = aux_node;
  VARR_CREATE (node_t, symbol.defs, alloc, 4);
  VARR_PUSH (node_t, symbol.defs, def_node);
  HTAB_DO (symbol_t, symbol_tab, symbol, HTAB_INSERT, el);
}

static void symbol_def_replace (c2m_ctx_t c2m_ctx, symbol_t symbol, node_t def_node) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  symbol_t el;
  VARR (node_t) * defs;

  VARR_CREATE (node_t, defs, alloc, 4);
  for (size_t i = 0; i < VARR_LENGTH (node_t, symbol.defs); i++)
    VARR_PUSH (node_t, defs, VARR_GET (node_t, symbol.defs, i));
  symbol.defs = defs;
  symbol.def_node = def_node;
  HTAB_DO (symbol_t, symbol_tab, symbol, HTAB_REPLACE, el);
}

static void symbol_finish (c2m_ctx_t c2m_ctx) {
  if (symbol_tab != NULL) HTAB_DESTROY (symbol_t, symbol_tab);
}

enum basic_type get_int_basic_type (size_t s) {
  return (s == sizeof (mir_int)     ? TP_INT
          : s == sizeof (mir_short) ? TP_SHORT
          : s == sizeof (mir_long)  ? TP_LONG
          : s == sizeof (mir_schar) ? TP_SCHAR
                                    : TP_LLONG);
}

static int type_qual_eq_p (const struct type_qual *tq1, const struct type_qual *tq2) {
  return (tq1->const_p == tq2->const_p && tq1->restrict_p == tq2->restrict_p
          && tq1->volatile_p == tq2->volatile_p && tq1->atomic_p == tq2->atomic_p);
}

static void clear_type_qual (struct type_qual *tq) {
  tq->const_p = tq->restrict_p = tq->volatile_p = tq->atomic_p = FALSE;
}

static int type_qual_subset_p (const struct type_qual *tq1, const struct type_qual *tq2) {
  return (tq1->const_p <= tq2->const_p && tq1->restrict_p <= tq2->restrict_p
          && tq1->volatile_p <= tq2->volatile_p && tq1->atomic_p <= tq2->atomic_p);
}

static struct type_qual type_qual_union (const struct type_qual *tq1, const struct type_qual *tq2) {
  struct type_qual res;

  res.const_p = tq1->const_p || tq2->const_p;
  res.restrict_p = tq1->restrict_p || tq2->restrict_p;
  res.volatile_p = tq1->volatile_p || tq2->volatile_p;
  res.atomic_p = tq1->atomic_p || tq2->atomic_p;
  return res;
}

static void init_type (struct type *type) {
  clear_type_qual (&type->type_qual);
  type->mode = TM_UNDEF;
  type->pos_node = NULL;
  type->arr_type = NULL;
  type->antialias = 0;
  type->align = -1;
  type->raw_size = MIR_SIZE_MAX;
  type->func_type_before_adjustment_p = FALSE;
  type->unnamed_anon_struct_union_member_type_p = FALSE;
}

static void set_type_pos_node (struct type *type, node_t n) {
  if (type->pos_node == NULL) type->pos_node = n;
}

static int char_type_p (const struct type *type) {
  return (type->mode == TM_BASIC
          && (type->u.basic_type == TP_CHAR || type->u.basic_type == TP_SCHAR
              || type->u.basic_type == TP_UCHAR));
}

static int standard_integer_type_p (const struct type *type) {
  return (type->mode == TM_BASIC && type->u.basic_type >= TP_BOOL
          && type->u.basic_type <= TP_ULLONG);
}

static int integer_type_p (const struct type *type) {
  return standard_integer_type_p (type) || type->mode == TM_ENUM;
}

static enum basic_type get_enum_basic_type (const struct type *type);

static int signed_integer_type_p (const struct type *type) {
  if (standard_integer_type_p (type)) {
    enum basic_type tp = type->u.basic_type;

    return ((tp == TP_CHAR && char_is_signed_p ()) || tp == TP_SCHAR || tp == TP_SHORT
            || tp == TP_INT || tp == TP_LONG || tp == TP_LLONG);
  }
  if (type->mode == TM_ENUM) {
    enum basic_type basic_type = get_enum_basic_type (type);
    return (basic_type == TP_INT || basic_type == TP_LONG || basic_type == TP_LLONG);
  }
  return FALSE;
}

static int floating_type_p (const struct type *type) {
  return type->mode == TM_BASIC
         && (type->u.basic_type == TP_FLOAT || type->u.basic_type == TP_DOUBLE
             || type->u.basic_type == TP_LDOUBLE);
}

static int arithmetic_type_p (const struct type *type) {
  return integer_type_p (type) || floating_type_p (type);
}

static int scalar_type_p (const struct type *type) {
  return arithmetic_type_p (type) || type->mode == TM_PTR;
}

static struct type get_ptr_int_type (int signed_p) {
  struct type res;

  init_type (&res);
  res.mode = TM_BASIC;
  if (sizeof (mir_int) == sizeof (mir_size_t)) {
    res.u.basic_type = signed_p ? TP_INT : TP_UINT;
    return res;
  } else if (sizeof (mir_long) == sizeof (mir_size_t)) {
    res.u.basic_type = signed_p ? TP_LONG : TP_ULONG;
    return res;
  }
  assert (sizeof (mir_llong) == sizeof (mir_size_t));
  res.u.basic_type = signed_p ? TP_LLONG : TP_ULLONG;
  return res;
}

static struct type integer_promotion (const struct type *type) {
  struct type res;

  assert (integer_type_p (type));
  init_type (&res);
  res.mode = TM_BASIC;
  if (type->mode == TM_BASIC && TP_LONG <= type->u.basic_type && type->u.basic_type <= TP_ULLONG) {
    res.u.basic_type = type->u.basic_type;
    return res;
  }
  if (type->mode == TM_BASIC
      && ((type->u.basic_type == TP_CHAR && MIR_CHAR_MAX > MIR_INT_MAX)
          || (type->u.basic_type == TP_UCHAR && MIR_UCHAR_MAX > MIR_INT_MAX)
          || (type->u.basic_type == TP_USHORT && MIR_USHORT_MAX > MIR_INT_MAX)))
    res.u.basic_type = TP_UINT;
  else if (type->mode == TM_ENUM)
    res.u.basic_type = get_enum_basic_type (type);
  else if (type->mode == TM_BASIC && type->u.basic_type == TP_UINT)
    res.u.basic_type = TP_UINT;
  else
    res.u.basic_type = TP_INT;
  return res;
}

static struct type arithmetic_conversion (const struct type *type1, const struct type *type2) {
  struct type res, t1, t2;

  assert (arithmetic_type_p (type1) && arithmetic_type_p (type2));
  init_type (&res);
  res.mode = TM_BASIC;
  if (floating_type_p (type1) || floating_type_p (type2)) {
    if ((type1->mode == TM_BASIC && type1->u.basic_type == TP_LDOUBLE)
        || (type2->mode == TM_BASIC && type2->u.basic_type == TP_LDOUBLE)) {
      res.u.basic_type = TP_LDOUBLE;
    } else if ((type1->mode == TM_BASIC && type1->u.basic_type == TP_DOUBLE)
               || (type2->mode == TM_BASIC && type2->u.basic_type == TP_DOUBLE)) {
      res.u.basic_type = TP_DOUBLE;
    } else if ((type1->mode == TM_BASIC && type1->u.basic_type == TP_FLOAT)
               || (type2->mode == TM_BASIC && type2->u.basic_type == TP_FLOAT)) {
      res.u.basic_type = TP_FLOAT;
    }
    return res;
  }
  t1 = integer_promotion (type1);
  t2 = integer_promotion (type2);
  if (signed_integer_type_p (&t1) == signed_integer_type_p (&t2)) {
    res.u.basic_type = t1.u.basic_type < t2.u.basic_type ? t2.u.basic_type : t1.u.basic_type;
  } else {
    struct type t;

    if (signed_integer_type_p (&t1)) SWAP (t1, t2, t);
    assert (!signed_integer_type_p (&t1) && signed_integer_type_p (&t2));
    if ((t1.u.basic_type == TP_ULONG && t2.u.basic_type < TP_LONG)
        || (t1.u.basic_type == TP_ULLONG && t2.u.basic_type < TP_LLONG)) {
      res.u.basic_type = t1.u.basic_type;
    } else if ((t1.u.basic_type == TP_UINT && t2.u.basic_type >= TP_LONG
                && MIR_LONG_MAX >= MIR_UINT_MAX)
               || (t1.u.basic_type == TP_ULONG && t2.u.basic_type >= TP_LLONG
                   && MIR_LLONG_MAX >= MIR_ULONG_MAX)) {
      res.u.basic_type = t2.u.basic_type;
    } else {
      res.u.basic_type = t1.u.basic_type;
    }
  }
  return res;
}

struct expr {
  unsigned int const_p : 1, const_addr_p : 1, builtin_call_p : 1;
  union {
    node_t lvalue_node;       /* for id, str, field, deref field, ind, deref, compound literal */
    node_t label_addr_target; /* for label address */
  } u;
  node_t def_node;    /* defined for id or const address (ref) or label address node */
  struct type *type;  /* type of the result */
  struct type *type2; /* used for assign expr type */
  union {             /* defined for const or const addr (i_val is offset) */
    mir_llong i_val;
    mir_ullong u_val;
    mir_ldouble d_val;
  } c;
};

struct decl_spec {
  unsigned int typedef_p : 1, extern_p : 1, static_p : 1;
  unsigned int auto_p : 1, register_p : 1, thread_local_p : 1;
  unsigned int inline_p : 1, no_return_p : 1; /* func specifiers  */
  int align;                                  // negative value means undefined
  node_t align_node;                          //  strictest valid N_ALIGNAS node
  node_code_t linkage;  // N_IGNORE - none, N_STATIC - internal, N_EXTERN - external
  struct type *type;
};

struct enum_type {
  enum basic_type enum_basic_type;
};

struct enum_value {
  union {
    mir_llong i_val;
    mir_ullong u_val;
  } u;
};

struct node_scope {
  unsigned char stack_var_p; /* necessity for frame */
  unsigned func_scope_num;
  mir_size_t size, offset, call_arg_area_size;
  node_t scope;
};

struct decl {
  /* true if address is taken, reg can be used or is used: */
  unsigned addr_p : 1, reg_p : 1, asm_p : 1, used_p : 1;
  int bit_offset, width; /* for bitfields, -1 bit_offset for non bitfields. */
  mir_size_t offset;     /* var offset in frame or bss */
  node_t scope;          /* declaration scope */
  /* The next 2 members are used only for param decls. The 1st member is number of the start MIR
     func arg. The 2nd one is number or MIR func args used to pass param value, it is positive
     only for aggregates passed by value.  */
  uint32_t param_args_start, param_args_num;
  struct decl_spec decl_spec;
  /* Unnamed member if this scope is anon struct/union for the member,
     NULL otherwise: */
  node_t containing_unnamed_anon_struct_union_member;
  union {
    const char *asm_str; /* register name for global reg used and defined only if asm_p */
    MIR_item_t item;     /* MIR_item for some declarations */
  } u;
  c2m_ctx_t c2m_ctx;
};

static enum basic_type get_enum_basic_type (const struct type *type) {
  assert (type->mode == TM_ENUM);
  if (type->u.tag_type->attr == NULL) return TP_INT; /* in case of undefined enum */
  return ((struct enum_type *) type->u.tag_type->attr)->enum_basic_type;
}

static struct decl_spec *get_param_decl_spec (node_t param) {
  node_t MIR_UNUSED declarator;

  if (param->code == N_TYPE) return param->attr;
  declarator = NL_EL (param->u.ops, 1);
  assert (param->code == N_SPEC_DECL && declarator != NULL && declarator->code == N_DECL);
  return &((decl_t) param->attr)->decl_spec;
}

static int type_eq_p (struct type *type1, struct type *type2) {
  if (type1->mode != type2->mode) return FALSE;
  if (!type_qual_eq_p (&type1->type_qual, &type2->type_qual)) return FALSE;
  switch (type1->mode) {
  case TM_BASIC: return type1->u.basic_type == type2->u.basic_type;
  case TM_ENUM:
  case TM_STRUCT:
  case TM_UNION: return type1->u.tag_type == type2->u.tag_type;
  case TM_PTR: return type_eq_p (type1->u.ptr_type, type2->u.ptr_type);
  case TM_ARR: {
    struct expr *cexpr1, *cexpr2;
    struct arr_type *at1 = type1->u.arr_type, *at2 = type2->u.arr_type;

    return (at1->static_p == at2->static_p && type_eq_p (at1->el_type, at2->el_type)
            && type_qual_eq_p (&at1->ind_type_qual, &at2->ind_type_qual)
            && at1->size->code != N_IGNORE && at2->size->code != N_IGNORE
            && (cexpr1 = at1->size->attr)->const_p && (cexpr2 = at2->size->attr)->const_p
            && integer_type_p (cexpr2->type) && integer_type_p (cexpr2->type)
            && cexpr1->c.i_val == cexpr2->c.i_val);
  }
  case TM_FUNC: {
    struct func_type *ft1 = type1->u.func_type, *ft2 = type2->u.func_type;
    struct decl_spec *ds1, *ds2;

    if (ft1->dots_p != ft2->dots_p || !type_eq_p (ft1->ret_type, ft2->ret_type)
        || NL_LENGTH (ft1->param_list->u.ops) != NL_LENGTH (ft2->param_list->u.ops))
      return FALSE;
    for (node_t p1 = NL_HEAD (ft1->param_list->u.ops), p2 = NL_HEAD (ft2->param_list->u.ops);
         p1 != NULL; p1 = NL_NEXT (p1), p2 = NL_NEXT (p2)) {
      ds1 = get_param_decl_spec (p1);
      ds2 = get_param_decl_spec (p2);
      if (!type_eq_p (ds1->type, ds2->type)) return FALSE;
      // ??? other qualifiers
    }
    return TRUE;
  }
  default: return FALSE;
  }
}

static int compatible_types_p (struct type *type1, struct type *type2, int ignore_quals_p) {
  if (type1->mode != type2->mode) {
    if (!ignore_quals_p && !type_qual_eq_p (&type1->type_qual, &type2->type_qual)) return FALSE;
    if (type1->mode == TM_ENUM && type2->mode == TM_BASIC)
      return type2->u.basic_type == get_enum_basic_type (type1);
    if (type2->mode == TM_ENUM && type1->mode == TM_BASIC)
      return type1->u.basic_type == get_enum_basic_type (type2);
    return FALSE;
  }
  if (type1->mode == TM_BASIC) {
    return (type1->u.basic_type == type2->u.basic_type
            && (ignore_quals_p || type_qual_eq_p (&type1->type_qual, &type2->type_qual)));
  } else if (type1->mode == TM_PTR) {
    return ((ignore_quals_p || type_qual_eq_p (&type1->type_qual, &type2->type_qual))
            && compatible_types_p (type1->u.ptr_type, type2->u.ptr_type, ignore_quals_p));
  } else if (type1->mode == TM_ARR) {
    struct expr *cexpr1, *cexpr2;
    struct arr_type *at1 = type1->u.arr_type, *at2 = type2->u.arr_type;

    if (!compatible_types_p (at1->el_type, at2->el_type, ignore_quals_p)) return FALSE;
    if (at1->size->code == N_IGNORE || at2->size->code == N_IGNORE) return TRUE;
    if ((cexpr1 = at1->size->attr)->const_p && (cexpr2 = at2->size->attr)->const_p
        && integer_type_p (cexpr2->type) && integer_type_p (cexpr2->type))
      return cexpr1->c.i_val == cexpr2->c.i_val;
    return TRUE;
  } else if (type1->mode == TM_FUNC) {
    struct func_type *ft1 = type1->u.func_type, *ft2 = type2->u.func_type;

    if (NL_HEAD (ft1->param_list->u.ops) != NULL && NL_HEAD (ft2->param_list->u.ops) != NULL
        && NL_LENGTH (ft1->param_list->u.ops) != NL_LENGTH (ft2->param_list->u.ops))
      return FALSE;
    // ??? check parameter types
  } else {
    assert (type1->mode == TM_STRUCT || type1->mode == TM_UNION || type1->mode == TM_ENUM);
    return (type1->u.tag_type == type2->u.tag_type
            && (ignore_quals_p || type_qual_eq_p (&type1->type_qual, &type2->type_qual)));
  }
  return TRUE;
}

static struct type composite_type (c2m_ctx_t c2m_ctx, struct type *tp1, struct type *tp2) {
  struct type t = *tp1;

  assert (compatible_types_p (tp1, tp2, TRUE));
  if (tp1->mode == TM_ARR) {
    struct arr_type *arr_type;

    t.u.arr_type = arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));
    *arr_type = *tp1->u.arr_type;
    if (arr_type->size->code == N_IGNORE) arr_type->size = tp2->u.arr_type->size;
    *arr_type->el_type
      = composite_type (c2m_ctx, tp1->u.arr_type->el_type, tp2->u.arr_type->el_type);
  } else if (tp1->mode == TM_FUNC) { /* ??? */
  }
  return t;
}

static struct type *create_type (c2m_ctx_t c2m_ctx, struct type *copy) {
  struct type *res = reg_malloc (c2m_ctx, sizeof (struct type));

  if (copy == NULL)
    init_type (res);
  else
    *res = *copy;
  return res;
}

DEF_DLIST_LINK (case_t);

struct case_attr {
  node_t case_node, case_target_node;
  DLIST_LINK (case_t) case_link;
};

DEF_DLIST (case_t, case_link);

struct switch_attr {
  struct type type; /* integer promoted type */
  int ranges_p;
  case_t min_val_case, max_val_case;
  DLIST (case_t) case_labels; /* default case is always a tail */
};

static int basic_type_size (enum basic_type bt) {
  switch (bt) {
  case TP_BOOL: return sizeof (mir_bool);
  case TP_CHAR: return sizeof (mir_char);
  case TP_SCHAR: return sizeof (mir_schar);
  case TP_UCHAR: return sizeof (mir_uchar);
  case TP_SHORT: return sizeof (mir_short);
  case TP_USHORT: return sizeof (mir_ushort);
  case TP_INT: return sizeof (mir_int);
  case TP_UINT: return sizeof (mir_uint);
  case TP_LONG: return sizeof (mir_long);
  case TP_ULONG: return sizeof (mir_ulong);
  case TP_LLONG: return sizeof (mir_llong);
  case TP_ULLONG: return sizeof (mir_ullong);
  case TP_FLOAT: return sizeof (mir_float);
  case TP_DOUBLE: return sizeof (mir_double);
  case TP_LDOUBLE: return sizeof (mir_ldouble);
  case TP_VOID: return 1;  // ???
  default: abort ();
  }
}

static int basic_type_align (enum basic_type bt) {
#ifdef MIR_LDOUBLE_ALIGN
  if (bt == TP_LDOUBLE) return MIR_LDOUBLE_ALIGN;
#endif
  return basic_type_size (bt);
}

static int type_align (struct type *type) {
  assert (type->align >= 0);
  return type->align;
}

static int incomplete_type_p (c2m_ctx_t c2m_ctx, struct type *type);

static void aux_set_type_align (c2m_ctx_t c2m_ctx, struct type *type) {
  /* Should be called only from set_type_layout. */
  int align, member_align;

  if (type->align >= 0) return;
  if (type->mode == TM_BASIC) {
    align = basic_type_align (type->u.basic_type);
  } else if (type->mode == TM_PTR) {
    align = sizeof (mir_size_t);
  } else if (type->mode == TM_ENUM) {
    align = basic_type_align (get_enum_basic_type (type));
  } else if (type->mode == TM_FUNC) {
    align = sizeof (mir_size_t);
  } else if (type->mode == TM_ARR) {
    align = type_align (type->u.arr_type->el_type);
  } else if (type->mode == TM_UNDEF) {
    align = 0; /* error type */
  } else {
    assert (type->mode == TM_STRUCT || type->mode == TM_UNION);
    if (incomplete_type_p (c2m_ctx, type)) {
      align = -1;
    } else {
      align = 1;
      for (node_t member = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); member != NULL;
           member = NL_NEXT (member))
        if (member->code == N_MEMBER) {
          decl_t decl = member->attr;
          node_t width = NL_EL (member->u.ops, 3);
          struct expr *expr;

          if (type->mode == TM_UNION && width->code != N_IGNORE && (expr = width->attr)->const_p
              && expr->c.u_val == 0)
            continue;
          member_align = type_align (decl->decl_spec.type);
          if (align < member_align) align = member_align;
        }
    }
  }
  type->align = align;
}

static mir_size_t type_size (c2m_ctx_t c2m_ctx, struct type *type) {
  mir_size_t size = raw_type_size (c2m_ctx, type);

  return type->align == 0 ? size : round_size (size, type->align);
}

static mir_size_t var_align (c2m_ctx_t c2m_ctx, struct type *type) {
  int align;

  raw_type_size (c2m_ctx, type); /* check */
  align = type->align;
  assert (align >= 0);
#ifdef ADJUST_VAR_ALIGNMENT
  align = ADJUST_VAR_ALIGNMENT (c2m_ctx, align, type);
#endif
  return align;
}

static mir_size_t var_size (c2m_ctx_t c2m_ctx, struct type *type) {
  mir_size_t size = raw_type_size (c2m_ctx, type);

  return round_size (size, var_align (c2m_ctx, type));
}

/* BOUND_BIT is used only if BF_P and updated only if BITS >= 0  */
static void update_field_layout (int *bf_p, mir_size_t *overall_size, mir_size_t *offset,
                                 int *bound_bit, mir_size_t prev_field_type_size,
                                 mir_size_t field_type_size, int field_type_align, int bits) {
  mir_size_t start_offset, curr_offset, prev_field_offset = *offset;

  assert (field_type_size > 0 && field_type_align > 0);
  start_offset = curr_offset
    = (*overall_size + field_type_align - 1) / field_type_align * field_type_align;
  if ((long) start_offset < field_type_align && bits >= 0) *bound_bit = 0;
  for (;; start_offset = curr_offset) {
    if ((long) curr_offset < field_type_align) {
      if (bits >= 0) *bound_bit += bits;
      break;
    }
    curr_offset -= field_type_align;
    if (!*bf_p) { /* previous is a regular field: */
      if (curr_offset < prev_field_offset + prev_field_type_size) {
        if (bits >= 0) {
          *bound_bit
            = (int) (prev_field_offset + prev_field_type_size - curr_offset) * MIR_CHAR_BIT;
          if (*bound_bit + bits <= (long) field_type_size * MIR_CHAR_BIT) continue;
          *bound_bit = bits;
          if (prev_field_offset + prev_field_type_size > start_offset)
            *bound_bit
              += (int) (prev_field_offset + prev_field_type_size - start_offset) * MIR_CHAR_BIT;
        }
        break;
      }
    } else if (bits < 0) { /* bitfield then regular field: */
      if (curr_offset < prev_field_offset + (*bound_bit + MIR_CHAR_BIT - 1) / MIR_CHAR_BIT) break;
    } else { /* bitfield then another bitfield: */
      if ((curr_offset + field_type_size) * MIR_CHAR_BIT
          < prev_field_offset * MIR_CHAR_BIT + *bound_bit + bits) {
        if (start_offset * MIR_CHAR_BIT >= prev_field_offset * MIR_CHAR_BIT + *bound_bit) {
          *bound_bit = bits;
        } else {
          *bound_bit = (int) (prev_field_offset * MIR_CHAR_BIT + *bound_bit + bits
                              - start_offset * MIR_CHAR_BIT);
        }
        break;
      }
    }
  }
  *bf_p = bits >= 0;
  *offset = start_offset;
  if (*overall_size < start_offset + field_type_size)
    *overall_size = start_offset + field_type_size;
}

/* Update offsets inside unnamed anonymous struct/union member. */
static void update_members_offset (struct type *type, mir_size_t offset) {
  assert ((type->mode == TM_STRUCT || type->mode == TM_UNION)
          && type->unnamed_anon_struct_union_member_type_p);
  assert (offset != MIR_SIZE_MAX || type->raw_size == MIR_SIZE_MAX);
  for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); el != NULL;
       el = NL_NEXT (el))
    if (el->code == N_MEMBER) {
      decl_t decl = el->attr;

      decl->offset = offset == MIR_SIZE_MAX ? 0 : decl->offset + offset;
      if (decl->decl_spec.type->unnamed_anon_struct_union_member_type_p)
        update_members_offset (decl->decl_spec.type,
                               offset == MIR_SIZE_MAX ? offset : decl->offset);
    }
}

static void set_type_layout (c2m_ctx_t c2m_ctx, struct type *type) {
  mir_size_t overall_size = 0;

  if (type->raw_size != MIR_SIZE_MAX) return; /* defined */
  if (type->mode == TM_BASIC) {
    overall_size = basic_type_size (type->u.basic_type);
  } else if (type->mode == TM_PTR) {
    overall_size = sizeof (mir_size_t);
  } else if (type->mode == TM_ENUM) {
    overall_size = basic_type_size (get_enum_basic_type (type));
  } else if (type->mode == TM_FUNC) {
    overall_size = sizeof (mir_size_t);
  } else if (type->mode == TM_ARR) {
    struct arr_type *arr_type = type->u.arr_type;
    struct expr *cexpr = arr_type->size->attr;
    mir_size_t nel
      = (arr_type->size->code == N_IGNORE || cexpr == NULL || !cexpr->const_p ? 1 : cexpr->c.i_val);

    set_type_layout (c2m_ctx, arr_type->el_type);
    overall_size = type_size (c2m_ctx, arr_type->el_type) * nel;
  } else if (type->mode == TM_UNDEF) {
    overall_size = sizeof (int); /* error type */
  } else {
    int bf_p = FALSE, bits = -1, bound_bit = 0;
    mir_size_t offset = 0, prev_size = 0;

    assert (type->mode == TM_STRUCT || type->mode == TM_UNION);
    if (incomplete_type_p (c2m_ctx, type)) {
      overall_size = MIR_SIZE_MAX;
    } else {
      for (node_t el = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); el != NULL;
           el = NL_NEXT (el))
        if (el->code == N_MEMBER) {
          decl_t decl = el->attr;
          int member_align;
          mir_size_t member_size;
          node_t width = NL_EL (el->u.ops, 3);
          struct expr *expr;
          int anon_process_p = (!type->unnamed_anon_struct_union_member_type_p
                                && decl->decl_spec.type->unnamed_anon_struct_union_member_type_p
                                && decl->decl_spec.type->raw_size == MIR_SIZE_MAX);

          if (anon_process_p) update_members_offset (decl->decl_spec.type, MIR_SIZE_MAX);
          set_type_layout (c2m_ctx, decl->decl_spec.type);
          if ((member_size = type_size (c2m_ctx, decl->decl_spec.type)) == 0) continue;
          member_align = type_align (decl->decl_spec.type);
          bits
            = width->code == N_IGNORE || !(expr = width->attr)->const_p ? -1 : (int) expr->c.u_val;
          update_field_layout (&bf_p, &overall_size, &offset, &bound_bit, prev_size, member_size,
                               member_align, bits);
          prev_size = member_size;
          decl->offset = offset;
          decl->bit_offset = bits < 0 ? -1 : bound_bit - bits;
          if (bits == 0) bf_p = FALSE;
          decl->width = bits;
          if (type->mode == TM_UNION) {
            offset = prev_size = 0;
            bf_p = FALSE;
            bits = -1;
            bound_bit = 0;
          }
          if (anon_process_p) update_members_offset (decl->decl_spec.type, decl->offset);
        }
    }
  }
  /* we might need raw_size for alignment calculations */
  type->raw_size = overall_size;
  aux_set_type_align (c2m_ctx, type);
  if (type->mode == TM_PTR) /* Visit the pointed but after setting size to avoid looping */
    set_type_layout (c2m_ctx, type->u.ptr_type);
}

static int int_bit_size (struct type *type) {
  assert (type->mode == TM_BASIC || type->mode == TM_ENUM);
  return (basic_type_size (type->mode == TM_ENUM ? get_enum_basic_type (type) : type->u.basic_type)
          * MIR_CHAR_BIT);
}

static int void_type_p (struct type *type) {
  return type->mode == TM_BASIC && type->u.basic_type == TP_VOID;
}

static int void_ptr_p (struct type *type) {
  return type->mode == TM_PTR && void_type_p (type->u.ptr_type);
}

static int incomplete_type_p (c2m_ctx_t c2m_ctx, struct type *type) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;

  switch (type->mode) {
  case TM_BASIC: return type->u.basic_type == TP_VOID;
  case TM_ENUM:
  case TM_STRUCT:
  case TM_UNION: {
    node_t scope, n = type->u.tag_type;

    if (NL_EL (n->u.ops, 1)->code == N_IGNORE) return TRUE;
    for (scope = curr_scope; scope != NULL && scope != top_scope && scope != n;
         scope = ((struct node_scope *) scope->attr)->scope)
      ;
    return scope == n;
  }
  case TM_PTR: return FALSE;
  case TM_ARR: {
    struct arr_type *arr_type = type->u.arr_type;

    return (arr_type->size->code == N_IGNORE || incomplete_type_p (c2m_ctx, arr_type->el_type));
  }
  case TM_FUNC:
    return ((type = type->u.func_type->ret_type) == NULL
            || (!void_type_p (type) && incomplete_type_p (c2m_ctx, type)));
  default: return FALSE;
  }
}

static int null_const_p (struct expr *expr, struct type *type) {
  return ((integer_type_p (type) && expr->const_p && expr->c.u_val == 0)
          || (void_ptr_p (type) && expr->const_p && expr->c.u_val == 0
              && type_qual_eq_p (&type->type_qual, &zero_type_qual)));
}

static void cast_value (struct expr *to_e, struct expr *from_e, struct type *to) {
  assert (to_e->const_p && from_e->const_p);
  struct type *from = from_e->type;

#define CONV(TP, cast, mto, mfrom) \
  case TP: to_e->c.mto = (cast) from_e->c.mfrom; break;
#define BASIC_FROM_CONV(mfrom)                                                           \
  switch (to->u.basic_type) {                                                            \
    CONV (TP_BOOL, mir_bool, u_val, mfrom) CONV (TP_UCHAR, mir_uchar, u_val, mfrom);     \
    CONV (TP_USHORT, mir_ushort, u_val, mfrom) CONV (TP_UINT, mir_uint, u_val, mfrom);   \
    CONV (TP_ULONG, mir_ulong, u_val, mfrom) CONV (TP_ULLONG, mir_ullong, u_val, mfrom); \
    CONV (TP_SCHAR, mir_char, i_val, mfrom);                                             \
    CONV (TP_SHORT, mir_short, i_val, mfrom) CONV (TP_INT, mir_int, i_val, mfrom);       \
    CONV (TP_LONG, mir_long, i_val, mfrom) CONV (TP_LLONG, mir_llong, i_val, mfrom);     \
    CONV (TP_FLOAT, mir_float, d_val, mfrom) CONV (TP_DOUBLE, mir_double, d_val, mfrom); \
    CONV (TP_LDOUBLE, mir_ldouble, d_val, mfrom);                                        \
  case TP_CHAR:                                                                          \
    if (char_is_signed_p ())                                                             \
      to_e->c.i_val = (mir_char) from_e->c.mfrom;                                        \
    else                                                                                 \
      to_e->c.u_val = (mir_char) from_e->c.mfrom;                                        \
    break;                                                                               \
  default: assert (FALSE);                                                               \
  }

#define BASIC_TO_CONV(cast, mto)                                \
  switch (from->u.basic_type) {                                 \
  case TP_BOOL:                                                 \
  case TP_UCHAR:                                                \
  case TP_USHORT:                                               \
  case TP_UINT:                                                 \
  case TP_ULONG:                                                \
  case TP_ULLONG: to_e->c.mto = (cast) from_e->c.u_val; break;  \
  case TP_CHAR:                                                 \
    if (!char_is_signed_p ()) {                                 \
      to_e->c.mto = (cast) from_e->c.u_val;                     \
      break;                                                    \
    }                                                           \
    /* falls through */                                         \
  case TP_SCHAR:                                                \
  case TP_SHORT:                                                \
  case TP_INT:                                                  \
  case TP_LONG:                                                 \
  case TP_LLONG: to_e->c.mto = (cast) from_e->c.i_val; break;   \
  case TP_FLOAT:                                                \
  case TP_DOUBLE:                                               \
  case TP_LDOUBLE: to_e->c.mto = (cast) from_e->c.d_val; break; \
  default: assert (FALSE);                                      \
  }

  struct type temp, temp2;
  if (to->mode == TM_ENUM) {
    temp.mode = TM_BASIC;
    temp.u.basic_type = get_enum_basic_type (to);
    to = &temp;
  }
  if (from->mode == TM_ENUM) {
    temp2.mode = TM_BASIC;
    temp2.u.basic_type = get_enum_basic_type (from);
    from = &temp2;
  }
  if (to->mode == from->mode && (from->mode == TM_PTR || from->mode == TM_ENUM)) {
    to_e->c = from_e->c;
  } else if (from->mode == TM_PTR) {
    BASIC_FROM_CONV (u_val);
  } else if (to->mode == TM_PTR) {
    BASIC_TO_CONV (mir_size_t, u_val);
  } else {
    switch (from->u.basic_type) {
    case TP_BOOL:
    case TP_UCHAR:
    case TP_USHORT:
    case TP_UINT:
    case TP_ULONG:
    case TP_ULLONG: BASIC_FROM_CONV (u_val); break;
    case TP_CHAR:
      if (!char_is_signed_p ()) {
        BASIC_FROM_CONV (u_val);
        break;
      }
      /* falls through */
    case TP_SCHAR:
    case TP_SHORT:
    case TP_INT:
    case TP_LONG:
    case TP_LLONG: BASIC_FROM_CONV (i_val); break;
    case TP_FLOAT:
    case TP_DOUBLE:
    case TP_LDOUBLE: BASIC_FROM_CONV (d_val); break;
    default: assert (FALSE);
    }
  }
#undef CONV
#undef BASIC_FROM_CONV
#undef BASIC_TO_CONV
}

static void convert_value (struct expr *e, struct type *to) { cast_value (e, e, to); }

static int non_reg_decl_spec_p (struct decl_spec *ds) {
  return (ds->typedef_p || ds->extern_p || ds->static_p || ds->auto_p || ds->thread_local_p
          || ds->inline_p || ds->no_return_p || ds->align_node);
}

static void create_node_scope (c2m_ctx_t c2m_ctx, node_t node) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  struct node_scope *ns = reg_malloc (c2m_ctx, sizeof (struct node_scope));

  assert (node != curr_scope);
  ns->func_scope_num = curr_func_scope_num++;
  ns->stack_var_p = FALSE;
  ns->offset = ns->size = ns->call_arg_area_size = 0;
  node->attr = ns;
  ns->scope = curr_scope;
  curr_scope = node;
}

static void finish_scope (c2m_ctx_t c2m_ctx) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;

  curr_scope = ((struct node_scope *) curr_scope->attr)->scope;
}

static void set_type_qual (c2m_ctx_t c2m_ctx, node_t r, struct type_qual *tq,
                           enum type_mode tmode) {
  for (node_t n = NL_HEAD (r->u.ops); n != NULL; n = NL_NEXT (n)) switch (n->code) {
      /* Type qualifiers: */
    case N_CONST: tq->const_p = TRUE; break;
    case N_RESTRICT:
      tq->restrict_p = TRUE;
      if (tmode != TM_PTR && tmode != TM_UNDEF)
        error (c2m_ctx, POS (n), "restrict requires a pointer");
      break;
    case N_VOLATILE: tq->volatile_p = TRUE; break;
    case N_ATOMIC:
      tq->atomic_p = TRUE;
      if (tmode == TM_ARR)
        error (c2m_ctx, POS (n), "_Atomic qualifying array");
      else if (tmode == TM_FUNC)
        error (c2m_ctx, POS (n), "_Atomic qualifying function");
      break;
    default: break; /* Ignore */
    }
}

static void check_type_duplication (c2m_ctx_t c2m_ctx, struct type *type, node_t n,
                                    const char *name, int size, int sign) {
  if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
    error (c2m_ctx, POS (n), "%s with another type", name);
  else if (type->mode != TM_BASIC && size != 0)
    error (c2m_ctx, POS (n), "size with non-numeric type");
  else if (type->mode != TM_BASIC && sign != 0)
    error (c2m_ctx, POS (n), "sign attribute with non-integer type");
}

static node_t find_def (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                        node_t *aux_node) {
  symbol_t sym;

  for (;;) {
    if (!symbol_find (c2m_ctx, mode, id, scope, &sym)) {
      if (scope == NULL) return NULL;
      scope = ((struct node_scope *) scope->attr)->scope;
    } else {
      if (aux_node) *aux_node = sym.aux_node;
      return sym.def_node;
    }
  }
}

static node_t process_tag (c2m_ctx_t c2m_ctx, node_t r, node_t id, node_t decl_list) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  symbol_t sym;
  int found_p;
  node_t scope, tab_decl_list;

  if (id->code != N_ID) return r;
  scope = curr_scope;
  while (scope != top_scope && (scope->code == N_STRUCT || scope->code == N_UNION))
    scope = ((struct node_scope *) scope->attr)->scope;
  sym.def_node = NULL; /* to remove uninitialized warning */
  if (decl_list->code != N_IGNORE) {
    found_p = symbol_find (c2m_ctx, S_TAG, id, scope, &sym);
  } else {
    sym.def_node = find_def (c2m_ctx, S_TAG, id, scope, NULL);
    found_p = sym.def_node != NULL;
  }
  if (!found_p) {
    symbol_insert (c2m_ctx, S_TAG, id, scope, r, NULL);
  } else if (sym.def_node->code != r->code) {
    error (c2m_ctx, POS (id), "kind of tag %s is unmatched with previous declaration", id->u.s.s);
  } else if ((tab_decl_list = NL_EL (sym.def_node->u.ops, 1))->code != N_IGNORE
             && decl_list->code != N_IGNORE) {
    error (c2m_ctx, POS (id), "tag %s redeclaration", id->u.s.s);
  } else {
    if (decl_list->code != N_IGNORE) { /* swap decl lists */
      DLIST (node_t) temp;
      SWAP (r->u.ops, sym.def_node->u.ops, temp);
    }
    r = sym.def_node;
  }
  return r;
}

static void def_symbol (c2m_ctx_t c2m_ctx, enum symbol_mode mode, node_t id, node_t scope,
                        node_t def_node, node_code_t linkage) {
  symbol_t sym;
  struct decl_spec tab_decl_spec, decl_spec;

  if (id->code == N_IGNORE) return;
  assert (id->code == N_ID && scope != NULL);
  assert (scope->code == N_MODULE || scope->code == N_BLOCK || scope->code == N_STRUCT
          || scope->code == N_UNION || scope->code == N_FUNC || scope->code == N_FOR);
  decl_spec = ((decl_t) def_node->attr)->decl_spec;
  if (decl_spec.thread_local_p && !decl_spec.static_p && !decl_spec.extern_p)
    error (c2m_ctx, POS (id), "auto %s is declared as thread local", id->u.s.s);
  if (!symbol_find (c2m_ctx, mode, id, scope, &sym)) {
    symbol_insert (c2m_ctx, mode, id, scope, def_node, NULL);
    return;
  }
  tab_decl_spec = ((decl_t) sym.def_node->attr)->decl_spec;
  if ((def_node->code == N_ENUM_CONST || sym.def_node->code == N_ENUM_CONST)
      && def_node->code != sym.def_node->code) {
    error (c2m_ctx, POS (id), "%s redeclared as a different kind of symbol", id->u.s.s);
    return;
  } else if (linkage == N_IGNORE) {
    if (!decl_spec.typedef_p || !tab_decl_spec.typedef_p
        || !type_eq_p (decl_spec.type, tab_decl_spec.type))
#if defined(__APPLE__)
      /* a hack to use our definition instead of macosx for non-GNU compiler */
      if (strcmp (id->u.s.s, "__darwin_va_list") != 0)
#endif
        error (c2m_ctx, POS (id), "repeated declaration %s", id->u.s.s);
  } else if (!compatible_types_p (decl_spec.type, tab_decl_spec.type, FALSE)) {
    error (c2m_ctx, POS (id), "incompatible types of %s declarations", id->u.s.s);
  }
  if (tab_decl_spec.thread_local_p != decl_spec.thread_local_p) {
    error (c2m_ctx, POS (id), "thread local and non-thread local declarations of %s", id->u.s.s);
  }
  if ((decl_spec.linkage == N_EXTERN && linkage == N_STATIC)
      || (decl_spec.linkage == N_STATIC && linkage == N_EXTERN))
    warning (c2m_ctx, POS (id), "%s defined with external and internal linkage", id->u.s.s);
  VARR_PUSH (node_t, sym.defs, def_node);
  if (incomplete_type_p (c2m_ctx, tab_decl_spec.type)) symbol_def_replace (c2m_ctx, sym, def_node);
}

static void make_type_complete (c2m_ctx_t c2m_ctx, struct type *type) {
  if (incomplete_type_p (c2m_ctx, type)) return;
  /* The type may become complete: recalculate size: */
  type->raw_size = MIR_SIZE_MAX;
  set_type_layout (c2m_ctx, type);
}

static node_t skip_struct_scopes (node_t scope) {
  for (; scope != NULL && (scope->code == N_STRUCT || scope->code == N_UNION);
       scope = ((struct node_scope *) scope->attr)->scope)
    ;
  return scope;
}
static void check (c2m_ctx_t c2m_ctx, node_t node, node_t context);

static struct decl_spec check_decl_spec (c2m_ctx_t c2m_ctx, node_t r, node_t decl_node) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  int n_sc = 0, sign = 0, size = 0, func_p = FALSE;
  struct decl_spec *res;
  struct type *type;

  if (r->attr != NULL) return *(struct decl_spec *) r->attr;
  if (decl_node->code == N_FUNC_DEF) {
    func_p = TRUE;
  } else if (decl_node->code == N_SPEC_DECL) {
    node_t declarator = NL_EL (decl_node->u.ops, 1);
    node_t list = NL_EL (declarator->u.ops, 1);

    func_p = list != NULL && NL_HEAD (list->u.ops) != NULL && NL_HEAD (list->u.ops)->code == N_FUNC;
  }
  r->attr = res = reg_malloc (c2m_ctx, sizeof (struct decl_spec));
  res->typedef_p = res->extern_p = res->static_p = FALSE;
  res->auto_p = res->register_p = res->thread_local_p = FALSE;
  res->inline_p = res->no_return_p = FALSE;
  res->align = -1;
  res->align_node = NULL;
  res->linkage = N_IGNORE;
  res->type = type = create_type (c2m_ctx, NULL);
  type->pos_node = r;
  type->mode = TM_BASIC;
  type->u.basic_type = TP_UNDEF;
  for (node_t n = NL_HEAD (r->u.ops); n != NULL; n = NL_NEXT (n))
    if (n->code == N_SIGNED || n->code == N_UNSIGNED) {
      if (sign != 0)
        error (c2m_ctx, POS (n), "more than one sign qualifier");
      else
        sign = n->code == N_SIGNED ? 1 : -1;
    } else if (n->code == N_SHORT) {
      if (size != 0)
        error (c2m_ctx, POS (n), "more than one type");
      else
        size = 1;
    } else if (n->code == N_LONG) {
      if (size == 2)
        size = 3;
      else if (size == 3)
        error (c2m_ctx, POS (n), "more than two long");
      else if (size == 1)
        error (c2m_ctx, POS (n), "short with long");
      else
        size = 2;
    }
  for (node_t n = NL_HEAD (r->u.ops); n != NULL; n = NL_NEXT (n)) switch (n->code) {
      /* Type qualifiers are already processed. */
    case N_CONST:
    case N_RESTRICT:
    case N_VOLATILE:
    case N_ATOMIC:
      break;
      /* Func specifiers: */
    case N_INLINE:
      if (!func_p)
        error (c2m_ctx, POS (n), "non-function declaration with inline");
      else
        res->inline_p = TRUE;
      break;
    case N_NO_RETURN:
      if (!func_p)
        error (c2m_ctx, POS (n), "non-function declaration with _Noreturn");
      else
        res->no_return_p = TRUE;
      break;
      /* Storage specifiers: */
    case N_TYPEDEF:
    case N_AUTO:
    case N_REGISTER:
      if (n_sc != 0)
        error (c2m_ctx, POS (n), "more than one storage specifier");
      else if (n->code == N_TYPEDEF)
        res->typedef_p = TRUE;
      else if (n->code == N_AUTO)
        res->auto_p = TRUE;
      else
        res->register_p = TRUE;
      n_sc++;
      break;
    case N_EXTERN:
    case N_STATIC:
      if (n_sc != 0 && (n_sc != 1 || !res->thread_local_p))
        error (c2m_ctx, POS (n), "more than one storage specifier");
      else if (n->code == N_EXTERN)
        res->extern_p = TRUE;
      else
        res->static_p = TRUE;
      n_sc++;
      break;
    case N_THREAD_LOCAL:
      if (n_sc != 0 && (n_sc != 1 || (!res->extern_p && !res->static_p)))
        error (c2m_ctx, POS (n), "more than one storage specifier");
      else
        res->thread_local_p = TRUE;
      n_sc++;
      break;
    case N_VOID:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, POS (n), "void with another type");
      else if (sign != 0)
        error (c2m_ctx, POS (n), "void with sign qualifier");
      else if (size != 0)
        error (c2m_ctx, POS (n), "void with short or long");
      else
        type->u.basic_type = TP_VOID;
      break;
    case N_UNSIGNED:
    case N_SIGNED:
    case N_SHORT:
    case N_LONG: set_type_pos_node (type, n); break;
    case N_CHAR:
    case N_INT:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF) {
        error (c2m_ctx, POS (n), "char or int with another type");
      } else if (n->code == N_CHAR) {
        if (size != 0)
          error (c2m_ctx, POS (n), "char with short or long");
        else
          type->u.basic_type = sign == 0 ? TP_CHAR : sign < 0 ? TP_UCHAR : TP_SCHAR;
      } else if (size == 0)
        type->u.basic_type = sign >= 0 ? TP_INT : TP_UINT;
      else if (size == 1)
        type->u.basic_type = sign >= 0 ? TP_SHORT : TP_USHORT;
      else if (size == 2)
        type->u.basic_type = sign >= 0 ? TP_LONG : TP_ULONG;
      else
        type->u.basic_type = sign >= 0 ? TP_LLONG : TP_ULLONG;
      break;
    case N_BOOL:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, POS (n), "_Bool with another type");
      else if (sign != 0)
        error (c2m_ctx, POS (n), "_Bool with sign qualifier");
      else if (size != 0)
        error (c2m_ctx, POS (n), "_Bool with short or long");
      type->u.basic_type = TP_BOOL;
      break;
    case N_FLOAT:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, POS (n), "float with another type");
      else if (sign != 0)
        error (c2m_ctx, POS (n), "float with sign qualifier");
      else if (size != 0)
        error (c2m_ctx, POS (n), "float with short or long");
      else
        type->u.basic_type = TP_FLOAT;
      break;
    case N_DOUBLE:
      set_type_pos_node (type, n);
      if (type->mode != TM_BASIC || type->u.basic_type != TP_UNDEF)
        error (c2m_ctx, POS (n), "double with another type");
      else if (sign != 0)
        error (c2m_ctx, POS (n), "double with sign qualifier");
      else if (size == 0)
        type->u.basic_type = TP_DOUBLE;
      else if (size == 2)
        type->u.basic_type = TP_LDOUBLE;
      else
        error (c2m_ctx, POS (n), "double with short");
      break;
    case N_ID: {
      node_t def = find_def (c2m_ctx, S_REGULAR, n, skip_struct_scopes (curr_scope), NULL);
      decl_t decl;

      set_type_pos_node (type, n);
      if (def == NULL) {
        error (c2m_ctx, POS (n), "unknown type %s", n->u.s.s);
        init_type (type);
        type->mode = TM_BASIC;
        type->u.basic_type = TP_INT;
      } else {
        assert (def->code == N_SPEC_DECL);
        decl = def->attr;
        decl->used_p = TRUE;
        assert (decl->decl_spec.typedef_p);
        *type = *decl->decl_spec.type;
      }
      break;
    }
    case N_STRUCT:
    case N_UNION: {
      int new_scope_p;
      node_t res_tag_type, id = NL_HEAD (n->u.ops);
      node_t decl_list = NL_NEXT (id);
      node_t saved_unnamed_anon_struct_union_member = curr_unnamed_anon_struct_union_member;

      set_type_pos_node (type, n);
      res_tag_type = process_tag (c2m_ctx, n, id, decl_list);
      check_type_duplication (c2m_ctx, type, n, n->code == N_STRUCT ? "struct" : "union", size,
                              sign);
      type->mode = n->code == N_STRUCT ? TM_STRUCT : TM_UNION;
      type->u.tag_type = res_tag_type;
      new_scope_p = (id->code != N_IGNORE || decl_node->code != N_MEMBER
                     || NL_EL (decl_node->u.ops, 1)->code != N_IGNORE);
      type->unnamed_anon_struct_union_member_type_p = !new_scope_p;
      curr_unnamed_anon_struct_union_member = new_scope_p ? NULL : decl_node;
      if (decl_list->code != N_IGNORE) {
        if (new_scope_p) create_node_scope (c2m_ctx, res_tag_type);
        check (c2m_ctx, decl_list, n);
        if (new_scope_p) finish_scope (c2m_ctx);
        if (res_tag_type != n) make_type_complete (c2m_ctx, type); /* recalculate size */
      }
      curr_unnamed_anon_struct_union_member = saved_unnamed_anon_struct_union_member;
      break;
    }
    case N_ENUM: {
      node_t res_tag_type, id = NL_HEAD (n->u.ops);
      node_t enum_list = NL_NEXT (id);
      node_t enum_const_scope = skip_struct_scopes (curr_scope);

      set_type_pos_node (type, n);
      res_tag_type = process_tag (c2m_ctx, n, id, enum_list);
      check_type_duplication (c2m_ctx, type, n, "enum", size, sign);
      type->mode = TM_ENUM;
      type->u.tag_type = res_tag_type;
      if (enum_list->code == N_IGNORE) {
        if (incomplete_type_p (c2m_ctx, type))
          error (c2m_ctx, POS (n), "enum storage size is unknown");
      } else {
        mir_llong curr_val = -1, min_val = 0;
        mir_ullong max_val = 0;
        struct enum_type *enum_type;
        int neg_p = FALSE;

        n->attr = enum_type = reg_malloc (c2m_ctx, sizeof (struct enum_type));
        enum_type->enum_basic_type = TP_INT;                                           // ???
        for (node_t en = NL_HEAD (enum_list->u.ops); en != NULL; en = NL_NEXT (en)) {  // ??? id
          node_t const_expr;
          symbol_t sym;
          struct enum_value *enum_value;

          assert (en->code == N_ENUM_CONST);
          id = NL_HEAD (en->u.ops);
          const_expr = NL_NEXT (id);
          check (c2m_ctx, const_expr, n);
          if (symbol_find (c2m_ctx, S_REGULAR, id, enum_const_scope, &sym)) {
            error (c2m_ctx, POS (id), "enum constant %s redeclaration", id->u.s.s);
          } else {
            symbol_insert (c2m_ctx, S_REGULAR, id, enum_const_scope, en, n);
          }
          curr_val++;
          if (curr_val == 0) neg_p = FALSE;
          if (const_expr->code != N_IGNORE) {
            struct expr *cexpr = const_expr->attr;

            if (!cexpr->const_p) {
              error (c2m_ctx, POS (const_expr), "non-constant value in enum const expression");
              continue;
            } else if (!integer_type_p (cexpr->type)) {
              error (c2m_ctx, POS (const_expr), "enum const expression is not of an integer type");
              continue;
            }
            curr_val = cexpr->c.i_val;
            neg_p = signed_integer_type_p (cexpr->type) && cexpr->c.i_val < 0;
          }
          en->attr = enum_value = reg_malloc (c2m_ctx, sizeof (struct enum_value));
          if (!neg_p) {
            if (max_val < (mir_ullong) curr_val) max_val = (mir_ullong) curr_val;
            if (min_val < 0 && (mir_ullong) curr_val >= MIR_LLONG_MAX)
              error (c2m_ctx, POS (const_expr),
                     "enum const expression is not represented by an int");
            enum_value->u.u_val = (mir_ullong) curr_val;
          } else {
            if (min_val > curr_val) {
              min_val = curr_val;
              if (min_val < 0 && max_val >= MIR_LLONG_MAX)
                error (c2m_ctx, POS (const_expr),
                       "enum const expression is not represented by an int");
            } else if (curr_val >= 0 && max_val < (mir_ullong) curr_val) {
              max_val = curr_val;
            }
            enum_value->u.i_val = curr_val;
          }
          enum_type->enum_basic_type
            = (max_val <= MIR_INT_MAX && MIR_INT_MIN <= min_val     ? TP_INT
               : max_val <= MIR_UINT_MAX && 0 <= min_val            ? TP_UINT
               : max_val <= MIR_LONG_MAX && MIR_LONG_MIN <= min_val ? TP_LONG
               : max_val <= MIR_ULONG_MAX && 0 <= min_val           ? TP_ULONG
               : min_val < 0 || max_val <= MIR_LLONG_MAX            ? TP_LLONG
                                                                    : TP_ULLONG);
        }
      }
      break;
    }
    case N_ALIGNAS: {
      node_t el;
      int align = -1;

      if (decl_node->code == N_FUNC_DEF) {
        error (c2m_ctx, POS (n), "_Alignas for function");
      } else if (decl_node->code == N_MEMBER && (el = NL_EL (decl_node->u.ops, 3)) != NULL
                 && el->code != N_IGNORE) {
        error (c2m_ctx, POS (n), "_Alignas for a bit-field");
      } else if (decl_node->code == N_SPEC_DECL && in_params_p) {
        error (c2m_ctx, POS (n), "_Alignas for a function parameter");
      } else {
        node_t op = NL_HEAD (n->u.ops);

        check (c2m_ctx, op, n);
        if (op->code == N_TYPE) {
          struct decl_spec *decl_spec = op->attr;

          align = type_align (decl_spec->type);
        } else {
          struct expr *cexpr = op->attr;

          if (!cexpr->const_p) {
            error (c2m_ctx, POS (op), "non-constant value in _Alignas");
          } else if (!integer_type_p (cexpr->type)) {
            error (c2m_ctx, POS (op), "constant value in _Alignas is not of an integer type");
          } else if (!signed_integer_type_p (cexpr->type)
                     || !supported_alignment_p (cexpr->c.i_val)) {
            error (c2m_ctx, POS (op), "constant value in _Alignas specifies unsupported alignment");
          } else if (invalid_alignment (cexpr->c.i_val)) {
            error (c2m_ctx, POS (op), "unsupported alignmnent");
          } else {
            align = (int) cexpr->c.i_val;
          }
        }
        if (align != 0 && res->align < align) {
          res->align = align;
          res->align_node = n;
        }
      }
      break;
    }
    default: abort ();
    }
  if (type->mode == TM_BASIC && type->u.basic_type == TP_UNDEF) {
    if (size == 0 && sign == 0) {
      (c2m_options->pedantic_p ? error (c2m_ctx, POS (r), "no any type specifier")
                               : warning (c2m_ctx, POS (r), "type defaults to int"));
      type->u.basic_type = TP_INT;
    } else if (size == 0) {
      type->u.basic_type = sign >= 0 ? TP_INT : TP_UINT;
    } else if (size == 1) {
      type->u.basic_type = sign >= 0 ? TP_SHORT : TP_USHORT;
    } else if (size == 2) {
      type->u.basic_type = sign >= 0 ? TP_LONG : TP_ULONG;
    } else {
      type->u.basic_type = sign >= 0 ? TP_LLONG : TP_ULLONG;
    }
  }
  set_type_qual (c2m_ctx, r, &type->type_qual, type->mode);
  if (res->align_node) {
    if (res->typedef_p)
      error (c2m_ctx, POS (res->align_node), "_Alignas in typedef");
    else if (res->register_p)
      error (c2m_ctx, POS (res->align_node), "_Alignas with register");
  }
  return *res;
}

static struct type *append_type (struct type *head, struct type *el) {
  struct type **holder;

  if (head == NULL) return el;
  if (head->mode == TM_PTR) {
    holder = &head->u.ptr_type;
  } else if (head->mode == TM_ARR) {
    holder = &head->u.arr_type->el_type;
  } else {
    assert (head->mode == TM_FUNC);
    holder = &head->u.func_type->ret_type;
  }
  *holder = append_type (*holder, el);
  return head;
}

static int void_param_p (node_t param) {
  struct decl_spec *decl_spec;
  struct type *type;

  if (param != NULL && param->code == N_TYPE) {
    decl_spec = param->attr;
    type = decl_spec->type;
    if (void_type_p (type)) return TRUE;
  }
  return FALSE;
}

static void adjust_param_type (c2m_ctx_t c2m_ctx, struct type **type_ptr) {
  struct type *par_type, *type = *type_ptr;
  struct arr_type *arr_type;

  if (type->mode == TM_ARR) {  // ??? static, old type qual
    arr_type = type->u.arr_type;
    par_type = create_type (c2m_ctx, NULL);
    par_type->mode = TM_PTR;
    par_type->pos_node = type->pos_node;
    par_type->u.ptr_type = arr_type->el_type;
    par_type->type_qual = arr_type->ind_type_qual;
    par_type->arr_type = type;
    *type_ptr = type = par_type;
    make_type_complete (c2m_ctx, type);
  } else if (type->mode == TM_FUNC) {
    par_type = create_type (c2m_ctx, NULL);
    par_type->mode = TM_PTR;
    par_type->pos_node = type->pos_node;
    par_type->func_type_before_adjustment_p = TRUE;
    par_type->u.ptr_type = type;
    *type_ptr = type = par_type;
    make_type_complete (c2m_ctx, type);
  }
}

static struct type *check_declarator (c2m_ctx_t c2m_ctx, node_t r, int func_def_p) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  struct type *type, *res = NULL;
  node_t list = NL_EL (r->u.ops, 1);

  assert (r->code == N_DECL);
  if (NL_HEAD (list->u.ops) == NULL) return NULL;
  for (node_t n = NL_HEAD (list->u.ops); n != NULL; n = NL_NEXT (n)) {
    type = create_type (c2m_ctx, NULL);
    type->pos_node = n;
    switch (n->code) {
    case N_POINTER: {
      node_t type_qual = NL_HEAD (n->u.ops);

      type->mode = TM_PTR;
      type->pos_node = n;
      type->u.ptr_type = NULL;
      set_type_qual (c2m_ctx, type_qual, &type->type_qual, TM_PTR);
      break;
    }
    case N_ARR: {
      struct arr_type *arr_type;
      node_t static_node = NL_HEAD (n->u.ops);
      node_t type_qual = NL_NEXT (static_node);
      node_t size = NL_NEXT (type_qual);

      type->mode = TM_ARR;
      type->pos_node = n;
      type->u.arr_type = arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));
      clear_type_qual (&arr_type->ind_type_qual);
      set_type_qual (c2m_ctx, type_qual, &arr_type->ind_type_qual, TM_UNDEF);
      check (c2m_ctx, size, n);
      arr_type->size = size;
      arr_type->static_p = static_node->code == N_STATIC;
      arr_type->el_type = NULL;
      break;
    }
    case N_FUNC: {
      struct func_type *func_type;
      node_t first_param, param_list = NL_HEAD (n->u.ops);
      node_t last = NL_TAIL (param_list->u.ops);
      int saved_in_params_p = in_params_p;

      type->mode = TM_FUNC;
      type->pos_node = n;
      type->u.func_type = func_type = reg_malloc (c2m_ctx, sizeof (struct func_type));
      func_type->ret_type = NULL;
      func_type->proto_item = NULL;
      if ((func_type->dots_p = last != NULL && last->code == N_DOTS))
        NL_REMOVE (param_list->u.ops, last);
      if (!func_def_p) create_node_scope (c2m_ctx, n);
      func_type->param_list = param_list;
      in_params_p = TRUE;
      first_param = NL_HEAD (param_list->u.ops);
      if (first_param != NULL && first_param->code != N_ID) check (c2m_ctx, first_param, n);
      if (void_param_p (first_param)) {
        struct decl_spec *ds = first_param->attr;

        if (non_reg_decl_spec_p (ds) || ds->register_p
            || !type_qual_eq_p (&ds->type->type_qual, &zero_type_qual)) {
          error (c2m_ctx, POS (first_param), "qualified void parameter");
        }
        if (NL_NEXT (first_param) != NULL) {
          error (c2m_ctx, POS (first_param), "void must be the only parameter");
        }
      } else {
        for (node_t p = first_param; p != NULL; p = NL_NEXT (p)) {
          struct decl_spec *decl_spec_ptr;

          if (p->code == N_ID) {
            if (!func_def_p)
              error (c2m_ctx, POS (p),
                     "parameters identifier list can be only in function definition");
            break;
          } else {
            if (p != first_param) check (c2m_ctx, p, n);
            decl_spec_ptr = get_param_decl_spec (p);
            adjust_param_type (c2m_ctx, &decl_spec_ptr->type);
          }
        }
      }
      in_params_p = saved_in_params_p;
      if (!func_def_p) finish_scope (c2m_ctx);
      break;
    }
    default: abort ();
    }
    res = append_type (res, type);
  }
  return res;
}

static int check_case_expr (c2m_ctx_t c2m_ctx, node_t case_expr, struct type *type, node_t target) {
  struct expr *expr;

  check (c2m_ctx, case_expr, target);
  expr = case_expr->attr;
  if (!expr->const_p) {
    error (c2m_ctx, POS (case_expr), "case-expr is not a constant expression");
    return FALSE;
  } else if (!integer_type_p (expr->type)) {
    error (c2m_ctx, POS (case_expr), "case-expr is not an integer type expression");
    return FALSE;
  } else {
    convert_value (expr, type);
    return TRUE;
  }
}

static void check_labels (c2m_ctx_t c2m_ctx, node_t labels, node_t target) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;

  for (node_t l = NL_HEAD (labels->u.ops); l != NULL; l = NL_NEXT (l)) {
    if (l->code == N_LABEL) {
      symbol_t sym;
      node_t id = NL_HEAD (l->u.ops);

      if (symbol_find (c2m_ctx, S_LABEL, id, func_block_scope, &sym)) {
        error (c2m_ctx, POS (id), "label %s redeclaration", id->u.s.s);
      } else {
        symbol_insert (c2m_ctx, S_LABEL, id, func_block_scope, target, NULL);
      }
    } else if (curr_switch == NULL) {
      error (c2m_ctx, POS (l), "%s not within a switch-stmt",
             l->code == N_CASE ? "case label" : "default label");
    } else {
      struct switch_attr *switch_attr = curr_switch->attr;
      struct type *type = &switch_attr->type;
      node_t case_expr = l->code == N_CASE ? NL_HEAD (l->u.ops) : NULL;
      node_t case_expr2 = l->code == N_CASE ? NL_EL (l->u.ops, 1) : NULL;
      case_t case_attr, tail = DLIST_TAIL (case_t, switch_attr->case_labels);
      int ok_p = FALSE, default_p = tail != NULL && tail->case_node->code == N_DEFAULT;

      if (case_expr == NULL) {
        if (default_p) {
          error (c2m_ctx, POS (l), "multiple default labels in one switch");
        } else {
          ok_p = TRUE;
        }
      } else {
        ok_p = check_case_expr (c2m_ctx, case_expr, type, target);
        if (case_expr2 != NULL) {
          ok_p = check_case_expr (c2m_ctx, case_expr2, type, target) && ok_p;
          (c2m_options->pedantic_p ? error : warning) (c2m_ctx, POS (l),
                                                       "range cases are not a part of C standard");
        }
      }
      if (ok_p) {
        case_attr = reg_malloc (c2m_ctx, sizeof (struct case_attr));
        case_attr->case_node = l;
        case_attr->case_target_node = target;
        if (default_p) {
          DLIST_INSERT_BEFORE (case_t, switch_attr->case_labels, tail, case_attr);
        } else {
          DLIST_APPEND (case_t, switch_attr->case_labels, case_attr);
        }
      }
    }
  }
}

static node_code_t get_id_linkage (c2m_ctx_t c2m_ctx, int func_p, node_t id, node_t scope,
                                   struct decl_spec decl_spec) {
  node_code_t linkage;
  node_t def = find_def (c2m_ctx, S_REGULAR, id, scope, NULL);

  if (decl_spec.typedef_p) return N_IGNORE;                       // p6: no linkage
  if (decl_spec.static_p && scope == top_scope) return N_STATIC;  // p3: internal linkage
  if (decl_spec.extern_p && def != NULL
      && (linkage = ((decl_t) def->attr)->decl_spec.linkage) != N_IGNORE)
    return linkage;  // p4: previous linkage
  if (decl_spec.extern_p && (def == NULL || ((decl_t) def->attr)->decl_spec.linkage == N_IGNORE))
    return N_EXTERN;  // p4: external linkage
  if (!decl_spec.static_p && !decl_spec.extern_p && (scope == top_scope || func_p))
    return N_EXTERN;                                                          // p5
  if (!decl_spec.extern_p && scope != top_scope && !func_p) return N_IGNORE;  // p6: no linkage
  return N_IGNORE;
}

static void check_type (c2m_ctx_t c2m_ctx, struct type *type, int level, int func_def_p) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;

  switch (type->mode) {
  case TM_PTR: check_type (c2m_ctx, type->u.ptr_type, level + 1, FALSE); break;
  case TM_STRUCT:
  case TM_UNION: break;
  case TM_ARR: {
    struct arr_type *arr_type = type->u.arr_type;
    node_t size_node = arr_type->size;
    struct type *el_type = arr_type->el_type;

    if (size_node->code == N_STAR) {
      error (c2m_ctx, POS (size_node), "variable size arrays are not supported");
    } else if (size_node->code != N_IGNORE) {
      struct expr *cexpr = size_node->attr;

      if (!integer_type_p (cexpr->type)) {
        error (c2m_ctx, POS (size_node), "non-integer array size type");
      } else if (!cexpr->const_p) {
        error (c2m_ctx, POS (size_node), "variable size arrays are not supported");
      } else if (signed_integer_type_p (cexpr->type) && cexpr->c.i_val < 0) {
        error (c2m_ctx, POS (size_node), "array size should be not negative");
      } else if (cexpr->c.i_val == 0) {
        (c2m_options->pedantic_p ? error : warning) (c2m_ctx, POS (size_node), "zero array size");
      }
    }
    check_type (c2m_ctx, el_type, level + 1, FALSE);
    if (el_type->mode == TM_FUNC) {
      error (c2m_ctx, POS (type->pos_node), "array of functions");
    } else if (incomplete_type_p (c2m_ctx, el_type)) {
      error (c2m_ctx, POS (type->pos_node), "incomplete array element type");
    } else if (!in_params_p || level != 0) {
      if (arr_type->static_p)
        error (c2m_ctx, POS (type->pos_node), "static should be only in parameter outermost");
      else if (!type_qual_eq_p (&arr_type->ind_type_qual, &zero_type_qual))
        error (c2m_ctx, POS (type->pos_node),
               "type qualifiers should be only in parameter outermost array");
    }
    break;
  }
  case TM_FUNC: {
    struct decl_spec decl_spec;
    struct func_type *func_type = type->u.func_type;
    struct type *ret_type = func_type->ret_type;
    node_t first_param, param_list = func_type->param_list;

    check_type (c2m_ctx, ret_type, level + 1, FALSE);
    if (ret_type->mode == TM_FUNC) {
      error (c2m_ctx, POS (ret_type->pos_node), "function returning a function");
    } else if (ret_type->mode == TM_ARR) {
      error (c2m_ctx, POS (ret_type->pos_node), "function returning an array");
    }
    first_param = NL_HEAD (param_list->u.ops);
    if (!void_param_p (first_param)) {
      for (node_t p = first_param; p != NULL; p = NL_NEXT (p)) {
        if (p->code == N_TYPE) {
          decl_spec = *((struct decl_spec *) p->attr);
          check_type (c2m_ctx, decl_spec.type, level + 1, FALSE);
        } else if (p->code == N_SPEC_DECL) {
          decl_spec = ((decl_t) p->attr)->decl_spec;
          check_type (c2m_ctx, decl_spec.type, level + 1, FALSE);
        } else {
          assert (p->code == N_ID);
          break;
        }
        if (non_reg_decl_spec_p (&decl_spec)) {
          error (c2m_ctx, POS (p), "prohibited specifier in a function parameter");
        } else if (func_def_p) {
          if (p->code == N_TYPE)
            error (c2m_ctx, POS (p), "parameter type without a name in function definition");
          else if (incomplete_type_p (c2m_ctx, decl_spec.type))
            error (c2m_ctx, POS (p), "incomplete parameter type in function definition");
        }
      }
    }
    break;
  }
  default: break;  // ???
  }
}

static void check_assignment_types (c2m_ctx_t c2m_ctx, struct type *left, struct type *right,
                                    struct expr *expr, node_t assign_node) {
  node_code_t code = assign_node->code;
  const char *msg;

  if (right == NULL) right = expr->type;
  if (arithmetic_type_p (left)) {
    if (!arithmetic_type_p (right)
        && !(left->mode == TM_BASIC && left->u.basic_type == TP_BOOL && right->mode == TM_PTR)) {
      if (integer_type_p (left) && right->mode == TM_PTR) {
        msg = (code == N_CALL     ? "using pointer without cast for integer type parameter"
               : code == N_RETURN ? "returning pointer without cast for integer result"
                                  : "assigning pointer without cast to integer");
        (c2m_options->pedantic_p ? error : warning) (c2m_ctx, POS (assign_node), "%s", msg);
      } else {
        msg = (code == N_CALL ? "incompatible argument type for arithmetic type parameter"
               : code != N_RETURN
                 ? "incompatible types in assignment to an arithmetic type lvalue"
                 : "incompatible return-expr type in function returning an arithmetic value");
        error (c2m_ctx, POS (assign_node), "%s", msg);
      }
    }
  } else if (left->mode == TM_STRUCT || left->mode == TM_UNION) {
    if ((right->mode != TM_STRUCT && right->mode != TM_UNION)
        || !compatible_types_p (left, right, TRUE)) {
      msg = (code == N_CALL ? "incompatible argument type for struct/union type parameter"
             : code != N_RETURN
               ? "incompatible types in assignment to struct/union"
               : "incompatible return-expr type in function returning a struct/union");
      error (c2m_ctx, POS (assign_node), "%s", msg);
    }
  } else if (left->mode == TM_PTR) {
    if (null_const_p (expr, right)) {
    } else if (right->mode != TM_PTR
               || !(compatible_types_p (left->u.ptr_type, right->u.ptr_type, TRUE)
                    || (void_ptr_p (left) || void_ptr_p (right))
                    || (left->u.ptr_type->mode == TM_ARR
                        && compatible_types_p (left->u.ptr_type->u.arr_type->el_type,
                                               right->u.ptr_type, TRUE)))) {
      if (right->mode == TM_PTR && left->u.ptr_type->mode == TM_BASIC
          && right->u.ptr_type->mode == TM_BASIC) {
        msg = (code == N_CALL     ? "incompatible pointer types of argument and parameter"
               : code == N_RETURN ? "incompatible pointer types of return-expr and function result"
                                  : "incompatible pointer types in assignment");
        int sign_diff_p = char_type_p (left->u.ptr_type) && char_type_p (right->u.ptr_type);
        if (!sign_diff_p || c2m_options->pedantic_p)
          (c2m_options->pedantic_p && !sign_diff_p ? error : warning) (c2m_ctx, POS (assign_node),
                                                                       "%s", msg);
      } else if (integer_type_p (right)) {
        msg = (code == N_CALL     ? "using integer without cast for pointer type parameter"
               : code == N_RETURN ? "returning integer without cast for pointer result"
                                  : "assigning integer without cast to pointer");
        (c2m_options->pedantic_p ? error : warning) (c2m_ctx, POS (assign_node), "%s", msg);
      } else {
        msg = (code == N_CALL     ? "incompatible argument type for pointer type parameter"
               : code == N_RETURN ? "incompatible return-expr type in function returning a pointer"
                                  : "incompatible types in assignment to a pointer");
        (c2m_options->pedantic_p || right->mode != TM_PTR ? error : warning) (c2m_ctx,
                                                                              POS (assign_node),
                                                                              "%s", msg);
      }
    } else if (right->u.ptr_type->type_qual.atomic_p) {
      msg = (code == N_CALL     ? "passing a pointer of an atomic type"
             : code == N_RETURN ? "returning a pointer of an atomic type"
                                : "assignment of pointer of an atomic type");
      error (c2m_ctx, POS (assign_node), "%s", msg);
    } else if (!type_qual_subset_p (&right->u.ptr_type->type_qual, &left->u.ptr_type->type_qual)) {
      msg = (code == N_CALL     ? "discarding type qualifiers in passing argument"
             : code == N_RETURN ? "return discards a type qualifier from a pointer"
                                : "assignment discards a type qualifier from a pointer");
      (c2m_options->pedantic_p ? error : warning) (c2m_ctx, POS (assign_node), "%s", msg);
    }
  } else {
    msg = (code == N_CALL     ? "passing assign incompatible value"
           : code == N_RETURN ? "returning assign incompatible value"
                              : "assignment of incompatible value");
    error (c2m_ctx, POS (assign_node), "%s", msg);
  }
}

static int anon_struct_union_type_member_p (node_t member) {
  decl_t decl = member->attr;

  return decl != NULL && decl->decl_spec.type->unnamed_anon_struct_union_member_type_p;
}

static node_t get_adjacent_member (node_t member, int next_p) {
  assert (member->code == N_MEMBER);
  while ((member = next_p ? NL_NEXT (member) : NL_PREV (member)) != NULL)
    if (member->code == N_MEMBER
        && (NL_EL (member->u.ops, 1)->code != N_IGNORE || anon_struct_union_type_member_p (member)))
      break;
  return member;
}

static int update_init_object_path (c2m_ctx_t c2m_ctx, size_t mark, struct type *value_type,
                                    int list_p) {
  init_object_t init_object;
  struct type *el_type;
  node_t size_node;
  mir_llong size_val;
  struct expr *sexpr;

  for (;;) {
    for (;;) {
      if (mark == VARR_LENGTH (init_object_t, init_object_path)) return FALSE;
      init_object = VARR_LAST (init_object_t, init_object_path);
      if (init_object.container_type->mode == TM_ARR) {
        el_type = init_object.container_type->u.arr_type->el_type;
        size_node = init_object.container_type->u.arr_type->size;
        sexpr = size_node->attr;
        size_val = (size_node->code != N_IGNORE && sexpr->const_p && integer_type_p (sexpr->type)
                      ? sexpr->c.i_val
                      : -1);
        init_object.u.curr_index++;
        if (size_val < 0 || init_object.u.curr_index < size_val) break;
        VARR_POP (init_object_t, init_object_path);
      } else {
        assert (init_object.container_type->mode == TM_STRUCT
                || init_object.container_type->mode == TM_UNION);
        if (init_object.u.curr_member == NULL) { /* finding the first named member */
          node_t declaration_list = NL_EL (init_object.container_type->u.tag_type->u.ops, 1);

          assert (declaration_list != NULL && declaration_list->code == N_LIST);
          for (init_object.u.curr_member = NL_HEAD (declaration_list->u.ops);
               init_object.u.curr_member != NULL
               && (init_object.u.curr_member->code != N_MEMBER
                   || (NL_EL (init_object.u.curr_member->u.ops, 1)->code == N_IGNORE
                       && !anon_struct_union_type_member_p (init_object.u.curr_member)));
               init_object.u.curr_member = NL_NEXT (init_object.u.curr_member))
            ;
        } else if (init_object.container_type->mode == TM_UNION
                   && !init_object.field_designator_p) { /* no next union member: */
          init_object.u.curr_member = NULL;
        } else { /* finding the next named struct member: */
          init_object.u.curr_member = get_adjacent_member (init_object.u.curr_member, TRUE);
        }
        if (init_object.u.curr_member != NULL) {
          init_object.field_designator_p = FALSE;
          el_type = ((decl_t) init_object.u.curr_member->attr)->decl_spec.type;
          break;
        }
        VARR_POP (init_object_t, init_object_path);
      }
    }
    VARR_SET (init_object_t, init_object_path, VARR_LENGTH (init_object_t, init_object_path) - 1,
              init_object);
    if (list_p || scalar_type_p (el_type) || void_type_p (el_type)) return TRUE;
    assert (el_type->mode == TM_ARR || el_type->mode == TM_STRUCT || el_type->mode == TM_UNION);
    if (el_type->mode != TM_ARR && value_type != NULL
        && el_type->u.tag_type == value_type->u.tag_type)
      return TRUE;
    init_object.container_type = el_type;
    init_object.field_designator_p = FALSE;
    if (el_type->mode == TM_ARR) {
      init_object.u.curr_index = -1;
    } else {
      init_object.u.curr_member = NULL;
    }
    VARR_PUSH (init_object_t, init_object_path, init_object);
  }
}

static enum basic_type get_uint_basic_type (size_t size) {
  if (sizeof (mir_uint) == size) return TP_UINT;
  if (sizeof (mir_ulong) == size) return TP_ULONG;
  if (sizeof (mir_ullong) == size) return TP_ULLONG;
  if (sizeof (mir_ushort) == size) return TP_USHORT;
  return TP_UCHAR;
}

static int init_compatible_string_p (node_t n, struct type *el_type) {
  return ((n->code == N_STR && char_type_p (el_type))
          || (n->code == N_STR16 && el_type->mode == TM_BASIC
              && el_type->u.basic_type == get_uint_basic_type (2))
          || (n->code == N_STR32 && el_type->mode == TM_BASIC
              && el_type->u.basic_type == get_uint_basic_type (4)));
}

static int update_path_and_do (c2m_ctx_t c2m_ctx, int go_inside_p,
                               void (*action) (c2m_ctx_t c2m_ctx, decl_t member_decl,
                                               struct type **type_ptr, node_t initializer,
                                               int const_only_p, int top_p),
                               size_t mark, node_t value, int const_only_p, mir_llong *max_index,
                               pos_t pos, const char *detail) {
  init_object_t init_object;
  mir_llong index;
  struct type *el_type;
  struct expr *value_expr = value->attr;

  if (!update_init_object_path (c2m_ctx, mark, value_expr == NULL ? NULL : value_expr->type,
                                !go_inside_p || value->code == N_LIST
                                  || value->code == N_COMPOUND_LITERAL)) {
    error (c2m_ctx, pos, "excess elements in %s initializer", detail);
    return FALSE;
  }
  if (!go_inside_p) return TRUE;
  init_object = VARR_LAST (init_object_t, init_object_path);
  if (init_object.container_type->mode == TM_ARR) {
    el_type = init_object.container_type->u.arr_type->el_type;
    action (c2m_ctx, NULL,
            (init_compatible_string_p (value, el_type)
               ? &init_object.container_type
               : &init_object.container_type->u.arr_type->el_type),
            value, const_only_p, FALSE);
  } else if (init_object.container_type->mode == TM_STRUCT
             || init_object.container_type->mode == TM_UNION) {
    action (c2m_ctx, (decl_t) init_object.u.curr_member->attr,
            &((decl_t) init_object.u.curr_member->attr)->decl_spec.type, value, const_only_p,
            FALSE);
  }
  if (max_index != NULL) {
    init_object = VARR_GET (init_object_t, init_object_path, mark);
    if (init_object.container_type->mode == TM_ARR
        && *max_index < (index = init_object.u.curr_index))
      *max_index = index;
  }
  return TRUE;
}

static int check_const_addr_p (c2m_ctx_t c2m_ctx, node_t r, node_t *base, mir_llong *offset,
                               int *deref) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  struct expr *e = r->attr;
  struct type *type;
  node_t op1, op2, temp;
  decl_t decl;
  struct decl_spec *decl_spec;
  mir_size_t size;

  if (e->const_p && integer_type_p (e->type)) {
    *base = NULL;
    *offset = (mir_size_t) e->c.u_val;
    *deref = 0;
    return TRUE;
  }
  switch (r->code) {
  case N_STR:
  case N_STR16:
  case N_STR32:
    *base = r;
    *offset = 0;
    *deref = 0;
    return curr_scope == top_scope;
  case N_LABEL_ADDR:
    *base = r;
    *offset = 0;
    *deref = 0;
    return TRUE;
  case N_ID:
    if (e->def_node == NULL)
      return FALSE;
    else if (e->def_node->code == N_FUNC_DEF
             || (e->def_node->code == N_SPEC_DECL
                 && ((decl_t) e->def_node->attr)->decl_spec.type->mode == TM_FUNC)) {
      *base = e->def_node;
      *deref = 0;
    } else if (e->u.lvalue_node == NULL
               || ((decl = e->u.lvalue_node->attr)->scope != top_scope
                   && decl->decl_spec.linkage != N_IGNORE)) {
      return FALSE;
    } else {
      *base = e->def_node;
      *deref = e->type->arr_type == NULL;
    }
    *offset = 0;
    return TRUE;
  case N_DEREF:
  case N_ADDR: {
    node_t op = NL_HEAD (r->u.ops);
    struct expr *op_e = op->attr;

    if (!check_const_addr_p (c2m_ctx, op, base, offset, deref)) return FALSE;
    if (r->code == N_ADDR
        && (op_e->type->mode == TM_ARR
            || (op_e->type->mode == TM_PTR && op_e->type->arr_type != NULL))) {
      if (*deref > 0) (*deref)--;
    } else if (op->code != N_ID
               || (op_e->def_node->code != N_FUNC_DEF
                   && (op_e->def_node->code != N_SPEC_DECL
                       || ((decl_t) op_e->def_node->attr)->decl_spec.type->mode != TM_FUNC))) {
      r->code == N_DEREF ? (*deref)++ : (*deref)--;
    }
    return TRUE;
  }
  case N_FIELD:
  case N_DEREF_FIELD:
    if (!check_const_addr_p (c2m_ctx, NL_HEAD (r->u.ops), base, offset, deref)) return FALSE;
    if (*deref != (r->code == N_FIELD ? 1 : 0)) return FALSE;
    *deref = 1;
    e = r->attr;
    if (e->u.lvalue_node != NULL) {
      decl = e->u.lvalue_node->attr;
      *offset += decl->offset;
    }
    return TRUE;
  case N_IND:
    if (((struct expr *) NL_HEAD (r->u.ops)->attr)->type->mode != TM_PTR) return FALSE;
    if (!check_const_addr_p (c2m_ctx, NL_HEAD (r->u.ops), base, offset, deref)) return FALSE;
    if (!(e = NL_EL (r->u.ops, 1)->attr)->const_p) return FALSE;
    type = ((struct expr *) r->attr)->type;
    size = type_size (c2m_ctx, type->arr_type != NULL ? type->arr_type : type);
    *deref = 1;
    *offset += e->c.i_val * size;
    return TRUE;
  case N_ADD:
  case N_SUB:
    if ((op2 = NL_EL (r->u.ops, 1)) == NULL) return FALSE;
    op1 = NL_HEAD (r->u.ops);
    if (r->code == N_ADD && (e = op1->attr)->const_p) SWAP (op1, op2, temp);
    if (!check_const_addr_p (c2m_ctx, op1, base, offset, deref)) return FALSE;
    if (*deref != 0 && ((struct expr *) op1->attr)->type->arr_type == NULL) return FALSE;
    if (!(e = op2->attr)->const_p
        && (!e->const_addr_p || e->def_node == NULL || e->def_node->code != N_LABEL_ADDR))
      return FALSE;
    type = ((struct expr *) r->attr)->type;
    assert (type->mode == TM_BASIC || type->mode == TM_PTR);
    size = (type->mode == TM_BASIC || type->u.ptr_type->mode == TM_FUNC
              ? 1
              : type_size (c2m_ctx, type->u.ptr_type->arr_type != NULL ? type->u.ptr_type->arr_type
                                                                       : type->u.ptr_type));
    if (r->code == N_ADD)
      *offset += e->c.i_val * size;
    else
      *offset -= e->c.i_val * size;
    return TRUE;
  case N_CAST:
    decl_spec = NL_HEAD (r->u.ops)->attr;
    if (type_size (c2m_ctx, decl_spec->type) != sizeof (mir_size_t)) return FALSE;
    return check_const_addr_p (c2m_ctx, NL_EL (r->u.ops, 1), base, offset, deref);
  default: return FALSE;
  }
}

static void setup_const_addr_p (c2m_ctx_t c2m_ctx, node_t r) {
  node_t base;
  mir_llong offset;
  int deref;
  struct expr *e;

  if (!check_const_addr_p (c2m_ctx, r, &base, &offset, &deref) || deref != 0) return;
  e = r->attr;
  e->const_addr_p = TRUE;
  e->def_node = base;
  e->c.i_val = offset;
}

static void process_init_field_designator (c2m_ctx_t c2m_ctx, node_t designator_member,
                                           struct type *container_type) {
  decl_t decl;
  init_object_t init_object;
  node_t curr_member;

  assert (designator_member->code == N_MEMBER);
  /* We can have *partial* path of containing anon members: pop them */
  while (VARR_LENGTH (init_object_t, init_object_path) != 0) {
    init_object = VARR_LAST (init_object_t, init_object_path);
    if (!init_object.field_designator_p || (decl = init_object.u.curr_member->attr) == NULL
        || !decl->decl_spec.type->unnamed_anon_struct_union_member_type_p) {
      break;
    }
    container_type = init_object.container_type;
    VARR_POP (init_object_t, init_object_path);
  }
  /* Now add *full* path to designator_member of containing anon members */
  assert (VARR_LENGTH (node_t, containing_anon_members) == 0);
  decl = designator_member->attr;
  for (curr_member = decl->containing_unnamed_anon_struct_union_member; curr_member != NULL;
       curr_member = decl->containing_unnamed_anon_struct_union_member) {
    decl = curr_member->attr;
    VARR_PUSH (node_t, containing_anon_members, curr_member);
  }
  while (VARR_LENGTH (node_t, containing_anon_members) != 0) {
    init_object.u.curr_member = VARR_POP (node_t, containing_anon_members);
    init_object.container_type = container_type;
    init_object.field_designator_p = FALSE;
    VARR_PUSH (init_object_t, init_object_path, init_object);
    container_type = (decl = init_object.u.curr_member->attr)->decl_spec.type;
  }
  init_object.u.curr_member = get_adjacent_member (designator_member, FALSE);
  init_object.container_type = container_type;
  init_object.field_designator_p = TRUE;
  VARR_PUSH (init_object_t, init_object_path, init_object);
}

static node_t get_compound_literal (node_t n, int *addr_p) {
  for (int addr = 0; n != NULL; n = NL_HEAD (n->u.ops)) {
    switch (n->code) {
    case N_ADDR: addr++; break;
    case N_DEREF: addr--; break;
    case N_CAST: break;  // ???
    case N_STR:
    case N_STR16:
    case N_STR32:
    case N_COMPOUND_LITERAL:
      if (addr < 0) return NULL;
      *addr_p = addr > 0;
      return n;
      break;
    default: return NULL;
    }
    if (addr != -1 && addr != 0 && addr != 1) return NULL;
  }
  return NULL;
}

static mir_llong get_arr_type_size (struct type *arr_type) {
  node_t size_node;
  struct expr *sexpr;

  assert (arr_type->mode == TM_ARR);
  size_node = arr_type->u.arr_type->size;
  sexpr = size_node->attr;
  return (size_node->code != N_IGNORE && sexpr->const_p && integer_type_p (sexpr->type)
            ? sexpr->c.i_val
            : -1);
}

static void check_initializer (c2m_ctx_t c2m_ctx, decl_t member_decl MIR_UNUSED,
                               struct type **type_ptr, node_t initializer, int const_only_p,
                               int top_p) {
  struct type *type = *type_ptr;
  struct expr *cexpr;
  node_t literal, des_list, curr_des, init, str, value, size_node, temp;
  mir_llong max_index;
  mir_llong size_val = 0; /* to remove an uninitialized warning */
  size_t mark, len;
  symbol_t sym;
  init_object_t init_object;
  int addr_p = FALSE; /* to remove an uninitialized warning */

  literal = get_compound_literal (initializer, &addr_p);
  if (literal != NULL && !addr_p && initializer->code != N_STR && initializer->code != N_STR16
      && initializer->code != N_STR32) {
    cexpr = initializer->attr;
    check_assignment_types (c2m_ctx, type, NULL, cexpr, initializer);
    initializer = NL_EL (literal->u.ops, 1);
  }
check_one_value:
  if (initializer->code != N_LIST
      && !(type->mode == TM_ARR
           && init_compatible_string_p (initializer, type->u.arr_type->el_type))) {
    if ((cexpr = initializer->attr)->const_p || initializer->code == N_STR
        || initializer->code == N_STR16 || initializer->code == N_STR32 || !const_only_p) {
      check_assignment_types (c2m_ctx, type, NULL, cexpr, initializer);
    } else {
      setup_const_addr_p (c2m_ctx, initializer);
      if ((cexpr = initializer->attr)->const_addr_p || (literal != NULL && addr_p))
        check_assignment_types (c2m_ctx, type, NULL, cexpr, initializer);
      else
        error (c2m_ctx, POS (initializer),
               "initializer of non-auto or thread local object"
               " should be a constant expression or address");
    }
    return;
  }
  init = NL_HEAD (initializer->u.ops);
  if (((str = initializer)->code == N_STR || str->code == N_STR16
       || str->code == N_STR32 /* string or string in parentheses */
       || (init != NULL && init->code == N_INIT && NL_EL (initializer->u.ops, 1) == NULL
           && (des_list = NL_HEAD (init->u.ops))->code == N_LIST
           && NL_HEAD (des_list->u.ops) == NULL && NL_EL (init->u.ops, 1) != NULL
           && ((str = NL_EL (init->u.ops, 1))->code == N_STR || str->code == N_STR16
               || str->code == N_STR32)))
      && type->mode == TM_ARR && init_compatible_string_p (str, type->u.arr_type->el_type)) {
    len = str->u.s.len;
    if (incomplete_type_p (c2m_ctx, type)) {
      assert (len < MIR_INT_MAX);
      type->u.arr_type->size = new_i_node (c2m_ctx, (long) len, POS (type->u.arr_type->size));
      check (c2m_ctx, type->u.arr_type->size, NULL);
      make_type_complete (c2m_ctx, type);
    } else if (len > (size_t) ((struct expr *) type->u.arr_type->size->attr)->c.i_val + 1) {
      error (c2m_ctx, POS (initializer), "string is too long for array initializer");
    }
    return;
  }
  if (init == NULL) {
    if (scalar_type_p (type)) error (c2m_ctx, POS (initializer), "empty scalar initializer");
    return;
  }
  assert (init->code == N_INIT);
  des_list = NL_HEAD (init->u.ops);
  assert (des_list->code == N_LIST);
  if (type->mode != TM_ARR && type->mode != TM_STRUCT && type->mode != TM_UNION) {
    if ((temp = NL_NEXT (init)) != NULL) {
      error (c2m_ctx, POS (temp), "excess elements in scalar initializer");
      return;
    }
    if ((temp = NL_HEAD (des_list->u.ops)) != NULL) {
      error (c2m_ctx, POS (temp), "designator in scalar initializer");
      return;
    }
    initializer = NL_NEXT (des_list);
    if (!top_p) {
      error (c2m_ctx, POS (init), "braces around scalar initializer");
      return;
    }
    top_p = FALSE;
    goto check_one_value;
  }
  mark = VARR_LENGTH (init_object_t, init_object_path);
  init_object.container_type = type;
  init_object.field_designator_p = FALSE;
  if (type->mode == TM_ARR) {
    size_val = get_arr_type_size (type);
    init_object.u.curr_index = -1;
  } else {
    init_object.u.curr_member = NULL;
  }
  VARR_PUSH (init_object_t, init_object_path, init_object);
  max_index = -1;
  for (; init != NULL; init = NL_NEXT (init)) {
    assert (init->code == N_INIT);
    des_list = NL_HEAD (init->u.ops);
    value = NL_NEXT (des_list);
    if ((value->code == N_LIST || value->code == N_COMPOUND_LITERAL) && type->mode != TM_ARR
        && type->mode != TM_STRUCT && type->mode != TM_UNION) {
      error (c2m_ctx, POS (init),
             value->code == N_LIST ? "braces around scalar initializer"
                                   : "compound literal for scalar initializer");
      break;
    }
    if ((curr_des = NL_HEAD (des_list->u.ops)) == NULL) {
      if (!update_path_and_do (c2m_ctx, TRUE, check_initializer, mark, value, const_only_p,
                               &max_index, POS (init), "array/struct/union"))
        break;
    } else {
      struct type *curr_type = type;
      mir_llong arr_size_val MIR_UNUSED;
      int first_p = TRUE;

      VARR_TRUNC (init_object_t, init_object_path, mark + 1);
      for (; curr_des != NULL; curr_des = NL_NEXT (curr_des), first_p = FALSE) {
        init_object = VARR_LAST (init_object_t, init_object_path);
        if (first_p) {
          VARR_POP (init_object_t, init_object_path);
        } else {
          if (init_object.container_type->mode == TM_ARR) {
            curr_type = init_object.container_type->u.arr_type->el_type;
          } else {
            assert (init_object.container_type->mode == TM_STRUCT
                    || init_object.container_type->mode == TM_UNION);
            decl_t el_decl = init_object.u.curr_member->attr;
            curr_type = el_decl->decl_spec.type;
          }
        }
        if (curr_des->code == N_FIELD_ID) {
          node_t id = NL_HEAD (curr_des->u.ops);

          if (curr_type->mode != TM_STRUCT && curr_type->mode != TM_UNION) {
            error (c2m_ctx, POS (curr_des), "field name not in struct or union initializer");
          } else if (!symbol_find (c2m_ctx, S_REGULAR, id, curr_type->u.tag_type, &sym)) {
            error (c2m_ctx, POS (curr_des), "unknown field %s in initializer", id->u.s.s);
          } else {
            process_init_field_designator (c2m_ctx, sym.def_node, curr_type);
            if (!update_path_and_do (c2m_ctx, NL_NEXT (curr_des) == NULL, check_initializer, mark,
                                     value, const_only_p, NULL, POS (init), "struct/union"))
              break;
          }
        } else if (curr_type->mode != TM_ARR) {
          error (c2m_ctx, POS (curr_des), "array index in initializer for non-array");
        } else if (!(cexpr = curr_des->attr)->const_p) {
          error (c2m_ctx, POS (curr_des), "nonconstant array index in initializer");
        } else if (!integer_type_p (cexpr->type)) {
          error (c2m_ctx, POS (curr_des), "array index in initializer not of integer type");
        } else if (incomplete_type_p (c2m_ctx, curr_type) && signed_integer_type_p (cexpr->type)
                   && cexpr->c.i_val < 0) {
          error (c2m_ctx, POS (curr_des),
                 "negative array index in initializer for array without size");
        } else if ((arr_size_val = get_arr_type_size (curr_type)) >= 0
                   && (mir_ullong) arr_size_val <= cexpr->c.u_val) {
          error (c2m_ctx, POS (curr_des), "array index in initializer exceeds array bounds");
        } else {
          init_object.u.curr_index = cexpr->c.i_val - 1; /* previous el */
          init_object.field_designator_p = FALSE;
          init_object.container_type = curr_type;
          VARR_PUSH (init_object_t, init_object_path, init_object);
          if (!update_path_and_do (c2m_ctx, NL_NEXT (curr_des) == NULL, check_initializer, mark,
                                   value, const_only_p, first_p ? &max_index : NULL, POS (init),
                                   "array"))
            break;
        }
      }
    }
  }
  if (type->mode == TM_ARR && size_val < 0 && max_index >= 0) {
    /* Array w/o size: define it.  Copy the type as the incomplete
       type can be shared by declarations with different length
       initializers.  We need only one level of copying as sub-array
       can not have incomplete type with an initializer. */
    struct arr_type *arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));

    type = create_type (c2m_ctx, type);
    assert (incomplete_type_p (c2m_ctx, type));
    *arr_type = *type->u.arr_type;
    type->u.arr_type = arr_type;
    size_node = type->u.arr_type->size;
    type->u.arr_type->size
      = (max_index < MIR_INT_MAX    ? new_i_node (c2m_ctx, (long) max_index + 1, POS (size_node))
         : max_index < MIR_LONG_MAX ? new_l_node (c2m_ctx, (long) max_index + 1, POS (size_node))
                                    : new_ll_node (c2m_ctx, max_index + 1, POS (size_node)));
    check (c2m_ctx, type->u.arr_type->size, NULL);
    make_type_complete (c2m_ctx, type);
  }
  VARR_TRUNC (init_object_t, init_object_path, mark);
  *type_ptr = type;
  return;
}

static void check_decl_align (c2m_ctx_t c2m_ctx, struct decl_spec *decl_spec) {
  if (decl_spec->align < 0) return;
  if (decl_spec->align < type_align (decl_spec->type))
    error (c2m_ctx, POS (decl_spec->align_node),
           "requested alignment is less than minimum alignment for the type");
}

static void init_decl (c2m_ctx_t c2m_ctx, decl_t decl) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;

  decl->addr_p = FALSE;
  decl->reg_p = decl->asm_p = decl->used_p = FALSE;
  decl->offset = 0;
  decl->bit_offset = -1;
  decl->param_args_start = decl->param_args_num = 0;
  decl->scope = curr_scope;
  decl->containing_unnamed_anon_struct_union_member = curr_unnamed_anon_struct_union_member;
  decl->u.item = NULL;
  decl->c2m_ctx = c2m_ctx;
}

static void create_decl (c2m_ctx_t c2m_ctx, node_t scope, node_t decl_node,
                         struct decl_spec decl_spec, node_t initializer, int param_p) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  int func_def_p = decl_node->code == N_FUNC_DEF, func_p = FALSE;
  node_t id = NULL; /* to remove an uninitialized warning */
  node_t list_head, declarator;
  struct type *type;
  decl_t decl = reg_malloc (c2m_ctx, sizeof (struct decl));

  assert (decl_node->code == N_MEMBER || decl_node->code == N_SPEC_DECL
          || decl_node->code == N_FUNC_DEF);
  init_decl (c2m_ctx, decl);
  decl->scope = scope;
  decl->decl_spec = decl_spec;
  decl_node->attr = decl;
  declarator = NL_EL (decl_node->u.ops, 1);
  if (declarator->code == N_IGNORE) {
    assert (decl_node->code == N_MEMBER);
    decl->decl_spec.linkage = N_IGNORE;
  } else {
    assert (declarator->code == N_DECL);
    type = check_declarator (c2m_ctx, declarator, func_def_p);
    decl->decl_spec.type = append_type (type, decl->decl_spec.type);
  }
  check_type (c2m_ctx, decl->decl_spec.type, 0, func_def_p);
  if (declarator->code == N_DECL) {
    id = NL_HEAD (declarator->u.ops);
    list_head = NL_HEAD (NL_NEXT (id)->u.ops);
    func_p = !param_p && list_head && list_head->code == N_FUNC;
    decl->decl_spec.linkage = get_id_linkage (c2m_ctx, func_p, id, scope, decl->decl_spec);
  }
  if (decl_node->code != N_MEMBER) {
    set_type_layout (c2m_ctx, decl->decl_spec.type);
    check_decl_align (c2m_ctx, &decl->decl_spec);
    if (!decl->decl_spec.typedef_p && decl->scope != top_scope && decl->scope->code != N_FUNC)
      VARR_PUSH (decl_t, func_decls_for_allocation, decl);
  }
  if (declarator->code == N_DECL) {
    def_symbol (c2m_ctx, S_REGULAR, id, scope, decl_node, decl->decl_spec.linkage);
    if (scope != top_scope && decl->decl_spec.linkage == N_EXTERN)
      def_symbol (c2m_ctx, S_REGULAR, id, top_scope, decl_node, N_EXTERN);
    if (func_p && decl->decl_spec.thread_local_p) {
      error (c2m_ctx, POS (id), "thread local function declaration");
      if (c2m_options->message_file != NULL) {
        if (id->code != N_IGNORE) fprintf (c2m_options->message_file, " of %s", id->u.s.s);
        fprintf (c2m_options->message_file, "\n");
      }
    }
  }
  if (initializer == NULL || initializer->code == N_IGNORE) return;
  if (incomplete_type_p (c2m_ctx, decl->decl_spec.type)
      && (decl->decl_spec.type->mode != TM_ARR
          || incomplete_type_p (c2m_ctx, decl->decl_spec.type->u.arr_type->el_type))) {
    if (decl->decl_spec.type->mode == TM_ARR
        && decl->decl_spec.type->u.arr_type->el_type->mode == TM_ARR)
      error (c2m_ctx, POS (initializer), "initialization of incomplete sub-array");
    else
      error (c2m_ctx, POS (initializer), "initialization of incomplete type variable");
    return;
  }
  if (decl->decl_spec.linkage == N_EXTERN && scope != top_scope) {
    error (c2m_ctx, POS (initializer), "initialization of %s in block scope with external linkage",
           id->u.s.s);
    return;
  }
  check (c2m_ctx, initializer, decl_node);
  check_initializer (c2m_ctx, NULL, &decl->decl_spec.type, initializer,
                     decl->decl_spec.linkage == N_STATIC || decl->decl_spec.linkage == N_EXTERN
                       || decl->decl_spec.thread_local_p || decl->decl_spec.static_p,
                     TRUE);
}

static struct type *adjust_type (c2m_ctx_t c2m_ctx, struct type *type) {
  struct type *res;

  if (type->mode != TM_ARR && type->mode != TM_FUNC) return type;
  res = create_type (c2m_ctx, NULL);
  res->mode = TM_PTR;
  res->pos_node = type->pos_node;
  if (type->mode == TM_FUNC) {
    res->func_type_before_adjustment_p = TRUE;
    res->u.ptr_type = type;
  } else {
    res->arr_type = type;
    res->u.ptr_type = type->u.arr_type->el_type;
    res->type_qual = type->u.arr_type->ind_type_qual;
  }
  set_type_layout (c2m_ctx, res);
  return res;
}

static void process_unop (c2m_ctx_t c2m_ctx, node_t r, node_t *op, struct expr **e, struct type **t,
                          node_t context) {
  *op = NL_HEAD (r->u.ops);
  check (c2m_ctx, *op, context);
  *e = (*op)->attr;
  *t = (*e)->type;
}

static void process_bin_ops (c2m_ctx_t c2m_ctx, node_t r, node_t *op1, node_t *op2,
                             struct expr **e1, struct expr **e2, struct type **t1, struct type **t2,
                             node_t context) {
  *op1 = NL_HEAD (r->u.ops);
  *op2 = NL_NEXT (*op1);
  check (c2m_ctx, *op1, context);
  check (c2m_ctx, *op2, context);
  *e1 = (*op1)->attr;
  *e2 = (*op2)->attr;
  *t1 = (*e1)->type;
  *t2 = (*e2)->type;
}

static void process_type_bin_ops (c2m_ctx_t c2m_ctx, node_t r, node_t *op1, node_t *op2,
                                  struct expr **e2, struct type **t2, node_t context) {
  *op1 = NL_HEAD (r->u.ops);
  *op2 = NL_NEXT (*op1);
  check (c2m_ctx, *op1, context);
  check (c2m_ctx, *op2, context);
  *e2 = (*op2)->attr;
  *t2 = (*e2)->type;
}

static struct expr *create_expr (c2m_ctx_t c2m_ctx, node_t r) {
  struct expr *e = reg_malloc (c2m_ctx, sizeof (struct expr));

  r->attr = e;
  e->type = create_type (c2m_ctx, NULL);
  e->type2 = NULL;
  e->type->pos_node = r;
  e->u.lvalue_node = NULL;
  e->const_p = e->const_addr_p = e->builtin_call_p = FALSE;
  return e;
}

static struct expr *create_basic_type_expr (c2m_ctx_t c2m_ctx, node_t r, enum basic_type bt) {
  struct expr *e = create_expr (c2m_ctx, r);

  e->type->mode = TM_BASIC;
  e->type->u.basic_type = bt;
  return e;
}

static void get_int_node (c2m_ctx_t c2m_ctx, node_t *op, struct expr **e, struct type **t,
                          mir_size_t i) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;

  if (i == 1) {
    *op = n_i1_node;
  } else {
    *op = new_i_node (c2m_ctx, (long) i, no_pos);
    check (c2m_ctx, *op, NULL);
  }
  *e = (*op)->attr;
  *t = (*e)->type;
  init_type (*t);
  (*e)->type->mode = TM_BASIC;
  (*e)->type->u.basic_type = TP_INT;
  (*e)->c.i_val = i;  // ???
}

static struct expr *check_assign_op (c2m_ctx_t c2m_ctx, node_t r, struct expr *e1, struct expr *e2,
                                     struct type *t1, struct type *t2) {
  struct expr *e = NULL;
  struct expr *te;
  struct type t, *tt;

  switch (r->code) {
  case N_AND:
  case N_OR:
  case N_XOR:
  case N_AND_ASSIGN:
  case N_OR_ASSIGN:
  case N_XOR_ASSIGN:
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!integer_type_p (t1) || !integer_type_p (t2)) {
      error (c2m_ctx, POS (r), "bitwise operation operands should be of an integer type");
    } else {
      t = arithmetic_conversion (t1, t2);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        convert_value (e1, &t);
        convert_value (e2, &t);
        e->const_p = TRUE;
        if (signed_integer_type_p (&t))
          e->c.i_val = (r->code == N_AND  ? e1->c.i_val & e2->c.i_val
                        : r->code == N_OR ? e1->c.i_val | e2->c.i_val
                                          : e1->c.i_val ^ e2->c.i_val);
        else
          e->c.u_val = (r->code == N_AND  ? e1->c.u_val & e2->c.u_val
                        : r->code == N_OR ? e1->c.u_val | e2->c.u_val
                                          : e1->c.u_val ^ e2->c.u_val);
      }
    }
    break;
  case N_LSH:
  case N_RSH:
  case N_LSH_ASSIGN:
  case N_RSH_ASSIGN:
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!integer_type_p (t1) || !integer_type_p (t2)) {
      error (c2m_ctx, POS (r), "shift operands should be of an integer type");
    } else {
      t = integer_promotion (t1);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        struct type rt = integer_promotion (t2);

        convert_value (e1, &t);
        convert_value (e2, &rt);
        e->const_p = TRUE;
        if (signed_integer_type_p (&t)) {
          if (signed_integer_type_p (&rt))
            e->c.i_val = r->code == N_LSH ? e1->c.i_val << e2->c.i_val : e1->c.i_val >> e2->c.i_val;
          else
            e->c.i_val = r->code == N_LSH ? e1->c.i_val << e2->c.u_val : e1->c.i_val >> e2->c.u_val;
        } else if (signed_integer_type_p (&rt)) {
          e->c.u_val = r->code == N_LSH ? e1->c.u_val << e2->c.i_val : e1->c.u_val >> e2->c.i_val;
        } else {
          e->c.u_val = r->code == N_LSH ? e1->c.u_val << e2->c.u_val : e1->c.u_val >> e2->c.u_val;
        }
      }
    }
    break;
  case N_INC:
  case N_DEC:
  case N_POST_INC:
  case N_POST_DEC:
  case N_ADD:
  case N_SUB:
  case N_ADD_ASSIGN:
  case N_SUB_ASSIGN: {
    mir_size_t size;
    int add_p
      = (r->code == N_ADD || r->code == N_ADD_ASSIGN || r->code == N_INC || r->code == N_POST_INC);

    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (arithmetic_type_p (t1) && arithmetic_type_p (t2)) {
      t = arithmetic_conversion (t1, t2);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        e->const_p = TRUE;
        convert_value (e1, &t);
        convert_value (e2, &t);
        if (floating_type_p (&t))
          e->c.d_val = (add_p ? e1->c.d_val + e2->c.d_val : e1->c.d_val - e2->c.d_val);
        else if (signed_integer_type_p (&t))
          e->c.i_val = (add_p ? e1->c.i_val + e2->c.i_val : e1->c.i_val - e2->c.i_val);
        else
          e->c.u_val = (add_p ? e1->c.u_val + e2->c.u_val : e1->c.u_val - e2->c.u_val);
      }
    } else if (add_p) {
      if (t2->mode == TM_PTR) {
        SWAP (t1, t2, tt);
        SWAP (e1, e2, te);
      }
      if (t1->mode != TM_PTR || !integer_type_p (t2)) {
        error (c2m_ctx, POS (r), "invalid operand types of +");
      } else if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)) {
        error (c2m_ctx, POS (r), "pointer to incomplete type as an operand of +");
      } else {
        *e->type = *t1;
        if (e1->const_p && e2->const_p) {
          size = type_size (c2m_ctx, t1->u.ptr_type);
          e->const_p = TRUE;
          e->c.u_val = (signed_integer_type_p (t2) ? e1->c.u_val + e2->c.i_val * size
                                                   : e1->c.u_val + e2->c.u_val * size);
        }
      }
    } else if (t1->mode == TM_PTR && integer_type_p (t2)) {
      if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)) {
        error (c2m_ctx, POS (r), "pointer to incomplete type as an operand of -");
      } else {
        *e->type = *t1;
        if (e1->const_p && e2->const_p) {
          size = type_size (c2m_ctx, t1->u.ptr_type);
          e->const_p = TRUE;
          e->c.u_val = (signed_integer_type_p (t2) ? e1->c.u_val - e2->c.i_val * size
                                                   : e1->c.u_val - e2->c.u_val * size);
        }
      }
    } else if (t1->mode == TM_PTR && t2->mode == TM_PTR && compatible_types_p (t1, t2, TRUE)) {
      if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)
          && incomplete_type_p (c2m_ctx, t2->u.ptr_type)) {
        error (c2m_ctx, POS (r), "pointer to incomplete type as an operand of -");
      } else if (t1->u.ptr_type->type_qual.atomic_p || t2->u.ptr_type->type_qual.atomic_p) {
        error (c2m_ctx, POS (r), "pointer to atomic type as an operand of -");
      } else {
        e->type->mode = TM_BASIC;
        e->type->u.basic_type = get_int_basic_type (sizeof (mir_ptrdiff_t));
        set_type_layout (c2m_ctx, e->type);
        if (e1->const_p && e2->const_p) {
          size = type_size (c2m_ctx, t1->u.ptr_type);
          e->const_p = TRUE;
          e->c.i_val
            = (e1->c.u_val > e2->c.u_val ? (mir_ptrdiff_t) ((e1->c.u_val - e2->c.u_val) / size)
                                         : -(mir_ptrdiff_t) ((e2->c.u_val - e1->c.u_val) / size));
        }
      }
    } else {
      error (c2m_ctx, POS (r), "invalid operand types of -");
    }
    break;
  }
  case N_MUL:
  case N_DIV:
  case N_MOD:
  case N_MUL_ASSIGN:
  case N_DIV_ASSIGN:
  case N_MOD_ASSIGN:
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (r->code == N_MOD && (!integer_type_p (t1) || !integer_type_p (t2))) {
      error (c2m_ctx, POS (r), "invalid operand types of %%");
    } else if (r->code != N_MOD && (!arithmetic_type_p (t1) || !arithmetic_type_p (t2))) {
      error (c2m_ctx, POS (r), "invalid operand types of %s", r->code == N_MUL ? "*" : "/");
    } else {
      t = arithmetic_conversion (t1, t2);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p && e2->const_p) {
        e->const_p = TRUE;
        convert_value (e1, &t);
        convert_value (e2, &t);
        if (r->code == N_MUL) {
          if (floating_type_p (&t))
            e->c.d_val = e1->c.d_val * e2->c.d_val;
          else if (signed_integer_type_p (&t))
            e->c.i_val = e1->c.i_val * e2->c.i_val;
          else
            e->c.u_val = e1->c.u_val * e2->c.u_val;
        } else if ((floating_type_p (&t) && e1->c.d_val == 0.0 && e2->c.d_val == 0.0)
                   || (signed_integer_type_p (&t) && e2->c.i_val == 0)
                   || (integer_type_p (&t) && !signed_integer_type_p (&t) && e2->c.u_val == 0)) {
          if (floating_type_p (&t)) {
            e->c.d_val = nanl (""); /* Use NaN */
          } else {
            if (signed_integer_type_p (&t))
              e->c.i_val = 0;
            else
              e->c.u_val = 0;
            error (c2m_ctx, POS (r), "Division by zero");
          }
        } else if (r->code != N_MOD && floating_type_p (&t)) {
          e->c.d_val = e1->c.d_val / e2->c.d_val;
        } else if (signed_integer_type_p (&t)) {  // ??? zero
          e->c.i_val = r->code == N_DIV ? e1->c.i_val / e2->c.i_val : e1->c.i_val % e2->c.i_val;
        } else {
          e->c.u_val = r->code == N_DIV ? e1->c.u_val / e2->c.u_val : e1->c.u_val % e2->c.u_val;
        }
      }
    }
    break;
  default: e = NULL; assert (FALSE);
  }
  return e;
}

static unsigned case_hash (case_t el, void *arg MIR_UNUSED) {
  node_t case_expr = NL_HEAD (el->case_node->u.ops);
  struct expr *expr;

  assert (el->case_node->code == N_CASE);
  expr = case_expr->attr;
  assert (expr->const_p);
  if (signed_integer_type_p (expr->type))
    return (unsigned) mir_hash (&expr->c.i_val, sizeof (expr->c.i_val), 0x42);
  return (unsigned) mir_hash (&expr->c.u_val, sizeof (expr->c.u_val), 0x42);
}

static int case_eq (case_t el1, case_t el2, void *arg MIR_UNUSED) {
  node_t case_expr1 = NL_HEAD (el1->case_node->u.ops);
  node_t case_expr2 = NL_HEAD (el2->case_node->u.ops);
  struct expr *expr1, *expr2;

  assert (el1->case_node->code == N_CASE && el2->case_node->code == N_CASE);
  expr1 = case_expr1->attr;
  expr2 = case_expr2->attr;
  assert (expr1->const_p && expr2->const_p);
  assert (signed_integer_type_p (expr1->type) == signed_integer_type_p (expr2->type));
  if (signed_integer_type_p (expr1->type)) return expr1->c.i_val == expr2->c.i_val;
  return expr1->c.u_val == expr2->c.u_val;
}

static void update_call_arg_area_offset (c2m_ctx_t c2m_ctx, struct type *type, int update_scope_p) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  node_t block = NL_EL (curr_func_def->u.ops, 3);
  struct node_scope *ns = block->attr;

  curr_call_arg_area_offset += round_size (type_size (c2m_ctx, type), MAX_ALIGNMENT);
  if (update_scope_p && ns->call_arg_area_size < curr_call_arg_area_offset)
    ns->call_arg_area_size = curr_call_arg_area_offset;
}

#define NODE_CASE(n) case N_##n:
#define REP_SEP
static void classify_node (node_t n, int *expr_attr_p, int *stmt_p) {
  *expr_attr_p = *stmt_p = FALSE;
  switch (n->code) {
    REP8 (NODE_CASE, I, L, LL, U, UL, ULL, F, D)
    REP7 (NODE_CASE, LD, CH, CH16, CH32, STR, STR16, STR32)
    REP7 (NODE_CASE, ID, LABEL_ADDR, COMMA, ANDAND, OROR, EQ, STMTEXPR)
    REP8 (NODE_CASE, NE, LT, LE, GT, GE, ASSIGN, BITWISE_NOT, NOT)
    REP8 (NODE_CASE, AND, AND_ASSIGN, OR, OR_ASSIGN, XOR, XOR_ASSIGN, LSH, LSH_ASSIGN)
    REP8 (NODE_CASE, RSH, RSH_ASSIGN, ADD, ADD_ASSIGN, SUB, SUB_ASSIGN, MUL, MUL_ASSIGN)
    REP8 (NODE_CASE, DIV, DIV_ASSIGN, MOD, MOD_ASSIGN, IND, FIELD, ADDR, DEREF)
    REP8 (NODE_CASE, DEREF_FIELD, COND, INC, DEC, POST_INC, POST_DEC, ALIGNOF, SIZEOF)
    REP6 (NODE_CASE, EXPR_SIZEOF, CAST, COMPOUND_LITERAL, CALL, GENERIC, GENERIC_ASSOC)
    *expr_attr_p = TRUE;
    break;
    REP8 (NODE_CASE, IF, SWITCH, WHILE, DO, FOR, GOTO, INDIRECT_GOTO, CONTINUE)
    REP5 (NODE_CASE, BREAK, RETURN, EXPR, BLOCK, SPEC_DECL) /* SPEC DECL may have an initializer */
    *stmt_p = TRUE;
    break;
    REP8 (NODE_CASE, IGNORE, CASE, DEFAULT, LABEL, LIST, SHARE, TYPEDEF, EXTERN)
    REP8 (NODE_CASE, STATIC, AUTO, REGISTER, THREAD_LOCAL, DECL, VOID, CHAR, SHORT)
    REP8 (NODE_CASE, INT, LONG, FLOAT, DOUBLE, SIGNED, UNSIGNED, BOOL, STRUCT)
    REP8 (NODE_CASE, UNION, ENUM, ENUM_CONST, MEMBER, CONST, RESTRICT, VOLATILE, ATOMIC)
    REP8 (NODE_CASE, INLINE, NO_RETURN, ALIGNAS, FUNC, STAR, POINTER, DOTS, ARR)
    REP6 (NODE_CASE, INIT, FIELD_ID, TYPE, ST_ASSERT, FUNC_DEF, MODULE)
    break;
  default: assert (FALSE);
  }
}
#undef REP_SEP

/* Create "static const char __func__[] = "<func name>" at the
   beginning of func_block if it is necessary.  */
static void add__func__def (c2m_ctx_t c2m_ctx, node_t func_block, str_t func_name) {
  static const char fdecl_name[] = "__func__";
  pos_t pos = POS (func_block);
  node_t list, declarator, decl, decl_specs;
  tab_str_t str;

  if (!str_exists_p (c2m_ctx, fdecl_name, strlen (fdecl_name) + 1, &str)) return;
  decl_specs = new_pos_node (c2m_ctx, N_LIST, pos);
  NL_APPEND (decl_specs->u.ops, new_pos_node (c2m_ctx, N_STATIC, pos));
  NL_APPEND (decl_specs->u.ops, new_pos_node (c2m_ctx, N_CONST, pos));
  NL_APPEND (decl_specs->u.ops, new_pos_node (c2m_ctx, N_CHAR, pos));
  list = new_pos_node (c2m_ctx, N_LIST, pos);
  NL_APPEND (list->u.ops, new_pos_node3 (c2m_ctx, N_ARR, pos, new_pos_node (c2m_ctx, N_IGNORE, pos),
                                         new_pos_node (c2m_ctx, N_LIST, pos),
                                         new_pos_node (c2m_ctx, N_IGNORE, pos)));
  declarator
    = new_pos_node2 (c2m_ctx, N_DECL, pos, new_str_node (c2m_ctx, N_ID, str.str, pos), list);
  decl = new_pos_node5 (c2m_ctx, N_SPEC_DECL, pos, decl_specs, declarator,
                        new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_IGNORE),
                        new_str_node (c2m_ctx, N_STR, func_name, pos));
  NL_PREPEND (NL_EL (func_block->u.ops, 1)->u.ops, decl);
}

/* Sort by decl scope nesting (more nested scope has a bigger UID) and decl size. */
static int decl_cmp (const void *v1, const void *v2) {
  const decl_t d1 = *(const decl_t *) v1, d2 = *(const decl_t *) v2;
  struct type *t1 = d1->decl_spec.type, *t2 = d2->decl_spec.type;
  mir_size_t s1 = raw_type_size (d1->c2m_ctx, t1), s2 = raw_type_size (d2->c2m_ctx, t2);

  if (d1->scope->uid < d2->scope->uid) return -1;
  if (d1->scope->uid > d2->scope->uid) return 1;
  if (s1 < s2) return -1;
  if (s1 > s2) return 1;
  return 0;
}

static void process_func_decls_for_allocation (c2m_ctx_t c2m_ctx) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  size_t i, j;
  decl_t decl;
  struct type *type;
  struct node_scope *ns, *curr_ns;
  node_t scope;
  mir_size_t start_offset = 0; /* to remove an uninitialized warning */

  /* Exclude decls which will be in regs: */
  for (i = j = 0; i < VARR_LENGTH (decl_t, func_decls_for_allocation); i++) {
    decl = VARR_GET (decl_t, func_decls_for_allocation, i);
    type = decl->decl_spec.type;
    ns = decl->scope->attr;
    if (scalar_type_p (type)) {
      decl->reg_p = TRUE;
      continue;
    }
    VARR_SET (decl_t, func_decls_for_allocation, j, decl);
    j++;
  }
  VARR_TRUNC (decl_t, func_decls_for_allocation, j);
  qsort (VARR_ADDR (decl_t, func_decls_for_allocation), j, sizeof (decl_t), decl_cmp);
  scope = NULL;
  for (i = 0; i < VARR_LENGTH (decl_t, func_decls_for_allocation); i++) {
    decl = VARR_GET (decl_t, func_decls_for_allocation, i);
    type = decl->decl_spec.type;
    ns = decl->scope->attr;
    if (decl->scope != scope) { /* new scope: process upper scopes */
      for (scope = ns->scope; scope != top_scope; scope = curr_ns->scope) {
        curr_ns = scope->attr;
        ns->offset += curr_ns->size;
        curr_ns->stack_var_p = TRUE;
      }
      scope = decl->scope;
      ns->stack_var_p = TRUE;
      start_offset = ns->offset;
    }
    ns->offset = round_size (ns->offset, var_align (c2m_ctx, type));
    decl->offset = ns->offset;
    ns->offset += var_size (c2m_ctx, type);
    ns->size = ns->offset - start_offset;
  }
  scope = NULL;
  for (i = 0; i < VARR_LENGTH (decl_t, func_decls_for_allocation); i++) { /* update scope sizes: */
    decl = VARR_GET (decl_t, func_decls_for_allocation, i);
    ns = decl->scope->attr;
    if (decl->scope == scope) continue;
    /* new scope: update upper scope sizes */
    for (scope = ns->scope; scope != top_scope; scope = curr_ns->scope) {
      curr_ns = scope->attr;
      if (curr_ns->size < ns->offset) curr_ns->size = ns->offset;
      if (ns->stack_var_p) curr_ns->stack_var_p = TRUE;
    }
  }
}

static const char *check_attrs (c2m_ctx_t c2m_ctx, node_t r, decl_t decl, node_t attrs,
                                int check_p) {
  node_t n, list, id, alias_id;
  if (attrs->code == N_IGNORE) return NULL;
  assert (attrs->code == N_LIST);
  alias_id = NULL;
  for (n = NL_HEAD (attrs->u.ops); n != NULL; n = NL_NEXT (n)) {
    assert (n->code == N_ATTR);
    id = NL_HEAD (n->u.ops);
    assert (id->code == N_ID);
    if (strcmp (id->u.s.s, "antialias") != 0) continue;
    list = NL_NEXT (id);
    assert (list->code == N_LIST);
    id = NL_HEAD (list->u.ops);
    if (id == NULL) continue;
    if (!check_p) {
      if (id->code == N_ID) return id->u.s.s;
    } else if (NL_NEXT (id) != NULL) {
      error (c2m_ctx, POS (r), "antialias attribute has more one arg");
    } else if (id->code != N_ID) {
      error (c2m_ctx, POS (r), "antialias attribute arg should be an identifier");
    } else if (alias_id != NULL && strcmp (id->u.s.s, alias_id->u.s.s) != 0) {
      error (c2m_ctx, POS (r), "antialias attributes have different ids %s and %s", id->u.s.s,
             alias_id->u.s.s);
    }
    alias_id = id;
  }
  if (alias_id == NULL) return NULL;
  if (decl->decl_spec.type->mode != TM_PTR) {
    error (c2m_ctx, POS (r), "antialias attribute should be given for a pointer type");
  }
  return alias_id->u.s.s;
}

#define BUILTIN_VA_START \
  (const char *[]) { "__builtin_va_start", NULL }
#define BUILTIN_VA_ARG \
  (const char *[]) { "__builtin_va_arg", NULL }
#define ALLOCA \
  (const char *[]) { "alloca", "__builtin_alloca", NULL }

static int str_eq_p (const char *str, const char *v[]) {
  for (int i = 0; v[i] != NULL; i++)
    if (strcmp (v[i], str) == 0) return TRUE;
  return FALSE;
}

static void check (c2m_ctx_t c2m_ctx, node_t r, node_t context) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;
  node_t op1, op2;
  struct expr *e = NULL, *e1, *e2;
  struct type t, *t1, *t2, *assign_expr_type;
  int expr_attr_p, stmt_p;

  VARR_PUSH (node_t, context_stack, context);
  classify_node (r, &expr_attr_p, &stmt_p);
  switch (r->code) {
  case N_IGNORE:
  case N_STAR:
  case N_FIELD_ID: break; /* do nothing */
  case N_LIST: {
    for (node_t n = NL_HEAD (r->u.ops); n != NULL; n = NL_NEXT (n)) check (c2m_ctx, n, r);
    break;
  }
  case N_I:
  case N_L:
    e = create_basic_type_expr (c2m_ctx, r, r->code == N_I ? TP_INT : TP_LONG);
    e->const_p = TRUE;
    e->c.i_val = r->u.l;
    break;
  case N_LL:
    e = create_basic_type_expr (c2m_ctx, r, TP_LLONG);
    e->const_p = TRUE;
    e->c.i_val = r->u.ll;
    break;
  case N_U:
  case N_UL:
    e = create_basic_type_expr (c2m_ctx, r, r->code == N_U ? TP_UINT : TP_ULONG);
    e->const_p = TRUE;
    e->c.u_val = r->u.ul;
    break;
  case N_ULL:
    e = create_basic_type_expr (c2m_ctx, r, TP_ULLONG);
    e->const_p = TRUE;
    e->c.u_val = r->u.ull;
    break;
  case N_F:
    e = create_basic_type_expr (c2m_ctx, r, TP_FLOAT);
    e->const_p = TRUE;
    e->c.d_val = r->u.f;
    break;
  case N_D:
    e = create_basic_type_expr (c2m_ctx, r, TP_DOUBLE);
    e->const_p = TRUE;
    e->c.d_val = r->u.d;
    break;
  case N_LD:
    e = create_basic_type_expr (c2m_ctx, r, TP_LDOUBLE);
    e->const_p = TRUE;
    e->c.d_val = r->u.ld;
    break;
  case N_CH:
    e = create_basic_type_expr (c2m_ctx, r, TP_CHAR);
    e->const_p = TRUE;
    if (char_is_signed_p ())
      e->c.i_val = r->u.ch;
    else
      e->c.u_val = r->u.ch;
    break;
  case N_CH16:
  case N_CH32:
    e = create_basic_type_expr (c2m_ctx, r, get_uint_basic_type (r->code == N_CH16 ? 2 : 4));
    e->const_p = TRUE;
    e->c.u_val = r->u.ul;
    break;
  case N_STR:
  case N_STR16:
  case N_STR32: {
    struct arr_type *arr_type;
    int size = r->code == N_STR ? 1 : r->code == N_STR16 ? 2 : 4;

    e = create_expr (c2m_ctx, r);
    e->u.lvalue_node = r;
    e->type->mode = TM_ARR;
    e->type->pos_node = r;
    e->type->u.arr_type = arr_type = reg_malloc (c2m_ctx, sizeof (struct arr_type));
    clear_type_qual (&arr_type->ind_type_qual);
    arr_type->static_p = FALSE;
    arr_type->el_type = create_type (c2m_ctx, NULL);
    arr_type->el_type->pos_node = r;
    arr_type->el_type->mode = TM_BASIC;
    arr_type->el_type->u.basic_type = size == 1 ? TP_CHAR : get_uint_basic_type (size);
    arr_type->size = new_i_node (c2m_ctx, (long) r->u.s.len, POS (r));
    check (c2m_ctx, arr_type->size, NULL);
    break;
  }
  case N_ID: {
    node_t aux_node = NULL;
    decl_t decl;

    op1 = find_def (c2m_ctx, S_REGULAR, r, curr_scope, &aux_node);
    e = create_expr (c2m_ctx, r);
    e->def_node = op1;
    if (op1 == NULL) {
      error (c2m_ctx, POS (r), "undeclared identifier %s", r->u.s.s);
    } else if (op1->code == N_IGNORE) {
      e->type->mode = TM_BASIC;
      e->type->u.basic_type = TP_INT;
    } else if (op1->code == N_SPEC_DECL) {
      decl = op1->attr;
      if (decl->decl_spec.typedef_p)
        error (c2m_ctx, POS (r), "typedef name %s as an operand", r->u.s.s);
      decl->used_p = TRUE;
      *e->type = *decl->decl_spec.type;
      if (e->type->mode != TM_FUNC) e->u.lvalue_node = op1;
    } else if (op1->code == N_FUNC_DEF) {
      decl = op1->attr;
      decl->used_p = TRUE;
      assert (decl->decl_spec.type->mode == TM_FUNC);
      *e->type = *decl->decl_spec.type;
    } else if (op1->code == N_ENUM_CONST) {
      assert (aux_node && aux_node->code == N_ENUM);
      e->type->mode = TM_ENUM;
      e->type->pos_node = r;
      e->type->u.tag_type = aux_node;
      e->const_p = TRUE;
      e->c.i_val = ((struct enum_value *) op1->attr)->u.i_val;
    } else { /* it is a member reference inside struct/union */
      assert (op1->code == N_MEMBER);
    }
    break;
  }
  case N_COMMA:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = create_expr (c2m_ctx, r);
    *e->type = *e2->type;
    break;
  case N_ANDAND:
  case N_OROR:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!scalar_type_p (t1) || !scalar_type_p (t2)) {
      error (c2m_ctx, POS (r), "invalid operand types of %s", r->code == N_ANDAND ? "&&" : "||");
    } else if (e1->const_p) {
      int v;

      if (floating_type_p (t1))
        v = e1->c.d_val != 0.0;
      else if (signed_integer_type_p (t1))
        v = e1->c.i_val != 0;
      else
        v = e1->c.u_val != 0;
      if (v && r->code == N_OROR) {
        e->const_p = TRUE;
        e->c.i_val = v;
      } else if (!v && r->code == N_ANDAND) {
        e->const_p = TRUE;
        e->c.i_val = v;
      } else if (e2->const_p) {
        e->const_p = TRUE;
        if (floating_type_p (t2))
          v = e2->c.d_val != 0.0;
        else if (signed_integer_type_p (t2))
          v = e2->c.i_val != 0;
        else
          v = e2->c.u_val != 0;
        e->c.i_val = v;
      }
    }
    break;
  case N_EQ:
  case N_NE:
  case N_LT:
  case N_LE:
  case N_GT:
  case N_GE:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if ((r->code == N_EQ || r->code == N_NE)
        && ((t1->mode == TM_PTR && null_const_p (e2, t2))
            || (t2->mode == TM_PTR && null_const_p (e1, t1))))
      ;
    else if (t1->mode == TM_PTR && t2->mode == TM_PTR) {
      if (!compatible_types_p (t1, t2, TRUE)
          && ((r->code != N_EQ && r->code != N_NE) || (!void_ptr_p (t1) && !void_ptr_p (t2)))) {
        (c2m_options->pedantic_p ? error : warning) (c2m_ctx, POS (r),
                                                     "incompatible pointer types in comparison");
      } else if (t1->u.ptr_type->type_qual.atomic_p || t2->u.ptr_type->type_qual.atomic_p) {
        error (c2m_ctx, POS (r), "pointer to atomic type as a comparison operand");
      } else if (e1->const_p && e2->const_p) {
        e->const_p = TRUE;
        e->c.i_val = (r->code == N_EQ   ? e1->c.u_val == e2->c.u_val
                      : r->code == N_NE ? e1->c.u_val != e2->c.u_val
                      : r->code == N_LT ? e1->c.u_val < e2->c.u_val
                      : r->code == N_LE ? e1->c.u_val <= e2->c.u_val
                      : r->code == N_GT ? e1->c.u_val > e2->c.u_val
                                        : e1->c.u_val >= e2->c.u_val);
      }
    } else if (arithmetic_type_p (t1) && arithmetic_type_p (t2)) {
      if (e1->const_p && e2->const_p) {
        t = arithmetic_conversion (t1, t2);
        convert_value (e1, &t);
        convert_value (e2, &t);
        e->const_p = TRUE;
        if (floating_type_p (&t))
          e->c.i_val = (r->code == N_EQ   ? e1->c.d_val == e2->c.d_val
                        : r->code == N_NE ? e1->c.d_val != e2->c.d_val
                        : r->code == N_LT ? e1->c.d_val < e2->c.d_val
                        : r->code == N_LE ? e1->c.d_val <= e2->c.d_val
                        : r->code == N_GT ? e1->c.d_val > e2->c.d_val
                                          : e1->c.d_val >= e2->c.d_val);
        else if (signed_integer_type_p (&t))
          e->c.i_val = (r->code == N_EQ   ? e1->c.i_val == e2->c.i_val
                        : r->code == N_NE ? e1->c.i_val != e2->c.i_val
                        : r->code == N_LT ? e1->c.i_val < e2->c.i_val
                        : r->code == N_LE ? e1->c.i_val <= e2->c.i_val
                        : r->code == N_GT ? e1->c.i_val > e2->c.i_val
                                          : e1->c.i_val >= e2->c.i_val);
        else
          e->c.i_val = (r->code == N_EQ   ? e1->c.u_val == e2->c.u_val
                        : r->code == N_NE ? e1->c.u_val != e2->c.u_val
                        : r->code == N_LT ? e1->c.u_val < e2->c.u_val
                        : r->code == N_LE ? e1->c.u_val <= e2->c.u_val
                        : r->code == N_GT ? e1->c.u_val > e2->c.u_val
                                          : e1->c.u_val >= e2->c.u_val);
      }
    } else if ((t1->mode == TM_PTR && integer_type_p (t2))
               || (t2->mode == TM_PTR && integer_type_p (t1))) {
      warning (c2m_ctx, POS (r), "comparison of integer with a pointer");
    } else {
      error (c2m_ctx, POS (r), "invalid types of comparison operands");
    }
    break;
  case N_BITWISE_NOT:
  case N_NOT:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (r->code == N_BITWISE_NOT && !integer_type_p (t1)) {
      error (c2m_ctx, POS (r), "bitwise-not operand should be of an integer type");
    } else if (r->code == N_NOT && !scalar_type_p (t1)) {
      error (c2m_ctx, POS (r), "not operand should be of a scalar type");
    } else if (r->code == N_BITWISE_NOT) {
      t = integer_promotion (t1);
      e->type->u.basic_type = t.u.basic_type;
      if (e1->const_p) {
        convert_value (e1, &t);
        e->const_p = TRUE;
        if (signed_integer_type_p (&t))
          e->c.i_val = ~e1->c.i_val;
        else
          e->c.u_val = ~e1->c.u_val;
      }
    } else if (e1->const_p) {
      e->const_p = TRUE;
      if (floating_type_p (t1))
        e->c.i_val = e1->c.d_val == 0.0;
      else if (signed_integer_type_p (t1))
        e->c.i_val = e1->c.i_val == 0;
      else
        e->c.i_val = e1->c.u_val == 0;
    }
    break;
  case N_INC:
  case N_DEC:
  case N_POST_INC:
  case N_POST_DEC: {
    struct expr saved_expr;

    process_unop (c2m_ctx, r, &op1, &e1, &t1, NULL);
    saved_expr = *e1;
    t1 = e1->type = adjust_type (c2m_ctx, e1->type);
    get_int_node (c2m_ctx, &op2, &e2, &t2,
                  t1->mode != TM_PTR ? 1 : type_size (c2m_ctx, t1->u.ptr_type));
    e = check_assign_op (c2m_ctx, r, e1, e2, t1, t2);
    t2 = ((struct expr *) r->attr)->type;
    *e1 = saved_expr;
    t1 = e1->type;
    assign_expr_type = create_type (c2m_ctx, NULL);
    *assign_expr_type = *e->type;
    goto assign;
    break;
  }
  case N_ADD:
  case N_SUB:
    if (NL_NEXT (NL_HEAD (r->u.ops)) == NULL) { /* unary */
      process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
      e = create_expr (c2m_ctx, r);
      e->type->mode = TM_BASIC;
      e->type->u.basic_type = TP_INT;
      if (!arithmetic_type_p (t1)) {
        error (c2m_ctx, POS (r), "unary + or - operand should be of an arithmentic type");
      } else {
        if (e1->const_p) e->const_p = TRUE;
        if (floating_type_p (t1)) {
          e->type->u.basic_type = t1->u.basic_type;
          if (e->const_p) e->c.d_val = (r->code == N_ADD ? +e1->c.d_val : -e1->c.d_val);
        } else {
          t = integer_promotion (t1);
          e->type->u.basic_type = t.u.basic_type;
          if (e1->const_p) {
            convert_value (e1, &t);
            if (signed_integer_type_p (&t))
              e->c.i_val = (r->code == N_ADD ? +e1->c.i_val : -e1->c.i_val);
            else
              e->c.u_val = (r->code == N_ADD ? +e1->c.u_val : -e1->c.u_val);
          }
        }
      }
      break;
    }
    /* falls through */
  case N_AND:
  case N_OR:
  case N_XOR:
  case N_LSH:
  case N_RSH:
  case N_MUL:
  case N_DIV:
  case N_MOD:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    e = check_assign_op (c2m_ctx, r, e1, e2, t1, t2);
    break;
  case N_AND_ASSIGN:
  case N_OR_ASSIGN:
  case N_XOR_ASSIGN:
  case N_LSH_ASSIGN:
  case N_RSH_ASSIGN:
  case N_ADD_ASSIGN:
  case N_SUB_ASSIGN:
  case N_MUL_ASSIGN:
  case N_DIV_ASSIGN:
  case N_MOD_ASSIGN: {
    struct expr saved_expr;

    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, NULL);
    saved_expr = *e1;
    t1 = e1->type = adjust_type (c2m_ctx, e1->type);
    t2 = e2->type = adjust_type (c2m_ctx, e2->type);
    e = check_assign_op (c2m_ctx, r, e1, e2, t1, t2);
    assign_expr_type = create_type (c2m_ctx, NULL);
    *assign_expr_type = *e->type;
    t2 = ((struct expr *) r->attr)->type;
    *e1 = saved_expr;
    t1 = e1->type;
    goto assign;
    break;
  }
  case N_ASSIGN:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, NULL);
    t2 = e2->type = adjust_type (c2m_ctx, e2->type);
    assign_expr_type = NULL;
  assign:
    e = create_expr (c2m_ctx, r);
    if (!e1->u.lvalue_node) {
      error (c2m_ctx, POS (r), "lvalue required as left operand of assignment");
    }
    check_assignment_types (c2m_ctx, t1, t2, e2, r);
    *e->type = *t1;
    if ((e->type2 = assign_expr_type) != NULL) set_type_layout (c2m_ctx, assign_expr_type);
    break;
  case N_IND:
    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    if (t1->mode != TM_PTR && t1->mode != TM_ARR && (t2->mode == TM_PTR || t2->mode == TM_ARR)) {
      struct type *temp;
      node_t op;

      SWAP (t1, t2, temp);
      SWAP (e1, e2, e);
      SWAP (op1, op2, op);
      NL_REMOVE (r->u.ops, op1);
      NL_APPEND (r->u.ops, op1);
    }
    e = create_expr (c2m_ctx, r);
    e->u.lvalue_node = r;
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (t1->mode != TM_PTR && t1->mode != TM_ARR) {
      error (c2m_ctx, POS (r), "subscripted value is neither array nor pointer");
    } else if (t1->mode == TM_PTR) {
      *e->type = *t1->u.ptr_type;
      if (incomplete_type_p (c2m_ctx, t1->u.ptr_type)) {
        error (c2m_ctx, POS (r), "pointer to incomplete type in array subscription");
      }
    } else {
      *e->type = *t1->u.arr_type->el_type;
      e->type->type_qual = t1->u.arr_type->ind_type_qual;
      if (incomplete_type_p (c2m_ctx, e->type)) {
        error (c2m_ctx, POS (r), "array type has incomplete element type");
      }
    }
    if (!integer_type_p (t2)) {
      error (c2m_ctx, POS (r), "array subscript is not an integer");
    }
    break;
  case N_LABEL_ADDR:
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_PTR;
    e->type->u.ptr_type = &VOID_TYPE;
    VARR_PUSH (node_t, label_uses, r);
    break;
  case N_ADDR:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    assert (t1->mode != TM_ARR);
    e = create_expr (c2m_ctx, r);
    if (op1->code == N_DEREF) {
      node_t deref_op = NL_HEAD (op1->u.ops);

      *e->type = *((struct expr *) deref_op->attr)->type;
      break;
    } else if (e1->type->mode == TM_PTR && e1->type->arr_type != NULL) {
      *e->type = *e1->type;
      break;
    } else if (e1->type->mode == TM_PTR && e1->type->u.ptr_type->mode == TM_FUNC
               && e1->type->func_type_before_adjustment_p) {
      *e->type = *e1->type;
      break;
    } else if (!e1->u.lvalue_node) {
      e->type->mode = TM_BASIC;
      e->type->u.basic_type = TP_INT;
      error (c2m_ctx, POS (r), "lvalue required as unary & operand");
      break;
    }
    if (op1->code == N_IND) {
      t2 = create_type (c2m_ctx, t1);
    } else if (op1->code == N_ID) {
      node_t decl_node = e1->u.lvalue_node;
      decl_t decl = decl_node->attr;

      decl->addr_p = TRUE;
      if (decl->decl_spec.register_p)
        error (c2m_ctx, POS (r), "address of register variable %s requested", op1->u.s.s);
      t2 = create_type (c2m_ctx, decl->decl_spec.type);
    } else if (e1->u.lvalue_node->code == N_MEMBER) {
      node_t declarator = NL_EL (e1->u.lvalue_node->u.ops, 1);
      node_t width = NL_NEXT (NL_NEXT (declarator));
      decl_t decl = e1->u.lvalue_node->attr;

      assert (declarator->code == N_DECL);
      if (width->code != N_IGNORE) {
        error (c2m_ctx, POS (r), "cannot take address of bit-field %s",
               NL_HEAD (declarator->u.ops)->u.s.s);
      }
      t2 = create_type (c2m_ctx, decl->decl_spec.type);
      if (op1->code == N_DEREF_FIELD && (e2 = NL_HEAD (op1->u.ops)->attr)->const_p) {
        e->const_p = TRUE;
        e->c.u_val = e2->c.u_val + decl->offset;
      }
    } else if (e1->u.lvalue_node->code == N_COMPOUND_LITERAL) {
      t2 = t1;
    } else {
      assert (e1->u.lvalue_node->code == N_STR || e1->u.lvalue_node->code == N_STR16
              || e1->u.lvalue_node->code == N_STR32);
      t2 = t1;
    }
    if (t2->mode == TM_ARR) {
      e->type = t2;
    } else {
      e->type->mode = TM_PTR;
      e->type->u.ptr_type = t2;
    }
    break;
  case N_DEREF:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (t1->mode != TM_PTR) {
      error (c2m_ctx, POS (r), "invalid type argument of unary *");
    } else {
      *e->type = *t1->u.ptr_type;
      e->u.lvalue_node = r;
    }
    break;
  case N_FIELD:
  case N_DEREF_FIELD: {
    symbol_t sym;
    decl_t decl;
    node_t width;
    struct expr *width_expr;

    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    op2 = NL_NEXT (op1);
    assert (op2->code == N_ID);
    if (r->code == N_DEREF_FIELD && t1->mode == TM_PTR) {
      t1 = t1->u.ptr_type;
    }
    if (t1->mode != TM_STRUCT && t1->mode != TM_UNION) {
      error (c2m_ctx, POS (r), "request for member %s in something not a structure or union",
             op2->u.s.s);
    } else if (!symbol_find (c2m_ctx, S_REGULAR, op2, t1->u.tag_type, &sym)) {
      error (c2m_ctx, POS (r), "%s has no member %s", t1->mode == TM_STRUCT ? "struct" : "union",
             op2->u.s.s);
    } else {
      assert (sym.def_node->code == N_MEMBER);
      decl = sym.def_node->attr;
      *e->type = *decl->decl_spec.type;
      e->u.lvalue_node = sym.def_node;
      if ((width = NL_EL (sym.def_node->u.ops, 3))->code != N_IGNORE && e->type->mode == TM_BASIC
          && (width_expr = width->attr)->const_p
          && width_expr->c.i_val < (mir_llong) sizeof (mir_int) * MIR_CHAR_BIT)
        e->type->u.basic_type = TP_INT;
    }
    break;
  }
  case N_COND: {
    node_t op3;
    struct expr *e3;
    struct type *t3;
    int v = 0;

    process_bin_ops (c2m_ctx, r, &op1, &op2, &e1, &e2, &t1, &t2, r);
    op3 = NL_NEXT (op2);
    check (c2m_ctx, op3, r);
    e3 = op3->attr;
    e3->type = adjust_type (c2m_ctx, e3->type);
    t3 = e3->type;
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
    if (!scalar_type_p (t1)) {
      error (c2m_ctx, POS (r), "condition should be of a scalar type");
      break;
    }
    if (e1->const_p) {
      if (floating_type_p (t1))
        v = e1->c.d_val != 0.0;
      else if (signed_integer_type_p (t1))
        v = e1->c.i_val != 0;
      else
        v = e1->c.u_val != 0;
    }
    if (arithmetic_type_p (t2) && arithmetic_type_p (t3)) {
      t = arithmetic_conversion (t2, t3);
      *e->type = t;
      if (e1->const_p) {
        if (v && e2->const_p) {
          e->const_p = TRUE;
          convert_value (e2, &t);
          e->c = e2->c;
        } else if (!v && e3->const_p) {
          e->const_p = TRUE;
          convert_value (e3, &t);
          e->c = e3->c;
        }
      }
      break;
    }
    if (void_type_p (t2) && void_type_p (t3)) {
      e->type->u.basic_type = TP_VOID;
    } else if ((t2->mode == TM_STRUCT || t2->mode == TM_UNION)
               && (t3->mode == TM_STRUCT || t3->mode == TM_UNION)
               && t2->u.tag_type == t3->u.tag_type) {
      *e->type = *t2;
    } else if ((t2->mode == TM_PTR && null_const_p (e3, t3))
               || (t3->mode == TM_PTR && null_const_p (e2, t2))) {
      e->type = null_const_p (e2, t2) ? t3 : t2;
    } else if (t2->mode != TM_PTR || t3->mode != TM_PTR) {
      error (c2m_ctx, POS (r), "incompatible types in true and false parts of cond-expression");
      break;
    } else if (compatible_types_p (t2, t3, TRUE)) {
      t = composite_type (c2m_ctx, t2->u.ptr_type, t3->u.ptr_type);
      e->type->mode = TM_PTR;
      e->type->pos_node = r;
      e->type->u.ptr_type = create_type (c2m_ctx, &t);
      e->type->u.ptr_type->type_qual
        = type_qual_union (&t2->u.ptr_type->type_qual, &t3->u.ptr_type->type_qual);
      if ((t2->u.ptr_type->type_qual.atomic_p || t3->u.ptr_type->type_qual.atomic_p)
          && !null_const_p (e2, t2) && !null_const_p (e3, t3)) {
        error (c2m_ctx, POS (r),
               "pointer to atomic type in true or false parts of cond-expression");
      }
    } else if (void_ptr_p (t2) || void_ptr_p (t3)) {
      e->type->mode = TM_PTR;
      e->type->pos_node = r;
      e->type->u.ptr_type = create_type (c2m_ctx, e3->type->u.ptr_type);
      e->type->u.ptr_type->pos_node = r;
      assert (!null_const_p (e2, t2) && !null_const_p (e3, t3));
      if (t2->u.ptr_type->type_qual.atomic_p || t3->u.ptr_type->type_qual.atomic_p) {
        error (c2m_ctx, POS (r),
               "pointer to atomic type in true or false parts of cond-expression");
      }
      e->type->u.ptr_type->mode = TM_BASIC;
      e->type->u.ptr_type->u.basic_type = TP_VOID;
      e->type->u.ptr_type->type_qual
        = type_qual_union (&t2->u.ptr_type->type_qual, &t3->u.ptr_type->type_qual);
    } else {
      error (c2m_ctx, POS (r),
             "incompatible pointer types in true and false parts of cond-expression");
      break;
    }
    if (e1->const_p) {
      if (v && e2->const_p) {
        e->const_p = TRUE;
        e->c = e2->c;
      } else if (!v && e3->const_p) {
        e->const_p = TRUE;
        e->c = e3->c;
      }
    }
    break;
  }
  case N_ALIGNOF:
  case N_SIZEOF: {
    struct decl_spec *decl_spec;

    op1 = NL_HEAD (r->u.ops);
    check (c2m_ctx, op1, r);
    e = create_expr (c2m_ctx, r);
    *e->type = get_ptr_int_type (FALSE);
    if (r->code == N_ALIGNOF && op1->code == N_IGNORE) {
      error (c2m_ctx, POS (r), "_Alignof of non-type");
      break;
    }
    assert (op1->code == N_TYPE);
    decl_spec = op1->attr;
    if (incomplete_type_p (c2m_ctx, decl_spec->type)) {
      error (c2m_ctx, POS (r), "%s of incomplete type requested",
             r->code == N_ALIGNOF ? "_Alignof" : "sizeof");
    } else if (decl_spec->type->mode == TM_FUNC) {
      error (c2m_ctx, POS (r), "%s of function type requested",
             r->code == N_ALIGNOF ? "_Alignof" : "sizeof");
    } else {
      e->const_p = TRUE;
      e->c.i_val = (r->code == N_SIZEOF ? type_size (c2m_ctx, decl_spec->type)
                                        : (mir_size_t) type_align (decl_spec->type));
    }
    break;
  }
  case N_EXPR_SIZEOF:
    process_unop (c2m_ctx, r, &op1, &e1, &t1, r);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;  // ???
    if (incomplete_type_p (c2m_ctx, t1)) {
      error (c2m_ctx, POS (r), "sizeof of incomplete type requested");
    } else if (t1->mode == TM_FUNC) {
      error (c2m_ctx, POS (r), "sizeof of function type requested");
    } else if (e1->u.lvalue_node && e1->u.lvalue_node->code == N_MEMBER) {
      node_t declarator = NL_EL (e1->u.lvalue_node->u.ops, 1);
      node_t width = NL_NEXT (NL_NEXT (declarator));

      assert (declarator->code == N_DECL);
      if (width->code != N_IGNORE) {
        error (c2m_ctx, POS (r), "sizeof applied to a bit-field %s",
               NL_HEAD (declarator->u.ops)->u.s.s);
      }
    }
    e->const_p = TRUE;
    e->c.i_val = type_size (c2m_ctx, t1);
    break;
  case N_CAST: {
    struct decl_spec *decl_spec;
    int void_p;

    process_type_bin_ops (c2m_ctx, r, &op1, &op2, &e2, &t2, r);
    e = create_expr (c2m_ctx, r);
    assert (op1->code == N_TYPE);
    decl_spec = op1->attr;
    *e->type = *decl_spec->type;
    void_p = void_type_p (decl_spec->type);
    if (!void_p && !scalar_type_p (decl_spec->type)) {
      error (c2m_ctx, POS (r), "conversion to non-scalar type requested");
    } else if (!void_p && !scalar_type_p (t2) && !void_type_p (t2)) {
      error (c2m_ctx, POS (r), "conversion of non-scalar value requested");
    } else if (t2->mode == TM_PTR && floating_type_p (decl_spec->type)) {
      error (c2m_ctx, POS (r), "conversion of a pointer to floating value requested");
    } else if (decl_spec->type->mode == TM_PTR && floating_type_p (t2)) {
      error (c2m_ctx, POS (r), "conversion of floating point value to a pointer requested");
    } else if (e2->const_p && !void_p) {
      e->const_p = TRUE;
      cast_value (e, e2, decl_spec->type);
    }
    break;
  }
  case N_COMPOUND_LITERAL: {
    node_t list, n;
    decl_t decl;
    int n_spec_index, addr_p;

    op1 = NL_HEAD (r->u.ops);
    list = NL_NEXT (op1);
    assert (op1->code == N_TYPE && list->code == N_LIST);
    check (c2m_ctx, op1, r);
    decl = op1->attr;
    t1 = decl->decl_spec.type;
    check (c2m_ctx, list, r);
    decl->addr_p = TRUE;
    if (incomplete_type_p (c2m_ctx, t1)
        && (t1->mode != TM_ARR || t1->u.arr_type->size->code != N_IGNORE
            || incomplete_type_p (c2m_ctx, t1->u.arr_type->el_type))) {
      error (c2m_ctx, POS (r), "compound literal of incomplete type");
      break;
    }
    for (n_spec_index = (int) VARR_LENGTH (node_t, context_stack) - 1;
         n_spec_index >= 0 && (n = VARR_GET (node_t, context_stack, n_spec_index)) != NULL
         && n->code != N_SPEC_DECL;
         n_spec_index--)
      ;
    if (n_spec_index < (int) VARR_LENGTH (node_t, context_stack) - 1
        && (n_spec_index < 0
            || !get_compound_literal (VARR_GET (node_t, context_stack, n_spec_index + 1), &addr_p)
            || addr_p))
      check_initializer (c2m_ctx, NULL, &t1, list,
                         curr_scope == top_scope || decl->decl_spec.static_p
                           || decl->decl_spec.thread_local_p,
                         FALSE);
    decl->decl_spec.type = t1;
    e = create_expr (c2m_ctx, r);
    e->u.lvalue_node = r;
    *e->type = *t1;
    if (curr_scope != top_scope) VARR_PUSH (decl_t, func_decls_for_allocation, decl);
    break;
  }
  case N_CALL: {
    struct func_type *func_type = NULL; /* to remove an uninitialized warning */
    struct type *ret_type;
    node_t list, spec_list, decl, param_list, start_param, param, arg_list, first_arg, arg;
    node_t saved_scope = curr_scope;
    struct decl_spec *decl_spec;
    mir_size_t saved_call_arg_area_offset_before_args;
    struct type res_type;
    int builtin_call_p, alloca_p = FALSE, va_arg_p = FALSE, va_start_p = FALSE;
    int add_overflow_p = FALSE, sub_overflow_p = FALSE, mul_overflow_p = FALSE, expect_p = FALSE;
    int jcall_p = FALSE, jret_p = FALSE, prop_set_p = FALSE, prop_eq_p = FALSE, prop_ne_p = FALSE;

    op1 = NL_HEAD (r->u.ops);
    if (op1->code == N_ID) {
      alloca_p = str_eq_p (op1->u.s.s, ALLOCA);
      add_overflow_p = strcmp (op1->u.s.s, ADD_OVERFLOW) == 0;
      sub_overflow_p = strcmp (op1->u.s.s, SUB_OVERFLOW) == 0;
      mul_overflow_p = strcmp (op1->u.s.s, MUL_OVERFLOW) == 0;
      expect_p = strcmp (op1->u.s.s, EXPECT) == 0;
      jcall_p = strcmp (op1->u.s.s, JCALL) == 0;
      jret_p = strcmp (op1->u.s.s, JRET) == 0;
      prop_set_p = strcmp (op1->u.s.s, PROP_SET) == 0;
      prop_eq_p = strcmp (op1->u.s.s, PROP_EQ) == 0;
      prop_ne_p = strcmp (op1->u.s.s, PROP_NE) == 0;
    }
    if (op1->code == N_ID && find_def (c2m_ctx, S_REGULAR, op1, curr_scope, NULL) == NULL) {
      va_arg_p = str_eq_p (op1->u.s.s, BUILTIN_VA_ARG);
      va_start_p = str_eq_p (op1->u.s.s, BUILTIN_VA_START);
      if (!va_arg_p && !va_start_p && !alloca_p) {
        /* N_SPEC_DECL (N_SHARE (N_LIST (N_INT)), N_DECL (N_ID, N_FUNC (N_LIST)), N_IGNORE,
           N_IGNORE, N_IGNORE) */
        spec_list = new_node (c2m_ctx, N_LIST);
        op_append (c2m_ctx, spec_list, new_node (c2m_ctx, N_INT));
        list = new_node (c2m_ctx, N_LIST);
        op_append (c2m_ctx, list, new_node1 (c2m_ctx, N_FUNC, new_node (c2m_ctx, N_LIST)));
        decl
          = new_pos_node5 (c2m_ctx, N_SPEC_DECL, POS (op1), new_node1 (c2m_ctx, N_SHARE, spec_list),
                           new_node2 (c2m_ctx, N_DECL, copy_node (c2m_ctx, op1), list),
                           new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_IGNORE),
                           new_node (c2m_ctx, N_IGNORE));
        curr_scope = top_scope;
        check (c2m_ctx, decl, NULL);
        curr_scope = saved_scope;
        assert (top_scope->code == N_MODULE);
        list = NL_HEAD (top_scope->u.ops);
        assert (list->code == N_LIST);
        op_prepend (c2m_ctx, list, decl);
      }
    }
    builtin_call_p = alloca_p || va_arg_p || va_start_p || add_overflow_p || sub_overflow_p
                     || mul_overflow_p || expect_p || jcall_p || jret_p || prop_set_p || prop_eq_p
                     || prop_ne_p;
    if (!builtin_call_p || jcall_p) VARR_PUSH (node_t, call_nodes, r);
    arg_list = NL_NEXT (op1);
    if (builtin_call_p) {
      if (func_block_scope != NULL && (va_arg_p || va_start_p))
        ((struct node_scope *) func_block_scope->attr)->stack_var_p = TRUE;
      for (arg = NL_HEAD (arg_list->u.ops); arg != NULL; arg = NL_NEXT (arg))
        check (c2m_ctx, arg, r);
      init_type (&res_type);
      if (alloca_p) {
        res_type.mode = TM_PTR;
        res_type.u.ptr_type = &VOID_TYPE;
      } else {
        res_type.mode = TM_BASIC;
        res_type.u.basic_type
          = (va_arg_p || add_overflow_p || sub_overflow_p || mul_overflow_p ? TP_INT
             : expect_p || prop_eq_p || prop_ne_p                           ? TP_LONG
                                                                            : TP_VOID);
      }
      ret_type = &res_type;
      if (builtin_call_p
          && ((va_start_p && NL_LENGTH (arg_list->u.ops) != 1)
              || (alloca_p && NL_LENGTH (arg_list->u.ops) != 1)
              || (add_overflow_p && NL_LENGTH (arg_list->u.ops) != 3)
              || (sub_overflow_p && NL_LENGTH (arg_list->u.ops) != 3)
              || (mul_overflow_p && NL_LENGTH (arg_list->u.ops) != 3)
              || (expect_p && NL_LENGTH (arg_list->u.ops) != 2)
              || (jret_p && NL_LENGTH (arg_list->u.ops) != 1)
              || (va_arg_p && NL_LENGTH (arg_list->u.ops) != 2)
              || (prop_set_p && NL_LENGTH (arg_list->u.ops) != 2)
              || ((prop_eq_p || prop_ne_p) && NL_LENGTH (arg_list->u.ops) != 2))) {
        error (c2m_ctx, POS (op1), "wrong number of arguments in %s call", op1->u.s.s);
      } else {
        /* first argument type ??? */
        if (va_arg_p) {
          arg = NL_EL (arg_list->u.ops, 1);
          e2 = arg->attr;
          t2 = e2->type;
          if (t2->mode != TM_PTR)
            error (c2m_ctx, POS (arg), "wrong type of 2nd argument of %s call", BUILTIN_VA_ARG);
          else
            ret_type = t2->u.ptr_type;
        } else if (add_overflow_p || sub_overflow_p || mul_overflow_p) {
          arg = NL_EL (arg_list->u.ops, 2);
          e2 = arg->attr;
          t2 = e2->type;
          if (t2->mode != TM_PTR || !standard_integer_type_p (t2->u.ptr_type)
              || t2->u.ptr_type->u.basic_type < TP_INT) /* only [u]int, [u]long, [u]longlong */
            error (c2m_ctx, POS (arg), "wrong type of 3rd argument of %s call",
                   add_overflow_p   ? ADD_OVERFLOW
                   : sub_overflow_p ? SUB_OVERFLOW
                                    : MUL_OVERFLOW);
          for (int i = 0; i < 2; i++) {
            arg = NL_EL (arg_list->u.ops, i);
            e2 = arg->attr;
            if (!integer_type_p (e2->type))
              error (c2m_ctx, POS (arg), "non-integer type of %d argument of %s call", i,
                     add_overflow_p   ? ADD_OVERFLOW
                     : sub_overflow_p ? SUB_OVERFLOW
                                      : MUL_OVERFLOW);
          }
        } else if (expect_p) {
          for (int i = 0; i < 2; i++) {
            arg = NL_EL (arg_list->u.ops, i);
            e2 = arg->attr;
            if (!integer_type_p (e2->type))
              error (c2m_ctx, POS (arg), "non-integer type of %d argument of %s call", i, EXPECT);
          }
        } else if (jret_p) {
          arg = NL_HEAD (arg_list->u.ops);
          e2 = arg->attr;
          t2 = e2->type;
          if (t2->mode != TM_PTR)
            error (c2m_ctx, POS (arg), "non-pointer type of 1st argument of %s call", JRET);
        } else if (jcall_p) {
          arg = NL_HEAD (arg_list->u.ops);
          e2 = arg->attr;
          t2 = e2->type;
          if (t2->mode != TM_PTR || (t2 = t2->u.ptr_type)->mode != TM_FUNC) {
            error (c2m_ctx, POS (r), "calling non-function in %s", JCALL);
            break;
          }
          func_type = t2->u.func_type;
          ret_type = func_type->ret_type;
          if (!void_type_p (ret_type)) {
            error (c2m_ctx, POS (arg), "calling non-void function in %s", JCALL);
            break;
          }
        } else if (prop_set_p || prop_eq_p || prop_ne_p) {
          arg = NL_HEAD (arg_list->u.ops);
          e2 = arg->attr;
          if (!e2->u.lvalue_node)
            error (c2m_ctx, POS (r), "1st arg of %s should be lvalue", op1->u.s.s);
          arg = NL_NEXT (arg);
          e2 = arg->attr;
          t2 = e2->type;
          if (!e2->const_p || !integer_type_p (t2))
            error (c2m_ctx, POS (arg),
                   "property value in 2nd arg of %s call should be an integer constant",
                   op1->u.s.s);
        }
      }
    } else {
      check (c2m_ctx, op1, r);
      e1 = op1->attr;
      t1 = e1->type;
      if (t1->mode != TM_PTR || (t1 = t1->u.ptr_type)->mode != TM_FUNC) {
        error (c2m_ctx, POS (r), "called object is not a function or function pointer");
        break;
      }
      func_type = t1->u.func_type;
      ret_type = func_type->ret_type;
    }
    e = create_expr (c2m_ctx, r);
    *e->type = *ret_type;
    e->builtin_call_p = builtin_call_p;
    if ((ret_type->mode != TM_BASIC || ret_type->u.basic_type != TP_VOID)
        && incomplete_type_p (c2m_ctx, ret_type)) {
      error (c2m_ctx, POS (r), "function return type is incomplete");
    }
    if (ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION) {
      set_type_layout (c2m_ctx, ret_type);
      if (!builtin_call_p && curr_scope != top_scope)
        update_call_arg_area_offset (c2m_ctx, ret_type, TRUE);
    }
    if (builtin_call_p && !jcall_p) break;
    first_arg = jcall_p ? NL_EL (arg_list->u.ops, 1) : NL_HEAD (arg_list->u.ops);
    param_list = func_type->param_list;
    param = start_param = NL_HEAD (param_list->u.ops);
    if (void_param_p (start_param)) { /* f(void) */
      if (first_arg != NULL) error (c2m_ctx, POS (first_arg), "too many arguments");
      break;
    }
    saved_call_arg_area_offset_before_args = curr_call_arg_area_offset;
    for (arg = first_arg; arg != NULL; arg = NL_NEXT (arg)) {
      check (c2m_ctx, arg, r);
      e2 = arg->attr;
      if (start_param == NULL || start_param->code == N_ID) continue; /* no params or ident list */
      if (param == NULL) {
        if (!func_type->dots_p) error (c2m_ctx, POS (arg), "too many arguments");
        start_param = NULL; /* ignore the rest args */
        continue;
      }
      assert (param->code == N_SPEC_DECL || param->code == N_TYPE);
      decl_spec = get_param_decl_spec (param);
      check_assignment_types (c2m_ctx, decl_spec->type, NULL, e2, r);
      param = NL_NEXT (param);
    }
    curr_call_arg_area_offset = saved_call_arg_area_offset_before_args;
    if (param != NULL) error (c2m_ctx, POS (r), "too few arguments");
    break;
  }
  case N_GENERIC: {
    node_t list, ga, ga2, type_name, type_name2, expr;
    node_t default_case = NULL, ga_case = NULL;
    struct decl_spec *decl_spec;

    op1 = NL_HEAD (r->u.ops);
    check (c2m_ctx, op1, r);
    e1 = op1->attr;
    t = *e1->type;
    if (integer_type_p (&t)) t = integer_promotion (&t);
    list = NL_NEXT (op1);
    for (ga = NL_HEAD (list->u.ops); ga != NULL; ga = NL_NEXT (ga)) {
      assert (ga->code == N_GENERIC_ASSOC);
      type_name = NL_HEAD (ga->u.ops);
      expr = NL_NEXT (type_name);
      check (c2m_ctx, type_name, r);
      check (c2m_ctx, expr, r);
      if (type_name->code == N_IGNORE) {
        if (default_case) error (c2m_ctx, POS (ga), "duplicate default case in _Generic");
        default_case = ga;
        continue;
      }
      assert (type_name->code == N_TYPE);
      decl_spec = type_name->attr;
      if (incomplete_type_p (c2m_ctx, decl_spec->type)) {
        error (c2m_ctx, POS (ga), "_Generic case has incomplete type");
      } else if (compatible_types_p (&t, decl_spec->type, TRUE)) {
        if (ga_case)
          error (c2m_ctx, POS (ga_case),
                 "_Generic expr type is compatible with more than one generic association type");
        ga_case = ga;
      } else {
        for (ga2 = NL_HEAD (list->u.ops); ga2 != ga; ga2 = NL_NEXT (ga2)) {
          type_name2 = NL_HEAD (ga2->u.ops);
          if (type_name2->code != N_IGNORE
              && !(incomplete_type_p (c2m_ctx, t2 = ((struct decl_spec *) type_name2->attr)->type))
              && compatible_types_p (t2, decl_spec->type, TRUE)) {
            error (c2m_ctx, POS (ga), "two or more compatible generic association types");
            break;
          }
        }
      }
    }
    e = create_expr (c2m_ctx, r);
    if (default_case == NULL && ga_case == NULL) {
      error (c2m_ctx, POS (r), "expression type is not compatible with generic association");
    } else { /* make compatible association a list head  */
      if (ga_case == NULL) ga_case = default_case;
      NL_REMOVE (list->u.ops, ga_case);
      NL_PREPEND (list->u.ops, ga_case);
      t2 = e->type;
      *e = *(struct expr *) NL_EL (ga_case->u.ops, 1)->attr;
      *t2 = *e->type;
      e->type = t2;
    }
    break;
  }
  case N_SPEC_DECL: {
    node_t specs = NL_HEAD (r->u.ops);
    node_t declarator = NL_NEXT (specs);
    node_t attrs = NL_NEXT (declarator);
    node_t asm_part = NL_NEXT (attrs);
    node_t initializer = NL_NEXT (asm_part);
    node_t unshared_specs = specs->code != N_SHARE ? specs : NL_HEAD (specs->u.ops);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, unshared_specs, r);
    int i;
    const char *asm_str = NULL;

    if (asm_part->code == N_ASM && decl_spec.register_p) {
      /* func can have asm which ignore here */
      if (initializer->code != N_IGNORE) {
        error (c2m_ctx, POS (r), "asm register decl with initializer");
      } else if (curr_scope != top_scope) {
        error (c2m_ctx, POS (r), "asm register decl should be at the top level");
      } else {
        asm_str = NL_HEAD (asm_part->u.ops)->u.s.s;
        for (i = 0; asm_str[i] != '\0' && _MIR_name_char_p (c2m_ctx->ctx, asm_str[i], i == 0); i++)
          ;
        if (asm_str[i] != '\0') {
          error (c2m_ctx, POS (r), "asm register name %s contains wrong char '%c'", asm_str,
                 asm_str[i]);
          asm_str = NULL;
        }
      }
    }
    if (declarator->code != N_IGNORE) {
      create_decl (c2m_ctx, curr_scope, r, decl_spec, initializer,
                   context != NULL && context->code == N_FUNC);
      decl_t decl = r->attr;
      const char *antialias = check_attrs (c2m_ctx, r, decl, attrs, TRUE);
      if (decl->decl_spec.type->mode == TM_PTR && antialias != NULL)
        decl->decl_spec.type->antialias = MIR_alias (c2m_ctx->ctx, antialias);
      if (asm_str != NULL) {
        if (!scalar_type_p (decl->decl_spec.type)) {
          error (c2m_ctx, POS (r), "asm register decl should have a scalar type");
        } else {
          decl->reg_p = decl->asm_p = TRUE;
          decl->u.asm_str = asm_str;
        }
      } else if ((initializer == NULL || initializer->code == N_IGNORE)
                 && !decl->decl_spec.typedef_p && !decl->decl_spec.extern_p
                 && (decl->decl_spec.type->mode == TM_STRUCT
                     || decl->decl_spec.type->mode == TM_UNION)) {
        VARR_PUSH (node_t, possible_incomplete_decls, r);
      }
    } else if (decl_spec.type->mode == TM_STRUCT || decl_spec.type->mode == TM_UNION) {
      if (NL_HEAD (decl_spec.type->u.tag_type->u.ops)->code != N_ID)
        error (c2m_ctx, POS (r), "unnamed struct/union with no instances");
    } else if (decl_spec.type->mode != TM_ENUM) {
      error (c2m_ctx, POS (r), "useless declaration");
    }
    /* We have at least one enum constant according to the syntax. */
    break;
  }
  case N_ST_ASSERT: {
    int ok_p;

    op1 = NL_HEAD (r->u.ops);
    check (c2m_ctx, op1, r);
    e1 = op1->attr;
    t1 = e1->type;
    if (!e1->const_p) {
      error (c2m_ctx, POS (r), "expression in static assertion is not constant");
    } else if (!integer_type_p (t1)) {
      error (c2m_ctx, POS (r), "expression in static assertion is not an integer");
    } else {
      if (signed_integer_type_p (t1))
        ok_p = e1->c.i_val != 0;
      else
        ok_p = e1->c.u_val != 0;
      if (!ok_p) {
        assert (NL_NEXT (op1) != NULL
                && (NL_NEXT (op1)->code == N_STR || NL_NEXT (op1)->code == N_STR16
                    || NL_NEXT (op1)->code == N_STR32));
        error (c2m_ctx, POS (r), "static assertion failed: \"%s\"",
               NL_NEXT (op1)->u.s.s);  // ???
      }
    }
    break;
  }
  case N_MEMBER: {
    struct type *type;
    node_t specs = NL_HEAD (r->u.ops);
    node_t declarator = NL_NEXT (specs);
    node_t attrs = NL_NEXT (declarator);
    node_t const_expr = NL_NEXT (attrs);
    node_t unshared_specs = specs->code != N_SHARE ? specs : NL_HEAD (specs->u.ops);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, unshared_specs, r);

    create_decl (c2m_ctx, curr_scope, r, decl_spec, NULL, FALSE);
    type = ((decl_t) r->attr)->decl_spec.type;
    if (const_expr->code != N_IGNORE) {
      struct expr *cexpr;
      check (c2m_ctx, const_expr, r);
      cexpr = const_expr->attr;
      if (cexpr != NULL) {
        if (type->type_qual.atomic_p) error (c2m_ctx, POS (const_expr), "bit field with _Atomic");
        if (!cexpr->const_p) {
          error (c2m_ctx, POS (const_expr), "bit field is not a constant expr");
        } else if (!integer_type_p (type)
                   && (type->mode != TM_BASIC || type->u.basic_type != TP_BOOL)) {
          error (c2m_ctx, POS (const_expr),
                 "bit field type should be _Bool, a signed integer, or an unsigned integer type");
        } else if (!integer_type_p (cexpr->type)
                   && (cexpr->type->mode != TM_BASIC || cexpr->type->u.basic_type != TP_BOOL)) {
          error (c2m_ctx, POS (const_expr), "bit field width is not of an integer type");
        } else if (signed_integer_type_p (cexpr->type) && cexpr->c.i_val < 0) {
          error (c2m_ctx, POS (const_expr), "bit field width is negative");
        } else if (cexpr->c.i_val == 0 && declarator->code == N_DECL) {
          error (c2m_ctx, POS (const_expr), "zero bit field width for %s",
                 NL_HEAD (declarator->u.ops)->u.s.s);
        } else if ((!signed_integer_type_p (cexpr->type)
                    && cexpr->c.u_val > (mir_ullong) int_bit_size (type))
                   || (signed_integer_type_p (cexpr->type)
                       && cexpr->c.i_val > int_bit_size (type))) {
          error (c2m_ctx, POS (const_expr), "bit field width exceeds its type");
        }
      }
    }
    if (declarator->code == N_IGNORE) {
      if (((decl_spec.type->mode != TM_STRUCT && decl_spec.type->mode != TM_UNION)
           || NL_HEAD (decl_spec.type->u.tag_type->u.ops)->code != N_IGNORE)
          && const_expr->code == N_IGNORE)
        error (c2m_ctx, POS (r), "no declarator in struct or union declaration");
    } else {
      node_t id = NL_HEAD (declarator->u.ops);
      const char *antialias = check_attrs (c2m_ctx, r, r->attr, attrs, TRUE);
      if (type->mode == TM_PTR && antialias != NULL)
        type->antialias = MIR_alias (c2m_ctx->ctx, antialias);
      if (type->mode == TM_FUNC) {
        error (c2m_ctx, POS (id), "field %s is declared as a function", id->u.s.s);
      } else if (incomplete_type_p (c2m_ctx, type)) {
        /* el_type is checked on completness in N_ARR */
        if (type->mode != TM_ARR || type->u.arr_type->size->code != N_IGNORE)
          error (c2m_ctx, POS (id), "field %s has incomplete type", id->u.s.s);
      }
    }
    break;
  }
  case N_INIT: {
    node_t des_list = NL_HEAD (r->u.ops), initializer = NL_NEXT (des_list);

    check (c2m_ctx, des_list, r);
    check (c2m_ctx, initializer, r);
    break;
  }
  case N_FUNC_DEF: {
    node_t specs = NL_HEAD (r->u.ops);
    node_t declarator = NL_NEXT (specs);
    node_t declarations = NL_NEXT (declarator);
    node_t block = NL_NEXT (declarations);
    node_t id = NL_HEAD (declarator->u.ops);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, specs, r);
    node_t decl_node, p, next_p, param_list, param_id, param_declarator, func;
    symbol_t sym;
    struct node_scope *ns;

    if (str_eq_p (id->u.s.s, ALLOCA) || str_eq_p (id->u.s.s, BUILTIN_VA_START)
        || str_eq_p (id->u.s.s, BUILTIN_VA_ARG) || strcmp (id->u.s.s, ADD_OVERFLOW) == 0
        || strcmp (id->u.s.s, SUB_OVERFLOW) == 0 || strcmp (id->u.s.s, MUL_OVERFLOW) == 0
        || strcmp (id->u.s.s, EXPECT) == 0 || strcmp (id->u.s.s, JCALL) == 0
        || strcmp (id->u.s.s, JRET) == 0) {
      error (c2m_ctx, POS (id), "%s is a builtin function", id->u.s.s);
      break;
    }
    curr_func_scope_num = 0;
    create_node_scope (c2m_ctx, block);
    func_block_scope = curr_scope;
    curr_func_def = r;
    jump_ret_p = FALSE;
    curr_switch = curr_loop = curr_loop_switch = NULL;
    curr_call_arg_area_offset = 0;
    VARR_TRUNC (decl_t, func_decls_for_allocation, 0);
    create_decl (c2m_ctx, top_scope, r, decl_spec, NULL, FALSE);
    curr_scope = func_block_scope;
    check (c2m_ctx, declarations, r);
    /* Process parameter identifier list:  */
    assert (declarator->code == N_DECL);
    func = NL_HEAD (NL_EL (declarator->u.ops, 1)->u.ops);
    /* func can be NULL or non-func in case of error */
    if (n_errors != 0 && (func == NULL || func->code != N_FUNC)) break;
    assert (func != NULL && func->code == N_FUNC);
    param_list = NL_HEAD (func->u.ops);
    for (p = NL_HEAD (param_list->u.ops); p != NULL; p = next_p) {
      next_p = NL_NEXT (p);
      if (p->code != N_ID) break;
      NL_REMOVE (param_list->u.ops, p);
      if (!symbol_find (c2m_ctx, S_REGULAR, p, curr_scope, &sym)) {
        if (c2m_options->pedantic_p) {
          error (c2m_ctx, POS (p), "parameter %s has no type", p->u.s.s);
        } else {
          warning (c2m_ctx, POS (p), "type of parameter %s defaults to int", p->u.s.s);
          decl_node = new_pos_node5 (c2m_ctx, N_SPEC_DECL, POS (p),
                                     new_node1 (c2m_ctx, N_SHARE,
                                                new_node1 (c2m_ctx, N_LIST,
                                                           new_pos_node (c2m_ctx, N_INT, POS (p)))),
                                     new_pos_node2 (c2m_ctx, N_DECL, POS (p),
                                                    new_str_node (c2m_ctx, N_ID, p->u.s, POS (p)),
                                                    new_node (c2m_ctx, N_LIST)),
                                     new_node (c2m_ctx, N_IGNORE), new_node (c2m_ctx, N_IGNORE),
                                     new_node (c2m_ctx, N_IGNORE));
          NL_APPEND (param_list->u.ops, decl_node);
          check (c2m_ctx, decl_node, r);
        }
      } else {
        struct decl_spec *decl_spec_ptr;

        decl_node = sym.def_node;
        assert (decl_node->code == N_SPEC_DECL);
        NL_REMOVE (declarations->u.ops, decl_node);
        NL_APPEND (param_list->u.ops, decl_node);
        param_declarator = NL_EL (decl_node->u.ops, 1);
        assert (param_declarator->code == N_DECL);
        param_id = NL_HEAD (param_declarator->u.ops);
        if (NL_NEXT (NL_NEXT (param_declarator))->code != N_IGNORE) {
          error (c2m_ctx, POS (p), "initialized parameter %s", param_id->u.s.s);
        }
        decl_spec_ptr = &((decl_t) decl_node->attr)->decl_spec;
        adjust_param_type (c2m_ctx, &decl_spec_ptr->type);
        decl_spec = *decl_spec_ptr;
        if (decl_spec.typedef_p || decl_spec.extern_p || decl_spec.static_p || decl_spec.auto_p
            || decl_spec.thread_local_p) {
          error (c2m_ctx, POS (param_id), "storage specifier in a function parameter %s",
                 param_id->u.s.s);
        }
      }
    }
    /* Process the rest declarations: */
    for (p = NL_HEAD (declarations->u.ops); p != NULL; p = NL_NEXT (p)) {
      if (p->code == N_ST_ASSERT) continue;
      assert (p->code == N_SPEC_DECL);
      param_declarator = NL_EL (p->u.ops, 1);
      if (param_declarator->code == N_IGNORE) continue;
      assert (param_declarator->code == N_DECL);
      param_id = NL_HEAD (param_declarator->u.ops);
      assert (param_id->code == N_ID);
      error (c2m_ctx, POS (param_id), "declaration for parameter %s but no such parameter",
             param_id->u.s.s);
    }
    add__func__def (c2m_ctx, block, id->u.s);
    check (c2m_ctx, block, r);
    /* Process all label uses: */
    for (size_t i = 0; i < VARR_LENGTH (node_t, label_uses); i++) {
      node_t n = VARR_GET (node_t, label_uses, i);

      id = n->code == N_LABEL_ADDR ? NL_HEAD (n->u.ops) : NL_NEXT (NL_HEAD (n->u.ops));
      if (!symbol_find (c2m_ctx, S_LABEL, id, func_block_scope, &sym)) {
        error (c2m_ctx, POS (id), "undefined label %s", id->u.s.s);
      } else if (n->code == N_LABEL_ADDR) {
        ((struct expr *) n->attr)->u.label_addr_target = sym.def_node;
      } else {
        n->attr = sym.def_node;
      }
    }
    VARR_TRUNC (node_t, label_uses, 0);
    assert (curr_scope == top_scope); /* set up in the block */
    func_block_scope = top_scope;
    process_func_decls_for_allocation (c2m_ctx);
    /* Add call arg area */
    ns = block->attr;
    ns->size = round_size (ns->size, MAX_ALIGNMENT);
    ns->size += ns->call_arg_area_size;
    break;
  }
  case N_TYPE: {
    struct type *type;
    node_t specs = NL_HEAD (r->u.ops);
    node_t abstract_declarator = NL_NEXT (specs);
    struct decl_spec decl_spec = check_decl_spec (c2m_ctx, specs, r); /* only spec_qual_list here */

    type = check_declarator (c2m_ctx, abstract_declarator, FALSE);
    assert (NL_HEAD (abstract_declarator->u.ops)->code == N_IGNORE);
    decl_spec.type = append_type (type, decl_spec.type);
    if (context && context->code == N_COMPOUND_LITERAL) {
      r->attr = reg_malloc (c2m_ctx, sizeof (struct decl));
      init_decl (c2m_ctx, r->attr);
      ((struct decl *) r->attr)->decl_spec = decl_spec;
      check_type (c2m_ctx, decl_spec.type, 0, FALSE);
      set_type_layout (c2m_ctx, decl_spec.type);
      check_decl_align (c2m_ctx, &((struct decl *) r->attr)->decl_spec);
    } else {
      r->attr = reg_malloc (c2m_ctx, sizeof (struct decl_spec));
      *((struct decl_spec *) r->attr) = decl_spec;
      check_type (c2m_ctx, decl_spec.type, 0, FALSE);
      set_type_layout (c2m_ctx, decl_spec.type);
      check_decl_align (c2m_ctx, r->attr);
    }
    break;
  }
  case N_STMTEXPR: {
    node_t block = NL_HEAD (r->u.ops);
    if (c2m_options->pedantic_p) {
      error (c2m_ctx, POS (r), "statement expression is not a part of C11 standard");
      break;
    }
    check (c2m_ctx, block, r);
    node_t last_stmt = NL_TAIL (NL_EL (block->u.ops, 1)->u.ops);
    if (!last_stmt || last_stmt->code != N_EXPR) {
      error (c2m_ctx, POS (r), "last statement in statement expression is not an expression");
      break;
    }
    node_t expr = NL_EL (last_stmt->u.ops, 1);
    e1 = expr->attr;
    t1 = e1->type;
    e = create_expr (c2m_ctx, r);
    *e->type = *t1;
    break;
  }
  case N_BLOCK:
    check_labels (c2m_ctx, NL_HEAD (r->u.ops), r);
    if (curr_scope != r)
      create_node_scope (c2m_ctx, r); /* it happens if it is the top func block */
    check (c2m_ctx, NL_EL (r->u.ops, 1), r);
    finish_scope (c2m_ctx);
    break;
  case N_MODULE:
    create_node_scope (c2m_ctx, r);
    top_scope = curr_scope;
    check (c2m_ctx, NL_HEAD (r->u.ops), r);
    finish_scope (c2m_ctx);
    break;
  case N_IF: {
    node_t labels = NL_HEAD (r->u.ops);
    node_t expr = NL_NEXT (labels);
    node_t if_stmt = NL_NEXT (expr);
    node_t else_stmt = NL_NEXT (if_stmt);

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    e1 = expr->attr;
    t1 = e1->type;
    if (!scalar_type_p (t1)) {
      error (c2m_ctx, POS (expr), "if-expr should be of a scalar type");
    }
    check (c2m_ctx, if_stmt, r);
    check (c2m_ctx, else_stmt, r);
    break;
  }
  case N_SWITCH: {
    node_t saved_switch = curr_switch;
    node_t saved_loop_switch = curr_loop_switch;
    node_t labels = NL_HEAD (r->u.ops);
    node_t expr = NL_NEXT (labels);
    node_t stmt = NL_NEXT (expr);
    struct type *type;
    struct switch_attr *switch_attr;
    case_t el;
    node_t case_expr, case_expr2, another_case_expr, another_case_expr2;
    struct expr *case_e, *case_e2, *another_case_e, *another_case_e2;
    int signed_p, skip_range_p;

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    type = ((struct expr *) expr->attr)->type;
    if (!integer_type_p (type)) {
      init_type (&t);
      t.mode = TM_BASIC;
      t.u.basic_type = TP_INT;
      error (c2m_ctx, POS (expr), "switch-expr is of non-integer type");
    } else {
      t = integer_promotion (type);
    }
    signed_p = signed_integer_type_p (type);
    curr_switch = curr_loop_switch = r;
    switch_attr = curr_switch->attr = reg_malloc (c2m_ctx, sizeof (struct switch_attr));
    switch_attr->type = t;
    switch_attr->ranges_p = FALSE;
    switch_attr->min_val_case = switch_attr->max_val_case = NULL;
    DLIST_INIT (case_t, ((struct switch_attr *) curr_switch->attr)->case_labels);
    check (c2m_ctx, stmt, r);
    for (case_t c = DLIST_HEAD (case_t, switch_attr->case_labels); c != NULL;
         c = DLIST_NEXT (case_t, c)) { /* process simple cases */
      if (c->case_node->code == N_DEFAULT || NL_EL (c->case_node->u.ops, 1) != NULL) continue;
      if (HTAB_DO (case_t, case_tab, c, HTAB_FIND, el)) {
        error (c2m_ctx, POS (c->case_node), "duplicate case value");
        continue;
      }
      HTAB_DO (case_t, case_tab, c, HTAB_INSERT, el);
      if (switch_attr->min_val_case == NULL) {
        switch_attr->min_val_case = switch_attr->max_val_case = c;
        continue;
      }
      case_e = NL_HEAD (c->case_node->u.ops)->attr;
      case_e2 = NL_HEAD (switch_attr->min_val_case->case_node->u.ops)->attr;
      if (signed_p ? case_e->c.i_val < case_e2->c.i_val : case_e->c.u_val < case_e2->c.u_val)
        switch_attr->min_val_case = c;
      case_e2 = NL_HEAD (switch_attr->max_val_case->case_node->u.ops)->attr;
      if (signed_p ? case_e->c.i_val > case_e2->c.i_val : case_e->c.u_val > case_e2->c.u_val)
        switch_attr->max_val_case = c;
    }
    HTAB_CLEAR (case_t, case_tab);
    /* Check range cases against *all* simple cases or range cases *before* it. */
    for (case_t c = DLIST_HEAD (case_t, switch_attr->case_labels); c != NULL;
         c = DLIST_NEXT (case_t, c)) {
      if (c->case_node->code == N_DEFAULT || (case_expr2 = NL_EL (c->case_node->u.ops, 1)) == NULL)
        continue;
      switch_attr->ranges_p = TRUE;
      case_expr = NL_HEAD (c->case_node->u.ops);
      case_e = case_expr->attr;
      case_e2 = case_expr2->attr;
      skip_range_p = FALSE;
      for (case_t c2 = DLIST_HEAD (case_t, switch_attr->case_labels); c2 != NULL;
           c2 = DLIST_NEXT (case_t, c2)) {
        if (c2->case_node->code == N_DEFAULT) continue;
        if (c2 == c) {
          skip_range_p = TRUE;
          continue;
        }
        another_case_expr = NL_HEAD (c2->case_node->u.ops);
        another_case_expr2 = NL_EL (c2->case_node->u.ops, 1);
        if (skip_range_p && another_case_expr2 != NULL) continue;
        another_case_e = another_case_expr->attr;
        assert (another_case_e->const_p && integer_type_p (another_case_e->type));
        if (another_case_expr2 == NULL) {
          if ((signed_p && case_e->c.i_val <= another_case_e->c.i_val
               && another_case_e->c.i_val <= case_e2->c.i_val)
              || (!signed_p && case_e->c.u_val <= another_case_e->c.u_val
                  && another_case_e->c.u_val <= case_e2->c.u_val)) {
            error (c2m_ctx, POS (c->case_node), "duplicate value in a range case");
            break;
          }
        } else {
          another_case_e2 = another_case_expr2->attr;
          assert (another_case_e2->const_p && integer_type_p (another_case_e2->type));
          if ((signed_p
               && ((case_e->c.i_val <= another_case_e->c.i_val
                    && another_case_e->c.i_val <= case_e2->c.i_val)
                   || (case_e->c.i_val <= another_case_e2->c.i_val
                       && another_case_e2->c.i_val <= case_e2->c.i_val)))
              || (!signed_p
                  && ((case_e->c.u_val <= another_case_e->c.u_val
                       && another_case_e->c.u_val <= case_e2->c.u_val)
                      || (case_e->c.u_val <= another_case_e2->c.u_val
                          && another_case_e2->c.u_val <= case_e2->c.u_val)))) {
            error (c2m_ctx, POS (c->case_node), "duplicate value in a range case");
            break;
          }
        }
      }
    }
    curr_switch = saved_switch;
    curr_loop_switch = saved_loop_switch;
    break;
  }
  case N_DO:
  case N_WHILE: {
    node_t labels = NL_HEAD (r->u.ops);
    node_t expr = NL_NEXT (labels);
    node_t stmt = NL_NEXT (expr);
    node_t saved_loop = curr_loop;
    node_t saved_loop_switch = curr_loop_switch;

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    e1 = expr->attr;
    t1 = e1->type;
    if (!scalar_type_p (t1)) {
      error (c2m_ctx, POS (expr), "while-expr should be of a scalar type");
    }
    curr_loop = curr_loop_switch = r;
    check (c2m_ctx, stmt, r);
    curr_loop_switch = saved_loop_switch;
    curr_loop = saved_loop;
    break;
  }
  case N_FOR: {
    node_t labels = NL_HEAD (r->u.ops);
    node_t init = NL_NEXT (labels);
    node_t cond = NL_NEXT (init);
    node_t iter = NL_NEXT (cond);
    node_t stmt = NL_NEXT (iter);
    decl_t decl;
    node_t saved_loop = curr_loop;
    node_t saved_loop_switch = curr_loop_switch;

    check_labels (c2m_ctx, labels, r);
    create_node_scope (c2m_ctx, r);
    curr_loop = curr_loop_switch = r;
    check (c2m_ctx, init, r);
    if (init->code == N_LIST) {
      for (node_t spec_decl = NL_HEAD (init->u.ops); spec_decl != NULL;
           spec_decl = NL_NEXT (spec_decl)) {
        assert (spec_decl->code == N_SPEC_DECL);
        decl = spec_decl->attr;
        if (decl->decl_spec.typedef_p || decl->decl_spec.extern_p || decl->decl_spec.static_p
            || decl->decl_spec.thread_local_p) {
          error (c2m_ctx, POS (spec_decl),
                 "wrong storage specifier of for-loop initial declaration");
          break;
        }
      }
    }
    if (cond->code != N_IGNORE) { /* non-empty condition: */
      check (c2m_ctx, cond, r);
      e1 = cond->attr;
      t1 = e1->type;
      if (!scalar_type_p (t1)) {
        error (c2m_ctx, POS (cond), "for-condition should be of a scalar type");
      }
    }
    check (c2m_ctx, iter, r);
    check (c2m_ctx, stmt, r);
    finish_scope (c2m_ctx);
    curr_loop_switch = saved_loop_switch;
    curr_loop = saved_loop;
    break;
  }
  case N_GOTO: {
    node_t labels = NL_HEAD (r->u.ops);

    check_labels (c2m_ctx, labels, r);
    VARR_PUSH (node_t, label_uses, r);
    break;
  }
  case N_INDIRECT_GOTO: {
    node_t labels = NL_HEAD (r->u.ops);
    node_t expr = NL_NEXT (labels);

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    e1 = expr->attr;
    if (e1->type->mode != TM_PTR) error (c2m_ctx, POS (r), "computed goto must be pointer type");
    break;
  }
  case N_CONTINUE:
  case N_BREAK: {
    node_t labels = NL_HEAD (r->u.ops);

    if (r->code == N_BREAK && curr_loop_switch == NULL) {
      error (c2m_ctx, POS (r), "break statement not within loop or switch");
    } else if (r->code == N_CONTINUE && curr_loop == NULL) {
      error (c2m_ctx, POS (r), "continue statement not within a loop");
    }
    check_labels (c2m_ctx, labels, r);
    break;
  }
  case N_RETURN: {
    node_t labels = NL_HEAD (r->u.ops);
    node_t expr = NL_NEXT (labels);
    decl_t decl = curr_func_def->attr;
    struct type *ret_type, *type = decl->decl_spec.type;

    assert (type->mode == TM_FUNC);
    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    ret_type = type->u.func_type->ret_type;
    if (expr->code != N_IGNORE && void_type_p (ret_type)) {
      error (c2m_ctx, POS (r), "return with a value in function returning void");
    } else if (expr->code == N_IGNORE
               && (ret_type->mode != TM_BASIC || ret_type->u.basic_type != TP_VOID)) {
      error (c2m_ctx, POS (r), "return with no value in function returning non-void");
    } else if (expr->code != N_IGNORE) {
      check_assignment_types (c2m_ctx, ret_type, NULL, expr->attr, r);
    }
    break;
  }
  case N_EXPR: {
    node_t labels = NL_HEAD (r->u.ops);
    node_t expr = NL_NEXT (labels);

    check_labels (c2m_ctx, labels, r);
    check (c2m_ctx, expr, r);
    break;
  }
  default: abort ();
  }
  if (e != NULL) {
    node_t base;
    mir_llong offset;
    int deref;

    assert (!stmt_p);
    if (context && context->code != N_ALIGNOF && context->code != N_SIZEOF
        && context->code != N_EXPR_SIZEOF)
      e->type = adjust_type (c2m_ctx, e->type);
    set_type_layout (c2m_ctx, e->type);
    if (!e->const_p && check_const_addr_p (c2m_ctx, r, &base, &offset, &deref) && deref == 0) {
      if (base == NULL) {
        e->const_p = TRUE;
        e->c.i_val = offset;
      } else if (base->code == N_LABEL_ADDR) {
        e->const_addr_p = TRUE;
        e->c.i_val = offset;
      }
    }
    if (e->const_p) convert_value (e, e->type);
  } else if (stmt_p) {
    curr_call_arg_area_offset = 0;
  } else if (expr_attr_p) { /* it is an error -- define any expr and type: */
    assert (!stmt_p);
    e = create_expr (c2m_ctx, r);
    e->type->mode = TM_BASIC;
    e->type->u.basic_type = TP_INT;
  }
  VARR_POP (node_t, context_stack);
}

static void do_context (c2m_ctx_t c2m_ctx, node_t r) {
  check_ctx_t check_ctx = c2m_ctx->check_ctx;

  VARR_TRUNC (node_t, call_nodes, 0);
  VARR_TRUNC (node_t, possible_incomplete_decls, 0);
  check (c2m_ctx, r, NULL);
  for (size_t i = 0; i < VARR_LENGTH (node_t, possible_incomplete_decls); i++) {
    node_t spec_decl = VARR_GET (node_t, possible_incomplete_decls, i);
    decl_t decl = spec_decl->attr;
    if (incomplete_type_p (c2m_ctx, decl->decl_spec.type))
      error (c2m_ctx, POS (spec_decl), "incomplete struct or union");
  }
}

static void context_init (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  check_ctx_t check_ctx;

  c2m_ctx->check_ctx = check_ctx = c2mir_calloc (c2m_ctx, sizeof (struct check_ctx));
  n_i1_node = new_i_node (c2m_ctx, 1, no_pos);
  VARR_CREATE (node_t, context_stack, alloc, 64);
  check (c2m_ctx, n_i1_node, NULL);
  func_block_scope = curr_scope = NULL;
  VARR_CREATE (node_t, label_uses, alloc, 0);
  symbol_init (c2m_ctx);
  in_params_p = FALSE;
  curr_unnamed_anon_struct_union_member = NULL;
  HTAB_CREATE (case_t, case_tab, alloc, 100, case_hash, case_eq, NULL);
  VARR_CREATE (decl_t, func_decls_for_allocation, alloc, 1024);
  VARR_CREATE (node_t, possible_incomplete_decls, alloc, 512);
}

static void context_finish (c2m_ctx_t c2m_ctx) {
  check_ctx_t check_ctx;

  if (c2m_ctx == NULL || (check_ctx = c2m_ctx->check_ctx) == NULL) return;
  if (context_stack != NULL) VARR_DESTROY (node_t, context_stack);
  if (label_uses != NULL) VARR_DESTROY (node_t, label_uses);
  symbol_finish (c2m_ctx);
  if (case_tab != NULL) HTAB_DESTROY (case_t, case_tab);
  if (func_decls_for_allocation != NULL) VARR_DESTROY (decl_t, func_decls_for_allocation);
  if (possible_incomplete_decls != NULL) VARR_DESTROY (node_t, possible_incomplete_decls);
  free (c2m_ctx->check_ctx);
}

/* ------------------------ Context Checker Finish ---------------------------- */

/* New Page */

/* -------------------------- MIR generator start ----------------------------- */

static const char *FP_NAME = "fp";
static const char *RET_ADDR_NAME = "Ret_Addr";

/* New attribute for non-empty label LIST is a MIR label.  */

/* MIR var naming:
   {I|U|i|u|f|d}_<integer> -- temporary of I64, U64, I32, U32, F, D type
   {I|U|i|u|f|d}<scope_number>_<name> -- variable with of original <name> of the corresponding
   type in scope with <scope_number>
*/

#if MIR_PTR64
static const MIR_type_t MIR_POINTER_TYPE = MIR_T_I64;
#else
static const MIR_type_t MIR_POINTER_TYPE = MIR_T_I32;
#endif

struct op {
  decl_t decl;
  MIR_op_t mir_op;
};

typedef struct op op_t;

struct reg_var {
  const char *name;
  MIR_reg_t reg;
};

typedef struct reg_var reg_var_t;

DEF_HTAB (reg_var_t);
DEF_VARR (int);
DEF_VARR (MIR_type_t);

struct init_el {
  c2m_ctx_t c2m_ctx; /* for sorting */
  mir_size_t num, offset;
  decl_t member_decl; /* NULL for non-member initialization  */
  struct type *el_type, *container_type;
  node_t init;
};

typedef struct init_el init_el_t;
DEF_VARR (init_el_t);

DEF_VARR (MIR_op_t);
DEF_VARR (case_t);
DEF_HTAB (MIR_item_t);

struct gen_ctx {
  op_t zero_op, one_op, minus_one_op;
  MIR_item_t curr_func;
  DLIST (MIR_insn_t) slow_code_part;
  HTAB (reg_var_t) * reg_var_tab;
  int reg_free_mark;
  MIR_label_t continue_label, break_label;
  op_t top_gen_last_op;
  struct {
    int res_ref_p; /* flag of returning an aggregate by reference */
    VARR (MIR_type_t) * ret_types;
    VARR (MIR_var_t) * arg_vars;
  } proto_info;
  VARR (init_el_t) * init_els;
  MIR_item_t memset_proto, memset_item;
  MIR_item_t memcpy_proto, memcpy_item;
  VARR (MIR_op_t) * call_ops, *ret_ops, *switch_ops;
  VARR (case_t) * switch_cases;
  int curr_mir_proto_num;
  HTAB (MIR_item_t) * proto_tab;
  VARR (node_t) * node_stack;
};

#define zero_op gen_ctx->zero_op
#define one_op gen_ctx->one_op
#define minus_one_op gen_ctx->minus_one_op
#define curr_func gen_ctx->curr_func
#define slow_code_part gen_ctx->slow_code_part
#define reg_var_tab gen_ctx->reg_var_tab
#define reg_free_mark gen_ctx->reg_free_mark
#define continue_label gen_ctx->continue_label
#define break_label gen_ctx->break_label
#define top_gen_last_op gen_ctx->top_gen_last_op
#define proto_info gen_ctx->proto_info
#define init_els gen_ctx->init_els
#define memset_proto gen_ctx->memset_proto
#define memset_item gen_ctx->memset_item
#define memcpy_proto gen_ctx->memcpy_proto
#define memcpy_item gen_ctx->memcpy_item
#define call_ops gen_ctx->call_ops
#define ret_ops gen_ctx->ret_ops
#define switch_ops gen_ctx->switch_ops
#define switch_cases gen_ctx->switch_cases
#define curr_mir_proto_num gen_ctx->curr_mir_proto_num
#define proto_tab gen_ctx->proto_tab
#define node_stack gen_ctx->node_stack

static op_t new_op (decl_t decl, MIR_op_t mir_op) {
  op_t res;

  res.decl = decl;
  res.mir_op = mir_op;
  return res;
}

static htab_hash_t reg_var_hash (reg_var_t r, void *arg MIR_UNUSED) {
  return (htab_hash_t) mir_hash (r.name, strlen (r.name), 0x42);
}
static int reg_var_eq (reg_var_t r1, reg_var_t r2, void *arg MIR_UNUSED) {
  return strcmp (r1.name, r2.name) == 0;
}

static void init_reg_vars (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  reg_free_mark = 0;
  HTAB_CREATE (reg_var_t, reg_var_tab, alloc, 128, reg_var_hash, reg_var_eq, NULL);
}

static void finish_curr_func_reg_vars (c2m_ctx_t c2m_ctx) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  reg_free_mark = 0;
  HTAB_CLEAR (reg_var_t, reg_var_tab);
}

static void finish_reg_vars (c2m_ctx_t c2m_ctx) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  if (reg_var_tab != NULL) HTAB_DESTROY (reg_var_t, reg_var_tab);
}

static reg_var_t get_reg_var (c2m_ctx_t c2m_ctx, MIR_type_t t, const char *reg_name,
                              const char *asm_str) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  reg_var_t reg_var, el;
  char *str;
  MIR_reg_t reg;

  reg_var.name = reg_name;
  if (HTAB_DO (reg_var_t, reg_var_tab, reg_var, HTAB_FIND, el)) return el;
  t = t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_U64 ? MIR_T_I64 : t;
  if (asm_str == NULL) {
    reg = (t != MIR_T_UNDEF ? MIR_new_func_reg (ctx, curr_func->u.func, t, reg_name)
                            : MIR_reg (ctx, reg_name, curr_func->u.func));
  } else {
    reg = MIR_new_global_func_reg (ctx, curr_func->u.func, t, reg_name, asm_str);
  }
  str = reg_malloc (c2m_ctx, (strlen (reg_name) + 1) * sizeof (char));
  strcpy (str, reg_name);
  reg_var.name = str;
  reg_var.reg = reg;
  HTAB_DO (reg_var_t, reg_var_tab, reg_var, HTAB_INSERT, el);
  return reg_var;
}

static int temp_reg_p (c2m_ctx_t c2m_ctx, MIR_op_t op) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;

  return op.mode == MIR_OP_REG && MIR_reg_name (ctx, op.u.reg, curr_func->u.func)[1] == '_';
}

static MIR_type_t reg_type (c2m_ctx_t c2m_ctx, MIR_reg_t reg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  const char *n = MIR_reg_name (ctx, reg, curr_func->u.func);
  MIR_type_t res;

  if (strcmp (n, FP_NAME) == 0 || strcmp (n, RET_ADDR_NAME) == 0) return MIR_POINTER_TYPE;
  res = (n[0] == 'I'   ? MIR_T_I64
         : n[0] == 'U' ? MIR_T_U64
         : n[0] == 'i' ? MIR_T_I32
         : n[0] == 'u' ? MIR_T_U32
         : n[0] == 'f' ? MIR_T_F
         : n[0] == 'd' ? MIR_T_D
         : n[0] == 'D' ? MIR_T_LD
                       : MIR_T_BOUND);
  assert (res != MIR_T_BOUND);
  return res;
}

static op_t get_new_temp (c2m_ctx_t c2m_ctx, MIR_type_t t) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  char reg_name[50];
  MIR_reg_t reg;

  assert (t == MIR_T_I64 || t == MIR_T_U64 || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_F
          || t == MIR_T_D || t == MIR_T_LD);
  sprintf (reg_name,
           t == MIR_T_I64   ? "I_%u"
           : t == MIR_T_U64 ? "U_%u"
           : t == MIR_T_I32 ? "i_%u"
           : t == MIR_T_U32 ? "u_%u"
           : t == MIR_T_F   ? "f_%u"
           : t == MIR_T_D   ? "d_%u"
                            : "D_%u",
           reg_free_mark++);
  reg = get_reg_var (c2m_ctx, t, reg_name, NULL).reg;
  return new_op (NULL, MIR_new_reg_op (ctx, reg));
}

static MIR_type_t get_int_mir_type (size_t size) {
  return size == 1 ? MIR_T_I8 : size == 2 ? MIR_T_I16 : size == 4 ? MIR_T_I32 : MIR_T_I64;
}

static int MIR_UNUSED get_int_mir_type_size (MIR_type_t t) {
  return (t == MIR_T_I8 || t == MIR_T_U8     ? 1
          : t == MIR_T_I16 || t == MIR_T_U16 ? 2
          : t == MIR_T_I32 || t == MIR_T_U32 ? 4
                                             : 8);
}

static MIR_type_t get_mir_type (c2m_ctx_t c2m_ctx, struct type *type) {
  size_t size = raw_type_size (c2m_ctx, type);
  int int_p = !floating_type_p (type), signed_p = signed_integer_type_p (type);

  if (!scalar_type_p (type)) return MIR_T_UNDEF;
  assert (type->mode == TM_BASIC || type->mode == TM_PTR || type->mode == TM_ENUM);
  if (!int_p) {
    assert (size == 4 || size == 8 || size == sizeof (mir_ldouble));
    return size == 4 ? MIR_T_F : size == 8 ? MIR_T_D : MIR_T_LD;
  }
  assert (size <= 2 || size == 4 || size == 8);
  if (signed_p) return get_int_mir_type (size);
  return size == 1 ? MIR_T_U8 : size == 2 ? MIR_T_U16 : size == 4 ? MIR_T_U32 : MIR_T_U64;
}

static MIR_type_t promote_mir_int_type (MIR_type_t t) {
  return (t == MIR_T_I8 || t == MIR_T_I16   ? MIR_T_I32
          : t == MIR_T_U8 || t == MIR_T_U16 ? MIR_T_U32
                                            : t);
}

static MIR_type_t get_op_type (c2m_ctx_t c2m_ctx, op_t op) {
  switch (op.mir_op.mode) {
  case MIR_OP_MEM: return op.mir_op.u.mem.type;
  case MIR_OP_REG: return reg_type (c2m_ctx, op.mir_op.u.reg);
  case MIR_OP_INT: return MIR_T_I64;
  case MIR_OP_UINT: return MIR_T_U64;
  case MIR_OP_FLOAT: return MIR_T_F;
  case MIR_OP_DOUBLE: return MIR_T_D;
  case MIR_OP_LDOUBLE: return MIR_T_LD;
  default: assert (FALSE); return MIR_T_I64;
  }
}

static void push_val (VARR (char) * repr, mir_long val) {
  mir_long bound;

  for (bound = 10; val >= bound;) bound *= 10;
  while (bound != 1) {
    bound /= 10;
    VARR_PUSH (char, repr, '0' + val / bound);
    val %= bound;
  }
}

static void get_type_alias_name (c2m_ctx_t c2m_ctx, struct type *type, VARR (char) * name) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  enum basic_type basic_type;
  size_t i;

  switch (type->mode) {
  case TM_ENUM: basic_type = get_enum_basic_type (type); goto basic;
  case TM_BASIC:
    basic_type = type->u.basic_type;
  basic:
    switch (basic_type) {
    case TP_VOID: VARR_PUSH (char, name, 'v'); break;
    case TP_BOOL: VARR_PUSH (char, name, 'b'); break;
    case TP_CHAR:
    case TP_SCHAR:
    case TP_UCHAR: VARR_PUSH (char, name, 'c'); break;
    case TP_SHORT:
    case TP_USHORT: VARR_PUSH (char, name, 's'); break;
    case TP_INT:
    case TP_UINT: VARR_PUSH (char, name, 'i'); break;
    case TP_LONG:
    case TP_ULONG: VARR_PUSH (char, name, 'l'); break;
    case TP_LLONG:
    case TP_ULLONG: VARR_PUSH (char, name, 'L'); break;
    case TP_FLOAT: VARR_PUSH (char, name, 'f'); break;
    case TP_DOUBLE: VARR_PUSH (char, name, 'd'); break;
    case TP_LDOUBLE: VARR_PUSH (char, name, 'D'); break;
    default: assert (FALSE);
    }
    break;
  case TM_PTR:
    VARR_PUSH (char, name, 'p');
    get_type_alias_name (c2m_ctx, type->u.ptr_type, name);
    break;
  case TM_STRUCT:
  case TM_UNION:
    VARR_PUSH (char, name, type->mode == TM_STRUCT ? 'S' : 'U');
    for (i = 0; i < VARR_LENGTH (node_t, node_stack); i++)
      if (VARR_GET (node_t, node_stack, i) == type->u.tag_type) break;
    if (i < VARR_LENGTH (node_t, node_stack)) {
      VARR_PUSH (char, name, 'r');
      push_val (name, (mir_long) i);
    } else {
      VARR_PUSH (node_t, node_stack, type->u.tag_type);
      for (node_t member = NL_HEAD (NL_EL (type->u.tag_type->u.ops, 1)->u.ops); member != NULL;
           member = NL_NEXT (member))
        if (member->code == N_MEMBER) {
          decl_t decl = member->attr;
          node_t width = NL_EL (member->u.ops, 3);
          struct expr *expr;

          get_type_alias_name (c2m_ctx, decl->decl_spec.type, name);
          if (width->code != N_IGNORE && (expr = width->attr)->const_p) {
            VARR_PUSH (char, name, 'w');
            push_val (name, (mir_long) expr->c.u_val);
            for (mir_ullong v = expr->c.u_val;;) {
              VARR_PUSH (char, name, v % 10 + '0');
              v /= 10;
              if (v == 0) break;
            }
          }
        }
    }
    VARR_PUSH (char, name, 'e');
    break;
  case TM_ARR:
    VARR_PUSH (char, name, 'A');
    get_type_alias_name (c2m_ctx, type->u.arr_type->el_type, name);
    break;
  case TM_FUNC:
    VARR_PUSH (char, name, 'F');
    get_type_alias_name (c2m_ctx, type->u.func_type->ret_type, name);
    for (node_t p = NL_HEAD (type->u.func_type->param_list->u.ops); p != NULL; p = NL_NEXT (p)) {
      struct decl_spec *ds = get_param_decl_spec (p);
      get_type_alias_name (c2m_ctx, ds->type, name);
    }
    VARR_PUSH (char, name, type->u.func_type->dots_p ? 'E' : 'e');
    break;
  default: assert (FALSE);
  }
}

static MIR_alias_t get_type_alias (c2m_ctx_t c2m_ctx, struct type *type) {
  MIR_context_t ctx = c2m_ctx->ctx;
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  switch (type->mode) {
  case TM_BASIC:
    if (type->u.basic_type != TP_CHAR && type->u.basic_type != TP_SCHAR
        && type->u.basic_type != TP_UCHAR)
      break;
    /* fall through */
  case TM_UNDEF:
  case TM_STRUCT:
  case TM_ARR:
  case TM_FUNC: return 0;
  default: break;
  }
  VARR_TRUNC (node_t, node_stack, 0);
  VARR_TRUNC (char, temp_string, 0);
  get_type_alias_name (c2m_ctx, type, temp_string);
  VARR_PUSH (char, temp_string, '\0');
  return MIR_alias (ctx, VARR_ADDR (char, temp_string));
}

static op_t gen (c2m_ctx_t c2m_ctx, node_t r, MIR_label_t true_label, MIR_label_t false_label,
                 int val_p, op_t *desirable_dest, int *expect_res);

static op_t val_gen (c2m_ctx_t c2m_ctx, node_t r) {
  return gen (c2m_ctx, r, NULL, NULL, TRUE, NULL, NULL);
}

static int push_const_val (c2m_ctx_t c2m_ctx, node_t r, op_t *res) {
  MIR_context_t ctx = c2m_ctx->ctx;
  struct expr *e = (struct expr *) r->attr;
  MIR_type_t mir_type;

  if (!e->const_p) return FALSE;
  if (floating_type_p (e->type)) {
    /* MIR support only IEEE float and double */
    mir_type = get_mir_type (c2m_ctx, e->type);
    *res = new_op (NULL, (mir_type == MIR_T_F   ? MIR_new_float_op (ctx, (float) e->c.d_val)
                          : mir_type == MIR_T_D ? MIR_new_double_op (ctx, e->c.d_val)
                                                : MIR_new_ldouble_op (ctx, e->c.d_val)));
  } else {
    assert (integer_type_p (e->type) || e->type->mode == TM_PTR);
    *res = new_op (NULL, (signed_integer_type_p (e->type) ? MIR_new_int_op (ctx, e->c.i_val)
                                                          : MIR_new_uint_op (ctx, e->c.u_val)));
  }
  return TRUE;
}

static MIR_insn_code_t tp_mov (MIR_type_t t) {
  return t == MIR_T_F ? MIR_FMOV : t == MIR_T_D ? MIR_DMOV : t == MIR_T_LD ? MIR_LDMOV : MIR_MOV;
}

static void emit_insn (c2m_ctx_t c2m_ctx, MIR_insn_t insn) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  MIR_append_insn (c2m_ctx->ctx, curr_func, insn);
}

/* BCOND T, L1; JMP L2; L1: => BNCOND T, L2; L1:
   JMP L; L: => L: */
static void emit_label_insn_opt (c2m_ctx_t c2m_ctx, MIR_insn_t insn) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_insn_code_t rev_code;
  MIR_insn_t last, prev;

  assert (insn->code == MIR_LABEL);
  if ((last = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns)) != NULL
      && (prev = DLIST_PREV (MIR_insn_t, last)) != NULL && last->code == MIR_JMP
      && (rev_code = MIR_reverse_branch_code (prev->code)) != MIR_INSN_BOUND
      && prev->ops[0].mode == MIR_OP_LABEL && prev->ops[0].u.label == insn) {
    prev->ops[0] = last->ops[0];
    prev->code = rev_code;
    MIR_remove_insn (ctx, curr_func, last);
  }
  if ((last = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns)) != NULL && last->code == MIR_JMP
      && last->ops[0].mode == MIR_OP_LABEL && last->ops[0].u.label == insn) {
    MIR_remove_insn (ctx, curr_func, last);
  }
  MIR_append_insn (ctx, curr_func, insn);
}

/* Change t1 = expr; v = t1 to v = expr */
static void emit_insn_opt (c2m_ctx_t c2m_ctx, MIR_insn_t insn) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_insn_t tail;
  int out_p;

  if ((insn->code == MIR_MOV || insn->code == MIR_FMOV || insn->code == MIR_DMOV
       || insn->code == MIR_LDMOV)
      && (tail = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns)) != NULL
      && MIR_insn_nops (ctx, tail) > 0 && temp_reg_p (c2m_ctx, insn->ops[1])
      && !temp_reg_p (c2m_ctx, insn->ops[0]) && temp_reg_p (c2m_ctx, tail->ops[0])
      && insn->ops[1].u.reg == tail->ops[0].u.reg) {
    MIR_insn_op_mode (ctx, tail, 0, &out_p);
    if (out_p) {
      tail->ops[0] = insn->ops[0];
      MIR_append_insn (ctx, curr_func, insn);
      MIR_remove_insn (ctx, curr_func, insn);
      return;
    }
  }
  MIR_append_insn (ctx, curr_func, insn);
}

static void emit1 (c2m_ctx_t c2m_ctx, MIR_insn_code_t code, MIR_op_t op1) {
  emit_insn_opt (c2m_ctx, MIR_new_insn (c2m_ctx->ctx, code, op1));
}
static void emit2 (c2m_ctx_t c2m_ctx, MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2) {
  emit_insn_opt (c2m_ctx, MIR_new_insn (c2m_ctx->ctx, code, op1, op2));
}
static void emit3 (c2m_ctx_t c2m_ctx, MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2,
                   MIR_op_t op3) {
  emit_insn_opt (c2m_ctx, MIR_new_insn (c2m_ctx->ctx, code, op1, op2, op3));
}

static void emit2_noopt (c2m_ctx_t c2m_ctx, MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2) {
  emit_insn (c2m_ctx, MIR_new_insn (c2m_ctx->ctx, code, op1, op2));
}

static op_t cast (c2m_ctx_t c2m_ctx, op_t op, MIR_type_t t, int new_op_p) {
  op_t res, interm;
  MIR_type_t op_type;
  MIR_insn_code_t insn_code = MIR_INSN_BOUND, insn_code2 = MIR_INSN_BOUND;

  assert (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16 || t == MIR_T_I32
          || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64 || t == MIR_T_F || t == MIR_T_D
          || t == MIR_T_LD);
  switch (op.mir_op.mode) {
  case MIR_OP_MEM:
    op_type = op.mir_op.u.mem.type;
    if (op_type == MIR_T_UNDEF) { /* ??? it is an array */

    } else if (op_type == MIR_T_I8 || op_type == MIR_T_U8 || op_type == MIR_T_I16
               || op_type == MIR_T_U16 || op_type == MIR_T_I32 || op_type == MIR_T_U32)
      op_type = MIR_T_I64;
    goto all_types;
  case MIR_OP_REG:
    op_type = reg_type (c2m_ctx, op.mir_op.u.reg);
  all_types:
    if (op_type == MIR_T_F) goto float_val;
    if (op_type == MIR_T_D) goto double_val;
    if (op_type == MIR_T_LD) goto ldouble_val;
    if (t == MIR_T_I64) {
      insn_code = (op_type == MIR_T_I32   ? MIR_EXT32
                   : op_type == MIR_T_U32 ? MIR_UEXT32
                   : op_type == MIR_T_F   ? MIR_F2I
                   : op_type == MIR_T_D   ? MIR_D2I
                   : op_type == MIR_T_LD  ? MIR_LD2I
                                          : MIR_INSN_BOUND);
    } else if (t == MIR_T_U64) {
      insn_code = (op_type == MIR_T_I32   ? MIR_EXT32
                   : op_type == MIR_T_U32 ? MIR_UEXT32
                   : op_type == MIR_T_F   ? MIR_F2I
                   : op_type == MIR_T_D   ? MIR_D2I
                   : op_type == MIR_T_LD  ? MIR_LD2I
                                          : MIR_INSN_BOUND);
    } else if (t == MIR_T_I32) {
      insn_code = (op_type == MIR_T_F    ? MIR_F2I
                   : op_type == MIR_T_D  ? MIR_D2I
                   : op_type == MIR_T_LD ? MIR_LD2I
                                         : MIR_INSN_BOUND);
    } else if (t == MIR_T_U32) {
      insn_code = (op_type == MIR_T_F    ? MIR_F2I
                   : op_type == MIR_T_D  ? MIR_D2I
                   : op_type == MIR_T_LD ? MIR_LD2I
                                         : MIR_INSN_BOUND);
    } else if (t == MIR_T_I16) {
      insn_code = (op_type == MIR_T_F    ? MIR_F2I
                   : op_type == MIR_T_D  ? MIR_D2I
                   : op_type == MIR_T_LD ? MIR_LD2I
                                         : MIR_INSN_BOUND);
      insn_code2 = MIR_EXT16;
    } else if (t == MIR_T_U16) {
      insn_code = (op_type == MIR_T_F    ? MIR_F2I
                   : op_type == MIR_T_D  ? MIR_D2I
                   : op_type == MIR_T_LD ? MIR_LD2I
                                         : MIR_INSN_BOUND);
      insn_code2 = MIR_UEXT16;
    } else if (t == MIR_T_I8) {
      insn_code = (op_type == MIR_T_F    ? MIR_F2I
                   : op_type == MIR_T_D  ? MIR_D2I
                   : op_type == MIR_T_LD ? MIR_LD2I
                                         : MIR_INSN_BOUND);
      insn_code2 = MIR_EXT8;
    } else if (t == MIR_T_U8) {
      insn_code = (op_type == MIR_T_F    ? MIR_F2I
                   : op_type == MIR_T_D  ? MIR_D2I
                   : op_type == MIR_T_LD ? MIR_LD2I
                                         : MIR_INSN_BOUND);
      insn_code2 = MIR_UEXT8;
    } else if (t == MIR_T_F) {
      insn_code = (op_type == MIR_T_I32   ? MIR_EXT32
                   : op_type == MIR_T_U32 ? MIR_UEXT32
                                          : MIR_INSN_BOUND);
      insn_code2 = (op_type == MIR_T_I64 || op_type == MIR_T_I32   ? MIR_I2F
                    : op_type == MIR_T_U64 || op_type == MIR_T_U32 ? MIR_UI2F
                                                                   : MIR_INSN_BOUND);
    } else if (t == MIR_T_D) {
      insn_code = (op_type == MIR_T_I32   ? MIR_EXT32
                   : op_type == MIR_T_U32 ? MIR_UEXT32
                                          : MIR_INSN_BOUND);
      insn_code2 = (op_type == MIR_T_I64 || op_type == MIR_T_I32   ? MIR_I2D
                    : op_type == MIR_T_U64 || op_type == MIR_T_U32 ? MIR_UI2D
                                                                   : MIR_INSN_BOUND);
    } else if (t == MIR_T_LD) {
      insn_code = (op_type == MIR_T_I32   ? MIR_EXT32
                   : op_type == MIR_T_U32 ? MIR_UEXT32
                                          : MIR_INSN_BOUND);
      insn_code2 = (op_type == MIR_T_I64 || op_type == MIR_T_I32   ? MIR_I2LD
                    : op_type == MIR_T_U64 || op_type == MIR_T_U32 ? MIR_UI2LD
                                                                   : MIR_INSN_BOUND);
    }
    break;
  case MIR_OP_INT:
    insn_code = (t == MIR_T_I8    ? MIR_EXT8
                 : t == MIR_T_U8  ? MIR_UEXT8
                 : t == MIR_T_I16 ? MIR_EXT16
                 : t == MIR_T_U16 ? MIR_UEXT16
                 : t == MIR_T_F   ? MIR_I2F
                 : t == MIR_T_D   ? MIR_I2D
                 : t == MIR_T_LD  ? MIR_I2LD
                                  : MIR_INSN_BOUND);
    break;
  case MIR_OP_UINT:
    insn_code = (t == MIR_T_I8    ? MIR_EXT8
                 : t == MIR_T_U8  ? MIR_UEXT8
                 : t == MIR_T_I16 ? MIR_EXT16
                 : t == MIR_T_U16 ? MIR_UEXT16
                 : t == MIR_T_F   ? MIR_UI2F
                 : t == MIR_T_D   ? MIR_UI2D
                 : t == MIR_T_LD  ? MIR_UI2LD
                                  : MIR_INSN_BOUND);
    break;
  case MIR_OP_FLOAT:
  float_val:
    insn_code = (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                     || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64
                   ? MIR_F2I
                 : t == MIR_T_D  ? MIR_F2D
                 : t == MIR_T_LD ? MIR_F2LD
                                 : MIR_INSN_BOUND);
    insn_code2 = (t == MIR_T_I8    ? MIR_EXT8
                  : t == MIR_T_U8  ? MIR_UEXT8
                  : t == MIR_T_I16 ? MIR_EXT16
                  : t == MIR_T_U16 ? MIR_UEXT16
                                   : MIR_INSN_BOUND);
    break;
  case MIR_OP_DOUBLE:
  double_val:
    insn_code = (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                     || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64
                   ? MIR_D2I
                 : t == MIR_T_F  ? MIR_D2F
                 : t == MIR_T_LD ? MIR_D2LD
                                 : MIR_INSN_BOUND);
    insn_code2 = (t == MIR_T_I8    ? MIR_EXT8
                  : t == MIR_T_U8  ? MIR_UEXT8
                  : t == MIR_T_I16 ? MIR_EXT16
                  : t == MIR_T_U16 ? MIR_UEXT16
                                   : MIR_INSN_BOUND);
    break;
  case MIR_OP_LDOUBLE:
  ldouble_val:
    insn_code = (t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                     || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_I64 || t == MIR_T_U64
                   ? MIR_LD2I
                 : t == MIR_T_F ? MIR_LD2F
                 : t == MIR_T_D ? MIR_LD2D
                                : MIR_INSN_BOUND);
    insn_code2 = (t == MIR_T_I8    ? MIR_EXT8
                  : t == MIR_T_U8  ? MIR_UEXT8
                  : t == MIR_T_I16 ? MIR_EXT16
                  : t == MIR_T_U16 ? MIR_UEXT16
                                   : MIR_INSN_BOUND);
    break;
  default: break;
  }
  if (!new_op_p && insn_code == MIR_INSN_BOUND && insn_code2 == MIR_INSN_BOUND) return op;
  res = get_new_temp (c2m_ctx, t == MIR_T_I8 || t == MIR_T_U8 || t == MIR_T_I16 || t == MIR_T_U16
                                 ? MIR_T_I64
                                 : t);
  if (insn_code == MIR_INSN_BOUND && insn_code2 == MIR_INSN_BOUND) {
    emit2 (c2m_ctx, tp_mov (t), res.mir_op, op.mir_op);
  } else if (insn_code == MIR_INSN_BOUND) {
    emit2 (c2m_ctx, insn_code2, res.mir_op, op.mir_op);
  } else if (insn_code2 == MIR_INSN_BOUND) {
    emit2 (c2m_ctx, insn_code, res.mir_op, op.mir_op);
  } else {
    interm = get_new_temp (c2m_ctx, MIR_T_I64);
    emit2 (c2m_ctx, insn_code, interm.mir_op, op.mir_op);
    emit2 (c2m_ctx, insn_code2, res.mir_op, interm.mir_op);
  }
  return res;
}

static op_t promote (c2m_ctx_t c2m_ctx, op_t op, MIR_type_t t, int new_op_p) {
  assert (t == MIR_T_I64 || t == MIR_T_U64 || t == MIR_T_I32 || t == MIR_T_U32 || t == MIR_T_F
          || t == MIR_T_D || t == MIR_T_LD);
  return cast (c2m_ctx, op, t, new_op_p);
}

static op_t mem_to_address (c2m_ctx_t c2m_ctx, op_t mem, int reg_p) {
  MIR_context_t ctx = c2m_ctx->ctx;
  op_t temp;

  if (mem.mir_op.mode == MIR_OP_STR) {
    if (!reg_p) return mem;
    temp = get_new_temp (c2m_ctx, MIR_T_I64);
    emit2 (c2m_ctx, MIR_MOV, temp.mir_op, mem.mir_op);
    temp.mir_op.value_mode = MIR_OP_INT;
    return temp;
  }
  assert (mem.mir_op.mode == MIR_OP_MEM);
  if (mem.mir_op.u.mem.base == 0 && mem.mir_op.u.mem.index == 0) {
    if (!reg_p) {
      mem.mir_op.mode = MIR_OP_INT;
      mem.mir_op.u.i = mem.mir_op.u.mem.disp;
    } else {
      temp = get_new_temp (c2m_ctx, MIR_T_I64);
      emit2 (c2m_ctx, MIR_MOV, temp.mir_op, MIR_new_int_op (ctx, mem.mir_op.u.mem.disp));
      mem = temp;
    }
  } else if (mem.mir_op.u.mem.index == 0 && mem.mir_op.u.mem.disp == 0) {
    mem.mir_op.mode = MIR_OP_REG;
    mem.mir_op.u.reg = mem.mir_op.u.mem.base;
  } else if (mem.mir_op.u.mem.index == 0) {
    temp = get_new_temp (c2m_ctx, MIR_T_I64);
    emit3 (c2m_ctx, MIR_ADD, temp.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.base),
           MIR_new_int_op (ctx, mem.mir_op.u.mem.disp));
    mem = temp;
  } else {
    temp = get_new_temp (c2m_ctx, MIR_T_I64);
    if (mem.mir_op.u.mem.scale != 1)
      emit3 (c2m_ctx, MIR_MUL, temp.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index),
             MIR_new_int_op (ctx, mem.mir_op.u.mem.scale));
    else
      emit2 (c2m_ctx, MIR_MOV, temp.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index));
    if (mem.mir_op.u.mem.base != 0)
      emit3 (c2m_ctx, MIR_ADD, temp.mir_op, temp.mir_op,
             MIR_new_reg_op (ctx, mem.mir_op.u.mem.base));
    if (mem.mir_op.u.mem.disp != 0)
      emit3 (c2m_ctx, MIR_ADD, temp.mir_op, temp.mir_op,
             MIR_new_int_op (ctx, mem.mir_op.u.mem.disp));
    mem = temp;
  }
  mem.mir_op.value_mode = MIR_OP_INT;
  return mem;
}

static op_t force_val (c2m_ctx_t c2m_ctx, op_t op, int arr_p) {
  MIR_context_t ctx = c2m_ctx->ctx;
  op_t temp_op;
  int sh;

  if (arr_p && op.mir_op.mode == MIR_OP_MEM) {
    /* an array -- use a pointer: */
    return mem_to_address (c2m_ctx, op, FALSE);
  }
  if (op.decl == NULL || op.decl->bit_offset < 0) return op;
  assert (op.mir_op.mode == MIR_OP_MEM);
  temp_op = get_new_temp (c2m_ctx, MIR_T_I64);
  emit2 (c2m_ctx, MIR_MOV, temp_op.mir_op, op.mir_op); /* ??? */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  sh = 64 - op.decl->bit_offset - op.decl->width;
#else
  sh = op.decl->bit_offset + (64 - type_size (c2m_ctx, op.decl->decl_spec.type) * MIR_CHAR_BIT);
#endif
  if (sh != 0) emit3 (c2m_ctx, MIR_LSH, temp_op.mir_op, temp_op.mir_op, MIR_new_int_op (ctx, sh));
  emit3 (c2m_ctx,
         signed_integer_type_p (op.decl->decl_spec.type)
             && (op.decl->decl_spec.type->mode != TM_ENUM
                 || op.decl->width >= (int) sizeof (mir_int) * MIR_CHAR_BIT)
           ? MIR_RSH
           : MIR_URSH,
         temp_op.mir_op, temp_op.mir_op, MIR_new_int_op (ctx, 64 - op.decl->width));
  return temp_op;
}

static void gen_unary_op (c2m_ctx_t c2m_ctx, node_t r, op_t *op, op_t *res) {
  MIR_type_t t;

  assert (!((struct expr *) r->attr)->const_p);
  *op = val_gen (c2m_ctx, NL_HEAD (r->u.ops));
  t = get_mir_type (c2m_ctx, ((struct expr *) r->attr)->type);
  *op = promote (c2m_ctx, *op, t, FALSE);
  *res = get_new_temp (c2m_ctx, t);
}

static void gen_assign_bin_op (c2m_ctx_t c2m_ctx, node_t r, struct type *assign_expr_type,
                               op_t *op1, op_t *op2, op_t *var) {
  MIR_type_t t;
  node_t e = NL_HEAD (r->u.ops);

  assert (!((struct expr *) r->attr)->const_p);
  t = get_mir_type (c2m_ctx, assign_expr_type);
  *op1 = gen (c2m_ctx, e, NULL, NULL, FALSE, NULL, NULL);
  *op2 = val_gen (c2m_ctx, NL_NEXT (e));
  *op2 = promote (c2m_ctx, *op2, t, FALSE);
  *var = *op1;
  *op1 = force_val (c2m_ctx, *op1, ((struct expr *) e->attr)->type->arr_type != NULL);
  *op1 = promote (c2m_ctx, *op1, t, TRUE);
}

static void gen_bin_op (c2m_ctx_t c2m_ctx, node_t r, op_t *op1, op_t *op2, op_t *res) {
  struct expr *e = (struct expr *) r->attr;
  MIR_type_t t = get_mir_type (c2m_ctx, e->type);

  assert (!e->const_p);
  *op1 = val_gen (c2m_ctx, NL_HEAD (r->u.ops));
  *op2 = val_gen (c2m_ctx, NL_EL (r->u.ops, 1));
  *op1 = promote (c2m_ctx, *op1, t, FALSE);
  *op2 = promote (c2m_ctx, *op2, t, FALSE);
  *res = get_new_temp (c2m_ctx, t);
}

static void gen_cmp_op (c2m_ctx_t c2m_ctx, node_t r, struct type *type, op_t *op1, op_t *op2,
                        op_t *res) {
  MIR_type_t t = get_mir_type (c2m_ctx, type), res_t = get_int_mir_type (sizeof (mir_int));

  assert (!((struct expr *) r->attr)->const_p);
  *op1 = val_gen (c2m_ctx, NL_HEAD (r->u.ops));
  *op2 = val_gen (c2m_ctx, NL_EL (r->u.ops, 1));
  *op1 = promote (c2m_ctx, *op1, t, FALSE);
  *op2 = promote (c2m_ctx, *op2, t, FALSE);
  *res = get_new_temp (c2m_ctx, res_t);
}

static MIR_insn_code_t get_mir_type_insn_code (c2m_ctx_t c2m_ctx, struct type *type, node_t r) {
  MIR_type_t t = get_mir_type (c2m_ctx, type);

  switch (r->code) {
  case N_INC:
  case N_POST_INC:
  case N_ADD:
  case N_ADD_ASSIGN:
    return (t == MIR_T_F                       ? MIR_FADD
            : t == MIR_T_D                     ? MIR_DADD
            : t == MIR_T_LD                    ? MIR_LDADD
            : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_ADD
                                               : MIR_ADDS);
  case N_DEC:
  case N_POST_DEC:
  case N_SUB:
  case N_SUB_ASSIGN:
    return (t == MIR_T_F                       ? MIR_FSUB
            : t == MIR_T_D                     ? MIR_DSUB
            : t == MIR_T_LD                    ? MIR_LDSUB
            : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_SUB
                                               : MIR_SUBS);
  case N_MUL:
  case N_MUL_ASSIGN:
    return (t == MIR_T_F                       ? MIR_FMUL
            : t == MIR_T_D                     ? MIR_DMUL
            : t == MIR_T_LD                    ? MIR_LDMUL
            : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_MUL
                                               : MIR_MULS);
  case N_DIV:
  case N_DIV_ASSIGN:
    return (t == MIR_T_F     ? MIR_FDIV
            : t == MIR_T_D   ? MIR_DDIV
            : t == MIR_T_LD  ? MIR_LDDIV
            : t == MIR_T_I64 ? MIR_DIV
            : t == MIR_T_U64 ? MIR_UDIV
            : t == MIR_T_I32 ? MIR_DIVS
                             : MIR_UDIVS);
  case N_MOD:
  case N_MOD_ASSIGN:
    return (t == MIR_T_I64   ? MIR_MOD
            : t == MIR_T_U64 ? MIR_UMOD
            : t == MIR_T_I32 ? MIR_MODS
                             : MIR_UMODS);
  case N_AND:
  case N_AND_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_AND : MIR_ANDS);
  case N_OR:
  case N_OR_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_OR : MIR_ORS);
  case N_XOR:
  case N_XOR_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_XOR : MIR_XORS);
  case N_LSH:
  case N_LSH_ASSIGN: return (t == MIR_T_I64 || t == MIR_T_U64 ? MIR_LSH : MIR_LSHS);
  case N_RSH:
  case N_RSH_ASSIGN:
    return (t == MIR_T_I64   ? MIR_RSH
            : t == MIR_T_U64 ? MIR_URSH
            : t == MIR_T_I32 ? MIR_RSHS
                             : MIR_URSHS);
  case N_EQ:
    return (t == MIR_T_F                       ? MIR_FEQ
            : t == MIR_T_D                     ? MIR_DEQ
            : t == MIR_T_LD                    ? MIR_LDEQ
            : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_EQ
                                               : MIR_EQS);
  case N_NE:
    return (t == MIR_T_F                       ? MIR_FNE
            : t == MIR_T_D                     ? MIR_DNE
            : t == MIR_T_LD                    ? MIR_LDNE
            : t == MIR_T_I64 || t == MIR_T_U64 ? MIR_NE
                                               : MIR_NES);
  case N_LT:
    return (t == MIR_T_F     ? MIR_FLT
            : t == MIR_T_D   ? MIR_DLT
            : t == MIR_T_LD  ? MIR_LDLT
            : t == MIR_T_I64 ? MIR_LT
            : t == MIR_T_U64 ? MIR_ULT
            : t == MIR_T_I32 ? MIR_LTS
                             : MIR_ULTS);
  case N_LE:
    return (t == MIR_T_F     ? MIR_FLE
            : t == MIR_T_D   ? MIR_DLE
            : t == MIR_T_LD  ? MIR_LDLE
            : t == MIR_T_I64 ? MIR_LE
            : t == MIR_T_U64 ? MIR_ULE
            : t == MIR_T_I32 ? MIR_LES
                             : MIR_ULES);
  case N_GT:
    return (t == MIR_T_F     ? MIR_FGT
            : t == MIR_T_D   ? MIR_DGT
            : t == MIR_T_LD  ? MIR_LDGT
            : t == MIR_T_I64 ? MIR_GT
            : t == MIR_T_U64 ? MIR_UGT
            : t == MIR_T_I32 ? MIR_GTS
                             : MIR_UGTS);
  case N_GE:
    return (t == MIR_T_F     ? MIR_FGE
            : t == MIR_T_D   ? MIR_DGE
            : t == MIR_T_LD  ? MIR_LDGE
            : t == MIR_T_I64 ? MIR_GE
            : t == MIR_T_U64 ? MIR_UGE
            : t == MIR_T_I32 ? MIR_GES
                             : MIR_UGES);
  default: assert (FALSE); return MIR_INSN_BOUND;
  }
}

static MIR_insn_code_t get_mir_insn_code (c2m_ctx_t c2m_ctx,
                                          node_t r) { /* result type is the same as op types */
  return get_mir_type_insn_code (c2m_ctx, ((struct expr *) r->attr)->type, r);
}

static MIR_insn_code_t get_compare_branch_code (MIR_insn_code_t code) {
#define B(n)                           \
  case MIR_##n: return MIR_B##n;       \
  case MIR_##n##S: return MIR_B##n##S; \
  case MIR_F##n: return MIR_FB##n;     \
  case MIR_D##n: return MIR_DB##n;     \
  case MIR_LD##n: return MIR_LDB##n;
#define BCMP(n)                    \
  B (n)                            \
  case MIR_U##n: return MIR_UB##n; \
  case MIR_U##n##S: return MIR_UB##n##S;
  switch (code) {
    B (EQ) B (NE) BCMP (LT) BCMP (LE) BCMP (GT) BCMP (GE) default : assert (FALSE);
    return MIR_INSN_BOUND;
  }
#undef B
#undef BCMP
}

static op_t force_reg (c2m_ctx_t c2m_ctx, op_t op, MIR_type_t t) {
  op_t res;

  if (op.mir_op.mode == MIR_OP_REG) return op;
  res = get_new_temp (c2m_ctx, promote_mir_int_type (t));
  emit2 (c2m_ctx, MIR_MOV, res.mir_op, op.mir_op);
  return res;
}

static op_t force_reg_or_mem (c2m_ctx_t c2m_ctx, op_t op, MIR_type_t t) {
  if (op.mir_op.mode == MIR_OP_REG || op.mir_op.mode == MIR_OP_MEM) return op;
  assert (op.mir_op.mode == MIR_OP_REF || op.mir_op.mode == MIR_OP_STR);
  return force_reg (c2m_ctx, op, t);
}

static void emit_label (c2m_ctx_t c2m_ctx, node_t r) {
  node_t labels = NL_HEAD (r->u.ops);

  assert (labels->code == N_LIST);
  if (NL_HEAD (labels->u.ops) == NULL) return;
  if (labels->attr == NULL) labels->attr = MIR_new_label (c2m_ctx->ctx);
  emit_label_insn_opt (c2m_ctx, labels->attr);
}

static MIR_label_t get_label (c2m_ctx_t c2m_ctx, node_t target) {
  node_t labels = NL_HEAD (target->u.ops);

  assert (labels->code == N_LIST && NL_HEAD (labels->u.ops) != NULL);
  if (labels->attr != NULL) return labels->attr;
  return labels->attr = MIR_new_label (c2m_ctx->ctx);
}

static void top_gen (c2m_ctx_t c2m_ctx, node_t r, MIR_label_t true_label, MIR_label_t false_label,
                     int *expect_res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  top_gen_last_op = gen (c2m_ctx, r, true_label, false_label, FALSE, NULL, expect_res);
}

static op_t modify_for_block_move (c2m_ctx_t c2m_ctx, op_t mem, op_t index) {
  MIR_context_t ctx = c2m_ctx->ctx;
  op_t base;

  assert (mem.mir_op.u.mem.base != 0 && mem.mir_op.mode == MIR_OP_MEM
          && index.mir_op.mode == MIR_OP_REG);
  if (mem.mir_op.u.mem.index == 0) {
    mem.mir_op.u.mem.index = index.mir_op.u.reg;
    mem.mir_op.u.mem.scale = 1;
  } else {
    base = get_new_temp (c2m_ctx, MIR_T_I64);
    if (mem.mir_op.u.mem.scale != 1)
      emit3 (c2m_ctx, MIR_MUL, base.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index),
             MIR_new_int_op (ctx, mem.mir_op.u.mem.scale));
    else
      emit2 (c2m_ctx, MIR_MOV, base.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.index));
    emit3 (c2m_ctx, MIR_ADD, base.mir_op, base.mir_op, MIR_new_reg_op (ctx, mem.mir_op.u.mem.base));
    mem.mir_op.u.mem.base = base.mir_op.u.reg;
    mem.mir_op.u.mem.index = index.mir_op.u.reg;
    mem.mir_op.u.mem.scale = 1;
  }
  mem.mir_op.u.mem.alias = mem.mir_op.u.mem.nonalias = 0;
  return mem;
}

static void gen_memcpy (c2m_ctx_t c2m_ctx, MIR_disp_t disp, MIR_reg_t base, op_t val,
                        mir_size_t len);

static void block_move (c2m_ctx_t c2m_ctx, op_t var, op_t val, mir_size_t size) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_label_t repeat_label;
  op_t index;

  if (MIR_op_eq_p (ctx, var.mir_op, val.mir_op) || size == 0) return;
  if (size > 5) {
    var = mem_to_address (c2m_ctx, var, TRUE);
    assert (var.mir_op.mode == MIR_OP_REG);
    gen_memcpy (c2m_ctx, 0, var.mir_op.u.reg, val, size);
  } else {
    repeat_label = MIR_new_label (ctx);
    index = get_new_temp (c2m_ctx, MIR_T_I64);
    emit2 (c2m_ctx, MIR_MOV, index.mir_op, MIR_new_int_op (ctx, size));
    val = modify_for_block_move (c2m_ctx, val, index);
    var = modify_for_block_move (c2m_ctx, var, index);
    emit_label_insn_opt (c2m_ctx, repeat_label);
    emit3 (c2m_ctx, MIR_SUB, index.mir_op, index.mir_op, one_op.mir_op);
    assert (var.mir_op.mode == MIR_OP_MEM && val.mir_op.mode == MIR_OP_MEM);
    val.mir_op.u.mem.type = var.mir_op.u.mem.type = MIR_T_I8;
    emit2 (c2m_ctx, MIR_MOV, var.mir_op, val.mir_op);
    emit3 (c2m_ctx, MIR_BGT, MIR_new_label_op (ctx, repeat_label), index.mir_op, zero_op.mir_op);
  }
}

static const char *get_reg_var_name (c2m_ctx_t c2m_ctx, MIR_type_t promoted_type,
                                     const char *suffix, unsigned func_scope_num) {
  char prefix[50];

  sprintf (prefix,
           promoted_type == MIR_T_I64   ? "I%u_"
           : promoted_type == MIR_T_U64 ? "U%u_"
           : promoted_type == MIR_T_I32 ? "i%u_"
           : promoted_type == MIR_T_U32 ? "u%u_"
           : promoted_type == MIR_T_F   ? "f%u_"
           : promoted_type == MIR_T_D   ? "d%u_"
                                        : "D%u_",
           func_scope_num);
  VARR_TRUNC (char, temp_string, 0);
  add_to_temp_string (c2m_ctx, prefix);
  add_to_temp_string (c2m_ctx, suffix);
  return uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
}

static const char *get_func_var_name (c2m_ctx_t c2m_ctx, const char *prefix, const char *suffix) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  assert (curr_func != NULL);
  VARR_TRUNC (char, temp_string, 0);
  add_to_temp_string (c2m_ctx, prefix);
  add_to_temp_string (c2m_ctx, curr_func->u.func->name);
  add_to_temp_string (c2m_ctx, "_");
  add_to_temp_string (c2m_ctx, suffix);
  return uniq_cstr (c2m_ctx, VARR_ADDR (char, temp_string)).s;
}

static const char *get_func_static_var_name (c2m_ctx_t c2m_ctx, const char *suffix, decl_t decl) {
  char prefix[50];
  unsigned func_scope_num = ((struct node_scope *) decl->scope->attr)->func_scope_num;

  sprintf (prefix, "S%u_", func_scope_num);
  return get_func_var_name (c2m_ctx, prefix, suffix);
}

static const char *get_param_name (c2m_ctx_t c2m_ctx, struct type *param_type, const char *name) {
  MIR_type_t type = (param_type->mode == TM_STRUCT || param_type->mode == TM_UNION
                       ? MIR_POINTER_TYPE
                       : get_mir_type (c2m_ctx, param_type));
  return get_reg_var_name (c2m_ctx, promote_mir_int_type (type), name, 0);
}

static void MIR_UNUSED simple_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                             void *arg_info MIR_UNUSED) {}

static int simple_return_by_addr_p (c2m_ctx_t c2m_ctx MIR_UNUSED, struct type *ret_type) {
  return ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION;
}

static void MIR_UNUSED simple_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                             void *arg_info MIR_UNUSED,
                                             VARR (MIR_type_t) * res_types,
                                             VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;

  if (void_type_p (ret_type)) return;
  if (!simple_return_by_addr_p (c2m_ctx, ret_type)) {
    VARR_PUSH (MIR_type_t, res_types, get_mir_type (c2m_ctx, ret_type));
  } else {
    var.name = RET_ADDR_NAME;
    var.type = MIR_T_RBLK;
    var.size = type_size (c2m_ctx, ret_type);
    VARR_PUSH (MIR_var_t, arg_vars, var);
  }
}

static int MIR_UNUSED simple_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                              void *arg_info MIR_UNUSED,
                                              size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t type;
  op_t temp;

  if (void_type_p (ret_type)) return -1;
  if (!simple_return_by_addr_p (c2m_ctx, ret_type)) {
    type = promote_mir_int_type (get_mir_type (c2m_ctx, ret_type));
    temp = get_new_temp (c2m_ctx, type);
    VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
    return 1;
  }
  temp = get_new_temp (c2m_ctx, MIR_T_I64);
  emit3 (c2m_ctx, MIR_ADD, temp.mir_op,
         MIR_new_reg_op (ctx, MIR_reg (ctx, FP_NAME, curr_func->u.func)),
         MIR_new_int_op (ctx, call_arg_area_offset));
  temp.mir_op
    = MIR_new_mem_op (ctx, MIR_T_RBLK, type_size (c2m_ctx, ret_type), temp.mir_op.u.reg, 0, 1);
  VARR_PUSH (MIR_op_t, call_ops, temp.mir_op);
  return 0;
}

static op_t MIR_UNUSED simple_gen_post_call_res_code (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                                      struct type *ret_type MIR_UNUSED, op_t res,
                                                      MIR_insn_t call MIR_UNUSED,
                                                      size_t call_ops_start MIR_UNUSED) {
  return res;
}

static void MIR_UNUSED simple_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t val) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_reg_t ret_addr_reg;
  op_t var;

  if (void_type_p (ret_type)) return;
  if (!simple_return_by_addr_p (c2m_ctx, ret_type)) {
    VARR_PUSH (MIR_op_t, ret_ops, val.mir_op);
  } else {
    ret_addr_reg = MIR_reg (ctx, RET_ADDR_NAME, curr_func->u.func);
    var = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, ret_addr_reg, 0, 1));
    block_move (c2m_ctx, var, val, type_size (c2m_ctx, ret_type));
  }
}

static MIR_type_t MIR_UNUSED simple_target_get_blk_type (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                                         struct type *arg_type MIR_UNUSED) {
  return MIR_T_BLK;
}

static void MIR_UNUSED simple_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name,
                                             struct type *arg_type, void *arg_info MIR_UNUSED,
                                             VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;
  MIR_type_t type;

  type = (arg_type->mode == TM_STRUCT || arg_type->mode == TM_UNION
            ? MIR_T_BLK
            : get_mir_type (c2m_ctx, arg_type));
  var.name = name;
  var.type = type;
  if (type == MIR_T_BLK) var.size = type_size (c2m_ctx, arg_type);
  VARR_PUSH (MIR_var_t, arg_vars, var);
}

static void MIR_UNUSED simple_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                               void *arg_info MIR_UNUSED, op_t arg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_type_t type;

  type = (arg_type->mode == TM_STRUCT || arg_type->mode == TM_UNION
            ? MIR_T_BLK
            : get_mir_type (c2m_ctx, arg_type));
  if (type != MIR_T_BLK) {
    VARR_PUSH (MIR_op_t, call_ops, arg.mir_op);
  } else {
    assert (arg.mir_op.mode == MIR_OP_MEM);
    arg = mem_to_address (c2m_ctx, arg, TRUE);
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_mem_op (c2m_ctx->ctx, MIR_T_BLK, type_size (c2m_ctx, arg_type),
                               arg.mir_op.u.reg, 0, 1));
  }
}

static int MIR_UNUSED simple_gen_gather_arg (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                             const char *name MIR_UNUSED,
                                             struct type *arg_type MIR_UNUSED,
                                             decl_t param_decl MIR_UNUSED,
                                             void *arg_info MIR_UNUSED) {
  return FALSE;
}

/* Can be used by target functions */
static MIR_UNUSED const char *gen_get_indexed_name (c2m_ctx_t c2m_ctx, const char *name,
                                                    int index) {
  assert (index >= 0 && index <= 9);
  VARR_TRUNC (char, temp_string, 0);
  VARR_PUSH_ARR (char, temp_string, name, strlen (name));
  VARR_PUSH (char, temp_string, '#');
  VARR_PUSH (char, temp_string, '0' + index);
  VARR_PUSH (char, temp_string, '\0');
  return _MIR_uniq_string (c2m_ctx->ctx, VARR_ADDR (char, temp_string));
}

/* Can be used by target functions */
static inline void MIR_UNUSED gen_multiple_load_store (c2m_ctx_t c2m_ctx, struct type *type,
                                                       MIR_op_t *var_ops, MIR_op_t mem_op,
                                                       int load_p) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_op_t op, var_op;
  MIR_insn_t insn;
  int i, sh, size = (int) type_size (c2m_ctx, type);

  if (size == 0) return;
  if (type_align (type) == 8) {
    assert (size % 8 == 0);
    for (i = 0; size > 0; size -= 8, i++) {
      if (load_p) {
        insn = MIR_new_insn (ctx, MIR_MOV, var_ops[i],
                             MIR_new_mem_op (ctx, MIR_T_I64, mem_op.u.mem.disp + i * 8,
                                             mem_op.u.mem.base, mem_op.u.mem.index,
                                             mem_op.u.mem.scale));
      } else {
        insn = MIR_new_insn (ctx, MIR_MOV,
                             MIR_new_mem_op (ctx, MIR_T_I64, mem_op.u.mem.disp + i * 8,
                                             mem_op.u.mem.base, mem_op.u.mem.index,
                                             mem_op.u.mem.scale),
                             var_ops[i]);
      }
      MIR_append_insn (ctx, curr_func, insn);
    }
  } else {
    op = get_new_temp (c2m_ctx, MIR_T_I64).mir_op;
    if (load_p) {
      for (i = 0; i < size; i += 8) {
        var_op = var_ops[i / 8];
        insn = MIR_new_insn (ctx, MIR_MOV, var_op, MIR_new_int_op (ctx, 0));
        MIR_append_insn (ctx, curr_func, insn);
      }
    }
    for (i = 0; size > 0; size--, i++) {
      var_op = var_ops[i / 8];
      if (load_p) {
        insn
          = MIR_new_insn (ctx, MIR_MOV, op,
                          MIR_new_mem_op (ctx, MIR_T_U8, mem_op.u.mem.disp + i, mem_op.u.mem.base,
                                          mem_op.u.mem.index, mem_op.u.mem.scale));
        MIR_append_insn (ctx, curr_func, insn);
        if ((sh = i * 8 % 64) != 0) {
          insn = MIR_new_insn (ctx, MIR_LSH, op, op, MIR_new_int_op (ctx, sh));
          MIR_append_insn (ctx, curr_func, insn);
        }
        insn = MIR_new_insn (ctx, MIR_OR, var_op, var_op, op);
        MIR_append_insn (ctx, curr_func, insn);
      } else {
        if ((sh = i * 8 % 64) == 0)
          insn = MIR_new_insn (ctx, MIR_MOV, op, var_op);
        else
          insn = MIR_new_insn (ctx, MIR_URSH, op, var_op, MIR_new_int_op (ctx, sh));
        MIR_append_insn (ctx, curr_func, insn);
        insn
          = MIR_new_insn (ctx, MIR_MOV,
                          MIR_new_mem_op (ctx, MIR_T_U8, mem_op.u.mem.disp + i, mem_op.u.mem.base,
                                          mem_op.u.mem.index, mem_op.u.mem.scale),
                          op);
        MIR_append_insn (ctx, curr_func, insn);
      }
    }
  }
}

#if defined(__x86_64__) || defined(_M_AMD64)
#include "x86_64/cx86_64-ABI-code.c"
#elif defined(__aarch64__)
#include "aarch64/caarch64-ABI-code.c"
#elif defined(__PPC64__)
#include "ppc64/cppc64-ABI-code.c"
#elif defined(__s390x__)
#include "s390x/cs390x-ABI-code.c"
#elif defined(__riscv)
#include "riscv64/criscv64-ABI-code.c"
#else
typedef int target_arg_info_t; /* whatever */
/* Initiate ARG_INFO for generating call, prototype, or prologue. */
static void target_init_arg_vars (c2m_ctx_t c2m_ctx, target_arg_info_t *arg_info) {
  simple_init_arg_vars (c2m_ctx, arg_info);
}
/* Return true if result of RET_TYPE should be return by addr. */
static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  return simple_return_by_addr_p (c2m_ctx, ret_type);
}
/* Add prototype result types to RES_TYPES or arg vars to ARG_VARS
   used to return value of RET_TYPES. */
static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  simple_add_res_proto (c2m_ctx, ret_type, arg_info, res_types, arg_vars);
}
/* Generate code and result operands or an input operand to call_ops
   for returning call result of RET_TYPE.  Return -1 if no any call op
   was added, 0 if only input operand (result address) was added or
   number of added results. Use CALL_ARG_AREA_OFFSET for result
   address offset on the stack.  */
static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  return simple_add_call_res_op (c2m_ctx, ret_type, arg_info, call_arg_area_offset);
}
/* Generate code to gather returned values of CALL into RES.  Return
   value of RET_TYPE.  CALL_OPS_START is start index of all call
   operands in call_ops for given call. */
static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call, size_t call_ops_start) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  return simple_gen_post_call_res_code (c2m_ctx, ret_type, res, call, call_ops_start);
}
/* Generate code and add operands to ret_ops which return VAL of RET_TYPE. */
static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t val) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  simple_add_ret_ops (c2m_ctx, ret_type, val);
}
/* Return BLK type should be used for VA_BLOCK_ARG for accessing aggregate type ARG_TYPE.  */
static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx, struct type *arg_type) {
  return simple_target_get_blk_type (c2m_ctx, arg_type);
}
/* Add one or more vars to arg_vars which pass arg NAME of ARG_TYPE. */
static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info, VARR (MIR_var_t) * arg_vars) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  simple_add_arg_proto (c2m_ctx, name, arg_type, arg_info, arg_vars);
}
/* Add operands to call_ops which pass ARG of ARG_TYPE. */
static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info, op_t arg) {
  simple_add_call_arg_op (c2m_ctx, arg_type, arg_info, arg);
}
/* Add code to gather aggregate arg with NAME, ARG_TYPE and PARAM_DECL passed by non-block args.
   Return true if it was the case.  */
static int target_gen_gather_arg (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  decl_t param_decl, target_arg_info_t *arg_info) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  return simple_gen_gather_arg (c2m_ctx, name, arg_type, param_decl, arg_info);
}
#endif

static void collect_args_and_func_types (c2m_ctx_t c2m_ctx, struct func_type *func_type) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  node_t declarator, id, first_param, p;
  struct type *param_type;
  decl_t param_decl;
  const char *name;
  target_arg_info_t arg_info;

  first_param = NL_HEAD (func_type->param_list->u.ops);
  VARR_TRUNC (MIR_var_t, proto_info.arg_vars, 0);
  VARR_TRUNC (MIR_type_t, proto_info.ret_types, 0);
  proto_info.res_ref_p = FALSE;
  target_init_arg_vars (c2m_ctx, &arg_info);
  set_type_layout (c2m_ctx, func_type->ret_type);
  target_add_res_proto (c2m_ctx, func_type->ret_type, &arg_info, proto_info.ret_types,
                        proto_info.arg_vars);
  if (first_param != NULL && !void_param_p (first_param)) {
    for (p = first_param; p != NULL; p = NL_NEXT (p)) {
      if (p->code == N_TYPE) {
        name = "p";
        param_type = ((struct decl_spec *) p->attr)->type;
        param_decl = NULL;
      } else {
        declarator = NL_EL (p->u.ops, 1);
        assert (p->code == N_SPEC_DECL && declarator != NULL && declarator->code == N_DECL);
        id = NL_HEAD (declarator->u.ops);
        param_decl = p->attr;
        param_type = param_decl->decl_spec.type;
        name = get_param_name (c2m_ctx, param_type, id->u.s.s);
      }
      target_add_arg_proto (c2m_ctx, name, param_type, &arg_info, proto_info.arg_vars);
    }
  }
}

static mir_size_t get_object_path_offset (c2m_ctx_t c2m_ctx) {
  init_object_t init_object;
  size_t offset = 0;

  for (size_t i = 0; i < VARR_LENGTH (init_object_t, init_object_path); i++) {
    init_object = VARR_GET (init_object_t, init_object_path, i);
    if (init_object.container_type->mode == TM_ARR) {  // ??? index < 0
      offset += (init_object.u.curr_index
                 * type_size (c2m_ctx, init_object.container_type->u.arr_type->el_type));
    } else {
      assert (init_object.container_type->mode == TM_STRUCT
              || init_object.container_type->mode == TM_UNION);
      assert (init_object.u.curr_member->code == N_MEMBER);
      if (!anon_struct_union_type_member_p (init_object.u.curr_member))
        /* Members inside anon struct/union already have adjusted offset */
        offset += ((decl_t) init_object.u.curr_member->attr)->offset;
    }
  }
  return offset;
}

/* The function has the same structure as check_initializer.  Keep it this way. */
static void collect_init_els (c2m_ctx_t c2m_ctx, decl_t member_decl, struct type **type_ptr,
                              node_t initializer, int const_only_p, int top_p MIR_UNUSED) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  struct type *type = *type_ptr;
  struct expr *cexpr;
  node_t literal, des_list, curr_des, str, init, value;
  mir_llong MIR_UNUSED size_val = 0; /* to remove an uninitialized warning */
  size_t mark;
  symbol_t sym;
  init_el_t init_el;
  int addr_p = FALSE; /* to remove an uninitialized warning */
  int MIR_UNUSED found_p, MIR_UNUSED ok_p;
  init_object_t init_object;

  literal = get_compound_literal (initializer, &addr_p);
  if (literal != NULL && !addr_p && initializer->code != N_STR && initializer->code != N_STR16
      && initializer->code != N_STR32)
    initializer = NL_EL (literal->u.ops, 1);
check_one_value:
  if (initializer->code != N_LIST
      && !(initializer->code == N_STR && type->mode == TM_ARR
           && init_compatible_string_p (initializer, type->u.arr_type->el_type))) {
    cexpr = initializer->attr;
    /* static or thread local object initialization should be const expr or addr: */
    assert (initializer->code == N_STR || initializer->code == N_STR16
            || initializer->code == N_STR32 || !const_only_p || cexpr->const_p
            || cexpr->const_addr_p || (literal != NULL && addr_p));
    init_el.c2m_ctx = c2m_ctx;
    init_el.num = VARR_LENGTH (init_el_t, init_els);
    init_el.offset = get_object_path_offset (c2m_ctx);
    init_el.member_decl = member_decl;
    init_el.el_type = type;
    init_el.container_type = VARR_LENGTH (init_object_t, init_object_path) == 0
                               ? NULL
                               : VARR_LAST (init_object_t, init_object_path).container_type;
    init_el.init = initializer;
    VARR_PUSH (init_el_t, init_els, init_el);
    return;
  }
  init = NL_HEAD (initializer->u.ops);
  if (((str = initializer)->code == N_STR || str->code == N_STR16
       || str->code == N_STR32 /* string or string in parentheses  */
       || (init != NULL && init->code == N_INIT && NL_EL (initializer->u.ops, 1) == NULL
           && (des_list = NL_HEAD (init->u.ops))->code == N_LIST
           && NL_HEAD (des_list->u.ops) == NULL && NL_EL (init->u.ops, 1) != NULL
           && ((str = NL_EL (init->u.ops, 1))->code == N_STR || str->code == N_STR16
               || str->code == N_STR32)))
      && type->mode == TM_ARR && init_compatible_string_p (str, type->u.arr_type->el_type)) {
    init_el.c2m_ctx = c2m_ctx;
    init_el.num = VARR_LENGTH (init_el_t, init_els);
    init_el.offset = get_object_path_offset (c2m_ctx);
    init_el.member_decl = NULL;
    init_el.el_type = type;
    init_el.container_type = VARR_LENGTH (init_object_t, init_object_path) == 0
                               ? NULL
                               : VARR_LAST (init_object_t, init_object_path).container_type;
    init_el.init = str;
    VARR_PUSH (init_el_t, init_els, init_el);
    return;
  }
  if (init == NULL) return;
  assert (init->code == N_INIT);
  des_list = NL_HEAD (init->u.ops);
  assert (des_list->code == N_LIST);
  if (type->mode != TM_ARR && type->mode != TM_STRUCT && type->mode != TM_UNION) {
    assert (NL_NEXT (init) == NULL && NL_HEAD (des_list->u.ops) == NULL);
    initializer = NL_NEXT (des_list);
    assert (top_p);
    top_p = FALSE;
    goto check_one_value;
  }
  mark = VARR_LENGTH (init_object_t, init_object_path);
  init_object.container_type = type;
  init_object.field_designator_p = FALSE;
  if (type->mode == TM_ARR) {
    size_val = get_arr_type_size (type);
    /* we already figured out the array size during check: */
    assert (size_val >= 0);
    init_object.u.curr_index = -1;
  } else {
    init_object.u.curr_member = NULL;
  }
  VARR_PUSH (init_object_t, init_object_path, init_object);
  for (; init != NULL; init = NL_NEXT (init)) {
    assert (init->code == N_INIT);
    des_list = NL_HEAD (init->u.ops);
    value = NL_NEXT (des_list);
    assert ((value->code != N_LIST && value->code != N_COMPOUND_LITERAL) || type->mode == TM_ARR
            || type->mode == TM_STRUCT || type->mode == TM_UNION);
    if ((curr_des = NL_HEAD (des_list->u.ops)) == NULL) {
      ok_p = update_path_and_do (c2m_ctx, TRUE, collect_init_els, mark, value, const_only_p, NULL,
                                 POS (init), "");
      assert (ok_p);
    } else {
      struct type *curr_type = type;
      mir_llong arr_size_val MIR_UNUSED;
      int first_p = TRUE;

      VARR_TRUNC (init_object_t, init_object_path, mark + 1);
      for (; curr_des != NULL; curr_des = NL_NEXT (curr_des), first_p = FALSE) {
        init_object = VARR_LAST (init_object_t, init_object_path);
        if (first_p) {
          VARR_POP (init_object_t, init_object_path);
        } else {
          if (init_object.container_type->mode == TM_ARR) {
            curr_type = init_object.container_type->u.arr_type->el_type;
          } else {
            assert (init_object.container_type->mode == TM_STRUCT
                    || init_object.container_type->mode == TM_UNION);
            decl_t el_decl = init_object.u.curr_member->attr;
            curr_type = el_decl->decl_spec.type;
          }
        }
        if (curr_des->code == N_FIELD_ID) {
          node_t id = NL_HEAD (curr_des->u.ops);

          /* field should be only in struct/union initializer */
          assert (curr_type->mode == TM_STRUCT || curr_type->mode == TM_UNION);
          found_p = symbol_find (c2m_ctx, S_REGULAR, id, curr_type->u.tag_type, &sym);
          assert (found_p); /* field should present */
          process_init_field_designator (c2m_ctx, sym.def_node, curr_type);
          ok_p = update_path_and_do (c2m_ctx, NL_NEXT (curr_des) == NULL, collect_init_els, mark,
                                     value, const_only_p, NULL, POS (init), "");
          assert (ok_p);
        } else {
          cexpr = curr_des->attr;
          /* index should be in array initializer and const expr of right type and value: */
          assert (curr_type->mode == TM_ARR && cexpr->const_p && integer_type_p (cexpr->type)
                  && !incomplete_type_p (c2m_ctx, curr_type)
                  && (arr_size_val = get_arr_type_size (curr_type)) >= 0
                  && (mir_ullong) arr_size_val > cexpr->c.u_val);
          init_object.u.curr_index = cexpr->c.i_val - 1;
          init_object.field_designator_p = FALSE;
          init_object.container_type = curr_type;
          VARR_PUSH (init_object_t, init_object_path, init_object);
          ok_p = update_path_and_do (c2m_ctx, NL_NEXT (curr_des) == NULL, collect_init_els, mark,
                                     value, const_only_p, NULL, POS (init), "");
          assert (ok_p);
        }
      }
    }
  }
  VARR_TRUNC (init_object_t, init_object_path, mark);
}

static int cmp_init_el (const void *p1, const void *p2) {
  const init_el_t *el1 = p1, *el2 = p2;
  int bit_offset1 = el1->member_decl == NULL || el1->member_decl->bit_offset < 0
                      ? 0
                      : el1->member_decl->bit_offset;
  int bit_offset2 = el2->member_decl == NULL || el2->member_decl->bit_offset < 0
                      ? 0
                      : el2->member_decl->bit_offset;

  if (el1->offset + bit_offset1 / MIR_CHAR_BIT < el2->offset + bit_offset2 / MIR_CHAR_BIT)
    return -1;
  else if (el1->offset + bit_offset1 / MIR_CHAR_BIT > el2->offset + bit_offset2 / MIR_CHAR_BIT)
    return 1;
  else if (el1->member_decl != NULL && el2->member_decl != NULL
           && el1->member_decl->bit_offset < el2->member_decl->bit_offset)
    return -1;
  else if (el1->member_decl != NULL && el2->member_decl != NULL
           && el1->member_decl->bit_offset > el2->member_decl->bit_offset)
    return 1;
  else if (el1->member_decl != NULL
           && type_size (el1->c2m_ctx, el1->member_decl->decl_spec.type) == 0)
    return -1;
  else if (el2->member_decl != NULL
           && type_size (el2->c2m_ctx, el2->member_decl->decl_spec.type) == 0)
    return 1;
  else if (el1->num < el2->num)
    return -1;
  else if (el1->num > el2->num)
    return 1;
  else
    return 0;
}

static void move_item_to_module_start (MIR_module_t module, MIR_item_t item) {
  DLIST_REMOVE (MIR_item_t, module->items, item);
  DLIST_PREPEND (MIR_item_t, module->items, item);
}

static void move_item_forward (c2m_ctx_t c2m_ctx, MIR_item_t item) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  assert (curr_func != NULL);
  if (DLIST_TAIL (MIR_item_t, curr_func->module->items) != item) return;
  DLIST_REMOVE (MIR_item_t, curr_func->module->items, item);
  DLIST_INSERT_BEFORE (MIR_item_t, curr_func->module->items, curr_func, item);
}

static void gen_memset (c2m_ctx_t c2m_ctx, MIR_disp_t disp, MIR_reg_t base, mir_size_t len) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t ret_type;
  MIR_var_t vars[3];
  MIR_op_t treg_op, args[6];
  MIR_module_t module;

  if (memset_item == NULL) {
    ret_type = get_int_mir_type (sizeof (mir_size_t));
    vars[0].name = "s";
    vars[0].type = get_int_mir_type (sizeof (mir_size_t));
    vars[1].name = "c";
    vars[1].type = get_int_mir_type (sizeof (mir_int));
    vars[2].name = "n";
    vars[2].type = get_int_mir_type (sizeof (mir_size_t));
    module = curr_func->module;
    memset_proto = MIR_new_proto_arr (ctx, "memset_p", 1, &ret_type, 3, vars);
    memset_item = MIR_new_import (ctx, "memset");
    move_item_to_module_start (module, memset_proto);
    move_item_to_module_start (module, memset_item);
  }
  args[0] = MIR_new_ref_op (ctx, memset_proto);
  args[1] = MIR_new_ref_op (ctx, memset_item);
  args[2] = get_new_temp (c2m_ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
  if (disp == 0) {
    treg_op = MIR_new_reg_op (ctx, base);
  } else {
    treg_op = get_new_temp (c2m_ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
    emit3 (c2m_ctx, MIR_ADD, treg_op, MIR_new_reg_op (ctx, base), MIR_new_int_op (ctx, disp));
  }
  args[3] = treg_op;
  args[4] = MIR_new_int_op (ctx, 0);
  args[5] = MIR_new_uint_op (ctx, len);
  emit_insn (c2m_ctx, MIR_new_insn_arr (ctx, MIR_CALL, 6 /* args + proto + func + res */, args));
}

static void gen_memcpy (c2m_ctx_t c2m_ctx, MIR_disp_t disp, MIR_reg_t base, op_t val,
                        mir_size_t len) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t ret_type;
  MIR_var_t vars[3];
  MIR_op_t treg_op, args[6];
  MIR_module_t module;

  if (val.mir_op.mode == MIR_OP_MEM && val.mir_op.u.mem.index == 0 && val.mir_op.u.mem.disp == disp
      && val.mir_op.u.mem.base == base)
    return;
  if (memcpy_item == NULL) {
    ret_type = get_int_mir_type (sizeof (mir_size_t));
    vars[0].name = "dest";
    vars[0].type = get_int_mir_type (sizeof (mir_size_t));
    vars[1].name = "src";
    vars[1].type = get_int_mir_type (sizeof (mir_size_t));
    vars[2].name = "n";
    vars[2].type = get_int_mir_type (sizeof (mir_size_t));
    module = curr_func->module;
    memcpy_proto = MIR_new_proto_arr (ctx, "memcpy_p", 1, &ret_type, 3, vars);
    memcpy_item = MIR_new_import (ctx, "memcpy");
    move_item_to_module_start (module, memcpy_proto);
    move_item_to_module_start (module, memcpy_item);
  }
  args[0] = MIR_new_ref_op (ctx, memcpy_proto);
  args[1] = MIR_new_ref_op (ctx, memcpy_item);
  args[2] = get_new_temp (c2m_ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
  if (disp == 0) {
    treg_op = MIR_new_reg_op (ctx, base);
  } else {
    treg_op = get_new_temp (c2m_ctx, get_int_mir_type (sizeof (mir_size_t))).mir_op;
    emit3 (c2m_ctx, MIR_ADD, treg_op, MIR_new_reg_op (ctx, base), MIR_new_int_op (ctx, disp));
  }
  args[3] = treg_op;
  args[4] = mem_to_address (c2m_ctx, val, FALSE).mir_op;
  args[5] = MIR_new_uint_op (ctx, len);
  emit_insn (c2m_ctx, MIR_new_insn_arr (ctx, MIR_CALL, 6 /* args + proto + func + res */, args));
}

static void emit_scalar_assign (c2m_ctx_t c2m_ctx, op_t var, op_t *val, MIR_type_t t,
                                int ignore_others_p) {
  if (var.decl == NULL || var.decl->bit_offset < 0) {
    emit2_noopt (c2m_ctx, tp_mov (t), var.mir_op, val->mir_op);
  } else {
    MIR_context_t ctx = c2m_ctx->ctx;
    int width = var.decl->width;
    uint64_t mask, mask2;
    op_t temp_op1, temp_op2, temp_op3, temp_op4;
    size_t MIR_UNUSED size = type_size (c2m_ctx, var.decl->decl_spec.type) * MIR_CHAR_BIT;

    assert (var.mir_op.mode == MIR_OP_MEM); /*???*/
    mask = 0xffffffffffffffff >> (64 - width);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    mask2 = ~(mask << var.decl->bit_offset);
#else
    mask2 = ~(mask << (size - var.decl->bit_offset - width));
#endif
    temp_op1 = get_new_temp (c2m_ctx, MIR_T_I64);
    temp_op2 = get_new_temp (c2m_ctx, MIR_T_I64);
    temp_op3 = get_new_temp (c2m_ctx, MIR_T_I64);
    if (!ignore_others_p) {
      emit2_noopt (c2m_ctx, MIR_MOV, temp_op2.mir_op, var.mir_op);
      emit3 (c2m_ctx, MIR_AND, temp_op2.mir_op, temp_op2.mir_op, MIR_new_uint_op (ctx, mask2));
    }
    if (!signed_integer_type_p (var.decl->decl_spec.type)) {
      emit2 (c2m_ctx, MIR_MOV, temp_op1.mir_op, val->mir_op);
      *val = temp_op3;
    } else {
      emit3 (c2m_ctx, MIR_LSH, temp_op1.mir_op, val->mir_op, MIR_new_int_op (ctx, 64 - width));
      emit3 (c2m_ctx, MIR_RSH, temp_op1.mir_op, temp_op1.mir_op, MIR_new_int_op (ctx, 64 - width));
      *val = temp_op1;
    }
    emit3 (c2m_ctx, MIR_AND, temp_op3.mir_op, temp_op1.mir_op, MIR_new_uint_op (ctx, mask));
    temp_op4 = get_new_temp (c2m_ctx, MIR_T_I64);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (var.decl->bit_offset == 0) {
      temp_op4 = temp_op3;
    } else {
      emit3 (c2m_ctx, MIR_LSH, temp_op4.mir_op, temp_op3.mir_op,
             MIR_new_int_op (ctx, var.decl->bit_offset));
    }
#else
    if (size - var.decl->bit_offset - width == 0) {
      temp_op4 = temp_op3;
    } else {
      emit3 (c2m_ctx, MIR_LSH, temp_op4.mir_op, temp_op3.mir_op,
             MIR_new_int_op (ctx, size - var.decl->bit_offset - width));
    }
#endif
    if (!ignore_others_p) {
      emit3 (c2m_ctx, MIR_OR, temp_op4.mir_op, temp_op4.mir_op, temp_op2.mir_op);
    }
    emit2 (c2m_ctx, MIR_MOV, var.mir_op, temp_op4.mir_op);
  }
}

static void add_bit_field (c2m_ctx_t c2m_ctx, uint64_t *u, uint64_t v, decl_t member_decl) {
  uint64_t mask, mask2;
  int bit_offset = member_decl->bit_offset, width = member_decl->width;
  size_t MIR_UNUSED size = type_size (c2m_ctx, member_decl->decl_spec.type) * MIR_CHAR_BIT;

  mask = 0xffffffffffffffff >> (64 - width);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  mask2 = ~(mask << bit_offset);
#else
  mask2 = ~(mask << (size - bit_offset - width));
#endif
  *u &= mask2;
  if (signed_integer_type_p (member_decl->decl_spec.type)) {
    v <<= (64 - width);
    v = (int64_t) v >> (64 - width);
  }
  v &= mask;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  v <<= bit_offset;
#else
  v <<= size - bit_offset - width;
#endif
  *u |= v;
}

static MIR_item_t get_mir_str_op_data (c2m_ctx_t c2m_ctx, MIR_str_t str) {
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_item_t data;
  char buff[50];
  MIR_module_t module = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));

  _MIR_get_temp_item_name (ctx, module, buff, sizeof (buff));
  data = MIR_new_string_data (ctx, buff, str);
  move_item_to_module_start (module, data);
  return data;
}

static MIR_item_t get_string_data (c2m_ctx_t c2m_ctx, node_t n) {
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_item_t data;
  char buff[50];
  MIR_module_t module = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));

  _MIR_get_temp_item_name (ctx, module, buff, sizeof (buff));
  if (n->code == N_STR) {
    data = MIR_new_string_data (ctx, buff, (MIR_str_t){n->u.s.len, n->u.s.s});
  } else {
    assert (n->code == N_STR16 || n->code == N_STR32);
    if (n->code == N_STR16) {
      data = MIR_new_data (ctx, buff, MIR_T_U16, n->u.s.len / 2, n->u.s.s);
    } else {
      data = MIR_new_data (ctx, buff, MIR_T_U32, n->u.s.len / 4, n->u.s.s);
    }
  }
  move_item_to_module_start (module, data);
  return data;
}

static void gen_initializer (c2m_ctx_t c2m_ctx, size_t init_start, op_t var,
                             const char *global_name, mir_size_t size, int local_p) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  op_t val;
  size_t str_len;
  mir_size_t data_size, el_size, offset = 0, rel_offset = 0, start_offset;
  init_el_t init_el, next_init_el;
  MIR_reg_t base;
  MIR_type_t t;
  MIR_item_t data = NULL; /* to remove a warning */
  struct expr *e;

  if (var.mir_op.mode == MIR_OP_REG) { /* scalar initialization: */
    assert (local_p && offset == 0 && VARR_LENGTH (init_el_t, init_els) - init_start == 1);
    init_el = VARR_GET (init_el_t, init_els, init_start);
    val = val_gen (c2m_ctx, init_el.init);
    t = get_op_type (c2m_ctx, var);
    val = cast (c2m_ctx, val, get_mir_type (c2m_ctx, init_el.el_type), FALSE);
    emit_scalar_assign (c2m_ctx, var, &val, t, FALSE);
  } else if (local_p) { /* local variable initialization: */
    assert (var.mir_op.mode == MIR_OP_MEM && var.mir_op.u.mem.index == 0); /*???*/
    offset = var.mir_op.u.mem.disp;
    base = var.mir_op.u.mem.base;
    for (size_t i = init_start; i < VARR_LENGTH (init_el_t, init_els); i++) {
      init_el = VARR_GET (init_el_t, init_els, i);
      t = get_mir_type (c2m_ctx, init_el.el_type);
      if (rel_offset < init_el.offset) { /* fill the gap: */
        gen_memset (c2m_ctx, offset + rel_offset, base, init_el.offset - rel_offset);
        rel_offset = init_el.offset;
      }
      if (t == MIR_T_UNDEF)
        val = new_op (NULL, MIR_new_mem_op (ctx, t, offset + rel_offset, base, 0, 1));
      val = gen (c2m_ctx, init_el.init, NULL, NULL, t != MIR_T_UNDEF,
                 t != MIR_T_UNDEF ? NULL : &val, NULL);
      if (!scalar_type_p (init_el.el_type)) {
        mir_size_t s = init_el.init->code == N_STR     ? init_el.init->u.s.len
                       : init_el.init->code == N_STR16 ? init_el.init->u.s.len / 2
                       : init_el.init->code == N_STR32 ? init_el.init->u.s.len / 4
                                                       : raw_type_size (c2m_ctx, init_el.el_type);
        gen_memcpy (c2m_ctx, offset + rel_offset, base, val, s);
        rel_offset = init_el.offset + s;
      } else {
        MIR_op_t mem
          = MIR_new_alias_mem_op (ctx, t, offset + init_el.offset, base, 0, 1,
                                  get_type_alias (c2m_ctx,
                                                  init_el.container_type != NULL
                                                      && init_el.container_type->mode == TM_UNION
                                                    ? init_el.container_type
                                                    : init_el.el_type),
                                  0);
        val = cast (c2m_ctx, val, get_mir_type (c2m_ctx, init_el.el_type), FALSE);
        emit_scalar_assign (c2m_ctx, new_op (init_el.member_decl, mem), &val, t,
                            i == init_start || rel_offset == init_el.offset);
        rel_offset = init_el.offset + _MIR_type_size (ctx, t);
      }
    }
    if (rel_offset < size) /* fill the tail: */
      gen_memset (c2m_ctx, offset + rel_offset, base, size - rel_offset);
  } else {
    assert (var.mir_op.mode == MIR_OP_REF);
    for (size_t i = init_start; i < VARR_LENGTH (init_el_t, init_els); i++) {
      init_el = VARR_GET (init_el_t, init_els, i);
      if (i != init_start && init_el.offset == VARR_GET (init_el_t, init_els, i - 1).offset
          && (init_el.member_decl == NULL || init_el.member_decl->bit_offset < 0))
        continue;
      e = init_el.init->attr;
      if (!e->const_addr_p) {
        if (e->const_p) {
          convert_value (e, init_el.el_type);
          e->type = init_el.el_type; /* to get the right value in the subsequent gen call */
        }
        val = val_gen (c2m_ctx, init_el.init);
        assert (val.mir_op.mode == MIR_OP_INT || val.mir_op.mode == MIR_OP_UINT
                || val.mir_op.mode == MIR_OP_FLOAT || val.mir_op.mode == MIR_OP_DOUBLE
                || val.mir_op.mode == MIR_OP_LDOUBLE || val.mir_op.mode == MIR_OP_STR
                || val.mir_op.mode == MIR_OP_REF);
      }
      if (rel_offset < init_el.offset) { /* fill the gap: */
        data = MIR_new_bss (ctx, global_name, init_el.offset - rel_offset);
        if (global_name != NULL) var.decl->u.item = data;
        global_name = NULL;
      }
      t = get_mir_type (c2m_ctx, init_el.el_type);
      if (e->const_addr_p) {
        node_t def;

        if ((def = e->def_node) == NULL) { /* constant address */
          mir_size_t s = e->c.i_val;
          data = MIR_new_data (ctx, global_name, MIR_T_P, 1, &s);
          data_size = _MIR_type_size (ctx, MIR_T_P);
        } else if (def->code == N_LABEL_ADDR) {
          data = MIR_new_lref_data (ctx, global_name,
                                    get_label (c2m_ctx,
                                               ((struct expr *) def->attr)->u.label_addr_target),
                                    NULL, e->c.i_val);
          data_size = _MIR_type_size (ctx, t);
        } else {
          if (def->code != N_STR && def->code != N_STR16 && def->code != N_STR32) {
            data = ((decl_t) def->attr)->u.item;
          } else {
            data = get_string_data (c2m_ctx, def);
          }
          data = MIR_new_ref_data (ctx, global_name, data, e->c.i_val);
          data_size = _MIR_type_size (ctx, t);
        }
      } else if (val.mir_op.mode == MIR_OP_REF) {
        data = MIR_new_ref_data (ctx, global_name, val.mir_op.u.ref, 0);
        data_size = _MIR_type_size (ctx, t);
      } else if (val.mir_op.mode != MIR_OP_STR) {
        union {
          int8_t i8;
          uint8_t u8;
          int16_t i16;
          uint16_t u16;
          int32_t i32;
          uint32_t u32;
          int64_t i64;
          uint64_t u64;
          float f;
          double d;
          long double ld;
          uint8_t data[8];
        } u;
        start_offset = 0;
        el_size = data_size = _MIR_type_size (ctx, t);
        if (init_el.member_decl != NULL && init_el.member_decl->bit_offset >= 0) {
          uint64_t uval = 0;

          assert (val.mir_op.mode == MIR_OP_INT || val.mir_op.mode == MIR_OP_UINT);
          assert (init_el.member_decl->bit_offset % 8 == 0); /* first in the group of bitfields */
          start_offset = init_el.member_decl->bit_offset / 8;
          add_bit_field (c2m_ctx, &uval, val.mir_op.u.u, init_el.member_decl);
          for (; i + 1 < VARR_LENGTH (init_el_t, init_els); i++, init_el = next_init_el) {
            next_init_el = VARR_GET (init_el_t, init_els, i + 1);
            if (next_init_el.offset != init_el.offset) break;
            if (next_init_el.member_decl->bit_offset == init_el.member_decl->bit_offset) continue;
            val = val_gen (c2m_ctx, next_init_el.init);
            assert (val.mir_op.mode == MIR_OP_INT || val.mir_op.mode == MIR_OP_UINT);
            add_bit_field (c2m_ctx, &uval, val.mir_op.u.u, next_init_el.member_decl);
          }
          val.mir_op.u.u = uval;
          if (i + 1 < VARR_LENGTH (init_el_t, init_els)
              && next_init_el.offset - init_el.offset < data_size)
            data_size = next_init_el.offset - init_el.offset;
        }
        switch (t) {
        case MIR_T_I8: u.i8 = (int8_t) val.mir_op.u.i; break;
        case MIR_T_U8: u.u8 = (uint8_t) val.mir_op.u.u; break;
        case MIR_T_I16: u.i16 = (int16_t) val.mir_op.u.i; break;
        case MIR_T_U16: u.u16 = (uint16_t) val.mir_op.u.u; break;
        case MIR_T_I32: u.i32 = (int32_t) val.mir_op.u.i; break;
        case MIR_T_U32: u.u32 = (uint32_t) val.mir_op.u.u; break;
        case MIR_T_I64: u.i64 = val.mir_op.u.i; break;
        case MIR_T_U64: u.u64 = val.mir_op.u.u; break;
        case MIR_T_F: u.f = val.mir_op.u.f; break;
        case MIR_T_D: u.d = val.mir_op.u.d; break;
        case MIR_T_LD: u.ld = val.mir_op.u.ld; break;
        default: assert (FALSE);
        }
        if (start_offset == 0 && data_size == el_size) {
          data = MIR_new_data (ctx, global_name, t, 1, &u);
        } else {
          for (mir_size_t byte_num = start_offset; byte_num < data_size; byte_num++) {
            if (byte_num == start_offset)
              data = MIR_new_data (ctx, global_name, MIR_T_U8, 1, &u.data[byte_num]);
            else
              MIR_new_data (ctx, NULL, MIR_T_U8, 1, &u.data[byte_num]);
          }
        }
      } else if (init_el.el_type->mode == TM_ARR) {
        data_size = raw_type_size (c2m_ctx, init_el.el_type);
        str_len = val.mir_op.u.str.len;
        if (data_size < str_len) {
          data = MIR_new_data (ctx, global_name, MIR_T_U8, data_size, val.mir_op.u.str.s);
        } else {
          data = MIR_new_string_data (ctx, global_name, val.mir_op.u.str);
          if (data_size > str_len) MIR_new_bss (ctx, NULL, data_size - str_len);
        }
      } else {
        data = get_mir_str_op_data (c2m_ctx, val.mir_op.u.str);
        data = MIR_new_ref_data (ctx, global_name, data, 0);
        data_size = _MIR_type_size (ctx, t);
      }
      if (global_name != NULL) var.decl->u.item = data;
      global_name = NULL;
      rel_offset = init_el.offset + data_size;
    }
    if (rel_offset < size || size == 0) { /* fill the tail: */
      data = MIR_new_bss (ctx, global_name, size - rel_offset);
      if (global_name != NULL) var.decl->u.item = data;
    }
  }
}

static MIR_item_t get_ref_item (c2m_ctx_t c2m_ctx, node_t def, const char *name) {
  MIR_context_t ctx = c2m_ctx->ctx;
  struct decl *decl = def->attr;

  if (def->code == N_FUNC_DEF
      || (def->code == N_SPEC_DECL && NL_EL (def->u.ops, 1)->code == N_DECL
          && decl->scope == top_scope && decl->decl_spec.type->mode != TM_FUNC
          && !decl->decl_spec.typedef_p && !decl->decl_spec.extern_p))
    return (decl->decl_spec.linkage == N_EXTERN ? MIR_new_export (ctx, name)
                                                : MIR_new_forward (ctx, name));
  return NULL;
}

static void emit_bin_op (c2m_ctx_t c2m_ctx, node_t r, struct type *type, op_t res, op_t op1,
                         op_t op2) {
  MIR_context_t ctx = c2m_ctx->ctx;
  op_t temp;

  if (type->mode == TM_PTR) { /* ptr +/- int */
    assert (r->code == N_ADD || r->code == N_SUB || r->code == N_ADD_ASSIGN
            || r->code == N_SUB_ASSIGN);
    if (((struct expr *) NL_HEAD (r->u.ops)->attr)->type->mode != TM_PTR) /* int + ptr */
      SWAP (op1, op2, temp);
    if (op2.mir_op.mode == MIR_OP_INT || op2.mir_op.mode == MIR_OP_UINT) {
      op2 = new_op (NULL,
                    MIR_new_int_op (ctx, op2.mir_op.u.i * type_size (c2m_ctx, type->u.ptr_type)));
    } else {
      temp = get_new_temp (c2m_ctx, get_mir_type (c2m_ctx, type));
      emit3 (c2m_ctx, sizeof (mir_size_t) == 8 ? MIR_MUL : MIR_MULS, temp.mir_op, op2.mir_op,
             MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type)));
      op2 = temp;
    }
  }
  emit3 (c2m_ctx, get_mir_type_insn_code (c2m_ctx, type, r), res.mir_op, op1.mir_op, op2.mir_op);
  if (type->mode != TM_PTR
      && (type = ((struct expr *) NL_HEAD (r->u.ops)->attr)->type)->mode
           == TM_PTR) { /* ptr - ptr */
    assert (r->code == N_SUB || r->code == N_SUB_ASSIGN);
    emit3 (c2m_ctx, sizeof (mir_size_t) == 8 ? MIR_DIV : MIR_DIVS, res.mir_op, res.mir_op,
           MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type)));
  }
}

static int signed_case_compare (const void *v1, const void *v2) {
  case_t c1 = *(const case_t *) v1, c2 = *(const case_t *) v2;
  struct expr *e1 = NL_HEAD (c1->case_node->u.ops)->attr;
  struct expr *e2 = NL_HEAD (c2->case_node->u.ops)->attr;

  assert (e1->c.i_val != e2->c.i_val);
  return e1->c.i_val < e2->c.i_val ? -1 : 1;
}

static int unsigned_case_compare (const void *v1, const void *v2) {
  case_t c1 = *(const case_t *) v1, c2 = *(const case_t *) v2;
  struct expr *e1 = NL_HEAD (c1->case_node->u.ops)->attr;
  struct expr *e2 = NL_HEAD (c2->case_node->u.ops)->attr;

  assert (e1->c.u_val != e2->c.u_val);
  return e1->c.u_val < e2->c.u_val ? -1 : 1;
}

static void make_cond_val (c2m_ctx_t c2m_ctx, node_t r, MIR_label_t true_label,
                           MIR_label_t false_label, op_t *res) {
  MIR_context_t ctx = c2m_ctx->ctx;
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  struct type *type = ((struct expr *) r->attr)->type;
  MIR_label_t end_label = MIR_new_label (ctx);
  *res = get_new_temp (c2m_ctx, get_mir_type (c2m_ctx, type));
  emit_label_insn_opt (c2m_ctx, true_label);
  emit2 (c2m_ctx, MIR_MOV, res->mir_op, one_op.mir_op);
  emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
  emit_label_insn_opt (c2m_ctx, false_label);
  emit2 (c2m_ctx, MIR_MOV, res->mir_op, zero_op.mir_op);
  emit_label_insn_opt (c2m_ctx, end_label);
}

static op_t gen (c2m_ctx_t c2m_ctx, node_t r, MIR_label_t true_label, MIR_label_t false_label,
                 int val_p, op_t *desirable_dest, int *expect_res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  check_ctx_t check_ctx = c2m_ctx->check_ctx; /* check and gen share curr_scope */
  MIR_context_t ctx = c2m_ctx->ctx;
  op_t res, op1, op2, op3, var, val;
  MIR_type_t t = MIR_T_UNDEF; /* to remove an uninitialized warning */
  MIR_insn_code_t insn_code;
  MIR_type_t mir_type;
  struct expr *e = NULL; /* to remove an uninitialized warning */
  struct type *type;
  decl_t decl;
  long double ld;
  long long ll;
  unsigned long long ull;
  int expr_attr_p, stmt_p;

  classify_node (r, &expr_attr_p, &stmt_p);
  assert ((true_label == NULL && false_label == NULL && expect_res == NULL)
          || (true_label != NULL && false_label != NULL));
  assert (!val_p || desirable_dest == NULL);
  if (expect_res != NULL) *expect_res = 0; /* no expected result */
  if (r->code != N_ANDAND && r->code != N_OROR && expr_attr_p && push_const_val (c2m_ctx, r, &res))
    goto finish;
  switch (r->code) {
  case N_LIST:
    for (node_t n = NL_HEAD (r->u.ops); n != NULL; n = NL_NEXT (n))
      gen (c2m_ctx, n, true_label, false_label, val_p, NULL, expect_res);
    break;
  case N_IGNORE: break; /* do nothing */
  case N_I:
  case N_L: ll = r->u.l; goto int_val;
  case N_LL:
    ll = r->u.ll;
  int_val:
    res = new_op (NULL, MIR_new_int_op (ctx, ll));
    break;
  case N_U:
  case N_UL: ull = r->u.ul; goto uint_val;
  case N_ULL:
    ull = r->u.ull;
  uint_val:
    res = new_op (NULL, MIR_new_uint_op (ctx, ull));
    break;
  case N_F: ld = r->u.f; goto float_val;
  case N_D: ld = r->u.d; goto float_val;
  case N_LD:
    ld = r->u.ld;
  float_val:
    mir_type = get_mir_type (c2m_ctx, ((struct expr *) r->attr)->type);
    res = new_op (NULL, (mir_type == MIR_T_F   ? MIR_new_float_op (ctx, (float) ld)
                         : mir_type == MIR_T_D ? MIR_new_double_op (ctx, ld)
                                               : MIR_new_ldouble_op (ctx, ld)));
    break;
  case N_CH: ll = r->u.ch; goto int_val;
  case N_CH16:
  case N_CH32: ll = r->u.ul; goto int_val;
  case N_STR16:
  case N_STR32: res = new_op (NULL, MIR_new_ref_op (ctx, get_string_data (c2m_ctx, r))); break;
  case N_STR:
    res
      = new_op (NULL,
                MIR_new_str_op (ctx, (MIR_str_t){r->u.s.len, r->u.s.s}));  //???what to do with decl
                                                                           // and str in initializer
    break;
  case N_COMMA:
    gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, FALSE, NULL, NULL);
    res = gen (c2m_ctx, NL_EL (r->u.ops, 1), true_label, false_label,
               true_label == NULL && !void_type_p (((struct expr *) r->attr)->type), NULL,
               expect_res);
    if (true_label != NULL) {
      true_label = false_label = NULL;
      val_p = FALSE;
    }
    break;
  case N_ANDAND:
  case N_OROR:
    if (!push_const_val (c2m_ctx, r, &res)) {
      MIR_label_t temp_label = MIR_new_label (ctx), t_label = true_label, f_label = false_label;
      int make_val_p = t_label == NULL;

      if (make_val_p) {
        t_label = MIR_new_label (ctx);
        f_label = MIR_new_label (ctx);
      }
      assert (t_label != NULL && f_label != NULL);
      gen (c2m_ctx, NL_HEAD (r->u.ops), r->code == N_ANDAND ? temp_label : t_label,
           r->code == N_ANDAND ? f_label : temp_label, FALSE, NULL, NULL);
      emit_label_insn_opt (c2m_ctx, temp_label);
      gen (c2m_ctx, NL_EL (r->u.ops, 1), t_label, f_label, FALSE, NULL, NULL);
      if (make_val_p) make_cond_val (c2m_ctx, r, t_label, f_label, &res);
      true_label = false_label = NULL;
    } else if (true_label != NULL) {
      int true_p;

      assert (res.mir_op.mode == MIR_OP_INT || res.mir_op.mode == MIR_OP_UINT
              || res.mir_op.mode == MIR_OP_FLOAT || res.mir_op.mode == MIR_OP_DOUBLE);
      true_p = ((res.mir_op.mode == MIR_OP_INT && res.mir_op.u.i != 0)
                || (res.mir_op.mode == MIR_OP_UINT && res.mir_op.u.u != 0)
                || (res.mir_op.mode == MIR_OP_FLOAT && res.mir_op.u.f != 0.0f)
                || (res.mir_op.mode == MIR_OP_DOUBLE && res.mir_op.u.d != 0.0));
      emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, true_p ? true_label : false_label));
      true_label = false_label = NULL;
    }
    break;
  case N_BITWISE_NOT:
    gen_unary_op (c2m_ctx, r, &op1, &res);
    t = get_mir_type (c2m_ctx, ((struct expr *) r->attr)->type);
    emit3 (c2m_ctx, t == MIR_T_I64 || t == MIR_T_U64 ? MIR_XOR : MIR_XORS, res.mir_op, op1.mir_op,
           minus_one_op.mir_op);
    break;
  case N_NOT:
    if (true_label != NULL) {
      gen (c2m_ctx, NL_HEAD (r->u.ops), false_label, true_label, FALSE, NULL, NULL);
      true_label = false_label = NULL;
    } else {
      MIR_label_t end_label = MIR_new_label (ctx);
      MIR_label_t t_label = MIR_new_label (ctx), f_label = MIR_new_label (ctx);

      res = get_new_temp (c2m_ctx, MIR_T_I64);
      gen (c2m_ctx, NL_HEAD (r->u.ops), t_label, f_label, FALSE, NULL, NULL);
      emit_label_insn_opt (c2m_ctx, t_label);
      emit2 (c2m_ctx, MIR_MOV, res.mir_op, zero_op.mir_op);
      emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
      emit_label_insn_opt (c2m_ctx, f_label);
      emit2 (c2m_ctx, MIR_MOV, res.mir_op, one_op.mir_op);
      emit_label_insn_opt (c2m_ctx, end_label);
    }
    break;
  case N_ADD:
  case N_SUB:
    if (NL_NEXT (NL_HEAD (r->u.ops)) == NULL) { /* unary */
      MIR_insn_code_t ic = get_mir_insn_code (c2m_ctx, r);

      gen_unary_op (c2m_ctx, r, &op1, &res);
      if (r->code == N_ADD) {
        ic = (ic == MIR_FADD    ? MIR_FMOV
              : ic == MIR_DADD  ? MIR_DMOV
              : ic == MIR_LDADD ? MIR_LDMOV
                                : MIR_MOV);
        emit2 (c2m_ctx, ic, res.mir_op, op1.mir_op);
      } else {
        ic = (ic == MIR_FSUB    ? MIR_FNEG
              : ic == MIR_DSUB  ? MIR_DNEG
              : ic == MIR_LDSUB ? MIR_LDNEG
              : ic == MIR_SUB   ? MIR_NEG
                                : MIR_NEGS);
        emit2 (c2m_ctx, ic, res.mir_op, op1.mir_op);
      }
      break;
    }
  /* falls through */
  case N_AND:
  case N_OR:
  case N_XOR:
  case N_LSH:
  case N_RSH:
  case N_MUL:
  case N_DIV:
  case N_MOD:
    gen_bin_op (c2m_ctx, r, &op1, &op2, &res);
    emit_bin_op (c2m_ctx, r, ((struct expr *) r->attr)->type, res, op1, op2);
    break;
  case N_EQ:
  case N_NE:
  case N_LT:
  case N_LE:
  case N_GT:
  case N_GE: {
    struct type *type1 = ((struct expr *) NL_HEAD (r->u.ops)->attr)->type;
    struct type *type2 = ((struct expr *) NL_EL (r->u.ops, 1)->attr)->type;
    struct type type_s, ptr_type_s = get_ptr_int_type (FALSE);

    type_s = arithmetic_conversion (type1->mode == TM_PTR ? &ptr_type_s : type1,
                                    type2->mode == TM_PTR ? &ptr_type_s : type2);
    set_type_layout (c2m_ctx, &type_s);
    gen_cmp_op (c2m_ctx, r, &type_s, &op1, &op2, &res);
    insn_code = get_mir_type_insn_code (c2m_ctx, &type_s, r);
    if (true_label == NULL) {
      emit3 (c2m_ctx, insn_code, res.mir_op, op1.mir_op, op2.mir_op);
    } else {
      insn_code = get_compare_branch_code (insn_code);
      emit3 (c2m_ctx, insn_code, MIR_new_label_op (ctx, true_label), op1.mir_op, op2.mir_op);
      emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, false_label));
      true_label = false_label = NULL;
    }
    break;
  }
  case N_POST_INC:
  case N_POST_DEC: {
    type = ((struct expr *) r->attr)->type2;
    t = get_mir_type (c2m_ctx, type);
    var = gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, FALSE, NULL, NULL);
    op1 = force_val (c2m_ctx, var, FALSE);
    if (val_p || true_label != NULL) {
      res = get_new_temp (c2m_ctx, t);
      emit2 (c2m_ctx, tp_mov (t), res.mir_op, op1.mir_op);
    }
    val = promote (c2m_ctx, op1, t, TRUE);
    op2 = promote (c2m_ctx,
                   type->mode != TM_PTR
                     ? one_op
                     : new_op (NULL, MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type))),
                   t, FALSE);
    emit3 (c2m_ctx, get_mir_insn_code (c2m_ctx, r), val.mir_op, val.mir_op, op2.mir_op);
    t = promote_mir_int_type (t);
    goto assign;
  }
  case N_INC:
  case N_DEC: {
    type = ((struct expr *) r->attr)->type2;
    t = get_mir_type (c2m_ctx, type);
    var = gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, FALSE, NULL, NULL);
    val = promote (c2m_ctx, force_val (c2m_ctx, var, FALSE), t, TRUE);
    op2 = promote (c2m_ctx,
                   type->mode != TM_PTR
                     ? one_op
                     : new_op (NULL, MIR_new_int_op (ctx, type_size (c2m_ctx, type->u.ptr_type))),
                   t, FALSE);
    t = promote_mir_int_type (t);
    res = get_new_temp (c2m_ctx, t);
    emit3 (c2m_ctx, get_mir_insn_code (c2m_ctx, r), val.mir_op, val.mir_op, op2.mir_op);
    goto assign;
  }
  case N_AND_ASSIGN:
  case N_OR_ASSIGN:
  case N_XOR_ASSIGN:
  case N_LSH_ASSIGN:
  case N_RSH_ASSIGN:
  case N_ADD_ASSIGN:
  case N_SUB_ASSIGN:
  case N_MUL_ASSIGN:
  case N_DIV_ASSIGN:
  case N_MOD_ASSIGN:
    gen_assign_bin_op (c2m_ctx, r, ((struct expr *) r->attr)->type2, &val, &op2, &var);
    emit_bin_op (c2m_ctx, r, ((struct expr *) r->attr)->type2, val, val, op2);
    t = get_op_type (c2m_ctx, var);
    t = promote_mir_int_type (t);
    res = get_new_temp (c2m_ctx, t);
    goto assign;
    break;
  case N_ASSIGN:
    var = gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, FALSE, NULL, NULL);
    t = get_op_type (c2m_ctx, var);
    op2 = gen (c2m_ctx, NL_EL (r->u.ops, 1), NULL, NULL, t != MIR_T_UNDEF,
               t != MIR_T_UNDEF ? NULL : &var, NULL);
    if ((!val_p && true_label == NULL) || t == MIR_T_UNDEF) {
      res = var;
      val = op2;
    } else {
      t = promote_mir_int_type (t);
      val = promote (c2m_ctx, op2, t, TRUE);
      res = get_new_temp (c2m_ctx, t);
    }
  assign: /* t/val is promoted type/new value of assign expression */
    if (scalar_type_p (((struct expr *) r->attr)->type)) {
      assert (t != MIR_T_UNDEF);
      val = cast (c2m_ctx, val, get_mir_type (c2m_ctx, ((struct expr *) r->attr)->type), FALSE);
      emit_scalar_assign (c2m_ctx, var, &val, t, FALSE);
      if ((val_p || true_label != NULL) && r->code != N_POST_INC && r->code != N_POST_DEC)
        emit2_noopt (c2m_ctx, tp_mov (t), res.mir_op, val.mir_op);
    } else { /* block move */
      mir_size_t size = type_size (c2m_ctx, ((struct expr *) r->attr)->type);

      assert (r->code == N_ASSIGN);
      block_move (c2m_ctx, var, val, size);
    }
    break;
  case N_ID: {
    e = r->attr;
    assert (!e->const_p);
    if (e->u.lvalue_node == NULL) {
      res = new_op (NULL, MIR_new_ref_op (ctx, ((decl_t) e->def_node->attr)->u.item));
    } else if (((decl = e->u.lvalue_node->attr)->scope == top_scope || decl->decl_spec.static_p
                || decl->decl_spec.linkage != N_IGNORE)
               && !decl->asm_p) {
      t = get_mir_type (c2m_ctx, e->type);
      res = get_new_temp (c2m_ctx, MIR_T_I64);
      emit2 (c2m_ctx, MIR_MOV, res.mir_op, MIR_new_ref_op (ctx, decl->u.item));
      res = new_op (decl, MIR_new_alias_mem_op (ctx, t, 0, res.mir_op.u.reg, 0, 1,
                                                get_type_alias (c2m_ctx, e->type), 0));
    } else if (!decl->reg_p) {
      t = get_mir_type (c2m_ctx, e->type);
      res = new_op (decl, MIR_new_alias_mem_op (ctx, t, decl->offset,
                                                MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1,
                                                get_type_alias (c2m_ctx, e->type), 0));
    } else {
      const char *name;
      reg_var_t reg_var;

      t = get_mir_type (c2m_ctx, e->type);
      assert (t != MIR_T_UNDEF);
      t = promote_mir_int_type (t);
      name = get_reg_var_name (c2m_ctx, t, r->u.s.s,
                               ((struct node_scope *) decl->scope->attr)->func_scope_num);
      reg_var = get_reg_var (c2m_ctx, t, name, decl->u.asm_str);
      res = new_op (decl, MIR_new_reg_op (ctx, reg_var.reg));
    }
    break;
  }
  case N_IND: {
    MIR_type_t ind_t;
    node_t arr = NL_HEAD (r->u.ops);
    struct type *el_type = ((struct expr *) r->attr)->type;
    struct type *arr_type = ((struct expr *) arr->attr)->type;
    mir_size_t size = type_size (c2m_ctx, el_type);

    t = get_mir_type (c2m_ctx, el_type);
    op1 = val_gen (c2m_ctx, arr);
    op2 = val_gen (c2m_ctx, NL_EL (r->u.ops, 1));
    ind_t = get_mir_type (c2m_ctx, ((struct expr *) NL_EL (r->u.ops, 1)->attr)->type);
#if MIR_PTR32
    op2 = force_reg (c2m_ctx, op2, ind_t);
#else
    if (op2.mir_op.mode != MIR_OP_REG) {
      op2 = force_reg (c2m_ctx, op2, ind_t);
    } else if (ind_t != MIR_T_I64 && ind_t != MIR_T_U64) {
      op2 = cast (c2m_ctx, op2, ind_t == MIR_T_I32 ? MIR_T_I64 : MIR_T_U64, FALSE);
    }
#endif
    if (el_type->mode == TM_PTR && el_type->arr_type != NULL) { /* elem is an array */
      size = type_size (c2m_ctx, el_type->arr_type);
    }
    if (arr_type->mode == TM_PTR && arr_type->arr_type != NULL) { /* indexing an array */
      op1 = force_reg_or_mem (c2m_ctx, op1, MIR_T_I64);
      assert (op1.mir_op.mode == MIR_OP_REG || op1.mir_op.mode == MIR_OP_MEM); /*???*/
    } else {
      op1 = force_reg (c2m_ctx, op1, MIR_T_I64);
      assert (op1.mir_op.mode == MIR_OP_REG);
    }
    res = op1;
    res.decl = NULL;
    if (res.mir_op.mode == MIR_OP_REG)
      res.mir_op = MIR_new_alias_mem_op (ctx, t, 0, res.mir_op.u.reg, 0, 1,
                                         get_type_alias (c2m_ctx, el_type), arr_type->antialias);
    if (res.mir_op.u.mem.base == 0 && size == 1) {
      res.mir_op.u.mem.base = op2.mir_op.u.reg;
    } else if (res.mir_op.u.mem.index == 0 && size <= MIR_MAX_SCALE) {
      res.mir_op.u.mem.index = op2.mir_op.u.reg;
      res.mir_op.u.mem.scale = (MIR_scale_t) size;
    } else {
      op_t temp_op;

      temp_op = get_new_temp (c2m_ctx, MIR_T_I64);
      emit3 (c2m_ctx, MIR_MUL, temp_op.mir_op, op2.mir_op, MIR_new_int_op (ctx, size));
      if (res.mir_op.u.mem.base != 0)
        emit3 (c2m_ctx, MIR_ADD, temp_op.mir_op, temp_op.mir_op,
               MIR_new_reg_op (ctx, res.mir_op.u.mem.base));
      res.mir_op.u.mem.base = temp_op.mir_op.u.reg;
    }
    res.mir_op.u.mem.type = t;
    break;
  }
  case N_LABEL_ADDR: {
    node_t target;

    e = r->attr;
    type = e->type;
    target = e->u.label_addr_target;
    t = get_mir_type (c2m_ctx, type);
    res = get_new_temp (c2m_ctx, t);
    emit2 (c2m_ctx, MIR_LADDR, res.mir_op, MIR_new_label_op (ctx, get_label (c2m_ctx, target)));
    break;
  }
  case N_ADDR: {
    int add_p = FALSE;

    op1 = gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, FALSE, NULL, NULL);
    type = ((struct expr *) r->attr)->type;
    t = get_mir_type (c2m_ctx, type);
    if (op1.mir_op.mode == MIR_OP_REG && type->mode == TM_PTR && scalar_type_p (type->u.ptr_type)) {
      MIR_insn_code_t code;
      res = get_new_temp (c2m_ctx, t);
      switch (get_mir_type (c2m_ctx, type->u.ptr_type)) {
      case MIR_T_I8:
      case MIR_T_U8: code = MIR_ADDR8; break;
      case MIR_T_I16:
      case MIR_T_U16: code = MIR_ADDR16; break;
      case MIR_T_I32:
      case MIR_T_U32: code = MIR_ADDR32; break;
      default: code = MIR_ADDR; break;
      }
      emit2 (c2m_ctx, code, res.mir_op, MIR_new_reg_op (ctx, op1.mir_op.u.reg));
      break;
    } else if (op1.mir_op.mode == MIR_OP_REG || op1.mir_op.mode == MIR_OP_REF
               || op1.mir_op.mode == MIR_OP_STR) { /* array or func */
      res = op1;
      res.decl = NULL;
      break;
    }
    assert (op1.mir_op.mode == MIR_OP_MEM);
    res = get_new_temp (c2m_ctx, t);
    if (op1.mir_op.u.mem.index != 0) {
      emit3 (c2m_ctx, MIR_MUL, res.mir_op, MIR_new_reg_op (ctx, op1.mir_op.u.mem.index),
             MIR_new_int_op (ctx, op1.mir_op.u.mem.scale));
      add_p = TRUE;
    }
    if (op1.mir_op.u.mem.disp != 0) {
      if (add_p)
        emit3 (c2m_ctx, MIR_ADD, res.mir_op, res.mir_op,
               MIR_new_int_op (ctx, op1.mir_op.u.mem.disp));
      else
        emit2 (c2m_ctx, MIR_MOV, res.mir_op, MIR_new_int_op (ctx, op1.mir_op.u.mem.disp));
      add_p = TRUE;
    }
    if (op1.mir_op.u.mem.base != 0) {
      if (add_p)
        emit3 (c2m_ctx, MIR_ADD, res.mir_op, res.mir_op,
               MIR_new_reg_op (ctx, op1.mir_op.u.mem.base));
      else
        emit2 (c2m_ctx, MIR_MOV, res.mir_op, MIR_new_reg_op (ctx, op1.mir_op.u.mem.base));
    }
    break;
  }
  case N_DEREF:
    op1 = val_gen (c2m_ctx, NL_HEAD (r->u.ops));
    op1 = force_reg (c2m_ctx, op1, MIR_T_I64);
    assert (op1.mir_op.mode == MIR_OP_REG);
    if ((type = ((struct expr *) r->attr)->type)->mode == TM_PTR
        && type->u.ptr_type->mode == TM_FUNC && type->func_type_before_adjustment_p) {
      res = op1;
    } else {
      struct expr *op_e = NL_HEAD (r->u.ops)->attr;
      t = get_mir_type (c2m_ctx, type);
      op1.mir_op = MIR_new_alias_mem_op (ctx, t, 0, op1.mir_op.u.reg, 0, 1,
                                         get_type_alias (c2m_ctx, type), op_e->type->antialias);
      res = new_op (NULL, op1.mir_op);
    }
    break;
  case N_FIELD:
  case N_DEREF_FIELD: {
    node_t def_node;
    MIR_alias_t alias;

    e = r->attr;
    def_node = e->u.lvalue_node;
    assert (def_node != NULL && def_node->code == N_MEMBER);
    decl = def_node->attr;
    op1 = gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, r->code == N_DEREF_FIELD, NULL, NULL);
    t = get_mir_type (c2m_ctx, decl->decl_spec.type);
    if (r->code == N_FIELD) {
      assert (op1.mir_op.mode == MIR_OP_MEM);
      alias = (op1.mir_op.u.mem.alias != 0 && MIR_alias_name (ctx, op1.mir_op.u.mem.alias)[0] == 'U'
                 ? op1.mir_op.u.mem.alias
                 : get_type_alias (c2m_ctx, e->type));
      op1.mir_op
        = MIR_new_alias_mem_op (ctx, t, op1.mir_op.u.mem.disp + decl->offset, op1.mir_op.u.mem.base,
                                op1.mir_op.u.mem.index, op1.mir_op.u.mem.scale, alias,
                                decl->decl_spec.type->antialias);
    } else {
      struct expr *left = NL_HEAD (r->u.ops)->attr;
      assert (left->type->mode == TM_PTR);
      op1 = force_reg (c2m_ctx, op1, MIR_T_I64);
      assert (op1.mir_op.mode == MIR_OP_REG);
      op1.mir_op
        = MIR_new_alias_mem_op (ctx, t, decl->offset, op1.mir_op.u.reg, 0, 1,
                                get_type_alias (c2m_ctx, left->type->u.ptr_type->mode == TM_UNION
                                                           ? left->type->u.ptr_type
                                                           : e->type),
                                decl->decl_spec.type->antialias);
    }
    res = new_op (decl, op1.mir_op);
    break;
  }
  case N_COND: {
    node_t cond = NL_HEAD (r->u.ops);
    node_t true_expr = NL_NEXT (cond);
    node_t false_expr = NL_NEXT (true_expr);
    MIR_label_t cond_true_label = MIR_new_label (ctx), cond_false_label = MIR_new_label (ctx);
    MIR_label_t end_label = MIR_new_label (ctx);
    struct type *cond_res_type = ((struct expr *) r->attr)->type;
    op_t addr;
    int void_p = void_type_p (cond_res_type), cond_expect_res;
    mir_size_t size = type_size (c2m_ctx, cond_res_type);

    if (!void_p) t = get_mir_type (c2m_ctx, cond_res_type);
    gen (c2m_ctx, cond, cond_true_label, cond_false_label, FALSE, NULL, &cond_expect_res);
    emit_label_insn_opt (c2m_ctx, cond_true_label);
    op1 = gen (c2m_ctx, true_expr, NULL, NULL, !void_p && t != MIR_T_UNDEF, NULL, NULL);
    if (!void_p) {
      if (t != MIR_T_UNDEF) {
        res = get_new_temp (c2m_ctx, t);
        op1 = cast (c2m_ctx, op1, t, FALSE);
        emit2 (c2m_ctx, tp_mov (t), res.mir_op, op1.mir_op);
      } else if (desirable_dest == NULL) {
        res = get_new_temp (c2m_ctx, MIR_T_I64);
        addr = mem_to_address (c2m_ctx, op1, FALSE);
        emit2 (c2m_ctx, MIR_MOV, res.mir_op, addr.mir_op);
      } else {
        block_move (c2m_ctx, *desirable_dest, op1, size);
        res = *desirable_dest;
      }
    }
    emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
    emit_label_insn_opt (c2m_ctx, cond_false_label);
    op1 = gen (c2m_ctx, false_expr, NULL, NULL, !void_p && t != MIR_T_UNDEF, NULL, NULL);
    if (!void_p) {
      if (t != MIR_T_UNDEF) {
        op1 = cast (c2m_ctx, op1, t, FALSE);
        emit2 (c2m_ctx, tp_mov (t), res.mir_op, op1.mir_op);
      } else if (desirable_dest == NULL) {
        addr = mem_to_address (c2m_ctx, op1, FALSE);
        emit2 (c2m_ctx, MIR_MOV, res.mir_op, addr.mir_op);
        res = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, res.mir_op.u.reg, 0, 1));
      } else {
        block_move (c2m_ctx, res, op1, size);
      }
    }
    emit_label_insn_opt (c2m_ctx, end_label);
    break;
  }
  case N_ALIGNOF:
  case N_SIZEOF:
  case N_EXPR_SIZEOF: assert (FALSE); break;
  case N_CAST:
    assert (!((struct expr *) r->attr)->const_p);
    type = ((struct expr *) r->attr)->type;
    op1 = gen (c2m_ctx, NL_EL (r->u.ops, 1), NULL, NULL, !void_type_p (type), NULL, NULL);
    if (void_type_p (type)) {
      res = op1;
      res.decl = NULL;
      res.mir_op.mode = MIR_OP_UNDEF;
    } else {
      t = get_mir_type (c2m_ctx, type);
      res = cast (c2m_ctx, op1, t, TRUE);
    }
    break;
  case N_COMPOUND_LITERAL: {
    const char *global_name = NULL;
    char buff[50];
    node_t type_name = NL_HEAD (r->u.ops);
    struct expr *expr = r->attr;
    MIR_module_t module = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
    size_t init_start;

    decl = type_name->attr;
    if (decl->scope == top_scope) {
      assert (decl->u.item == NULL);
      _MIR_get_temp_item_name (ctx, module, buff, sizeof (buff));
      global_name = buff;
    }
    init_start = VARR_LENGTH (init_el_t, init_els);
    collect_init_els (c2m_ctx, NULL, &decl->decl_spec.type, NL_EL (r->u.ops, 1),
                      decl->scope == top_scope || decl->decl_spec.linkage == N_STATIC
                        || decl->decl_spec.linkage == N_EXTERN || decl->decl_spec.static_p
                        || decl->decl_spec.thread_local_p,
                      TRUE);
    qsort (VARR_ADDR (init_el_t, init_els) + init_start,
           VARR_LENGTH (init_el_t, init_els) - init_start, sizeof (init_el_t), cmp_init_el);
    if (decl->scope == top_scope || decl->decl_spec.static_p || decl->decl_spec.thread_local_p) {
      var = new_op (decl, MIR_new_ref_op (ctx, NULL));
    } else {
      t = get_mir_type (c2m_ctx, expr->type);
      var = new_op (decl, MIR_new_alias_mem_op (ctx, t, decl->offset,
                                                MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1,
                                                get_type_alias (c2m_ctx, expr->type), 0));
    }
    int local_p
      = decl->scope != top_scope && !decl->decl_spec.static_p && !decl->decl_spec.thread_local_p;
    gen_initializer (c2m_ctx, init_start, var, global_name,
                     (local_p ? raw_type_size : type_size) (c2m_ctx, decl->decl_spec.type),
                     local_p);
    VARR_TRUNC (init_el_t, init_els, init_start);
    if (var.mir_op.mode == MIR_OP_REF) var.mir_op.u.ref = var.decl->u.item;
    res = var;
    break;
  }
  case N_CALL: {
    node_t func = NL_HEAD (r->u.ops), param_list, param, args = NL_EL (r->u.ops, 1), first_arg;
    struct decl_spec *decl_spec;
    size_t ops_start;
    struct expr *call_expr = r->attr, *func_expr;
    struct type *func_type = NULL; /* to remove an uninitialized warning */
    MIR_item_t proto_item;
    MIR_insn_t call_insn, label;
    mir_size_t saved_call_arg_area_offset_before_args, arg_area_offset;
    int va_arg_p = call_expr->builtin_call_p && str_eq_p (func->u.s.s, BUILTIN_VA_ARG);
    int va_start_p = call_expr->builtin_call_p && str_eq_p (func->u.s.s, BUILTIN_VA_START);
    int alloca_p = call_expr->builtin_call_p && str_eq_p (func->u.s.s, ALLOCA);
    int add_overflow_p = call_expr->builtin_call_p && strcmp (func->u.s.s, ADD_OVERFLOW) == 0;
    int sub_overflow_p = call_expr->builtin_call_p && strcmp (func->u.s.s, SUB_OVERFLOW) == 0;
    int mul_overflow_p = call_expr->builtin_call_p && strcmp (func->u.s.s, MUL_OVERFLOW) == 0;
    int expect_p = call_expr->builtin_call_p && strcmp (func->u.s.s, EXPECT) == 0;
    int jcall_p = call_expr->builtin_call_p && strcmp (func->u.s.s, JCALL) == 0;
    int jret_p = call_expr->builtin_call_p && strcmp (func->u.s.s, JRET) == 0;
    int prop_set_p = call_expr->builtin_call_p && strcmp (func->u.s.s, PROP_SET) == 0;
    int prop_eq_p = call_expr->builtin_call_p && strcmp (func->u.s.s, PROP_EQ) == 0;
    int prop_ne_p = call_expr->builtin_call_p && strcmp (func->u.s.s, PROP_NE) == 0;
    int builtin_call_p = alloca_p || va_arg_p || va_start_p, inline_p = FALSE;
    node_t block = NL_EL (curr_func_def->u.ops, 3);
    struct node_scope *ns = block->attr;
    target_arg_info_t arg_info;
    int n, struct_p;

    type = call_expr->type;
    if (add_overflow_p || sub_overflow_p || mul_overflow_p) {
      op1 = val_gen (c2m_ctx, NL_HEAD (args->u.ops));
      op2 = val_gen (c2m_ctx, NL_EL (args->u.ops, 1));
      op3 = val_gen (c2m_ctx, NL_EL (args->u.ops, 2));
      e = NL_EL (args->u.ops, 2)->attr;
      assert (e->type->mode == TM_PTR && standard_integer_type_p (e->type->u.ptr_type));
      t = get_mir_type (c2m_ctx, e->type->u.ptr_type);
      assert (op3.mir_op.mode == MIR_OP_REG);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx,
                                     t == MIR_T_I32 || t == MIR_T_U32
                                       ? (add_overflow_p   ? MIR_ADDOS
                                          : sub_overflow_p ? MIR_SUBOS
                                          : t == MIR_T_I32 ? MIR_MULOS
                                                           : MIR_UMULOS)
                                       : (add_overflow_p   ? MIR_ADDO
                                          : sub_overflow_p ? MIR_SUBO
                                          : t == MIR_T_I64 ? MIR_MULO
                                                           : MIR_UMULO),
                                     MIR_new_mem_op (ctx, t, 0, op3.mir_op.u.reg, 0, 1), op1.mir_op,
                                     op2.mir_op));
      if (true_label != NULL) {
        MIR_op_t lab_op = MIR_new_label_op (ctx, true_label);
        emit1 (c2m_ctx, t == MIR_T_I32 || t == MIR_T_I64 ? MIR_BO : MIR_UBO, lab_op);
        emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, false_label));
      } else {
        label = MIR_new_label (ctx);
        res = get_new_temp (c2m_ctx, MIR_T_I64);
        emit1 (c2m_ctx, t == MIR_T_I32 || t == MIR_T_I64 ? MIR_BO : MIR_UBO,
               MIR_new_label_op (ctx, label));
        emit2 (c2m_ctx, MIR_MOV, res.mir_op, MIR_new_int_op (ctx, 0));
        emit_label_insn_opt (c2m_ctx, label);
      }
      true_label = false_label = NULL;
      break;
    }
    if (expect_p) {
      e = NL_EL (args->u.ops, 1)->attr;
      if (e->const_p && true_label != NULL && expect_res != NULL)
        *expect_res = e->c.u_val == 0 ? -1 : 1;
      res = gen (c2m_ctx, NL_HEAD (args->u.ops), true_label, false_label, val_p, desirable_dest,
                 NULL);
      true_label = false_label = NULL;
      val_p = FALSE;
      break;
    }
    if (jret_p) {
      op1 = val_gen (c2m_ctx, NL_HEAD (args->u.ops));
      emit1 (c2m_ctx, MIR_JRET, op1.mir_op);
      true_label = false_label = NULL;
      val_p = FALSE;
      break;
    }
    if (prop_set_p) {
      op1 = gen (c2m_ctx, NL_HEAD (args->u.ops), NULL, NULL, FALSE, NULL, NULL);
      op2 = val_gen (c2m_ctx, NL_EL (args->u.ops, 1));
      emit2 (c2m_ctx, MIR_PRSET, op1.mir_op, op2.mir_op);
      true_label = false_label = NULL;
      val_p = FALSE;
      break;
    }
    if (prop_eq_p || prop_ne_p) {
      MIR_label_t t_label = true_label, f_label = false_label;
      int make_val_p = t_label == NULL;
      if (make_val_p) {
        t_label = MIR_new_label (ctx);
        f_label = MIR_new_label (ctx);
      }
      node_t arg = NL_HEAD (args->u.ops);
      op1 = gen (c2m_ctx, arg, NULL, NULL, FALSE, NULL, NULL);
      arg = NL_NEXT (arg);
      op2 = val_gen (c2m_ctx, arg);
      emit3 (c2m_ctx, prop_eq_p ? MIR_PRBEQ : MIR_PRBNE, MIR_new_label_op (ctx, t_label),
             op1.mir_op, op2.mir_op);
      emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, f_label));
      if (make_val_p) make_cond_val (c2m_ctx, r, t_label, f_label, &res);
      true_label = false_label = NULL;
      val_p = FALSE;
      break;
    }
    first_arg = NL_HEAD (args->u.ops);
    if (jcall_p) {
      func = NL_HEAD (args->u.ops);
      first_arg = NL_EL (args->u.ops, 1);
      assert (void_type_p (type));
    }
    ops_start = VARR_LENGTH (MIR_op_t, call_ops);
    if (!builtin_call_p || jcall_p) {
      func_expr = func->attr;
      func_type = func_expr->type;
      assert (func_type->mode == TM_PTR && func_type->u.ptr_type->mode == TM_FUNC);
      func_type = func_type->u.ptr_type;
      proto_item = func_type->u.func_type->proto_item;  // ???
      VARR_PUSH (MIR_op_t, call_ops, MIR_new_ref_op (ctx, proto_item));
      op1 = val_gen (c2m_ctx, func);
      if (!jcall_p && op1.mir_op.mode == MIR_OP_REF && func->code == N_ID
          && ((decl_t) func_expr->def_node->attr)->decl_spec.inline_p)
        inline_p = TRUE;
      VARR_PUSH (MIR_op_t, call_ops, op1.mir_op);
    }
    target_init_arg_vars (c2m_ctx, &arg_info);
    arg_area_offset = curr_call_arg_area_offset + ns->size - ns->call_arg_area_size;
    if ((n = target_add_call_res_op (c2m_ctx, type, &arg_info, arg_area_offset)) < 0) {
      /* pass nothing */
    } else if (n == 0) { /* by addr */
      if (!builtin_call_p || jcall_p) update_call_arg_area_offset (c2m_ctx, type, FALSE);
      res = new_op (NULL, VARR_LAST (MIR_op_t, call_ops));
      assert (res.mir_op.mode == MIR_OP_MEM && res.mir_op.u.mem.type == MIR_T_RBLK);
      res.mir_op = MIR_new_mem_op (ctx, MIR_T_UNDEF, 0, res.mir_op.u.mem.base, 0, 1);
      t = MIR_T_I64;
    } else if (type->mode == TM_STRUCT || type->mode == TM_UNION) { /* passed in regs */
      if (!va_arg_p) {
        res = get_new_temp (c2m_ctx, MIR_T_I64);
        emit3 (c2m_ctx, MIR_ADD, res.mir_op,
               MIR_new_reg_op (ctx, MIR_reg (ctx, FP_NAME, curr_func->u.func)),
               MIR_new_int_op (ctx, arg_area_offset));
        if (!builtin_call_p) update_call_arg_area_offset (c2m_ctx, type, FALSE);
        res.mir_op = MIR_new_mem_op (ctx, MIR_T_UNDEF, 0, res.mir_op.u.reg, 0, 1);
        t = MIR_T_I64;
      }
    } else if (n > 0) {
      assert (n == 1);
      t = promote_mir_int_type (get_mir_type (c2m_ctx, type));
      res = new_op (NULL, VARR_LAST (MIR_op_t, call_ops));
    }
    saved_call_arg_area_offset_before_args = curr_call_arg_area_offset;
    if (va_arg_p) {
      op1 = get_new_temp (c2m_ctx, MIR_T_I64);
      op2 = val_gen (c2m_ctx, NL_HEAD (args->u.ops));
      if (op2.mir_op.mode == MIR_OP_MEM) {
#ifndef _WIN32
        if (op2.mir_op.u.mem.type == MIR_T_UNDEF)
#endif
          op2 = mem_to_address (c2m_ctx, op2, FALSE);
      }
      if (type->mode == TM_STRUCT || type->mode == TM_UNION) {
        if (desirable_dest == NULL) {
          res = get_new_temp (c2m_ctx, MIR_T_I64);
          MIR_append_insn (ctx, curr_func,
                           MIR_new_insn (ctx, MIR_MOV, res.mir_op, MIR_new_int_op (ctx, 0)));
        } else {
          assert (desirable_dest->mir_op.mode == MIR_OP_MEM);
          res = mem_to_address (c2m_ctx, *desirable_dest, TRUE);
        }
        MIR_append_insn (ctx, curr_func,
                         MIR_new_insn (ctx, MIR_VA_BLOCK_ARG, res.mir_op, op2.mir_op,
                                       MIR_new_int_op (ctx, type_size (c2m_ctx, type)),
                                       MIR_new_int_op (ctx, target_get_blk_type (c2m_ctx, type)
                                                              - MIR_T_BLK)));
        if (desirable_dest != NULL) res = *desirable_dest;
      } else {
        MIR_append_insn (ctx, curr_func,
                         MIR_new_insn (ctx, MIR_VA_ARG, op1.mir_op, op2.mir_op,
                                       MIR_new_mem_op (ctx, t, 0, 0, 0, 1)));
        op2 = get_new_temp (c2m_ctx, t);
        MIR_append_insn (ctx, curr_func,
                         MIR_new_insn (ctx, tp_mov (t), op2.mir_op,
                                       MIR_new_alias_mem_op (ctx, t, 0, op1.mir_op.u.reg, 0, 1,
                                                             get_type_alias (c2m_ctx, type), 0)));
        if (res.mir_op.mode == MIR_OP_REG) {
          res = op2;
        } else {
          assert (res.mir_op.mode == MIR_OP_MEM);
          res.mir_op.u.mem.base = op2.mir_op.u.reg;
        }
      }
    } else if (va_start_p) {
      op1 = val_gen (c2m_ctx, NL_HEAD (args->u.ops));
      if (op1.mir_op.mode == MIR_OP_MEM) {
#ifndef _WIN32
        if (op1.mir_op.u.mem.type == MIR_T_UNDEF)
#endif
          op1 = mem_to_address (c2m_ctx, op1, FALSE);
      }
      MIR_append_insn (ctx, curr_func, MIR_new_insn (ctx, MIR_VA_START, op1.mir_op));
    } else if (alloca_p) {
      res = get_new_temp (c2m_ctx, t);
      op1 = val_gen (c2m_ctx, NL_HEAD (args->u.ops));
      MIR_append_insn (ctx, curr_func, MIR_new_insn (ctx, MIR_ALLOCA, res.mir_op, op1.mir_op));
    } else {
      param_list = func_type->u.func_type->param_list;
      param = NL_HEAD (param_list->u.ops);
      for (node_t arg = first_arg; arg != NULL; arg = NL_NEXT (arg)) {
        struct type *arg_type;
        e = arg->attr;
        struct_p = e->type->mode == TM_STRUCT || e->type->mode == TM_UNION;
        op2 = gen (c2m_ctx, arg, NULL, NULL, !struct_p, NULL, NULL);
        assert (param != NULL || NL_HEAD (param_list->u.ops) == NULL
                || func_type->u.func_type->dots_p);
        arg_type = e->type;
        if (struct_p) {
        } else if (param != NULL) {
          assert (param->code == N_SPEC_DECL || param->code == N_TYPE);
          decl_spec = get_param_decl_spec (param);
          arg_type = decl_spec->type;
          t = get_mir_type (c2m_ctx, arg_type);
          t = promote_mir_int_type (t);
          op2 = promote (c2m_ctx, op2, t, FALSE);
        } else {
          t = get_mir_type (c2m_ctx, e->type);
          t = promote_mir_int_type (t);
          op2 = promote (c2m_ctx, op2, t == MIR_T_F ? MIR_T_D : t, FALSE);
        }
        target_add_call_arg_op (c2m_ctx, arg_type, &arg_info, op2);
        if (param != NULL) param = NL_NEXT (param);
      }
      call_insn = MIR_new_insn_arr (ctx,
                                    (jcall_p    ? MIR_JCALL
                                     : inline_p ? MIR_INLINE
                                                : MIR_CALL),
                                    VARR_LENGTH (MIR_op_t, call_ops) - ops_start,
                                    VARR_ADDR (MIR_op_t, call_ops) + ops_start);
      MIR_append_insn (ctx, curr_func, call_insn);
      res = target_gen_post_call_res_code (c2m_ctx, func_type->u.func_type->ret_type, res,
                                           call_insn, ops_start);
    }
    curr_call_arg_area_offset = saved_call_arg_area_offset_before_args;
    VARR_TRUNC (MIR_op_t, call_ops, ops_start);
    break;
  }
  case N_GENERIC: {
    node_t list = NL_EL (r->u.ops, 1);
    node_t ga_case = NL_HEAD (list->u.ops);

    /* first element is now a compatible generic association case */
    op1 = val_gen (c2m_ctx, NL_EL (ga_case->u.ops, 1));
    t = get_mir_type (c2m_ctx, ((struct expr *) r->attr)->type);
    res = promote (c2m_ctx, op1, t, TRUE);
    break;
  }
  case N_SPEC_DECL: {  // ??? export and defintion with external declaration
    node_t specs = NL_HEAD (r->u.ops);
    node_t declarator = NL_NEXT (specs);
    node_t attrs = NL_NEXT (declarator);
    node_t asm_part = NL_NEXT (attrs);
    node_t initializer = NL_NEXT (asm_part);
    node_t id, curr_node;
    symbol_t sym;
    decl_t curr_decl;
    size_t i, init_start;
    const char *name;

    decl = (decl_t) r->attr;
    if (declarator != NULL && declarator->code != N_IGNORE && decl->u.item == NULL) {
      id = NL_HEAD (declarator->u.ops);
      name = (decl->scope != top_scope && decl->decl_spec.static_p
                ? get_func_static_var_name (c2m_ctx, id->u.s.s, decl)
                : id->u.s.s);
      if (decl->asm_p) {
      } else if (decl->used_p && decl->scope != top_scope && decl->decl_spec.linkage == N_STATIC) {
        decl->u.item = MIR_new_forward (ctx, name);
        move_item_forward (c2m_ctx, decl->u.item);
      } else if (decl->used_p && decl->decl_spec.linkage != N_IGNORE) {
        if (symbol_find (c2m_ctx, S_REGULAR, id,
                         decl->decl_spec.linkage == N_EXTERN ? top_scope : decl->scope, &sym)
            && (decl->u.item = get_ref_item (c2m_ctx, sym.def_node, name)) == NULL) {
          for (i = 0; i < VARR_LENGTH (node_t, sym.defs); i++)
            if ((decl->u.item = get_ref_item (c2m_ctx, VARR_GET (node_t, sym.defs, i), name))
                != NULL)
              break;
        }
        if (decl->u.item == NULL) decl->u.item = MIR_new_import (ctx, name);
        if (decl->scope != top_scope) move_item_forward (c2m_ctx, decl->u.item);
      }
      if (declarator->code == N_DECL && decl->decl_spec.type->mode != TM_FUNC
          && !decl->decl_spec.typedef_p && !decl->decl_spec.extern_p && !decl->asm_p) {
        if (initializer->code == N_IGNORE) {
          if (decl->scope != top_scope && decl->decl_spec.static_p) {
            decl->u.item = MIR_new_bss (ctx, name, raw_type_size (c2m_ctx, decl->decl_spec.type));
          } else if (decl->scope == top_scope
                     && symbol_find (c2m_ctx, S_REGULAR, id, top_scope, &sym)
                     && ((curr_decl = sym.def_node->attr)->u.item == NULL
                         || curr_decl->u.item->item_type != MIR_bss_item)) {
            for (i = 0; i < VARR_LENGTH (node_t, sym.defs); i++) {
              curr_node = VARR_GET (node_t, sym.defs, i);
              curr_decl = curr_node->attr;
              if ((curr_decl->u.item != NULL && curr_decl->u.item->item_type == MIR_bss_item)
                  || NL_EL (curr_node->u.ops, 4)->code != N_IGNORE)
                break;
            }
            if (i >= VARR_LENGTH (node_t, sym.defs)) /* No item yet or no decl with intializer: */
              decl->u.item = MIR_new_bss (ctx, name, raw_type_size (c2m_ctx, decl->decl_spec.type));
          }
        } else if (initializer->code != N_IGNORE) {  // ??? general code
          init_start = VARR_LENGTH (init_el_t, init_els);
          collect_init_els (c2m_ctx, NULL, &decl->decl_spec.type, initializer,
                            decl->decl_spec.linkage == N_STATIC
                              || decl->decl_spec.linkage == N_EXTERN || decl->decl_spec.static_p
                              || decl->decl_spec.thread_local_p,
                            TRUE);
          qsort (VARR_ADDR (init_el_t, init_els) + init_start,
                 VARR_LENGTH (init_el_t, init_els) - init_start, sizeof (init_el_t), cmp_init_el);
          if (id->attr == NULL) {
            node_t saved_scope = curr_scope;

            curr_scope = decl->scope;
            check (c2m_ctx, id, NULL);
            curr_scope = saved_scope;
          }
          if (decl->scope == top_scope || decl->decl_spec.static_p
              || decl->decl_spec.thread_local_p) {
            var = new_op (decl, MIR_new_ref_op (ctx, NULL));
          } else {
            var = gen (c2m_ctx, id, NULL, NULL, FALSE, NULL, NULL);
            assert (var.decl != NULL
                    && (var.mir_op.mode == MIR_OP_REG
                        || (var.mir_op.mode == MIR_OP_MEM && var.mir_op.u.mem.index == 0)));
          }
          int local_p = (decl->scope != top_scope && !decl->decl_spec.static_p
                         && !decl->decl_spec.thread_local_p);
          gen_initializer (c2m_ctx, init_start, var, name,
                           (local_p ? raw_type_size : type_size) (c2m_ctx, decl->decl_spec.type),
                           local_p);
          VARR_TRUNC (init_el_t, init_els, init_start);
        }
        if (decl->u.item != NULL && decl->scope == top_scope && !decl->decl_spec.static_p) {
          MIR_new_export (ctx, name);
        } else if (decl->u.item != NULL && decl->scope != top_scope && decl->decl_spec.static_p) {
          MIR_item_t item = MIR_new_forward (ctx, name);

          move_item_forward (c2m_ctx, item);
        }
      }
    }
    break;
  }
  case N_ST_ASSERT: /* do nothing */ break;
  case N_INIT: break;  // ???
  case N_FUNC_DEF: {
    node_t decl_specs = NL_HEAD (r->u.ops);
    node_t declarator = NL_NEXT (decl_specs);
    node_t decls = NL_NEXT (declarator);
    node_t stmt = NL_NEXT (decls);
    struct node_scope *ns = stmt->attr;
    decl_t param_decl, func_decl = r->attr;
    struct type *decl_type = func_decl->decl_spec.type;
    node_t first_param, param, param_declarator, param_id;
    struct type *param_type;
    MIR_insn_t insn;
    MIR_type_t res_type, param_mir_type;
    MIR_reg_t fp_reg, param_reg;
    target_arg_info_t arg_info;
    const char *name;

    assert (declarator != NULL && declarator->code == N_DECL
            && NL_HEAD (declarator->u.ops)->code == N_ID);
    assert (decl_type->mode == TM_FUNC);
    reg_free_mark = 0;
    curr_func_def = r;
    curr_call_arg_area_offset = 0;
    collect_args_and_func_types (c2m_ctx, decl_type->u.func_type);
    curr_func = ((decl_type->u.func_type->dots_p
                    ? MIR_new_vararg_func_arr
                    : MIR_new_func_arr) (ctx, NL_HEAD (declarator->u.ops)->u.s.s,
                                         VARR_LENGTH (MIR_type_t, proto_info.ret_types),
                                         VARR_ADDR (MIR_type_t, proto_info.ret_types),
                                         VARR_LENGTH (MIR_var_t, proto_info.arg_vars),
                                         VARR_ADDR (MIR_var_t, proto_info.arg_vars)));
    func_decl->u.item = curr_func;
    DLIST_INIT (MIR_insn_t, slow_code_part);
    if (ns->stack_var_p /* we can have empty struct only with size 0 and still need a frame: */
        || ns->size > 0) {
      fp_reg = MIR_new_func_reg (ctx, curr_func->u.func, MIR_T_I64, FP_NAME);
      MIR_append_insn (ctx, curr_func,
                       MIR_new_insn (ctx, MIR_ALLOCA, MIR_new_reg_op (ctx, fp_reg),
                                     MIR_new_int_op (ctx, ns->size)));
    }
    for (size_t i = 0; i < VARR_LENGTH (MIR_var_t, proto_info.arg_vars); i++)
      get_reg_var (c2m_ctx, MIR_T_UNDEF, VARR_GET (MIR_var_t, proto_info.arg_vars, i).name, NULL);
    target_init_arg_vars (c2m_ctx, &arg_info);
    if ((first_param = NL_HEAD (decl_type->u.func_type->param_list->u.ops)) != NULL
        && !void_param_p (first_param)) {
      for (param = first_param; param != NULL; param = NL_NEXT (param)) {
        param_declarator = NL_EL (param->u.ops, 1);
        assert (param_declarator != NULL && param_declarator->code == N_DECL);
        param_decl = param->attr;
        param_id = NL_HEAD (param_declarator->u.ops);
        param_type = param_decl->decl_spec.type;
        assert (!param_decl->reg_p
                || (param_type->mode != TM_STRUCT && param_type->mode != TM_UNION));
        name = get_param_name (c2m_ctx, param_type, param_id->u.s.s);
        if (target_gen_gather_arg (c2m_ctx, name, param_type, param_decl, &arg_info)) continue;
        if (param_decl->reg_p) continue;
        if (param_type->mode == TM_STRUCT
            || param_type->mode == TM_UNION) { /* ??? only block pass */
          param_reg = get_reg_var (c2m_ctx, MIR_POINTER_TYPE, name, NULL).reg;
          val = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_UNDEF, 0, param_reg, 0, 1));
          var
            = new_op (param_decl, MIR_new_mem_op (ctx, MIR_T_UNDEF, param_decl->offset,
                                                  MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1));
          block_move (c2m_ctx, var, val, type_size (c2m_ctx, param_type));
        } else {
          param_mir_type = get_mir_type (c2m_ctx, param_type);
          emit2 (c2m_ctx, tp_mov (param_mir_type),
                 MIR_new_alias_mem_op (ctx, param_mir_type, param_decl->offset,
                                       MIR_reg (ctx, FP_NAME, curr_func->u.func), 0, 1,
                                       get_type_alias (c2m_ctx, param_type), 0),
                 MIR_new_reg_op (ctx, get_reg_var (c2m_ctx, MIR_T_UNDEF, name, NULL).reg));
        }
      }
    }
    gen (c2m_ctx, stmt, NULL, NULL, FALSE, NULL, NULL);
    if ((insn = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns)) == NULL
        || (insn->code != MIR_RET && insn->code != MIR_JRET && insn->code != MIR_JMP)) {
      if (VARR_LENGTH (MIR_type_t, proto_info.ret_types) == 0) {
        if (jump_ret_p)
          emit1 (c2m_ctx, MIR_JRET, MIR_new_int_op (ctx, 0));
        else
          emit_insn (c2m_ctx, MIR_new_ret_insn (ctx, 0));
      } else {
        VARR_TRUNC (MIR_op_t, ret_ops, 0);
        for (size_t i = 0; i < VARR_LENGTH (MIR_type_t, proto_info.ret_types); i++) {
          res_type = VARR_GET (MIR_type_t, proto_info.ret_types, i);
          if (res_type == MIR_T_D) {
            VARR_PUSH (MIR_op_t, ret_ops, MIR_new_double_op (ctx, 0.0));
          } else if (res_type == MIR_T_LD) {
            VARR_PUSH (MIR_op_t, ret_ops, MIR_new_ldouble_op (ctx, 0.0));
          } else if (res_type == MIR_T_F) {
            VARR_PUSH (MIR_op_t, ret_ops, MIR_new_float_op (ctx, 0.0));
          } else {
            VARR_PUSH (MIR_op_t, ret_ops, MIR_new_int_op (ctx, 0));
          }
        }
        emit_insn (c2m_ctx, MIR_new_insn_arr (ctx, MIR_RET, VARR_LENGTH (MIR_op_t, ret_ops),
                                              VARR_ADDR (MIR_op_t, ret_ops)));
      }
    }
    while ((insn = DLIST_HEAD (MIR_insn_t, slow_code_part)) != NULL) {
      DLIST_REMOVE (MIR_insn_t, slow_code_part, insn);
      DLIST_APPEND (MIR_insn_t, curr_func->u.func->insns, insn);
    }
    MIR_finish_func (ctx);
    if (func_decl->decl_spec.linkage == N_EXTERN)
      MIR_new_export (ctx, NL_HEAD (declarator->u.ops)->u.s.s);
    finish_curr_func_reg_vars (c2m_ctx);
    break;
  }
  case N_STMTEXPR: {
    gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, FALSE, NULL, NULL);
    res = top_gen_last_op;
    break;
  }
  case N_BLOCK:
    emit_label (c2m_ctx, r);
    gen (c2m_ctx, NL_EL (r->u.ops, 1), NULL, NULL, FALSE, NULL, NULL);
    break;
  case N_MODULE: gen (c2m_ctx, NL_HEAD (r->u.ops), NULL, NULL, FALSE, NULL, NULL); break;  // ???
  case N_IF: {
    node_t expr = NL_EL (r->u.ops, 1);
    node_t if_stmt = NL_NEXT (expr);
    node_t else_stmt = NL_NEXT (if_stmt);
    MIR_label_t if_label = MIR_new_label (ctx), else_label = MIR_new_label (ctx);
    MIR_label_t end_label = MIR_new_label (ctx);
    int cond_expect_res;
    MIR_insn_t insn, last = NULL;

    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    top_gen (c2m_ctx, expr, if_label, else_label, &cond_expect_res);
    if (cond_expect_res == 0 || cond_expect_res == 1) { /* fall through on true */
      emit_label_insn_opt (c2m_ctx, if_label);
      gen (c2m_ctx, if_stmt, NULL, NULL, FALSE, NULL, NULL);
      if (cond_expect_res == 0) {
        emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, end_label));
        emit_label_insn_opt (c2m_ctx, else_label);
        gen (c2m_ctx, else_stmt, NULL, NULL, FALSE, NULL, NULL);
      } else {
        last = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns);
        MIR_append_insn (ctx, curr_func, else_label);
        gen (c2m_ctx, else_stmt, NULL, NULL, FALSE, NULL, NULL);
      }
    } else { /* fall on false */
      emit_label_insn_opt (c2m_ctx, else_label);
      gen (c2m_ctx, else_stmt, NULL, NULL, FALSE, NULL, NULL);
      last = DLIST_TAIL (MIR_insn_t, curr_func->u.func->insns);
      MIR_append_insn (ctx, curr_func, if_label);
      gen (c2m_ctx, if_stmt, NULL, NULL, FALSE, NULL, NULL);
    }
    if (last != NULL) { /* move to slow part of code */
      while ((insn = DLIST_NEXT (MIR_insn_t, last)) != NULL) {
        DLIST_REMOVE (MIR_insn_t, curr_func->u.func->insns, insn);
        DLIST_APPEND (MIR_insn_t, slow_code_part, insn);
      }
      DLIST_APPEND (MIR_insn_t, slow_code_part,
                    MIR_new_insn (ctx, MIR_JMP, MIR_new_label_op (ctx, end_label)));
    }
    emit_label_insn_opt (c2m_ctx, end_label);
    break;
  }
  case N_SWITCH: {
    node_t expr = NL_EL (r->u.ops, 1);
    node_t stmt = NL_NEXT (expr);
    struct switch_attr *switch_attr = r->attr;
    op_t case_reg_op;
    struct expr *e2;
    case_t c;
    MIR_label_t saved_break_label = break_label;
    int signed_p, short_p;
    size_t len;
    mir_ullong range = 0;

    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    break_label = MIR_new_label (ctx);
    case_reg_op = val_gen (c2m_ctx, expr);
    type = ((struct expr *) expr->attr)->type;
    signed_p = signed_integer_type_p (type);
    mir_type = get_mir_type (c2m_ctx, type);
    short_p = mir_type != MIR_T_I64 && mir_type != MIR_T_U64;
    case_reg_op = force_reg (c2m_ctx, case_reg_op, mir_type);
    if (switch_attr->min_val_case != NULL) {
      e = NL_HEAD (switch_attr->min_val_case->case_node->u.ops)->attr;
      e2 = NL_HEAD (switch_attr->max_val_case->case_node->u.ops)->attr;
      range = signed_p ? (mir_ullong) (e2->c.i_val - e->c.i_val) : e2->c.u_val - e->c.u_val;
    }
    len = DLIST_LENGTH (case_t, switch_attr->case_labels);
    if (!switch_attr->ranges_p && len > 4 && range != 0 && range / len < 3) { /* use MIR_SWITCH */
      mir_ullong curr_val, prev_val, n;
      op_t index = get_new_temp (c2m_ctx, MIR_T_I64);
      MIR_label_t label = break_label;

      c = DLIST_TAIL (case_t, switch_attr->case_labels);
      if (c->case_node->code == N_DEFAULT) {
        assert (DLIST_NEXT (case_t, c) == NULL);
        label = get_label (c2m_ctx, c->case_target_node);
      }
      emit3 (c2m_ctx, short_p ? MIR_SUBS : MIR_SUB, index.mir_op, case_reg_op.mir_op,
             signed_p ? MIR_new_int_op (ctx, e->c.i_val) : MIR_new_uint_op (ctx, e->c.u_val));
      emit3 (c2m_ctx, short_p ? MIR_UBGTS : MIR_UBGT, MIR_new_label_op (ctx, label), index.mir_op,
             MIR_new_uint_op (ctx, range));
      if (short_p) emit2 (c2m_ctx, MIR_UEXT32, index.mir_op, index.mir_op);
      VARR_TRUNC (case_t, switch_cases, 0);
      for (c = DLIST_HEAD (case_t, switch_attr->case_labels);
           c != NULL && c->case_node->code != N_DEFAULT; c = DLIST_NEXT (case_t, c))
        VARR_PUSH (case_t, switch_cases, c);
      qsort (VARR_ADDR (case_t, switch_cases), VARR_LENGTH (case_t, switch_cases), sizeof (case_t),
             signed_p ? signed_case_compare : unsigned_case_compare);
      VARR_TRUNC (MIR_op_t, switch_ops, 0);
      VARR_PUSH (MIR_op_t, switch_ops, index.mir_op);
      for (size_t i = 0; i < VARR_LENGTH (case_t, switch_cases); i++) {
        c = VARR_GET (case_t, switch_cases, i);
        e2 = NL_HEAD (c->case_node->u.ops)->attr;
        curr_val = signed_p ? (mir_ullong) (e2->c.i_val - e->c.i_val) : e2->c.u_val - e->c.u_val;
        if (i != 0) {
          for (n = prev_val + 1; n < curr_val; n++)
            VARR_PUSH (MIR_op_t, switch_ops, MIR_new_label_op (ctx, label));
        }
        VARR_PUSH (MIR_op_t, switch_ops,
                   MIR_new_label_op (ctx, get_label (c2m_ctx, c->case_target_node)));
        prev_val = curr_val;
      }
      emit_insn (c2m_ctx, MIR_new_insn_arr (ctx, MIR_SWITCH, VARR_LENGTH (MIR_op_t, switch_ops),
                                            VARR_ADDR (MIR_op_t, switch_ops)));
    } else {
      for (c = DLIST_HEAD (case_t, switch_attr->case_labels); c != NULL;
           c = DLIST_NEXT (case_t, c)) {
        MIR_label_t cont_label, label = get_label (c2m_ctx, c->case_target_node);
        node_t case_expr, case_expr2;

        if (c->case_node->code == N_DEFAULT) {
          assert (DLIST_NEXT (case_t, c) == NULL);
          emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, label));
          break;
        }
        case_expr = NL_HEAD (c->case_node->u.ops);
        case_expr2 = NL_NEXT (case_expr);
        e = case_expr->attr;
        assert (e->const_p && integer_type_p (e->type));
        if (case_expr2 == NULL) {
          emit3 (c2m_ctx, short_p ? MIR_BEQS : MIR_BEQ, MIR_new_label_op (ctx, label),
                 case_reg_op.mir_op, MIR_new_int_op (ctx, e->c.i_val));
        } else {
          e2 = case_expr2->attr;
          assert (e2->const_p && integer_type_p (e2->type));
          cont_label = MIR_new_label (ctx);
          if (signed_p) {
            emit3 (c2m_ctx, short_p ? MIR_BLTS : MIR_BLT, MIR_new_label_op (ctx, cont_label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e->c.i_val));
            emit3 (c2m_ctx, short_p ? MIR_BLES : MIR_BLE, MIR_new_label_op (ctx, label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e2->c.i_val));
          } else {
            emit3 (c2m_ctx, short_p ? MIR_UBLTS : MIR_UBLT, MIR_new_label_op (ctx, cont_label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e->c.i_val));
            emit3 (c2m_ctx, short_p ? MIR_UBLES : MIR_UBLE, MIR_new_label_op (ctx, label),
                   case_reg_op.mir_op, MIR_new_int_op (ctx, e2->c.i_val));
          }
          emit_label_insn_opt (c2m_ctx, cont_label);
        }
      }
      if (c == NULL) /* no default: */
        emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, break_label));
    }
    top_gen (c2m_ctx, stmt, NULL, NULL, NULL);
    emit_label_insn_opt (c2m_ctx, break_label);
    break_label = saved_break_label;
    break;
  }
  case N_DO: {
    node_t expr = NL_EL (r->u.ops, 1);
    node_t stmt = NL_NEXT (expr);
    MIR_label_t saved_continue_label = continue_label, saved_break_label = break_label;
    MIR_label_t start_label = MIR_new_label (ctx);

    assert (false_label == NULL && true_label == NULL);
    continue_label = MIR_new_label (ctx);
    break_label = MIR_new_label (ctx);
    emit_label (c2m_ctx, r);
    emit_label_insn_opt (c2m_ctx, start_label);
    gen (c2m_ctx, stmt, NULL, NULL, FALSE, NULL, NULL);
    emit_label_insn_opt (c2m_ctx, continue_label);
    top_gen (c2m_ctx, expr, start_label, break_label, NULL);
    emit_label_insn_opt (c2m_ctx, break_label);
    continue_label = saved_continue_label;
    break_label = saved_break_label;
    break;
  }
  case N_WHILE: {
    node_t expr = NL_EL (r->u.ops, 1);
    node_t stmt = NL_NEXT (expr);
    MIR_label_t stmt_label = MIR_new_label (ctx);
    MIR_label_t saved_continue_label = continue_label, saved_break_label = break_label;

    assert (false_label == NULL && true_label == NULL);
    continue_label = MIR_new_label (ctx);
    break_label = MIR_new_label (ctx);
    emit_label (c2m_ctx, r);
    emit_label_insn_opt (c2m_ctx, continue_label);
    top_gen (c2m_ctx, expr, stmt_label, break_label, NULL);
    emit_label_insn_opt (c2m_ctx, stmt_label);
    gen (c2m_ctx, stmt, NULL, NULL, FALSE, NULL, NULL);
    top_gen (c2m_ctx, expr, stmt_label, break_label, NULL);
    emit_label_insn_opt (c2m_ctx, break_label);
    continue_label = saved_continue_label;
    break_label = saved_break_label;
    break;
  }
  case N_FOR: {
    node_t init = NL_EL (r->u.ops, 1);
    node_t cond = NL_NEXT (init);
    node_t iter = NL_NEXT (cond);
    node_t stmt = NL_NEXT (iter);
    MIR_label_t stmt_label = MIR_new_label (ctx);
    MIR_label_t saved_continue_label = continue_label, saved_break_label = break_label;

    assert (false_label == NULL && true_label == NULL);
    continue_label = MIR_new_label (ctx);
    break_label = MIR_new_label (ctx);
    emit_label (c2m_ctx, r);
    top_gen (c2m_ctx, init, NULL, NULL, NULL);
    if (cond->code != N_IGNORE) /* non-empty condition: */
      top_gen (c2m_ctx, cond, stmt_label, break_label, NULL);
    emit_label_insn_opt (c2m_ctx, stmt_label);
    gen (c2m_ctx, stmt, NULL, NULL, FALSE, NULL, NULL);
    emit_label_insn_opt (c2m_ctx, continue_label);
    top_gen (c2m_ctx, iter, NULL, NULL, NULL);
    if (cond->code == N_IGNORE) { /* empty condition: */
      emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, stmt_label));
    } else {
      top_gen (c2m_ctx, cond, stmt_label, break_label, NULL);
    }
    emit_label_insn_opt (c2m_ctx, break_label);
    continue_label = saved_continue_label;
    break_label = saved_break_label;
    break;
  }
  case N_GOTO: {
    node_t target = r->attr;

    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, get_label (c2m_ctx, target)));
    break;
  }
  case N_INDIRECT_GOTO: {
    node_t expr = NL_EL (r->u.ops, 1);

    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    val = val_gen (c2m_ctx, expr);
    emit1 (c2m_ctx, MIR_JMPI, val.mir_op);
    break;
  }
  case N_CONTINUE:
    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, continue_label));
    break;
  case N_BREAK:
    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, break_label));
    break;
  case N_RETURN: {
    decl_t func_decl = curr_func_def->attr;
    struct type *func_type = func_decl->decl_spec.type;
    struct type *ret_type = func_type->u.func_type->ret_type;
    int scalar_p = ret_type->mode != TM_STRUCT && ret_type->mode != TM_UNION;
    int ret_by_addr_p = target_return_by_addr_p (c2m_ctx, ret_type);

    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    if (NL_EL (r->u.ops, 1)->code == N_IGNORE) {
      emit_insn (c2m_ctx, MIR_new_ret_insn (ctx, 0));
      break;
    }
    if (ret_by_addr_p) {
      MIR_reg_t ret_addr_reg = MIR_reg (ctx, RET_ADDR_NAME, curr_func->u.func);

      var = new_op (NULL, MIR_new_mem_op (ctx, MIR_T_I8, 0, ret_addr_reg, 0, 1));
    }
    val = gen (c2m_ctx, NL_EL (r->u.ops, 1), NULL, NULL, !ret_by_addr_p && scalar_p,
               !ret_by_addr_p || scalar_p ? NULL : &var, NULL);
    if (!ret_by_addr_p && scalar_p) {
      t = get_mir_type (c2m_ctx, ret_type);
      t = promote_mir_int_type (t);
      val = promote (c2m_ctx, val, t, FALSE);
    }
    VARR_TRUNC (MIR_op_t, ret_ops, 0);
    target_add_ret_ops (c2m_ctx, func_type->u.func_type->ret_type, val);
    emit_insn (c2m_ctx, MIR_new_insn_arr (ctx, MIR_RET, VARR_LENGTH (MIR_op_t, ret_ops),
                                          VARR_ADDR (MIR_op_t, ret_ops)));
    break;
  }
  case N_EXPR:
    assert (false_label == NULL && true_label == NULL);
    emit_label (c2m_ctx, r);
    top_gen (c2m_ctx, NL_EL (r->u.ops, 1), NULL, NULL, NULL);
    break;
  default: abort ();
  }
finish:
  if (true_label != NULL) {
    MIR_op_t lab_op = MIR_new_label_op (ctx, true_label);

    type = ((struct expr *) r->attr)->type;
    if (!floating_type_p (type)) {
      res = promote (c2m_ctx, force_val (c2m_ctx, res, type->arr_type != NULL), MIR_T_I64, FALSE);
      emit2 (c2m_ctx, MIR_BT, lab_op, res.mir_op);
    } else if (type->u.basic_type == TP_FLOAT) {
      emit3 (c2m_ctx, MIR_FBNE, lab_op, res.mir_op, MIR_new_float_op (ctx, 0.0));
    } else if (type->u.basic_type == TP_DOUBLE) {
      emit3 (c2m_ctx, MIR_DBNE, lab_op, res.mir_op, MIR_new_double_op (ctx, 0.0));
    } else {
      assert (type->u.basic_type == TP_LDOUBLE);
      emit3 (c2m_ctx, MIR_LDBNE, lab_op, res.mir_op, MIR_new_ldouble_op (ctx, 0.0));
    }
    emit1 (c2m_ctx, MIR_JMP, MIR_new_label_op (ctx, false_label));
  } else if (val_p) {
    res = force_val (c2m_ctx, res, ((struct expr *) r->attr)->type->arr_type != NULL);
  }
  if (stmt_p) curr_call_arg_area_offset = 0;
  return res;
}

static htab_hash_t proto_hash (MIR_item_t pi, void *arg MIR_UNUSED) {
  MIR_proto_t p = pi->u.proto;
  MIR_var_t *args = VARR_ADDR (MIR_var_t, p->args);
  uint64_t h = mir_hash_init (42);

  h = mir_hash_step (h, p->nres);
  h = mir_hash_step (h, p->vararg_p);
  for (uint32_t i = 0; i < p->nres; i++) h = mir_hash_step (h, p->res_types[i]);
  for (size_t i = 0; i < VARR_LENGTH (MIR_var_t, p->args); i++) {
    h = mir_hash_step (h, args[i].type);
    h = mir_hash_step (h, mir_hash (args[i].name, strlen (args[i].name), 24));
    if (MIR_all_blk_type_p (args[i].type)) h = mir_hash_step (h, args[i].size);
  }
  return (htab_hash_t) mir_hash_finish (h);
}

static int proto_eq (MIR_item_t pi1, MIR_item_t pi2, void *arg MIR_UNUSED) {
  MIR_proto_t p1 = pi1->u.proto, p2 = pi2->u.proto;

  if (p1->nres != p2->nres || p1->vararg_p != p2->vararg_p
      || VARR_LENGTH (MIR_var_t, p1->args) != VARR_LENGTH (MIR_var_t, p2->args))
    return FALSE;
  for (uint32_t i = 0; i < p1->nres; i++)
    if (p1->res_types[i] != p2->res_types[i]) return FALSE;

  MIR_var_t *args1 = VARR_ADDR (MIR_var_t, p1->args), *args2 = VARR_ADDR (MIR_var_t, p2->args);

  for (size_t i = 0; i < VARR_LENGTH (MIR_var_t, p1->args); i++)
    if (args1[i].type != args2[i].type || strcmp (args1[i].name, args2[i].name) != 0
        || (MIR_all_blk_type_p (args1[i].type) && args1[i].size != args2[i].size))
      return FALSE;
  return TRUE;
}

static MIR_item_t get_mir_proto (c2m_ctx_t c2m_ctx, int vararg_p) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  struct MIR_item pi, *el;
  struct MIR_proto p;
  char buff[30];

  pi.u.proto = &p;
  p.vararg_p = vararg_p;
  p.nres = (uint32_t) VARR_LENGTH (MIR_type_t, proto_info.ret_types);
  p.res_types = VARR_ADDR (MIR_type_t, proto_info.ret_types);
  p.args = proto_info.arg_vars;
  if (HTAB_DO (MIR_item_t, proto_tab, &pi, HTAB_FIND, el)) return el;
  sprintf (buff, "proto%d", curr_mir_proto_num++);
  el = (vararg_p ? MIR_new_vararg_proto_arr
                 : MIR_new_proto_arr) (c2m_ctx->ctx, buff, p.nres, p.res_types,
                                       VARR_LENGTH (MIR_var_t, proto_info.arg_vars),
                                       VARR_ADDR (MIR_var_t, proto_info.arg_vars));
  HTAB_DO (MIR_item_t, proto_tab, el, HTAB_INSERT, el);
  return el;
}

static void gen_mir_protos (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  node_t call, func, op1;
  struct type *type;
  struct func_type *func_type;

  curr_mir_proto_num = 0;
  HTAB_CREATE (MIR_item_t, proto_tab, alloc, 512, proto_hash, proto_eq, NULL);
  for (size_t i = 0; i < VARR_LENGTH (node_t, call_nodes); i++) {
    call = VARR_GET (node_t, call_nodes, i);
    assert (call->code == N_CALL);
    op1 = NL_HEAD (call->u.ops);
    if (op1->code == N_ID && strcmp (op1->u.s.s, JCALL) == 0)
      func = NL_HEAD (NL_NEXT (op1)->u.ops);
    else
      func = NL_HEAD (call->u.ops);
    type = ((struct expr *) func->attr)->type;
    assert (type->mode == TM_PTR && type->u.ptr_type->mode == TM_FUNC);
    set_type_layout (c2m_ctx, type);
    func_type = type->u.ptr_type->u.func_type;
    assert (func_type->param_list->code == N_LIST);
    collect_args_and_func_types (c2m_ctx, func_type);
    func_type->proto_item
      = get_mir_proto (c2m_ctx,
                       func_type->dots_p || NL_HEAD (func_type->param_list->u.ops) == NULL);
  }
  HTAB_DESTROY (MIR_item_t, proto_tab);
}

static void gen_finish (c2m_ctx_t c2m_ctx) {
  gen_ctx_t gen_ctx;

  if (c2m_ctx == NULL || (gen_ctx = c2m_ctx->gen_ctx) == NULL) return;
  finish_reg_vars (c2m_ctx);
  if (proto_info.arg_vars != NULL) VARR_DESTROY (MIR_var_t, proto_info.arg_vars);
  if (proto_info.ret_types != NULL) VARR_DESTROY (MIR_type_t, proto_info.ret_types);
  if (call_ops != NULL) VARR_DESTROY (MIR_op_t, call_ops);
  if (ret_ops != NULL) VARR_DESTROY (MIR_op_t, ret_ops);
  if (switch_ops != NULL) VARR_DESTROY (MIR_op_t, switch_ops);
  if (switch_cases != NULL) VARR_DESTROY (case_t, switch_cases);
  if (init_els != NULL) VARR_DESTROY (init_el_t, init_els);
  if (node_stack != NULL) VARR_DESTROY (node_t, node_stack);
  free (c2m_ctx->gen_ctx);
}

static void gen_mir (c2m_ctx_t c2m_ctx, node_t r) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  gen_ctx_t gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;

  c2m_ctx->gen_ctx = gen_ctx = c2mir_calloc (c2m_ctx, sizeof (struct gen_ctx));
  zero_op = new_op (NULL, MIR_new_int_op (ctx, 0));
  one_op = new_op (NULL, MIR_new_int_op (ctx, 1));
  minus_one_op = new_op (NULL, MIR_new_int_op (ctx, -1));
  init_reg_vars (c2m_ctx);
  VARR_CREATE (MIR_var_t, proto_info.arg_vars, alloc, 32);
  VARR_CREATE (MIR_type_t, proto_info.ret_types, alloc, 16);
  gen_mir_protos (c2m_ctx);
  VARR_CREATE (MIR_op_t, call_ops, alloc, 32);
  VARR_CREATE (MIR_op_t, ret_ops, alloc, 8);
  VARR_CREATE (MIR_op_t, switch_ops, alloc, 128);
  VARR_CREATE (case_t, switch_cases, alloc, 64);
  VARR_CREATE (init_el_t, init_els, alloc, 128);
  VARR_CREATE (node_t, node_stack, alloc, 8);
  memset_proto = memset_item = memcpy_proto = memcpy_item = NULL;
  top_gen (c2m_ctx, r, NULL, NULL, NULL);
  gen_finish (c2m_ctx);
}

/* ------------------------- MIR generator finish ----------------------------- */

/* New Page */

static const char *get_node_name (node_code_t code) {
#define REP_SEP ;
#define C(n) \
  case N_##n: return #n
  switch (code) {
    C (IGNORE);
    REP8 (C, I, L, LL, U, UL, ULL, F, D);
    REP7 (C, LD, CH, CH16, CH32, STR, STR16, STR32);
    REP6 (C, ID, COMMA, ANDAND, OROR, EQ, STMTEXPR);
    REP8 (C, NE, LT, LE, GT, GE, ASSIGN, BITWISE_NOT, NOT);
    REP8 (C, AND, AND_ASSIGN, OR, OR_ASSIGN, XOR, XOR_ASSIGN, LSH, LSH_ASSIGN);
    REP8 (C, RSH, RSH_ASSIGN, ADD, ADD_ASSIGN, SUB, SUB_ASSIGN, MUL, MUL_ASSIGN);
    REP8 (C, DIV, DIV_ASSIGN, MOD, MOD_ASSIGN, IND, FIELD, ADDR, DEREF);
    REP8 (C, DEREF_FIELD, COND, INC, DEC, POST_INC, POST_DEC, ALIGNOF, SIZEOF);
    REP8 (C, EXPR_SIZEOF, CAST, COMPOUND_LITERAL, CALL, GENERIC, GENERIC_ASSOC, IF, SWITCH);
    REP8 (C, WHILE, DO, FOR, GOTO, INDIRECT_GOTO, CONTINUE, BREAK, RETURN);
    REP8 (C, EXPR, BLOCK, CASE, DEFAULT, LABEL, LABEL_ADDR, LIST, SPEC_DECL);
    REP8 (C, SHARE, TYPEDEF, EXTERN, STATIC, AUTO, REGISTER, THREAD_LOCAL, DECL);
    REP8 (C, VOID, CHAR, SHORT, INT, LONG, FLOAT, DOUBLE, SIGNED);
    REP8 (C, UNSIGNED, BOOL, STRUCT, UNION, ENUM, ENUM_CONST, MEMBER, CONST);
    REP8 (C, RESTRICT, VOLATILE, ATOMIC, INLINE, NO_RETURN, ALIGNAS, FUNC, STAR);
    REP8 (C, POINTER, DOTS, ARR, INIT, FIELD_ID, TYPE, ST_ASSERT, FUNC_DEF);
    REP3 (C, MODULE, ASM, ATTR);
  default: abort ();
  }
#undef C
#undef REP_SEP
}

static void print_char (FILE *f, mir_ulong ch) {
  if (ch >= 0x100) {
    fprintf (f, ch <= 0xFFFF ? "\\u%04x" : "\\U%08x", (unsigned int) ch);
  } else {
    if (ch == '"' || ch == '\"' || ch == '\\') fprintf (f, "\\");
    if (isprint (ch))
      fprintf (f, "%c", (unsigned int) ch);
    else
      fprintf (f, "\\%o", (unsigned int) ch);
  }
}

static void print_chars (FILE *f, const char *str, size_t len) {
  for (size_t i = 0; i < len; i++) print_char (f, str[i]);
}

static void print_chars16 (FILE *f, const char *str, size_t len) {
  for (size_t i = 0; i < len; i += 2) print_char (f, ((mir_char16 *) str)[i]);
}

static void print_chars32 (FILE *f, const char *str, size_t len) {
  for (size_t i = 0; i < len; i += 4) print_char (f, ((mir_char32 *) str)[i]);
}

static void print_node (c2m_ctx_t c2m_ctx, FILE *f, node_t n, int indent, int attr_p);

void debug_node (c2m_ctx_t c2m_ctx, node_t n) { print_node (c2m_ctx, stderr, n, 0, TRUE); }

static void print_ops (c2m_ctx_t c2m_ctx, FILE *f, node_t n, int indent, int attr_p) {
  int i;
  node_t op;

  for (i = 0; (op = get_op (n, i)) != NULL; i++) print_node (c2m_ctx, f, op, indent + 2, attr_p);
}

static void print_qual (FILE *f, struct type_qual type_qual) {
  if (type_qual.const_p) fprintf (f, ", const");
  if (type_qual.restrict_p) fprintf (f, ", restrict");
  if (type_qual.volatile_p) fprintf (f, ", volatile");
  if (type_qual.atomic_p) fprintf (f, ", atomic");
}

static void print_basic_type (FILE *f, enum basic_type basic_type) {
  switch (basic_type) {
  case TP_UNDEF: fprintf (f, "undef type"); break;
  case TP_VOID: fprintf (f, "void"); break;
  case TP_BOOL: fprintf (f, "bool"); break;
  case TP_CHAR: fprintf (f, "char"); break;
  case TP_SCHAR: fprintf (f, "signed char"); break;
  case TP_UCHAR: fprintf (f, "unsigned char"); break;
  case TP_SHORT: fprintf (f, "short"); break;
  case TP_USHORT: fprintf (f, "unsigned short"); break;
  case TP_INT: fprintf (f, "int"); break;
  case TP_UINT: fprintf (f, "unsigned int"); break;
  case TP_LONG: fprintf (f, "long"); break;
  case TP_ULONG: fprintf (f, "unsigned long"); break;
  case TP_LLONG: fprintf (f, "long long"); break;
  case TP_ULLONG: fprintf (f, "unsigned long long"); break;
  case TP_FLOAT: fprintf (f, "float"); break;
  case TP_DOUBLE: fprintf (f, "double"); break;
  case TP_LDOUBLE: fprintf (f, "long double"); break;
  default: assert (FALSE);
  }
}
static void print_type (c2m_ctx_t c2m_ctx, FILE *f, struct type *type) {
  switch (type->mode) {
  case TM_UNDEF: fprintf (f, "undef type mode"); break;
  case TM_BASIC: print_basic_type (f, type->u.basic_type); break;
  case TM_ENUM: fprintf (f, "enum node %u", type->u.tag_type->uid); break;
  case TM_PTR:
    fprintf (f, "ptr (");
    print_type (c2m_ctx, f, type->u.ptr_type);
    fprintf (f, ")");
    if (type->arr_type != NULL) {
      fprintf (f, ", former ");
      print_type (c2m_ctx, f, type->arr_type);
    }
    if (type->func_type_before_adjustment_p) fprintf (f, ", former func");
    break;
  case TM_STRUCT: fprintf (f, "struct node %u", type->u.tag_type->uid); break;
  case TM_UNION: fprintf (f, "union node %u", type->u.tag_type->uid); break;
  case TM_ARR:
    fprintf (f, "array [%s", type->u.arr_type->static_p ? "static " : "");
    print_qual (f, type->u.arr_type->ind_type_qual);
    fprintf (f, "size node %u] (", type->u.arr_type->size->uid);
    print_type (c2m_ctx, f, type->u.arr_type->el_type);
    fprintf (f, ")");
    break;
  case TM_FUNC:
    fprintf (f, "func ");
    print_type (c2m_ctx, f, type->u.func_type->ret_type);
    fprintf (f, "(params node %u", type->u.func_type->param_list->uid);
    fprintf (f, type->u.func_type->dots_p ? ", ...)" : ")");
    break;
  default: assert (FALSE);
  }
  print_qual (f, type->type_qual);
  if (incomplete_type_p (c2m_ctx, type)) fprintf (f, ", incomplete");
  if (type->raw_size != MIR_SIZE_MAX)
    fprintf (f, ", raw size = %llu", (unsigned long long) type->raw_size);
  if (type->align >= 0) fprintf (f, ", align = %d", type->align);
  fprintf (f, " ");
}

static void print_decl_spec (c2m_ctx_t c2m_ctx, FILE *f, struct decl_spec *decl_spec) {
  if (decl_spec->typedef_p) fprintf (f, " typedef, ");
  if (decl_spec->extern_p) fprintf (f, " extern, ");
  if (decl_spec->static_p) fprintf (f, " static, ");
  if (decl_spec->auto_p) fprintf (f, " auto, ");
  if (decl_spec->register_p) fprintf (f, " register, ");
  if (decl_spec->thread_local_p) fprintf (f, " thread local, ");
  if (decl_spec->inline_p) fprintf (f, " inline, ");
  if (decl_spec->no_return_p) fprintf (f, " no return, ");
  if (decl_spec->align >= 0) fprintf (f, " align = %d, ", decl_spec->align);
  if (decl_spec->align_node != NULL)
    fprintf (f, " strictest align node %u, ", decl_spec->align_node->uid);
  if (decl_spec->linkage != N_IGNORE)
    fprintf (f, " %s linkage, ", decl_spec->linkage == N_STATIC ? "static" : "extern");
  print_type (c2m_ctx, f, decl_spec->type);
}

static void print_decl (c2m_ctx_t c2m_ctx, FILE *f, decl_t decl) {
  if (decl == NULL) return;
  fprintf (f, ": ");
  if (decl->scope != NULL) fprintf (f, "scope node = %u, ", decl->scope->uid);
  print_decl_spec (c2m_ctx, f, &decl->decl_spec);
  if (decl->addr_p) fprintf (f, ", addressable");
  if (decl->used_p) fprintf (f, ", used");
  if (decl->reg_p)
    fprintf (f, ", reg");
  else {
    fprintf (f, ", offset = %llu", (unsigned long long) decl->offset);
    if (decl->bit_offset >= 0) fprintf (f, ", bit offset = %d", decl->bit_offset);
  }
  if (decl->asm_p) fprintf (f, ", asm=%s", decl->u.asm_str);
}

static void print_expr (c2m_ctx_t c2m_ctx, FILE *f, struct expr *e) {
  if (e == NULL) return; /* e.g. N_ID which is not an expr */
  fprintf (f, ": ");
  if (e->u.lvalue_node) fprintf (f, "lvalue, ");
  print_type (c2m_ctx, f, e->type);
  if (e->const_p) {
    fprintf (f, ", const = ");
    if (!integer_type_p (e->type)) {
      fprintf (f, " %.*Lg\n", LDBL_MANT_DIG, (long double) e->c.d_val);
    } else if (signed_integer_type_p (e->type)) {
      fprintf (f, "%lld", (long long) e->c.i_val);
    } else {
      fprintf (f, "%llu", (unsigned long long) e->c.u_val);
    }
  }
}

static void print_node (c2m_ctx_t c2m_ctx, FILE *f, node_t n, int indent, int attr_p) {
  int i;

  fprintf (f, "%6u: ", n->uid);
  for (i = 0; i < indent; i++) fprintf (f, " ");
  if (n == err_node) {
    fprintf (f, "<error>\n");
    return;
  }
  fprintf (f, "%s (", get_node_name (n->code));
  print_pos (f, POS (n), FALSE);
  fprintf (f, ")");
  switch (n->code) {
  case N_IGNORE: fprintf (f, "\n"); break;
  case N_I: fprintf (f, " %lld", (long long) n->u.l); goto expr;
  case N_L: fprintf (f, " %lldl", (long long) n->u.l); goto expr;
  case N_LL: fprintf (f, " %lldll", (long long) n->u.ll); goto expr;
  case N_U: fprintf (f, " %lluu", (unsigned long long) n->u.ul); goto expr;
  case N_UL: fprintf (f, " %lluul", (unsigned long long) n->u.ul); goto expr;
  case N_ULL: fprintf (f, " %lluull", (unsigned long long) n->u.ull); goto expr;
  case N_F: fprintf (f, " %.*g", FLT_MANT_DIG, (double) n->u.f); goto expr;
  case N_D: fprintf (f, " %.*g", DBL_MANT_DIG, (double) n->u.d); goto expr;
  case N_LD: fprintf (f, " %.*Lg", LDBL_MANT_DIG, (long double) n->u.ld); goto expr;
  case N_CH:
  case N_CH16:
  case N_CH32:
    fprintf (f, " %s'", n->code == N_CH ? "" : n->code == N_CH16 ? "u" : "U");
    print_char (f, n->u.ch);
    fprintf (f, "'");
    goto expr;
  case N_STR:
  case N_STR16:
  case N_STR32:
    fprintf (f, " %s\"", n->code == N_STR ? "" : n->code == N_STR16 ? "u" : "U");
    (n->code == N_STR     ? print_chars
     : n->code == N_STR16 ? print_chars16
                          : print_chars32) (f, n->u.s.s, n->u.s.len);
    fprintf (f, "\"");
    goto expr;
  case N_ID:
    fprintf (f, " %s", n->u.s.s);
  expr:
    if (attr_p && n->attr != NULL) print_expr (c2m_ctx, f, n->attr);
    fprintf (f, "\n");
    break;
  case N_COMMA:
  case N_ANDAND:
  case N_OROR:
  case N_EQ:
  case N_NE:
  case N_LT:
  case N_LE:
  case N_GT:
  case N_GE:
  case N_ASSIGN:
  case N_BITWISE_NOT:
  case N_NOT:
  case N_AND:
  case N_AND_ASSIGN:
  case N_OR:
  case N_OR_ASSIGN:
  case N_XOR:
  case N_XOR_ASSIGN:
  case N_LSH:
  case N_LSH_ASSIGN:
  case N_RSH:
  case N_RSH_ASSIGN:
  case N_ADD:
  case N_ADD_ASSIGN:
  case N_SUB:
  case N_SUB_ASSIGN:
  case N_MUL:
  case N_MUL_ASSIGN:
  case N_DIV:
  case N_DIV_ASSIGN:
  case N_MOD:
  case N_MOD_ASSIGN:
  case N_IND:
  case N_FIELD:
  case N_ADDR:
  case N_DEREF:
  case N_DEREF_FIELD:
  case N_COND:
  case N_INC:
  case N_DEC:
  case N_POST_INC:
  case N_POST_DEC:
  case N_ALIGNOF:
  case N_SIZEOF:
  case N_EXPR_SIZEOF:
  case N_CAST:
  case N_COMPOUND_LITERAL:
  case N_CALL:
  case N_GENERIC:
  case N_STMTEXPR:
  case N_LABEL_ADDR:
    if (attr_p && n->attr != NULL) print_expr (c2m_ctx, f, n->attr);
    fprintf (f, "\n");
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_GENERIC_ASSOC:
  case N_IF:
  case N_WHILE:
  case N_DO:
  case N_CONTINUE:
  case N_BREAK:
  case N_RETURN:
  case N_EXPR:
  case N_CASE:
  case N_DEFAULT:
  case N_LABEL:
  case N_SHARE:
  case N_TYPEDEF:
  case N_EXTERN:
  case N_STATIC:
  case N_AUTO:
  case N_REGISTER:
  case N_THREAD_LOCAL:
  case N_DECL:
  case N_VOID:
  case N_CHAR:
  case N_SHORT:
  case N_INT:
  case N_LONG:
  case N_FLOAT:
  case N_DOUBLE:
  case N_SIGNED:
  case N_UNSIGNED:
  case N_BOOL:
  case N_CONST:
  case N_RESTRICT:
  case N_VOLATILE:
  case N_ATOMIC:
  case N_INLINE:
  case N_NO_RETURN:
  case N_ALIGNAS:
  case N_STAR:
  case N_POINTER:
  case N_DOTS:
  case N_ARR:
  case N_INIT:
  case N_FIELD_ID:
  case N_TYPE:
  case N_ST_ASSERT:
  case N_ASM:
    fprintf (f, "\n");
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_ATTR:
    fprintf (f, "\n");
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_LIST:
    if (attr_p && n->attr != NULL) {
      fprintf (f, ": ");
      print_decl_spec (c2m_ctx, f, (struct decl_spec *) n->attr);
    }
    fprintf (f, "\n");
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_SPEC_DECL:
  case N_MEMBER:
  case N_FUNC_DEF:
    if (attr_p && n->attr != NULL) print_decl (c2m_ctx, f, (decl_t) n->attr);
    fprintf (f, "\n");
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_FUNC:
    if (!attr_p || n->attr == NULL) {
      fprintf (f, "\n");
      print_ops (c2m_ctx, f, n, indent, attr_p);
      break;
    }
    /* fall through */
  case N_STRUCT:
  case N_UNION:
  case N_MODULE:
  case N_BLOCK:
  case N_FOR:
    if (!attr_p
        || ((n->code == N_STRUCT || n->code == N_UNION)
            && (NL_EL (n->u.ops, 1) == NULL || NL_EL (n->u.ops, 1)->code == N_IGNORE)))
      fprintf (f, "\n");
    else if (n->code == N_MODULE)
      fprintf (f, ": the top scope");
    else if (n->attr != NULL)
      fprintf (f, ": higher scope node %u", ((struct node_scope *) n->attr)->scope->uid);
    if (n->code == N_STRUCT || n->code == N_UNION)
      fprintf (f, "\n");
    else if (attr_p && n->attr != NULL)
      fprintf (f, ", size = %llu, offset = %llu\n",
               (unsigned long long) ((struct node_scope *) n->attr)->size,
               (unsigned long long) ((struct node_scope *) n->attr)->offset);
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_SWITCH:
    if (attr_p && n->attr != NULL) {
      fprintf (f, ": ");
      print_type (c2m_ctx, f, &((struct switch_attr *) n->attr)->type);
    }
    fprintf (f, "\n");
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_GOTO:
    if (attr_p && n->attr != NULL) fprintf (f, ": target node %u\n", ((node_t) n->attr)->uid);
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_INDIRECT_GOTO: print_ops (c2m_ctx, f, n, indent, attr_p); break;
  case N_ENUM:
    if (attr_p && n->attr != NULL) {
      fprintf (f, ": enum_basic_type = ");
      print_basic_type (f, ((struct enum_type *) n->attr)->enum_basic_type);
      fprintf (f, "\n");
    }
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  case N_ENUM_CONST:
    if (attr_p && n->attr != NULL)  // ???!!!
      fprintf (f, ": val = %lld\n", (long long) ((struct enum_value *) n->attr)->u.i_val);
    print_ops (c2m_ctx, f, n, indent, attr_p);
    break;
  default: abort ();
  }
}

static void init_include_dirs (c2m_ctx_t c2m_ctx) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  int MIR_UNUSED added_p = FALSE;

  VARR_CREATE (char_ptr_t, headers, alloc, 0);
  VARR_CREATE (char_ptr_t, system_headers, alloc, 0);
  for (size_t i = 0; i < c2m_options->include_dirs_num; i++) {
    VARR_PUSH (char_ptr_t, headers, c2m_options->include_dirs[i]);
    VARR_PUSH (char_ptr_t, system_headers, c2m_options->include_dirs[i]);
  }
  VARR_PUSH (char_ptr_t, headers, NULL);
#if defined(__APPLE__) || defined(__unix__)
  VARR_PUSH (char_ptr_t, system_headers, "/usr/local/include");
#endif
#ifdef ADDITIONAL_INCLUDE_PATH
  if (ADDITIONAL_INCLUDE_PATH[0] != 0) {
    added_p = TRUE;
    VARR_PUSH (char_ptr_t, system_headers, ADDITIONAL_INCLUDE_PATH);
  }
#endif
#if defined(__APPLE__)
  if (!added_p)
    VARR_PUSH (char_ptr_t, system_headers,
               "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include");
#endif
#if defined(__linux__) && defined(__x86_64__)
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include/x86_64-linux-gnu");
#elif defined(__linux__) && defined(__aarch64__)
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include/aarch64-linux-gnu");
#elif defined(__linux__) && defined(__PPC64__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include/powerpc64le-linux-gnu");
#else
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include/powerpc64-linux-gnu");
#endif
#elif defined(__linux__) && defined(__s390x__)
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include/s390x-linux-gnu");
#elif defined(__linux__) && defined(__riscv)
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include/riscv64-linux-gnu");
#endif
#if defined(__APPLE__) || defined(__unix__)
  VARR_PUSH (char_ptr_t, system_headers, "/usr/include");
#endif
  VARR_PUSH (char_ptr_t, system_headers, NULL);
  header_dirs = (const char **) VARR_ADDR (char_ptr_t, headers);
  system_header_dirs = (const char **) VARR_ADDR (char_ptr_t, system_headers);
}

static int check_id_p (c2m_ctx_t c2m_ctx, const char *str) {
  int ok_p;

  if ((ok_p = isalpha (str[0]) || str[0] == '_')) {
    for (size_t i = 1; str[i] != '\0'; i++)
      if (!isalnum (str[i]) && str[i] != '_') {
        ok_p = FALSE;
        break;
      }
  }
  if (!ok_p && c2m_options->message_file != NULL)
    fprintf (c2m_options->message_file, "macro name %s is not an identifier\n", str);
  return ok_p;
}

static void define_cmd_macro (c2m_ctx_t c2m_ctx, const char *name, const char *def) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  pos_t pos;
  token_t t, id;
  struct macro macro;
  macro_t tab_m;
  VARR (token_t) * repl;

  pos.fname = COMMAND_LINE_SOURCE_NAME;
  pos.lno = 1;
  pos.ln_pos = 0;
  VARR_CREATE (token_t, repl, alloc, 16);
  id = new_id_token (c2m_ctx, pos, name);
  VARR_TRUNC (char, temp_string, 0);
  for (; *def != '\0'; def++) VARR_PUSH (char, temp_string, *def);
  VARR_PUSH (char, temp_string, '\0');
  reverse (temp_string);
  set_string_stream (c2m_ctx, VARR_ADDR (char, temp_string), pos, NULL);
  while ((t = get_next_pptoken (c2m_ctx))->code != T_EOFILE && t->code != T_EOU)
    VARR_PUSH (token_t, repl, t);
  if (check_id_p (c2m_ctx, id->repr)) {
    macro.id = id;
    if (HTAB_DO (macro_t, macro_tab, &macro, HTAB_FIND, tab_m)) {
      if (!replacement_eq_p (tab_m->replacement, repl) && c2m_options->message_file != NULL)
        fprintf (c2m_options->message_file,
                 "warning -- redefinition of macro %s on the command line\n", id->repr);
      HTAB_DO (macro_t, macro_tab, &macro, HTAB_DELETE, tab_m);
    }
    new_macro (c2m_ctx, macro.id, NULL, repl);
  }
}

static void undefine_cmd_macro (c2m_ctx_t c2m_ctx, const char *name) {
  pre_ctx_t pre_ctx = c2m_ctx->pre_ctx;
  pos_t pos;
  token_t id;
  struct macro macro;
  macro_t tab_m;

  pos.fname = COMMAND_LINE_SOURCE_NAME;
  pos.lno = 1;
  pos.ln_pos = 0;
  id = new_id_token (c2m_ctx, pos, name);
  if (check_id_p (c2m_ctx, id->repr)) {
    macro.id = id;
    HTAB_DO (macro_t, macro_tab, &macro, HTAB_DELETE, tab_m);
  }
}

static void process_macro_commands (c2m_ctx_t c2m_ctx) {
  for (size_t i = 0; i < c2m_options->macro_commands_num; i++)
    if (c2m_options->macro_commands[i].def)
      define_cmd_macro (c2m_ctx, c2m_options->macro_commands[i].name,
                        c2m_options->macro_commands[i].def);
    else
      undefine_cmd_macro (c2m_ctx, c2m_options->macro_commands[i].name);
}

static void compile_init (c2m_ctx_t c2m_ctx, struct c2mir_options *ops, int (*getc_func) (void *),
                          void *getc_data) {
  MIR_alloc_t alloc = c2m_alloc (c2m_ctx);
  c2m_options = ops;
  n_errors = n_warnings = 0;
  c_getc = getc_func;
  c_getc_data = getc_data;
  VARR_CREATE (char, symbol_text, alloc, 128);
  VARR_CREATE (char, temp_string, alloc, 128);
  VARR_CREATE (pos_t, node_positions, alloc, 128);
  parse_init (c2m_ctx);
  context_init (c2m_ctx);
  init_include_dirs (c2m_ctx);
  process_macro_commands (c2m_ctx);
  VARR_CREATE (node_t, call_nodes, alloc, 128); /* used in context and gen */
  VARR_CREATE (node_t, containing_anon_members, alloc, 8);
  VARR_CREATE (init_object_t, init_object_path, alloc, 8);
}

static void compile_finish (c2m_ctx_t c2m_ctx) {
  if (symbol_text != NULL) VARR_DESTROY (char, symbol_text);
  if (temp_string != NULL) VARR_DESTROY (char, temp_string);
  if (node_positions != NULL) VARR_DESTROY (pos_t, node_positions);
  parse_finish (c2m_ctx);
  context_finish (c2m_ctx);
  if (headers != NULL) VARR_DESTROY (char_ptr_t, headers);
  if (system_headers != NULL) VARR_DESTROY (char_ptr_t, system_headers);
  if (call_nodes != NULL) VARR_DESTROY (node_t, call_nodes);
  if (containing_anon_members != NULL) VARR_DESTROY (node_t, containing_anon_members);
  if (init_object_path != NULL) VARR_DESTROY (init_object_t, init_object_path);
}

#include "real-time.h"

static const char *get_module_name (c2m_ctx_t c2m_ctx) {
  sprintf (temp_str_buff, "M%ld", (long) c2m_options->module_num);
  return temp_str_buff;
}

static int top_level_getc (c2m_ctx_t c2m_ctx) { return c_getc (c_getc_data); }

int c2mir_compile (MIR_context_t ctx, struct c2mir_options *ops, int (*getc_func) (void *),
                   void *getc_data, const char *source_name, FILE *output_file) {
  struct c2m_ctx *c2m_ctx = *c2m_ctx_loc (ctx);
  double start_time = real_usec_time ();
  node_t r;
  unsigned n_error_before;
  MIR_module_t m;

  if (c2m_ctx == NULL) return 0;
  if (setjmp (c2m_ctx->env)) {
    compile_finish (c2m_ctx);
    return 0;
  }
  compile_init (c2m_ctx, ops, getc_func, getc_data);
  if (c2m_options->verbose_p && c2m_options->message_file != NULL)
    fprintf (c2m_options->message_file, "C2MIR init end           -- %.0f usec\n",
             real_usec_time () - start_time);
  add_stream (c2m_ctx, NULL, source_name, top_level_getc);
  if (!c2m_options->no_prepro_p) add_standard_includes (c2m_ctx);
  pre (c2m_ctx);
  if (c2m_options->verbose_p && c2m_options->message_file != NULL)
    fprintf (c2m_options->message_file, "  C2MIR preprocessor end    -- %.0f usec\n",
             real_usec_time () - start_time);
  if (!c2m_options->prepro_only_p) {
    r = parse (c2m_ctx);
    if (c2m_options->verbose_p && c2m_options->message_file != NULL)
      fprintf (c2m_options->message_file, "  C2MIR parser end          -- %.0f usec\n",
               real_usec_time () - start_time);
    if (c2m_options->verbose_p && c2m_options->message_file != NULL && n_errors)
      fprintf (c2m_options->message_file, "parser - FAIL\n");
    if (!c2m_options->syntax_only_p) {
      n_error_before = n_errors;
      do_context (c2m_ctx, r);
      if (n_errors > n_error_before) {
        if (c2m_options->debug_p) print_node (c2m_ctx, c2m_options->message_file, r, 0, FALSE);
        if (c2m_options->verbose_p && c2m_options->message_file != NULL)
          fprintf (c2m_options->message_file, "C2MIR context checker - FAIL\n");
      } else {
        if (c2m_options->debug_p) print_node (c2m_ctx, c2m_options->message_file, r, 0, TRUE);
        if (c2m_options->verbose_p && c2m_options->message_file != NULL)
          fprintf (c2m_options->message_file, "  C2MIR context checker end -- %.0f usec\n",
                   real_usec_time () - start_time);
        m = MIR_new_module (ctx, get_module_name (c2m_ctx));
        gen_mir (c2m_ctx, r);
        if ((c2m_options->asm_p || c2m_options->object_p) && n_errors == 0) {
          if (strcmp (source_name, COMMAND_LINE_SOURCE_NAME) == 0) {
            MIR_output_module (ctx, c2m_options->message_file, m);
          } else if (output_file != NULL) {
            (c2m_options->asm_p ? MIR_output_module : MIR_write_module) (ctx, output_file, m);
            if (ferror (output_file) || fclose (output_file)) {
              fprintf (c2m_options->message_file, "C2MIR error in writing mir for source file %s\n",
                       source_name);
              n_errors++;
            }
          }
        }
        MIR_finish_module (ctx);
        if (c2m_options->verbose_p && c2m_options->message_file != NULL)
          fprintf (c2m_options->message_file, "  C2MIR generator end       -- %.0f usec\n",
                   real_usec_time () - start_time);
      }
    }
  }
  compile_finish (c2m_ctx);
  if (c2m_options->verbose_p && c2m_options->message_file != NULL)
    fprintf (c2m_options->message_file, "C2MIR compiler end                -- %.0f usec\n",
             real_usec_time () - start_time);
  return n_errors == 0;
}

/* Local Variables:                */
/* mode: c                         */
/* page-delimiter: "/\\* New Page" */
/* End:                            */
