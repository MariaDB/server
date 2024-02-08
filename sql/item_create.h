/* Copyright (c) 2000, 2010, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* Functions to create an item. Used by sql/sql_yacc.yy */

#ifndef ITEM_CREATE_H
#define ITEM_CREATE_H

#include "item_func.h" // Cast_target

typedef struct st_udf_func udf_func;

/**
  Public function builder interface.
  The parser (sql/sql_yacc.yy) uses a factory / builder pattern to
  construct an <code>Item</code> object for each function call.
  All the concrete function builders implements this interface,
  either directly or indirectly with some adapter helpers.
  Keeping the function creation separated from the bison grammar allows
  to simplify the parser, and avoid the need to introduce a new token
  for each function, which has undesirable side effects in the grammar.
*/

class Create_func
{
public:
  /**
    The builder create method.
    Given the function name and list or arguments, this method creates
    an <code>Item</code> that represents the function call.
    In case or errors, a NULL item is returned, and an error is reported.
    Note that the <code>thd</code> object may be modified by the builder.
    In particular, the following members/methods can be set/called,
    depending on the function called and the function possible side effects.
    <ul>
      <li><code>thd->lex->binlog_row_based_if_mixed</code></li>
      <li><code>thd->lex->current_context()</code></li>
      <li><code>thd->lex->safe_to_cache_query</code></li>
      <li><code>thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT)</code></li>
      <li><code>thd->lex->uncacheable(UNCACHEABLE_RAND)</code></li>
      <li><code>thd->lex->add_time_zone_tables_to_query_tables(thd)</code></li>
    </ul>
    @param thd The current thread
    @param name The function name
    @param item_list The list of arguments to the function, can be NULL
    @return An item representing the parsed function call, or NULL
  */
  virtual Item *create_func(THD *thd, const LEX_CSTRING *name,
                            List<Item> *item_list) = 0;

protected:
  /** Constructor */
  Create_func() = default;
  /** Destructor */
  virtual ~Create_func() = default;
};


/**
  Adapter for functions that takes exactly zero arguments.
*/

class Create_func_arg0 : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  /**
    Builder method, with no arguments.
    @param thd The current thread
    @return An item representing the function call
  */
  virtual Item *create_builder(THD *thd) = 0;

protected:
  /** Constructor. */
  Create_func_arg0() = default;
  /** Destructor. */
  virtual ~Create_func_arg0() = default;
};


/**
  Adapter for functions that takes exactly one argument.
*/

class Create_func_arg1 : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  /**
    Builder method, with one argument.
    @param thd The current thread
    @param arg1 The first argument of the function
    @return An item representing the function call
  */
  virtual Item *create_1_arg(THD *thd, Item *arg1) = 0;

protected:
  /** Constructor. */
  Create_func_arg1() = default;
  /** Destructor. */
  virtual ~Create_func_arg1() = default;
};


/**
  Adapter for functions that takes exactly two arguments.
*/

class Create_func_arg2 : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  /**
    Builder method, with two arguments.
    @param thd The current thread
    @param arg1 The first argument of the function
    @param arg2 The second argument of the function
    @return An item representing the function call
  */
  virtual Item *create_2_arg(THD *thd, Item *arg1, Item *arg2) = 0;

protected:
  /** Constructor. */
  Create_func_arg2() = default;
  /** Destructor. */
  virtual ~Create_func_arg2() = default;
};


/**
  Adapter for functions that takes exactly three arguments.
*/

class Create_func_arg3 : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  /**
    Builder method, with three arguments.
    @param thd The current thread
    @param arg1 The first argument of the function
    @param arg2 The second argument of the function
    @param arg3 The third argument of the function
    @return An item representing the function call
  */
  virtual Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3) = 0;

protected:
  /** Constructor. */
  Create_func_arg3() = default;
  /** Destructor. */
  virtual ~Create_func_arg3() = default;
};




/**
  Adapter for native functions with a variable number of arguments.
  The main use of this class is to discard the following calls:
  <code>foo(expr1 AS name1, expr2 AS name2, ...)</code>
  which are syntactically correct (the syntax can refer to a UDF),
  but semantically invalid for native functions.
*/

class Create_native_func : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  /**
    Builder method, with no arguments.
    @param thd The current thread
    @param name The native function name
    @param item_list The function parameters, none of which are named
    @return An item representing the function call
  */
  virtual Item *create_native(THD *thd, const LEX_CSTRING *name,
                              List<Item> *item_list) = 0;

protected:
  /** Constructor. */
  Create_native_func() = default;
  /** Destructor. */
  virtual ~Create_native_func() = default;
};


/**
  Function builder for qualified functions.
  This builder is used with functions call using a qualified function name
  syntax, as in <code>db.func(expr, expr, ...)</code>.
*/

class Create_qfunc : public Create_func
{
public:
  /**
    The builder create method, for unqualified functions.
    This builder will use the current database for the database name.
    @param thd The current thread
    @param name The function name
    @param item_list The list of arguments to the function, can be NULL
    @return An item representing the parsed function call
  */
  Item *create_func(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  /**
    The builder create method, for qualified functions.
    @param thd The current thread
    @param db The database name
    @param name The function name
    @param use_explicit_name Should the function be represented as 'db.name'?
    @param item_list The list of arguments to the function, can be NULL
    @return An item representing the parsed function call
  */
  virtual Item *create_with_db(THD *thd,
                               const LEX_CSTRING *db,
                               const LEX_CSTRING *name,
                               bool use_explicit_name,
                               List<Item> *item_list) = 0;

protected:
  /** Constructor. */
  Create_qfunc() = default;
  /** Destructor. */
  virtual ~Create_qfunc() = default;
};


/**
  Find the function builder for qualified functions.
  @param thd The current thread
  @return A function builder for qualified functions
*/
extern Create_qfunc * find_qualified_function_builder(THD *thd);


#ifdef HAVE_DLOPEN
/**
  Function builder for User Defined Functions.
*/

class Create_udf_func : public Create_func
{
public:
  Item *create_func(THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
    override;

  /**
    The builder create method, for User Defined Functions.
    @param thd The current thread
    @param fct The User Defined Function metadata
    @param item_list The list of arguments to the function, can be NULL
    @return An item representing the parsed function call
  */
  Item *create(THD *thd, udf_func *fct, List<Item> *item_list);

  /** Singleton. */
  static Create_udf_func s_singleton;

protected:
  /** Constructor. */
  Create_udf_func() = default;
  /** Destructor. */
  virtual ~Create_udf_func() = default;
};
#endif


struct Native_func_registry
{
  LEX_CSTRING name;
  Create_func *builder;
};


class Native_functions_hash: public HASH
{
public:
  Native_functions_hash()
  {
    bzero(this, sizeof(*this));
  }
  ~Native_functions_hash()
  {
    /*
      No automatic free because objects of this type
      are expected to be declared statically.
      The code in cleanup() calls my_hash_free() which may not work correctly
      at the very end of mariadbd shutdown.
      The the upper level code should call cleanup() explicitly.

      Unfortunatelly, it's not possible to use DBUG_ASSERT(!records) here,
      because the server terminates using exit() in some cases,
      e.g. in the test main.named_pipe with the "Create named pipe failed"
      error.
    */
  }
  bool init(size_t count);
  bool append(const Native_func_registry array[], size_t count);
  bool remove(const Native_func_registry array[], size_t count);
  bool replace(const Native_func_registry array[], size_t count)
  {
    DBUG_ENTER("Native_functions_hash::replace");
    remove(array, count);
    DBUG_RETURN(append(array, count));
  }
  void cleanup();
  /**
    Find the native function builder associated with a given function name.
    @param thd The current thread
    @param name The native function name
    @return The native function builder associated with the name, or NULL
  */
  Create_func *find(THD *thd, const LEX_CSTRING &name) const;
};

extern MYSQL_PLUGIN_IMPORT Native_functions_hash native_functions_hash;
extern MYSQL_PLUGIN_IMPORT Native_functions_hash native_functions_hash_oracle;

extern const Native_func_registry func_array[];
extern const size_t func_array_length;

int item_create_init();
void item_create_cleanup();

Item *create_func_dyncol_create(THD *thd, List<DYNCALL_CREATE_DEF> &list);
Item *create_func_dyncol_add(THD *thd, Item *str,
                             List<DYNCALL_CREATE_DEF> &list);
Item *create_func_dyncol_delete(THD *thd, Item *str, List<Item> &nums);
Item *create_func_dyncol_get(THD *thd, Item *num, Item *str,
                             const Type_handler *handler,
                             const Lex_length_and_dec_st &length_and_dec,
                             CHARSET_INFO *cs);
Item *create_func_dyncol_json(THD *thd, Item *str);


class Native_func_registry_array
{
  const Native_func_registry *m_elements;
  size_t m_count;
public:
  Native_func_registry_array()
   :m_elements(NULL),
    m_count(0)
  { }
  Native_func_registry_array(const Native_func_registry *elements, size_t count)
   :m_elements(elements),
    m_count(count)
  { }
  const Native_func_registry& element(size_t i) const
  {
    DBUG_ASSERT(i < m_count);
    return m_elements[i];
  }
  const Native_func_registry *elements() const { return m_elements; }
  size_t count() const { return m_count; }
};


#endif
