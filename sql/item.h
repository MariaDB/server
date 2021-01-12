#ifndef SQL_ITEM_INCLUDED
#define SQL_ITEM_INCLUDED

/* Copyright (c) 2000, 2017, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB Corporation.

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
struct st_value
{
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

#ifdef DBUG_OFF
static inline const char *dbug_print_item(Item *item) { return NULL; }
#else
const char *dbug_print_item(Item *item);
#endif

class Protocol;
struct TABLE_LIST;
void item_init(void);			/* Init item functions */
class Item_field;
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

typedef Bounds_checked_array<Item*> Ref_ptr_array;

static inline uint32
char_to_byte_length_safe(size_t char_length_arg, uint32 mbmaxlen_arg)
{
  ulonglong tmp= ((ulonglong) char_length_arg) * mbmaxlen_arg;
  return tmp > UINT_MAX32 ? UINT_MAX32 : static_cast<uint32>(tmp);
}

bool mark_unsupported_function(const char *where, void *store, uint result);

/* convenience helper for mark_unsupported_function() above */
bool mark_unsupported_function(const char *w1, const char *w2,
                               void *store, uint result);

/* Bits for the split_sum_func() function */
#define SPLIT_SUM_SKIP_REGISTERED 1     /* Skip registered funcs */
#define SPLIT_SUM_SELECT 2		/* SELECT item; Split all parts */

/*
   "Declared Type Collation"
   A combination of collation and its derivation.

  Flags for collation aggregation modes:
  MY_COLL_ALLOW_SUPERSET_CONV  - allow conversion to a superset
  MY_COLL_ALLOW_COERCIBLE_CONV - allow conversion of a coercible value
                                 (i.e. constant).
  MY_COLL_ALLOW_CONV           - allow any kind of conversion
                                 (combination of the above two)
  MY_COLL_ALLOW_NUMERIC_CONV   - if all items were numbers, convert to
                                 @@character_set_connection
  MY_COLL_DISALLOW_NONE        - don't allow return DERIVATION_NONE
                                 (e.g. when aggregating for comparison)
  MY_COLL_CMP_CONV             - combination of MY_COLL_ALLOW_CONV
                                 and MY_COLL_DISALLOW_NONE
*/

#define MY_COLL_ALLOW_SUPERSET_CONV   1
#define MY_COLL_ALLOW_COERCIBLE_CONV  2
#define MY_COLL_DISALLOW_NONE         4
#define MY_COLL_ALLOW_NUMERIC_CONV    8

#define MY_COLL_ALLOW_CONV (MY_COLL_ALLOW_SUPERSET_CONV | MY_COLL_ALLOW_COERCIBLE_CONV)
#define MY_COLL_CMP_CONV   (MY_COLL_ALLOW_CONV | MY_COLL_DISALLOW_NONE)

#define NO_EXTRACTION_FL              (1 << 6)
#define FULL_EXTRACTION_FL            (1 << 7)
#define SUBSTITUTION_FL               (1 << 8)
#define EXTRACTION_MASK               (NO_EXTRACTION_FL | FULL_EXTRACTION_FL)

class DTCollation {
public:
  CHARSET_INFO     *collation;
  enum Derivation derivation;
  uint repertoire;
  
  void set_repertoire_from_charset(CHARSET_INFO *cs)
  {
    repertoire= cs->state & MY_CS_PUREASCII ?
                MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  }
  DTCollation()
  {
    collation= &my_charset_bin;
    derivation= DERIVATION_NONE;
    repertoire= MY_REPERTOIRE_UNICODE30;
  }
  DTCollation(CHARSET_INFO *collation_arg, Derivation derivation_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  DTCollation(CHARSET_INFO *collation_arg,
              Derivation derivation_arg,
              uint repertoire_arg)
   :collation(collation_arg),
    derivation(derivation_arg),
    repertoire(repertoire_arg)
  { }
  void set(const DTCollation &dt)
  { 
    collation= dt.collation;
    derivation= dt.derivation;
    repertoire= dt.repertoire;
  }
  void set(CHARSET_INFO *collation_arg, Derivation derivation_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  void set(CHARSET_INFO *collation_arg,
           Derivation derivation_arg,
           uint repertoire_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    repertoire= repertoire_arg;
  }
  void set_numeric()
  {
    collation= &my_charset_numeric;
    derivation= DERIVATION_NUMERIC;
    repertoire= MY_REPERTOIRE_NUMERIC;
  }
  void set(CHARSET_INFO *collation_arg)
  {
    collation= collation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  void set(Derivation derivation_arg)
  { derivation= derivation_arg; }
  bool aggregate(const DTCollation &dt, uint flags= 0);
  bool set(DTCollation &dt1, DTCollation &dt2, uint flags= 0)
  { set(dt1); return aggregate(dt2, flags); }
  const char *derivation_name() const
  {
    switch(derivation)
    {
      case DERIVATION_NUMERIC:   return "NUMERIC";
      case DERIVATION_IGNORABLE: return "IGNORABLE";
      case DERIVATION_COERCIBLE: return "COERCIBLE";
      case DERIVATION_IMPLICIT:  return "IMPLICIT";
      case DERIVATION_SYSCONST:  return "SYSCONST";
      case DERIVATION_EXPLICIT:  return "EXPLICIT";
      case DERIVATION_NONE:      return "NONE";
      default: return "UNKNOWN";
    }
  }
  int sortcmp(const String *s, const String *t) const
  {
    return collation->coll->strnncollsp(collation,
                                        (uchar *) s->ptr(), s->length(),
                                        (uchar *) t->ptr(), t->length());
  }
};


void dummy_error_processor(THD *thd, void *data);

void view_error_processor(THD *thd, void *data);

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
    Security context of this name resolution context. It's used for views
    and is non-zero only if the view is defined with SQL SECURITY DEFINER.
  */
  Security_context *security_ctx;

  Name_resolution_context()
    :outer_context(0), table_list(0), select_lex(0),
    error_processor_data(0),
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
  uint pos_in_query;

  /*
    Byte length of parameter name in the statement.  This is not
    Item::name_length because name_length contains byte length of UTF8-encoded
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

struct st_cond_statistic;

struct find_selective_predicates_list_processor_data
{
  TABLE *table;
  List<st_cond_statistic> list;
};

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
  A class to store type attributes for the standard data types.
  Does not include attributes for the extended data types
  such as ENUM, SET, GEOMETRY.
*/
class Type_std_attributes
{
public:
  DTCollation collation;
  uint decimals;
  /*
    The maximum value length in characters multiplied by collation->mbmaxlen.
    Almost always it's the maximum value length in bytes.
  */
  uint32 max_length;
  bool unsigned_flag;
  Type_std_attributes()
   :collation(&my_charset_bin, DERIVATION_COERCIBLE),
    decimals(0), max_length(0), unsigned_flag(false)
  { }
  Type_std_attributes(const Type_std_attributes *other)
   :collation(other->collation),
    decimals(other->decimals),
    max_length(other->max_length),
    unsigned_flag(other->unsigned_flag)
  { }
  void set(const Type_std_attributes *other)
  {
    *this= *other;
  }
  void set(const Field *field)
  {
    decimals= field->decimals();
    max_length= field->field_length;
    collation.set(field->charset());
    unsigned_flag= MY_TEST(field->flags & UNSIGNED_FLAG);
  }
};


class Item: public Value_source,
            public Type_std_attributes,
            public Type_handler
{
  /**
    The index in the JOIN::join_tab array of the JOIN_TAB this Item is attached
    to. Items are attached (or 'pushed') to JOIN_TABs during optimization by the
    make_cond_for_table procedure. During query execution, this item is
    evaluated when the join loop reaches the corresponding JOIN_TAB.

    If the value of join_tab_idx >= MAX_TABLES, this means that there is no
    corresponding JOIN_TAB.
  */
  uint join_tab_idx;

  static void *operator new(size_t size);

public:
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void operator delete(void *ptr,size_t size) { TRASH_FREE(ptr, size); }
  static void operator delete(void *ptr, MEM_ROOT *mem_root) {}

  enum Type {FIELD_ITEM= 0, FUNC_ITEM, SUM_FUNC_ITEM,
             WINDOW_FUNC_ITEM, STRING_ITEM,
	     INT_ITEM, REAL_ITEM, NULL_ITEM, VARBIN_ITEM,
	     COPY_STR_ITEM, FIELD_AVG_ITEM, DEFAULT_VALUE_ITEM,
	     PROC_ITEM,COND_ITEM, REF_ITEM, FIELD_STD_ITEM,
	     FIELD_VARIANCE_ITEM, INSERT_VALUE_ITEM,
             SUBSELECT_ITEM, ROW_ITEM, CACHE_ITEM, TYPE_HOLDER,
             PARAM_ITEM, TRIGGER_FIELD_ITEM, DECIMAL_ITEM,
             XPATH_NODESET, XPATH_NODESET_CMP,
             VIEW_FIXER_ITEM, EXPR_CACHE_ITEM,
             DATE_ITEM};

  enum cond_result { COND_UNDEF,COND_OK,COND_TRUE,COND_FALSE };

  enum traverse_order { POSTFIX, PREFIX };
  
  /* Cache of the result of is_expensive(). */
  int8 is_expensive_cache;
  
  /* Reuse size, only used by SP local variable assignment, otherwise 0 */
  uint rsize;

protected:
  /*
    str_values's main purpose is to be used to cache the value in
    save_in_field
  */
  String str_value;

  SEL_TREE *get_mm_tree_for_const(RANGE_OPT_PARAM *param);

  virtual Field *make_string_field(TABLE *table);
  Field *tmp_table_field_from_field_type(TABLE *table,
                                         bool fixed_length,
                                         bool set_blob_packlength);
  Field *create_tmp_field(bool group, TABLE *table, uint convert_int_length);
  /* Helper methods, to get an Item value from another Item */
  double val_real_from_item(Item *item)
  {
    DBUG_ASSERT(fixed == 1);
    double value= item->val_real();
    null_value= item->null_value;
    return value;
  }
  longlong val_int_from_item(Item *item)
  {
    DBUG_ASSERT(fixed == 1);
    longlong value= item->val_int();
    null_value= item->null_value;
    return value;
  }
  String *val_str_from_item(Item *item, String *str)
  {
    DBUG_ASSERT(fixed == 1);
    String *res= item->val_str(str);
    if (res)
      res->set_charset(collation.collation);
    if ((null_value= item->null_value))
      res= NULL;
    return res;
  }
  my_decimal *val_decimal_from_item(Item *item, my_decimal *decimal_value)
  {
    DBUG_ASSERT(fixed == 1);
    my_decimal *value= item->val_decimal(decimal_value);
    if ((null_value= item->null_value))
      value= NULL;
    return value;
  }
  bool get_date_from_item(Item *item, MYSQL_TIME *ltime, ulonglong fuzzydate)
  {
    bool rc= item->get_date(ltime, fuzzydate);
    null_value= MY_TEST(rc || item->null_value);
    return rc;
  }
  /*
    This method is used if the item was not null but conversion to
    TIME/DATE/DATETIME failed. We return a zero date if allowed,
    otherwise - null.
  */
  bool make_zero_date(MYSQL_TIME *ltime, ulonglong fuzzydate);

  void push_note_converted_to_negative_complement(THD *thd);
  void push_note_converted_to_positive_complement(THD *thd);
public:
  /*
    Cache val_str() into the own buffer, e.g. to evaluate constant
    expressions with subqueries in the ORDER/GROUP clauses.
  */
  String *val_str() { return val_str(&str_value); }

  char * name;			/* Name from select */
  /* Original item name (if it was renamed)*/
  char * orig_name;
  /**
     Intrusive list pointer for free list. If not null, points to the next
     Item on some Query_arena's free list. For instance, stored procedures
     have their own Query_arena's.

     @see Query_arena::free_list
   */
  Item *next;
  /*
    TODO: convert name and name_length fields into LEX_STRING to keep them in
    sync (see bug #11829681/60295 etc). Then also remove some strlen(name)
    calls.
  */
  uint name_length;                     /* Length of name */
  int  marker;
  bool maybe_null;			/* If item may be null */
  bool in_rollup;                       /* If used in GROUP BY list
                                           of a query with ROLLUP */ 
  bool null_value;			/* if item is null */
  bool with_sum_func;                   /* True if item contains a sum func */
  bool with_param;                      /* True if contains an SP parameter */
  bool with_window_func;             /* True if item contains a window func */
  /**
    True if any item except Item_sum contains a field. Set during parsing.
  */
  bool with_field;
  bool fixed;                           /* If item fixed with fix_fields */
  bool is_autogenerated_name;           /* indicate was name of this Item
                                           autogenerated or set by user */
  bool with_subselect;                  /* If this item is a subselect or some
                                           of its arguments is or contains a
                                           subselect */
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
  virtual ~Item()
  {
#ifdef EXTRA_DEBUG
    name=0;
#endif
  }		/*lint -e1509 */
  void set_name(THD *thd, const char *str, uint length, CHARSET_INFO *cs);
  void set_name_no_truncate(THD *thd, const char *str, uint length,
                            CHARSET_INFO *cs);
  void set_name_for_rollback(THD *thd, const char *str, uint length,
                             CHARSET_INFO *cs);
  void rename(char *new_name);
  void share_name_with(Item *item)
  {
    name= item->name;
    name_length= item->name_length;
    is_autogenerated_name= item->is_autogenerated_name;
  }
  void init_make_field(Send_field *tmp_field,enum enum_field_types type);
  virtual void cleanup();
  virtual void make_field(THD *thd, Send_field *field);
  virtual bool fix_fields(THD *, Item **);
  /*
    Fix after some tables has been pulled out. Basically re-calculate all
    attributes that are dependent on the tables.
  */
  virtual void fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                 bool merge)
    {};

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
  virtual inline void quick_fix_field() { fixed= 1; }

  bool store(struct st_value *value, ulonglong fuzzydate)
  {
    switch (cmp_type()) {
    case INT_RESULT:
    {
      value->m_type= unsigned_flag ? DYN_COL_UINT : DYN_COL_INT;
      value->value.m_longlong= val_int();
      break;
    }
    case REAL_RESULT:
    {
      value->m_type= DYN_COL_DOUBLE;
      value->value.m_double= val_real();
      break;
    }
    case DECIMAL_RESULT:
    {
      value->m_type= DYN_COL_DECIMAL;
      my_decimal *dec= val_decimal(&value->m_decimal);
      if (dec != &value->m_decimal && !null_value)
        my_decimal2decimal(dec, &value->m_decimal);
      break;
    }
    case STRING_RESULT:
    {
      value->m_type= DYN_COL_STRING;
      String *str= val_str(&value->m_string);
      if (str != &value->m_string && !null_value)
        value->m_string.set(str->ptr(), str->length(), str->charset());
      break;
    }
    case TIME_RESULT:
    {
      value->m_type= DYN_COL_DATETIME;
      get_date(&value->value.m_time, fuzzydate);
      break;
    }
    case ROW_RESULT:
      DBUG_ASSERT(false);
      null_value= true;
      break;
    }
    if (null_value)
    {
      value->m_type= DYN_COL_NULL;
      return true;
    }
    return false;
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
  virtual bool send(Protocol *protocol, String *str);
  virtual bool eq(const Item *, bool binary_cmp) const;
  const Type_handler  *type_handler() const
  {
    return get_handler_by_field_type(field_type());
  }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                            const Item *item) const
  {
    return type_handler()->make_num_distinct_aggregator_field(mem_root, this);
  }
  Field *make_conversion_table_field(TABLE *table,
                                     uint metadata, const Field *target) const
  {
    DBUG_ASSERT(0); // Should not be called in Item context
    return NULL;
  }
  /* result_type() of an item specifies how the value should be returned */
  Item_result result_type() const { return type_handler()->result_type(); }
  /* ... while cmp_type() specifies how it should be compared */
  Item_result cmp_type() const { return type_handler()->cmp_type(); }
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const
  {
    type_handler()->make_sort_key(to, item, sort_field, param);
  }
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const
  {
    type_handler()->sortlength(thd, item, attr);
  }
  virtual Item_result cast_to_int_type() const { return cmp_type(); }
  enum_field_types string_field_type() const
  {
    return Type_handler::string_type_handler(max_length)->field_type();
  }
  virtual enum Type type() const =0;
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
    Convert "func_arg $CMP$ const" half-interval into "FUNC(func_arg) $CMP2$ const2"

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
  /*
    Return integer representation of item.

    SYNOPSIS
      val_int()

    RETURN
      In case of NULL value return 0 and set null_value flag to TRUE.
      If value is not null null_value flag will be reset to FALSE.
  */
  virtual longlong val_int()=0;
  /**
    Get a value for CAST(x AS SIGNED).
    Too large positive unsigned integer values are converted
    to negative complements.
    Values of non-integer data types are adjusted to the SIGNED range.
  */
  virtual longlong val_int_signed_typecast();
  /**
    Get a value for CAST(x AS UNSIGNED).
    Negative signed integer values are converted
    to positive complements.
    Values of non-integer data types are adjusted to the UNSIGNED range.
  */
  virtual longlong val_int_unsigned_typecast();
  Longlong_hybrid to_longlong_hybrid()
  {
    return Longlong_hybrid(val_int(), unsigned_flag);
  }
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
  virtual bool val_bool();
  virtual String *val_nodeset(String*) { return 0; }

  bool eval_const_cond()
  {
    DBUG_ASSERT(const_item());
    DBUG_ASSERT(!is_expensive());
    return val_bool();
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
  String *val_string_from_decimal(String *str);
  String *val_string_from_date(String *str);
  my_decimal *val_decimal_from_real(my_decimal *decimal_value);
  my_decimal *val_decimal_from_int(my_decimal *decimal_value);
  my_decimal *val_decimal_from_string(my_decimal *decimal_value);
  my_decimal *val_decimal_from_date(my_decimal *decimal_value);
  my_decimal *val_decimal_from_time(my_decimal *decimal_value);
  longlong val_int_from_decimal();
  longlong val_int_from_date();
  longlong val_int_from_real()
  {
    DBUG_ASSERT(fixed == 1);
    return Converter_double_to_longlong_with_warn(val_real(), false).result();
  }
  longlong val_int_from_str(int *error);
  double val_real_from_decimal();
  double val_real_from_date();

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

  // Get TIME, DATE or DATETIME using proper sql_mode flags for the field type
  bool get_temporal_with_sql_mode(MYSQL_TIME *ltime);
  // Check NULL value for a TIME, DATE or DATETIME expression
  bool is_null_from_temporal();

  int save_time_in_field(Field *field);
  int save_date_in_field(Field *field);
  int save_str_value_in_field(Field *field, String *result);

  virtual Field *get_tmp_table_field() { return 0; }
  virtual Field *create_field_for_create_select(TABLE *table);
  virtual Field *create_field_for_schema(THD *thd, TABLE *table);
  virtual const char *full_name() const { return name ? name : "???"; }
  const char *field_name_or_null()
  { return real_item()->type() == Item::FIELD_ITEM ? name : NULL; }
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
  /*
    Determines if the expression is allowed as
    a virtual column assignment source:
      INSERT INTO t1 (vcol) VALUES (10)    -> error
      INSERT INTO t1 (vcol) VALUES (NULL)  -> ok
  */
  virtual bool vcol_assignment_allowed_value() const { return false; }
  /* cloning of constant items (0 if it is not const) */
  virtual Item *clone_item(THD *thd) { return 0; }
  virtual Item* build_clone(THD *thd, MEM_ROOT *mem_root) { return get_copy(thd, mem_root); }
  virtual cond_result eq_cmp_result() const { return COND_OK; }
  inline uint float_length(uint decimals_par) const
  { return decimals < FLOATING_POINT_DECIMALS ? (DBL_DIG+2+decimals_par) : DBL_DIG+8;}
  /* Returns total number of decimal digits */
  virtual uint decimal_precision() const;
  /* Returns the number of integer part digits only */
  inline int decimal_int_part() const
  { return my_decimal_int_part(decimal_precision(), decimals); }
  /*
    Returns the number of fractional digits only.
    NOT_FIXED_DEC is replaced to the maximum possible number
    of fractional digits, taking into account the data type.
  */
  uint decimal_scale() const
  {
    return decimals < NOT_FIXED_DEC ? decimals :
           is_temporal_type_with_time(field_type()) ?
           TIME_SECOND_PART_DIGITS :
           MY_MIN(max_length, DECIMAL_MAX_SCALE);
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
    return decimals <  NOT_FIXED_DEC ? decimals :
           is_temporal_type_with_time(field_type()) ?
           TIME_SECOND_PART_DIGITS :
           decimals;
  }
  /**
    TIME or DATETIME precision of the item: 0..6
  */
  uint temporal_precision(enum_field_types type);
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
  virtual bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  bool get_time(MYSQL_TIME *ltime)
  { return get_date(ltime, TIME_TIME_ONLY | TIME_INVALID_DATES); }
  // Get date with automatic TIME->DATETIME conversion
  bool convert_time_to_datetime(THD *thd, MYSQL_TIME *ltime, ulonglong fuzzydate)
  {
    MYSQL_TIME tmp;
    if (time_to_datetime_with_warn(thd, ltime, &tmp, fuzzydate))
      return null_value= true;
    *ltime= tmp;
    return false;
  }
  bool get_date_with_conversion(MYSQL_TIME *ltime, ulonglong fuzzydate);
  /*
    Get time with automatic DATE/DATETIME to TIME conversion.

    Performes a reverse operation to get_date_with_conversion().
    Suppose:
    - we have a set of items (typically with the native MYSQL_TYPE_TIME type)
      whose item->get_date() return TIME1 value, and
    - item->get_date_with_conversion() for the same Items return DATETIME1,
      after applying time-to-datetime conversion to TIME1.

    then all items (typically of the native MYSQL_TYPE_{DATE|DATETIME} types)
    whose get_date() return DATETIME1 must also return TIME1 from
    get_time_with_conversion()

    @param thd        - the thread, its variables.old_mode is checked
                        to decide if use simple YYYYMMDD truncation (old mode),
                        or perform full DATETIME-to-TIME conversion with
                        CURRENT_DATE subtraction.
    @param[out] ltime - store the result here
    @param fuzzydate  - flags to be used for the get_date() call.
                        Normally, should include TIME_TIME_ONLY, to let
                        the called low-level routines, e.g. str_to_date(),
                        know that we prefer TIME rather that DATE/DATETIME
                        and do less conversion outside of the low-level
                        routines.

    @returns true     - on error, e.g. get_date() returned NULL value,
                        or get_date() returned DATETIME/DATE with non-zero
                        YYYYMMDD part.
    @returns false    - on success
  */
  bool get_time_with_conversion(THD *thd, MYSQL_TIME *ltime,
                                ulonglong fuzzydate);
  // Get a DATE or DATETIME value in numeric packed format for comparison
  virtual longlong val_datetime_packed()
  {
    MYSQL_TIME ltime;
    uint fuzzydate= TIME_FUZZY_DATES | TIME_INVALID_DATES;
    return get_date_with_conversion(&ltime, fuzzydate) ? 0 : pack_time(&ltime);
  }
  // Get a TIME value in numeric packed format for comparison
  virtual longlong val_time_packed()
  {
    MYSQL_TIME ltime;
    uint fuzzydate= TIME_FUZZY_DATES | TIME_INVALID_DATES | TIME_TIME_ONLY;
    return get_date(&ltime, fuzzydate) ? 0 : pack_time(&ltime);
  }
  longlong val_datetime_packed_result();
  longlong val_time_packed_result()
  {
    MYSQL_TIME ltime;
    uint fuzzydate= TIME_TIME_ONLY | TIME_INVALID_DATES | TIME_FUZZY_DATES;
    return get_date_result(&ltime, fuzzydate) ? 0 : pack_time(&ltime);
  }
  // Get a temporal value in packed DATE/DATETIME or TIME format
  longlong val_temporal_packed(enum_field_types f_type)
  {
    return f_type == MYSQL_TYPE_TIME ? val_time_packed() :
                                       val_datetime_packed();
  }
  enum_field_types field_type_for_temporal_comparison(const Item *other) const
  {
    if (cmp_type() == TIME_RESULT)
    {
      if (other->cmp_type() == TIME_RESULT)
        return Field::field_type_merge(field_type(), other->field_type());
      else
        return field_type();
    }
    else
    {
      if (other->cmp_type() == TIME_RESULT)
        return other->field_type();
      DBUG_ASSERT(0); // Two non-temporal data types, we should not get to here
      return MYSQL_TYPE_DATETIME;
    }
  }
  // Get a temporal value to compare to another Item
  longlong val_temporal_packed(const Item *other)
  {
    return val_temporal_packed(field_type_for_temporal_comparison(other));
  }
  bool get_seconds(ulonglong *sec, ulong *sec_part);
  virtual bool get_date_result(MYSQL_TIME *ltime, ulonglong fuzzydate)
  { return get_date(ltime,fuzzydate); }
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
    switch (cmp_type()) {
    case INT_RESULT:
      (void) val_int();
      break;
    case REAL_RESULT:
      (void) val_real();
      break;
    case DECIMAL_RESULT:
      {
        my_decimal tmp;
        (void) val_decimal(&tmp);
      }
      break;
    case TIME_RESULT:
      {
        MYSQL_TIME ltime;
        (void) get_temporal_with_sql_mode(&ltime);
      }
      break;
    case STRING_RESULT:
      {
        StringBuffer<MAX_FIELD_WIDTH> tmp;
        (void) val_str(&tmp);
      }
      break;
    case ROW_RESULT:
      DBUG_ASSERT(0);
      null_value= true;
    }
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
    set field of temporary table for Item which can be switched on temporary
    table during query processing (grouping and so on)
  */
  virtual void set_result_field(Field *field) {}
  virtual bool is_result_field() { return 0; }
  virtual bool is_bool_type() { return false; }
  virtual bool is_json_type() { return false; }
  /* This is to handle printing of default values */
  virtual bool need_parentheses_in_default() { return false; }
  virtual void save_in_result_field(bool no_conversions) {}
  /*
    set value of aggregate function in case of no rows for grouping were found
  */
  virtual void no_rows_in_result() {}
  virtual void restore_to_before_no_rows_in_result() {}
  virtual Item *copy_or_same(THD *thd) { return this; }
  virtual Item *copy_andor_structure(THD *thd) { return this; }
  virtual Item *real_item() { return this; }
  virtual Item *get_tmp_table_item(THD *thd) { return copy_or_same(thd); }

  static CHARSET_INFO *default_charset();

  /*
    For backward compatibility, to make numeric
    data types return "binary" charset in client-side metadata.
  */
  virtual CHARSET_INFO *charset_for_protocol(void) const
  {
    return cmp_type() == STRING_RESULT ? collation.collation :
                                         &my_charset_bin;
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
  virtual bool cleanup_excluding_fields_processor(void *arg) { return cleanup_processor(arg); }
  virtual bool cleanup_excluding_const_fields_processor(void *arg) { return cleanup_processor(arg); }
  virtual bool collect_item_field_processor(void *arg) { return 0; }
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
  bool cleanup_is_expensive_cache_processor(void *arg)
  {
    is_expensive_cache= (int8)(-1);
    return 0;
  }

  /* 
    TRUE if the expression depends only on the table indicated by tab_map
    or can be converted to such an exression using equalities.
    Not to be used for AND/OR formulas.
  */
  virtual bool excl_dep_on_table(table_map tab_map) { return false; }
  /* 
    TRUE if the expression depends only on grouping fields of sel
    or can be converted to such an exression using equalities.
    Not to be used for AND/OR formulas.
  */
  virtual bool excl_dep_on_grouping_fields(st_select_lex *sel) { return false; }

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
  virtual bool check_partition_func_processor(void *arg) { return 1;}
  virtual bool post_fix_fields_part_expr_processor(void *arg) { return 0; }
  virtual bool rename_fields_processor(void *arg) { return 0; }
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
  virtual bool check_field_expression_processor(void *arg) { return 0; }
  virtual bool check_func_default_processor(void *arg) { return 0; }
  /*
    Check if an expression value has allowed arguments, like DATE/DATETIME
    for date functions. Also used by partitioning code to reject
    timezone-dependent expressions in a (sub)partitioning function.
  */
  virtual bool check_valid_arguments_processor(void *arg) { return 0; }
  virtual bool update_vcol_processor(void *arg) { return 0; }
  /*============== End of Item processor list ======================*/

  virtual Item *get_copy(THD *thd, MEM_ROOT *mem_root)=0;

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
  virtual uint cols() { return 1; }
  virtual Item* element_index(uint i) { return this; }
  virtual Item** addr(uint i) { return 0; }
  virtual bool check_cols(uint c);
  // It is not row => null inside is impossible
  virtual bool null_inside() { return 0; }
  // used in row subselects to get value of elements
  virtual void bring_value() {}

  virtual Field *create_tmp_field(bool group, TABLE *table)
  {
    /*
      Values with MY_INT32_NUM_DECIMAL_DIGITS digits may or may not fit into
      Field_long : make them Field_longlong.
    */
    return create_tmp_field(false, table, MY_INT32_NUM_DECIMAL_DIGITS - 2);
  }

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
  virtual Item *derived_grouping_field_transformer_for_where(THD *thd,
                                                             uchar *arg)
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
    my_error(ER_NONUPDATEABLE_COLUMN, MYF(0), name);
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
  virtual Field::geometry_type get_geometry_type() const
    { return Field::GEOM_GEOMETRY; };
  String *check_well_formed_result(String *str, bool send_error= 0);
  bool eq_by_collation(Item *item, bool binary_cmp, CHARSET_INFO *cs); 
  uint32 max_char_length() const
  { return max_length / collation.collation->mbmaxlen; }
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
  virtual bool is_outer_field() const { DBUG_ASSERT(fixed); return FALSE; }

  /**
    Checks if this item or any of its descendents contains a subquery.
  */
  virtual bool has_subquery() const { return with_subselect; }

  Item* set_expr_cache(THD *thd);

  virtual Item_equal *get_item_equal() { return NULL; }
  virtual void set_item_equal(Item_equal *item_eq) {};
  virtual Item_equal *find_item_equal(COND_EQUAL *cond_equal) { return NULL; }
  /**
    Set the join tab index to the minimal (left-most) JOIN_TAB to which this
    Item is attached. The number is an index is depth_first_tab() traversal
    order.
  */
  virtual void set_join_tab_idx(uint join_tab_idx_arg)
  {
    if (join_tab_idx_arg < join_tab_idx)
      join_tab_idx= join_tab_idx_arg;
  }
  virtual uint get_join_tab_idx() { return join_tab_idx; }

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
	

  void register_in(THD *thd);	 
  
  bool depends_only_on(table_map view_map) 
  { return marker & FULL_EXTRACTION_FL; }
  int get_extraction_flag()
  { return  marker & EXTRACTION_MASK; }
  void set_extraction_flag(int flags) 
  { 
    marker &= ~EXTRACTION_MASK;
    marker|= flags; 
  }
  void clear_extraction_flag()
  {
    marker &= ~EXTRACTION_MASK;
  }
};


template <class T>
inline Item* get_item_copy (THD *thd, MEM_ROOT *mem_root, T* item)
{
  Item *copy= new (mem_root) T(*item);
  copy->register_in(thd);
  return copy;
}	


/**
  Compare two Items for List<Item>::add_unique()
*/

bool cmp_items(Item *a, Item *b);


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

class sp_head;
class Item_string;


/**
  A common class for Item_basic_constant and Item_param
*/
class Item_basic_value :public Item
{
  bool is_basic_value(const Item *item, Type type_arg) const
  {
    return item->basic_const_item() && item->type() == type_arg;
  }
  bool is_basic_value(Type type_arg) const
  {
    return basic_const_item() && type() == type_arg;
  }
  bool str_eq(const String *value,
              const String *other, CHARSET_INFO *cs, bool binary_cmp) const
  {
    return binary_cmp ?
      value->bin_eq(other) :
      collation.collation == cs && value->eq(other, collation.collation);
  }

protected:
  // Value metadata, e.g. to make string processing easier
  class Metadata: private MY_STRING_METADATA
  {
  public:
    Metadata(const String *str)
    {
      my_string_metadata_get(this, str->charset(), str->ptr(), str->length());
    }
    Metadata(const String *str, uint repertoire_arg)
    {
      MY_STRING_METADATA::repertoire= repertoire_arg;
      MY_STRING_METADATA::char_length= str->numchars();
    }
    uint repertoire() const { return MY_STRING_METADATA::repertoire; }
    size_t char_length() const { return MY_STRING_METADATA::char_length; }
  };
  void fix_charset_and_length_from_str_value(Derivation dv, Metadata metadata)
  {
    /*
      We have to have a different max_length than 'length' here to
      ensure that we get the right length if we do use the item
      to create a new table. In this case max_length must be the maximum
      number of chars for a string of this type because we in Create_field::
      divide the max_length with mbmaxlen).
    */
    collation.set(str_value.charset(), dv, metadata.repertoire());
    fix_char_length(metadata.char_length());
    decimals= NOT_FIXED_DEC;
  }
  void fix_charset_and_length_from_str_value(Derivation dv)
  {
    fix_charset_and_length_from_str_value(dv, Metadata(&str_value));
  }
  Item_basic_value(THD *thd): Item(thd) {}
  /*
    In the xxx_eq() methods below we need to cast off "const" to
    call val_xxx(). This is OK for Item_basic_constant and Item_param.
  */
  bool null_eq(const Item *item) const
  {
    DBUG_ASSERT(is_basic_value(NULL_ITEM));
    return item->type() == NULL_ITEM;
  }
  bool str_eq(const String *value, const Item *item, bool binary_cmp) const
  {
    DBUG_ASSERT(is_basic_value(STRING_ITEM));
    return is_basic_value(item, STRING_ITEM) &&
           str_eq(value, ((Item_basic_value*)item)->val_str(NULL),
                  item->collation.collation, binary_cmp);
  }
  bool real_eq(double value, const Item *item) const
  {
    DBUG_ASSERT(is_basic_value(REAL_ITEM));
    return is_basic_value(item, REAL_ITEM) &&
           value == ((Item_basic_value*)item)->val_real();
  }
  bool int_eq(longlong value, const Item *item) const
  {
    DBUG_ASSERT(is_basic_value(INT_ITEM));
    return is_basic_value(item, INT_ITEM) &&
           value == ((Item_basic_value*)item)->val_int() &&
           (value >= 0 || item->unsigned_flag == unsigned_flag);
  }
};


class Item_basic_constant :public Item_basic_value
{
  table_map used_table_map;
public:
  Item_basic_constant(THD *thd): Item_basic_value(thd), used_table_map(0) {};
  void set_used_tables(table_map map) { used_table_map= map; }
  table_map used_tables() const { return used_table_map; }
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  /* to prevent drop fixed flag (no need parent cleanup call) */
  void cleanup()
  {
    /*
      Restore the original field name as it might not have been allocated
      in the statement memory. If the name is auto generated, it must be
      done again between subsequent executions of a prepared statement.
    */
    if (orig_name)
      name= orig_name;
  }
};


/*****************************************************************************
  The class is a base class for representation of stored routine variables in
  the Item-hierarchy. There are the following kinds of SP-vars:
    - local variables (Item_splocal);
    - CASE expression (Item_case_expr);
*****************************************************************************/

class Item_sp_variable :public Item
{
protected:
  /*
    THD, which is stored in fix_fields() and is used in this_item() to avoid
    current_thd use.
  */
  THD *m_thd;

public:
  LEX_STRING m_name;

public:
#ifndef DBUG_OFF
  /*
    Routine to which this Item_splocal belongs. Used for checking if correct
    runtime context is used for variable handling.
  */
  sp_head *m_sp;
#endif

public:
  Item_sp_variable(THD *thd, char *sp_var_name_str, uint sp_var_name_length);

public:
  bool fix_fields(THD *thd, Item **);

  double val_real();
  longlong val_int();
  String *val_str(String *sp);
  my_decimal *val_decimal(my_decimal *decimal_value);
  bool is_null();

public:
  inline void make_field(THD *thd, Send_field *field);
  
  inline bool const_item() const;
  
  inline int save_in_field(Field *field, bool no_conversions);
  inline bool send(Protocol *protocol, String *str);
  bool check_vcol_func_processor(void *arg) 
  {
    return mark_unsupported_function(m_name.str, arg, VCOL_IMPOSSIBLE);
  }
}; 

/*****************************************************************************
  Item_sp_variable inline implementation.
*****************************************************************************/

inline void Item_sp_variable::make_field(THD *thd, Send_field *field)
{
  Item *it= this_item();

  if (name)
    it->set_name(thd, name, (uint) strlen(name), system_charset_info);
  else
    it->set_name(thd, m_name.str, (uint) m_name.length, system_charset_info);
  it->make_field(thd, field);
}

inline bool Item_sp_variable::const_item() const
{
  return TRUE;
}

inline int Item_sp_variable::save_in_field(Field *field, bool no_conversions)
{
  return this_item()->save_in_field(field, no_conversions);
}

inline bool Item_sp_variable::send(Protocol *protocol, String *str)
{
  return this_item()->send(protocol, str);
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
  uint m_var_idx;

  Type m_type;
public:
  Item_splocal(THD *thd, const LEX_STRING &sp_var_name, uint sp_var_idx,
               enum_field_types sp_var_type,
               uint pos_in_q= 0, uint len_in_q= 0);

  Item *this_item();
  const Item *this_item() const;
  Item **this_item_addr(THD *thd, Item **);

  virtual void print(String *str, enum_query_type query_type);

public:
  inline const LEX_STRING *my_name() const;

  inline uint get_var_idx() const;

  inline enum Type type() const;
  enum_field_types field_type() const
  { return Type_handler_hybrid_field_type::field_type(); }
  enum Item_result result_type () const
  { return Type_handler_hybrid_field_type::result_type(); }
  enum Item_result cmp_type () const
  { return Type_handler_hybrid_field_type::cmp_type(); }

private:
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it);

public:
  Item_splocal *get_item_splocal() { return this; }

  Rewritable_query_parameter *get_rewritable_query_parameter()
  { return this; }

  Settable_routine_parameter *get_settable_routine_parameter()
  { return this; }

  bool append_for_log(THD *thd, String *str);
  
  Item *get_copy(THD *thd, MEM_ROOT *mem_root) { return 0; }
};

/*****************************************************************************
  Item_splocal inline implementation.
*****************************************************************************/

inline const LEX_STRING *Item_splocal::my_name() const
{
  return &m_name;
}

inline uint Item_splocal::get_var_idx() const
{
  return m_var_idx;
}

inline enum Item::Type Item_splocal::type() const
{
  return m_type;
}

/*****************************************************************************
  A reference to case expression in SP, used in runtime.
*****************************************************************************/

class Item_case_expr :public Item_sp_variable
{
public:
  Item_case_expr(THD *thd, uint case_expr_id);

public:
  Item *this_item();
  const Item *this_item() const;
  Item **this_item_addr(THD *thd, Item **);

  inline enum Type type() const;
  inline Item_result result_type() const;
  enum_field_types field_type() const { return this_item()->field_type(); }

public:
  /*
    NOTE: print() is intended to be used from views and for debug.
    Item_case_expr can not occur in views, so here it is only for debug
    purposes.
  */
  virtual void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root) { return 0; }

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

inline Item_result Item_case_expr::result_type() const
{
  return this_item()->result_type();
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

class Item_name_const : public Item
{
  Item *value_item;
  Item *name_item;
  bool valid_args;
public:
  Item_name_const(THD *thd, Item *name_arg, Item *val);

  bool fix_fields(THD *, Item **);

  enum Type type() const;
  double val_real();
  longlong val_int();
  String *val_str(String *sp);
  my_decimal *val_decimal(my_decimal *);
  bool is_null();
  virtual void print(String *str, enum_query_type query_type);

  enum_field_types field_type() const
  {
    return value_item->field_type();
  }

  Item_result result_type() const
  {
    return value_item->result_type();
  }

  bool const_item() const
  {
    return TRUE;
  }

  int save_in_field(Field *field, bool no_conversions)
  {
    return  value_item->save_in_field(field, no_conversions);
  }

  bool send(Protocol *protocol, String *str)
  {
    return value_item->send(protocol, str);
  }
  bool check_vcol_func_processor(void *arg) 
  {
    return mark_unsupported_function("name_const()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_name_const>(thd, mem_root, this); }
};

class Item_num: public Item_basic_constant
{
public:
  Item_num(THD *thd): Item_basic_constant(thd) { collation.set_numeric(); }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs);
  bool check_partition_func_processor(void *int_arg) { return FALSE;}
};

#define NO_CACHED_FIELD_INDEX ((uint)(-1))

class st_select_lex;


class Item_result_field :public Item	/* Item with result field */
{
public:
  Field *result_field;				/* Save result here */
  Item_result_field(THD *thd): Item(thd), result_field(0) {}
  // Constructor used for Item_sum/Item_cond_and/or (see Item comment)
  Item_result_field(THD *thd, Item_result_field *item):
    Item(thd, item), result_field(item->result_field)
  {}
  ~Item_result_field() {}			/* Required with gcc 2.95 */
  Field *get_tmp_table_field() { return result_field; }
  /*
    This implementation of used_tables() used by Item_avg_field and
    Item_variance_field which work when only temporary table left, so theu
    return table map of the temporary table.
  */
  table_map used_tables() const { return 1; }
  void set_result_field(Field *field) { result_field= field; }
  bool is_result_field() { return true; }
  void save_in_result_field(bool no_conversions)
  {
    save_in_field(result_field, no_conversions);
  }
  void cleanup();
  bool check_vcol_func_processor(void *arg) { return FALSE;}
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
  const char *orig_db_name;
  const char *orig_table_name;
  const char *orig_field_name;

public:
  Name_resolution_context *context;
  const char *db_name;
  const char *table_name;
  const char *field_name;
  bool alias_name_used; /* true if item was resolved against alias */
  /* 
    Cached value of index for this field in table->field array, used by prep. 
    stmts for speeding up their re-execution. Holds NO_CACHED_FIELD_INDEX 
    if index value is not known.
  */
  uint cached_field_index;
  /*
    Cached pointer to table which contains this field, used for the same reason
    by prep. stmt. too in case then we have not-fully qualified field.
    0 - means no cached value.
  */
  TABLE_LIST *cached_table;
  st_select_lex *depended_from;
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
  Item_ident(THD *thd, Name_resolution_context *context_arg,
             const char *db_name_arg, const char *table_name_arg,
             const char *field_name_arg);
  Item_ident(THD *thd, Item_ident *item);
  Item_ident(THD *thd, TABLE_LIST *view_arg, const char *field_name_arg);
  const char *full_name() const;
  void cleanup();
  st_select_lex *get_depended_from() const;
  bool remove_dependence_processor(void * arg);
  virtual void print(String *str, enum_query_type query_type);
  virtual bool change_context_processor(void *cntx)
    { context= (Name_resolution_context *)cntx; return FALSE; }
  /**
    Collect outer references
  */
  virtual bool collect_outer_ref_processor(void *arg);
  friend bool insert_fields(THD *thd, Name_resolution_context *context,
                            const char *db_name,
                            const char *table_name, List_iterator<Item> *it,
                            bool any_privileges);
};


class Item_ident_for_show :public Item
{
public:
  Field *field;
  const char *db_name;
  const char *table_name;

  Item_ident_for_show(THD *thd, Field *par_field, const char *db_arg,
                      const char *table_name_arg):
    Item(thd), field(par_field), db_name(db_arg), table_name(table_name_arg)
  {}

  enum Type type() const { return FIELD_ITEM; }
  double val_real() { return field->val_real(); }
  longlong val_int() { return field->val_int(); }
  String *val_str(String *str) { return field->val_str(str); }
  my_decimal *val_decimal(my_decimal *dec) { return field->val_decimal(dec); }
  void make_field(THD *thd, Send_field *tmp_field);
  CHARSET_INFO *charset_for_protocol(void) const
  { return field->charset_for_protocol(); }
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  Item* get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_ident_for_show>(thd, mem_root, this); }
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
  uint have_privileges;
  /* field need any privileges (for VIEW creation) */
  bool any_privileges;
  Item_field(THD *thd, Name_resolution_context *context_arg,
             const char *db_arg,const char *table_name_arg,
	     const char *field_name_arg);
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
  enum Type type() const { return FIELD_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const;
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *);
  String *val_str(String*);
  void save_result(Field *to);
  double val_result();
  longlong val_int_result();
  String *str_result(String* tmp);
  my_decimal *val_decimal_result(my_decimal *);
  bool val_bool_result();
  bool is_null_result();
  bool send(Protocol *protocol, String *str_arg);
  Load_data_outvar *get_load_data_outvar()
  {
    return this;
  }
  bool load_data_set_null(THD *thd, const Load_data_param *param)
  {
    return field->load_data_set_null(thd);
  }
  bool load_data_set_value(THD *thd, const char *pos, uint length,
                           const Load_data_param *param)
  {
    field->load_data_set_value(pos, length, param->charset());
    return false;
  }
  bool load_data_set_no_data(THD *thd, const Load_data_param *param);
  void load_data_print_for_log_event(THD *thd, String *to) const;
  bool load_data_add_outvar(THD *thd, Load_data_param *param) const
  {
    return param->add_outvar_field(thd, field);
  }
  uint load_data_fixed_length() const
  {
    return field->field_length;
  }
  void reset_field(Field *f);
  bool fix_fields(THD *, Item **);
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge);
  void make_field(THD *thd, Send_field *tmp_field);
  int save_in_field(Field *field,bool no_conversions);
  void save_org_in_field(Field *field, fast_field_copier optimizer_data);
  fast_field_copier setup_fast_field_copier(Field *field);
  table_map used_tables() const;
  table_map all_used_tables() const; 
  enum Item_result result_type () const
  {
    return field->result_type();
  }
  Item_result cast_to_int_type() const
  {
    return field->cmp_type();
  }
  enum_field_types field_type() const
  {
    return field->type();
  }
  enum_monotonicity_info get_monotonicity_info() const
  {
    return MONOTONIC_STRICT_INCREASING;
  }
  Sql_mode_dependency value_depends_on_sql_mode() const
  {
    return Sql_mode_dependency(0, field->value_depends_on_sql_mode());
  }
  longlong val_int_endpoint(bool left_endp, bool *incl_endp);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  bool get_date_result(MYSQL_TIME *ltime,ulonglong fuzzydate);
  bool is_null() { return field->is_null(); }
  void update_null_value();
  void update_table_bitmaps()
  {
    if (field && field->table)
    {
      TABLE *tab= field->table;
      tab->covering_keys.intersect(field->part_of_key);
      if (tab->read_set)
        bitmap_fast_test_and_set(tab->read_set, field->field_index);
      /* 
        Do not mark a self-referecing virtual column.
        Such virtual columns are reported as invalid.
      */
      if (field->vcol_info && tab->vcol_set)
        tab->mark_virtual_col(field);
    }
  }
  void update_used_tables()
  {
    update_table_bitmaps();
  }
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref)
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
  bool is_result_field() { return false; }
  void set_result_field(Field *field_arg) {}
  void save_in_result_field(bool no_conversions) { }
  Item *get_tmp_table_item(THD *thd);
  bool collect_item_field_processor(void * arg);
  bool add_field_to_set_processor(void * arg);
  bool find_item_in_field_list_processor(void *arg);
  bool register_field_in_read_map(void *arg);
  bool register_field_in_write_map(void *arg);
  bool register_field_in_bitmap(void *arg);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool post_fix_fields_part_expr_processor(void *bool_arg);
  bool check_valid_arguments_processor(void *bool_arg);
  bool check_field_expression_processor(void *arg);
  bool enumerate_field_refs_processor(void *arg);
  bool update_table_bitmaps_processor(void *arg);
  bool switch_to_nullable_fields_processor(void *arg);
  bool update_vcol_processor(void *arg);
  bool rename_fields_processor(void *arg);
  bool check_vcol_func_processor(void *arg)
  {
    context= 0;
    if (field && (field->unireg_check == Field::NEXT_NUMBER))
    {
      // Auto increment fields are unsupported
      return mark_unsupported_function(field_name, arg, VCOL_FIELD_REF | VCOL_AUTO_INC);
    }
    return mark_unsupported_function(field_name, arg, VCOL_FIELD_REF);
  }
  void cleanup();
  Item_equal *get_item_equal() { return item_equal; }
  void set_item_equal(Item_equal *item_eq) { item_equal= item_eq; }
  Item_equal *find_item_equal(COND_EQUAL *cond_equal);
  Item* propagate_equal_fields(THD *, const Context &, COND_EQUAL *);
  Item *replace_equal_field(THD *thd, uchar *arg);
  inline uint32 max_disp_length() { return field->max_display_length(); }
  Item_field *field_for_view_update() { return this; }
  int fix_outer_field(THD *thd, Field **field, Item **reference);
  virtual Item *update_value_transformer(THD *thd, uchar *select_arg);
  Item *derived_field_transformer_for_having(THD *thd, uchar *arg);
  Item *derived_field_transformer_for_where(THD *thd, uchar *arg);
  Item *derived_grouping_field_transformer_for_where(THD *thd, uchar *arg);
  virtual void print(String *str, enum_query_type query_type);
  bool excl_dep_on_table(table_map tab_map);
  bool excl_dep_on_grouping_fields(st_select_lex *sel);
  bool cleanup_excluding_fields_processor(void *arg)
  { return field ? 0 : cleanup_processor(arg); }
  bool cleanup_excluding_const_fields_processor(void *arg)
  { return field && const_item() ? 0 : cleanup_processor(arg); }
  
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_field>(thd, mem_root, this); }
  bool is_outer_field() const
  {
    DBUG_ASSERT(fixed);
    return field->table->pos_in_table_list->outer_join;
  }
  Field::geometry_type get_geometry_type() const
  {
    DBUG_ASSERT(field_type() == MYSQL_TYPE_GEOMETRY);
    return field->get_geometry_type();
  }
  CHARSET_INFO *charset_for_protocol(void) const
  { return field->charset_for_protocol(); }
  friend class Item_default_value;
  friend class Item_insert_value;
  friend class st_select_lex_unit;
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

  virtual void print(String *str, enum_query_type query_type);
};


class Item_null :public Item_basic_constant
{
public:
  Item_null(THD *thd, char *name_par=0, CHARSET_INFO *cs= &my_charset_bin):
    Item_basic_constant(thd)
  {
    maybe_null= null_value= TRUE;
    max_length= 0;
    name= name_par ? name_par : (char*) "NULL";
    fixed= 1;
    collation.set(cs, DERIVATION_IGNORABLE, MY_REPERTOIRE_ASCII);
  }
  enum Type type() const { return NULL_ITEM; }
  bool vcol_assignment_allowed_value() const { return true; }
  bool eq(const Item *item, bool binary_cmp) const { return null_eq(item); }
  double val_real();
  longlong val_int();
  String *val_str(String *str);
  my_decimal *val_decimal(my_decimal *);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  longlong val_datetime_packed();
  longlong val_time_packed();
  int save_in_field(Field *field, bool no_conversions);
  int save_safe_in_field(Field *field);
  bool send(Protocol *protocol, String *str);
  enum Item_result result_type () const { return STRING_RESULT; }
  enum_field_types field_type() const   { return MYSQL_TYPE_NULL; }
  bool basic_const_item() const { return 1; }
  Item *clone_item(THD *thd);
  bool is_null() { return 1; }

  virtual inline void print(String *str, enum_query_type query_type)
  {
    str->append(STRING_WITH_LEN("NULL"));
  }

  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_null>(thd, mem_root, this); }
};

class Item_null_result :public Item_null
{
public:
  Field *result_field;
  Item_null_result(THD *thd): Item_null(thd), result_field(0) {}
  bool is_result_field() { return result_field != 0; }
#if MARIADB_VERSION_ID < 100300
  enum_field_types field_type() const
  {
    return result_field->type();
  }
  CHARSET_INFO *charset_for_protocol(void) const
  {
    return collation.collation;
  }
#else
  const Type_handler *type_handler() const
  {
    return result_field->type_handler();
  }
#endif
  void save_in_result_field(bool no_conversions)
  {
    save_in_field(result_field, no_conversions);
  }
  bool check_partition_func_processor(void *int_arg) {return TRUE;}
  bool check_vcol_func_processor(void *arg)
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
*/

class Item_param :public Item_basic_value,
                  private Settable_routine_parameter,
                  public Rewritable_query_parameter,
                  public Type_handler_hybrid_field_type
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
      - Item::type() returns NULL_ITEM, INT_ITEM, REAL_ITEM, DECIMAL_ITEM,
        DATE_ITEM, STRING_ITEM, depending on the value assigned.
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
    NO_VALUE, NULL_VALUE, INT_VALUE, REAL_VALUE,
    STRING_VALUE, TIME_VALUE, LONG_DATA_VALUE,
    DECIMAL_VALUE, DEFAULT_VALUE, IGNORE_VALUE
  } state;

  enum Type item_type;

  void fix_type(Type type)
  {
    item_type= type;
    fixed= true;
  }

  void fix_temporal(uint32 max_length_arg, uint decimals_arg);

public:
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

  /*
    Used for bulk protocol only.
  */
  enum enum_indicator_type indicator;

  bool vcol_assignment_allowed_value() const
  {
    switch (state) {
    case NULL_VALUE:
    case DEFAULT_VALUE:
    case IGNORE_VALUE:
      return true;
    case NO_VALUE:
    case INT_VALUE:
    case REAL_VALUE:
    case STRING_VALUE:
    case TIME_VALUE:
    case LONG_DATA_VALUE:
    case DECIMAL_VALUE:
      break;
    }
    return false;
  }

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
  String str_value_ptr;
  my_decimal decimal_value;
  union
  {
    longlong integer;
    double   real;
    CONVERSION_INFO cs_info;
    MYSQL_TIME     time;
  } value;

  enum_field_types field_type() const
  { return Type_handler_hybrid_field_type::field_type(); }
  enum Item_result result_type () const
  { return Type_handler_hybrid_field_type::result_type(); }
  enum Item_result cmp_type () const
  { return Type_handler_hybrid_field_type::cmp_type(); }

  Item_param(THD *thd, uint pos_in_query_arg);

  enum Type type() const
  {
    return item_type;
  }

  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal*);
  String *val_str(String*);
  bool get_date(MYSQL_TIME *tm, ulonglong fuzzydate);
  int  save_in_field(Field *field, bool no_conversions);

  void set_default();
  void set_ignore();
  void set_null();
  void set_int(longlong i, uint32 max_length_arg);
  void set_double(double i);
  void set_decimal(const char *str, ulong length);
  void set_decimal(const my_decimal *dv, bool unsigned_arg);
  bool set_str(const char *str, ulong length);
  bool set_longdata(const char *str, ulong length);
  void set_time(MYSQL_TIME *tm, timestamp_type type, uint32 max_length_arg);
  void set_time(const MYSQL_TIME *tm, uint32 max_length_arg, uint decimals_arg);
  bool set_from_item(THD *thd, Item *item);
  void reset();
  /*
    Assign placeholder value from bind data.
    Note, that 'len' has different semantics in embedded library (as we
    don't need to check that packet is not broken there). See
    sql_prepare.cc for details.
  */
  void (*set_param_func)(Item_param *param, uchar **pos, ulong len);

  const String *query_val_str(THD *thd, String *str) const;

  bool convert_str_value(THD *thd);

  /*
    If value for parameter was not set we treat it as non-const
    so no one will use parameters value in fix_fields still
    parameter is constant during execution.
  */
  virtual table_map used_tables() const
  { return state != NO_VALUE ? (table_map)0 : PARAM_TABLE_BIT; }
  virtual void print(String *str, enum_query_type query_type);
  bool is_null()
  { DBUG_ASSERT(state != NO_VALUE); return state == NULL_VALUE; }
  bool basic_const_item() const;
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
    return state == INT_VALUE;
  }
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
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs);
  Item *clone_item(THD *thd);
  /*
    Implement by-value equality evaluation if parameter value
    is set and is a basic constant (integer, real or string).
    Otherwise return FALSE.
  */
  bool eq(const Item *item, bool binary_cmp) const;
  void set_param_type_and_swap_value(Item_param *from);

  Rewritable_query_parameter *get_rewritable_query_parameter()
  { return this; }
  Settable_routine_parameter *get_settable_routine_parameter()
  { return m_is_settable_routine_parameter ? this : NULL; }

  bool append_for_log(THD *thd, String *str);
  bool check_vcol_func_processor(void *int_arg) {return FALSE;}
  Item *get_copy(THD *thd, MEM_ROOT *mem_root) { return 0; }

  bool add_as_clone(THD *thd);
  void sync_clones();
  bool register_clone(Item_param *i) { return m_clones.push_back(i); }

private:
  void invalid_default_param() const;

  virtual bool set_value(THD *thd, sp_rcontext *ctx, Item **it);

  virtual void set_out_param_info(Send_field *info);

public:
  virtual const Send_field *get_out_param_info() const;

  virtual void make_field(THD *thd, Send_field *field);

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
  Item_int(THD *thd, int32 i,uint length= MY_INT32_NUM_DECIMAL_DIGITS):
    Item_num(thd), value((longlong) i)
    { max_length=length; fixed= 1; }
  Item_int(THD *thd, longlong i,uint length= MY_INT64_NUM_DECIMAL_DIGITS):
    Item_num(thd), value(i)
    { max_length=length; fixed= 1; }
  Item_int(THD *thd, ulonglong i, uint length= MY_INT64_NUM_DECIMAL_DIGITS):
    Item_num(thd), value((longlong)i)
    { max_length=length; fixed= 1; unsigned_flag= 1; }
  Item_int(THD *thd, const char *str_arg,longlong i,uint length):
    Item_num(thd), value(i)
    { max_length=length; name=(char*) str_arg; fixed= 1; }
  Item_int(THD *thd, const char *str_arg, uint length=64);
  enum Type type() const { return INT_ITEM; }
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  longlong val_int() { DBUG_ASSERT(fixed == 1); return value; }
  double val_real() { DBUG_ASSERT(fixed == 1); return (double) value; }
  my_decimal *val_decimal(my_decimal *);
  String *val_str(String*);
  int save_in_field(Field *field, bool no_conversions);
  bool basic_const_item() const { return 1; }
  Item *clone_item(THD *thd);
  virtual void print(String *str, enum_query_type query_type);
  Item *neg(THD *thd);
  uint decimal_precision() const
  { return (uint) (max_length - MY_TEST(value < 0)); }
  bool eq(const Item *item, bool binary_cmp) const
  { return int_eq(value, item); }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_int>(thd, mem_root, this); }
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
  bool is_bool_type() { return true; }
  Item *neg_transformer(THD *thd);
};


class Item_uint :public Item_int
{
public:
  Item_uint(THD *thd, const char *str_arg, uint length);
  Item_uint(THD *thd, ulonglong i): Item_int(thd, i, 10) {}
  Item_uint(THD *thd, const char *str_arg, longlong i, uint length);
  double val_real()
    { DBUG_ASSERT(fixed == 1); return ulonglong2double((ulonglong)value); }
  String *val_str(String*);
  Item *clone_item(THD *thd);
  virtual void print(String *str, enum_query_type query_type);
  Item *neg(THD *thd);
  uint decimal_precision() const { return max_length; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_uint>(thd, mem_root, this); }
};


class Item_datetime :public Item_int
{
protected:
  MYSQL_TIME ltime;
public:
  Item_datetime(THD *thd): Item_int(thd, 0) { unsigned_flag=0; }
  int save_in_field(Field *field, bool no_conversions);
  longlong val_int();
  double val_real() { return (double)val_int(); }
  void set(longlong packed);
};


/* decimal (fixed point) constant */
class Item_decimal :public Item_num
{
protected:
  my_decimal decimal_value;
public:
  Item_decimal(THD *thd, const char *str_arg, uint length,
               CHARSET_INFO *charset);
  Item_decimal(THD *thd, const char *str, const my_decimal *val_arg,
               uint decimal_par, uint length);
  Item_decimal(THD *thd, my_decimal *value_par);
  Item_decimal(THD *thd, longlong val, bool unsig);
  Item_decimal(THD *thd, double val, int precision, int scale);
  Item_decimal(THD *thd, const uchar *bin, int precision, int scale);

  enum Type type() const { return DECIMAL_ITEM; }
  enum Item_result result_type () const { return DECIMAL_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_NEWDECIMAL; }
  longlong val_int();
  double val_real();
  String *val_str(String*);
  my_decimal *val_decimal(my_decimal *val) { return &decimal_value; }
  int save_in_field(Field *field, bool no_conversions);
  bool basic_const_item() const { return 1; }
  Item *clone_item(THD *thd);
  virtual void print(String *str, enum_query_type query_type);
  Item *neg(THD *thd);
  uint decimal_precision() const { return decimal_value.precision(); }
  bool eq(const Item *, bool binary_cmp) const;
  void set_decimal_value(my_decimal *value_par);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_decimal>(thd, mem_root, this); }
};


class Item_float :public Item_num
{
  char *presentation;
public:
  double value;
  Item_float(THD *thd, const char *str_arg, uint length);
  Item_float(THD *thd, const char *str, double val_arg, uint decimal_par,
             uint length): Item_num(thd), value(val_arg)
  {
    presentation= name=(char*) str;
    decimals=(uint8) decimal_par;
    max_length=length;
    fixed= 1;
  }
  Item_float(THD *thd, double value_par, uint decimal_par):
    Item_num(thd), presentation(0), value(value_par)
  {
    decimals= (uint8) decimal_par;
    fixed= 1;
  }
  int save_in_field(Field *field, bool no_conversions);
  enum Type type() const { return REAL_ITEM; }
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  double val_real() { DBUG_ASSERT(fixed == 1); return value; }
  longlong val_int()
  {
    DBUG_ASSERT(fixed == 1);
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
  String *val_str(String*);
  my_decimal *val_decimal(my_decimal *);
  bool basic_const_item() const { return 1; }
  Item *clone_item(THD *thd);
  Item *neg(THD *thd);
  virtual void print(String *str, enum_query_type query_type);
  bool eq(const Item *item, bool binary_cmp) const
  { return real_eq(value, item); }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_float>(thd, mem_root, this); }
};


class Item_static_float_func :public Item_float
{
  const char *func_name;
public:
  Item_static_float_func(THD *thd, const char *str, double val_arg,
                         uint decimal_par, uint length):
    Item_float(thd, NullS, val_arg, decimal_par, length), func_name(str)
  {}

  virtual inline void print(String *str, enum_query_type query_type)
  {
    str->append(func_name);
  }

  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
  {
    return const_charset_converter(thd, tocs, true, func_name);
  }
};


class Item_string :public Item_basic_constant
{
protected:
  void fix_from_value(Derivation dv, const Metadata metadata)
  {
    fix_charset_and_length_from_str_value(dv, metadata);
    // it is constant => can be used without fix_fields (and frequently used)
    fixed= 1;
  }
  void fix_and_set_name_from_value(THD *thd, Derivation dv,
                                   const Metadata metadata)
  {
    fix_from_value(dv, metadata);
    set_name(thd, str_value.ptr(), str_value.length(), str_value.charset());
  }
protected:
  /* Just create an item and do not fill string representation */
  Item_string(THD *thd, CHARSET_INFO *cs, Derivation dv= DERIVATION_COERCIBLE):
    Item_basic_constant(thd)
  {
    collation.set(cs, dv);
    max_length= 0;
    set_name(thd, NULL, 0, system_charset_info);
    decimals= NOT_FIXED_DEC;
    fixed= 1;
  }
public:
  Item_string(THD *thd, CHARSET_INFO *csi, const char *str_arg, uint length_arg):
    Item_basic_constant(thd)
  {
    collation.set(csi, DERIVATION_COERCIBLE);
    set_name(thd, NULL, 0, system_charset_info);
    decimals= NOT_FIXED_DEC;
    fixed= 1;
    str_value.copy(str_arg, length_arg, csi);
    max_length= str_value.numchars() * csi->mbmaxlen;
  }
  // Constructors with the item name set from its value
  Item_string(THD *thd, const char *str, uint length, CHARSET_INFO *cs,
              Derivation dv, uint repertoire): Item_basic_constant(thd)
  {
    str_value.set_or_copy_aligned(str, length, cs);
    fix_and_set_name_from_value(thd, dv, Metadata(&str_value, repertoire));
  }
  Item_string(THD *thd, const char *str, uint length,
              CHARSET_INFO *cs, Derivation dv= DERIVATION_COERCIBLE):
    Item_basic_constant(thd)
  {
    str_value.set_or_copy_aligned(str, length, cs);
    fix_and_set_name_from_value(thd, dv, Metadata(&str_value));
  }
  Item_string(THD *thd, const String *str, CHARSET_INFO *tocs, uint *conv_errors,
              Derivation dv, uint repertoire): Item_basic_constant(thd)
  {
    if (str_value.copy(str, tocs, conv_errors))
      str_value.set("", 0, tocs); // EOM ?
    str_value.mark_as_const();
    fix_and_set_name_from_value(thd, dv, Metadata(&str_value, repertoire));
  }
  // Constructors with an externally provided item name
  Item_string(THD *thd, const char *name_par, const char *str, uint length,
              CHARSET_INFO *cs, Derivation dv= DERIVATION_COERCIBLE):
    Item_basic_constant(thd)
  {
    str_value.set_or_copy_aligned(str, length, cs);
    fix_from_value(dv, Metadata(&str_value));
    set_name(thd, name_par, 0, system_charset_info);
  }
  Item_string(THD *thd, const char *name_par, const char *str, uint length,
              CHARSET_INFO *cs, Derivation dv, uint repertoire):
    Item_basic_constant(thd)
  {
    str_value.set_or_copy_aligned(str, length, cs);
    fix_from_value(dv, Metadata(&str_value, repertoire));
    set_name(thd, name_par, 0, system_charset_info);
  }
  void print_value(String *to) const
  {
    str_value.print(to);
  }
  enum Type type() const { return STRING_ITEM; }
  double val_real();
  longlong val_int();
  String *val_str(String*)
  {
    DBUG_ASSERT(fixed == 1);
    return (String*) &str_value;
  }
  my_decimal *val_decimal(my_decimal *);
  int save_in_field(Field *field, bool no_conversions);
  enum Item_result result_type () const { return STRING_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  bool basic_const_item() const { return 1; }
  bool eq(const Item *item, bool binary_cmp) const
  {
    return str_eq(&str_value, item, binary_cmp);
  }
  Item *clone_item(THD *thd);
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
  {
    return const_charset_converter(thd, tocs, true);
  }
  inline void append(char *str, uint length)
  {
    str_value.append(str, length);
    max_length= str_value.numchars() * collation.collation->mbmaxlen;
  }
  virtual void print(String *str, enum_query_type query_type);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}

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

  enum_field_types odbc_temporal_literal_type(const LEX_STRING *type_str) const
  {
    /*
      If string is a reasonably short pure ASCII string literal,
      try to parse known ODBC style date, time or timestamp literals,
      e.g:
      SELECT {d'2001-01-01'};
      SELECT {t'10:20:30'};
      SELECT {ts'2001-01-01 10:20:30'};
    */
    if (collation.repertoire == MY_REPERTOIRE_ASCII &&
        str_value.length() < MAX_DATE_STRING_REP_LENGTH * 4)
    {
      if (type_str->length == 1)
      {
        if (type_str->str[0] == 'd')  /* {d'2001-01-01'} */
          return MYSQL_TYPE_DATE;
        else if (type_str->str[0] == 't') /* {t'10:20:30'} */
          return MYSQL_TYPE_TIME;
      }
      else if (type_str->length == 2) /* {ts'2001-01-01 10:20:30'} */
      {
        if (type_str->str[0] == 't' && type_str->str[1] == 's')
          return MYSQL_TYPE_DATETIME;
      }
    }
    return MYSQL_TYPE_STRING; // Not a temporal literal
  }
  
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_string>(thd, mem_root, this); }

};


class Item_string_with_introducer :public Item_string
{
public:
  Item_string_with_introducer(THD *thd, const char *str, uint length,
                              CHARSET_INFO *cs):
    Item_string(thd, str, length, cs)
  { }
  Item_string_with_introducer(THD *thd, const char *name_arg,
                              const char *str, uint length, CHARSET_INFO *tocs):
    Item_string(thd, name_arg, str, length, tocs)
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
    Item_string(thd, str, (uint)strlen(str), &my_charset_latin1,
                DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII)
  { }
};


class Item_static_string_func :public Item_string
{
  const char *func_name;
public:
  Item_static_string_func(THD *thd, const char *name_par, const char *str,
                          uint length, CHARSET_INFO *cs,
                          Derivation dv= DERIVATION_COERCIBLE):
    Item_string(thd, NullS, str, length, cs, dv), func_name(name_par)
  {}
  Item_static_string_func(THD *thd, const char *name_par,
                          const String *str,
                          CHARSET_INFO *tocs, uint *conv_errors,
                          Derivation dv, uint repertoire):
    Item_string(thd, str, tocs, conv_errors, dv, repertoire),
    func_name(name_par)
  {}
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
  {
    return const_charset_converter(thd, tocs, true, func_name);
  }

  virtual inline void print(String *str, enum_query_type query_type)
  {
    str->append(func_name);
  }

  bool check_partition_func_processor(void *int_arg) {return TRUE;}

  bool check_vcol_func_processor(void *arg)
  { // VCOL_TIME_FUNC because the value is not constant, but does not
    // require fix_fields() to be re-run for every statement.
    return mark_unsupported_function(func_name, arg, VCOL_TIME_FUNC);
  }
};


/* for show tables */
class Item_partition_func_safe_string: public Item_string
{
public:
  Item_partition_func_safe_string(THD *thd, const char *name_arg, uint length,
                                  CHARSET_INFO *cs= NULL):
    Item_string(thd, name_arg, length, cs)
  {}
  bool check_vcol_func_processor(void *arg) 
  {
    return mark_unsupported_function("safe_string", arg, VCOL_IMPOSSIBLE);
  }
};


class Item_return_date_time :public Item_partition_func_safe_string
{
  enum_field_types date_time_field_type;
public:
  Item_return_date_time(THD *thd, const char *name_arg, uint length_arg,
                        enum_field_types field_type_arg):
    Item_partition_func_safe_string(thd, name_arg, length_arg, &my_charset_bin),
    date_time_field_type(field_type_arg)
  { decimals= 0; }
  enum_field_types field_type() const { return date_time_field_type; }
};


class Item_blob :public Item_partition_func_safe_string
{
public:
  Item_blob(THD *thd, const char *name_arg, uint length):
    Item_partition_func_safe_string(thd, name_arg, (uint) strlen(name_arg), &my_charset_bin)
  { max_length= length; }
  enum Type type() const { return TYPE_HOLDER; }
  enum_field_types field_type() const { return MYSQL_TYPE_BLOB; }
  Field *create_field_for_schema(THD *thd, TABLE *table)
  { return tmp_table_field_from_field_type(table, false, true); }
};


/**
  Item_empty_string -- is a utility class to put an item into List<Item>
  which is then used in protocol.send_result_set_metadata() when sending SHOW output to
  the client.
*/

class Item_empty_string :public Item_partition_func_safe_string
{
public:
  Item_empty_string(THD *thd, const char *header,uint length,
                    CHARSET_INFO *cs= NULL):
    Item_partition_func_safe_string(thd, "", 0,
                                    cs ? cs : &my_charset_utf8_general_ci)
    { name=(char*) header; max_length= length * collation.collation->mbmaxlen; }
  void make_field(THD *thd, Send_field *field);
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
  enum_field_types field_type() const { return int_field_type; }
};


/**
  Item_hex_constant -- a common class for hex literals: X'HHHH' and 0xHHHH
*/
class Item_hex_constant: public Item_basic_constant
{
private:
  void hex_string_init(THD *thd, const char *str, uint str_length);
public:
  Item_hex_constant(THD *thd): Item_basic_constant(thd)
  {
    hex_string_init(thd, "", 0);
  }
  Item_hex_constant(THD *thd, const char *str, uint str_length):
    Item_basic_constant(thd)
  {
    hex_string_init(thd, str, str_length);
  }
  enum Type type() const { return VARBIN_ITEM; }
  enum Item_result result_type () const { return STRING_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  virtual Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
  {
    return const_charset_converter(thd, tocs, true);
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool basic_const_item() const { return 1; }
  bool eq(const Item *item, bool binary_cmp) const
  {
    return item->basic_const_item() && item->type() == type() &&
           item->cast_to_int_type() == cast_to_int_type() &&
           str_value.bin_eq(&((Item_hex_constant*)item)->str_value);
  }
  String *val_str(String*) { DBUG_ASSERT(fixed == 1); return &str_value; }
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
  Item_hex_hybrid(THD *thd, const char *str, uint str_length):
    Item_hex_constant(thd, str, str_length) {}
  double val_real()
  { 
    DBUG_ASSERT(fixed == 1); 
    return (double) (ulonglong) Item_hex_hybrid::val_int();
  }
  longlong val_int()
  {
    // following assert is redundant, because fixed=1 assigned in constructor
    DBUG_ASSERT(fixed == 1);
    return longlong_from_hex_hybrid(str_value.ptr(), str_value.length());
  }
  my_decimal *val_decimal(my_decimal *decimal_value)
  {
    // following assert is redundant, because fixed=1 assigned in constructor
    DBUG_ASSERT(fixed == 1);
    ulonglong value= (ulonglong) Item_hex_hybrid::val_int();
    int2my_decimal(E_DEC_FATAL_ERROR, value, TRUE, decimal_value);
    return decimal_value;
  }
  int save_in_field(Field *field, bool no_conversions)
  {
    field->set_notnull();
    return field->store_hex_hybrid(str_value.ptr(), str_value.length());
  }
  enum Item_result cast_to_int_type() const { return INT_RESULT; }
  void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_hex_hybrid>(thd, mem_root, this); }
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
  Item_hex_string(THD *thd, const char *str, uint str_length):
    Item_hex_constant(thd, str, str_length) {}
  longlong val_int()
  {
    DBUG_ASSERT(fixed == 1);
    return longlong_from_string_with_check(&str_value);
  }
  double val_real()
  { 
    DBUG_ASSERT(fixed == 1);
    return double_from_string_with_check(&str_value);
  }
  my_decimal *val_decimal(my_decimal *decimal_value)
  {
    return val_decimal_from_string(decimal_value);
  }
  int save_in_field(Field *field, bool no_conversions)
  {
    field->set_notnull();
    return field->store(str_value.ptr(), str_value.length(), 
                        collation.collation);
  }
  enum Item_result cast_to_int_type() const { return STRING_RESULT; }
  void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_hex_string>(thd, mem_root, this); }
};


class Item_bin_string: public Item_hex_hybrid
{
public:
  Item_bin_string(THD *thd, const char *str,uint str_length);
};


class Item_temporal_literal :public Item_basic_constant
{
protected:
  MYSQL_TIME cached_time;
public:
  /**
    Constructor for Item_date_literal.
    @param ltime  DATE value.
  */
  Item_temporal_literal(THD *thd, MYSQL_TIME *ltime): Item_basic_constant(thd)
  {
    collation.set(&my_charset_numeric, DERIVATION_NUMERIC, MY_REPERTOIRE_ASCII);
    decimals= 0;
    cached_time= *ltime;
  }
  Item_temporal_literal(THD *thd, MYSQL_TIME *ltime, uint dec_arg):
    Item_basic_constant(thd)
  {
    collation.set(&my_charset_numeric, DERIVATION_NUMERIC, MY_REPERTOIRE_ASCII);
    decimals= dec_arg;
    cached_time= *ltime;
  }
  bool basic_const_item() const { return true; }
  bool const_item() const { return true; }
  enum Type type() const { return DATE_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const;
  enum Item_result result_type () const { return STRING_RESULT; }
  Item_result cmp_type() const { return TIME_RESULT; }

  bool check_partition_func_processor(void *int_arg) {return FALSE;}

  bool is_null()
  { return is_null_from_temporal(); }
  bool get_date_with_sql_mode(MYSQL_TIME *to);
  String *val_str(String *str)
  { return val_string_from_date(str); }
  longlong val_int()
  { return val_int_from_date(); }
  double val_real()
  { return val_real_from_date(); }
  my_decimal *val_decimal(my_decimal *decimal_value)
  { return  val_decimal_from_date(decimal_value); }
  int save_in_field(Field *field, bool no_conversions)
  { return save_date_in_field(field); }
};


/**
  DATE'2010-01-01'
*/
class Item_date_literal: public Item_temporal_literal
{
public:
  Item_date_literal(THD *thd, MYSQL_TIME *ltime)
    :Item_temporal_literal(thd, ltime)
  {
    max_length= MAX_DATE_WIDTH;
    fixed= 1;
    /*
      If date has zero month or day, it can return NULL in case of
      NO_ZERO_DATE or NO_ZERO_IN_DATE.
      We can't just check the current sql_mode here in constructor,
      because sql_mode can change in case of prepared statements
      between PREPARE and EXECUTE.
    */
    maybe_null= !ltime->month || !ltime->day;
  }
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
  void print(String *str, enum_query_type query_type);
  Item *clone_item(THD *thd);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_date_literal>(thd, mem_root, this); }
};


/**
  TIME'10:10:10'
*/
class Item_time_literal: public Item_temporal_literal
{
public:
  Item_time_literal(THD *thd, MYSQL_TIME *ltime, uint dec_arg):
    Item_temporal_literal(thd, ltime, dec_arg)
  {
    max_length= MIN_TIME_WIDTH + (decimals ? decimals + 1 : 0);
    fixed= 1;
  }
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
  void print(String *str, enum_query_type query_type);
  Item *clone_item(THD *thd);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_time_literal>(thd, mem_root, this); }
};


/**
  TIMESTAMP'2001-01-01 10:20:30'
*/
class Item_datetime_literal: public Item_temporal_literal
{
public:
  Item_datetime_literal(THD *thd, MYSQL_TIME *ltime, uint dec_arg):
    Item_temporal_literal(thd, ltime, dec_arg)
  {
    max_length= MAX_DATETIME_WIDTH + (decimals ? decimals + 1 : 0);
    fixed= 1;
    // See the comment on maybe_null in Item_date_literal
    maybe_null= !ltime->month || !ltime->day;
  }
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
  void print(String *str, enum_query_type query_type);
  Item *clone_item(THD *thd);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_datetime_literal>(thd, mem_root, this); }
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

    This is done to make the eqial field propagation code handle mixtures of
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
  Item_date_literal_for_invalid_dates(THD *thd, MYSQL_TIME *ltime)
   :Item_date_literal(thd, ltime) { }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date)
  {
    *ltime= cached_time;
    return (null_value= false);
  }
};


/**
  An error-safe counterpart for Item_datetime_literal
  (see Item_date_literal_for_invalid_dates for comments)
*/
class Item_datetime_literal_for_invalid_dates: public Item_datetime_literal
{
public:
  Item_datetime_literal_for_invalid_dates(THD *thd,
                                          MYSQL_TIME *ltime, uint dec_arg)
   :Item_datetime_literal(thd, ltime, dec_arg) { }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date)
  {
    *ltime= cached_time;
    return (null_value= false);
  }
};


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
  bool excl_dep_on_grouping_fields(st_select_lex *sel)
  {
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->const_item())
        continue;      
      if (!args[i]->excl_dep_on_grouping_fields(sel))
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
    if ((args= (Item**) thd_alloc(thd, sizeof(Item*) * 3)))
    {
      arg_count= 3;
      args[0]= a; args[1]= b; args[2]= c;
    }
  }
  Item_args(THD *thd, Item *a, Item *b, Item *c, Item *d)
  {
    arg_count= 0;
    if ((args= (Item**) thd_alloc(thd, sizeof(Item*) * 4)))
    {
      arg_count= 4;
      args[0]= a; args[1]= b; args[2]= c; args[3]= d;
    }
  }
  Item_args(THD *thd, Item *a, Item *b, Item *c, Item *d, Item* e)
  {
    arg_count= 5;
    if ((args= (Item**) thd_alloc(thd, sizeof(Item*) * 5)))
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
  inline Item **arguments() const { return args; }
  inline uint argument_count() const { return arg_count; }
  inline void remove_arguments() { arg_count=0; }
  Sql_mode_dependency value_depends_on_sql_mode_bit_or() const;
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
  bool agg_item_collations(DTCollation &c, const char *name,
                           Item **items, uint nitems,
                           uint flags, int item_sep);
  bool agg_item_set_converter(const DTCollation &coll, const char *fname,
                              Item **args, uint nargs,
                              uint flags, int item_sep);
protected:
  /*
    Collect arguments' character sets together.
    We allow to apply automatic character set conversion in some cases.
    The conditions when conversion is possible are:
    - arguments A and B have different charsets
    - A wins according to coercibility rules
      (i.e. a column is stronger than a string constant,
       an explicit COLLATE clause is stronger than a column)
    - character set of A is either superset for character set of B,
      or B is a string constant which can be converted into the
      character set of A without data loss.

    If all of the above is true, then it's possible to convert
    B into the character set of A, and then compare according
    to the collation of A.

    For functions with more than two arguments:

      collect(A,B,C) ::= collect(collect(A,B),C)

    Since this function calls THD::change_item_tree() on the passed Item **
    pointers, it is necessary to pass the original Item **'s, not copies.
    Otherwise their values will not be properly restored (see BUG#20769).
    If the items are not consecutive (eg. args[2] and args[5]), use the
    item_sep argument, ie.

      agg_item_charsets(coll, fname, &args[2], 2, flags, 3)
  */
  bool agg_arg_charsets(DTCollation &c, Item **items, uint nitems,
                        uint flags, int item_sep)
  {
    if (agg_item_collations(c, func_name(), items, nitems, flags, item_sep))
      return true;

    return agg_item_set_converter(c, func_name(), items, nitems,
                                  flags, item_sep);
  }
  /*
    Aggregate arguments for string result, e.g: CONCAT(a,b)
    - convert to @@character_set_connection if all arguments are numbers
    - allow DERIVATION_NONE
  */
  bool agg_arg_charsets_for_string_result(DTCollation &c,
                                          Item **items, uint nitems,
                                          int item_sep= 1)
  {
    uint flags= MY_COLL_ALLOW_SUPERSET_CONV |
                MY_COLL_ALLOW_COERCIBLE_CONV |
                MY_COLL_ALLOW_NUMERIC_CONV;
    return agg_arg_charsets(c, items, nitems, flags, item_sep);
  }
  /*
    Aggregate arguments for string result, when some comparison
    is involved internally, e.g: REPLACE(a,b,c)
    - convert to @@character_set_connection if all arguments are numbers
    - disallow DERIVATION_NONE
  */
  bool agg_arg_charsets_for_string_result_with_comparison(DTCollation &c,
                                                          Item **items,
                                                          uint nitems,
                                                          int item_sep= 1)
  {
    uint flags= MY_COLL_ALLOW_SUPERSET_CONV |
                MY_COLL_ALLOW_COERCIBLE_CONV |
                MY_COLL_ALLOW_NUMERIC_CONV |
                MY_COLL_DISALLOW_NONE;
    return agg_arg_charsets(c, items, nitems, flags, item_sep);
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
    uint flags= MY_COLL_ALLOW_SUPERSET_CONV |
                MY_COLL_ALLOW_COERCIBLE_CONV |
                MY_COLL_DISALLOW_NONE;
    return agg_arg_charsets(c, items, nitems, flags, item_sep);
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
               (*a)->collation.collation->name,
               (*a)->collation.derivation_name(),
               (*b)->collation.collation->name,
               (*b)->collation.derivation_name(),
               func_name());
      return true;
    }
    if (agg_item_set_converter(tmp, func_name(),
                               a, 1, MY_COLL_CMP_CONV, 1) ||
        agg_item_set_converter(tmp, func_name(),
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
  bool walk(Item_processor processor, bool walk_subquery, void *arg)
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

    NOTE: for Items inherited from Item_sum, func_name() return part of
    function name till first argument (including '(') to make difference in
    names for functions with 'distinct' clause and without 'distinct' and
    also to make printing of items inherited from Item_sum uniform.
  */
  virtual const char *func_name() const= 0;
  virtual bool fix_length_and_dec()= 0;
  bool const_item() const { return const_item_cache; }
  table_map used_tables() const { return used_tables_cache; }
  Item* build_clone(THD *thd, MEM_ROOT *mem_root);
  Sql_mode_dependency value_depends_on_sql_mode() const
  {
    return Item_args::value_depends_on_sql_mode_bit_or().soft_to_hard();
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
           const char *db_arg, const char *table_name_arg,
           const char *field_name_arg):
    Item_ident(thd, context_arg, db_arg, table_name_arg, field_name_arg),
    set_properties_only(0), ref(0), reference_trough_name(1) {}
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
           const char *table_name_arg, const char *field_name_arg,
           bool alias_name_used_arg= FALSE);
  Item_ref(THD *thd, TABLE_LIST *view_arg, Item **item,
           const char *field_name_arg, bool alias_name_used_arg= FALSE);

  /* Constructor need to process subselect with temporary tables (see Item) */
  Item_ref(THD *thd, Item_ref *item)
    :Item_ident(thd, item), set_properties_only(0), ref(item->ref) {}
  enum Type type() const		{ return REF_ITEM; }
  enum Type real_type() const           { return ref ? (*ref)->type() :
                                          REF_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const
  { 
    Item *it= ((Item *) item)->real_item();
    return ref && (*ref)->eq(it, binary_cmp);
  }
  void save_val(Field *to);
  void save_result(Field *to);
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *);
  bool val_bool();
  String *val_str(String* tmp);
  bool is_null();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  longlong val_datetime_packed();
  longlong val_time_packed();
  double val_result();
  longlong val_int_result();
  String *str_result(String* tmp);
  my_decimal *val_decimal_result(my_decimal *);
  bool val_bool_result();
  bool is_null_result();
  bool send(Protocol *prot, String *tmp);
  void make_field(THD *thd, Send_field *field);
  bool fix_fields(THD *, Item **);
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge);
  int save_in_field(Field *field, bool no_conversions);
  void save_org_in_field(Field *field, fast_field_copier optimizer_data);
  fast_field_copier setup_fast_field_copier(Field *field)
  { return (*ref)->setup_fast_field_copier(field); }
#if MARIADB_VERSION_ID < 100300
  CHARSET_INFO *charset_for_protocol(void) const
  {
    return (*ref)->charset_for_protocol();
  }
#endif
  enum Item_result result_type () const { return (*ref)->result_type(); }
  enum_field_types field_type() const   { return (*ref)->field_type(); }
  Field *get_tmp_table_field()
  { return result_field ? result_field : (*ref)->get_tmp_table_field(); }
  Item *get_tmp_table_item(THD *thd);
  table_map used_tables() const;		
  void update_used_tables(); 
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref)
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
  bool const_item() const 
  {
    return (*ref)->const_item();
  }
  table_map not_null_tables() const 
  { 
    return depended_from ? 0 : (*ref)->not_null_tables();
  }
  void save_in_result_field(bool no_conversions)
  {
    (*ref)->save_in_field(result_field, no_conversions);
  }
  Item *real_item()
  {
    return ref ? (*ref)->real_item() : this;
  }
  bool walk(Item_processor processor, bool walk_subquery, void *arg)
  { 
    if (ref && *ref)
      return (*ref)->walk(processor, walk_subquery, arg) ||
             (this->*processor)(arg); 
    else
      return FALSE;
  }
  Item* transform(THD *thd, Item_transformer, uchar *arg);
  Item* compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                Item_transformer transformer, uchar *arg_t);
  bool enumerate_field_refs_processor(void *arg)
  { return (*ref)->enumerate_field_refs_processor(arg); }
  void no_rows_in_result()
  {
    (*ref)->no_rows_in_result();
  }
  void restore_to_before_no_rows_in_result()
  {
    (*ref)->restore_to_before_no_rows_in_result();
  }
  void print(String *str, enum_query_type query_type);
  enum precedence precedence() const
  {
    return ref ? (*ref)->precedence() : DEFAULT_PRECEDENCE;
  }
  void cleanup();
  Item_field *field_for_view_update()
    { return (*ref)->field_for_view_update(); }
  Load_data_outvar *get_load_data_outvar()
  {
    return (*ref)->get_load_data_outvar();
  }
  virtual Ref_Type ref_type() { return REF; }

  // Row emulation: forwarding of ROW-related calls to ref
  uint cols()
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->cols() : 1;
  }
  Item* element_index(uint i)
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->element_index(i) : this;
  }
  Item** addr(uint i)
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->addr(i) : 0;
  }
  bool check_cols(uint c)
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->check_cols(c) 
                                              : Item::check_cols(c);
  }
  bool null_inside()
  {
    return ref && result_type() == ROW_RESULT ? (*ref)->null_inside() : 0;
  }
  void bring_value()
  { 
    if (ref && result_type() == ROW_RESULT)
      (*ref)->bring_value();
  }
  bool check_vcol_func_processor(void *arg) 
  {
    return mark_unsupported_function("ref", arg, VCOL_IMPOSSIBLE);
  }
  bool basic_const_item() const { return ref && (*ref)->basic_const_item(); }
  bool is_outer_field() const
  {
    DBUG_ASSERT(fixed);
    DBUG_ASSERT(ref);
    return (*ref)->is_outer_field();
  }
  
  Item* build_clone(THD *thd, MEM_ROOT *mem_root);

  /**
    Checks if the item tree that ref points to contains a subquery.
  */
  virtual bool has_subquery() const 
  { 
    return (*ref)->has_subquery();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_ref>(thd, mem_root, this); }
  bool excl_dep_on_table(table_map tab_map)
  { 
    table_map used= used_tables();
    if (used & OUTER_REF_TABLE_BIT)
      return false;
    return (used == tab_map) || (*ref)->excl_dep_on_table(tab_map);
  }
  bool excl_dep_on_grouping_fields(st_select_lex *sel)
  { return (*ref)->excl_dep_on_grouping_fields(sel); }
  bool cleanup_excluding_fields_processor(void *arg)
  {
    Item *item= real_item();
    if (item && item->type() == FIELD_ITEM &&
        ((Item_field *)item)->field)
      return 0;
    return cleanup_processor(arg);
  }
  bool cleanup_excluding_const_fields_processor(void *arg)
  { 
    Item *item= real_item();
    if (item && item->type() == FIELD_ITEM &&
        ((Item_field *) item)->field && item->const_item())
      return 0;
    return cleanup_processor(arg);
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
                  const char *table_name_arg,
                  const char *field_name_arg,
                  bool alias_name_used_arg= FALSE):
    Item_ref(thd, context_arg, item, table_name_arg,
             field_name_arg, alias_name_used_arg)
  {}
  /* Constructor need to process subselect with temporary tables (see Item) */
  Item_direct_ref(THD *thd, Item_direct_ref *item) : Item_ref(thd, item) {}
  Item_direct_ref(THD *thd, TABLE_LIST *view_arg, Item **item,
                  const char *field_name_arg,
                  bool alias_name_used_arg= FALSE):
    Item_ref(thd, view_arg, item, field_name_arg,
             alias_name_used_arg)
  {}

  bool fix_fields(THD *thd, Item **it)
  {
    if ((!(*ref)->fixed && (*ref)->fix_fields(thd, ref)) ||
        (*ref)->check_cols(1))
      return TRUE;
    return Item_ref::fix_fields(thd, it);
  }
  void save_val(Field *to);
  /* Below we should have all val() methods as in Item_ref */
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *);
  bool val_bool();
  String *val_str(String* tmp);
  bool is_null();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  longlong val_datetime_packed();
  longlong val_time_packed();
  virtual Ref_Type ref_type() { return DIRECT_REF; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_direct_ref>(thd, mem_root, this); }
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

  bool fix_fields(THD *thd, Item **it)
  {
    DBUG_ASSERT(ident->type() == FIELD_ITEM || ident->type() == REF_ITEM);
    if ((!ident->fixed && ident->fix_fields(thd, ref)) ||
        ident->check_cols(1))
      return TRUE;
    set_properties();
    return FALSE;
  }

  virtual void print(String *str, enum_query_type query_type)
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

  enum Type type() const { return EXPR_CACHE_ITEM; }
  enum Type real_type() const { return orig_item->type(); }

  bool set_cache(THD *thd);
  Expression_cache_tracker* init_tracker(MEM_ROOT *mem_root);

  bool fix_fields(THD *thd, Item **it);
  void cleanup();

  Item *get_orig_item() const { return orig_item; }

  /* Methods of getting value which should be cached in the cache */
  void save_val(Field *to);
  double val_real();
  longlong val_int();
  String *val_str(String* tmp);
  my_decimal *val_decimal(my_decimal *);
  bool val_bool();
  bool is_null();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  bool send(Protocol *protocol, String *buffer);
  void save_org_in_field(Field *field,
                         fast_field_copier data __attribute__ ((__unused__)))
  {
    save_val(field);
  }
  void save_in_result_field(bool no_conversions)
  {
    save_val(result_field);
  }
  Item* get_tmp_table_item(THD *thd_arg);

  /* Following methods make this item transparent as much as possible */

  virtual void print(String *str, enum_query_type query_type);
  virtual const char *full_name() const { return orig_item->full_name(); }
  virtual void make_field(THD *thd, Send_field *field)
  { orig_item->make_field(thd, field); }
  bool eq(const Item *item, bool binary_cmp) const
  {
    Item *it= ((Item *) item)->real_item();
    return orig_item->eq(it, binary_cmp);
  }
  void fix_after_pullout(st_select_lex *new_parent, Item **refptr, bool merge)
  {
    orig_item->fix_after_pullout(new_parent, &orig_item, merge);
  }
  int save_in_field(Field *to, bool no_conversions);
  enum Item_result result_type () const { return orig_item->result_type(); }
  enum_field_types field_type() const   { return orig_item->field_type(); }
  table_map used_tables() const { return orig_item->used_tables(); }
  void update_used_tables()
  {
    orig_item->update_used_tables();
  }
  bool const_item() const { return orig_item->const_item(); }
  table_map not_null_tables() const { return orig_item->not_null_tables(); }
  bool walk(Item_processor processor, bool walk_subquery, void *arg)
  {
    return orig_item->walk(processor, walk_subquery, arg) ||
      (this->*processor)(arg);
  }
  bool enumerate_field_refs_processor(void *arg)
  { return orig_item->enumerate_field_refs_processor(arg); }
  Item_field *field_for_view_update()
  { return orig_item->field_for_view_update(); }

  /* Row emulation: forwarding of ROW-related calls to orig_item */
  uint cols()
  { return result_type() == ROW_RESULT ? orig_item->cols() : 1; }
  Item* element_index(uint i)
  { return result_type() == ROW_RESULT ? orig_item->element_index(i) : this; }
  Item** addr(uint i)
  { return result_type() == ROW_RESULT ? orig_item->addr(i) : 0; }
  bool check_cols(uint c)
  {
    return (result_type() == ROW_RESULT ?
            orig_item->check_cols(c) :
            Item::check_cols(c));
  }
  bool null_inside()
  { return result_type() == ROW_RESULT ? orig_item->null_inside() : 0; }
  void bring_value()
  {
    if (result_type() == ROW_RESULT)
      orig_item->bring_value();
  }
  bool is_expensive() { return orig_item->is_expensive(); }
  bool is_expensive_processor(void *arg)
  { return orig_item->is_expensive_processor(arg); }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function("cache", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_wrapper>(thd, mem_root, this); }
  Item *build_clone(THD *thd, MEM_ROOT *mem_root) { return 0; }
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
                       const char *table_name_arg,
                       const char *field_name_arg,
                       TABLE_LIST *view_arg):
    Item_direct_ref(thd, context_arg, item, table_name_arg, field_name_arg),
    item_equal(0), view(view_arg),
    null_ref_table(NULL)
  {
    if (fixed)
      set_null_ref_table();
  }

  bool fix_fields(THD *, Item **);
  bool eq(const Item *item, bool binary_cmp) const;
  Item *get_tmp_table_item(THD *thd)
  {
    if (const_item())
      return copy_or_same(thd);
    Item *item= Item_ref::get_tmp_table_item(thd);
    item->name= name;
    return item;
  }
  virtual Ref_Type ref_type() { return VIEW_REF; }
  Item_equal *get_item_equal() { return item_equal; }
  void set_item_equal(Item_equal *item_eq) { item_equal= item_eq; }
  Item_equal *find_item_equal(COND_EQUAL *cond_equal);
  Item* propagate_equal_fields(THD *, const Context &, COND_EQUAL *);
  Item *replace_equal_field(THD *thd, uchar *arg);
  table_map used_tables() const;
  void update_used_tables();
  table_map not_null_tables() const;
  bool const_item() const { return used_tables() == 0; }
  TABLE *get_null_ref_table() const { return null_ref_table; }
  bool walk(Item_processor processor, bool walk_subquery, void *arg)
  { 
    return (*ref)->walk(processor, walk_subquery, arg) ||
           (this->*processor)(arg);
  }
  bool view_used_tables_processor(void *arg) 
  {
    TABLE_LIST *view_arg= (TABLE_LIST *) arg;
    if (view_arg == view)
      view_arg->view_used_tables|= (*ref)->used_tables();
    return 0;
  }
  bool excl_dep_on_table(table_map tab_map);
  bool excl_dep_on_grouping_fields(st_select_lex *sel);
  Item *derived_field_transformer_for_having(THD *thd, uchar *arg);
  Item *derived_field_transformer_for_where(THD *thd, uchar *arg);
  Item *derived_grouping_field_transformer_for_where(THD *thd,
                                                     uchar *arg);

  void save_val(Field *to)
  {
    if (check_null_ref())
      to->set_null();
    else
      Item_direct_ref::save_val(to);
  }
  double val_real()
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_real();
  }
  longlong val_int()
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_int();
  }
  String *val_str(String* tmp)
  {
    if (check_null_ref())
      return NULL;
    else
      return Item_direct_ref::val_str(tmp);
  }
  my_decimal *val_decimal(my_decimal *tmp)
  {
    if (check_null_ref())
      return NULL;
    else
      return Item_direct_ref::val_decimal(tmp);
  }
  bool val_bool()
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_bool();
  }
  bool is_null()
  {
    if (check_null_ref())
      return 1;
    else
      return Item_direct_ref::is_null();
  }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  {
    if (check_null_ref())
    {
      bzero((char*) ltime,sizeof(*ltime));
      return 1;
    }
    return Item_direct_ref::get_date(ltime, fuzzydate);
  }
  longlong val_time_packed()
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_time_packed();
  }
  longlong val_datetime_packed()
  {
    if (check_null_ref())
      return 0;
    else
      return Item_direct_ref::val_datetime_packed();
  }
  bool send(Protocol *protocol, String *buffer);
  void save_org_in_field(Field *field,
                         fast_field_copier data __attribute__ ((__unused__)))
  {
    if (check_null_ref())
      field->set_null();
    else
      Item_direct_ref::save_val(field);
  }
  void save_in_result_field(bool no_conversions)
  {
    if (check_null_ref())
      result_field->set_null();
    else
      Item_direct_ref::save_in_result_field(no_conversions);
  }

  void cleanup()
  {
    null_ref_table= NULL;
    item_equal= NULL;
    Item_direct_ref::cleanup();
  }
  /*
    TODO move these val_*_result function to Item_dierct_ref (maybe)
  */
  double val_result();
  longlong val_int_result();
  String *str_result(String* tmp);
  my_decimal *val_decimal_result(my_decimal *val);
  bool val_bool_result();

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_direct_view_ref>(thd, mem_root, this); }
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
    fixed= 0;                     /* reset flag set in set_properties() */
  }
  Item_outer_ref(THD *thd, Name_resolution_context *context_arg, Item **item,
                 const char *table_name_arg, const char *field_name_arg,
                 bool alias_name_used_arg):
    Item_direct_ref(thd, context_arg, item, table_name_arg, field_name_arg,
                    alias_name_used_arg),
    outer_ref(0), in_sum_func(0), found_in_select_list(1), found_in_group_by(0)
  {}
  void save_in_result_field(bool no_conversions)
  {
    outer_ref->save_org_in_field(result_field, NULL);
  }
  bool fix_fields(THD *, Item **);
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge);
  table_map used_tables() const
  {
    return (*ref)->const_item() ? 0 : OUTER_REF_TABLE_BIT;
  }
  table_map not_null_tables() const { return 0; }
  virtual Ref_Type ref_type() { return OUTER_REF; }
  bool check_inner_refs_processor(void * arg); 
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
		       const char *table_name_arg, const char *field_name_arg):
    Item_ref(thd, context_arg, item, table_name_arg, field_name_arg),
    owner(master) {}
  void save_val(Field *to);
  double val_real();
  longlong val_int();
  String* val_str(String* s);
  my_decimal *val_decimal(my_decimal *);
  bool val_bool();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  virtual void print(String *str, enum_query_type query_type);
  table_map used_tables() const;
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_ref_null_helper>(thd, mem_root, this); }
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
  int save_in_field(Field *field, bool no_conversions)
  {
    return ref->save_in_field(field, no_conversions);
  }
  Item *clone_item(THD *thd);
  virtual Item *real_item() { return ref; }
};

#ifdef MYSQL_SERVER
#include "gstream.h"
#include "spatial.h"
#include "item_sum.h"
#include "item_func.h"
#include "item_row.h"
#include "item_cmpfunc.h"
#include "item_strfunc.h"
#include "item_geofunc.h"
#include "item_timefunc.h"
#include "item_subselect.h"
#include "item_xmlfunc.h"
#include "item_jsonfunc.h"
#include "item_create.h"
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
  Item_copy(THD *thd, Item *i): Item(thd)
  {
    item= i;
    null_value=maybe_null=item->maybe_null;
    Type_std_attributes::set(item);
    name=item->name;
    set_handler_by_field_type(item->field_type());
    fixed= item->fixed;
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
  enum Type type() const { return COPY_STR_ITEM; }

  enum_field_types field_type() const
  { return Type_handler_hybrid_field_type::field_type(); }
  enum Item_result result_type () const
  { return Type_handler_hybrid_field_type::result_type(); }
  enum Item_result cmp_type () const
  { return Type_handler_hybrid_field_type::cmp_type(); }

  void make_field(THD *thd, Send_field *field) { item->make_field(thd, field); }
  table_map used_tables() const { return (table_map) 1L; }
  bool const_item() const { return 0; }
  bool is_null() { return null_value; }
  bool check_vcol_func_processor(void *arg) 
  {
    return mark_unsupported_function("copy", arg, VCOL_IMPOSSIBLE);
  }

  /*  
    Override the methods below as pure virtual to make sure all the 
    sub-classes implement them.
  */  

  virtual String *val_str(String*) = 0;
  virtual my_decimal *val_decimal(my_decimal *) = 0;
  virtual double val_real() = 0;
  virtual longlong val_int() = 0;
  virtual int save_in_field(Field *field, bool no_conversions) = 0;
  bool walk(Item_processor processor, bool walk_subquery, void *args)
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

  String *val_str(String*);
  my_decimal *val_decimal(my_decimal *);
  double val_real();
  longlong val_int();
  void copy();
  int save_in_field(Field *field, bool no_conversions);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_copy_string>(thd, mem_root, this); }
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
  bool cmp(void);
  int  cmp_read_only();
  ~Cached_item_str();                           // Deallocate String:s
};


class Cached_item_real :public Cached_item_item
{
  double value;
public:
  Cached_item_real(Item *item_par) :Cached_item_item(item_par),value(0.0) {}
  bool cmp(void);
  int  cmp_read_only();
};

class Cached_item_int :public Cached_item_item
{
  longlong value;
public:
  Cached_item_int(Item *item_par) :Cached_item_item(item_par),value(0) {}
  bool cmp(void);
  int  cmp_read_only();
};


class Cached_item_decimal :public Cached_item_item
{
  my_decimal value;
public:
  Cached_item_decimal(Item *item_par);
  bool cmp(void);
  int  cmp_read_only();
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
  bool cmp(void);
  int  cmp_read_only();
};

class Item_default_value : public Item_field
{
  void calculate();
public:
  Item *arg;
  Field *cached_field;
  Item_default_value(THD *thd, Name_resolution_context *context_arg)
    :Item_field(thd, context_arg, (const char *)NULL, (const char *)NULL,
               (const char *)NULL),
    arg(NULL), cached_field(NULL) {}
  Item_default_value(THD *thd, Name_resolution_context *context_arg, Item *a)
    :Item_field(thd, context_arg, (const char *)NULL, (const char *)NULL,
                (const char *)NULL),
    arg(a), cached_field(NULL) {}
  Item_default_value(THD *thd, Name_resolution_context *context_arg, Field *a)
    :Item_field(thd, context_arg, (const char *)NULL, (const char *)NULL,
                (const char *)NULL),
    arg(NULL),cached_field(NULL) {}
  enum Type type() const { return DEFAULT_VALUE_ITEM; }
  bool vcol_assignment_allowed_value() const { return arg == NULL; }
  bool eq(const Item *item, bool binary_cmp) const;
  bool fix_fields(THD *, Item **);
  void cleanup();
  void print(String *str, enum_query_type query_type);
  String *val_str(String *str);
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *decimal_value);
  bool get_date(MYSQL_TIME *ltime,ulonglong fuzzydate);
  bool send(Protocol *protocol, String *buffer);
  int save_in_field(Field *field_arg, bool no_conversions);
  bool save_in_param(THD *thd, Item_param *param)
  {
    // It should not be possible to have "EXECUTE .. USING DEFAULT(a)"
    DBUG_ASSERT(arg == NULL);
    param->set_default();
    return false;
  }
  table_map used_tables() const;
  virtual void update_used_tables()
  {
    if (field && field->default_value)
      field->default_value->expr->update_used_tables();
  }
  Field *get_tmp_table_field() { return 0; }
  Item *get_tmp_table_item(THD *thd) { return this; }
  Item_field *field_for_view_update() { return 0; }
  bool update_vcol_processor(void *arg) { return 0; }
  bool check_func_default_processor(void *arg) { return true; }

  bool walk(Item_processor processor, bool walk_subquery, void *args)
  {
    return (arg && arg->walk(processor, walk_subquery, args)) ||
      (this->*processor)(args);
  }

  Item *transform(THD *thd, Item_transformer transformer, uchar *args);
};

/**
  This class is used as bulk parameter INGNORE representation.

  It just do nothing when assigned to a field

*/

class Item_ignore_value : public Item_default_value
{
public:
  Item_ignore_value(THD *thd, Name_resolution_context *context_arg)
    :Item_default_value(thd, context_arg)
  {};

  void print(String *str, enum_query_type query_type);
  int save_in_field(Field *field_arg, bool no_conversions);
  bool save_in_param(THD *thd, Item_param *param)
  {
    param->set_ignore();
    return false;
  }

  String *val_str(String *str);
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *decimal_value);
  bool get_date(MYSQL_TIME *ltime,ulonglong fuzzydate);
  bool send(Protocol *protocol, String *buffer);
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
    :Item_field(thd, context_arg, (const char *)NULL, (const char *)NULL,
               (const char *)NULL),
     arg(a) {}
  bool eq(const Item *item, bool binary_cmp) const;
  bool fix_fields(THD *, Item **);
  virtual void print(String *str, enum_query_type query_type);
  int save_in_field(Field *field_arg, bool no_conversions)
  {
    return Item_field::save_in_field(field_arg, no_conversions);
  }
  enum Type type() const { return INSERT_VALUE_ITEM; }
  /*
   We use RAND_TABLE_BIT to prevent Item_insert_value from
   being treated as a constant and precalculated before execution
  */
  table_map used_tables() const { return RAND_TABLE_BIT; }

  Item_field *field_for_view_update() { return 0; }

  bool walk(Item_processor processor, bool walk_subquery, void *args)
  {
    return arg->walk(processor, walk_subquery, args) ||
	    (this->*processor)(args);
  }
  bool check_partition_func_processor(void *int_arg) {return TRUE;}
  bool update_vcol_processor(void *arg) { return 0; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function("values()", arg, VCOL_IMPOSSIBLE);
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
public:
  /* Is this item represents row from NEW or OLD row ? */
  enum row_version_type {OLD_ROW, NEW_ROW};
  row_version_type row_version;
  /* Next in list of all Item_trigger_field's in trigger */
  Item_trigger_field *next_trg_field;
  /* Index of the field in the TABLE::field array */
  uint field_idx;
  /* Pointer to Table_trigger_list object for table of this trigger */
  Table_triggers_list *triggers;

  Item_trigger_field(THD *thd, Name_resolution_context *context_arg,
                     row_version_type row_ver_arg,
                     const char *field_name_arg,
                     ulong priv, const bool ro)
    :Item_field(thd, context_arg,
               (const char *)NULL, (const char *)NULL, field_name_arg),
     row_version(row_ver_arg), field_idx((uint)-1), original_privilege(priv),
     want_privilege(priv), table_grants(NULL), read_only (ro)
  {}
  void setup_field(THD *thd, TABLE *table, GRANT_INFO *table_grant_info);
  enum Type type() const { return TRIGGER_FIELD_ITEM; }
  bool eq(const Item *item, bool binary_cmp) const;
  bool fix_fields(THD *, Item **);
  virtual void print(String *str, enum_query_type query_type);
  table_map used_tables() const { return (table_map)0L; }
  Field *get_tmp_table_field() { return 0; }
  Item *copy_or_same(THD *thd) { return this; }
  Item *get_tmp_table_item(THD *thd) { return copy_or_same(thd); }
  void cleanup();

private:
  void set_required_privilege(bool rw);
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it);

public:
  Settable_routine_parameter *get_settable_routine_parameter()
  {
    return (read_only ? 0 : this);
  }

  bool set_value(THD *thd, Item **it)
  {
    return set_value(thd, NULL, it);
  }

private:
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
  ulong original_privilege;
  ulong want_privilege;
  GRANT_INFO *table_grants;
  /*
    Trigger field is read-only unless it belongs to the NEW row in a
    BEFORE INSERT of BEFORE UPDATE trigger.
  */
  bool read_only;
public:
  bool check_vcol_func_processor(void *arg);
};


/**
  @todo
  Implement the is_null() method for this class. Currently calling is_null()
  on any Item_cache object resolves to Item::is_null(), which returns FALSE
  for any value.
*/

class Item_cache: public Item_basic_constant,
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
public:
  Item_cache(THD *thd):
    Item_basic_constant(thd),
    Type_handler_hybrid_field_type(MYSQL_TYPE_STRING),
    example(0), cached_field(0),
    value_cached(0)
  {
    fixed= 1;
    maybe_null= 1;
    null_value= 1;
  }
protected:
  Item_cache(THD *thd, enum_field_types field_type_arg):
    Item_basic_constant(thd),
    Type_handler_hybrid_field_type(field_type_arg),
    example(0), cached_field(0),
    value_cached(0)
  {
    fixed= 1;
    maybe_null= 1;
    null_value= 1;
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
  enum Type type() const { return CACHE_ITEM; }

  enum_field_types field_type() const
  { return Type_handler_hybrid_field_type::field_type(); }
  enum Item_result result_type () const
  { return Type_handler_hybrid_field_type::result_type(); }
  enum Item_result cmp_type () const
  { return Type_handler_hybrid_field_type::cmp_type(); }

  static Item_cache* get_cache(THD *thd, const Item* item,
                         const Item_result type, const enum_field_types f_type);
  static Item_cache* get_cache(THD *thd, const Item* item,
                         const Item_result type)
  {
    return get_cache(thd, item, type, item->field_type());
  }
  static Item_cache* get_cache(THD *thd, const Item *item)
  {
    return get_cache(thd, item, item->cmp_type());
  }
  virtual void keep_array() {}
  virtual void print(String *str, enum_query_type query_type);
  bool eq_def(const Field *field) 
  { 
    return cached_field ? cached_field->eq_def (field) : FALSE;
  }
  bool eq(const Item *item, bool binary_cmp) const
  {
    return this == item;
  }
  bool check_vcol_func_processor(void *arg) 
  {
    if (example)
    {
      Item::vcol_func_processor_result *res= (Item::vcol_func_processor_result*)arg;
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
  void cleanup()
  {
    clear();
    Item_basic_constant::cleanup();
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
  virtual bool cache_value()= 0;
  bool basic_const_item() const
  { return example && example->basic_const_item(); }
  virtual void clear() { null_value= TRUE; value_cached= FALSE; }
  bool is_null() { return !has_value(); }
  virtual bool is_expensive()
  {
    if (value_cached)
      return false;
    return example->is_expensive();
  }
  bool is_expensive_processor(void *arg)
  {
    DBUG_ASSERT(example);
    if (value_cached)
      return false;
    return example->is_expensive_processor(arg);
  }
  virtual void set_null();
  bool walk(Item_processor processor, bool walk_subquery, void *arg)
  {
    if (example && example->walk(processor, walk_subquery, arg))
      return TRUE;
    return (this->*processor)(arg);
  }
  virtual Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs);
  void split_sum_func2_example(THD *thd,  Ref_ptr_array ref_pointer_array,
                               List<Item> &fields, uint flags)
  {
    example->split_sum_func2(thd, ref_pointer_array, fields, &example, flags);
  }
  Item *get_example() const { return example; }

  virtual Item *convert_to_basic_const_item(THD *thd) { return 0; };
  Item *derived_field_transformer_for_having(THD *thd, uchar *arg)
  { return convert_to_basic_const_item(thd); }
  Item *derived_field_transformer_for_where(THD *thd, uchar *arg)
  { return convert_to_basic_const_item(thd); }
  Item *derived_grouping_field_transformer_for_where(THD *thd, uchar *arg)
  { return convert_to_basic_const_item(thd); }
};


class Item_cache_int: public Item_cache
{
protected:
  longlong value;
public:
  Item_cache_int(THD *thd): Item_cache(thd, MYSQL_TYPE_LONGLONG),
    value(0) {}
  Item_cache_int(THD *thd, enum_field_types field_type_arg):
    Item_cache(thd, field_type_arg), value(0) {}

  double val_real();
  longlong val_int();
  String* val_str(String *str);
  my_decimal *val_decimal(my_decimal *);
  enum Item_result result_type() const { return INT_RESULT; }
  bool cache_value();
  int save_in_field(Field *field, bool no_conversions);
  Item *convert_to_basic_const_item(THD *thd);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_int>(thd, mem_root, this); }
};


class Item_cache_temporal: public Item_cache_int
{
public:
  Item_cache_temporal(THD *thd, enum_field_types field_type_arg);
  String* val_str(String *str);
  my_decimal *val_decimal(my_decimal *);
  longlong val_int();
  longlong val_datetime_packed();
  longlong val_time_packed();
  double val_real();
  bool cache_value();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  int save_in_field(Field *field, bool no_conversions);
  bool setup(THD *thd, Item *item)
  {
    if (Item_cache_int::setup(thd, item))
      return true;
    set_if_smaller(decimals, TIME_SECOND_PART_DIGITS);
    return false;
  }
  Item_result cmp_type() const { return TIME_RESULT; }
  void store_packed(longlong val_arg, Item *example);
  /*
    Having a clone_item method tells optimizer that this object
    is a constant and need not be optimized further.
    Important when storing packed datetime values.
  */
  Item *clone_item(THD *thd);
  Item *convert_to_basic_const_item(THD *thd);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_temporal>(thd, mem_root, this); }
};


class Item_cache_real: public Item_cache
{
  double value;
public:
  Item_cache_real(THD *thd): Item_cache(thd, MYSQL_TYPE_DOUBLE),
    value(0) {}

  double val_real();
  longlong val_int();
  String* val_str(String *str);
  my_decimal *val_decimal(my_decimal *);
  enum Item_result result_type() const { return REAL_RESULT; }
  bool cache_value();
  Item *convert_to_basic_const_item(THD *thd);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_real>(thd, mem_root, this); }
};


class Item_cache_decimal: public Item_cache
{
protected:
  my_decimal decimal_value;
public:
  Item_cache_decimal(THD *thd): Item_cache(thd, MYSQL_TYPE_NEWDECIMAL) {}

  double val_real();
  longlong val_int();
  String* val_str(String *str);
  my_decimal *val_decimal(my_decimal *);
  enum Item_result result_type() const { return DECIMAL_RESULT; }
  bool cache_value();
  Item *convert_to_basic_const_item(THD *thd);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_decimal>(thd, mem_root, this); }
};


class Item_cache_str: public Item_cache
{
  char buffer[STRING_BUFFER_USUAL_SIZE];
  String *value, value_buff;
  bool is_varbinary;
  
public:
  Item_cache_str(THD *thd, const Item *item):
    Item_cache(thd, item->field_type()), value(0),
    is_varbinary(item->type() == FIELD_ITEM &&
                 Item_cache_str::field_type() == MYSQL_TYPE_VARCHAR &&
                 !((const Item_field *) item)->field->has_charset())
  {
    collation.set(const_cast<DTCollation&>(item->collation));
  }
  double val_real();
  longlong val_int();
  String* val_str(String *);
  my_decimal *val_decimal(my_decimal *);
  enum Item_result result_type() const { return STRING_RESULT; }
  CHARSET_INFO *charset() const { return value->charset(); };
  int save_in_field(Field *field, bool no_conversions);
  bool cache_value();
  Item *convert_to_basic_const_item(THD *thd);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_str>(thd, mem_root, this); }
};


class Item_cache_str_for_nullif: public Item_cache_str
{
public:
  Item_cache_str_for_nullif(THD *thd, const Item *item)
   :Item_cache_str(thd, item)
  { }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs)
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
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_str_for_nullif>(thd, mem_root, this); }
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
  bool allocate(THD *thd, uint num);
  /*
    'setup' is needed only by row => it not called by simple row subselect
    (only by IN subselect (in subselect optimizer))
  */
  bool setup(THD *thd, Item *item);
  void store(Item *item);
  void illegal_method_call(const char *);
  void make_field(THD *thd, Send_field *)
  {
    illegal_method_call((const char*)"make_field");
  };
  double val_real()
  {
    illegal_method_call((const char*)"val");
    return 0;
  };
  longlong val_int()
  {
    illegal_method_call((const char*)"val_int");
    return 0;
  };
  String *val_str(String *)
  {
    illegal_method_call((const char*)"val_str");
    return 0;
  };
  my_decimal *val_decimal(my_decimal *val)
  {
    illegal_method_call((const char*)"val_decimal");
    return 0;
  };

  enum Item_result result_type() const { return ROW_RESULT; }
  
  uint cols() { return item_count; }
  Item *element_index(uint i) { return values[i]; }
  Item **addr(uint i) { return (Item **) (values + i); }
  bool check_cols(uint c);
  bool null_inside();
  void bring_value();
  void keep_array() { save_array= 1; }
  void cleanup()
  {
    DBUG_ENTER("Item_cache_row::cleanup");
    Item_cache::cleanup();
    if (save_array)
      bzero(values, item_count*sizeof(Item**));
    else
      values= 0;
    DBUG_VOID_RETURN;
  }
  bool cache_value();
  virtual void set_null();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_cache_row>(thd, mem_root, this); }
};


/*
  Item_type_holder used to store type. name, length of Item for UNIONS &
  derived tables.

  Item_type_holder do not need cleanup() because its time of live limited by
  single SP/PS execution.
*/
class Item_type_holder: public Item,
                        public Type_handler_hybrid_real_field_type
{
protected:
  TYPELIB *enum_set_typelib;
  Field::geometry_type geometry_type;

  void get_full_info(Item *item);

  /* It is used to count decimal precision in join_types */
  int prev_decimal_int_part;
public:
  Item_type_holder(THD*, Item*);

  enum_field_types field_type() const
  { return Type_handler_hybrid_real_field_type::field_type(); }
  enum_field_types real_field_type() const
  { return Type_handler_hybrid_real_field_type::real_field_type(); }
  enum Item_result result_type () const
  {
    /*
      In 10.1 Item_type_holder::result_type() returned
      Field::result_merge_type(field_type()), which returned STRING_RESULT
      for the BIT data type. In 10.2 it returns INT_RESULT, similar
      to what Field_bit::result_type() does. This should not be
      important because Item_type_holder is a limited purpose Item
      and its result_type() should not be called from outside of
      Item_type_holder. It's called only internally from decimal_int_part()
      from join_types(), to calculate "decimals" of the result data type.
      As soon as we get BIT as one of the joined types, the result field
      type cannot be numeric: it's either BIT, or VARBINARY.
    */
    return Type_handler_hybrid_real_field_type::result_type();
  }

  enum Type type() const { return TYPE_HOLDER; }
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *);
  String *val_str(String*);
  bool join_types(THD *thd, Item *);
  Field *make_field_by_type(TABLE *table);
  static uint32 display_length(Item *item);
  static enum_field_types get_real_type(Item *);
  Field::geometry_type get_geometry_type() const { return geometry_type; };
  Item* get_copy(THD *thd, MEM_ROOT *mem_root) { return 0; }
};


class st_select_lex;
void mark_select_range_as_dependent(THD *thd,
                                    st_select_lex *last_select,
                                    st_select_lex *current_sel,
                                    Field *found_field, Item *found_item,
                                    Item_ident *resolved_item);

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
  return field_type == vcol->get_real_type()
      && stored_in_db == vcol->is_stored()
      && expr->eq(vcol->expr, true);
}

inline void Virtual_column_info::print(String* str)
{
  expr->print_for_table_def(str);
}

#endif /* SQL_ITEM_INCLUDED */
