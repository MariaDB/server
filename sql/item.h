#ifndef SQL_ITEM_INCLUDED
#define SQL_ITEM_INCLUDED

/* Copyright (c) 2000, 2017, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "sql_priv.h"                /* STRING_BUFFER_USUAL_SIZE */
#include "unireg.h"
#include "sql_const.h"                 /* RAND_TABLE_BIT, MAX_FIELD_NAME */
#include "field.h"                              /* Derivation */
#include "sql_type.h"
#include "sql_time.h"
#include "mem_root_array.h"

C_MODE_START
#include <ma_dyncol.h>

/*
  A prototype for a C-compatible structure to store a value of any data type.
  Currently it has to stay in /sql, as it depends on String and my_decimal.
  We'll do the following changes:
  1. add pure C "struct st_string" and "struct st_my_decimal"
  2. change type of m_string to struct st_string and move inside the union
  3. change type of m_decmal to struct st_my_decimal and move inside the union
  4. move the definition to some file in /include
*/
class st_value
{
public:
  st_value() {}
  st_value(char *buffer, size_t buffer_size) :
  m_string(buffer, buffer_size, &my_charset_bin)
  {}
  enum enum_dynamic_column_type m_type;
  union
  {
    longlong m_longlong;
    double m_double;
    MYSQL_TIME m_time;
  } value;
  String m_string;
  my_decimal m_decimal;
};

C_MODE_END


class Value: public st_value
{
public:
  Value(char *buffer, size_t buffer_size) : st_value(buffer, buffer_size)
  {}
  Value()
  {}
  bool is_null() const { return m_type == DYN_COL_NULL; }
  bool is_longlong() const
  {
    return m_type == DYN_COL_UINT || m_type == DYN_COL_INT;
  }
  bool is_double() const { return m_type == DYN_COL_DOUBLE; }
  bool is_temporal() const { return m_type == DYN_COL_DATETIME; }
  bool is_string() const { return m_type == DYN_COL_STRING; }
  bool is_decimal() const { return m_type == DYN_COL_DECIMAL; }
};


template<size_t buffer_size>
class ValueBuffer: public Value
{
  char buffer[buffer_size];
public:
  ValueBuffer(): Value(buffer, buffer_size)
  {}
  void reset_buffer()
  {
    m_string.set_buffer_if_not_allocated(buffer, buffer_size, &my_charset_bin);
  }
};


#ifdef DBUG_OFF
static inline const char *dbug_print_item(Item *item) { return NULL; }
#else
const char *dbug_print_item(Item *item);
#endif

class Virtual_tmp_table;
class sp_head;
class Protocol;
struct TABLE_LIST;
void item_init(void);			/* Init item functions */
class Item_basic_value;
class Item_result_field;
class Item_field;
class Item_ref;
class Item_param;
class user_var_entry;
class JOIN;
struct KEY_FIELD;
struct SARGABLE_PARAM;
class RANGE_OPT_PARAM;
class SEL_TREE;

enum precedence {
  LOWEST_PRECEDENCE,
  ASSIGN_PRECEDENCE,    // :=
  OR_PRECEDENCE,        // OR, || (unless PIPES_AS_CONCAT)
  XOR_PRECEDENCE,       // XOR
  AND_PRECEDENCE,       // AND, &&
  NOT_PRECEDENCE,       // NOT (unless HIGH_NOT_PRECEDENCE)
  CMP_PRECEDENCE,       // =, <=>, >=, >, <=, <, <>, !=, IS
  BETWEEN_PRECEDENCE,   // BETWEEN
  IN_PRECEDENCE,        // IN, LIKE, REGEXP
  BITOR_PRECEDENCE,     // |
  BITAND_PRECEDENCE,    // &
  SHIFT_PRECEDENCE,     // <<, >>
  INTERVAL_PRECEDENCE,  // first argument in +INTERVAL
  ADD_PRECEDENCE,       // +, -
  MUL_PRECEDENCE,       // *, /, DIV, %, MOD
  BITXOR_PRECEDENCE,    // ^
  PIPES_PRECEDENCE,     // || (if PIPES_AS_CONCAT)
  NEG_PRECEDENCE,       // unary -, ~, !, NOT (if HIGH_NOT_PRECEDENCE)
  COLLATE_PRECEDENCE,   // BINARY, COLLATE
  DEFAULT_PRECEDENCE,
  HIGHEST_PRECEDENCE
};

bool mark_unsupported_function(const char *where, void *store, uint result);

/* convenience helper for mark_unsupported_function() above */
bool mark_unsupported_function(const char *w1, const char *w2,
                               void *store, uint result);

/* Bits for the split_sum_func() function */
#define SPLIT_SUM_SKIP_REGISTERED 1     /* Skip registered funcs */
#define SPLIT_SUM_SELECT 2		/* SELECT item; Split all parts */

/*
  Values for item->marker for cond items in the WHERE clause as used
  by the optimizer.

  Note that for Item_fields, the marker contains
  'select->cur_pos_in_select_list
*/
/* Used to check GROUP BY list in the MODE_ONLY_FULL_GROUP_BY mode */
#define MARKER_UNDEF_POS      -1
#define MARKER_UNUSED         0
#define MARKER_CHANGE_COND    1
#define MARKER_PROCESSED      2
#define MARKER_CHECK_ON_READ  3
#define MARKER_NULL_KEY       4
#define MARKER_FOUND_IN_ORDER 6

/* Used as bits in marker by Item::check_pushable_cond() */
#define MARKER_NO_EXTRACTION              (1 << 6)
#define MARKER_FULL_EXTRACTION            (1 << 7)
#define MARKER_DELETION                   (1 << 8)
#define MARKER_IMMUTABLE                  (1 << 9)
#define MARKER_SUBSTITUTION               (1 << 10)

/* Used as bits in marker by window functions */
#define MARKER_SORTORDER_CHANGE           (1 << 11)
#define MARKER_PARTITION_CHANGE           (1 << 12)
#define MARKER_FRAME_CHANGE               (1 << 13)
#define MARKER_EXTRACTION_MASK                                         \
  (MARKER_NO_EXTRACTION | MARKER_FULL_EXTRACTION | MARKER_DELETION |   \
   MARKER_IMMUTABLE)

extern const char *item_empty_name;

void dummy_error_processor(THD *thd, void *data);

void view_error_processor(THD *thd, void *data);

typedef List<TABLE_LIST>* ignored_tables_list_t;
bool ignored_list_includes_table(ignored_tables_list_t list, TABLE_LIST *tbl);

/*
  Instances of Name_resolution_context store the information necessary for
  name resolution of Items and other context analysis of a query made in
  fix_fields().

  This structure is a part of SELECT_LEX, a pointer to this structure is
  assigned when an item is created (which happens mostly during  parsing
  (sql_yacc.yy)), but the structure itself will be initialized after parsing
  is complete

  TODO: move subquery of INSERT ... SELECT and CREATE ... SELECT to
  separate SELECT_LEX which allow to remove tricks of changing this
  structure before and after INSERT/CREATE and its SELECT to make correct
  field name resolution.
*/
struct Name_resolution_context: Sql_alloc
{
  /*
    The name resolution context to search in when an Item cannot be
    resolved in this context (the context of an outer select)
  */
  Name_resolution_context *outer_context;

  /*
    List of tables used to resolve the items of this context.  Usually these
    are tables from the FROM clause of SELECT statement.  The exceptions are
    INSERT ... SELECT and CREATE ... SELECT statements, where SELECT
    subquery is not moved to a separate SELECT_LEX.  For these types of
    statements we have to change this member dynamically to ensure correct
    name resolution of different parts of the statement.
  */
  TABLE_LIST *table_list;
  /*
    In most cases the two table references below replace 'table_list' above
    for the purpose of name resolution. The first and last name resolution
    table references allow us to search only in a sub-tree of the nested
    join tree in a FROM clause. This is needed for NATURAL JOIN, JOIN ... USING
    and JOIN ... ON. 
  */
  TABLE_LIST *first_name_resolution_table;
  /*
    Last table to search in the list of leaf table references that begins
    with first_name_resolution_table.
  */
  TABLE_LIST *last_name_resolution_table;

  /* Cache first_name_resolution_table in setup_natural_join_row_types */
  TABLE_LIST *natural_join_first_table;
  /*
    SELECT_LEX item belong to, in case of merged VIEW it can differ from
    SELECT_LEX where item was created, so we can't use table_list/field_list
    from there
  */
  st_select_lex *select_lex;

  /*
    Processor of errors caused during Item name resolving, now used only to
    hide underlying tables in errors about views (i.e. it substitute some
    errors for views)
  */
  void (*error_processor)(THD *, void *);
  void *error_processor_data;

  /*
    When TRUE items are resolved in this context both against the
    SELECT list and this->table_list. If FALSE, items are resolved
    only against this->table_list.
  */
  bool resolve_in_select_list;

  /*
    Bitmap of tables that should be ignored when doing name resolution.
    Normally it is {0}. Non-zero values are used by table functions.
  */
  ignored_tables_list_t ignored_tables;

  /*
    Security context of this name resolution context. It's used for views
    and is non-zero only if the view is defined with SQL SECURITY DEFINER.
  */
  Security_context *security_ctx;

  Name_resolution_context()
    :outer_context(0), table_list(0), select_lex(0),
    error_processor_data(0),
    ignored_tables(NULL),
    security_ctx(0)
    {}

  void init()
  {
    resolve_in_select_list= FALSE;
    error_processor= &dummy_error_processor;
    first_name_resolution_table= NULL;
    last_name_resolution_table= NULL;
  }

  void resolve_in_table_list_only(TABLE_LIST *tables)
  {
    table_list= first_name_resolution_table= tables;
    resolve_in_select_list= FALSE;
  }

  void process_error(THD *thd)
  {
    (*error_processor)(thd, error_processor_data);
  }
  st_select_lex *outer_select()
  {
    return (outer_context ?
            outer_context->select_lex :
            NULL);
  }
};


/*
  Store and restore the current state of a name resolution context.
*/

class Name_resolution_context_state
{
private:
  TABLE_LIST *save_table_list;
  TABLE_LIST *save_first_name_resolution_table;
  TABLE_LIST *save_next_name_resolution_table;
  bool        save_resolve_in_select_list;
  TABLE_LIST *save_next_local;

public:
  Name_resolution_context_state() {}          /* Remove gcc warning */

public:
  /* Save the state of a name resolution context. */
  void save_state(Name_resolution_context *context, TABLE_LIST *table_list)
  {
    save_table_list=                  context->table_list;
    save_first_name_resolution_table= context->first_name_resolution_table;
    save_resolve_in_select_list=      context->resolve_in_select_list;
    save_next_local=                  table_list->next_local;
    save_next_name_resolution_table=  table_list->next_name_resolution_table;
  }

  /* Restore a name resolution context from saved state. */
  void restore_state(Name_resolution_context *context, TABLE_LIST *table_list)
  {
    table_list->next_local=                save_next_local;
    table_list->next_name_resolution_table= save_next_name_resolution_table;
    context->table_list=                   save_table_list;
    context->first_name_resolution_table=  save_first_name_resolution_table;
    context->resolve_in_select_list=       save_resolve_in_select_list;
  }

  TABLE_LIST *get_first_name_resolution_table()
  {
    return save_first_name_resolution_table;
  }
};

class Name_resolution_context_backup
{
  Name_resolution_context &ctx;
  TABLE_LIST &table_list;
  table_map save_map;
  Name_resolution_context_state ctx_state;

public:
  Name_resolution_context_backup(Name_resolution_context &_ctx, TABLE_LIST &_table_list)
    : ctx(_ctx), table_list(_table_list), save_map(_table_list.map)
  {
    ctx_state.save_state(&ctx, &table_list);
    ctx.table_list= &table_list;
    ctx.first_name_resolution_table= &table_list;
  }
  ~Name_resolution_context_backup()
  {
    ctx_state.restore_state(&ctx, &table_list);
    table_list.map= save_map;
  }
};


/*
  This enum is used to report information about monotonicity of function
  represented by Item* tree.
  Monotonicity is defined only for Item* trees that represent table
  partitioning expressions (i.e. have no subselects/user vars/PS parameters
  etc etc). An Item* tree is assumed to have the same monotonicity properties
  as its corresponding function F:

  [signed] longlong F(field1, field2, ...) {
    put values of field_i into table record buffer;
    return item->val_int(); 
  }

  NOTE
  At the moment function monotonicity is not well defined (and so may be
  incorrect) for Item trees with parameters/return types that are different
  from INT_RESULT, may be NULL, or are unsigned.
  It will be possible to address this issue once the related partitioning bugs
  (BUG#16002, BUG#15447, BUG#13436) are fixed.

  The NOT_NULL enums are used in TO_DAYS, since TO_DAYS('2001-00-00') returns
  NULL which puts those rows into the NULL partition, but
  '2000-12-31' < '2001-00-00' < '2001-01-01'. So special handling is needed
  for this (see Bug#20577).
*/

typedef enum monotonicity_info 
{
   NON_MONOTONIC,              /* none of the below holds */
   MONOTONIC_INCREASING,       /* F() is unary and (x < y) => (F(x) <= F(y)) */
   MONOTONIC_INCREASING_NOT_NULL,  /* But only for valid/real x and y */
   MONOTONIC_STRICT_INCREASING,/* F() is unary and (x < y) => (F(x) <  F(y)) */
   MONOTONIC_STRICT_INCREASING_NOT_NULL  /* But only for valid/real x and y */
} enum_monotonicity_info;

/*************************************************************************/

class sp_rcontext;

/**
  A helper class to collect different behavior of various kinds of SP variables:
  - local SP variables and SP parameters
  - PACKAGE BODY routine variables
  - (there will be more kinds in the future)
*/

class Sp_rcontext_handler
{
public:
  virtual ~Sp_rcontext_handler() {}
  /**
    A prefix used for SP variable names in queries:
    - EXPLAIN EXTENDED
    - SHOW PROCEDURE CODE
    Local variables and SP parameters have empty prefixes.
    Package body variables are marked with a special prefix.
    This improves readability of the output of these queries,
    especially when a local variable or a parameter has the same
    name with a package body variable.
  */
  virtual const LEX_CSTRING *get_name_prefix() const= 0;
  /**
    At execution time THD->spcont points to the run-time context (sp_rcontext)
    of the currently executed routine.
    Local variables store their data in the sp_rcontext pointed by thd->spcont.
    Package body variables store data in separate sp_rcontext that belongs
    to the package.
    This method provides access to the proper sp_rcontext structure,
    depending on the SP variable kind.
  */
  virtual sp_rcontext *get_rcontext(sp_rcontext *ctx) const= 0;
};


class Sp_rcontext_handler_local: public Sp_rcontext_handler
{
public:
  const LEX_CSTRING *get_name_prefix() const;
  sp_rcontext *get_rcontext(sp_rcontext *ctx) const;
};


class Sp_rcontext_handler_package_body: public Sp_rcontext_handler
{
public:
  const LEX_CSTRING *get_name_prefix() const;
  sp_rcontext *get_rcontext(sp_rcontext *ctx) const;
};


extern MYSQL_PLUGIN_IMPORT
  Sp_rcontext_handler_local sp_rcontext_handler_local;


extern MYSQL_PLUGIN_IMPORT
  Sp_rcontext_handler_package_body sp_rcontext_handler_package_body;



class Item_equal;

struct st_join_table* const NO_PARTICULAR_TAB= (struct st_join_table*)0x1;

typedef struct replace_equal_field_arg 
{
  Item_equal *item_equal;
  struct st_join_table *context_tab;
} REPLACE_EQUAL_FIELD_ARG;

class Settable_routine_parameter
{
public:
  /*
    Set required privileges for accessing the parameter.

    SYNOPSIS
      set_required_privilege()
        rw        if 'rw' is true then we are going to read and set the
                  parameter, so SELECT and UPDATE privileges might be
                  required, otherwise we only reading it and SELECT
                  privilege might be required.
  */
  Settable_routine_parameter() {}
  virtual ~Settable_routine_parameter() {}
  virtual void set_required_privilege(bool rw) {};

  /*
    Set parameter value.

    SYNOPSIS
      set_value()
        thd       thread handle
        ctx       context to which parameter belongs (if it is local
                  variable).
        it        item which represents new value

    RETURN
      FALSE if parameter value has been set,
      TRUE if error has occurred.
  */
  virtual bool set_value(THD *thd, sp_rcontext *ctx, Item **it)= 0;

  virtual void set_out_param_info(Send_field *info) {}

  virtual const Send_field *get_out_param_info() const
  { return NULL; }

  virtual Item_param *get_item_param() { return 0; }
};


/*
  A helper class to calculate offset and length of a query fragment
  - outside of SP
  - inside an SP
  - inside a compound block
*/
class Query_fragment
{
  uint m_pos;
  uint m_length;
  void set(size_t pos, size_t length)
  {
    DBUG_ASSERT(pos < UINT_MAX32);
    DBUG_ASSERT(length < UINT_MAX32);
    m_pos= (uint) pos;
    m_length= (uint) length;
  }
public:
  Query_fragment(THD *thd, sp_head *sphead, const char *start, const char *end);
  uint pos() const { return m_pos; }
  uint length() const { return m_length; }
};


/**
  This is used for items in the query that needs to be rewritten
  before binlogging

  At the moment this applies to Item_param and Item_splocal
*/
class Rewritable_query_parameter
{
  public:
  /*
    Offset inside the query text.
    Value of 0 means that this object doesn't have to be replaced
    (for example SP variables in control statements)
  */
  my_ptrdiff_t pos_in_query;

  /*
    Byte length of parameter name in the statement.  This is not
    Item::name.length because name.length contains byte length of UTF8-encoded
    name, but the query string is in the client charset.
  */
  uint len_in_query;

  bool limit_clause_param;

  Rewritable_query_parameter(uint pos_in_q= 0, uint len_in_q= 0)
    : pos_in_query(pos_in_q), len_in_query(len_in_q),
      limit_clause_param(false)
  { }

  virtual ~Rewritable_query_parameter() { }

  virtual bool append_for_log(THD *thd, String *str) = 0;
};

class Copy_query_with_rewrite
{
  THD *thd;
  const char *src;
  size_t src_len, from;
  String *dst;

  bool copy_up_to(size_t bytes)
  {
    DBUG_ASSERT(bytes >= from);
    return dst->append(src + from, uint32(bytes - from));
  }

public:

  Copy_query_with_rewrite(THD *t, const char *s, size_t l, String *d)
    :thd(t), src(s), src_len(l), from(0), dst(d) { }

  bool append(Rewritable_query_parameter *p)
  {
    if (copy_up_to(p->pos_in_query) || p->append_for_log(thd, dst))
      return true;
    from= p->pos_in_query + p->len_in_query;
    return false;
  }

  bool finalize()
  { return copy_up_to(src_len); }
};

struct st_dyncall_create_def
{
  Item  *key, *value;
  CHARSET_INFO *cs;
  uint len, frac;
  DYNAMIC_COLUMN_TYPE type;
};

typedef struct st_dyncall_create_def DYNCALL_CREATE_DEF;


typedef bool (Item::*Item_processor) (void *arg);
/*
  Analyzer function
    SYNOPSIS
      argp   in/out IN:  Analysis parameter
                    OUT: Parameter to be passed to the transformer

    RETURN 
      TRUE   Invoke the transformer
      FALSE  Don't do it

*/
typedef bool (Item::*Item_analyzer) (uchar **argp);
typedef Item* (Item::*Item_transformer) (THD *thd, uchar *arg);
typedef void (*Cond_traverser) (const Item *item, void *arg);
typedef bool (Item::*Pushdown_checker) (uchar *arg);

struct st_cond_statistic;

struct find_selective_predicates_list_processor_data
{
  TABLE *table;
  List<st_cond_statistic> list;
};

class MY_LOCALE;

class Item_equal;
class COND_EQUAL;

class st_select_lex_unit;

class Item_func_not;
class Item_splocal;

/**
  String_copier that sends Item specific warnings.
*/
class String_copier_for_item: public String_copier
{
  THD *m_thd;
public:
  bool copy_with_warn(CHARSET_INFO *dstcs, String *dst,
                      CHARSET_INFO *srccs, const char *src,
                      uint32 src_length, uint32 nchars);
  String_copier_for_item(THD *thd): m_thd(thd) { }
};


/**
  A helper class describing what kind of Item created a temporary field.
  - If m_field is set, then the temporary field was created from Field
    (e.g. when the Item was Item_field, or Item_ref pointing to Item_field)
  - If m_default_field is set, then there is a usable DEFAULT value.
    (e.g. when the Item is Item_field)
  - If m_item_result_field is set, then the temporary field was created
    from certain sub-types of Item_result_field (e.g. Item_func)
  See create_tmp_field() in sql_select.cc for details.
*/

class Tmp_field_src
{
  Field *m_field;
  Field *m_default_field;
  Item_result_field *m_item_result_field;
public:
  Tmp_field_src()
   :m_field(0),
    m_default_field(0),
    m_item_result_field(0)
  { }
  Field *field() const { return m_field; }
  Field *default_field() const { return m_default_field; }
  Item_result_field *item_result_field() const { return m_item_result_field; }
  void set_field(Field *field) { m_field= field; }
  void set_default_field(Field *field) { m_default_field= field; }
  void set_item_result_field(Item_result_field *item)
  { m_item_result_field= item; }
};


/**
  Parameters for create_tmp_field_ex().
  See create_tmp_field() in sql_select.cc for details.
*/

class Tmp_field_param
{
  bool m_group;
  bool m_modify_item;
  bool m_table_cant_handle_bit_fields;
  bool m_make_copy_field;
public:
  Tmp_field_param(bool group,
                  bool modify_item,
                  bool table_cant_handle_bit_fields,
                  bool make_copy_field)
   :m_group(group),
    m_modify_item(modify_item),
    m_table_cant_handle_bit_fields(table_cant_handle_bit_fields),
    m_make_copy_field(make_copy_field)
  { }
  bool group() const { return m_group; }
  bool modify_item() const { return m_modify_item; }
  bool table_cant_handle_bit_fields() const
  { return m_table_cant_handle_bit_fields; }
  bool make_copy_field() const { return m_make_copy_field; }
  void set_modify_item(bool to) { m_modify_item= to; }
};


class Item_const
{
public:
  virtual ~Item_const() {}
  virtual const Type_all_attributes *get_type_all_attributes_from_const() const= 0;
  virtual bool const_is_null() const { return false; }
  virtual const longlong *const_ptr_longlong() const { return NULL; }
  virtual const double *const_ptr_double() const { return NULL; }
  virtual const my_decimal *const_ptr_my_decimal() const { return NULL; }
  virtual const MYSQL_TIME *const_ptr_mysql_time() const { return NULL; }
  virtual const String *const_ptr_string() const { return NULL; }
};


/****************************************************************************/

#define STOP_PTR ((void *) 1)

/* Base flags (including IN) for an item */

typedef uint8 item_flags_t;

enum class item_base_t : item_flags_t
{
  NONE=                  0,
#define ITEM_FLAGS_MAYBE_NULL_SHIFT 0 // Must match MAYBE_NULL
  MAYBE_NULL=            (1<<0),   // May be NULL.
  IN_ROLLUP=             (1<<1),   // Appears in GROUP BY list
                                   // of a query with ROLLUP.
  FIXED=                 (1<<2),   // Was fixed with fix_fields().
  IS_EXPLICIT_NAME=      (1<<3),   // The name of this Item was set by the user
                                   // (or was auto generated otherwise)
  IS_IN_WITH_CYCLE=      (1<<4)    // This item is in CYCLE clause
                                   // of WITH.
};


/* Flags that tells us what kind of items the item contains */

enum class item_with_t : item_flags_t
{
  NONE=             0,
  SP_VAR=      (1<<0), // If Item contains a stored procedure variable
  WINDOW_FUNC= (1<<1), // If item contains a window func
  FIELD=       (1<<2), // If any item except Item_sum contains a field.
  SUM_FUNC=    (1<<3), // If item contains a sum func
  SUBQUERY=    (1<<4), // If item containts a sub query
  ROWNUM_FUNC= (1<<5)
};


/* Make operations in item_base_t and item_with_t work like 'int' */
static inline item_base_t operator&(const item_base_t a, const item_base_t b)
{
  return (item_base_t) (((item_flags_t) a) & ((item_flags_t) b));
}

static inline item_base_t & operator&=(item_base_t &a, item_base_t b)
{
  a= (item_base_t) (((item_flags_t) a) & (item_flags_t) b);
  return a;
}

static inline item_base_t operator|(const item_base_t a, const item_base_t b)
{
  return (item_base_t) (((item_flags_t) a) | ((item_flags_t) b));
}

static inline item_base_t & operator|=(item_base_t &a, item_base_t b)
{
  a= (item_base_t) (((item_flags_t) a) | (item_flags_t) b);
  return a;
}

static inline item_base_t operator~(const item_base_t a)
{
  return (item_base_t) ~(item_flags_t) a;
}

static inline item_with_t operator&(const item_with_t a, const item_with_t b)
{
  return (item_with_t) (((item_flags_t) a) & ((item_flags_t) b));
}

static inline item_with_t & operator&=(item_with_t &a, item_with_t b)
{
  a= (item_with_t) (((item_flags_t) a) & (item_flags_t) b);
  return a;
}

static inline item_with_t operator|(const item_with_t a, const item_with_t b)
{
  return (item_with_t) (((item_flags_t) a) | ((item_flags_t) b));
}

static inline item_with_t & operator|=(item_with_t &a, item_with_t b)
{
  a= (item_with_t) (((item_flags_t) a) | (item_flags_t) b);
  return a;
}

static inline item_with_t operator~(const item_with_t a)
{
  return (item_with_t) ~(item_flags_t) a;
}


class Item :public Value_source,
            public Type_all_attributes
{
  static void *operator new(size_t size);

public:
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void operator delete(void *ptr,size_t size) { TRASH_FREE(ptr, size); }
  static void operator delete(void *ptr, MEM_ROOT *mem_root) {}

  enum Type {FIELD_ITEM= 0, FUNC_ITEM, SUM_FUNC_ITEM,
             WINDOW_FUNC_ITEM,
             /*
               NOT NULL literal-alike constants, which do not change their
               value during an SQL statement execution, but can optionally
               change their value between statements:
               - Item_literal               - real NOT NULL constants
               - Item_param                 - can change between statements
               - Item_splocal               - can change between statements
               - Item_user_var_as_out_param - hack
               Note, Item_user_var_as_out_param actually abuses the type code.
               It should be moved out of the Item tree eventually.
             */
             CONST_ITEM,
             NULL_ITEM,     // Item_null or Item_param bound to NULL
             COPY_STR_ITEM, FIELD_AVG_ITEM, DEFAULT_VALUE_ITEM,
             CONTEXTUALLY_TYPED_VALUE_ITEM,
             PROC_ITEM,COND_ITEM, REF_ITEM, FIELD_STD_ITEM,
             FIELD_VARIANCE_ITEM, INSERT_VALUE_ITEM,
             SUBSELECT_ITEM, ROW_ITEM, CACHE_ITEM, TYPE_HOLDER,
             PARAM_ITEM, TRIGGER_FIELD_ITEM,
             EXPR_CACHE_ITEM};

  enum cond_result { COND_UNDEF,COND_OK,COND_TRUE,COND_FALSE };
  enum traverse_order { POSTFIX, PREFIX };

protected:
  SEL_TREE *get_mm_tree_for_const(RANGE_OPT_PARAM *param);

  /**
    Create a field based on the exact data type handler.
  */
  Field *create_table_field_from_handler(MEM_ROOT *root, TABLE *table)
  {
    const Type_handler *h= type_handler();
    return h->make_and_init_table_field(root, &name,
                                        Record_addr(maybe_null()),
                                        *this, table);
  }
  /**
    Create a field based on field_type of argument.
    This is used to create a field for
    - IFNULL(x,something)
    - time functions
    - prepared statement placeholders
    - SP variables with data type references: DECLARE a TYPE OF t1.a;
    @retval  NULL  error
    @retval  !NULL on success
  */
  Field *tmp_table_field_from_field_type(MEM_ROOT *root, TABLE *table)
  {
    DBUG_ASSERT(fixed());
    const Type_handler *h= type_handler()->type_handler_for_tmp_table(this);
    return h->make_and_init_table_field(root, &name,
                                        Record_addr(maybe_null()),
                                        *this, table);
  }
  /**
    Create a temporary field for a simple Item, which does not
    need any special action after the field creation:
    - is not an Item_field descendant (and not a reference to Item_field)
    - is not an Item_result_field descendant
    - does not need to copy any DEFAULT value to the result Field
    - does not need to set Field::is_created_from_null_item for the result
    See create_tmp_field_ex() for details on parameters and return values.
  */
  Field *create_tmp_field_ex_simple(MEM_ROOT *root,
                                    TABLE *table,
                                    Tmp_field_src *src,
                                    const Tmp_field_param *param)
  {
    DBUG_ASSERT(!param->make_copy_field());
    DBUG_ASSERT(!is_result_field());
    DBUG_ASSERT(type() != NULL_ITEM);
    return tmp_table_field_from_field_type(root, table);
  }
  Field *create_tmp_field_int(MEM_ROOT *root, TABLE *table,
                              uint convert_int_length);
  Field *tmp_table_field_from_field_type_maybe_null(MEM_ROOT *root,
                                            TABLE *table,
                                            Tmp_field_src *src,
                                            const Tmp_field_param *param,
                                            bool is_explicit_null);

  void raise_error_not_evaluable();
  void push_note_converted_to_negative_complement(THD *thd);
  void push_note_converted_to_positive_complement(THD *thd);

  /* Helper methods, to get an Item value from another Item */
  double val_real_from_item(Item *item)
  {
    DBUG_ASSERT(fixed());
    double value= item->val_real();
    null_value= item->null_value;
    return value;
  }
  longlong val_int_from_item(Item *item)
  {
    DBUG_ASSERT(fixed());
    longlong value= item->val_int();
    null_value= item->null_value;
    return value;
  }
  String *val_str_from_item(Item *item, String *str)
  {
    DBUG_ASSERT(fixed());
    String *res= item->val_str(str);
    if (res)
      res->set_charset(collation.collation);
    if ((null_value= item->null_value))
      res= NULL;
    return res;
  }
  bool val_native_from_item(THD *thd, Item *item, Native *to)
  {
    DBUG_ASSERT(fixed());
    null_value= item->val_native(thd, to);
    DBUG_ASSERT(null_value == item->null_value);
    return null_value;
  }
  bool val_native_from_field(Field *field, Native *to)
  {
    if ((null_value= field->is_null()))
      return true;
    return (null_value= field->val_native(to));
  }
  bool val_native_with_conversion_from_item(THD *thd, Item *item, Native *to,
                                            const Type_handler *handler)
  {
    DBUG_ASSERT(fixed());
    return (null_value= item->val_native_with_conversion(thd, to, handler));
  }
  my_decimal *val_decimal_from_item(Item *item, my_decimal *decimal_value)
  {
    DBUG_ASSERT(fixed());
    my_decimal *value= item->val_decimal(decimal_value);
    if ((null_value= item->null_value))
      value= NULL;
    return value;
  }
  bool get_date_from_item(THD *thd, Item *item,
                          MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    bool rc= item->get_date(thd, ltime, fuzzydate);
    null_value= MY_TEST(rc || item->null_value);
    return rc;
  }
public:

  /*
    Cache val_str() into the own buffer, e.g. to evaluate constant
    expressions with subqueries in the ORDER/GROUP clauses.
  */
  String *val_str() { return val_str(&str_value); }
  virtual Item_func *get_item_func() { return NULL; }

  const MY_LOCALE *locale_from_val_str();

  /* All variables for the Item class */

  /**
     Intrusive list pointer for free list. If not null, points to the next
     Item on some Query_arena's free list. For instance, stored procedures
     have their own Query_arena's.

     @see Query_arena::free_list
   */
  Item *next;

  /*
    str_values's main purpose is to be used to cache the value in
    save_in_field. Calling full_name() for Item_field will also use str_value.
  */
  String str_value;

  LEX_CSTRING name;			/* Name of item */
  /* Original item name (if it was renamed)*/
  const char *orig_name;

  /* All common bool variables for an Item is stored here */
  item_base_t base_flags;
  item_with_t with_flags;

   /* Marker is used in some functions to temporary mark an item */
  int16 marker;

  /*
    Tells is the val() value of the item is/was null.
    This should not be part of the bit flags as it's changed a lot and also
    we use pointers to it
  */
  bool null_value;
  /* Cache of the result of is_expensive(). */
  int8 is_expensive_cache;
  /**
    The index in the JOIN::join_tab array of the JOIN_TAB this Item
    is attached to. Items are attached (or 'pushed') to JOIN_TABs
    during optimization by the make_cond_for_table procedure. During
    query execution, this item is evaluated when the join loop reaches
    the corresponding JOIN_TAB.

    If the value of join_tab_idx >= MAX_TABLES, this means that there is no
    corresponding JOIN_TAB.
  */
  uint8 join_tab_idx;

  inline bool maybe_null() const
  { return (bool) (base_flags & item_base_t::MAYBE_NULL); }
  inline bool in_rollup() const
  { return (bool) (base_flags & item_base_t::IN_ROLLUP); }
  inline bool fixed() const
  { return (bool) (base_flags & item_base_t::FIXED); }
  inline bool is_explicit_name() const
  { return (bool) (base_flags & item_base_t::IS_EXPLICIT_NAME); }
  inline bool is_in_with_cycle() const
  { return (bool) (base_flags & item_base_t::IS_IN_WITH_CYCLE); }

  inline bool with_sp_var() const
  { return (bool) (with_flags & item_with_t::SP_VAR); }
  inline bool with_window_func() const
  { return (bool) (with_flags & item_with_t::WINDOW_FUNC); }
  inline bool with_field() const
  { return (bool) (with_flags & item_with_t::FIELD); }
  inline bool with_sum_func() const
  { return (bool) (with_flags & item_with_t::SUM_FUNC); }
  inline bool with_subquery() const
  { return (bool) (with_flags & item_with_t::SUBQUERY); }
  inline bool with_rownum_func() const
  { return (bool) (with_flags & item_with_t::ROWNUM_FUNC); }
  inline void copy_flags(const Item *org, item_base_t mask)
  {
    base_flags= (item_base_t) (((item_flags_t) base_flags &
                                ~(item_flags_t) mask) |
                               ((item_flags_t) org->base_flags &
                                (item_flags_t) mask));
  }
  inline void copy_flags(const Item *org, item_with_t mask)
  {
    with_flags= (item_with_t) (((item_flags_t) with_flags &
                                ~(item_flags_t) mask) |
                               ((item_flags_t) org->with_flags &
                                (item_flags_t) mask));
  }

  // alloc & destruct is done as start of select on THD::mem_root
  Item(THD *thd);
  /*
     Constructor used by Item_field, Item_ref & aggregate (sum) functions.
     Used for duplicating lists in processing queries with temporary
     tables
     Also it used for Item_cond_and/Item_cond_or for creating
     top AND/OR structure of WHERE clause to protect it of
     optimisation changes in prepared statements
  */
  Item(THD *thd, Item *item);
  Item();                                        /* For const item */
  virtual ~Item()
  {
#ifdef EXTRA_DEBUG
    name.str= 0;
    name.length= 0;
#endif
  }		/*lint -e1509 */
  void set_name(THD *thd, const char *str, size_t length, CHARSET_INFO *cs);
  void set_name(THD *thd, String *str)
  {
    set_name(thd, str->ptr(), str->length(), str->charset());
  }
  void set_name(THD *thd, const LEX_CSTRING &str,
                CHARSET_INFO *cs= system_charset_info)
  {
    set_name(thd, str.str, str.length, cs);
  }
  void set_name_no_truncate(THD *thd, const char *str, uint length,
                            CHARSET_INFO *cs);
  void init_make_send_field(Send_field *tmp_field, const Type_handler *h);
  void share_name_with(const Item *item)
  {
    name= item->name;
    copy_flags(item, item_base_t::IS_EXPLICIT_NAME);
  }
  virtual void cleanup();
  virtual void make_send_field(THD *thd, Send_field *field);

  bool fix_fields_if_needed(THD *thd, Item **ref)
  {
    return fixed() ? false : fix_fields(thd, ref);
  }
  bool fix_fields_if_needed_for_scalar(THD *thd, Item **ref)
  {
    return fix_fields_if_needed(thd, ref) || check_cols(1);
  }
  bool fix_fields_if_needed_for_bool(THD *thd, Item **ref)
  {
    return fix_fields_if_needed_for_scalar(thd, ref);
  }
  bool fix_fields_if_needed_for_order_by(THD *thd, Item **ref)
  {
    return fix_fields_if_needed_for_scalar(thd, ref);
  }
  /*
    By default we assume that an Item is fixed by the constructor
  */
  virtual bool fix_fields(THD *, Item **)
  {
    /*
      This should not normally be called, because usually before
      fix_fields() we check fixed() to be false.
      But historically we allow fix_fields() to be called for Items
      who return basic_const_item()==true.
    */
    DBUG_ASSERT(fixed());
    DBUG_ASSERT(basic_const_item());
    return false;
  }
  virtual void unfix_fields()
  {
    DBUG_ASSERT(0);
  }

  /*
    Fix after some tables has been pulled out. Basically re-calculate all
    attributes that are dependent on the tables.
  */
  virtual void fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                 bool merge)
    {};

  /*
    This is for items that require a fixup after the JOIN::prepare()
    is done.
  */
  virtual void fix_after_optimize(THD *thd)
  {}
  /*
    This method should be used in case where we are sure that we do not need
    complete fix_fields() procedure.
    Usually this method is used by the optimizer when it has to create a new
    item out of other already fixed items. For example, if the optimizer has
    to create a new Item_func for an inferred equality whose left and right
    parts are already fixed items. In some cases the optimizer cannot use
    directly fixed items as the arguments of the created functional item, 
    but rather uses intermediate type conversion items. Then the method is
    supposed to be applied recursively.  
  */
  virtual void quick_fix_field()
  {
    DBUG_ASSERT(0);
  }

  bool save_in_value(THD *thd, st_value *value)
  {
    return type_handler()->Item_save_in_value(thd, this, value);
  }

  /* Function returns 1 on overflow and -1 on fatal errors */
  int save_in_field_no_warnings(Field *field, bool no_conversions);
  virtual int save_in_field(Field *field, bool no_conversions);
  virtual bool save_in_param(THD *thd, Item_param *param);
  virtual void save_org_in_field(Field *field,
                                 fast_field_copier data
                                 __attribute__ ((__unused__)))
  { (void) save_in_field(field, 1); }
  virtual fast_field_copier setup_fast_field_copier(Field *field)
  { return NULL; }
  virtual int save_safe_in_field(Field *field)
  { return save_in_field(field, 1); }
  virtual bool send(Protocol *protocol, st_value *buffer)
  {
    return type_handler()->Item_send(this, protocol, buffer);
  }
  virtual bool eq(const Item *, bool binary_cmp) const;
  enum_field_types field_type() const
  {
    return type_handler()->field_type();
  }
  virtual const Type_handler *type_handler() const= 0;
  /**
    Detects if an Item has a fixed data type which is known
    even before fix_fields().
    Currently it's important only to find Items with a fixed boolean
    data type. More item types can be marked in the future as having
    a fixed data type (e.g. all literals, all fixed type functions, etc).

    @retval  NULL if the Item type is not known before fix_fields()
    @retval  the pointer to the data type handler, if the data type
             is known before fix_fields().
  */
  virtual const Type_handler *fixed_type_handler() const
  {
    return NULL;
  }
  const Type_handler *type_handler_for_comparison() const
  {
    return type_handler()->type_handler_for_comparison();
  }
  virtual const Type_handler *real_type_handler() const
  {
    return type_handler();
  }
  const Type_handler *cast_to_int_type_handler() const
  {
    return real_type_handler()->cast_to_int_type_handler();
  }
  /* result_type() of an item specifies how the value should be returned */
  Item_result result_type() const
  {
    return type_handler()->result_type();
  }
  /* ... while cmp_type() specifies how it should be compared */
  Item_result cmp_type() const
  {
    return type_handler()->cmp_type();
  }
  const Type_handler *string_type_handler() const
  {
    return Type_handler::string_type_handler(max_length);
  }
  /*
    Calculate the maximum length of an expression.
    This method is used in data type aggregation for UNION, e.g.:
      SELECT 'b' UNION SELECT COALESCE(double_10_3_field) FROM t1;

    The result is usually equal to max_length, except for some numeric types.
    In case of the INT, FLOAT, DOUBLE data types Item::max_length and
    Item::decimals are ignored, so the returned value depends only on the
    data type itself. E.g. for an expression of the DOUBLE(10,3) data type,
    the result is always 53 (length 10 and precision 3 do not matter).

    max_length is ignored for these numeric data types because the length limit
    means only "expected maximum length", it is not a hard limit, so it does
    not impose any data truncation. E.g. a column of the type INT(4) can
    normally store big values up to 2147483647 without truncation. When we're
    aggregating such column for UNION it's important to create a long enough
    result column, not to lose any data.

    For detailed behaviour of various data types see implementations of
    the corresponding Type_handler_xxx::max_display_length().

    Note, Item_field::max_display_length() overrides this to get
    max_display_length() from the underlying field.
  */
  virtual uint32 max_display_length() const
  {
    return type_handler()->max_display_length(this);
  }
  const TYPELIB *get_typelib() const override { return NULL; }
  /* optimized setting of maybe_null without jumps. Minimizes code size */
  inline void set_maybe_null(bool maybe_null_arg)
  {
    base_flags= ((item_base_t) ((base_flags & ~item_base_t::MAYBE_NULL)) |
                 (item_base_t) (maybe_null_arg <<
                                ITEM_FLAGS_MAYBE_NULL_SHIFT));
  }
  /* This is used a lot, so make it simpler to use */
  void set_maybe_null()
  {
    base_flags|= item_base_t::MAYBE_NULL;
  }
  /* This is used when calling Type_all_attributes::set_type_maybe_null() */
  void set_type_maybe_null(bool maybe_null_arg) override
  {
    set_maybe_null(maybe_null_arg);
  }

  void set_typelib(const TYPELIB *typelib) override
  {
    // Non-field Items (e.g. hybrid functions) never have ENUM/SET types yet.
    DBUG_ASSERT(0);
  }
  Item_cache* get_cache(THD *thd) const
  {
    return type_handler()->Item_get_cache(thd, this);
  }
  virtual enum Type type() const =0;
  bool is_of_type(Type t, Item_result cmp) const
  {
    return type() == t && cmp_type() == cmp;
  }
  /*
    real_type() is the type of base item.  This is same as type() for
    most items, except Item_ref() and Item_cache_wrapper() where it
    shows the type for the underlying item.
  */
  virtual enum Type real_type() const { return type(); }
  
  /*
    Return information about function monotonicity. See comment for
    enum_monotonicity_info for details. This function can only be called
    after fix_fields() call.
  */
  virtual enum_monotonicity_info get_monotonicity_info() const
  { return NON_MONOTONIC; }

  /*
    Convert "func_arg $CMP$ const" half-interval into
            "FUNC(func_arg) $CMP2$ const2"

    SYNOPSIS
      val_int_endpoint()
        left_endp  FALSE  <=> The interval is "x < const" or "x <= const"
                   TRUE   <=> The interval is "x > const" or "x >= const"

        incl_endp  IN   FALSE <=> the comparison is '<' or '>'
                        TRUE  <=> the comparison is '<=' or '>='
                   OUT  The same but for the "F(x) $CMP$ F(const)" comparison

    DESCRIPTION
      This function is defined only for unary monotonic functions. The caller
      supplies the source half-interval

         x $CMP$ const

      The value of const is supplied implicitly as the value this item's
      argument, the form of $CMP$ comparison is specified through the
      function's arguments. The calle returns the result interval
         
         F(x) $CMP2$ F(const)
      
      passing back F(const) as the return value, and the form of $CMP2$ 
      through the out parameter. NULL values are assumed to be comparable and
      be less than any non-NULL values.

    RETURN
      The output range bound, which equal to the value of val_int()
        - If the value of the function is NULL then the bound is the 
          smallest possible value of LONGLONG_MIN 
  */
  virtual longlong val_int_endpoint(bool left_endp, bool *incl_endp)
  { DBUG_ASSERT(0); return 0; }


  /* valXXX methods must return NULL or 0 or 0.0 if null_value is set. */
  /*
    Return double precision floating point representation of item.

    SYNOPSIS
      val_real()

    RETURN
      In case of NULL value return 0.0 and set null_value flag to TRUE.
      If value is not null null_value flag will be reset to FALSE.
  */
  virtual double val_real()=0;
  Double_null to_double_null()
  {
    // val_real() must be caleed on a separate line. See to_longlong_null()
    double nr= val_real();
    return Double_null(nr, null_value);
  }
  /*
    Return integer representation of item.

    SYNOPSIS
      val_int()

    RETURN
      In case of NULL value return 0 and set null_value flag to TRUE.
      If value is not null null_value flag will be reset to FALSE.
  */
  virtual longlong val_int()=0;
  Longlong_hybrid to_longlong_hybrid()
  {
    return Longlong_hybrid(val_int(), unsigned_flag);
  }
  Longlong_null to_longlong_null()
  {
    longlong nr= val_int();
    /*
      C++ does not guarantee the order of parameter evaluation,
      so to make sure "null_value" is passed to the constructor
      after the val_int() call, val_int() is caled on a separate line.
    */
    return Longlong_null(nr, null_value);
  }
  Longlong_hybrid_null to_longlong_hybrid_null()
  {
    return Longlong_hybrid_null(to_longlong_null(), unsigned_flag);
  }
  /**
    Get a value for CAST(x AS SIGNED).
    Too large positive unsigned integer values are converted
    to negative complements.
    Values of non-integer data types are adjusted to the SIGNED range.
  */
  virtual longlong val_int_signed_typecast()
  {
    return cast_to_int_type_handler()->Item_val_int_signed_typecast(this);
  }
  longlong val_int_signed_typecast_from_str();
  /**
    Get a value for CAST(x AS UNSIGNED).
    Negative signed integer values are converted
    to positive complements.
    Values of non-integer data types are adjusted to the UNSIGNED range.
  */
  virtual longlong val_int_unsigned_typecast()
  {
    return cast_to_int_type_handler()->Item_val_int_unsigned_typecast(this);
  }
  longlong val_int_unsigned_typecast_from_int();
  longlong val_int_unsigned_typecast_from_str();
  longlong val_int_unsigned_typecast_from_real();

  /**
    Get a value for CAST(x AS UNSIGNED).
    Huge positive unsigned values are converted to negative complements.
  */
  longlong val_int_signed_typecast_from_int();
  longlong val_int_signed_typecast_from_real();

  /*
    This is just a shortcut to avoid the cast. You should still use
    unsigned_flag to check the sign of the item.
  */
  inline ulonglong val_uint() { return (ulonglong) val_int(); }

  /*
    Return string representation of this item object.

    SYNOPSIS
      val_str()
      str   an allocated buffer this or any nested Item object can use to
            store return value of this method.

    NOTE
      The caller can modify the returned String, if it's not marked
      "const" (with the String::mark_as_const() method). That means that
      if the item returns its own internal buffer (e.g. tmp_value), it
      *must* be marked "const" [1]. So normally it's preferable to
      return the result value in the String, that was passed as an
      argument. But, for example, SUBSTR() returns a String that simply
      points into the buffer of SUBSTR()'s args[0]->val_str(). Such a
      String is always "const", so it's ok to use tmp_value for that and
      avoid reallocating/copying of the argument String.

      [1] consider SELECT CONCAT(f, ":", f) FROM (SELECT func() AS f);
      here the return value of f() is used twice in the top-level
      select, and if they share the same tmp_value buffer, modifying the
      first one will implicitly modify the second too.

    RETURN
      In case of NULL value return 0 (NULL pointer) and set null_value flag
      to TRUE.
      If value is not null null_value flag will be reset to FALSE.
  */
  virtual String *val_str(String *str)=0;


  bool val_native_with_conversion(THD *thd, Native *to, const Type_handler *th)
  {
    return th->Item_val_native_with_conversion(thd, this, to);
  }
  bool val_native_with_conversion_result(THD *thd, Native *to,
                                         const Type_handler *th)
  {
    return th->Item_val_native_with_conversion_result(thd, this, to);
  }

  virtual bool val_native(THD *thd, Native *to)
  {
   /*
     The default implementation for the Items that do not need native format:
     - Item_basic_value (default implementation)
     - Item_copy
     - Item_exists_subselect
     - Item_sum_field
     - Item_sum_or_func (default implementation)
     - Item_proc
     - Item_type_holder (as val_xxx() are never called for it);

     These hybrid Item types override val_native():
     - Item_field
     - Item_param
     - Item_sp_variable
     - Item_ref
     - Item_cache_wrapper
     - Item_direct_ref
     - Item_direct_view_ref
     - Item_ref_null_helper
     - Item_name_const
     - Item_time_literal
     - Item_sum_or_func
         Note, these hybrid type Item_sum_or_func descendants
         override the default implementation:
         * Item_sum_hybrid
         * Item_func_hybrid_field_type
         * Item_func_min_max
         * Item_func_sp
         * Item_func_last_value
         * Item_func_rollup_const
   */
    DBUG_ASSERT(0);
    return (null_value= 1);
  }
  virtual bool val_native_result(THD *thd, Native *to)
  {
    return val_native(thd, to);
  }

  /*
    Returns string representation of this item in ASCII format.

    SYNOPSIS
      val_str_ascii()
      str - similar to val_str();

    NOTE
      This method is introduced for performance optimization purposes.

      1. val_str() result of some Items in string context
      depends on @@character_set_results.
      @@character_set_results can be set to a "real multibyte" character
      set like UCS2, UTF16, UTF32. (We'll use only UTF32 in the examples
      below for convenience.)

      So the default string result of such functions
      in these circumstances is real multi-byte character set, like UTF32.

      For example, all numbers in string context
      return result in @@character_set_results:

      SELECT CONCAT(20010101); -> UTF32

      We do sprintf() first (to get ASCII representation)
      and then convert to UTF32;
      
      So these kind "data sources" can use ASCII representation
      internally, but return multi-byte data only because
      @@character_set_results wants so.
      Therefore, conversion from ASCII to UTF32 is applied internally.


      2. Some other functions need in fact ASCII input.

      For example,
        inet_aton(), GeometryFromText(), Convert_TZ(), GET_FORMAT().

      Similar, fields of certain type, like DATE, TIME,
      when you insert string data into them, expect in fact ASCII input.
      If they get non-ASCII input, for example UTF32, they
      convert input from UTF32 to ASCII, and then use ASCII
      representation to do further processing.


      3. Now imagine we pass result of a data source of the first type
         to a data destination of the second type.

      What happens:
        a. data source converts data from ASCII to UTF32, because
           @@character_set_results wants so and passes the result to
           data destination.
        b. data destination gets UTF32 string.
        c. data destination converts UTF32 string to ASCII,
           because it needs ASCII representation to be able to handle data
           correctly.

      As a result we get two steps of unnecessary conversion:
      From ASCII to UTF32, then from UTF32 to ASCII.

      A better way to handle these situations is to pass ASCII
      representation directly from the source to the destination.

      This is why val_str_ascii() introduced.

    RETURN
      Similar to val_str()
  */
  virtual String *val_str_ascii(String *str);

  /*
    Returns the result of val_str_ascii(), translating NULLs back
    to empty strings (if MODE_EMPTY_STRING_IS_NULL is set).
  */
  String *val_str_ascii_revert_empty_string_is_null(THD *thd, String *str);

  /*
    Returns the val_str() value converted to the given character set.
  */
  String *val_str(String *str, String *converter, CHARSET_INFO *to);

  virtual String *val_json(String *str) { return val_str(str); }
  /*
    Return decimal representation of item with fixed point.

    SYNOPSIS
      val_decimal()
      decimal_buffer  buffer which can be used by Item for returning value
                      (but can be not)

    NOTE
      Returned value should not be changed if it is not the same which was
      passed via argument.

    RETURN
      Return pointer on my_decimal (it can be other then passed via argument)
        if value is not NULL (null_value flag will be reset to FALSE).
      In case of NULL value it return 0 pointer and set null_value flag
        to TRUE.
  */
  virtual my_decimal *val_decimal(my_decimal *decimal_buffer)= 0;
  /*
    Return boolean value of item.

    RETURN
      FALSE value is false or NULL
      TRUE value is true (not equal to 0)
  */
  virtual bool val_bool()
  {
    return type_handler()->Item_val_bool(this);
  }

  bool eval_const_cond()
  {
    DBUG_ASSERT(const_item());
    DBUG_ASSERT(!is_expensive());
    return val_bool();
  }
  bool can_eval_in_optimize()
  {
    return const_item() && !is_expensive();
  }

  /*
    save_val() is method of val_* family which stores value in the given
    field.
  */
  virtual void save_val(Field *to) { save_org_in_field(to, NULL); }
  /*
    save_result() is method of val*result() family which stores value in
    the given field.
  */
  virtual void save_result(Field *to) { save_val(to); }
  /* Helper functions, see item_sum.cc */
  String *val_string_from_real(String *str);
  String *val_string_from_int(String *str);
  my_decimal *val_decimal_from_real(my_decimal *decimal_value);
  my_decimal *val_decimal_from_int(my_decimal *decimal_value);
  my_decimal *val_decimal_from_string(my_decimal *decimal_value);
  longlong val_int_from_real()
  {
    DBUG_ASSERT(fixed());
    return Converter_double_to_longlong_with_warn(val_real(), false).result();
  }
  longlong val_int_from_str(int *error);

  /*
    Returns true if this item can be calculated during
    value_depends_on_sql_mode()
  */
  bool value_depends_on_sql_mode_const_item()
  {
    /*
      Currently we use value_depends_on_sql_mode() only for virtual
      column expressions. They should not contain any expensive items.
      If we ever get a crash on the assert below, it means
      check_vcol_func_processor() is badly implemented for this item.
    */
    DBUG_ASSERT(!is_expensive());
    /*
      It should return const_item() actually.
      But for some reasons Item_field::const_item() returns true
      at value_depends_on_sql_mode() call time.
      This should be checked and fixed.
    */
    return basic_const_item();
  }
  virtual Sql_mode_dependency value_depends_on_sql_mode() const
  {
    return Sql_mode_dependency();
  }

  int save_time_in_field(Field *field, bool no_conversions);
  int save_date_in_field(Field *field, bool no_conversions);
  int save_str_in_field(Field *field, bool no_conversions);
  int save_real_in_field(Field *field, bool no_conversions);
  int save_int_in_field(Field *field, bool no_conversions);
  int save_decimal_in_field(Field *field, bool no_conversions);

  int save_str_value_in_field(Field *field, String *result);

  virtual Field *get_tmp_table_field() { return 0; }
  virtual Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table);
  inline const char *full_name() const { return full_name_cstring().str; }
  virtual LEX_CSTRING full_name_cstring() const
  {
    if (name.str)
      return name;
    return { STRING_WITH_LEN("???") };
  }
  const char *field_name_or_null()
  { return real_item()->type() == Item::FIELD_ITEM ? name.str : NULL; }
  const TABLE_SHARE *field_table_or_null();

  /*
    *result* family of methods is analog of *val* family (see above) but
    return value of result_field of item if it is present. If Item have not
    result field, it return val(). This methods set null_value flag in same
    way as *val* methods do it.
  */
  virtual double  val_result() { return val_real(); }
  virtual longlong val_int_result() { return val_int(); }
  virtual String *str_result(String* tmp) { return val_str(tmp); }
  virtual my_decimal *val_decimal_result(my_decimal *val)
  { return val_decimal(val); }
  virtual bool val_bool_result() { return val_bool(); }
  virtual bool is_null_result() { return is_null(); }
  /*
    Returns 1 if result type and collation for val_str() can change between
    calls
  */
  virtual bool dynamic_result() { return 0; }
  /* 
    Bitmap of tables used by item
    (note: if you need to check dependencies on individual columns, check out
     class Field_enumerator)
  */
  virtual table_map used_tables() const { return (table_map) 0L; }
  virtual table_map all_used_tables() const { return used_tables(); }
  /*
    Return table map of tables that can't be NULL tables (tables that are
    used in a context where if they would contain a NULL row generated
    by a LEFT or RIGHT join, the item would not be true).
    This expression is used on WHERE item to determinate if a LEFT JOIN can be
    converted to a normal join.
    Generally this function should return used_tables() if the function
    would return null if any of the arguments are null
    As this is only used in the beginning of optimization, the value don't
    have to be updated in update_used_tables()
  */
  virtual table_map not_null_tables() const { return used_tables(); }
  /*
    Returns true if this is a simple constant item like an integer, not
    a constant expression. Used in the optimizer to propagate basic constants.
  */
  virtual bool basic_const_item() const { return 0; }
  /**
    Determines if the expression is allowed as
    a virtual column assignment source:
      INSERT INTO t1 (vcol) VALUES (10)    -> error
      INSERT INTO t1 (vcol) VALUES (NULL)  -> ok
  */
  virtual bool vcol_assignment_allowed_value() const { return false; }
  /**
    Test if "this" is an ORDER position (rather than an expression).
    Notes:
    - can be called before fix_fields().
    - local SP variables (even of integer types) are always expressions, not
      positions. (And they can't be used before fix_fields is called for them).
  */
  virtual bool is_order_clause_position() const { return false; }
  /*
    Determines if the Item is an evaluable expression, that is
    it can return a value, so we can call methods val_xxx(), get_date(), etc.
    Most items are evaluable expressions.
    Examples of non-evaluable expressions:
    - Item_contextually_typed_value_specification (handling DEFAULT and IGNORE)
    - Item_type_param bound to DEFAULT and IGNORE
    We cannot call the mentioned methods for these Items,
    their method implementations typically have DBUG_ASSERT(0).
  */
  virtual bool is_evaluable_expression() const { return true; }

  /**
   * Check whether the item is a parameter  ('?') of stored routine.
   * Default implementation returns false. Method is overridden in the class
   * Item_param where it returns true.
   */
  virtual bool is_stored_routine_parameter() const { return false; }

  bool check_is_evaluable_expression_or_error()
  {
    if (is_evaluable_expression())
      return false; // Ok
    raise_error_not_evaluable();
    return true;    // Error
  }
  /* cloning of constant items (0 if it is not const) */
  virtual Item *clone_item(THD *thd) { return 0; }
  /* deep copy item */
  virtual Item* build_clone(THD *thd) { return get_copy(thd); }
  virtual cond_result eq_cmp_result() const { return COND_OK; }
  inline uint float_length(uint decimals_par) const
  { return decimals < FLOATING_POINT_DECIMALS ? (DBL_DIG+2+decimals_par) : DBL_DIG+8;}
  /* Returns total number of decimal digits */
  decimal_digits_t decimal_precision() const override
  {
    return type_handler()->Item_decimal_precision(this);
  }
  /* Returns the number of integer part digits only */
  inline decimal_digits_t decimal_int_part() const
  { return (decimal_digits_t) my_decimal_int_part(decimal_precision(), decimals); }
  /*
    Returns the number of fractional digits only.
    NOT_FIXED_DEC is replaced to the maximum possible number
    of fractional digits, taking into account the data type.
  */
  decimal_digits_t decimal_scale() const
  {
    return type_handler()->Item_decimal_scale(this);
  }
  /*
    Returns how many digits a divisor adds into a division result.
    This is important when the integer part of the divisor can be 0.
    In this  example:
      SELECT 1 / 0.000001; -> 1000000.0000
    the divisor adds 5 digits into the result precision.

    Currently this method only replaces NOT_FIXED_DEC to
    TIME_SECOND_PART_DIGITS for temporal data types.
    This method can be made virtual, to create more efficient (smaller)
    data types for division results.
    For example, in
      SELECT 1/1.000001;
    the divisor could provide no additional precision into the result,
    so could any other items that are know to return a result
    with non-zero integer part.
  */
  uint divisor_precision_increment() const
  {
    return type_handler()->Item_divisor_precision_increment(this);
  }
  /**
    TIME or DATETIME precision of the item: 0..6
  */
  uint time_precision(THD *thd)
  {
    return const_item() ? type_handler()->Item_time_precision(thd, this) :
                          MY_MIN(decimals, TIME_SECOND_PART_DIGITS);
  }
  uint datetime_precision(THD *thd)
  {
    return const_item() ? type_handler()->Item_datetime_precision(thd, this) :
                          MY_MIN(decimals, TIME_SECOND_PART_DIGITS);
  }
  virtual longlong val_int_min() const
  {
    return LONGLONG_MIN;
  }
  /* 
    Returns true if this is constant (during query execution, i.e. its value
    will not change until next fix_fields) and its value is known.
  */
  virtual bool const_item() const { return used_tables() == 0; }
  /* 
    Returns true if this is constant but its value may be not known yet.
    (Can be used for parameters of prep. stmts or of stored procedures.)
  */
  virtual bool const_during_execution() const 
  { return (used_tables() & ~PARAM_TABLE_BIT) == 0; }

  /**
    This method is used for to:
      - to generate a view definition query (SELECT-statement);
      - to generate a SQL-query for EXPLAIN EXTENDED;
      - to generate a SQL-query to be shown in INFORMATION_SCHEMA;
      - debug.

    For more information about view definition query, INFORMATION_SCHEMA
    query and why they should be generated from the Item-tree, @see
    mysql_register_view().
  */
  virtual enum precedence precedence() const { return DEFAULT_PRECEDENCE; }
  enum precedence higher_precedence() const
  { return (enum precedence)(precedence() + 1); }
  void print_parenthesised(String *str, enum_query_type query_type,
                           enum precedence parent_prec);
  /**
    This helper is used to print expressions as a part of a table definition,
    in particular for
      - generated columns
      - check constraints
      - default value expressions
      - partitioning expressions
  */
  void print_for_table_def(String *str)
  {
    print_parenthesised(str,
                     (enum_query_type)(QT_ITEM_ORIGINAL_FUNC_NULLIF |
                                       QT_ITEM_IDENT_SKIP_DB_NAMES |
                                       QT_ITEM_IDENT_SKIP_TABLE_NAMES |
                                       QT_NO_DATA_EXPANSION |
                                       QT_TO_SYSTEM_CHARSET),
                     LOWEST_PRECEDENCE);
  }
  virtual void print(String *str, enum_query_type query_type);
  class Print: public String
  {
  public:
    Print(Item *item, enum_query_type type)
    {
      item->print(this, type);
    }
  };

  void print_item_w_name(String *str, enum_query_type query_type);
  void print_value(String *str);

  virtual void update_used_tables() {}
  virtual COND *build_equal_items(THD *thd, COND_EQUAL *inheited,
                                  bool link_item_fields,
                                  COND_EQUAL **cond_equal_ref)
  {
    update_used_tables();
    DBUG_ASSERT(!cond_equal_ref || !cond_equal_ref[0]);
    return this;
  }
  virtual COND *remove_eq_conds(THD *thd, Item::cond_result *cond_value,
                                bool top_level);
  virtual void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                              uint *and_level,
                              table_map usable_tables,
                              SARGABLE_PARAM **sargables)
  {
    return;
  }
   /*
     Make a select tree for all keys in a condition or a condition part
     @param param         Context
     @param cond_ptr[OUT] Store a replacement item here if the condition
                          can be simplified, e.g.:
                            WHERE part1 OR part2 OR part3
                          with one of the partN evaluating to SEL_TREE::ALWAYS.
   */
   virtual SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr);
  /*
    Checks whether the item is:
    - a simple equality (field=field_item or field=constant_item), or
    - a row equality
    and form multiple equality predicates.
  */
  virtual bool check_equality(THD *thd, COND_EQUAL *cond, List<Item> *eq_list)
  {
    return false;
  }
  virtual void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                              List<Item> &fields, uint flags) {}
  /* Called for items that really have to be split */
  void split_sum_func2(THD *thd, Ref_ptr_array ref_pointer_array,
                       List<Item> &fields,
                       Item **ref, uint flags);
  virtual bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)= 0;
  bool get_date_from_int(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  bool get_date_from_real(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  bool get_date_from_string(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  bool get_time(THD *thd, MYSQL_TIME *ltime)
  { return get_date(thd, ltime, Time::Options(thd)); }
  // Get a DATE or DATETIME value in numeric packed format for comparison
  virtual longlong val_datetime_packed(THD *thd)
  {
    return Datetime(thd, this, Datetime::Options_cmp(thd)).to_packed();
  }
  // Get a TIME value in numeric packed format for comparison
  virtual longlong val_time_packed(THD *thd)
  {
    return Time(thd, this, Time::Options_cmp(thd)).to_packed();
  }
  longlong val_datetime_packed_result(THD *thd);
  longlong val_time_packed_result(THD *thd);

  virtual bool get_date_result(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  { return get_date(thd, ltime,fuzzydate); }

  /*
    The method allows to determine nullness of a complex expression 
    without fully evaluating it, instead of calling val/result*() then 
    checking null_value. Used in Item_func_isnull/Item_func_isnotnull
    and Item_sum_count.
    Any new item which can be NULL must implement this method.
  */
  virtual bool is_null() { return 0; }

  /*
   Make sure the null_value member has a correct value.
  */
  virtual void update_null_value ()
  {
    return type_handler()->Item_update_null_value(this);
  }

  /*
    Inform the item that there will be no distinction between its result
    being FALSE or NULL.

    NOTE
      This function will be called for eg. Items that are top-level AND-parts
      of the WHERE clause. Items implementing this function (currently
      Item_cond_and and subquery-related item) enable special optimizations
      when they are "top level".
  */
  virtual void top_level_item() {}
  /*
    Return TRUE if it is item of top WHERE level (AND/OR)  and it is
    important, return FALSE if it not important (we can not use to simplify
    calculations) or not top level
  */
  virtual bool is_top_level_item() const
  { return FALSE; /* not important */}
  /*
    return IN/ALL/ANY subquery or NULL
  */
  virtual Item_in_subselect* get_IN_subquery()
  { return NULL; /* in is not IN/ALL/ANY */ }
  /*
    set field of temporary table for Item which can be switched on temporary
    table during query processing (grouping and so on)
  */
  virtual bool is_result_field() { return 0; }
  virtual bool is_json_type() { return false; }
  virtual bool is_bool_literal() const { return false; }
  /* This is to handle printing of default values */
  virtual bool need_parentheses_in_default() { return false; }
  virtual void save_in_result_field(bool no_conversions) {}
  /*
    Data type format implied by the CHECK CONSTRAINT,
    to be sent to the client in the result set metadata.
  */
  virtual bool set_format_by_check_constraint(Send_field_extended_metadata *)
                                                                        const
  {
    return false;
  }
  /*
    set value of aggregate function in case of no rows for grouping were found
  */
  virtual void no_rows_in_result() {}
  virtual void restore_to_before_no_rows_in_result() {}
  virtual Item *copy_or_same(THD *thd) { return this; }
  virtual Item *copy_andor_structure(THD *thd) { return this; }
  virtual Item *real_item() { return this; }
  virtual Item *get_tmp_table_item(THD *thd) { return copy_or_same(thd); }
  virtual Item *make_odbc_literal(THD *thd, const LEX_CSTRING *typestr)
  {
    return this;
  }

  static CHARSET_INFO *default_charset();

  CHARSET_INFO *charset_for_protocol(void) const
  {
    return type_handler()->charset_for_protocol(this);
  };

  virtual bool walk(Item_processor processor, bool walk_subquery, void *arg)
  {
    return (this->*processor)(arg);
  }

  virtual Item* transform(THD *thd, Item_transformer transformer, uchar *arg);

  /*
    This function performs a generic "compilation" of the Item tree.
    The process of compilation is assumed to go as follows: 
    
    compile()
    {
      if (this->*some_analyzer(...))
      {
        compile children if any;
        this->*some_transformer(...);
      }
    }

    i.e. analysis is performed top-down while transformation is done
    bottom-up.      
  */
  virtual Item* compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                        Item_transformer transformer, uchar *arg_t)
  {
    if ((this->*analyzer) (arg_p))
      return ((this->*transformer) (thd, arg_t));
    return 0;
  }

   virtual void traverse_cond(Cond_traverser traverser,
                              void *arg, traverse_order order)
   {
     (*traverser)(this, arg);
   }

  /*========= Item processors, to be used with Item::walk() ========*/
  virtual bool remove_dependence_processor(void *arg) { return 0; }
  virtual bool cleanup_processor(void *arg);
  virtual bool cleanup_excluding_fields_processor (void *arg)
  { return cleanup_processor(arg); }
  bool cleanup_excluding_immutables_processor (void *arg);
  virtual bool cleanup_excluding_const_fields_processor (void *arg)
  { return cleanup_processor(arg); }
  virtual bool collect_item_field_processor(void *arg) { return 0; }
  virtual bool unknown_splocal_processor(void *arg) { return 0; }
  virtual bool collect_outer_ref_processor(void *arg) {return 0; }
  virtual bool check_inner_refs_processor(void *arg) { return 0; }
  virtual bool find_item_in_field_list_processor(void *arg) { return 0; }
  virtual bool find_item_processor(void *arg);
  virtual bool change_context_processor(void *arg) { return 0; }
  virtual bool reset_query_id_processor(void *arg) { return 0; }
  virtual bool is_expensive_processor(void *arg) { return 0; }

  // FIXME reduce the number of "add field to bitmap" processors
  virtual bool add_field_to_set_processor(void *arg) { return 0; }
  virtual bool register_field_in_read_map(void *arg) { return 0; }
  virtual bool register_field_in_write_map(void *arg) { return 0; }
  virtual bool register_field_in_bitmap(void *arg) { return 0; }
  virtual bool update_table_bitmaps_processor(void *arg) { return 0; }

  virtual bool enumerate_field_refs_processor(void *arg) { return 0; }
  virtual bool mark_as_eliminated_processor(void *arg) { return 0; }
  virtual bool eliminate_subselect_processor(void *arg) { return 0; }
  virtual bool set_fake_select_as_master_processor(void *arg) { return 0; }
  virtual bool view_used_tables_processor(void *arg) { return 0; }
  virtual bool eval_not_null_tables(void *arg) { return 0; }
  virtual bool is_subquery_processor(void *arg) { return 0; }
  virtual bool count_sargable_conds(void *arg) { return 0; }
  virtual bool limit_index_condition_pushdown_processor(void *arg) { return 0; }
  virtual bool exists2in_processor(void *arg) { return 0; }
  virtual bool find_selective_predicates_list_processor(void *arg) { return 0; }
  virtual bool cleanup_is_expensive_cache_processor(void *arg)
  {
    is_expensive_cache= (int8)(-1);
    return 0;
  }

  /**
    Check db/table_name if they defined in item and match arg values

    @param arg Pointer to Check_table_name_prm structure

    @retval true Match failed
    @retval false Match succeeded
  */
  virtual bool check_table_name_processor(void *arg) { return false; }
  /* 
    TRUE if the expression depends only on the table indicated by tab_map
    or can be converted to such an exression using equalities.
    Not to be used for AND/OR formulas.
  */
  virtual bool excl_dep_on_table(table_map tab_map) { return false; }
  /*
    TRUE if the expression depends only on grouping fields of sel
    or can be converted to such an expression using equalities.
    It also checks if the expression doesn't contain stored procedures,
    subqueries or randomly generated elements.
    Not to be used for AND/OR formulas.
  */
  virtual bool excl_dep_on_grouping_fields(st_select_lex *sel)
  { return false; }
  /*
    TRUE if the expression depends only on fields from the left part of
    IN subquery or can be converted to such an expression using equalities.
    Not to be used for AND/OR formulas.
  */
  virtual bool excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred)
  { return false; }

  virtual bool switch_to_nullable_fields_processor(void *arg) { return 0; }
  virtual bool find_function_processor (void *arg) { return 0; }
  /*
    Check if a partition function is allowed
    SYNOPSIS
      check_partition_func_processor()
      int_arg                        Ignored
    RETURN VALUE
      TRUE                           Partition function not accepted
      FALSE                          Partition function accepted

    DESCRIPTION
    check_partition_func_processor is used to check if a partition function
    uses an allowed function. An allowed function will always ensure that
    X=Y guarantees that also part_function(X)=part_function(Y) where X is
    a set of partition fields and so is Y. The problems comes mainly from
    character sets where two equal strings can be quite unequal. E.g. the
    german character for double s is equal to 2 s.

    The default is that an item is not allowed
    in a partition function. Allowed functions
    can never depend on server version, they cannot depend on anything
    related to the environment. They can also only depend on a set of
    fields in the table itself. They cannot depend on other tables and
    cannot contain any queries and cannot contain udf's or similar.
    If a new Item class is defined and it inherits from a class that is
    allowed in a partition function then it is very important to consider
    whether this should be inherited to the new class. If not the function
    below should be defined in the new Item class.

    The general behaviour is that most integer functions are allowed.
    If the partition function contains any multi-byte collations then
    the function check_part_func_fields will report an error on the
    partition function independent of what functions are used. So the
    only character sets allowed are single character collation and
    even for those only a limited set of functions are allowed. The
    problem with multi-byte collations is that almost every string
    function has the ability to change things such that two strings
    that are equal will not be equal after manipulated by a string
    function. E.g. two strings one contains a double s, there is a
    special german character that is equal to two s. Now assume a
    string function removes one character at this place, then in
    one the double s will be removed and in the other there will
    still be one s remaining and the strings are no longer equal
    and thus the partition function will not sort equal strings into
    the same partitions.

    So the check if a partition function is valid is two steps. First
    check that the field types are valid, next check that the partition
    function is valid. The current set of partition functions valid
    assumes that there are no multi-byte collations amongst the partition
    fields.
  */
  virtual bool check_partition_func_processor(void *arg) { return true; }
  virtual bool post_fix_fields_part_expr_processor(void *arg) { return 0; }
  virtual bool rename_fields_processor(void *arg) { return 0; }
  /*
    TRUE if the function is knowingly TRUE or FALSE.
    Not to be used for AND/OR formulas.
  */
  virtual bool is_simplified_cond_processor(void *arg) { return false; }

  /** Processor used to check acceptability of an item in the defining
      expression for a virtual column 

    @param arg     always ignored

    @retval 0    the item is accepted in the definition of a virtual column
    @retval 1    otherwise
  */
  struct vcol_func_processor_result
  {
    uint errors;                                /* Bits of possible errors */
    const char *name;                           /* Not supported function */
    Alter_info *alter_info;
    vcol_func_processor_result() :
      errors(0), name(NULL), alter_info(NULL) {}
  };
  struct func_processor_rename
  {
    LEX_CSTRING db_name;
    LEX_CSTRING table_name;
    List<Create_field> fields;
  };
  virtual bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(full_name(), arg, VCOL_IMPOSSIBLE);
  }
  virtual bool check_handler_func_processor(void *arg) { return 0; }
  virtual bool check_field_expression_processor(void *arg) { return 0; }
  virtual bool check_func_default_processor(void *arg) { return 0; }
  /*
    Check if an expression value has allowed arguments, like DATE/DATETIME
    for date functions. Also used by partitioning code to reject
    timezone-dependent expressions in a (sub)partitioning function.
  */
  virtual bool check_valid_arguments_processor(void *arg) { return 0; }
  virtual bool update_vcol_processor(void *arg) { return 0; }
  virtual bool set_fields_as_dependent_processor(void *arg) { return 0; }
  /*
    Find if some of the key parts of table keys (the reference on table is
    passed as an argument) participate in the expression.
    If there is some, sets a bit for this key in the proper key map.
  */
  virtual bool check_index_dependence(void *arg) { return 0; }
  /*============== End of Item processor list ======================*/

  /*
    Given a condition P from the WHERE clause or from an ON expression of
    the processed SELECT S and a set of join tables from S marked in the
    parameter 'allowed'={T} a call of P->find_not_null_fields({T}) has to
    find the set fields {F} of the tables from 'allowed' such that:
    - each field from {F} is declared as nullable
    - each record of table t from {T} that contains NULL as the value for at
      at least one field from {F} can be ignored when building the result set
      for S
    It is assumed here that the condition P is conjunctive and all its column
    references belong to T.

    Examples:
      CREATE TABLE t1 (a int, b int);
      CREATE TABLE t2 (a int, b int);

      SELECT * FROM t1,t2 WHERE t1.a=t2.a and t1.b > 5;
      A call of find_not_null_fields() for the whole WHERE condition and {t1,t2}
      should find {t1.a,t1.b,t2.a}

      SELECT * FROM t1 LEFT JOIN ON (t1.a=t2.a and t2.a > t2.b);
      A call of find_not_null_fields() for the ON expression and {t2}
      should find {t2.a,t2.b}

    The function returns TRUE if it succeeds to prove that all records of
    a table from {T} can be ignored. Otherwise it always returns FALSE.

    Example:
      SELECT * FROM t1,t2 WHERE t1.a=t2.a AND t2.a IS NULL;
    A call of find_not_null_fields() for the WHERE condition and {t1,t2}
    will return TRUE.

    It is assumed that the implementation of this virtual function saves
    the info on the found set of fields in the structures associates with
    tables from {T}.
  */
  virtual bool find_not_null_fields(table_map allowed) { return false; }

  /*
    Does not guarantee deep copy (depends on copy ctor).
    See build_clone() for deep copy.
  */
  virtual Item *get_copy(THD *thd)=0;

  bool cache_const_expr_analyzer(uchar **arg);
  Item* cache_const_expr_transformer(THD *thd, uchar *arg);

  virtual Item* propagate_equal_fields(THD*, const Context &, COND_EQUAL *)
  {
    return this;
  };

  Item* propagate_equal_fields_and_change_item_tree(THD *thd,
                                                    const Context &ctx,
                                                    COND_EQUAL *cond,
                                                    Item **place);

  /* arg points to REPLACE_EQUAL_FIELD_ARG object */
  virtual Item *replace_equal_field(THD *thd, uchar *arg) { return this; }

  struct Collect_deps_prm
  {
    List<Item> *parameters;
    /* unit from which we count nest_level */
    st_select_lex_unit *nest_level_base;
    uint count;
    int nest_level;
    bool collect;
  };

  struct Check_table_name_prm
  {
    LEX_CSTRING db;
    LEX_CSTRING table_name;
    String field;
    Check_table_name_prm(LEX_CSTRING _db, LEX_CSTRING _table_name) :
      db(_db), table_name(_table_name) {}
  };

  /*
    For SP local variable returns pointer to Item representing its
    current value and pointer to current Item otherwise.
  */
  virtual Item *this_item() { return this; }
  virtual const Item *this_item() const { return this; }

  /*
    For SP local variable returns address of pointer to Item representing its
    current value and pointer passed via parameter otherwise.
  */
  virtual Item **this_item_addr(THD *thd, Item **addr_arg) { return addr_arg; }

  // Row emulation
  virtual uint cols() const { return 1; }
  virtual Item* element_index(uint i) { return this; }
  virtual Item** addr(uint i) { return 0; }
  virtual bool check_cols(uint c);
  bool check_type_traditional_scalar(const LEX_CSTRING &opname) const;
  bool check_type_scalar(const LEX_CSTRING &opname) const;
  bool check_type_or_binary(const LEX_CSTRING &opname,
                            const Type_handler *handler) const;
  bool check_type_general_purpose_string(const LEX_CSTRING &opname) const;
  bool check_type_can_return_int(const LEX_CSTRING &opname) const;
  bool check_type_can_return_decimal(const LEX_CSTRING &opname) const;
  bool check_type_can_return_real(const LEX_CSTRING &opname) const;
  bool check_type_can_return_str(const LEX_CSTRING &opname) const;
  bool check_type_can_return_text(const LEX_CSTRING &opname) const;
  bool check_type_can_return_date(const LEX_CSTRING &opname) const;
  bool check_type_can_return_time(const LEX_CSTRING &opname) const;
  // It is not row => null inside is impossible
  virtual bool null_inside() { return 0; }
  // used in row subselects to get value of elements
  virtual void bring_value() {}

  const Type_handler *type_handler_long_or_longlong() const
  {
    return Type_handler::type_handler_long_or_longlong(max_char_length(),
                                                       unsigned_flag);
  }

  /**
    Create field for temporary table.
    @param table          Temporary table
    @param [OUT] src      Who created the fields
    @param param          Create parameters
    @retval               NULL (on error)
    @retval               a pointer to a newly create Field (on success)
  */
  virtual Field *create_tmp_field_ex(MEM_ROOT *root,
                                     TABLE *table,
                                     Tmp_field_src *src,
                                     const Tmp_field_param *param)= 0;
  virtual Item_field *field_for_view_update() { return 0; }

  virtual Item *neg_transformer(THD *thd) { return NULL; }
  virtual Item *update_value_transformer(THD *thd, uchar *select_arg)
  { return this; }
  virtual Item *expr_cache_insert_transformer(THD *thd, uchar *unused)
  { return this; }
  virtual Item *derived_field_transformer_for_having(THD *thd, uchar *arg)
  { return this; }
  virtual Item *derived_field_transformer_for_where(THD *thd, uchar *arg)
  { return this; }
  virtual Item *grouping_field_transformer_for_where(THD *thd, uchar *arg)
  { return this; }
  /* Now is not used. */
  virtual Item *in_subq_field_transformer_for_where(THD *thd, uchar *arg)
  { return this; }
  virtual Item *in_subq_field_transformer_for_having(THD *thd, uchar *arg)
  { return this; }
  virtual Item *in_predicate_to_in_subs_transformer(THD *thd, uchar *arg)
  { return this; }
  virtual Item *field_transformer_for_having_pushdown(THD *thd, uchar *arg)
  { return this; }
  virtual Item *multiple_equality_transformer(THD *thd, uchar *arg)
  { return this; }
  virtual bool expr_cache_is_needed(THD *) { return FALSE; }
  virtual Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs);
  bool needs_charset_converter(uint32 length, CHARSET_INFO *tocs) const
  {
    /*
      This will return "true" if conversion happens:
      - between two non-binary different character sets
      - from "binary" to "unsafe" character set
        (those that can have non-well-formed string)
      - from "binary" to UCS2-alike character set with mbminlen>1,
        when prefix left-padding is needed for an incomplete character:
        binary 0xFF -> ucs2 0x00FF)
    */
    if (!String::needs_conversion_on_storage(length,
                                             collation.collation, tocs))
      return false;
    /*
      No needs to add converter if an "arg" is NUMERIC or DATETIME
      value (which is pure ASCII) and at the same time target DTCollation
      is ASCII-compatible. For example, no needs to rewrite:
        SELECT * FROM t1 WHERE datetime_field = '2010-01-01';
      to
        SELECT * FROM t1 WHERE CONVERT(datetime_field USING cs) = '2010-01-01';

      TODO: avoid conversion of any values with
      repertoire ASCII and 7bit-ASCII-compatible,
      not only numeric/datetime origin.
    */
    if (collation.derivation == DERIVATION_NUMERIC &&
        collation.repertoire == MY_REPERTOIRE_ASCII &&
        !(collation.collation->state & MY_CS_NONASCII) &&
        !(tocs->state & MY_CS_NONASCII))
      return false;
    return true;
  }
  bool needs_charset_converter(CHARSET_INFO *tocs)
  {
    // Pass 1 as length to force conversion if tocs->mbminlen>1.
    return needs_charset_converter(1, tocs);
  }
  Item *const_charset_converter(THD *thd, CHARSET_INFO *tocs, bool lossless,
                                const char *func_name);
  Item *const_charset_converter(THD *thd, CHARSET_INFO *tocs, bool lossless)
  { return const_charset_converter(thd, tocs, lossless, NULL); }
  void delete_self()
  {
    cleanup();
    delete this;
  }

  virtual const Item_const *get_item_const() const { return NULL; }
  virtual Item_splocal *get_item_splocal() { return 0; }
  virtual Rewritable_query_parameter *get_rewritable_query_parameter()
  { return 0; }

  /*
    Return Settable_routine_parameter interface of the Item.  Return 0
    if this Item is not Settable_routine_parameter.
  */
  virtual Settable_routine_parameter *get_settable_routine_parameter()
  {
    return 0;
  }

  virtual Load_data_outvar *get_load_data_outvar()
  {
    return 0;
  }
  Load_data_outvar *get_load_data_outvar_or_error()
  {
    Load_data_outvar *dst= get_load_data_outvar();
    if (dst)
      return dst;
    my_error(ER_NONUPDATEABLE_COLUMN, MYF(0), name.str);
    return NULL;
  }

  /**
    Test whether an expression is expensive to compute. Used during
    optimization to avoid computing expensive expressions during this
    phase. Also used to force temp tables when sorting on expensive
    functions.
    @todo
    Normally we should have a method:
      cost Item::execution_cost(),
    where 'cost' is either 'double' or some structure of various cost
    parameters.

    @note
      This function is now used to prevent evaluation of expensive subquery
      predicates during the optimization phase. It also prevents evaluation
      of predicates that are not computable at this moment.
  */
  virtual bool is_expensive()
  {
    if (is_expensive_cache < 0)
      is_expensive_cache= walk(&Item::is_expensive_processor, 0, NULL);
    return MY_TEST(is_expensive_cache);
  }
  String *check_well_formed_result(String *str, bool send_error= 0);
  bool eq_by_collation(Item *item, bool binary_cmp, CHARSET_INFO *cs); 
  bool too_big_for_varchar() const
  { return max_char_length() > CONVERT_IF_BIGGER_TO_BLOB; }
  void fix_length_and_charset(uint32 max_char_length_arg, CHARSET_INFO *cs)
  {
    max_length= char_to_byte_length_safe(max_char_length_arg, cs->mbmaxlen);
    collation.collation= cs;
  }
  void fix_char_length(size_t max_char_length_arg)
  {
    max_length= char_to_byte_length_safe(max_char_length_arg,
                                         collation.collation->mbmaxlen);
  }
  /*
    Return TRUE if the item points to a column of an outer-joined table.
  */
  virtual bool is_outer_field() const { DBUG_ASSERT(fixed()); return FALSE; }

  Item* set_expr_cache(THD *thd);

  virtual Item_equal *get_item_equal() { return NULL; }
  virtual void set_item_equal(Item_equal *item_eq) {};
  virtual Item_equal *find_item_equal(COND_EQUAL *cond_equal) { return NULL; }
  /**
    Set the join tab index to the minimal (left-most) JOIN_TAB to which this
    Item is attached. The number is an index is depth_first_tab() traversal
    order.
  */
  virtual void set_join_tab_idx(uint8 join_tab_idx_arg)
  {
    if (join_tab_idx_arg < join_tab_idx)
      join_tab_idx= join_tab_idx_arg;
  }
  uint get_join_tab_idx() const { return join_tab_idx; }

  table_map view_used_tables(TABLE_LIST *view)
  {
    view->view_used_tables= 0;
    walk(&Item::view_used_tables_processor, 0, view);
    return view->view_used_tables;
  }

  /**
    Collect and add to the list cache parameters for this Item.

    @note Now implemented only for subqueries and in_optimizer,
    if we need it for general function then this method should
    be defined for Item_func.
  */
  virtual void get_cache_parameters(List<Item> &parameters) { };

  virtual void mark_as_condition_AND_part(TABLE_LIST *embedding) {};

  /* how much position should be reserved for Exists2In transformation */
  virtual uint exists2in_reserved_items() { return 0; };

  virtual Item *neg(THD *thd);

  /**
    Inform the item that it is located under a NOT, which is a top-level item.
  */
  virtual void under_not(Item_func_not * upper
                         __attribute__((unused))) {};
  /*
    If Item_field is wrapped in Item_direct_wrep remove this Item_direct_ref
    wrapper.
  */
  virtual Item *remove_item_direct_ref() { return this; }
	

  void register_in(THD *thd);	 
  
  bool depends_only_on(table_map view_map) 
  { return marker & MARKER_FULL_EXTRACTION; }
  int get_extraction_flag()
  { return marker & MARKER_EXTRACTION_MASK; }
  void set_extraction_flag(int16 flags)
  {
    marker &= ~MARKER_EXTRACTION_MASK;
    marker|= flags;
  }
  void clear_extraction_flag()
  {
    marker &= ~MARKER_EXTRACTION_MASK;
  }
  void check_pushable_cond(Pushdown_checker excl_dep_func, uchar *arg);
  bool pushable_cond_checker_for_derived(uchar *arg)
  {
    return excl_dep_on_table(*((table_map *)arg));
  }
  bool pushable_cond_checker_for_subquery(uchar *arg)
  {
    DBUG_ASSERT(((Item*) arg)->get_IN_subquery());
    return excl_dep_on_in_subq_left_part(((Item*)arg)->get_IN_subquery());
  }
  Item *build_pushable_cond(THD *thd,
                            Pushdown_checker checker,
                            uchar *arg);
  /*
    Checks if this item depends only on the arg table
  */
  bool pushable_equality_checker_for_derived(uchar *arg)
  {
    return (used_tables() == *((table_map *)arg));
  }
  /*
    Checks if this item consists in the left part of arg IN subquery predicate
  */
  bool pushable_equality_checker_for_subquery(uchar *arg);
};

MEM_ROOT *get_thd_memroot(THD *thd);

template <class T>
inline Item* get_item_copy (THD *thd, T* item)
{
  Item *copy= new (get_thd_memroot(thd)) T(*item);
  if (likely(copy))
    copy->register_in(thd);
  return copy;
}	


#ifndef DBUG_OFF
/**
  A helper class to print the data type and the value for an Item
  in debug builds.
*/
class DbugStringItemTypeValue: public StringBuffer<128>
{
public:
  DbugStringItemTypeValue(THD *thd, const Item *item)
  {
    append('(');
    Name Item_name= item->type_handler()->name();
    append(Item_name.ptr(), Item_name.length());
    append(')');
    const_cast<Item*>(item)->print(this, QT_EXPLAIN);
    /* Append end \0 to allow usage of c_ptr() */
    append('\0');
    str_length--;
  }
};
#endif /* DBUG_OFF */


/**
  Compare two Items for List<Item>::add_unique()
*/

bool cmp_items(Item *a, Item *b);


/**
  Array of items, e.g. function or aggerate function arguments.
*/
class Item_args
{
protected:
  Item **args, *tmp_arg[2];
  uint arg_count;
  void set_arguments(THD *thd, List<Item> &list);
  bool walk_args(Item_processor processor, bool walk_subquery, void *arg)
  {
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->walk(processor, walk_subquery, arg))
        return true;
    }
    return false;
  }
  bool transform_args(THD *thd, Item_transformer transformer, uchar *arg);
  void propagate_equal_fields(THD *, const Item::Context &, COND_EQUAL *);
  bool excl_dep_on_table(table_map tab_map)
  {
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->const_item())
        continue;
      if (!args[i]->excl_dep_on_table(tab_map))
        return false;
    }
    return true;
  }
  bool excl_dep_on_grouping_fields(st_select_lex *sel);
  bool eq(const Item_args *other, bool binary_cmp) const
  {
    for (uint i= 0; i < arg_count ; i++)
    {
      if (!args[i]->eq(other->args[i], binary_cmp))
        return false;
    }
    return true;
  }
  bool excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred)
  {
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->const_item())
        continue;
      if (!args[i]->excl_dep_on_in_subq_left_part(subq_pred))
        return false;
    }
    return true;
  }
public:
  Item_args(void)
    :args(NULL), arg_count(0)
  { }
  Item_args(Item *a)
    :args(tmp_arg), arg_count(1)
  {
    args[0]= a;
  }
  Item_args(Item *a, Item *b)
    :args(tmp_arg), arg_count(2)
  {
    args[0]= a; args[1]= b;
  }
  Item_args(THD *thd, Item *a, Item *b, Item *c)
  {
    arg_count= 0;
    if (likely((args= (Item**) thd_alloc(thd, sizeof(Item*) * 3))))
    {
      arg_count= 3;
      args[0]= a; args[1]= b; args[2]= c;
    }
  }
  Item_args(THD *thd, Item *a, Item *b, Item *c, Item *d)
  {
    arg_count= 0;
    if (likely((args= (Item**) thd_alloc(thd, sizeof(Item*) * 4))))
    {
      arg_count= 4;
      args[0]= a; args[1]= b; args[2]= c; args[3]= d;
    }
  }
  Item_args(THD *thd, Item *a, Item *b, Item *c, Item *d, Item* e)
  {
    arg_count= 5;
    if (likely((args= (Item**) thd_alloc(thd, sizeof(Item*) * 5))))
    {
      arg_count= 5;
      args[0]= a; args[1]= b; args[2]= c; args[3]= d; args[4]= e;
    }
  }
  Item_args(THD *thd, List<Item> &list)
  {
    set_arguments(thd, list);
  }
  Item_args(THD *thd, const Item_args *other);
  bool alloc_arguments(THD *thd, uint count);
  void add_argument(Item *item)
  {
    args[arg_count++]= item;
  }
  /**
    Extract row elements from the given position.
    For example, for this input:  (1,2),(3,4),(5,6)
      pos=0 will extract  (1,3,5)
      pos=1 will extract  (2,4,6)
    @param  thd  - current thread, to allocate memory on its mem_root
    @param  rows - an array of compatible ROW-type items
    @param  pos  - the element position to extract
  */
  bool alloc_and_extract_row_elements(THD *thd, const Item_args *rows, uint pos)
  {
    DBUG_ASSERT(rows->argument_count() > 0);
    DBUG_ASSERT(rows->arguments()[0]->cols() > pos);
    if (alloc_arguments(thd, rows->argument_count()))
      return true;
    for (uint i= 0; i < rows->argument_count(); i++)
    {
      DBUG_ASSERT(rows->arguments()[0]->cols() == rows->arguments()[i]->cols());
      Item *arg= rows->arguments()[i]->element_index(pos);
      add_argument(arg);
    }
    DBUG_ASSERT(argument_count() == rows->argument_count());
    return false;
  }
  inline Item **arguments() const { return args; }
  inline uint argument_count() const { return arg_count; }
  inline void remove_arguments() { arg_count=0; }
  Sql_mode_dependency value_depends_on_sql_mode_bit_or() const;
};


/*
  Class to be used to enumerate all field references in an item tree. This
  includes references to outside but not fields of the tables within a
  subquery.
  Suggested usage:

    class My_enumerator : public Field_enumerator 
    {
      virtual void visit_field() { ... your actions ...} 
    }

    My_enumerator enumerator;
    item->walk(Item::enumerate_field_refs_processor, ...,&enumerator);

  This is similar to Visitor pattern.
*/

class Field_enumerator
{
public:
  virtual void visit_field(Item_field *field)= 0;
  virtual ~Field_enumerator() {};             /* purecov: inspected */
  Field_enumerator() {}                       /* Remove gcc warning */
};

class Item_string;


class Item_fixed_hybrid: public Item
{
public:
  Item_fixed_hybrid(THD *thd): Item(thd)
  {
    base_flags&= ~item_base_t::FIXED;
  }
  Item_fixed_hybrid(THD *thd, Item_fixed_hybrid *item)
   :Item(thd, item)
  {
    base_flags|= (item->base_flags & item_base_t::FIXED);
  }
  bool fix_fields(THD *thd, Item **ref) override
  {
    DBUG_ASSERT(!fixed());
    base_flags|= item_base_t::FIXED;
    return false;
  }
  void cleanup() override
  {
    Item::cleanup();
    base_flags&= ~item_base_t::FIXED;
  }
  void quick_fix_field() override
  { base_flags|= item_base_t::FIXED; }
  void unfix_fields() override
  { base_flags&= ~item_base_t::FIXED; }
};


/**
  A common class for Item_basic_constant and Item_param
*/
class Item_basic_value :public Item,
                        public Item_const
{
protected:
  // Value metadata, e.g. to make string processing easier
  class Metadata: private MY_STRING_METADATA
  {
  public:
    Metadata(const String *str)
    {
      my_string_metadata_get(this, str->charset(), str->ptr(), str->length());
    }
    Metadata(const String *str, my_repertoire_t repertoire_arg)
    {
      MY_STRING_METADATA::repertoire= repertoire_arg;
      MY_STRING_METADATA::char_length= str->numchars();
    }
    my_repertoire_t repertoire() const
    {
      return MY_STRING_METADATA::repertoire;
    }
    size_t char_length() const { return MY_STRING_METADATA::char_length; }
  };
  void fix_charset_and_length(CHARSET_INFO *cs,
                              Derivation dv, Metadata metadata)
  {
    /*
      We have to have a different max_length than 'length' here to
      ensure that we get the right length if we do use the item
      to create a new table. In this case max_length must be the maximum
      number of chars for a string of this type because we in Create_field::
      divide the max_length with mbmaxlen).
    */
    collation.set(cs, dv, metadata.repertoire());
    fix_char_length(metadata.char_length());
    decimals= NOT_FIXED_DEC;
  }
  void fix_charset_and_length_from_str_value(const String &str, Derivation dv)
  {
    fix_charset_and_length(str.charset(), dv, Metadata(&str));
  }
  Item_basic_value(THD *thd): Item(thd) {}
  Item_basic_value(): Item() {}
public:
  Field *create_tmp_field_ex(MEM_ROOT *root,
                             TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {

    /*
      create_tmp_field_ex() for this type of Items is called for:
      - CREATE TABLE ... SELECT
      - In ORDER BY: SELECT max(a) FROM t1 GROUP BY a ORDER BY 'const';
      - In CURSORS:
          DECLARE c CURSOR FOR SELECT 'test';
          OPEN c;
    */
    return tmp_table_field_from_field_type_maybe_null(root,
                                            table, src, param,
                                            type() == Item::NULL_ITEM);
  }
  bool eq(const Item *item, bool binary_cmp) const override;
  const Type_all_attributes *get_type_all_attributes_from_const() const
    override
  { return this; }
};


class Item_basic_constant :public Item_basic_value
{
public:
  Item_basic_constant(THD *thd): Item_basic_value(thd) {};
  Item_basic_constant(): Item_basic_value() {};
  bool check_vcol_func_processor(void *arg) { return false; }
  const Item_const *get_item_const() const { return this; }
  virtual Item_basic_constant *make_string_literal_concat(THD *thd,
                                                          const LEX_CSTRING *)
  {
    DBUG_ASSERT(0);
    return this;
  }
};


/*****************************************************************************
  The class is a base class for representation of stored routine variables in
  the Item-hierarchy. There are the following kinds of SP-vars:
    - local variables (Item_splocal);
    - CASE expression (Item_case_expr);
*****************************************************************************/

class Item_sp_variable :public Item_fixed_hybrid
{
protected:
  /*
    THD, which is stored in fix_fields() and is used in this_item() to avoid
    current_thd use.
  */
  THD *m_thd;

  bool fix_fields_from_item(THD *thd, Item **, const Item *);
public:
  LEX_CSTRING m_name;

public:
#ifdef DBUG_ASSERT_EXISTS
  /*
    Routine to which this Item_splocal belongs. Used for checking if correct
    runtime context is used for variable handling.
  */
  const sp_head *m_sp;
#endif

public:
  Item_sp_variable(THD *thd, const LEX_CSTRING *sp_var_name);

public:
  bool fix_fields(THD *thd, Item **) override= 0;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *sp) override;
  my_decimal *val_decimal(my_decimal *decimal_value) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool val_native(THD *thd, Native *to) override;
  bool is_null() override;

public:
  void make_send_field(THD *thd, Send_field *field) override;
  bool const_item() const override { return true; }
  Field *create_tmp_field_ex(MEM_ROOT *root,
                             TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    return create_tmp_field_ex_simple(root, table, src, param);
  }
  inline int save_in_field(Field *field, bool no_conversions) override;
  inline bool send(Protocol *protocol, st_value *buffer) override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(m_name.str, arg, VCOL_IMPOSSIBLE);
  }
};

/*****************************************************************************
  Item_sp_variable inline implementation.
*****************************************************************************/

inline int Item_sp_variable::save_in_field(Field *field, bool no_conversions)
{
  return this_item()->save_in_field(field, no_conversions);
}

inline bool Item_sp_variable::send(Protocol *protocol, st_value *buffer)
{
  return this_item()->send(protocol, buffer);
}


/*****************************************************************************
  A reference to local SP variable (incl. reference to SP parameter), used in
  runtime.
*****************************************************************************/

class Item_splocal :public Item_sp_variable,
                    private Settable_routine_parameter,
                    public Rewritable_query_parameter,
                    public Type_handler_hybrid_field_type
{
protected:
  const Sp_rcontext_handler *m_rcontext_handler;

  uint m_var_idx;

  Type m_type;

  bool append_value_for_log(THD *thd, String *str);

  sp_rcontext *get_rcontext(sp_rcontext *local_ctx) const;
  Item_field *get_variable(sp_rcontext *ctx) const;

public:
  Item_splocal(THD *thd, const Sp_rcontext_handler *rh,
               const LEX_CSTRING *sp_var_name, uint sp_var_idx,
               const Type_handler *handler,
               uint pos_in_q= 0, uint len_in_q= 0);

  bool fix_fields(THD *, Item **) override;
  Item *this_item() override;
  const Item *this_item() const override;
  Item **this_item_addr(THD *thd, Item **) override;

  void print(String *str, enum_query_type query_type) override;

public:
  inline const LEX_CSTRING *my_name() const;

  inline uint get_var_idx() const;

  Type type() const override { return m_type; }
  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }
  uint cols() const override { return this_item()->cols(); }
  Item* element_index(uint i) override
  { return this_item()->element_index(i); }
  Item** addr(uint i) override { return this_item()->addr(i); }
  bool check_cols(uint c) override;

private:
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;

public:
  Item_splocal *get_item_splocal() override { return this; }

  Rewritable_query_parameter *get_rewritable_query_parameter() override
  { return this; }

  Settable_routine_parameter *get_settable_routine_parameter() override
  { return this; }

  bool append_for_log(THD *thd, String *str) override;

  Item *get_copy(THD *) override { return nullptr; }

  /*
    Override the inherited create_field_for_create_select(),
    because we want to preserve the exact data type for:
      DECLARE a1 INT;
      DECLARE a2 TYPE OF t1.a2;
      CREATE TABLE t1 AS SELECT a1, a2;
    The inherited implementation would create a column
    based on result_type(), which is less exact.
  */
  Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table) override
  { return create_table_field_from_handler(root, table); }

  bool is_valid_limit_clause_variable_with_error() const
  {
    /*
      In case if the variable has an anchored data type, e.g.:
        DECLARE a TYPE OF t1.a;
      type_handler() is set to &type_handler_null and this
      function detects such variable as not valid in LIMIT.
    */
    if (type_handler()->is_limit_clause_valid_type())
      return true;
    my_error(ER_WRONG_SPVAR_TYPE_IN_LIMIT, MYF(0));
    return false;
  }
};


/**
  An Item_splocal variant whose data type becomes known only at
  sp_rcontext creation time, e.g. "DECLARE var1 t1.col1%TYPE".
*/
class Item_splocal_with_delayed_data_type: public Item_splocal
{
public:
  Item_splocal_with_delayed_data_type(THD *thd,
                                      const Sp_rcontext_handler *rh,
                                      const LEX_CSTRING *sp_var_name,
                                      uint sp_var_idx,
                                      uint pos_in_q, uint len_in_q)
   :Item_splocal(thd, rh, sp_var_name, sp_var_idx, &type_handler_null,
                 pos_in_q, len_in_q)
  { }
};


/**
  SP variables that are fields of a ROW.
  DELCARE r ROW(a INT,b INT);
  SELECT r.a; -- This is handled by Item_splocal_row_field
*/
class Item_splocal_row_field :public Item_splocal
{
protected:
  LEX_CSTRING m_field_name;
  uint m_field_idx;
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;
public:
  Item_splocal_row_field(THD *thd,
                         const Sp_rcontext_handler *rh,
                         const LEX_CSTRING *sp_var_name,
                         const LEX_CSTRING *sp_field_name,
                         uint sp_var_idx, uint sp_field_idx,
                         const Type_handler *handler,
                         uint pos_in_q= 0, uint len_in_q= 0)
   :Item_splocal(thd, rh, sp_var_name, sp_var_idx, handler, pos_in_q, len_in_q),
    m_field_name(*sp_field_name),
    m_field_idx(sp_field_idx)
  { }
  bool fix_fields(THD *thd, Item **) override;
  Item *this_item() override;
  const Item *this_item() const override;
  Item **this_item_addr(THD *thd, Item **) override;
  bool append_for_log(THD *thd, String *str) override;
  void print(String *str, enum_query_type query_type) override;
};


class Item_splocal_row_field_by_name :public Item_splocal_row_field
{
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;
public:
  Item_splocal_row_field_by_name(THD *thd,
                                 const Sp_rcontext_handler *rh,
                                 const LEX_CSTRING *sp_var_name,
                                 const LEX_CSTRING *sp_field_name,
                                 uint sp_var_idx,
                                 const Type_handler *handler,
                                 uint pos_in_q= 0, uint len_in_q= 0)
   :Item_splocal_row_field(thd, rh, sp_var_name, sp_field_name,
                           sp_var_idx, 0 /* field index will be set later */,
                           handler, pos_in_q, len_in_q)
  { }
  bool fix_fields(THD *thd, Item **it) override;
  void print(String *str, enum_query_type query_type) override;
};


/*****************************************************************************
  Item_splocal inline implementation.
*****************************************************************************/

inline const LEX_CSTRING *Item_splocal::my_name() const
{
  return &m_name;
}

inline uint Item_splocal::get_var_idx() const
{
  return m_var_idx;
}

/*****************************************************************************
  A reference to case expression in SP, used in runtime.
*****************************************************************************/

class Item_case_expr :public Item_sp_variable
{
public:
  Item_case_expr(THD *thd, uint case_expr_id);

public:
  bool fix_fields(THD *thd, Item **) override;
  Item *this_item() override;
  const Item *this_item() const override;
  Item **this_item_addr(THD *thd, Item **) override;

  Type type() const override;
  const Type_handler *type_handler() const override
  { return this_item()->type_handler(); }

public:
  /*
    NOTE: print() is intended to be used from views and for debug.
    Item_case_expr can not occur in views, so here it is only for debug
    purposes.
  */
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *) override { return nullptr; }

private:
  uint m_case_expr_id;
};

/*****************************************************************************
  Item_case_expr inline implementation.
*****************************************************************************/

inline enum Item::Type Item_case_expr::type() const
{
  return this_item()->type();
}

/*
  NAME_CONST(given_name, const_value). 
  This 'function' has all properties of the supplied const_value (which is 
  assumed to be a literal constant), and the name given_name. 

  This is used to replace references to SP variables when we write PROCEDURE
  statements into the binary log.

  TODO
    Together with Item_splocal and Item::this_item() we can actually extract
    common a base of this class and Item_splocal. Maybe it is possible to
    extract a common base with class Item_ref, too.
*/

class Item_name_const : public Item_fixed_hybrid
{
  Item *value_item;
  Item *name_item;
public:
  Item_name_const(THD *thd, Item *name_arg, Item *val);

  bool fix_fields(THD *, Item **) override;

  Type type() const override;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *sp) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool val_native(THD *thd, Native *to) override;
  bool is_null() override;
  void print(String *str, enum_query_type query_type) override;

  const Type_handler *type_handler() const override
  {
    return value_item->type_handler();
  }

  bool const_item() const override { return true; }

  Field *create_tmp_field_ex(MEM_ROOT *root,
                             TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    /*
      We can get to here when using a CURSOR for a query with NAME_CONST():
        DECLARE c CURSOR FOR SELECT NAME_CONST('x','y') FROM t1;
        OPEN c;
    */
    return tmp_table_field_from_field_type_maybe_null(root, table, src, param,
                                              type() == Item::NULL_ITEM);
  }
  int save_in_field(Field *field, bool no_conversions) override
  {
    return value_item->save_in_field(field, no_conversions);
  }

  bool send(Protocol *protocol, st_value *buffer) override
  {
    return value_item->send(protocol, buffer);
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function("name_const()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_name_const>(thd, this); }
};


class Item_literal: public Item_basic_constant
{
public:
  Item_literal(THD *thd): Item_basic_constant(thd)
  { }
  Item_literal(): Item_basic_constant()
  {}
  Type type() const override { return CONST_ITEM; }
  bool check_partition_func_processor(void *int_arg) override { return false;}
  bool const_item() const override { return true; }
  bool basic_const_item() const override { return true; }
  bool is_expensive() override { return false; }
  bool cleanup_is_expensive_cache_processor(void *arg) override { return 0; }
};


class Item_num: public Item_literal
{
public:
  Item_num(THD *thd): Item_literal(thd) { collation= DTCollation_numeric(); }
  Item_num(): Item_literal() { collation= DTCollation_numeric(); }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
};

#define NO_CACHED_FIELD_INDEX ((field_index_t) ~0U)

class st_select_lex;


class Item_result_field :public Item_fixed_hybrid /* Item with result field */
{
protected:
  Field *create_tmp_field_ex_from_handler(MEM_ROOT *root, TABLE *table,
                                          Tmp_field_src *src,
                                          const Tmp_field_param *param,
                                          const Type_handler *h);
public:
  Field *result_field;				/* Save result here */
  Item_result_field(THD *thd): Item_fixed_hybrid(thd), result_field(0) {}
  // Constructor used for Item_sum/Item_cond_and/or (see Item comment)
  Item_result_field(THD *thd, Item_result_field *item):
    Item_fixed_hybrid(thd, item), result_field(item->result_field)
  {}
  ~Item_result_field() {}			/* Required with gcc 2.95 */
  Field *get_tmp_table_field() override { return result_field; }
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    DBUG_ASSERT(fixed());
    const Type_handler *h= type_handler()->type_handler_for_tmp_table(this);
    return create_tmp_field_ex_from_handler(root, table, src, param, h);
  }
  void get_tmp_field_src(Tmp_field_src *src, const Tmp_field_param *param);
  /*
    This implementation of used_tables() used by Item_avg_field and
    Item_variance_field which work when only temporary table left, so theu
    return table map of the temporary table.
  */
  table_map used_tables() const override { return 1; }
  bool is_result_field() override { return true; }
  void save_in_result_field(bool no_conversions) override
  {
    save_in_field(result_field, no_conversions);
  }
  void cleanup() override;
  bool check_vcol_func_processor(void *) override { return false; }
};


class Item_ident :public Item_result_field
{
protected:
  /* 
    We have to store initial values of db_name, table_name and field_name
    to be able to restore them during cleanup() because they can be 
    updated during fix_fields() to values from Field object and life-time 
    of those is shorter than life-time of Item_field.
  */
  LEX_CSTRING orig_db_name;
  LEX_CSTRING orig_table_name;
  LEX_CSTRING orig_field_name;

  void undeclared_spvar_error() const;

public:
  Name_resolution_context *context;
  LEX_CSTRING db_name;
  LEX_CSTRING table_name;
  LEX_CSTRING field_name;
  /*
    Cached pointer to table which contains this field, used for the same reason
    by prep. stmt. too in case then we have not-fully qualified field.
    0 - means no cached value.
  */
  TABLE_LIST *cached_table;
  st_select_lex *depended_from;
  /*
    Cached value of index for this field in table->field array, used by prepared
    stmts for speeding up their re-execution. Holds NO_CACHED_FIELD_INDEX
    if index value is not known.
  */
  field_index_t cached_field_index;
  /*
    Some Items resolved in another select should not be marked as dependency
    of the subquery where they are. During normal name resolution, we check
    this. Stored procedures and prepared statements first try to resolve an
    ident item using a cached table reference and field position from the
    previous query execution (cached_table/cached_field_index). If the
    tables were not changed, the ident matches the table/field, and we have
    faster resolution of the ident without looking through all tables and
    fields in the query. But in this case, we can not check all conditions
    about this ident item dependency, so we should cache the condition in
    this variable.
  */
  bool can_be_depended;
  bool alias_name_used; /* true if item was resolved against alias */

  Item_ident(THD *thd, Name_resolution_context *context_arg,
             const LEX_CSTRING &db_name_arg, const LEX_CSTRING &table_name_arg,
             const LEX_CSTRING &field_name_arg);
  Item_ident(THD *thd, Item_ident *item);
  Item_ident(THD *thd, TABLE_LIST *view_arg, const LEX_CSTRING &field_name_arg);
  LEX_CSTRING full_name_cstring() const override;
  void cleanup() override;
  st_select_lex *get_depended_from() const;
  bool remove_dependence_processor(void * arg) override;
  void print(String *str, enum_query_type query_type) override;
  bool change_context_processor(void *cntx) override
    { context= (Name_resolution_context *)cntx; return FALSE; }
  /**
    Collect outer references
  */
  bool collect_outer_ref_processor(void *arg) override;
  Item *derived_field_transformer_for_having(THD *thd, uchar *arg) override;
  friend bool insert_fields(THD *thd, Name_resolution_context *context,
                            const char *db_name,
                            const char *table_name, List_iterator<Item> *it,
                            bool any_privileges, bool returning_field);
};


class Item_field :public Item_ident,
                  public Load_data_outvar
{
protected:
  void set_field(Field *field);
public:
  Field *field;
  Item_equal *item_equal;
  /*
    if any_privileges set to TRUE then here real effective privileges will
    be stored
  */
  privilege_t have_privileges;
  /* field need any privileges (for VIEW creation) */
  bool any_privileges;
  Item_field(THD *thd, Name_resolution_context *context_arg,
             const LEX_CSTRING &db_arg, const LEX_CSTRING &table_name_arg,
	     const LEX_CSTRING &field_name_arg);
  Item_field(THD *thd, Name_resolution_context *context_arg,
             const LEX_CSTRING &field_name_arg)
   :Item_field(thd, context_arg, null_clex_str, null_clex_str, field_name_arg)
  { }
  Item_field(THD *thd, Name_resolution_context *context_arg)
   :Item_field(thd, context_arg, null_clex_str, null_clex_str, null_clex_str)
  { }
  /*
    Constructor needed to process subselect with temporary tables (see Item)
  */
  Item_field(THD *thd, Item_field *item);
  /*
    Constructor used inside setup_wild(), ensures that field, table,
    and database names will live as long as Item_field (this is important
    in prepared statements).
  */
  Item_field(THD *thd, Name_resolution_context *context_arg, Field *field);
  /*
    If this constructor is used, fix_fields() won't work, because
    db_name, table_name and column_name are unknown. It's necessary to call
    reset_field() before fix_fields() for all fields created this way.
  */
  Item_field(THD *thd, Field *field);
  Type type() const override { return FIELD_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const override;
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;
  String *val_str(String*) override;
  void save_result(Field *to) override;
  double val_result() override;
  longlong val_int_result() override;
  bool val_native(THD *thd, Native *to) override;
  bool val_native_result(THD *thd, Native *to) override;
  String *str_result(String* tmp) override;
  my_decimal *val_decimal_result(my_decimal *) override;
  bool val_bool_result() override;
  bool is_null_result() override;
  bool is_json_type() override;
  bool send(Protocol *protocol, st_value *buffer) override;
  Load_data_outvar *get_load_data_outvar() override { return this; }
  bool load_data_set_null(THD *thd, const Load_data_param *param) override
  {
    return field->load_data_set_null(thd);
  }
  bool load_data_set_value(THD *thd, const char *pos, uint length,
                           const Load_data_param *param) override
  {
    field->load_data_set_value(pos, length, param->charset());
    return false;
  }
  bool load_data_set_no_data(THD *thd, const Load_data_param *param) override;
  void load_data_print_for_log_event(THD *thd, String *to) const override;
  bool load_data_add_outvar(THD *thd, Load_data_param *param) const override
  {
    return param->add_outvar_field(thd, field);
  }
  uint load_data_fixed_length() const override
  {
    return field->field_length;
  }
  void reset_field(Field *f);
  bool fix_fields(THD *, Item **) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;
  void make_send_field(THD *thd, Send_field *tmp_field) override;
  int save_in_field(Field *field,bool no_conversions) override;
  void save_org_in_field(Field *field, fast_field_copier optimizer_data)
    override;
  fast_field_copier setup_fast_field_copier(Field *field) override;
  table_map used_tables() const override;
  table_map all_used_tables() const override;
  const Type_handler *type_handler() const override
  {
    const Type_handler *handler= field->type_handler();
    return handler->type_handler_for_item_field();
  }
  const Type_handler *real_type_handler() const override
  {
    if (field->is_created_from_null_item)
      return &type_handler_null;
    return field->type_handler();
  }
  Field *create_tmp_field_from_item_field(MEM_ROOT *root, TABLE *new_table,
                                          Item_ref *orig_item,
                                          const Tmp_field_param *param);
  Field *create_tmp_field_ex(MEM_ROOT *root,
                             TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override;
  const TYPELIB *get_typelib() const override { return field->get_typelib(); }
  enum_monotonicity_info get_monotonicity_info() const override
  {
    return MONOTONIC_STRICT_INCREASING;
  }
  Sql_mode_dependency value_depends_on_sql_mode() const override
  {
    return Sql_mode_dependency(0, field->value_depends_on_sql_mode());
  }
  longlong val_int_endpoint(bool left_endp, bool *incl_endp) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool get_date_result(THD *thd, MYSQL_TIME *ltime,date_mode_t fuzzydate)
    override;
  longlong val_datetime_packed(THD *thd) override;
  longlong val_time_packed(THD *thd) override;
  bool is_null() override { return field->is_null(); }
  void update_null_value() override;
  void update_table_bitmaps()
  {
    if (field && field->table)
    {
      TABLE *tab= field->table;
      tab->covering_keys.intersect(field->part_of_key);
      if (tab->read_set)
        tab->mark_column_with_deps(field);
    }
  }
  void update_used_tables() override
  {
    update_table_bitmaps();
  }
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override
  {
    /*
      normilize_cond() replaced all conditions of type
         WHERE/HAVING field
      to:
        WHERE/HAVING field<>0
      By the time of a build_equal_items() call, all such conditions should
      already be replaced. No Item_field are possible.
      Note, some Item_field derivants are still possible.
      Item_insert_value:
        SELECT * FROM t1 WHERE VALUES(a);
      Item_default_value:
        SELECT * FROM t1 WHERE DEFAULT(a);
    */
    DBUG_ASSERT(type() != FIELD_ITEM);
    return Item_ident::build_equal_items(thd, inherited, link_item_fields,
                                         cond_equal_ref);
  }
  bool is_result_field() override { return false; }
  void save_in_result_field(bool no_conversions) override;
  Item *get_tmp_table_item(THD *thd) override;
  bool find_not_null_fields(table_map allowed) override;
  bool collect_item_field_processor(void * arg) override;
  bool unknown_splocal_processor(void *arg) override;
  bool add_field_to_set_processor(void * arg) override;
  bool find_item_in_field_list_processor(void *arg) override;
  bool register_field_in_read_map(void *arg) override;
  bool register_field_in_write_map(void *arg) override;
  bool register_field_in_bitmap(void *arg) override;
  bool check_partition_func_processor(void *) override {return false;}
  bool post_fix_fields_part_expr_processor(void *bool_arg) override;
  bool check_valid_arguments_processor(void *bool_arg) override;
  bool check_field_expression_processor(void *arg) override;
  bool enumerate_field_refs_processor(void *arg) override;
  bool update_table_bitmaps_processor(void *arg) override;
  bool switch_to_nullable_fields_processor(void *arg) override;
  bool update_vcol_processor(void *arg) override;
  bool rename_fields_processor(void *arg) override;
  bool check_vcol_func_processor(void *arg) override;
  bool set_fields_as_dependent_processor(void *arg) override
  {
    if (!(used_tables() & OUTER_REF_TABLE_BIT))
    {
      depended_from= (st_select_lex *) arg;
      item_equal= NULL;
    }
    return 0;
  }
  bool check_table_name_processor(void *arg) override
  {
    Check_table_name_prm &p= *static_cast<Check_table_name_prm*>(arg);
    if (!field && p.table_name.length && table_name.length)
    {
      DBUG_ASSERT(p.db.length);
      if ((db_name.length &&
          my_strcasecmp(table_alias_charset, p.db.str, db_name.str)) ||
          my_strcasecmp(table_alias_charset, p.table_name.str, table_name.str))
      {
        print(&p.field, (enum_query_type) (QT_ITEM_ORIGINAL_FUNC_NULLIF |
                                          QT_NO_DATA_EXPANSION |
                                          QT_TO_SYSTEM_CHARSET));
        return true;
      }
    }
    return false;
  }
  void cleanup() override;
  Item_equal *get_item_equal() override { return item_equal; }
  void set_item_equal(Item_equal *item_eq) override { item_equal= item_eq; }
  Item_equal *find_item_equal(COND_EQUAL *cond_equal) override;
  Item* propagate_equal_fields(THD *, const Context &, COND_EQUAL *) override;
  Item *replace_equal_field(THD *thd, uchar *arg) override;
  uint32 max_display_length() const override
  { return field->max_display_length(); }
  Item_field *field_for_view_update() override { return this; }
  int fix_outer_field(THD *thd, Field **field, Item **reference);
  Item *update_value_transformer(THD *thd, uchar *select_arg) override;
  Item *derived_field_transformer_for_having(THD *thd, uchar *arg) override;
  Item *derived_field_transformer_for_where(THD *thd, uchar *arg) override;
  Item *grouping_field_transformer_for_where(THD *thd, uchar *arg) override;
  Item *in_subq_field_transformer_for_where(THD *thd, uchar *arg) override;
  Item *in_subq_field_transformer_for_having(THD *thd, uchar *arg) override;
  void print(String *str, enum_query_type query_type) override;
  bool excl_dep_on_table(table_map tab_map) override;
  bool excl_dep_on_grouping_fields(st_select_lex *sel) override;
  bool excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred) override;
  bool cleanup_excluding_fields_processor(void *arg) override
  { return field ? 0 : cleanup_processor(arg); }
  bool cleanup_excluding_const_fields_processor(void *arg) override
  { return field && const_item() ? 0 : cleanup_processor(arg); }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_field>(thd, this); }
  bool is_outer_field() const override
  {
    DBUG_ASSERT(fixed());
    return field->table->pos_in_table_list->outer_join;
  }
  bool check_index_dependence(void *arg) override;
  friend class Item_default_value;
  friend class Item_insert_value;
  friend class st_select_lex_unit;
};


/**
  Item_field for the ROW data type
*/
class Item_field_row: public Item_field,
                      public Item_args
{
public:
  Item_field_row(THD *thd, Field *field)
   :Item_field(thd, field),
    Item_args()
  { }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_field_row>(thd, this); }

  const Type_handler *type_handler() const override
  { return &type_handler_row; }
  uint cols() const override { return arg_count; }
  Item* element_index(uint i) override { return arg_count ? args[i] : this; }
  Item** addr(uint i) override { return arg_count ? args + i : NULL; }
  bool check_cols(uint c) override
  {
    if (cols() != c)
    {
      my_error(ER_OPERAND_COLUMNS, MYF(0), c);
      return true;
    }
    return false;
  }
  bool row_create_items(THD *thd, List<Spvar_definition> *list);
};


/*
  @brief 
    Item_temptable_field is the same as Item_field, except that print() 
    continues to work even if the table has been dropped.

  @detail

    We need this item for "ANALYZE statement" feature. Query execution has 
    these steps:

      1. Run the query.
      2. Cleanup starts. Temporary tables are destroyed
      3. print "ANALYZE statement" output, if needed
      4. Call close_thread_table() for regular tables.

    Step #4 is done after step #3, so "ANALYZE stmt" has no problem printing
    Item_field objects that refer to regular tables.

    However, Step #3 is done after Step #2. Attempt to print Item_field objects
    that refer to temporary tables will cause access to freed memory. 
    
    To resolve this, we use Item_temptable_field to refer to items in temporary
    (work) tables.
*/

class Item_temptable_field :public Item_field
{
public:
  Item_temptable_field(THD *thd, Name_resolution_context *context_arg, Field *field)
   : Item_field(thd, context_arg, field) {}

  Item_temptable_field(THD *thd, Field *field)
   : Item_field(thd, field) {}

  Item_temptable_field(THD *thd, Item_field *item) : Item_field(thd, item) {};

  void print(String *str, enum_query_type query_type) override;
};


class Item_null :public Item_basic_constant
{
public:
  Item_null(THD *thd, const char *name_par=0, CHARSET_INFO *cs= &my_charset_bin):
    Item_basic_constant(thd)
  {
    set_maybe_null();
    null_value= TRUE;
    max_length= 0;
    name.str= name_par ? name_par : "NULL";
    name.length= strlen(name.str);
    collation.set(cs, DERIVATION_IGNORABLE, MY_REPERTOIRE_ASCII);
  }
  Type type() const override { return NULL_ITEM; }
  bool vcol_assignment_allowed_value() const override { return true; }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *str) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  longlong val_datetime_packed(THD *) override;
  longlong val_time_packed(THD *) override;
  int save_in_field(Field *field, bool no_conversions) override;
  int save_safe_in_field(Field *field) override;
  bool send(Protocol *protocol, st_value *buffer) override;
  const Type_handler *type_handler() const override
  { return &type_handler_null; }
  bool basic_const_item() const override { return true; }
  Item *clone_item(THD *thd) override;
  bool const_is_null() const override { return true; }
  bool is_null() override { return true; }

  void print(String *str, enum_query_type) override
  {
    str->append(NULL_clex_str);
  }

  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override;
  bool check_partition_func_processor(void *) override { return false; }
  Item_basic_constant *make_string_literal_concat(THD *thd,
                                                  const LEX_CSTRING *)
    override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_null>(thd, this); }
};

class Item_null_result :public Item_null
{
public:
  Field *result_field;
  Item_null_result(THD *thd): Item_null(thd), result_field(0) {}
  bool is_result_field() override { return result_field != 0; }
  const Type_handler *type_handler() const override
  {
    if (result_field)
      return result_field->type_handler();
    return &type_handler_null;
  }
  Field *create_tmp_field_ex(MEM_ROOT *, TABLE *, Tmp_field_src *,
                             const Tmp_field_param *) override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  void save_in_result_field(bool no_conversions) override
  {
    save_in_field(result_field, no_conversions);
  }
  bool check_partition_func_processor(void *int_arg) override { return true; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(full_name(), arg, VCOL_IMPOSSIBLE);
  }
};

/*
  Item represents one placeholder ('?') of prepared statement

  Notes:
  Item_param::field_type() is used when this item is in a temporary table.
  This is NOT placeholder metadata sent to client, as this value
  is assigned after sending metadata (in setup_one_conversion_function).
  For example in case of 'SELECT ?' you'll get MYSQL_TYPE_STRING both
  in result set and placeholders metadata, no matter what type you will
  supply for this placeholder in mysql_stmt_execute.

  Item_param has two Type_handler pointers,
  which can point to different handlers:

  1. In the Type_handler_hybrid_field_type member
     It's initialized in:
     - Item_param::setup_conversion(), for client-server PS protocol,
       according to the bind type.
     - Item_param::set_from_item(), for EXECUTE and EXECUTE IMMEDIATE,
       according to the actual parameter data type.

  2. In the "value" member.
     It's initialized in:
     - Item_param::set_param_func(), for client-server PS protocol.
     - Item_param::set_from_item(), for EXECUTE and EXECUTE IMMEDIATE.
*/

class Item_param :public Item_basic_value,
                  private Settable_routine_parameter,
                  public Rewritable_query_parameter,
                  private Type_handler_hybrid_field_type
{
  /*
    NO_VALUE is a special value meaning that the parameter has not been
    assigned yet. Item_param::state is assigned to NO_VALUE in constructor
    and is used at prepare time.

    1. At prepare time
      Item_param::fix_fields() sets "fixed" to true,
      but as Item_param::state is still NO_VALUE,
      Item_param::basic_const_item() returns false. This prevents various
      optimizations to happen at prepare time fix_fields().
      For example, in this query:
        PREPARE stmt FROM 'SELECT FORMAT(10000,2,?)';
      Item_param::basic_const_item() is tested from
      Item_func_format::fix_length_and_dec().

    2. At execute time:
      When Item_param gets a value
      (or a pseudo-value like DEFAULT_VALUE or IGNORE_VALUE):
      - Item_param::state changes from NO_VALUE to something else
      - Item_param::fixed is changed to true
      All Item_param::set_xxx() make sure to do so.
      In the state with an assigned value:
      - Item_param::basic_const_item() returns true
      - Item::type() returns NULL_ITEM or CONST_ITEM,
        depending on the value assigned.
      So in this state Item_param behaves in many cases like a literal.

      When Item_param::cleanup() is called:
      - Item_param::state does not change
      - Item_param::fixed changes to false
      Note, this puts Item_param into an inconsistent state:
      - Item_param::basic_const_item() still returns "true"
      - Item_param::type() still pretends to be a basic constant Item
      Both are not expected in combination with fixed==false.
      However, these methods are not really called in this state,
      see asserts in Item_param::basic_const_item() and Item_param::type().

      When Item_param::reset() is called:
      - Item_param::state changes to NO_VALUE
      - Item_param::fixed changes to false
  */
  enum enum_item_param_state
  {
    NO_VALUE, NULL_VALUE, SHORT_DATA_VALUE, LONG_DATA_VALUE,
    DEFAULT_VALUE, IGNORE_VALUE
  } state;

  void fix_temporal(uint32 max_length_arg, uint decimals_arg);

  struct CONVERSION_INFO
  {
    /*
      Character sets conversion info for string values.
      Character sets of client and connection defined at bind time are used
      for all conversions, even if one of them is later changed (i.e.
      between subsequent calls to mysql_stmt_execute).
    */
    CHARSET_INFO *character_set_client;
    CHARSET_INFO *character_set_of_placeholder;
    /*
      This points at character set of connection if conversion
      to it is required (i. e. if placeholder typecode is not BLOB).
      Otherwise it's equal to character_set_client (to simplify
      check in convert_str_value()).
    */
    CHARSET_INFO *final_character_set_of_str_value;
  private:
    bool needs_conversion() const
    {
      return final_character_set_of_str_value !=
             character_set_of_placeholder;
    }
    bool convert(THD *thd, String *str);
  public:
    void set(THD *thd, CHARSET_INFO *cs);
    bool convert_if_needed(THD *thd, String *str)
    {
      /*
        Check is so simple because all charsets were set up properly
        in setup_one_conversion_function, where typecode of
        placeholder was also taken into account: the variables are different
        here only if conversion is really necessary.
      */
      if (needs_conversion())
        return convert(thd, str);
      str->set_charset(final_character_set_of_str_value);
      return false;
    }
  };

  bool m_empty_string_is_null;

  class PValue_simple
  {
  public:
    union
    {
      longlong integer;
      double   real;
      CONVERSION_INFO cs_info;
      MYSQL_TIME     time;
    };
    void swap(PValue_simple &other)
    {
      swap_variables(PValue_simple, *this, other);
    }
  };

  class PValue: public Type_handler_hybrid_field_type,
                public PValue_simple,
                public Value_source
  {
  public:
    PValue(): Type_handler_hybrid_field_type(&type_handler_null) {}
    my_decimal m_decimal;
    String m_string;
    /*
      A buffer for string and long data values. Historically all allocated
      values returned from val_str() were treated as eligible to
      modification. I. e. in some cases Item_func_concat can append it's
      second argument to return value of the first one. Because of that we
      can't return the original buffer holding string data from val_str(),
      and have to have one buffer for data and another just pointing to
      the data. This is the latter one and it's returned from val_str().
      Can not be declared inside the union as it's not a POD type.
    */
    String m_string_ptr;

    void swap(PValue &other)
    {
      Type_handler_hybrid_field_type::swap(other);
      PValue_simple::swap(other);
      m_decimal.swap(other.m_decimal);
      m_string.swap(other.m_string);
      m_string_ptr.swap(other.m_string_ptr);
    }
    double val_real(const Type_std_attributes *attr) const;
    longlong val_int(const Type_std_attributes *attr) const;
    my_decimal *val_decimal(my_decimal *dec, const Type_std_attributes *attr);
    String *val_str(String *str, const Type_std_attributes *attr);
  };

  PValue value;

  const String *value_query_val_str(THD *thd, String* str) const;
  Item *value_clone_item(THD *thd);
  bool is_evaluable_expression() const override;
  bool can_return_value() const;

public:
  /*
    Used for bulk protocol only.
  */
  enum enum_indicator_type indicator;

  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }

  bool vcol_assignment_allowed_value() const override
  {
    switch (state) {
    case NULL_VALUE:
    case DEFAULT_VALUE:
    case IGNORE_VALUE:
      return true;
    case NO_VALUE:
    case SHORT_DATA_VALUE:
    case LONG_DATA_VALUE:
      break;
    }
    return false;
  }

  Item_param(THD *thd, const LEX_CSTRING *name_arg,
             uint pos_in_query_arg, uint len_in_query_arg);

  Type type() const override
  {
    // Don't pretend to be a constant unless value for this item is set.
    switch (state) {
    case NO_VALUE:         return PARAM_ITEM;
    case NULL_VALUE:       return NULL_ITEM;
    case SHORT_DATA_VALUE: return CONST_ITEM;
    case LONG_DATA_VALUE:  return CONST_ITEM;
    case DEFAULT_VALUE:    return PARAM_ITEM;
    case IGNORE_VALUE:     return PARAM_ITEM;
    }
    DBUG_ASSERT(0);
    return PARAM_ITEM;
  }

  bool is_order_clause_position() const override
  {
    return state == SHORT_DATA_VALUE &&
           type_handler()->is_order_clause_position_type();
  }

  const Item_const *get_item_const() const override
  {
    switch (state) {
    case SHORT_DATA_VALUE:
    case LONG_DATA_VALUE:
    case NULL_VALUE:
      return this;
    case IGNORE_VALUE:
    case DEFAULT_VALUE:
    case NO_VALUE:
      break;
    }
    return NULL;
  }

  bool const_is_null() const override { return state == NULL_VALUE; }
  bool can_return_const_value(Item_result type) const
  {
    return can_return_value() &&
           value.type_handler()->cmp_type() == type &&
           type_handler()->cmp_type() == type;
  }
  const longlong *const_ptr_longlong() const override
  { return can_return_const_value(INT_RESULT) ? &value.integer : NULL; }
  const double *const_ptr_double() const override
  { return can_return_const_value(REAL_RESULT) ? &value.real : NULL; }
  const my_decimal *const_ptr_my_decimal() const override
  { return can_return_const_value(DECIMAL_RESULT) ? &value.m_decimal : NULL; }
  const MYSQL_TIME *const_ptr_mysql_time() const override
  { return can_return_const_value(TIME_RESULT) ? &value.time : NULL; }
  const String *const_ptr_string() const override
  { return can_return_const_value(STRING_RESULT) ? &value.m_string : NULL; }

  double val_real() override
  {
    return can_return_value() ? value.val_real(this) : 0e0;
  }
  longlong val_int() override
  {
    return can_return_value() ? value.val_int(this) : 0;
  }
  my_decimal *val_decimal(my_decimal *dec) override
  {
    return can_return_value() ? value.val_decimal(dec, this) : NULL;
  }
  String *val_str(String *str) override
  {
    return can_return_value() ? value.val_str(str, this) : NULL;
  }
  bool get_date(THD *thd, MYSQL_TIME *tm, date_mode_t fuzzydate) override;
  bool val_native(THD *thd, Native *to) override
  {
    return Item_param::type_handler()->Item_param_val_native(thd, this, to);
  }

  int save_in_field(Field *field, bool no_conversions) override;

  void set_default();
  void set_ignore();
  void set_null();
  void set_int(longlong i, uint32 max_length_arg);
  void set_double(double i);
  void set_decimal(const char *str, ulong length);
  void set_decimal(const my_decimal *dv, bool unsigned_arg);
  bool set_str(const char *str, ulong length,
               CHARSET_INFO *fromcs, CHARSET_INFO *tocs);
  bool set_longdata(const char *str, ulong length);
  void set_time(MYSQL_TIME *tm, timestamp_type type, uint32 max_length_arg);
  void set_time(const MYSQL_TIME *tm, uint32 max_length_arg, uint decimals_arg);
  bool set_from_item(THD *thd, Item *item);
  void reset();

  void set_param_tiny(uchar **pos, ulong len);
  void set_param_short(uchar **pos, ulong len);
  void set_param_int32(uchar **pos, ulong len);
  void set_param_int64(uchar **pos, ulong len);
  void set_param_float(uchar **pos, ulong len);
  void set_param_double(uchar **pos, ulong len);
  void set_param_decimal(uchar **pos, ulong len);
  void set_param_time(uchar **pos, ulong len);
  void set_param_datetime(uchar **pos, ulong len);
  void set_param_date(uchar **pos, ulong len);
  void set_param_str(uchar **pos, ulong len);

  void setup_conversion(THD *thd, uchar param_type);
  void setup_conversion_blob(THD *thd);
  void setup_conversion_string(THD *thd, CHARSET_INFO *fromcs);

  /*
    Assign placeholder value from bind data.
    Note, that 'len' has different semantics in embedded library (as we
    don't need to check that packet is not broken there). See
    sql_prepare.cc for details.
  */
  void set_param_func(uchar **pos, ulong len)
  {
    /*
      To avoid Item_param::set_xxx() asserting on data type mismatch,
      we set the value type handler here:
      - It can not be initialized yet after Item_param::setup_conversion().
      - Also, for LIMIT clause parameters, the value type handler might have
        changed from the real type handler to type_handler_longlong.
        So here we'll restore it.
    */
    const Type_handler *h= Item_param::type_handler();
    value.set_handler(h);
    h->Item_param_set_param_func(this, pos, len);
  }

  bool set_value(THD *thd, const Type_all_attributes *attr,
                 const st_value *val, const Type_handler *h)
  {
    value.set_handler(h); // See comments in set_param_func()
    return h->Item_param_set_from_value(thd, this, attr, val);
  }

  bool set_limit_clause_param(longlong nr)
  {
    value.set_handler(&type_handler_slonglong);
    set_int(nr, MY_INT64_NUM_DECIMAL_DIGITS);
    return !unsigned_flag && value.integer < 0;
  }
  const String *query_val_str(THD *thd, String *str) const;

  bool convert_str_value(THD *thd);

  /*
    If value for parameter was not set we treat it as non-const
    so no one will use parameters value in fix_fields still
    parameter is constant during execution.
  */
  bool const_item() const override
  {
    return state != NO_VALUE;
  }
  table_map used_tables() const override
  {
    return state != NO_VALUE ? (table_map)0 : PARAM_TABLE_BIT;
  }
  void print(String *str, enum_query_type query_type) override;
  bool is_null() override
  { DBUG_ASSERT(state != NO_VALUE); return state == NULL_VALUE; }
  bool basic_const_item() const override;
  bool has_no_value() const
  {
    return state == NO_VALUE;
  }
  bool has_long_data_value() const
  {
    return state == LONG_DATA_VALUE;
  }
  bool has_int_value() const
  {
    return state == SHORT_DATA_VALUE &&
           value.type_handler()->cmp_type() == INT_RESULT;
  }
  bool is_stored_routine_parameter() const override { return true; }
  /*
    This method is used to make a copy of a basic constant item when
    propagating constants in the optimizer. The reason to create a new
    item and not use the existing one is not precisely known (2005/04/16).
    Probably we are trying to preserve tree structure of items, in other
    words, avoid pointing at one item from two different nodes of the tree.
    Return a new basic constant item if parameter value is a basic
    constant, assert otherwise. This method is called only if
    basic_const_item returned TRUE.
  */
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override;
  Item *clone_item(THD *thd) override;
  void set_param_type_and_swap_value(Item_param *from);

  Rewritable_query_parameter *get_rewritable_query_parameter() override
  { return this; }
  Settable_routine_parameter *get_settable_routine_parameter() override
  { return m_is_settable_routine_parameter ? this : nullptr; }

  bool append_for_log(THD *thd, String *str) override;
  bool check_vcol_func_processor(void *) override { return false; }
  Item *get_copy(THD *) override { return nullptr; }
  bool add_as_clone(THD *thd);
  void sync_clones();
  bool register_clone(Item_param *i) { return m_clones.push_back(i); }

private:
  void invalid_default_param() const;
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;
  void set_out_param_info(Send_field *info) override;

public:
  const Send_field *get_out_param_info() const override;
  Item_param *get_item_param() override { return this; }
  void make_send_field(THD *thd, Send_field *field) override;

private:
  Send_field *m_out_param_info;
  bool m_is_settable_routine_parameter;
  /*
    Array of all references of this parameter marker used in a CTE to its clones
    created for copies of this marker used the CTE's copies. It's used to
    synchronize the actual value of the parameter with the values of the clones.
  */
  Mem_root_array<Item_param *, true> m_clones;
};


class Item_int :public Item_num
{
public:
  longlong value;
  Item_int(THD *thd, int32 i,size_t length= MY_INT32_NUM_DECIMAL_DIGITS):
    Item_num(thd), value((longlong) i)
    { max_length=(uint32)length; }
  Item_int(THD *thd, longlong i,size_t length= MY_INT64_NUM_DECIMAL_DIGITS):
    Item_num(thd), value(i)
    { max_length=(uint32)length; }
  Item_int(THD *thd, ulonglong i, size_t length= MY_INT64_NUM_DECIMAL_DIGITS):
    Item_num(thd), value((longlong)i)
    { max_length=(uint32)length; unsigned_flag= 1; }
  Item_int(THD *thd, const char *str_arg,longlong i,size_t length):
    Item_num(thd), value(i)
    {
      max_length=(uint32)length;
      name.str= str_arg; name.length= safe_strlen(name.str);
    }
  Item_int(THD *thd, const char *str_arg,longlong i,size_t length, bool flag):
    Item_num(thd), value(i)
    {
      max_length=(uint32)length;
      name.str= str_arg; name.length= safe_strlen(name.str);
      unsigned_flag= flag;
    }
  Item_int(const char *str_arg,longlong i,size_t length):
    Item_num(), value(i)
    {
      max_length=(uint32)length;
      name.str= str_arg; name.length= safe_strlen(name.str);
      unsigned_flag= 1;
    }
  Item_int(THD *thd, const char *str_arg, size_t length=64);
  const Type_handler *type_handler() const override
  { return type_handler_long_or_longlong(); }
  Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table) override
  { return tmp_table_field_from_field_type(root, table); }
  const longlong *const_ptr_longlong() const override { return &value; }
  longlong val_int() override { return value; }
  longlong val_int_min() const override { return value; }
  double val_real() override { return (double) value; }
  my_decimal *val_decimal(my_decimal *) override;
  String *val_str(String*) override;
  int save_in_field(Field *field, bool no_conversions) override;
  bool is_order_clause_position() const override { return true; }
  Item *clone_item(THD *thd) override;
  void print(String *str, enum_query_type query_type) override;
  Item *neg(THD *thd) override;
  decimal_digits_t decimal_precision() const override
  { return (decimal_digits_t) (max_length - MY_TEST(value < 0)); }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_int>(thd, this); }
};


/*
  We sometimes need to distinguish a number from a boolean:
  a[1] and a[true] are different things in XPath.
  Also in JSON boolean values should be treated differently.
*/
class Item_bool :public Item_int
{
public:
  Item_bool(THD *thd, const char *str_arg, longlong i):
    Item_int(thd, str_arg, i, 1) {}
  Item_bool(THD *thd, bool i) :Item_int(thd, (longlong) i, 1) { }
  Item_bool(const char *str_arg, longlong i):
    Item_int(str_arg, i, 1) {}
  bool is_bool_literal() const override { return true; }
  Item *neg_transformer(THD *thd) override;
  const Type_handler *type_handler() const override
  { return &type_handler_bool; }
  const Type_handler *fixed_type_handler() const override
  { return &type_handler_bool; }
  void quick_fix_field() override
  {
    /*
      We can get here when Item_bool is created instead of a constant
      predicate at various condition optimization stages in sql_select.
    */
  }
};


class Item_bool_static :public Item_bool
{
public:
  Item_bool_static(const char *str_arg, longlong i):
    Item_bool(str_arg, i) {};

  void set_join_tab_idx(uint8 join_tab_idx_arg) override
  { DBUG_ASSERT(0); }
};

extern const Item_bool_static Item_false, Item_true;

class Item_uint :public Item_int
{
public:
  Item_uint(THD *thd, const char *str_arg, size_t length);
  Item_uint(THD *thd, ulonglong i): Item_int(thd, i, 10) {}
  Item_uint(THD *thd, const char *str_arg, longlong i, uint length);
  double val_real()  override { return ulonglong2double((ulonglong)value); }
  Item *clone_item(THD *thd) override;
  Item *neg(THD *thd) override;
  decimal_digits_t decimal_precision() const override
  { return decimal_digits_t(max_length); }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_uint>(thd, this); }
};


class Item_datetime :public Item_int
{
protected:
  MYSQL_TIME ltime;
public:
  Item_datetime(THD *thd): Item_int(thd, 0) { unsigned_flag=0; }
  int save_in_field(Field *field, bool no_conversions) override;
  longlong val_int() override;
  double val_real() override { return (double)val_int(); }
  void set(longlong packed, enum_mysql_timestamp_type ts_type);
  bool get_date(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate) override
  {
    *to= ltime;
    return false;
  }
};


/* decimal (fixed point) constant */
class Item_decimal :public Item_num
{
protected:
  my_decimal decimal_value;
public:
  Item_decimal(THD *thd, const char *str_arg, size_t length,
               CHARSET_INFO *charset);
  Item_decimal(THD *thd, const char *str, const my_decimal *val_arg,
               uint decimal_par, uint length);
  Item_decimal(THD *thd, const my_decimal *value_par);
  Item_decimal(THD *thd, longlong val, bool unsig);
  Item_decimal(THD *thd, double val, int precision, int scale);
  Item_decimal(THD *thd, const uchar *bin, int precision, int scale);

  const Type_handler *type_handler() const override
  { return &type_handler_newdecimal; }
  longlong val_int() override
  { return decimal_value.to_longlong(unsigned_flag); }
  double val_real() override
  { return decimal_value.to_double(); }
  String *val_str(String *to) override
  { return decimal_value.to_string(to); }
  my_decimal *val_decimal(my_decimal *val) override
  { return &decimal_value; }
  const my_decimal *const_ptr_my_decimal() const override
  { return &decimal_value; }
  int save_in_field(Field *field, bool no_conversions) override;
  Item *clone_item(THD *thd) override;
  void print(String *str, enum_query_type query_type) override
  {
    decimal_value.to_string(&str_value);
    str->append(str_value);
  }
  Item *neg(THD *thd) override;
  decimal_digits_t decimal_precision() const override
  { return decimal_value.precision(); }
  void set_decimal_value(my_decimal *value_par);
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_decimal>(thd, this); }
};


class Item_float :public Item_num
{
  const char *presentation;
public:
  double value;
  Item_float(THD *thd, const char *str_arg, size_t length);
  Item_float(THD *thd, const char *str, double val_arg, uint decimal_par,
             uint length): Item_num(thd), value(val_arg)
  {
    presentation= name.str= str;
    name.length= safe_strlen(str);
    decimals=(uint8) decimal_par;
    max_length= length;
  }
  Item_float(THD *thd, double value_par, uint decimal_par):
    Item_num(thd), presentation(0), value(value_par)
  {
    decimals= (uint8) decimal_par;
  }
  int save_in_field(Field *field, bool no_conversions) override;
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  const double *const_ptr_double() const override { return &value; }
  double val_real() override { return value; }
  longlong val_int() override
  {
    if (value <= (double) LONGLONG_MIN)
    {
       return LONGLONG_MIN;
    }
    else if (value >= (double) (ulonglong) LONGLONG_MAX)
    {
      return LONGLONG_MAX;
    }
    return (longlong) rint(value);
  }
  String *val_str(String*) override;
  my_decimal *val_decimal(my_decimal *) override;
  Item *clone_item(THD *thd) override;
  Item *neg(THD *thd) override;
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_float>(thd, this); }
};


class Item_static_float_func :public Item_float
{
  const char *func_name;
public:
  Item_static_float_func(THD *thd, const char *str, double val_arg,
                         uint decimal_par, uint length):
    Item_float(thd, NullS, val_arg, decimal_par, length), func_name(str)
  {}
  void print(String *str, enum_query_type) override
  {
    str->append(func_name, strlen(func_name));
  }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override
  {
    return const_charset_converter(thd, tocs, true, func_name);
  }
};


class Item_string :public Item_literal
{
protected:
  void fix_from_value(Derivation dv, const Metadata metadata)
  {
    fix_charset_and_length(str_value.charset(), dv, metadata);
  }
  void fix_and_set_name_from_value(THD *thd, Derivation dv,
                                   const Metadata metadata)
  {
    fix_from_value(dv, metadata);
    set_name(thd, &str_value);
  }
protected:
  /* Just create an item and do not fill string representation */
  Item_string(THD *thd, CHARSET_INFO *cs, Derivation dv= DERIVATION_COERCIBLE):
    Item_literal(thd)
  {
    collation.set(cs, dv);
    max_length= 0;
    set_name(thd, NULL, 0, system_charset_info);
    decimals= NOT_FIXED_DEC;
  }
public:
  Item_string(THD *thd, CHARSET_INFO *csi, const char *str_arg, uint length_arg)
   :Item_literal(thd)
  {
    collation.set(csi, DERIVATION_COERCIBLE);
    set_name(thd, NULL, 0, system_charset_info);
    decimals= NOT_FIXED_DEC;
    str_value.copy(str_arg, length_arg, csi);
    max_length= str_value.numchars() * csi->mbmaxlen;
  }
  // Constructors with the item name set from its value
  Item_string(THD *thd, const char *str, uint length, CHARSET_INFO *cs,
              Derivation dv, my_repertoire_t repertoire)
   :Item_literal(thd)
  {
    str_value.set_or_copy_aligned(str, length, cs);
    fix_and_set_name_from_value(thd, dv, Metadata(&str_value, repertoire));
  }
  Item_string(THD *thd, const char *str, size_t length,
              CHARSET_INFO *cs, Derivation dv= DERIVATION_COERCIBLE)
   :Item_literal(thd)
  {
    str_value.set_or_copy_aligned(str, length, cs);
    fix_and_set_name_from_value(thd, dv, Metadata(&str_value));
  }
  Item_string(THD *thd, const String *str, CHARSET_INFO *tocs, uint *conv_errors,
              Derivation dv, my_repertoire_t repertoire)
   :Item_literal(thd)
  {
    if (str_value.copy(str, tocs, conv_errors))
      str_value.set("", 0, tocs); // EOM ?
    str_value.mark_as_const();
    fix_and_set_name_from_value(thd, dv, Metadata(&str_value, repertoire));
  }
  // Constructors with an externally provided item name
  Item_string(THD *thd, const LEX_CSTRING &name_par, const LEX_CSTRING &str,
              CHARSET_INFO *cs, Derivation dv= DERIVATION_COERCIBLE)
   :Item_literal(thd)
  {
    str_value.set_or_copy_aligned(str.str, str.length, cs);
    fix_from_value(dv, Metadata(&str_value));
    set_name(thd, name_par);
  }
  Item_string(THD *thd, const LEX_CSTRING &name_par, const LEX_CSTRING &str,
              CHARSET_INFO *cs, Derivation dv, my_repertoire_t repertoire)
   :Item_literal(thd)
  {
    str_value.set_or_copy_aligned(str.str, str.length, cs);
    fix_from_value(dv, Metadata(&str_value, repertoire));
    set_name(thd, name_par);
  }
  void print_value(String *to) const
  {
    str_value.print(to);
  }
  double val_real() override;
  longlong val_int() override;
  const String *const_ptr_string() const override { return &str_value; }
  String *val_str(String*) override
  {
    return (String*) &str_value;
  }
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return get_date_from_string(thd, ltime, fuzzydate);
  }
  int save_in_field(Field *field, bool no_conversions) override;
  const Type_handler *type_handler() const override
  { return &type_handler_varchar; }
  Item *clone_item(THD *thd) override;
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override
  {
    return const_charset_converter(thd, tocs, true);
  }
  inline void append(const char *str, uint length)
  {
    str_value.append(str, length);
    max_length= str_value.numchars() * collation.collation->mbmaxlen;
  }
  void print(String *str, enum_query_type query_type) override;

  /**
    Return TRUE if character-set-introducer was explicitly specified in the
    original query for this item (text literal).

    This operation is to be called from Item_string::print(). The idea is
    that when a query is generated (re-constructed) from the Item-tree,
    character-set-introducers should appear only for those literals, where
    they were explicitly specified by the user. Otherwise, that may lead to
    loss collation information (character set introducers implies default
    collation for the literal).

    Basically, that makes sense only for views and hopefully will be gone
    one day when we start using original query as a view definition.

    @return This operation returns the value of m_cs_specified attribute.
      @retval TRUE if character set introducer was explicitly specified in
      the original query.
      @retval FALSE otherwise.
  */
  virtual bool is_cs_specified() const
  {
    return false;
  }

  String *check_well_formed_result(bool send_error)
  { return Item::check_well_formed_result(&str_value, send_error); }

  Item_basic_constant *make_string_literal_concat(THD *thd,
                                                  const LEX_CSTRING *) override;
  Item *make_odbc_literal(THD *thd, const LEX_CSTRING *typestr) override;

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_string>(thd, this); }

};


class Item_string_with_introducer :public Item_string
{
public:
  Item_string_with_introducer(THD *thd, const LEX_CSTRING &str,
                              CHARSET_INFO *cs):
    Item_string(thd, str.str, str.length, cs)
  { }
  Item_string_with_introducer(THD *thd, const LEX_CSTRING &name_arg,
                              const LEX_CSTRING &str, CHARSET_INFO *tocs):
    Item_string(thd, name_arg, str, tocs)
  { }
  virtual bool is_cs_specified() const
  {
    return true;
  }
};


class Item_string_sys :public Item_string
{
public:
  Item_string_sys(THD *thd, const char *str, uint length):
    Item_string(thd, str, length, system_charset_info)
  { }
  Item_string_sys(THD *thd, const char *str):
    Item_string(thd, str, (uint) strlen(str), system_charset_info)
  { }
};


class Item_string_ascii :public Item_string
{
public:
  Item_string_ascii(THD *thd, const char *str, uint length):
    Item_string(thd, str, length, &my_charset_latin1,
                DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII)
  { }
  Item_string_ascii(THD *thd, const char *str):
    Item_string(thd, str, (uint) strlen(str), &my_charset_latin1,
                DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII)
  { }
};


class Item_static_string_func :public Item_string
{
  const LEX_CSTRING func_name;
public:
  Item_static_string_func(THD *thd, const LEX_CSTRING &name_par,
                          const LEX_CSTRING &str, CHARSET_INFO *cs,
                          Derivation dv= DERIVATION_COERCIBLE):
    Item_string(thd, LEX_CSTRING({NullS,0}), str, cs, dv), func_name(name_par)
  {}
  Item_static_string_func(THD *thd, const LEX_CSTRING &name_par,
                          const String *str,
                          CHARSET_INFO *tocs, uint *conv_errors,
                          Derivation dv, my_repertoire_t repertoire):
    Item_string(thd, str, tocs, conv_errors, dv, repertoire),
    func_name(name_par)
  {}
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override
  {
    return const_charset_converter(thd, tocs, true, func_name.str);
  }

  void print(String *str, enum_query_type) override
  {
    str->append(func_name);
  }

  bool check_partition_func_processor(void *) override { return true; }

  bool check_vcol_func_processor(void *arg) override
  { // VCOL_TIME_FUNC because the value is not constant, but does not
    // require fix_fields() to be re-run for every statement.
    return mark_unsupported_function(func_name.str, arg, VCOL_TIME_FUNC);
  }
};


/* for show tables */
class Item_partition_func_safe_string: public Item_string
{
public:
  Item_partition_func_safe_string(THD *thd, const LEX_CSTRING &name_arg,
                                  uint length, CHARSET_INFO *cs):
    Item_string(thd, name_arg, LEX_CSTRING({0,0}), cs)
  {
    max_length= length;
  }
  bool check_vcol_func_processor(void *arg) 
  {
    return mark_unsupported_function("safe_string", arg, VCOL_IMPOSSIBLE);
  }
};


/**
  Item_empty_string -- is a utility class to put an item into List<Item>
  which is then used in protocol.send_result_set_metadata() when sending SHOW output to
  the client.
*/

class Item_empty_string :public Item_partition_func_safe_string
{
public:
  Item_empty_string(THD *thd, const LEX_CSTRING &header, uint length,
                    CHARSET_INFO *cs= &my_charset_utf8mb3_general_ci)
   :Item_partition_func_safe_string(thd, header, length * cs->mbmaxlen, cs)
  { }
  Item_empty_string(THD *thd, const char *header, uint length,
                    CHARSET_INFO *cs= &my_charset_utf8mb3_general_ci)
   :Item_partition_func_safe_string(thd, LEX_CSTRING({header, strlen(header)}),
                                    length * cs->mbmaxlen, cs)
  { }
  void make_send_field(THD *thd, Send_field *field);
};


class Item_return_int :public Item_int
{
  enum_field_types int_field_type;
public:
  Item_return_int(THD *thd, const char *name_arg, uint length,
		  enum_field_types field_type_arg, longlong value_arg= 0):
    Item_int(thd, name_arg, value_arg, length), int_field_type(field_type_arg)
  {
    unsigned_flag=1;
  }
  const Type_handler *type_handler() const override
  {
    const Type_handler *h=
      Type_handler::get_handler_by_field_type(int_field_type);
    return unsigned_flag ? h->type_handler_unsigned() : h;
  }
};


/**
  Item_hex_constant -- a common class for hex literals: X'HHHH' and 0xHHHH
*/
class Item_hex_constant: public Item_literal
{
private:
  void hex_string_init(THD *thd, const char *str, size_t str_length);
public:
  Item_hex_constant(THD *thd): Item_literal(thd)
  {
    hex_string_init(thd, "", 0);
  }
  Item_hex_constant(THD *thd, const char *str, size_t str_length):
    Item_literal(thd)
  {
    hex_string_init(thd, str, str_length);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_varchar; }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override
  {
    return const_charset_converter(thd, tocs, true);
  }
  const String *const_ptr_string() const override { return &str_value; }
  String *val_str(String*) override { return &str_value; }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
};


/**
  Item_hex_hybrid -- is a class implementing 0xHHHH literals, e.g.:
    SELECT 0x3132;
  They can behave as numbers and as strings depending on context.
*/
class Item_hex_hybrid: public Item_hex_constant
{
public:
  Item_hex_hybrid(THD *thd): Item_hex_constant(thd) {}
  Item_hex_hybrid(THD *thd, const char *str, size_t str_length):
    Item_hex_constant(thd, str, str_length) {}
  const Type_handler *type_handler() const override
  { return &type_handler_hex_hybrid; }
  decimal_digits_t decimal_precision() const override;
  double val_real() override
  {
    return (double) (ulonglong) Item_hex_hybrid::val_int();
  }
  longlong val_int() override
  {
    return longlong_from_hex_hybrid(str_value.ptr(), str_value.length());
  }
  my_decimal *val_decimal(my_decimal *decimal_value) override
  {
    longlong value= Item_hex_hybrid::val_int();
    int2my_decimal(E_DEC_FATAL_ERROR, value, TRUE, decimal_value);
    return decimal_value;
  }
  int save_in_field(Field *field, bool) override
  {
    field->set_notnull();
    return field->store_hex_hybrid(str_value.ptr(), str_value.length());
  }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_hex_hybrid>(thd, this); }
};


/**
  Item_hex_string -- is a class implementing X'HHHH' literals, e.g.:
    SELECT X'3132';
  Unlike Item_hex_hybrid, X'HHHH' literals behave as strings in all contexts.
  X'HHHH' are also used in replication of string constants in case of
  "dangerous" charsets (sjis, cp932, big5, gbk) who can have backslash (0x5C)
  as the second byte of a multi-byte character, so using '\' escaping for
  these charsets is not desirable.
*/
class Item_hex_string: public Item_hex_constant
{
public:
  Item_hex_string(THD *thd): Item_hex_constant(thd) {}
  Item_hex_string(THD *thd, const char *str, size_t str_length):
    Item_hex_constant(thd, str, str_length) {}
  longlong val_int() override
  {
    return longlong_from_string_with_check(&str_value);
  }
  double val_real() override
  {
    return double_from_string_with_check(&str_value);
  }
  my_decimal *val_decimal(my_decimal *decimal_value) override
  {
    return val_decimal_from_string(decimal_value);
  }
  int save_in_field(Field *field, bool) override
  {
    field->set_notnull();
    return field->store(str_value.ptr(), str_value.length(), 
                        collation.collation);
  }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_hex_string>(thd, this); }
};


class Item_bin_string: public Item_hex_hybrid
{
public:
  Item_bin_string(THD *thd, const char *str, size_t str_length);
};


class Item_timestamp_literal: public Item_literal
{
  Timestamp_or_zero_datetime m_value;
public:
  Item_timestamp_literal(THD *thd)
   :Item_literal(thd)
  { }
  const Type_handler *type_handler() const override
  { return &type_handler_timestamp2; }
  int save_in_field(Field *field, bool) override
  {
    Timestamp_or_zero_datetime_native native(m_value, decimals);
    return native.save_in_field(field, decimals);
  }
  longlong val_int() override
  {
    return m_value.to_datetime(current_thd).to_longlong();
  }
  double val_real() override
  {
    return m_value.to_datetime(current_thd).to_double();
  }
  String *val_str(String *to) override
  {
    return m_value.to_datetime(current_thd).to_string(to, decimals);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return m_value.to_datetime(current_thd).to_decimal(to);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    bool res= m_value.to_TIME(thd, ltime, fuzzydate);
    DBUG_ASSERT(!res);
    return res;
  }
  bool val_native(THD *thd, Native *to) override
  {
    return m_value.to_native(to, decimals);
  }
  void set_value(const Timestamp_or_zero_datetime &value)
  {
    m_value= value;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_timestamp_literal>(thd, this); }
};


class Item_temporal_literal :public Item_literal
{
public:
  Item_temporal_literal(THD *thd)
   :Item_literal(thd)
  {
    collation= DTCollation_numeric();
    decimals= 0;
  }
  Item_temporal_literal(THD *thd, decimal_digits_t dec_arg):
    Item_literal(thd)
  {
    collation= DTCollation_numeric();
    decimals= dec_arg;
  }

  int save_in_field(Field *field, bool no_conversions) override
  { return save_date_in_field(field, no_conversions); }
};


/**
  DATE'2010-01-01'
*/
class Item_date_literal: public Item_temporal_literal
{
protected:
  Date cached_time;
  bool update_null()
  {
    return (maybe_null() &&
            (null_value= cached_time.check_date_with_warn(current_thd)));
  }
public:
  Item_date_literal(THD *thd, const Date *ltime)
    :Item_temporal_literal(thd),
     cached_time(*ltime)
  {
    DBUG_ASSERT(cached_time.is_valid_date());
    max_length= MAX_DATE_WIDTH;
    /*
      If date has zero month or day, it can return NULL in case of
      NO_ZERO_DATE or NO_ZERO_IN_DATE.
      If date is `February 30`, it can return NULL in case if
      no ALLOW_INVALID_DATES is set.
      We can't set null_value using the current sql_mode here in constructor,
      because sql_mode can change in case of prepared statements
      between PREPARE and EXECUTE.
      Here we only set maybe_null to true if the value has such anomalies.
      Later (during execution time), if maybe_null is true, then the value
      will be checked per row, according to the execution time sql_mode.
      The check_date() below call should cover all cases mentioned.
    */
    set_maybe_null(cached_time.check_date(TIME_NO_ZERO_DATE |
                                          TIME_NO_ZERO_IN_DATE));
  }
  const Type_handler *type_handler() const override
  { return &type_handler_newdate; }
  void print(String *str, enum_query_type query_type) override;
  const MYSQL_TIME *const_ptr_mysql_time() const override
  {
    return cached_time.get_mysql_time();
  }
  Item *clone_item(THD *thd) override;
  longlong val_int() override
  {
    return update_null() ? 0 : cached_time.to_longlong();
  }
  double val_real() override
  {
    return update_null() ? 0 : cached_time.to_double();
  }
  String *val_str(String *to) override
  {
    return update_null() ? 0 : cached_time.to_string(to);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return update_null() ? 0 : cached_time.to_decimal(to);
  }
  longlong val_datetime_packed(THD *thd) override
  {
    return update_null() ? 0 : cached_time.valid_date_to_packed();
  }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_date_literal>(thd, this); }
};


/**
  TIME'10:10:10'
*/
class Item_time_literal final: public Item_temporal_literal
{
protected:
  Time cached_time;
public:
  Item_time_literal(THD *thd, const Time *ltime, decimal_digits_t dec_arg):
    Item_temporal_literal(thd, dec_arg),
    cached_time(*ltime)
  {
    DBUG_ASSERT(cached_time.is_valid_time());
    max_length= MIN_TIME_WIDTH + (decimals ? decimals + 1 : 0);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_time2; }
  void print(String *str, enum_query_type query_type) override;
  const MYSQL_TIME *const_ptr_mysql_time() const override
  {
    return cached_time.get_mysql_time();
  }
  Item *clone_item(THD *thd) override;
  longlong val_int() override { return cached_time.to_longlong(); }
  double val_real() override { return cached_time.to_double(); }
  String *val_str(String *to) override
  { return cached_time.to_string(to, decimals); }
  my_decimal *val_decimal(my_decimal *to) override
  { return cached_time.to_decimal(to); }
  longlong val_time_packed(THD *thd) override
  {
    return cached_time.valid_time_to_packed();
  }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate) override;
  bool val_native(THD *thd, Native *to) override
  {
    return Time(thd, this).to_native(to, decimals);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_time_literal>(thd, this); }
};


/**
  TIMESTAMP'2001-01-01 10:20:30'
*/

class Item_datetime_literal: public Item_temporal_literal
{
protected:
  Datetime cached_time;
  bool update_null()
  {
    return (maybe_null() &&
            (null_value= cached_time.check_date_with_warn(current_thd)));
  }
public:
  Item_datetime_literal(THD *thd, const Datetime *ltime,
                        decimal_digits_t dec_arg):
    Item_temporal_literal(thd, dec_arg),
    cached_time(*ltime)
  {
    DBUG_ASSERT(cached_time.is_valid_datetime());
    max_length= MAX_DATETIME_WIDTH + (decimals ? decimals + 1 : 0);
    // See the comment on maybe_null in Item_date_literal
    set_maybe_null(cached_time.check_date(TIME_NO_ZERO_DATE |
                                          TIME_NO_ZERO_IN_DATE));
  }
  const Type_handler *type_handler() const override
  { return &type_handler_datetime2; }
  void print(String *str, enum_query_type query_type) override;
  const MYSQL_TIME *const_ptr_mysql_time() const override
  {
    return cached_time.get_mysql_time();
  }
  Item *clone_item(THD *thd) override;
  longlong val_int() override
  {
    return update_null() ? 0 : cached_time.to_longlong();
  }
  double val_real() override
  {
    return update_null() ? 0 : cached_time.to_double();
  }
  String *val_str(String *to) override
  {
    return update_null() ? NULL : cached_time.to_string(to, decimals);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return update_null() ? NULL : cached_time.to_decimal(to);
  }
  longlong val_datetime_packed(THD *thd) override
  {
    return update_null() ? 0 : cached_time.valid_datetime_to_packed();
  }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_datetime_literal>(thd, this); }
};


/**
  An error-safe counterpart for Item_date_literal
*/
class Item_date_literal_for_invalid_dates: public Item_date_literal
{
  /**
    During equal field propagation we can replace non-temporal constants
    found in equalities to their native temporal equivalents:
      WHERE date_column='2001-01-01'      ... ->
      WHERE date_column=DATE'2001-01-01'  ...

    This is done to make the equal field propagation code handle mixtures of
    different temporal types in the same expressions easier (MDEV-8706), e.g.
      WHERE LENGTH(date_column)=10 AND date_column=TIME'00:00:00'

    Item_date_literal_for_invalid_dates::get_date()
    (unlike the regular Item_date_literal::get_date())
    does not check the result for NO_ZERO_IN_DATE and NO_ZERO_DATE,
    always returns success (false), and does not produce error/warning messages.

    We need these _for_invalid_dates classes to be able to rewrite:
      SELECT * FROM t1 WHERE date_column='0000-00-00' ...
    to:
      SELECT * FROM t1 WHERE date_column=DATE'0000-00-00' ...

    to avoid returning NULL value instead of '0000-00-00' even
    in sql_mode=TRADITIONAL.
  */
public:
  Item_date_literal_for_invalid_dates(THD *thd, const Date *ltime)
   :Item_date_literal(thd, ltime)
  {
    base_flags&= ~item_base_t::MAYBE_NULL;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    cached_time.copy_to_mysql_time(ltime);
    return (null_value= false);
  }
};


/**
  An error-safe counterpart for Item_datetime_literal
  (see Item_date_literal_for_invalid_dates for comments)
*/
class Item_datetime_literal_for_invalid_dates final: public Item_datetime_literal
{
public:
  Item_datetime_literal_for_invalid_dates(THD *thd,
                                          const Datetime *ltime,
                                          decimal_digits_t dec_arg)
   :Item_datetime_literal(thd, ltime, dec_arg)
  {
    base_flags&= ~item_base_t::MAYBE_NULL;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    cached_time.copy_to_mysql_time(ltime);
    return (null_value= false);
  }
};


class Used_tables_and_const_cache
{
public:
  /*
    In some cases used_tables_cache is not what used_tables() return
    so the method should be used where one need used tables bit map
    (even internally in Item_func_* code).
  */
  table_map used_tables_cache;
  bool const_item_cache;

  Used_tables_and_const_cache()
   :used_tables_cache(0),
    const_item_cache(true)
  { }
  Used_tables_and_const_cache(const Used_tables_and_const_cache *other)
   :used_tables_cache(other->used_tables_cache),
    const_item_cache(other->const_item_cache)
  { }
  void used_tables_and_const_cache_init()
  {
    used_tables_cache= 0;
    const_item_cache= true;
  }
  void used_tables_and_const_cache_join(const Item *item)
  {
    used_tables_cache|= item->used_tables();
    const_item_cache&= item->const_item();
  }
  void used_tables_and_const_cache_update_and_join(Item *item)
  {
    item->update_used_tables();
    used_tables_and_const_cache_join(item);
  }
  /*
    Call update_used_tables() for all "argc" items in the array "argv"
    and join with the current cache.
    "this" must be initialized with a constructor or
    re-initialized with used_tables_and_const_cache_init().
  */
  void used_tables_and_const_cache_update_and_join(uint argc, Item **argv)
  {
    for (uint i=0 ; i < argc ; i++)
      used_tables_and_const_cache_update_and_join(argv[i]);
  }
  /*
    Call update_used_tables() for all items in the list
    and join with the current cache.
    "this" must be initialized with a constructor or
    re-initialized with used_tables_and_const_cache_init().
  */
  void used_tables_and_const_cache_update_and_join(List<Item> &list)
  {
    List_iterator_fast<Item> li(list);
    Item *item;
    while ((item=li++))
      used_tables_and_const_cache_update_and_join(item);
  }
};


/**
  An abstract class representing common features of
  regular functions and aggregate functions.
*/
class Item_func_or_sum: public Item_result_field,
                        public Item_args,
                        public Used_tables_and_const_cache
{
protected:
  bool agg_arg_charsets(DTCollation &c, Item **items, uint nitems,
                        uint flags, int item_sep)
  {
    return Type_std_attributes::agg_arg_charsets(c, func_name_cstring(),
                                                 items, nitems,
                                                 flags, item_sep);
  }
  bool agg_arg_charsets_for_string_result(DTCollation &c,
                                          Item **items, uint nitems,
                                          int item_sep= 1)
  {
    return Type_std_attributes::
      agg_arg_charsets_for_string_result(c, func_name_cstring(),
                                         items, nitems, item_sep);
  }
  bool agg_arg_charsets_for_string_result_with_comparison(DTCollation &c,
                                                          Item **items,
                                                          uint nitems,
                                                          int item_sep= 1)
  {
    return Type_std_attributes::
      agg_arg_charsets_for_string_result_with_comparison(c, func_name_cstring(),
                                                         items, nitems,
                                                         item_sep);
  }

  /*
    Aggregate arguments for comparison, e.g: a=b, a LIKE b, a RLIKE b
    - don't convert to @@character_set_connection if all arguments are numbers
    - don't allow DERIVATION_NONE
  */
  bool agg_arg_charsets_for_comparison(DTCollation &c,
                                       Item **items, uint nitems,
                                       int item_sep= 1)
  {
    return Type_std_attributes::
      agg_arg_charsets_for_comparison(c, func_name_cstring(), items, nitems, item_sep);
  }

public:
  // This method is used by Arg_comparator
  bool agg_arg_charsets_for_comparison(CHARSET_INFO **cs, Item **a, Item **b)
  {
    DTCollation tmp;
    if (tmp.set((*a)->collation, (*b)->collation, MY_COLL_CMP_CONV) ||
        tmp.derivation == DERIVATION_NONE)
    {
      my_error(ER_CANT_AGGREGATE_2COLLATIONS,MYF(0),
               (*a)->collation.collation->coll_name.str,
               (*a)->collation.derivation_name(),
               (*b)->collation.collation->coll_name.str,
               (*b)->collation.derivation_name(),
               func_name());
      return true;
    }
    if (agg_item_set_converter(tmp, func_name_cstring(),
                               a, 1, MY_COLL_CMP_CONV, 1) ||
        agg_item_set_converter(tmp, func_name_cstring(),
                               b, 1, MY_COLL_CMP_CONV, 1))
      return true;
    *cs= tmp.collation;
    return false;
  }

public:
  Item_func_or_sum(THD *thd): Item_result_field(thd), Item_args() {}
  Item_func_or_sum(THD *thd, Item *a): Item_result_field(thd), Item_args(a) { }
  Item_func_or_sum(THD *thd, Item *a, Item *b):
    Item_result_field(thd), Item_args(a, b) { }
  Item_func_or_sum(THD *thd, Item *a, Item *b, Item *c):
    Item_result_field(thd), Item_args(thd, a, b, c) { }
  Item_func_or_sum(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_result_field(thd), Item_args(thd, a, b, c, d) { }
  Item_func_or_sum(THD *thd, Item *a, Item *b, Item *c, Item *d, Item *e):
    Item_result_field(thd), Item_args(thd, a, b, c, d, e) { }
  Item_func_or_sum(THD *thd, Item_func_or_sum *item):
    Item_result_field(thd, item), Item_args(thd, item),
    Used_tables_and_const_cache(item) { }
  Item_func_or_sum(THD *thd, List<Item> &list):
    Item_result_field(thd), Item_args(thd, list) { }
  bool walk(Item_processor processor, bool walk_subquery, void *arg) override
  {
    if (walk_args(processor, walk_subquery, arg))
      return true;
    return (this->*processor)(arg);
  }
  /*
    This method is used for debug purposes to print the name of an
    item to the debug log. The second use of this method is as
    a helper function of print() and error messages, where it is
    applicable. To suit both goals it should return a meaningful,
    distinguishable and sintactically correct string. This method
    should not be used for runtime type identification, use enum
    {Sum}Functype and Item_func::functype()/Item_sum::sum_func()
    instead.
    Added here, to the parent class of both Item_func and Item_sum.

    NOTE: for Items inherited from Item_sum, func_name() and
    func_name_cstring() returns part of function name till first
    argument (including '(') to make difference in names for functions
    with 'distinct' clause and without 'distinct' and also to make
    printing of items inherited from Item_sum uniform.
  */
  inline const char *func_name() const
  { return (char*) func_name_cstring().str; }
  virtual LEX_CSTRING func_name_cstring() const= 0;
  virtual bool fix_length_and_dec()= 0;
  bool const_item() const override { return const_item_cache; }
  table_map used_tables() const override { return used_tables_cache; }
  Item* build_clone(THD *thd) override;
  Sql_mode_dependency value_depends_on_sql_mode() const override
  {
    return Item_args::value_depends_on_sql_mode_bit_or().soft_to_hard();
  }
};

class sp_head;
class sp_name;
struct st_sp_security_context;

class Item_sp
{
protected:
  // Can be NULL in some non-SELECT queries
  Name_resolution_context *context;
public:
  sp_name *m_name;
  sp_head *m_sp;
  TABLE *dummy_table;
  uchar result_buf[64];
  sp_rcontext *func_ctx;
  MEM_ROOT sp_mem_root;
  Query_arena *sp_query_arena;

  /*
     The result field of the stored function.
  */
  Field *sp_result_field;
  Item_sp(THD *thd, Name_resolution_context *context_arg, sp_name *name_arg);
  Item_sp(THD *thd, Item_sp *item);
  LEX_CSTRING func_name_cstring(THD *thd) const;
  void cleanup();
  bool sp_check_access(THD *thd);
  bool execute(THD *thd, bool *null_value, Item **args, uint arg_count);
  bool execute_impl(THD *thd, Item **args, uint arg_count);
  bool init_result_field(THD *thd, uint max_length, uint maybe_null,
                         bool *null_value, LEX_CSTRING *name);
  void process_error(THD *thd)
  {
    if (context)
      context->process_error(thd);
  }
};

class Item_ref :public Item_ident
{
protected:
  void set_properties();
  bool set_properties_only; // the item doesn't need full fix_fields
public:
  enum Ref_Type { REF, DIRECT_REF, VIEW_REF, OUTER_REF, AGGREGATE_REF };
  Item **ref;
  bool reference_trough_name;
  Item_ref(THD *thd, Name_resolution_context *context_arg,
           const LEX_CSTRING &db_arg, const LEX_CSTRING &table_name_arg,
           const LEX_CSTRING &field_name_arg):
    Item_ident(thd, context_arg, db_arg, table_name_arg, field_name_arg),
    set_properties_only(0), ref(0), reference_trough_name(1) {}
  Item_ref(THD *thd, Name_resolution_context *context_arg,
           const LEX_CSTRING &field_name_arg)
   :Item_ref(thd, context_arg, null_clex_str, null_clex_str, field_name_arg)
  { }
  /*
    This constructor is used in two scenarios:
    A) *item = NULL
      No initialization is performed, fix_fields() call will be necessary.
      
    B) *item points to an Item this Item_ref will refer to. This is 
      used for GROUP BY. fix_fields() will not be called in this case,
      so we call set_properties to make this item "fixed". set_properties
      performs a subset of action Item_ref::fix_fields does, and this subset
      is enough for Item_ref's used in GROUP BY.
    
    TODO we probably fix a superset of problems like in BUG#6658. Check this 
         with Bar, and if we have a more broader set of problems like this.
  */
  Item_ref(THD *thd, Name_resolution_context *context_arg, Item **item,
           const LEX_CSTRING &table_name_arg, const LEX_CSTRING &field_name_arg,
           bool alias_name_used_arg= FALSE);
  Item_ref(THD *thd, TABLE_LIST *view_arg, Item **item,
           const LEX_CSTRING &field_name_arg, bool alias_name_used_arg= FALSE);

  /* Constructor need to process subselect with temporary tables (see Item) */
  Item_ref(THD *thd, Item_ref *item)
    :Item_ident(thd, item), set_properties_only(0), ref(item->ref) {}
  enum Type type() const override	{ return REF_ITEM; }
  enum Type real_type() const override
  { return ref ? (*ref)->type() : REF_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const override
  {
    Item *it= ((Item *) item)->real_item();
    return ref && (*ref)->eq(it, binary_cmp);
  }
  void save_val(Field *to) override;
  void save_result(Field *to) override;
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override;
  String *val_str(String* tmp) override;
  bool val_native(THD *thd, Native *to) override;
  bool is_null() override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  longlong val_datetime_packed(THD *) override;
  longlong val_time_packed(THD *) override;
  double val_result() override;
  longlong val_int_result() override;
  String *str_result(String* tmp) override;
  bool val_native_result(THD *thd, Native *to) override;
  my_decimal *val_decimal_result(my_decimal *) override;
  bool val_bool_result() override;
  bool is_null_result() override;
  bool send(Protocol *prot, st_value *buffer) override;
  void make_send_field(THD *thd, Send_field *field) override;
  bool fix_fields(THD *, Item **) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;
  int save_in_field(Field *field, bool no_conversions) override;
  void save_org_in_field(Field *field, fast_field_copier optimizer_data)
    override;
  fast_field_copier setup_fast_field_copier(Field *field) override
  { return (*ref)->setup_fast_field_copier(field); }
  const Type_handler *type_handler() const override
  { return (*ref)->type_handler(); }
  const Type_handler *real_type_handler() const override
  { return (*ref)->real_type_handler(); }
  Field *get_tmp_table_field() override
  { return result_field ? result_field : (*ref)->get_tmp_table_field(); }
  Item *get_tmp_table_item(THD *thd) override;
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override;
  Item* propagate_equal_fields(THD *, const Context &, COND_EQUAL *) override;
  table_map used_tables() const override;
  void update_used_tables() override;
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override
  {
    /*
      normilize_cond() replaced all conditions of type
         WHERE/HAVING field
      to:
        WHERE/HAVING field<>0
      By the time of a build_equal_items() call, all such conditions should
      already be replaced. No Item_ref referencing to Item_field are possible.
    */
    DBUG_ASSERT(real_type() != FIELD_ITEM);
    return Item_ident::build_equal_items(thd, inherited, link_item_fields,
                                         cond_equal_ref);
  }
  bool const_item() const override
  {
    return (*ref)->const_item();
  }
  table_map not_null_tables() const override
  {
    return depended_from ? 0 : (*ref)->not_null_tables();
  }
  bool find_not_null_fields(table_map allowed) override
  {
    return depended_from ? false : (*ref)->find_not_null_fields(allowed);
  }
  void save_in_result_field(bool no_conversions) override
  {
    (*ref)->save_in_field(result_field, no_conversions);
  }
  Item *real_item() override
  {
    return ref ? (*ref)->real_item() : this;
  }
  const TYPELIB *get_typelib() const override
  {
    return ref ? (*ref)->get_typelib() : NULL;
  }
  bool is_json_type() override { return (*ref)->is_json_type(); }

  bool walk(Item_processor processor, bool walk_subquery, void *arg) override
  {
    if (ref && *ref)
      return (*ref)->walk(processor, walk_subquery, arg) ||
             (this->*processor)(arg); 
    else
      return FALSE;
  }
  Item* transform(THD *thd, Item_transformer, uchar *arg) override;
  Item* compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                Item_transformer transformer, uchar *arg_t) override;
  bool enumerate_field_refs_processor(void *arg) override
  { return (*ref)->enumerate_field_refs_processor(arg); }
  void no_rows_in_result() override
  {
    (*ref)->no_rows_in_result();
  }
  void restore_to_before_no_rows_in_result() override
  {
    (*ref)->restore_to_before_no_rows_in_result();
  }
  void print(String *str, enum_query_type query_type) override;
  enum precedence precedence() const override
  {
    return ref ? (*ref)->precedence() : DEFAULT_PRECEDENCE;
  }
  void cleanup() override;
  Item_field *field_for_view_update() override
  { return (*ref)->field_for_view_update(); }
  Load_data_outvar *get_load_data_outvar() override
  {
    return (*ref)->get_load_data_outvar();
  }
  virtual Ref_Type ref_type() { return REF; }

  // Row emulation: forwarding of ROW-related calls to ref
  uint cols() const override
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->cols() : 1;
  }
  Item* element_index(uint i) override
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->element_index(i) : this;
  }
  Item** addr(uint i) override
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->addr(i) : 0;
  }
  bool check_cols(uint c) override
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->check_cols(c) 
                                              : Item::check_cols(c);
  }
  bool null_inside() override
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->null_inside() : 0;
  }
  void bring_value() override
  {
    if (ref && result_type() == ROW_RESULT)
      (*ref)->bring_value();
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function("ref", arg, VCOL_IMPOSSIBLE);
  }
  bool basic_const_item() const override
  { return ref && (*ref)->basic_const_item(); }
  bool is_outer_field() const override
  {
    DBUG_ASSERT(fixed());
    DBUG_ASSERT(ref);
    return (*ref)->is_outer_field();
  }
  Item* build_clone(THD *thd) override;
  /**
    Checks if the item tree that ref points to contains a subquery.
  */
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_ref>(thd, this); }
  bool excl_dep_on_table(table_map tab_map) override
  {
    table_map used= used_tables();
    if (used & OUTER_REF_TABLE_BIT)
      return false;
    return (used == tab_map) || (*ref)->excl_dep_on_table(tab_map);
  }
  bool excl_dep_on_grouping_fields(st_select_lex *sel) override
  { return (*ref)->excl_dep_on_grouping_fields(sel); }
  bool excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred) override
  { return (*ref)->excl_dep_on_in_subq_left_part(subq_pred); }
  bool cleanup_excluding_fields_processor(void *arg) override
  {
    Item *item= real_item();
    if (item && item->type() == FIELD_ITEM &&
        ((Item_field *)item)->field)
      return 0;
    return cleanup_processor(arg);
  }
  bool cleanup_excluding_const_fields_processor(void *arg) override
  {
    Item *item= real_item();
    if (item && item->type() == FIELD_ITEM &&
        ((Item_field *) item)->field && item->const_item())
      return 0;
    return cleanup_processor(arg);
  }
  Item *field_transformer_for_having_pushdown(THD *thd, uchar *arg) override
  { return (*ref)->field_transformer_for_having_pushdown(thd, arg); }
  Item *remove_item_direct_ref() override
  {
    *ref= (*ref)->remove_item_direct_ref();
    return this;
  }
};


/*
  The same as Item_ref, but get value from val_* family of method to get
  value of item on which it referred instead of result* family.
*/
class Item_direct_ref :public Item_ref
{
public:
  Item_direct_ref(THD *thd, Name_resolution_context *context_arg, Item **item,
                  const LEX_CSTRING &table_name_arg,
                  const LEX_CSTRING &field_name_arg,
                  bool alias_name_used_arg= FALSE):
    Item_ref(thd, context_arg, item, table_name_arg,
             field_name_arg, alias_name_used_arg)
  {}
  /* Constructor need to process subselect with temporary tables (see Item) */
  Item_direct_ref(THD *thd, Item_direct_ref *item) : Item_ref(thd, item) {}
  Item_direct_ref(THD *thd, TABLE_LIST *view_arg, Item **item,
                  const LEX_CSTRING &field_name_arg,
                  bool alias_name_used_arg= FALSE):
    Item_ref(thd, view_arg, item, field_name_arg,
             alias_name_used_arg)
  {}

  bool fix_fields(THD *thd, Item **it) override
  {
    if ((*ref)->fix_fields_if_needed_for_scalar(thd, ref))
      return TRUE;
    return Item_ref::fix_fields(thd, it);
  }
  void save_val(Field *to) override;
  /* Below we should have all val() methods as in Item_ref */
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override;
  String *val_str(String* tmp) override;
  bool val_native(THD *thd, Native *to) override;
  bool is_null() override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  longlong val_datetime_packed(THD *) override;
  longlong val_time_packed(THD *) override;
  Ref_Type ref_type() override { return DIRECT_REF; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_direct_ref>(thd, this); }
  Item *remove_item_direct_ref() override
  { return (*ref)->remove_item_direct_ref(); }

  /* Should be called if ref is changed */
  inline void ref_changed()
  {
    set_properties();
  }
};


/**
  This class is the same as Item_direct_ref but created to wrap Item_ident
  before fix_fields() call
*/

class Item_direct_ref_to_ident :public Item_direct_ref
{
  Item_ident *ident;
public:
  Item_direct_ref_to_ident(THD *thd, Item_ident *item):
    Item_direct_ref(thd, item->context, (Item**)&item, item->table_name,
                    item->field_name, FALSE)
  {
    ident= item;
    ref= (Item**)&ident;
  }

  bool fix_fields(THD *thd, Item **it) override
  {
    DBUG_ASSERT(ident->type() == FIELD_ITEM || ident->type() == REF_ITEM);
    if (ident->fix_fields_if_needed_for_scalar(thd, ref))
      return TRUE;
    set_properties();
    return FALSE;
  }

  void print(String *str, enum_query_type query_type) override
  { ident->print(str, query_type); }
};


class Item_cache;
class Expression_cache;
class Expression_cache_tracker;

/**
  The objects of this class can store its values in an expression cache.
*/

class Item_cache_wrapper :public Item_result_field
{
private:
  /* Pointer on the cached expression */
  Item *orig_item;
  Expression_cache *expr_cache;
  /*
    In order to put the expression into the expression cache and return
    value of val_*() method, we will need to get the expression value twice
    (probably in different types).  In order to avoid making two
    (potentially costly) orig_item->val_*() calls, we store expression value
    in this Item_cache object.
  */
  Item_cache *expr_value;

  List<Item> parameters;

  Item *check_cache();
  void cache();
  void init_on_demand();

public:
  Item_cache_wrapper(THD *thd, Item *item_arg);
  ~Item_cache_wrapper();

  Type type() const override { return EXPR_CACHE_ITEM; }
  Type real_type() const override { return orig_item->type(); }
  bool set_cache(THD *thd);
  Expression_cache_tracker* init_tracker(MEM_ROOT *mem_root);
  bool fix_fields(THD *thd, Item **it) override;
  void cleanup() override;
  Item *get_orig_item() const { return orig_item; }

  /* Methods of getting value which should be cached in the cache */
  void save_val(Field *to) override;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String* tmp) override;
  bool val_native(THD *thd, Native *to) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override;
  bool is_null() override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool send(Protocol *protocol, st_value *buffer) override;
  void save_org_in_field(Field *field, fast_field_copier) override
  {
    save_val(field);
  }
  void save_in_result_field(bool) override { save_val(result_field); }
  Item* get_tmp_table_item(THD *thd_arg) override;

  /* Following methods make this item transparent as much as possible */

  void print(String *str, enum_query_type query_type) override;
  LEX_CSTRING full_name_cstring() const override
  { return orig_item->full_name_cstring(); }
  void make_send_field(THD *thd, Send_field *field) override
  { orig_item->make_send_field(thd, field); }
  bool eq(const Item *item, bool binary_cmp) const override
  {
    Item *it= const_cast<Item*>(item)->real_item();
    return orig_item->eq(it, binary_cmp);
  }
  void fix_after_pullout(st_select_lex *new_parent, Item **refptr, bool merge)
    override
  {
    orig_item->fix_after_pullout(new_parent, &orig_item, merge);
  }
  int save_in_field(Field *to, bool no_conversions) override;
  const Type_handler *type_handler() const override
  { return orig_item->type_handler(); }
  table_map used_tables() const override
  { return orig_item->used_tables(); }
  void update_used_tables() override
  {
    orig_item->update_used_tables();
  }
  bool const_item() const override { return orig_item->const_item(); }
  table_map not_null_tables() const override
  { return orig_item->not_null_tables(); }
  bool walk(Item_processor processor, bool walk_subquery, void *arg) override
  {
    return orig_item->walk(processor, walk_subquery, arg) ||
      (this->*processor)(arg);
  }
  bool enumerate_field_refs_processor(void *arg) override
  { return orig_item->enumerate_field_refs_processor(arg); }
  Item_field *field_for_view_update() override
  { return orig_item->field_for_view_update(); }

  /* Row emulation: forwarding of ROW-related calls to orig_item */
  uint cols() const override
  { return result_type() == ROW_RESULT ? orig_item->cols() : 1; }
  Item* element_index(uint i) override
  { return result_type() == ROW_RESULT ? orig_item->element_index(i) : this; }
  Item** addr(uint i) override
  { return result_type() == ROW_RESULT ? orig_item->addr(i) : 0; }
  bool check_cols(uint c) override
  {
    return (result_type() == ROW_RESULT ?
            orig_item->check_cols(c) :
            Item::check_cols(c));
  }
  bool null_inside() override
  { return result_type() == ROW_RESULT ? orig_item->null_inside() : 0; }
  void bring_value() override
  {
    if (result_type() == ROW_RESULT)
      orig_item->bring_value();
  }
  bool is_expensive() override { return orig_item->is_expensive(); }
  bool is_expensive_processor(void *arg) override
  { return orig_item->is_expensive_processor(arg); }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function("cache", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_wrapper>(thd, this); }
  Item *build_clone(THD *) override { return nullptr; }
};


/*
  Class for view fields, the same as Item_direct_ref, but call fix_fields
  of reference if it is not called yet
*/
class Item_direct_view_ref :public Item_direct_ref
{
  Item_equal *item_equal;
  TABLE_LIST *view;
  TABLE *null_ref_table;

#define NO_NULL_TABLE (reinterpret_cast<TABLE *>(0x1))

  void set_null_ref_table()
  {
    if (!view->is_inner_table_of_outer_join() ||
        !(null_ref_table= view->get_real_join_table()))
      null_ref_table= NO_NULL_TABLE;
  }

  bool check_null_ref()
  {
    DBUG_ASSERT(null_ref_table);
    if (null_ref_table != NO_NULL_TABLE && null_ref_table->null_row)
    {
      null_value= 1;
      return TRUE;
    }
    return FALSE;
  }

public:
  Item_direct_view_ref(THD *thd, Name_resolution_context *context_arg,
                       Item **item,
                       LEX_CSTRING &table_name_arg,
                       LEX_CSTRING &field_name_arg,
                       TABLE_LIST *view_arg):
    Item_direct_ref(thd, context_arg, item, table_name_arg, field_name_arg),
    item_equal(0), view(view_arg),
    null_ref_table(NULL)
  {
    if (fixed())
      set_null_ref_table();
  }

  bool fix_fields(THD *, Item **) override;
  bool eq(const Item *item, bool binary_cmp) const override;
  Item *get_tmp_table_item(THD *thd) override
  {
    if (const_item())
      return copy_or_same(thd);
    Item *item= Item_ref::get_tmp_table_item(thd);
    item->name= name;
    return item;
  }
  Ref_Type ref_type() override { return VIEW_REF; }
  Item_equal *get_item_equal() override { return item_equal; }
  void set_item_equal(Item_equal *item_eq) override { item_equal= item_eq; }
  Item_equal *find_item_equal(COND_EQUAL *cond_equal) override;
  Item* propagate_equal_fields(THD *, const Context &, COND_EQUAL *) override;
  Item *replace_equal_field(THD *thd, uchar *arg) override;
  table_map used_tables() const override;
  void update_used_tables() override;
  table_map not_null_tables() const override;
  bool const_item() const override
  {
    return (*ref)->const_item() && (null_ref_table == NO_NULL_TABLE);
  }
  TABLE *get_null_ref_table() const { return null_ref_table; }
  bool walk(Item_processor processor, bool walk_subquery, void *arg) override
  {
    return (*ref)->walk(processor, walk_subquery, arg) ||
           (this->*processor)(arg);
  }
  bool view_used_tables_processor(void *arg) override
  {
    TABLE_LIST *view_arg= (TABLE_LIST *) arg;
    if (view_arg == view)
      view_arg->view_used_tables|= (*ref)->used_tables();
    return 0;
  }
  bool excl_dep_on_table(table_map tab_map) override;
  bool excl_dep_on_grouping_fields(st_select_lex *sel) override;
  bool excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred) override;
  Item *derived_field_transformer_for_having(THD *thd, uchar *arg) override;
  Item *derived_field_transformer_for_where(THD *thd, uchar *arg) override;
  Item *grouping_field_transformer_for_where(THD *thd, uchar *arg) override;
  Item *in_subq_field_transformer_for_where(THD *thd, uchar *arg) override;
  Item *in_subq_field_transformer_for_having(THD *thd, uchar *arg) override;

  void save_val(Field *to) override
  {
    if (check_null_ref())
      to->set_null();
    else
      Item_direct_ref::save_val(to);
  }
  double val_real() override
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_real();
  }
  longlong val_int() override
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_int();
  }
  String *val_str(String* tmp) override
  {
    if (check_null_ref())
      return NULL;
    else
      return Item_direct_ref::val_str(tmp);
  }
  bool val_native(THD *thd, Native *to) override
  {
    if (check_null_ref())
      return true;
    return Item_direct_ref::val_native(thd, to);
  }
  my_decimal *val_decimal(my_decimal *tmp) override
  {
    if (check_null_ref())
      return NULL;
    else
      return Item_direct_ref::val_decimal(tmp);
  }
  bool val_bool() override
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_bool();
  }
  bool is_null() override
  {
    if (check_null_ref())
      return 1;
    else
      return Item_direct_ref::is_null();
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    if (check_null_ref())
    {
      bzero((char*) ltime,sizeof(*ltime));
      return 1;
    }
    return Item_direct_ref::get_date(thd, ltime, fuzzydate);
  }
  longlong val_time_packed(THD *thd) override
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_time_packed(thd);
  }
  longlong val_datetime_packed(THD *thd) override
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_datetime_packed(thd);
  }
  bool send(Protocol *protocol, st_value *buffer) override;
  void save_org_in_field(Field *field, fast_field_copier) override
  {
    if (check_null_ref())
      field->set_null();
    else
      Item_direct_ref::save_val(field);
  }
  void save_in_result_field(bool no_conversions) override
  {
    if (check_null_ref())
      result_field->set_null();
    else
      Item_direct_ref::save_in_result_field(no_conversions);
  }

  void cleanup() override
  {
    null_ref_table= NULL;
    item_equal= NULL;
    Item_direct_ref::cleanup();
  }
  /*
    TODO move these val_*_result function to Item_direct_ref (maybe)
  */
  double val_result() override;
  longlong val_int_result() override;
  String *str_result(String* tmp) override;
  my_decimal *val_decimal_result(my_decimal *val) override;
  bool val_bool_result() override;

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_direct_view_ref>(thd, this); }
  Item *field_transformer_for_having_pushdown(THD *, uchar *) override
  { return this; }
  Item *remove_item_direct_ref() override { return this; }
};


/*
  Class for outer fields.
  An object of this class is created when the select where the outer field was
  resolved is a grouping one. After it has been fixed the ref field will point
  to either an Item_ref or an Item_direct_ref object which will be used to
  access the field.
  See also comments for the fix_inner_refs() and the
  Item_field::fix_outer_field() functions.
*/

class Item_sum;
class Item_outer_ref :public Item_direct_ref
{
public:
  Item *outer_ref;
  /* The aggregate function under which this outer ref is used, if any. */
  Item_sum *in_sum_func;
  /*
    TRUE <=> that the outer_ref is already present in the select list
    of the outer select.
  */
  bool found_in_select_list;
  bool found_in_group_by;
  Item_outer_ref(THD *thd, Name_resolution_context *context_arg,
                 Item_field *outer_field_arg):
    Item_direct_ref(thd, context_arg, 0, outer_field_arg->table_name,
                    outer_field_arg->field_name),
    outer_ref(outer_field_arg), in_sum_func(0),
    found_in_select_list(0), found_in_group_by(0)
  {
    ref= &outer_ref;
    set_properties();
    /* reset flag set in set_properties() */
    base_flags&= ~item_base_t::FIXED;
  }
  Item_outer_ref(THD *thd, Name_resolution_context *context_arg, Item **item,
                 const LEX_CSTRING &table_name_arg, LEX_CSTRING &field_name_arg,
                 bool alias_name_used_arg):
    Item_direct_ref(thd, context_arg, item, table_name_arg, field_name_arg,
                    alias_name_used_arg),
    outer_ref(0), in_sum_func(0), found_in_select_list(1), found_in_group_by(0)
  {}
  void save_in_result_field(bool no_conversions) override
  {
    outer_ref->save_org_in_field(result_field, NULL);
  }
  bool fix_fields(THD *, Item **) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;
  table_map used_tables() const override
  {
    return (*ref)->const_item() ? 0 : OUTER_REF_TABLE_BIT;
  }
  table_map not_null_tables() const override { return 0; }
  Ref_Type ref_type() override { return OUTER_REF; }
  bool check_inner_refs_processor(void * arg) override;
};


class Item_in_subselect;


/*
  An object of this class:
   - Converts val_XXX() calls to ref->val_XXX_result() calls, like Item_ref.
   - Sets owner->was_null=TRUE if it has returned a NULL value from any
     val_XXX() function. This allows to inject an Item_ref_null_helper
     object into subquery and then check if the subquery has produced a row
     with NULL value.
*/

class Item_ref_null_helper: public Item_ref
{
protected:
  Item_in_subselect* owner;
public:
  Item_ref_null_helper(THD *thd, Name_resolution_context *context_arg,
                       Item_in_subselect* master, Item **item,
		       const LEX_CSTRING &table_name_arg,
                       const LEX_CSTRING &field_name_arg):
    Item_ref(thd, context_arg, item, table_name_arg, field_name_arg),
    owner(master) {}
  void save_val(Field *to) override;
  double val_real() override;
  longlong val_int() override;
  String* val_str(String* s) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool val_native(THD *thd, Native *to) override;
  void print(String *str, enum_query_type query_type) override;
  table_map used_tables() const override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_ref_null_helper>(thd, this); }
};

/*
  The following class is used to optimize comparing of date and bigint columns
  We need to save the original item ('ref') to be able to call
  ref->save_in_field(). This is used to create index search keys.
  
  An instance of Item_int_with_ref may have signed or unsigned integer value.
  
*/

class Item_int_with_ref :public Item_int
{
  Item *ref;
public:
  Item_int_with_ref(THD *thd, longlong i, Item *ref_arg, bool unsigned_arg):
    Item_int(thd, i), ref(ref_arg)
  {
    unsigned_flag= unsigned_arg;
  }
  int save_in_field(Field *field, bool no_conversions) override
  {
    return ref->save_in_field(field, no_conversions);
  }
  Item *clone_item(THD *thd) override;
  Item *real_item() override { return ref; }
};

#ifdef MYSQL_SERVER
#include "item_sum.h"
#include "item_func.h"
#include "item_row.h"
#include "item_cmpfunc.h"
#include "item_strfunc.h"
#include "item_timefunc.h"
#include "item_subselect.h"
#include "item_xmlfunc.h"
#include "item_jsonfunc.h"
#include "item_create.h"
#include "item_vers.h"
#endif

/**
  Base class to implement typed value caching Item classes

  Item_copy_ classes are very similar to the corresponding Item_
  classes (e.g. Item_copy_string is similar to Item_string) but they add
  the following additional functionality to Item_ :
    1. Nullability
    2. Possibility to store the value not only on instantiation time,
       but also later.
  Item_copy_ classes are a functionality subset of Item_cache_ 
  classes, as e.g. they don't support comparisons with the original Item
  as Item_cache_ classes do.
  Item_copy_ classes are used in GROUP BY calculation.
  TODO: Item_copy should be made an abstract interface and Item_copy_
  classes should inherit both the respective Item_ class and the interface.
  Ideally we should drop Item_copy_ classes altogether and merge 
  their functionality to Item_cache_ (and these should be made to inherit
  from Item_).
*/

class Item_copy :public Item,
                 public Type_handler_hybrid_field_type
{
protected:  

  /**
    Type_handler_hybrid_field_type is used to
    store the type of the resulting field that would be used to store the data
    in the cache. This is to avoid calls to the original item.
  */

  /** The original item that is copied */
  Item *item;

  /**
    Constructor of the Item_copy class

    stores metadata information about the original class as well as a 
    pointer to it.
  */
  Item_copy(THD *thd, Item *org): Item(thd)
  {
    DBUG_ASSERT(org->fixed());
    item= org;
    null_value= item->maybe_null();
    copy_flags(item, item_base_t::MAYBE_NULL);
    Type_std_attributes::set(item);
    name= item->name;
    set_handler(item->type_handler());
  }

public:

  /** 
    Update the cache with the value of the original item
   
    This is the method that updates the cached value.
    It must be explicitly called by the user of this class to store the value 
    of the original item in the cache.
  */
  virtual void copy() = 0;

  Item *get_item() { return item; }
  /** All of the subclasses should have the same type tag */
  Type type() const override { return COPY_STR_ITEM; }

  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }

  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  void make_send_field(THD *thd, Send_field *field) override
  { item->make_send_field(thd, field); }
  table_map used_tables() const override { return (table_map) 1L; }
  bool const_item() const override { return false; }
  bool is_null() override { return null_value; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function("copy", arg, VCOL_IMPOSSIBLE);
  }

  /*
    Override the methods below as pure virtual to make sure all the
    sub-classes implement them.
  */
  String *val_str(String*) override = 0;
  my_decimal *val_decimal(my_decimal *) override = 0;
  double val_real() override = 0;
  longlong val_int() override = 0;
  int save_in_field(Field *field, bool no_conversions) override = 0;
  bool walk(Item_processor processor, bool walk_subquery, void *args) override
  {
    return (item->walk(processor, walk_subquery, args)) ||
      (this->*processor)(args);
  }
};

/**
 Implementation of a string cache.
 
 Uses Item::str_value for storage
*/ 
class Item_copy_string : public Item_copy
{
public:
  Item_copy_string(THD *thd, Item *item_arg): Item_copy(thd, item_arg) {}

  String *val_str(String*) override;
  my_decimal *val_decimal(my_decimal *) override;
  double val_real() override;
  longlong val_int() override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return get_date_from_string(thd, ltime, fuzzydate); }
  void copy() override;
  int save_in_field(Field *field, bool no_conversions) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_copy_string>(thd, this); }
};


/**
  We need a separate class Item_copy_timestamp because
  TIMESTAMP->string->TIMESTAMP conversion is not round trip safe
  near the DST change, e.g. '2010-10-31 02:25:26' can mean:
   - my_time_t(1288477526) - summer time in Moscow
   - my_time_t(1288481126) - winter time in Moscow, one hour later
*/
class Item_copy_timestamp: public Item_copy
{
  Timestamp_or_zero_datetime m_value;
  bool sane() const { return !null_value || m_value.is_zero_datetime(); }
public:
  Item_copy_timestamp(THD *thd, Item *arg): Item_copy(thd, arg) { }
  const Type_handler *type_handler() const override
  { return &type_handler_timestamp2; }
  void copy() override
  {
    Timestamp_or_zero_datetime_native_null tmp(current_thd, item, false);
    null_value= tmp.is_null();
    m_value= tmp.is_null() ? Timestamp_or_zero_datetime() :
                             Timestamp_or_zero_datetime(tmp);
  }
  int save_in_field(Field *field, bool) override
  {
    DBUG_ASSERT(sane());
    if (null_value)
      return set_field_to_null(field);
    Timestamp_or_zero_datetime_native native(m_value, decimals);
    return native.save_in_field(field, decimals);
  }
  longlong val_int() override
  {
    DBUG_ASSERT(sane());
    return null_value ? 0 :
           m_value.to_datetime(current_thd).to_longlong();
  }
  double val_real() override
  {
    DBUG_ASSERT(sane());
    return null_value ? 0e0 :
           m_value.to_datetime(current_thd).to_double();
  }
  String *val_str(String *to) override
  {
    DBUG_ASSERT(sane());
    return null_value ? NULL :
           m_value.to_datetime(current_thd).to_string(to, decimals);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    DBUG_ASSERT(sane());
    return null_value ? NULL :
           m_value.to_datetime(current_thd).to_decimal(to);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    DBUG_ASSERT(sane());
    bool res= m_value.to_TIME(thd, ltime, fuzzydate);
    DBUG_ASSERT(!res);
    return null_value || res;
  }
  bool val_native(THD *thd, Native *to) override
  {
    DBUG_ASSERT(sane());
    return null_value || m_value.to_native(to, decimals);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_copy_timestamp>(thd, this); }
};


/*
  Cached_item_XXX objects are not exactly caches. They do the following:

  Each Cached_item_XXX object has
   - its source item
   - saved value of the source item
   - cmp() method that compares the saved value with the current value of the
     source item, and if they were not equal saves item's value into the saved
     value.

  TODO: add here:
   - a way to save the new value w/o comparison
   - a way to do less/equal/greater comparison
*/

class Cached_item :public Sql_alloc
{
public:
  bool null_value;
  Cached_item() :null_value(0) {}
  /*
    Compare the cached value with the source value. If not equal, copy
    the source value to the cache.
    @return
      true  - Not equal
      false - Equal
  */
  virtual bool cmp(void)=0;

  /* Compare the cached value with the source value, without copying */
  virtual int  cmp_read_only()=0;

  virtual ~Cached_item(); /*line -e1509 */
};

class Cached_item_item : public Cached_item
{
protected:
  Item *item;

  Cached_item_item(Item *arg) : item(arg) {}
public:
  void fetch_value_from(Item *new_item)
  {
    Item *save= item;
    item= new_item;
    cmp();
    item= save;
  }
};

class Cached_item_str :public Cached_item_item
{
  uint32 value_max_length;
  String value,tmp_value;
public:
  Cached_item_str(THD *thd, Item *arg);
  bool cmp() override;
  int cmp_read_only() override;
  ~Cached_item_str();                           // Deallocate String:s
};


class Cached_item_real :public Cached_item_item
{
  double value;
public:
  Cached_item_real(Item *item_par) :Cached_item_item(item_par),value(0.0) {}
  bool cmp() override;
  int cmp_read_only() override;
};

class Cached_item_int :public Cached_item_item
{
  longlong value;
public:
  Cached_item_int(Item *item_par) :Cached_item_item(item_par),value(0) {}
  bool cmp() override;
  int cmp_read_only() override;
};


class Cached_item_decimal :public Cached_item_item
{
  my_decimal value;
public:
  Cached_item_decimal(Item *item_par);
  bool cmp() override;
  int cmp_read_only() override;
};

class Cached_item_field :public Cached_item
{
  uchar *buff;
  Field *field;
  uint length;

public:
  Cached_item_field(THD *thd, Field *arg_field): field(arg_field)
  {
    field= arg_field;
    /* TODO: take the memory allocation below out of the constructor. */
    buff= (uchar*) thd_calloc(thd, length= field->pack_length());
  }
  bool cmp() override;
  int cmp_read_only() override;
};

class Item_default_value : public Item_field
{
  bool vcol_assignment_ok;
  void calculate();
public:
  Item *arg= nullptr;
  Field *cached_field= nullptr;
  Item_default_value(THD *thd, Name_resolution_context *context_arg, Item *a,
                     bool vcol_assignment_arg)
    : Item_field(thd, context_arg),
      vcol_assignment_ok(vcol_assignment_arg), arg(a) {}
  Type type() const override { return DEFAULT_VALUE_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const override;
  bool fix_fields(THD *, Item **) override;
  void cleanup() override;
  void print(String *str, enum_query_type query_type) override;
  String *val_str(String *str) override;
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *decimal_value) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime,date_mode_t fuzzydate) override;
  bool val_native(THD *thd, Native *to) override;
  bool val_native_result(THD *thd, Native *to) override;

  /* Result variants */
  double val_result() override;
  longlong val_int_result() override;
  String *str_result(String* tmp) override;
  my_decimal *val_decimal_result(my_decimal *val) override;
  bool val_bool_result() override;
  bool is_null_result() override;
  bool get_date_result(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
    override;

  bool send(Protocol *protocol, st_value *buffer) override;
  int save_in_field(Field *field_arg, bool no_conversions) override;
  bool save_in_param(THD *, Item_param *param) override
  {
    // It should not be possible to have "EXECUTE .. USING DEFAULT(a)"
    DBUG_ASSERT(0);
    param->set_default();
    return false;
  }
  table_map used_tables() const override;
  void update_used_tables() override
  {
    if (field && field->default_value)
      field->default_value->expr->update_used_tables();
  }
  bool vcol_assignment_allowed_value() const override
  { return vcol_assignment_ok; }
  Field *get_tmp_table_field() override { return nullptr; }
  Item *get_tmp_table_item(THD *) override { return this; }
  Item_field *field_for_view_update() override { return nullptr; }
  bool update_vcol_processor(void *) override { return false; }
  bool check_func_default_processor(void *) override { return true; }
  bool walk(Item_processor processor, bool walk_subquery, void *args) override
  {
    return (arg && arg->walk(processor, walk_subquery, args)) ||
      (this->*processor)(args);
  }
  Item *transform(THD *thd, Item_transformer transformer, uchar *args)
    override;
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override;
};


class Item_contextually_typed_value_specification: public Item
{
public:
  Item_contextually_typed_value_specification(THD *thd) :Item(thd)
  { }
  Type type() const override { return CONTEXTUALLY_TYPED_VALUE_ITEM; }
  bool vcol_assignment_allowed_value() const override { return true; }
  bool eq(const Item *item, bool binary_cmp) const override { return false; }
  bool is_evaluable_expression() const override { return false; }
  Field *create_tmp_field_ex(MEM_ROOT *,
                             TABLE *, Tmp_field_src *,
                             const Tmp_field_param *) override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  String *val_str(String *str) override
  {
    DBUG_ASSERT(0); // never should be called
    null_value= true;
    return 0;
  }
  double val_real() override
  {
    DBUG_ASSERT(0); // never should be called
    null_value= true;
    return 0.0;
  }
  longlong val_int() override
  {
    DBUG_ASSERT(0); // never should be called
    null_value= true;
    return 0;
  }
  my_decimal *val_decimal(my_decimal *) override
  {
    DBUG_ASSERT(0); // never should be called
    null_value= true;
    return 0;
  }
  bool get_date(THD *, MYSQL_TIME *, date_mode_t) override
  {
    DBUG_ASSERT(0); // never should be called
    return (null_value= true);
  }
  bool send(Protocol *, st_value *) override
  {
    DBUG_ASSERT(0);
    return true;
  }
  const Type_handler *type_handler() const override
  {
    DBUG_ASSERT(0);
    return &type_handler_null;
  }
};


/*
  <default specification> ::= DEFAULT
*/
class Item_default_specification:
        public Item_contextually_typed_value_specification
{
public:
  Item_default_specification(THD *thd)
   :Item_contextually_typed_value_specification(thd)
  { }
  void print(String *str, enum_query_type) override
  {
    str->append(STRING_WITH_LEN("default"));
  }
  int save_in_field(Field *field_arg, bool) override
  {
    return field_arg->save_in_field_default_value(false);
  }
  bool save_in_param(THD *, Item_param *param) override
  {
    param->set_default();
    return false;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_default_specification>(thd, this); }
};


/**
  This class is used as bulk parameter INGNORE representation.

  It just do nothing when assigned to a field

  This is a non-standard MariaDB extension.
*/

class Item_ignore_specification:
        public Item_contextually_typed_value_specification
{
public:
  Item_ignore_specification(THD *thd)
   :Item_contextually_typed_value_specification(thd)
  { }
  void print(String *str, enum_query_type) override
  {
    str->append(STRING_WITH_LEN("ignore"));
  }
  int save_in_field(Field *field_arg, bool) override
  {
    return field_arg->save_in_field_ignore_value(false);
  }
  bool save_in_param(THD *, Item_param *param) override
  {
    param->set_ignore();
    return false;
  }

  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_ignore_specification>(thd, this); }
};


/*
  Item_insert_value -- an implementation of VALUES() function.
  You can use the VALUES(col_name) function in the UPDATE clause
  to refer to column values from the INSERT portion of the INSERT
  ... UPDATE statement. In other words, VALUES(col_name) in the
  UPDATE clause refers to the value of col_name that would be
  inserted, had no duplicate-key conflict occurred.
  In all other places this function returns NULL.
*/

class Item_insert_value : public Item_field
{
public:
  Item *arg;
  Item_insert_value(THD *thd, Name_resolution_context *context_arg, Item *a)
    :Item_field(thd, context_arg),
     arg(a) {}
  bool eq(const Item *item, bool binary_cmp) const override;
  bool fix_fields(THD *, Item **) override;
  void print(String *str, enum_query_type query_type) override;
  int save_in_field(Field *field_arg, bool no_conversions) override
  {
    return Item_field::save_in_field(field_arg, no_conversions);
  }
  Type type() const override { return INSERT_VALUE_ITEM; }
  /*
   We use RAND_TABLE_BIT to prevent Item_insert_value from
   being treated as a constant and precalculated before execution
  */
  table_map used_tables() const override { return RAND_TABLE_BIT; }

  Item_field *field_for_view_update() override { return nullptr; }

  bool walk(Item_processor processor, bool walk_subquery, void *args) override
  {
    return arg->walk(processor, walk_subquery, args) ||
	    (this->*processor)(args);
  }
  bool check_partition_func_processor(void *) override { return true; }
  bool update_vcol_processor(void *) override { return false; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function("value()", arg, VCOL_IMPOSSIBLE);
  }
};


class Table_triggers_list;

/*
  Represents NEW/OLD version of field of row which is
  changed/read in trigger.

  Note: For this item main part of actual binding to Field object happens
        not during fix_fields() call (like for Item_field) but right after
        parsing of trigger definition, when table is opened, with special
        setup_field() call. On fix_fields() stage we simply choose one of
        two Field instances representing either OLD or NEW version of this
        field.
*/
class Item_trigger_field : public Item_field,
                           private Settable_routine_parameter
{
private:
  GRANT_INFO *table_grants;
public:
  /* Next in list of all Item_trigger_field's in trigger */
  Item_trigger_field *next_trg_field;
  /* Pointer to Table_trigger_list object for table of this trigger */
  Table_triggers_list *triggers;
  /* Is this item represents row from NEW or OLD row ? */
  enum __attribute__((packed)) row_version_type {OLD_ROW, NEW_ROW};
  row_version_type row_version;
  /* Index of the field in the TABLE::field array */
  field_index_t field_idx;

private:
  /*
    Trigger field is read-only unless it belongs to the NEW row in a
    BEFORE INSERT of BEFORE UPDATE trigger.
  */
  bool read_only;

  /*
    'want_privilege' holds privileges required to perform operation on
    this trigger field (SELECT_ACL if we are going to read it and
    UPDATE_ACL if we are going to update it).  It is initialized at
    parse time but can be updated later if this trigger field is used
    as OUT or INOUT parameter of stored routine (in this case
    set_required_privilege() is called to appropriately update
    want_privilege and cleanup() is responsible for restoring of
    original want_privilege once parameter's value is updated).
  */
  privilege_t original_privilege;
  privilege_t want_privilege;
public:

Item_trigger_field(THD *thd, Name_resolution_context *context_arg,
                     row_version_type row_ver_arg,
                     const LEX_CSTRING &field_name_arg,
                     privilege_t priv, const bool ro)
    :Item_field(thd, context_arg, field_name_arg),
    table_grants(NULL),  next_trg_field(NULL),  triggers(NULL),
    row_version(row_ver_arg), field_idx(NO_CACHED_FIELD_INDEX),
    read_only (ro),  original_privilege(priv), want_privilege(priv)
  {
  }
  void setup_field(THD *thd, TABLE *table, GRANT_INFO *table_grant_info);
  Type type() const override { return TRIGGER_FIELD_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const override;
  bool fix_fields(THD *, Item **) override;
  void print(String *str, enum_query_type query_type) override;
  table_map used_tables() const override { return (table_map)0L; }
  Field *get_tmp_table_field() override { return nullptr; }
  Item *copy_or_same(THD *) override { return this; }
  Item *get_tmp_table_item(THD *thd) override { return copy_or_same(thd); }
  void cleanup() override;

private:
  void set_required_privilege(bool rw) override;
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;

public:
  Settable_routine_parameter *get_settable_routine_parameter() override
  {
    return read_only ? nullptr : this;
  }

  bool set_value(THD *thd, Item **it)
  {
    return set_value(thd, NULL, it);
  }

public:
  bool unknown_splocal_processor(void *) override { return false; }
  bool check_vcol_func_processor(void *arg) override;
};


/**
  @todo
  Implement the is_null() method for this class. Currently calling is_null()
  on any Item_cache object resolves to Item::is_null(), which returns FALSE
  for any value.
*/

class Item_cache: public Item,
                  public Type_handler_hybrid_field_type
{
protected:
  Item *example;
  /**
    Field that this object will get value from. This is used by 
    index-based subquery engines to detect and remove the equality injected 
    by IN->EXISTS transformation.
  */  
  Field *cached_field;
  /*
    TRUE <=> cache holds value of the last stored item (i.e actual value).
    store() stores item to be cached and sets this flag to FALSE.
    On the first call of val_xxx function if this flag is set to FALSE the 
    cache_value() will be called to actually cache value of saved item.
    cache_value() will set this flag to TRUE.
  */
  bool value_cached;

  table_map used_table_map;
public:
  /*
    This is set if at least one of the values of a sub query is NULL
    Item_cache_row returns this with null_inside().
    For not row items, it's set to the value of null_value
    It is set after cache_value() is called.
  */
  bool null_value_inside;

  Item_cache(THD *thd):
    Item(thd),
    Type_handler_hybrid_field_type(&type_handler_string),
    example(0), cached_field(0),
    value_cached(0),
    used_table_map(0)
  {
    set_maybe_null();
    null_value= 1;
    null_value_inside= true;
  }
protected:
  Item_cache(THD *thd, const Type_handler *handler):
    Item(thd),
    Type_handler_hybrid_field_type(handler),
    example(0), cached_field(0),
    value_cached(0),
    used_table_map(0)
  {
    set_maybe_null();
    null_value= 1;
    null_value_inside= true;
  }

public:
  virtual bool allocate(THD *thd, uint i) { return 0; }
  virtual bool setup(THD *thd, Item *item)
  {
    example= item;
    Type_std_attributes::set(item);
    if (item->type() == FIELD_ITEM)
      cached_field= ((Item_field *)item)->field;
    return 0;
  };

  void set_used_tables(table_map map) { used_table_map= map; }
  table_map used_tables() const override { return used_table_map; }
  Type type() const override { return CACHE_ITEM; }

  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    return create_tmp_field_ex_simple(root, table, src, param);
  }

  virtual void keep_array() {}
  void print(String *str, enum_query_type query_type) override;
  bool eq_def(const Field *field) 
  {
    return cached_field ? cached_field->eq_def (field) : FALSE;
  }
  bool eq(const Item *item, bool binary_cmp) const override
  {
    return this == item;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    if (example)
    {
      Item::vcol_func_processor_result *res=
        (Item::vcol_func_processor_result*) arg;
      example->check_vcol_func_processor(arg);
      /*
        Item_cache of a non-deterministic function requires re-fixing
        even if the function itself doesn't (e.g. CURRENT_TIMESTAMP)
      */
      if (res->errors & VCOL_NOT_STRICTLY_DETERMINISTIC)
        res->errors|= VCOL_SESSION_FUNC;
      return false;
    }
    return mark_unsupported_function("cache", arg, VCOL_IMPOSSIBLE);
  }
  void cleanup() override
  {
    clear();
    Item::cleanup();
  }
  /**
     Check if saved item has a non-NULL value.
     Will cache value of saved item if not already done. 
     @return TRUE if cached value is non-NULL.
   */
  bool has_value()
  {
    return (value_cached || cache_value()) && !null_value;
  }

  virtual void store(Item *item);
  virtual Item *get_item() { return example; }
  virtual bool cache_value()= 0;
  bool basic_const_item() const override
  { return example && example->basic_const_item(); }
  virtual void clear() { null_value= TRUE; value_cached= FALSE; }
  bool is_null() override { return !has_value(); }
  bool is_expensive() override
  {
    if (value_cached)
      return false;
    return example->is_expensive();
  }
  bool is_expensive_processor(void *arg) override
  {
    DBUG_ASSERT(example);
    if (value_cached)
      return false;
    return example->is_expensive_processor(arg);
  }
  virtual void set_null();
  bool walk(Item_processor processor, bool walk_subquery, void *arg) override
  {
    if (arg == STOP_PTR)
      return FALSE;
    if (example && example->walk(processor, walk_subquery, arg))
      return TRUE;
    return (this->*processor)(arg);
  }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override;
  void split_sum_func2_example(THD *thd,  Ref_ptr_array ref_pointer_array,
                               List<Item> &fields, uint flags)
  {
    example->split_sum_func2(thd, ref_pointer_array, fields, &example, flags);
  }
  Item *get_example() const { return example; }

  virtual Item *convert_to_basic_const_item(THD *thd) { return 0; };
  Item *derived_field_transformer_for_having(THD *thd, uchar *) override
  { return convert_to_basic_const_item(thd); }
  Item *derived_field_transformer_for_where(THD *thd, uchar *) override
  { return convert_to_basic_const_item(thd); }
  Item *grouping_field_transformer_for_where(THD *thd, uchar *) override
  { return convert_to_basic_const_item(thd); }
  Item *in_subq_field_transformer_for_where(THD *thd, uchar *) override
  { return convert_to_basic_const_item(thd); }
  Item *in_subq_field_transformer_for_having(THD *thd, uchar *) override
  { return convert_to_basic_const_item(thd); }
};


class Item_cache_int: public Item_cache
{
protected:
  longlong value;
public:
  Item_cache_int(THD *thd, const Type_handler *handler):
    Item_cache(thd, handler), value(0) {}

  double val_real() override;
  longlong val_int() override;
  String* val_str(String *str) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return get_date_from_int(thd, ltime, fuzzydate); }
  bool cache_value() override;
  int save_in_field(Field *field, bool no_conversions) override;
  Item *convert_to_basic_const_item(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_int>(thd, this); }
};


class Item_cache_year: public Item_cache_int
{
public:
  Item_cache_year(THD *thd, const Type_handler *handler)
   :Item_cache_int(thd, handler) { }
  bool get_date(THD *thd, MYSQL_TIME *to, date_mode_t mode)
  {
    return type_handler_year.Item_get_date_with_warn(thd, this, to, mode);
  }
};


class Item_cache_temporal: public Item_cache_int
{
protected:
  Item_cache_temporal(THD *thd, const Type_handler *handler);
public:
  bool cache_value() override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  int save_in_field(Field *field, bool no_conversions) override;
  bool setup(THD *thd, Item *item) override
  {
    if (Item_cache_int::setup(thd, item))
      return true;
    set_if_smaller(decimals, TIME_SECOND_PART_DIGITS);
    return false;
  }
  void store_packed(longlong val_arg, Item *example);
  /*
    Having a clone_item method tells optimizer that this object
    is a constant and need not be optimized further.
    Important when storing packed datetime values.
  */
  Item *clone_item(THD *thd) override;
  Item *convert_to_basic_const_item(THD *thd) override;
  virtual Item *make_literal(THD *) =0;
};


class Item_cache_time: public Item_cache_temporal
{
public:
  Item_cache_time(THD *thd)
   :Item_cache_temporal(thd, &type_handler_time2) { }
  bool cache_value() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_time>(thd, this); }
  Item *make_literal(THD *) override;
  longlong val_datetime_packed(THD *thd) override
  {
    Datetime::Options_cmp opt(thd);
    return has_value() ? Datetime(thd, this, opt).to_packed() : 0;
  }
  longlong val_time_packed(THD *) override
  {
    return has_value() ? value : 0;
  }
  longlong val_int() override
  {
    return has_value() ? Time(this).to_longlong() : 0;
  }
  double val_real() override
  {
    return has_value() ? Time(this).to_double() : 0;
  }
  String *val_str(String *to) override
  {
    return has_value() ? Time(this).to_string(to, decimals) : NULL;
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return has_value() ? Time(this).to_decimal(to) : NULL;
  }
  bool val_native(THD *thd, Native *to) override
  {
    return has_value() ? Time(thd, this).to_native(to, decimals) : true;
  }
};


class Item_cache_datetime: public Item_cache_temporal
{
public:
  Item_cache_datetime(THD *thd)
   :Item_cache_temporal(thd, &type_handler_datetime2) { }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_datetime>(thd, this); }
  Item *make_literal(THD *) override;
  longlong val_datetime_packed(THD *) override
  {
    return has_value() ? value : 0;
  }
  longlong val_time_packed(THD *thd) override
  {
    return Time(thd, this, Time::Options_cmp(thd)).to_packed();
  }
  longlong val_int() override
  {
    return has_value() ? Datetime(this).to_longlong() : 0;
  }
  double val_real() override
  {
    return has_value() ? Datetime(this).to_double() : 0;
  }
  String *val_str(String *to) override
  {
    return has_value() ? Datetime(this).to_string(to, decimals) : NULL;
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return has_value() ? Datetime(this).to_decimal(to) : NULL;
  }
};


class Item_cache_date: public Item_cache_temporal
{
public:
  Item_cache_date(THD *thd)
   :Item_cache_temporal(thd, &type_handler_newdate) { }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_date>(thd, this); }
  Item *make_literal(THD *) override;
  longlong val_datetime_packed(THD *) override
  {
    return has_value() ? value : 0;
  }
  longlong val_time_packed(THD *thd) override
  {
    return Time(thd, this, Time::Options_cmp(thd)).to_packed();
  }
  longlong val_int() override
  { return has_value() ? Date(this).to_longlong() : 0; }
  double val_real() override
  { return has_value() ? Date(this).to_double() : 0; }
  String *val_str(String *to) override
  {
    return has_value() ? Date(this).to_string(to) : NULL;
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return has_value() ? Date(this).to_decimal(to) : NULL;
  }
};


class Item_cache_timestamp: public Item_cache
{
  Timestamp_or_zero_datetime_native m_native;
  Datetime to_datetime(THD *thd);
public:
  Item_cache_timestamp(THD *thd)
   :Item_cache(thd, &type_handler_timestamp2) { }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_timestamp>(thd, this); }
  bool cache_value() override;
  String* val_str(String *to) override
  {
    return to_datetime(current_thd).to_string(to, decimals);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return to_datetime(current_thd).to_decimal(to);
  }
  longlong val_int() override
  {
    return to_datetime(current_thd).to_longlong();
  }
  double val_real() override
  {
    return to_datetime(current_thd).to_double();
  }
  longlong val_datetime_packed(THD *thd) override
  {
    return to_datetime(thd).to_packed();
  }
  longlong val_time_packed(THD *) override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  int save_in_field(Field *field, bool no_conversions) override;
  bool val_native(THD *thd, Native *to) override;
};


class Item_cache_real: public Item_cache
{
protected:
  double value;
public:
  Item_cache_real(THD *thd, const Type_handler *h)
   :Item_cache(thd, h),
    value(0)
  {}
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return get_date_from_real(thd, ltime, fuzzydate); }
  bool cache_value() override;
  Item *convert_to_basic_const_item(THD *thd) override;
};


class Item_cache_double: public Item_cache_real
{
public:
  Item_cache_double(THD *thd)
   :Item_cache_real(thd, &type_handler_double)
  { }
  String *val_str(String *str) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_double>(thd, this); }
};


class Item_cache_float: public Item_cache_real
{
public:
  Item_cache_float(THD *thd)
   :Item_cache_real(thd, &type_handler_float)
  { }
  String *val_str(String *str) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_float>(thd, this); }
};


class Item_cache_decimal: public Item_cache
{
protected:
  my_decimal decimal_value;
public:
  Item_cache_decimal(THD *thd): Item_cache(thd, &type_handler_newdecimal) {}

  double val_real() override;
  longlong val_int() override;
  String* val_str(String *str) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *to, date_mode_t mode) override
  {
    return decimal_to_datetime_with_warn(thd, VDec(this).ptr(), to, mode,
                                         NULL, NULL);
  }
  bool cache_value() override;
  Item *convert_to_basic_const_item(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_decimal>(thd, this); }
};


class Item_cache_str: public Item_cache
{
  char buffer[STRING_BUFFER_USUAL_SIZE];
  String *value, value_buff;
  bool is_varbinary;
  
public:
  Item_cache_str(THD *thd, const Item *item):
    Item_cache(thd, item->type_handler()), value(0),
    is_varbinary(item->type() == FIELD_ITEM &&
                 Item_cache_str::field_type() == MYSQL_TYPE_VARCHAR &&
                 !((const Item_field *) item)->field->has_charset())
  {
    collation.set(const_cast<DTCollation&>(item->collation));
  }
  double val_real() override;
  longlong val_int() override;
  String* val_str(String *) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return get_date_from_string(thd, ltime, fuzzydate); }
  CHARSET_INFO *charset() const { return value->charset(); };
  int save_in_field(Field *field, bool no_conversions) override;
  bool cache_value() override;
  Item *convert_to_basic_const_item(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_str>(thd, this); }
};


class Item_cache_str_for_nullif: public Item_cache_str
{
public:
  Item_cache_str_for_nullif(THD *thd, const Item *item)
   :Item_cache_str(thd, item)
  { }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override
  {
    /**
      Item_cache_str::safe_charset_converter() returns a new Item_cache
      with Item_func_conv_charset installed on "example". The original
      Item_cache is not referenced (neither directly nor recursively)
      from the result of Item_cache_str::safe_charset_converter().

      For NULLIF() purposes we need a different behavior:
      we need a new instance of Item_func_conv_charset,
      with the original Item_cache referenced in args[0]. See MDEV-9181.
    */
    return Item::safe_charset_converter(thd, tocs);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_str_for_nullif>(thd, this); }
};


class Item_cache_row: public Item_cache
{
  Item_cache  **values;
  uint item_count;
  bool save_array;
public:
  Item_cache_row(THD *thd):
    Item_cache(thd), values(0), item_count(2),
    save_array(0) {}

  /*
    'allocate' used only in row transformer, to preallocate space for row
    cache.
  */
  bool allocate(THD *thd, uint num) override;
  /*
    'setup' is needed only by row => it not called by simple row subselect
    (only by IN subselect (in subselect optimizer))
  */
  bool setup(THD *thd, Item *item) override;
  void store(Item *item) override;
  void illegal_method_call(const char *);
  void make_send_field(THD *, Send_field *) override
  {
    illegal_method_call("make_send_field");
  };
  double val_real() override
  {
    illegal_method_call("val");
    return 0;
  };
  longlong val_int() override
  {
    illegal_method_call("val_int");
    return 0;
  };
  String *val_str(String *) override
  {
    illegal_method_call("val_str");
    return nullptr;
  };
  my_decimal *val_decimal(my_decimal *) override
  {
    illegal_method_call("val_decimal");
    return nullptr;
  };
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    illegal_method_call("val_decimal");
    return true;
  }

  uint cols() const override { return item_count; }
  Item *element_index(uint i) override { return values[i]; }
  Item **addr(uint i) override { return (Item **) (values + i); }
  bool check_cols(uint c) override;
  bool null_inside() override;
  void bring_value() override;
  void keep_array() override { save_array= 1; }
  void cleanup() override
  {
    DBUG_ENTER("Item_cache_row::cleanup");
    Item_cache::cleanup();
    if (save_array)
      bzero(values, item_count*sizeof(Item**));
    else
      values= 0;
    DBUG_VOID_RETURN;
  }
  bool cache_value() override;
  void set_null() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_row>(thd, this); }
};


/*
  Item_type_holder used to store type. name, length of Item for UNIONS &
  derived tables.

  Item_type_holder do not need cleanup() because its time of live limited by
  single SP/PS execution.
*/
class Item_type_holder: public Item, public Type_handler_hybrid_field_type
{
protected:
  const TYPELIB *enum_set_typelib;
public:
  Item_type_holder(THD *thd, Item *item, const Type_handler *handler,
                   const Type_all_attributes *attr, bool maybe_null_arg)
   :Item(thd), Type_handler_hybrid_field_type(handler),
    enum_set_typelib(attr->get_typelib())
  {
    name= item->name;
    Type_std_attributes::set(*attr);
    set_maybe_null(maybe_null_arg);
    copy_flags(item, item_base_t::IS_EXPLICIT_NAME |
                     item_base_t::IS_IN_WITH_CYCLE);
  }

  const Type_handler *type_handler() const override
  {
    return Type_handler_hybrid_field_type::type_handler()->
             type_handler_for_item_field();
  }
  const Type_handler *real_type_handler() const override
  {
    return Type_handler_hybrid_field_type::type_handler();
  }

  Type type() const override { return TYPE_HOLDER; }
  const TYPELIB *get_typelib() const override { return enum_set_typelib; }
  /*
    When handling a query like this:
      VALUES ('') UNION VALUES( _utf16 0x0020 COLLATE utf16_bin);
    Item_type_holder can be passed to
      Type_handler_xxx::Item_hybrid_func_fix_attributes()
    We don't want the latter to perform character set conversion of a
    Item_type_holder by calling its val_str(), which calls DBUG_ASSERT(0).
    Let's override const_item() and is_expensive() to avoid this.
    Note, Item_hybrid_func_fix_attributes() could probably
    have a new argument to distinguish what we need:
    - (a) aggregate data type attributes only
    - (b) install converters after attribute aggregation
    So st_select_lex_unit::join_union_type_attributes() could
    ask it to do (a) only, without (b).
  */
  bool const_item() const override { return false; }
  bool is_expensive() override { return true; }
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;
  String *val_str(String*) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    return Item_type_holder::real_type_handler()->
      make_and_init_table_field(root, &name, Record_addr(maybe_null()),
                                *this, table);
  }
  Item* get_copy(THD *) override { return nullptr; }
};


class st_select_lex;
void mark_select_range_as_dependent(THD *thd,
                                    st_select_lex *last_select,
                                    st_select_lex *current_sel,
                                    Field *found_field, Item *found_item,
                                    Item_ident *resolved_item,
                                    bool suppress_warning_output);

extern Cached_item *new_Cached_item(THD *thd, Item *item,
                                    bool pass_through_ref);
extern Item_result item_cmp_type(Item_result a,Item_result b);
extern void resolve_const_item(THD *thd, Item **ref, Item *cmp_item);
extern int stored_field_cmp_to_item(THD *thd, Field *field, Item *item);

extern const String my_null_string;

/**
  Interface for Item iterator
*/

class Item_iterator
{
public:
  /**
    Shall set this iterator to the position before the first item

    @note
    This method also may perform some other initialization actions like
    allocation of certain resources.
  */
  virtual void open()= 0;
  /**
    Shall return the next Item (or NULL if there is no next item) and
    move pointer to position after it.
  */
  virtual Item *next()= 0;
  /**
    Shall force iterator to free resources (if it holds them)

    @note
    One should not use the iterator without open() call after close()
  */
  virtual void close()= 0;

  virtual ~Item_iterator() {}
};


/**
  Item iterator over List_iterator_fast for Item references
*/

class Item_iterator_ref_list: public Item_iterator
{
  List_iterator<Item*> list;
public:
  Item_iterator_ref_list(List_iterator<Item*> &arg_list):
    list(arg_list) {}
  void open() { list.rewind(); }
  Item *next() { return *(list++); }
  void close() {}
};


/**
  Item iterator over List_iterator_fast for Items
*/

class Item_iterator_list: public Item_iterator
{
  List_iterator<Item> list;
public:
  Item_iterator_list(List_iterator<Item> &arg_list):
    list(arg_list) {}
  void open() { list.rewind(); }
  Item *next() { return (list++); }
  void close() {}
};


/**
  Item iterator over Item interface for rows
*/

class Item_iterator_row: public Item_iterator
{
  Item *base_item;
  uint current;
public:
  Item_iterator_row(Item *base) : base_item(base), current(0) {}
  void open() { current= 0; }
  Item *next()
  {
    if (current >= base_item->cols())
      return NULL;
    return base_item->element_index(current++);
  }
  void close() {}
};


/*
  It's used in ::fix_fields() methods of LIKE and JSON_SEARCH
  functions to handle the ESCAPE parameter.
  This parameter is quite non-standard so the specific function.
*/
bool fix_escape_item(THD *thd, Item *escape_item, String *tmp_str,
                     bool escape_used_in_parsing, CHARSET_INFO *cmp_cs,
                     int *escape);

inline bool Virtual_column_info::is_equal(const Virtual_column_info* vcol) const
{
  return type_handler()  == vcol->type_handler()
      && stored_in_db == vcol->is_stored()
      && expr->eq(vcol->expr, true);
}

inline void Virtual_column_info::print(String* str)
{
  expr->print_for_table_def(str);
}

inline bool TABLE::mark_column_with_deps(Field *field)
{
  bool res;
  if (!(res= bitmap_fast_test_and_set(read_set, field->field_index)))
  {
    if (field->vcol_info)
      mark_virtual_column_deps(field);
  }
  return res;
}

inline bool TABLE::mark_virtual_column_with_deps(Field *field)
{
  bool res;
  DBUG_ASSERT(field->vcol_info);
  if (!(res= bitmap_fast_test_and_set(read_set, field->field_index)))
    mark_virtual_column_deps(field);
  return res;
}

inline void TABLE::mark_virtual_column_deps(Field *field)
{
  DBUG_ASSERT(field->vcol_info);
  DBUG_ASSERT(field->vcol_info->expr);
  field->vcol_info->expr->walk(&Item::register_field_in_read_map, 1, 0);
}

inline void TABLE::use_all_stored_columns()
{
  bitmap_set_all(read_set);
  if (Field **vf= vfield)
    for (; *vf; vf++)
      bitmap_clear_bit(read_set, (*vf)->field_index);
}

#endif /* SQL_ITEM_INCLUDED */
