#ifndef JSON_TABLE_INCLUDED
#define JSON_TABLE_INCLUDED

/* Copyright (c) 2020, MariaDB Corporation. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */


#include <json_lib.h>

class Json_table_column;

/*
  The Json_table_nested_path represents the 'current nesting' level
  for a set of JSON_TABLE columns.
  Each column (Json_table_column instance) is linked with corresponding
  'nested path' object and gets its piece of JSON to parse during the computation
  phase.
  The root 'nested_path' is always present as a part of Table_function_json_table,
  then other 'nested_paths' can be created and linked into a tree structure when new
  'NESTED PATH' is met. The nested 'nested_paths' are linked with 'm_nested', the same-level
  'nested_paths' are linked with 'm_next_nested'.
  So for instance
    JSON_TABLE( '...', '$[*]'
       COLUMNS( a INT PATH '$.a' ,
          NESTED PATH '$.b[*]' COLUMNS (b INT PATH '$',
                                        NESTED PATH '$.c[*]' COLUMNS(x INT PATH '$')),
          NESTED PATH '$.n[*]' COLUMNS (z INT PATH '$'))
  results in 4 'nested_path' created:
                 root          nested_b       nested_c     nested_n
  m_path           '$[*]'         '$.b[*]'        '$.c[*]'     '$.n[*]
  m_nested          &nested_b     &nested_c       NULL         NULL
  n_next_nested     NULL          &nested_n       NULL         NULL

  and 4 columns created:
              a          b            x            z
  m_nest    &root      &nested_b    &nested_c    &nested_n
*/

class Json_table_nested_path : public Sql_alloc
{
public:
  json_path_t m_path;  /* The JSON Path to get the rows from */
  bool m_null; // TRUE <=> producing a NULL-complemented row.

  /*** Construction interface ***/
  Json_table_nested_path():
    m_null(TRUE), m_nested(NULL), m_next_nested(NULL)
  {}

  int set_path(THD *thd, const LEX_CSTRING &path);

  /*** Methods for performing a scan ***/
  void scan_start(CHARSET_INFO *i_cs, const uchar *str, const uchar *end);
  int scan_next();
  bool check_error(const char *str);

  /*** Members for getting the values we've scanned to ***/
  const uchar *get_value() { return m_engine.value_begin; }
  const uchar *get_value_end() { return m_engine.s.str_end; }

  /* Counts the rows produced. Used by FOR ORDINALITY columns */
  longlong m_ordinality_counter;

  int print(THD *thd, Field ***f, String *str,
            List_iterator_fast<Json_table_column> &it,
            Json_table_column **last_column);
private:
  /* The head of the list of nested NESTED PATH statements. */
  Json_table_nested_path *m_nested;

  /* in the above list items are linked with the */
  Json_table_nested_path *m_next_nested;

  /*** Members describing NESTED PATH structure ***/
  /* Parent nested path. The "root" path has this NULL */
  Json_table_nested_path *m_parent;

  /*** Members describing current JSON Path scan state ***/
  /* The JSON Parser and JSON Path evaluator */
  json_engine_t m_engine;

  /* The path the parser is currently pointing to */
  json_path_t m_cur_path;

  /* The child NESTED PATH we're currently scanning */
  Json_table_nested_path *m_cur_nested;

  static bool column_in_this_or_nested(const Json_table_nested_path *p,
                                       const Json_table_column *jc);
  friend class Table_function_json_table;
};


/*
  @brief
    Describes the column definition in JSON_TABLE(...) syntax.

  @detail
    Has methods for printing/handling errors but otherwise it's a static
    object.
*/

class Json_table_column : public Sql_alloc
{
public:
  enum enum_type
  {
    FOR_ORDINALITY,
    PATH,
    EXISTS_PATH
  };

  enum enum_on_type
  {
    ON_EMPTY,
    ON_ERROR
  };

  enum enum_on_response
  {
    RESPONSE_NOT_SPECIFIED,
    RESPONSE_ERROR,
    RESPONSE_NULL,
    RESPONSE_DEFAULT
  };

  struct On_response
  {
  public:
    Json_table_column::enum_on_response m_response;
    LEX_CSTRING m_default;
    int respond(Json_table_column *jc, Field *f, uint error_num);
    int print(const char *name, String *str) const;
    bool specified() const { return m_response != RESPONSE_NOT_SPECIFIED; }
  };

  enum_type m_column_type;
  json_path_t m_path;
  On_response m_on_error;
  On_response m_on_empty;
  Create_field *m_field;
  Json_table_nested_path *m_nest;
  CHARSET_INFO *m_explicit_cs;
  CHARSET_INFO *m_defaults_cs;

  void set(enum_type ctype)
  {
    m_column_type= ctype;
  }
  int set(THD *thd, enum_type ctype, const LEX_CSTRING &path, CHARSET_INFO *cs);
  int set(THD *thd, enum_type ctype, const LEX_CSTRING &path,
          const Lex_charset_collation_st &cl);
  Json_table_column(Create_field *f, Json_table_nested_path *nest) :
    m_field(f), m_nest(nest), m_explicit_cs(NULL)
  {
    m_on_error.m_response= RESPONSE_NOT_SPECIFIED;
    m_on_empty.m_response= RESPONSE_NOT_SPECIFIED;
  }
  int print(THD *tnd, Field **f, String *str);
};


/*
  Class represents the table function, the function
  that returns the table as a result so supposed to appear
  in the FROM list of the SELECT statement.
  At the moment there is only one such function JSON_TABLE,
  so the class named after it, but should be refactored
  into the hierarchy root if we create more of that functions.

  As the parser finds the table function in the list it
  creates an instance of Table_function_json_table storing it
  into the TABLE_LIST::table_function.
  Then the ha_json_table instance is created based on it in
  the create_table_for_function().

  == Replication: whether JSON_TABLE is deterministic ==

  In sql_yacc.yy, we set BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION whenever
  JSON_TABLE is used. The reasoning behind this is as follows:

  In the current MariaDB code, evaluation of JSON_TABLE is deterministic,
  that is, for a given input string JSON_TABLE will always produce the same
  set of rows in the same order.  However one can think of JSON documents
  that one can consider indentical which will produce different output.
  In order to be feature-proof and withstand changes like:
  - sorting JSON object members by name (like MySQL does)
  - changing the way duplicate object members are handled
  we mark the function as SBR-unsafe.
  (If there is ever an issue with this, marking the function as SBR-safe
   is a non-intrusive change we will always be able to make)
*/

class Table_function_json_table : public Sql_alloc
{
public:
  /*** Basic properties of the original JSON_TABLE(...) ***/
  Item *m_json; /* The JSON value to be parsed. */

  /* The COLUMNS(...) part representation. */
  Json_table_nested_path m_nested_path;

  /* The list of table column definitions. */
  List<Json_table_column> m_columns;

  /*** Name resolution functions ***/
  bool setup(THD *thd, TABLE_LIST *sql_table, SELECT_LEX *s_lex);

  int walk_items(Item_processor processor, bool walk_subquery,
                 void *argument);

  /*** Functions for interaction with the Query Optimizer ***/
  void fix_after_pullout(TABLE_LIST *sql_table,
                         st_select_lex *new_parent, bool merge);
  void update_used_tables() { m_json->update_used_tables(); }

  table_map used_tables() const { return m_json->used_tables(); }
  bool join_cache_allowed() const
  {
    /*
      Can use join cache when we have an outside reference.
      If there's dependency on any other table or randomness,
      cannot use it.
    */
    return !(used_tables() & ~OUTER_REF_TABLE_BIT);
  }
  void get_estimates(ha_rows *out_rows,
                     double *scan_time, double *startup_cost);

  int print(THD *thd, TABLE_LIST *sql_table,
            String *str, enum_query_type query_type);

  /*** Construction interface to be used from the parser ***/
  Table_function_json_table(Item *json):
    m_json(json),
    m_context_setup_done(false)
  {
    cur_parent= &m_nested_path;
    last_sibling_hook= &m_nested_path.m_nested;
  }

  void start_nested_path(Json_table_nested_path *np);
  void end_nested_path();
  Json_table_nested_path *get_cur_nested_path() { return cur_parent; }
  void set_name_resolution_context(Name_resolution_context *arg)
  {
    m_context= arg;
  }

  /* SQL Parser: current column in JSON_TABLE (...) syntax */
  Json_table_column *m_cur_json_table_column;

  /* SQL Parser: charset of the current text literal */
  CHARSET_INFO *m_text_literal_cs;

private:
  /* Context to be used for resolving the first argument. */
  Name_resolution_context *m_context;

  bool m_context_setup_done;

  /* Current NESTED PATH level being parsed */
  Json_table_nested_path *cur_parent;

  /*
    Pointer to the list tail where we add the next NESTED PATH.
    It points to the cur_parnt->m_nested for the first nested
    and prev_nested->m_next_nested for the coesequent ones.
  */
  Json_table_nested_path **last_sibling_hook;
};

bool push_table_function_arg_context(LEX *lex, MEM_ROOT *alloc);

TABLE *create_table_for_function(THD *thd, TABLE_LIST *sql_table);

table_map add_table_function_dependencies(List<TABLE_LIST> *join_list,
                                          table_map nest_tables);

#endif /* JSON_TABLE_INCLUDED */

