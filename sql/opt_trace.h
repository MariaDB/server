#ifndef OPT_TRACE_INCLUDED
#define OPT_TRACE_INCLUDED
/* This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "opt_trace_context.h"  // Opt_trace_context
#include "sql_lex.h"
#include "my_json_writer.h"
#include "sql_select.h"
class Item;
class THD;
struct TABLE_LIST;

/*
   User-visible information about a trace.
*/

struct Opt_trace_info
{
  /**
     String containing trace.
     If trace has been end()ed, this is 0-terminated, which is only to aid
     debugging or unit testing; this property is not relied upon in normal
     server usage.
     If trace has not been ended, this is not 0-terminated. That rare case can
     happen when a substatement reads OPTIMIZER_TRACE (at that stage, the top
     statement is still executing so its trace is not ended yet, but may still
     be read by the sub-statement).
  */
  const char *trace_ptr;
  size_t trace_length;
  //// String containing original query.
  const char *query_ptr;
  size_t query_length;
  const CHARSET_INFO *query_charset;  ///< charset of query string
  /**
    How many bytes this trace is missing (for traces which were truncated
    because of @@@@optimizer-trace-max-mem-size).
    The trace is not extended beyond trace-max-mem-size.
  */
  size_t missing_bytes;
  /*
    Whether user lacks privilege to see this trace.
    If this is set to TRUE, then we return an empty trace
  */
  bool missing_priv;
};

/**
  Instantiate this class to start tracing a THD's actions (generally at a
  statement's start), and to set the "original" query (not transformed, as
  sent by client) for the new trace. Destructor will end the trace.

  @param  thd          the THD
  @param  tbl          list of tables read/written by the statement.
  @param  sql_command  SQL command being prepared or executed
  @param  set_vars     what variables are set by this command (only used if
                       sql_command is SQLCOM_SET_OPTION)
  @param  query        query
  @param  length       query's length
  @param  charset      charset which was used to encode this query
*/


class Opt_trace_start
{
 public:
  Opt_trace_start(THD *thd_arg): ctx(&thd_arg->opt_trace), traceable(false) {}

  void init(THD *thd, TABLE_LIST *tbl,
            enum enum_sql_command sql_command,
            List<set_var_base> *set_vars,
            const char *query,
            size_t query_length,
            const CHARSET_INFO *query_charset);

  ~Opt_trace_start();

 private:
  Opt_trace_context *const ctx;
  /*
    True: the query will be traced
    False: otherwise
  */
  bool traceable;
};

/**
   Prints SELECT query to optimizer trace. It is not the original query (as in
   @c Opt_trace_context::set_query()) but a printout of the parse tree
   (Item-s).
   @param  thd         the THD
   @param  select_lex  query's parse tree
   @param  trace_object  Json_writer object to which the query will be added
*/
void opt_trace_print_expanded_query(THD *thd, SELECT_LEX *select_lex,
                                    Json_writer_object *trace_object);

void add_table_scan_values_to_trace(THD *thd, JOIN_TAB *tab);
void trace_plan_prefix(JOIN *join, uint idx, table_map join_tables);
void print_final_join_order(JOIN *join);
void print_best_access_for_table(THD *thd, POSITION *pos,
                                 enum join_type type);

void trace_condition(THD * thd, const char *name, const char *transform_type,
                    Item *item, const char *table_name= nullptr);


/*
  Security related (need to add a proper comment here)
*/

/**
   If the security context is not that of the connected user, inform the trace
   system that a privilege is missing. With one exception: see below.

   @param thd

   This serves to eliminate the following issue.
   Any information readable by a SELECT may theoretically end up in
   the trace. And a SELECT may read information from other places than tables:
   - from views (reading their bodies)
   - from stored routines (reading their bodies)
   - from files (reading their content), with LOAD_FILE()
   - from the list of connections (reading their queries...), with
   I_S.PROCESSLIST.
   If the connected user has EXECUTE privilege on a routine which does a
   security context change, the routine can retrieve information internally
   (if allowed by the SUID context's privileges), and present only a portion
   of it to the connected user. But with tracing on, all information is
   possibly in the trace. So the connected user receives more information than
   the routine's definer intended to provide.  Fixing this issue would require
   adding, near many privilege checks in the server, a new
   optimizer-trace-specific check done against the connected user's context,
   to verify that the connected user has the right to see the retrieved
   information.

   Instead, our chosen simpler solution is that if we see a security context
   change where SUID user is not the connected user, we disable tracing. With
   only one safe exception: if the connected user has all global privileges
   (because then she/he can find any information anyway). By "all global
   privileges" we mean everything but WITH GRANT OPTION (that latter one isn't
   related to information gathering).

   Read access to I_S.OPTIMIZER_TRACE by another user than the connected user
   is restricted: @see fill_optimizer_trace_info().
*/
void opt_trace_disable_if_no_security_context_access(THD *thd);

void opt_trace_disable_if_no_tables_access(THD *thd, TABLE_LIST *tbl);

/**
   If tracing is on, checks additional privileges for a view, to make sure
   that the user has the right to do SHOW CREATE VIEW. For that:
   - this function checks SHOW VIEW
   - SELECT is tested in opt_trace_disable_if_no_tables_access()
   - SELECT + SHOW VIEW is sufficient for SHOW CREATE VIEW.
   We also check underlying tables.
   If a privilege is missing, notifies the trace system.
   This function should be called when the view's underlying tables have not
   yet been merged.

   @param thd               THD context
   @param view              view to check
   @param underlying_tables underlying tables/views of 'view'
 */

void opt_trace_disable_if_no_view_access(THD *thd, TABLE_LIST *view,
                                         TABLE_LIST *underlying_tables);

/**
  If tracing is on, checks additional privileges on a stored routine, to make
  sure that the user has the right to do SHOW CREATE PROCEDURE/FUNCTION. For
  that, we use the same checks as in those SHOW commands.
  If a privilege is missing, notifies the trace system.

  This function is not redundant with
  opt_trace_disable_if_no_security_context_access().
  Indeed, for a SQL SECURITY INVOKER routine, there is no context change, but
  we must still verify that the invoker can do SHOW CREATE.

  For triggers, see note in sp_head::execute_trigger().

  @param thd
  @param sp  routine to check
 */
void opt_trace_disable_if_no_stored_proc_func_access(THD *thd, sp_head *sp);

/**
   Fills information_schema.OPTIMIZER_TRACE with rows (one per trace)
   @retval 0 ok
   @retval 1 error
*/
int fill_optimizer_trace_info(THD *thd, TABLE_LIST *tables, Item *);

#define OPT_TRACE_TRANSFORM(thd, object_level0, object_level1, \
                            select_number, from, to)             \
  Json_writer_object object_level0(thd);                         \
  Json_writer_object object_level1(thd, "transformation");       \
  object_level1.add_select_number(select_number).add("from", from).add("to", to);

#define OPT_TRACE_VIEWS_TRANSFORM(thd, object_level0, object_level1, \
                                  derived, name, select_number, algorithm) \
    Json_writer_object trace_wrapper(thd);              \
    Json_writer_object trace_derived(thd, derived);     \
    trace_derived.add("table", name).add_select_number(select_number)  \
                 .add("algorithm", algorithm);
#endif
