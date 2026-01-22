/*
   Copyright (c) 2002, 2016, Oracle and/or its affiliates.
   Copyright (c) 2011, 2024, MariaDB

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

#include "sp_instr.h"

#include "opt_trace.h"    // class Opt_trace_start
#include "sql_array.h"    // class Dynamic_array
#include "sql_audit.h"    // mysql_audit_general
#include "sql_base.h"     // open_and_lock_tables
#include "sql_derived.h"  // mysql_handle_derived
#include "sp_head.h"      // class sp_head
#include "sql_parse.h"    // check_table_access
#include "sp_rcontext.h"  // class sp_rcontext
#include "sql_prepare.h"  // reinit_stmt_before_use
#include "transaction.h"  // trans_commit_stmt, trans_rollback_stmt, ...

/*
  Sufficient max length of printed destinations.
*/
static const int SP_STMT_PRINT_MAXLEN= 40;

static int cmp_rqp_locations(const void *a_, const void *b_)
{
  auto a= static_cast<Rewritable_query_parameter *const *>(a_);
  auto b= static_cast<Rewritable_query_parameter *const *>(b_);
  return (int)((*a)->pos_in_query - (*b)->pos_in_query);
}


/**
  Traverse the list of Item_param instances created on the fist parsing of
  SP instruction's statement and put them back into sp_inst_lex->free list
  for releasing them on deallocating statement's resources to avoid
  memory leaks.
*/

void
sp_lex_instr::put_back_item_params(THD *thd, LEX *lex,
                                   const List<Item_param>& param_values)
{
  /*
    Instance of Item_param must be ignored on re-parsing a statement
    of failed SP instruction, therefore lex->param_list must be empty.
    Instance of the class Item_param created on first (initial) parsing of
    Prepared Statement is used for whole its life.
  */
  DBUG_ASSERT(lex->param_list.is_empty());

  for (auto it= param_values.begin();
       it != param_values.end(); ++it)
  {
    /*
      Put retained instances of Item_param back into sp_lex_inst::free_list
      to avoid leaking them. Original ordering of Item_param objects
      are preserved since param_values contains items in reverse order.
    */
    Item_param *param_for_adding_to_free_list= it.operator->();

    Item *prev_head= free_list;
    free_list= param_for_adding_to_free_list;
    param_for_adding_to_free_list->next= prev_head;
  }
}


static constexpr LEX_CSTRING cursor_str= {C_STRING_WITH_LEN("cursor")};

/*
  Print the instruction name with an array variable element:
  @param str [OUT]     The destination string
  @param cmd           The instruction name
  @param rcontext_name The name of the array rcontext
  @param array_name    The array name
  @param index_offest  The offset of the index variable.

  Example: "cclose SESSION.cursor[c@1]"
  - cclose is the command name
  - SESSION is the name of the cursor rcontext
  - c@1 is the index variable name and offset
*/
void sp_instr::print_cmd_and_array_element(String *str,
                                           const LEX_CSTRING &cmd,
                                           const LEX_CSTRING &rcontext_name,
                                           const LEX_CSTRING &array_name,
                                           uint index_offset) const
{
  const sp_variable *pv= m_ctx->find_variable(index_offset);
  size_t rsrv= cmd.length + 1/*space*/ +
               rcontext_name.length +
               array_name.length + 2/*[]*/ +
               (pv ? pv->name.length + 1/*@*/ + SP_INSTR_UINT_MAXLEN : 0);
  if (str->reserve(rsrv))
    return;
  str->qs_append(cmd.str, cmd.length);
  str->qs_append(' ');
  if (pv)
  {
    str->qs_append(&rcontext_name);
    str->qs_append(&array_name);
    str->qs_append('[');
    str->qs_append(&pv->name);
    str->qs_append('@');
    str->qs_append(pv->offset);
    str->qs_append(']');
  }
}

/*
  StoredRoutinesBinlogging
  This paragraph applies only to statement-based binlogging. Row-based
  binlogging does not need anything special like this.

  Top-down overview:

  1. Statements

  Statements that have is_update_query(stmt) == true are written into the
  binary log verbatim.
  Examples:
    UPDATE tbl SET tbl.x = spfunc_w_side_effects()
    UPDATE tbl SET tbl.x=1 WHERE spfunc_w_side_effect_that_returns_false(tbl.y)

  Statements that have is_update_query(stmt) == false (e.g. SELECTs) are not
  written into binary log. Instead we catch function calls the statement
  makes and write it into binary log separately (see #3).

  2. PROCEDURE calls

  CALL statements are not written into binary log. Instead
  * Any FUNCTION invocation (in SET, IF, WHILE, OPEN CURSOR and other SP
    instructions) is written into binlog separately.

  * Each statement executed in SP is binlogged separately, according to rules
    in #1, with the exception that we modify query string: we replace uses
    of SP local variables with NAME_CONST('spvar_name', <spvar-value>) calls.
    This substitution is done in subst_spvars().

  3. FUNCTION calls

  In sp_head::execute_function(), we check
   * If this function invocation is done from a statement that is written
     into the binary log.
   * If there were any attempts to write events to the binary log during
     function execution (grep for start_union_events and stop_union_events)

   If the answers are No and Yes, we write the function call into the binary
   log as "SELECT spfunc(<param1value>, <param2value>, ...)"


  4. Miscellaneous issues.

  4.1 User variables.

  When we call mysql_bin_log.write() for an SP statement, thd->user_var_events
  must hold set<{var_name, value}> pairs for all user variables used during
  the statement execution.
  This set is produced by tracking user variable reads during statement
  execution.

  For SPs, this has the following implications:
  1) thd->user_var_events may contain events from several SP statements and
     needs to be valid after exection of these statements was finished. In
     order to achieve that, we
     * Allocate user_var_events array elements on appropriate mem_root (grep
       for user_var_events_alloc).
     * Use is_query_in_union() to determine if user_var_event is created.

  2) We need to empty thd->user_var_events after we have wrote a function
     call. This is currently done by making
     reset_dynamic(&thd->user_var_events);
     calls in several different places. (TODO consider moving this into
     mysql_bin_log.write() function)

  4.2 Auto_increment storage in binlog

  As we may write two statements to binlog from one single logical statement
  (case of "SELECT func1(),func2()": it is binlogged as "SELECT func1()" and
  then "SELECT func2()"), we need to reset auto_increment binlog variables
  after each binlogged SELECT. Otherwise, the auto_increment value of the
  first SELECT would be used for the second too.
*/


/**
  Replace thd->query{_length} with a string that one can write to
  the binlog.

  The binlog-suitable string is produced by replacing references to SP local
  variables with NAME_CONST('sp_var_name', value) calls.

  @param thd        Current thread.
  @param instr      Instruction (we look for Item_splocal instances in
                    instr->free_list)
  @param query_str  Original query string

  @return
    - false  on success.
    thd->query{_length} either has been appropriately replaced or there
    is no need for replacements.
    - true   out of memory error.
*/

static bool
subst_spvars(THD *thd, sp_instr *instr, LEX_STRING *query_str)
{
  DBUG_ENTER("subst_spvars");

  Dynamic_array<Rewritable_query_parameter*> rewritables(PSI_INSTRUMENT_MEM);
  char *pbuf;
  StringBuffer<512> qbuf;
  Copy_query_with_rewrite acc(thd, query_str->str, query_str->length, &qbuf);

  /* Find rewritable Items used in this statement */
  for (Item *item= instr->free_list; item; item= item->next)
  {
    Rewritable_query_parameter *rqp= item->get_rewritable_query_parameter();
    if (rqp && rqp->pos_in_query)
      rewritables.append(rqp);
  }
  if (!rewritables.elements())
    DBUG_RETURN(false);

  rewritables.sort(cmp_rqp_locations);

  thd->query_name_consts= (uint)rewritables.elements();

  for (Rewritable_query_parameter **rqp= rewritables.front();
       rqp <= rewritables.back(); rqp++)
  {
    if (acc.append(*rqp))
      DBUG_RETURN(true);
  }
  if (acc.finalize())
    DBUG_RETURN(true);

  /*
    Allocate additional space at the end of the new query string for the
    query_cache_send_result_to_client function.

    The query buffer layout is:
       buffer :==
            <statement>   The input statement(s)
            '\0'          Terminating null char
            <length>      Length of following current database name 2
            <db_name>     Name of current database
            <flags>       Flags struct
  */
  size_t buf_len= (qbuf.length() + 1 + QUERY_CACHE_DB_LENGTH_SIZE +
                thd->db.length + QUERY_CACHE_FLAGS_SIZE + 1);
  if ((pbuf= (char *) alloc_root(thd->mem_root, buf_len)))
  {
    char *ptr= pbuf + qbuf.length();
    memcpy(pbuf, qbuf.ptr(), qbuf.length());
    *ptr= 0;
    int2store(ptr+1, thd->db.length);
  }
  else
    DBUG_RETURN(true);

  thd->set_query(pbuf, qbuf.length());

  DBUG_RETURN(false);
}


#ifndef DBUG_OFF
/*
  Check if all rewrittable query params in an instruction are fixed.
  They can be fixed e.g. if append_for_log() already happened.
*/
bool dbug_rqp_are_fixed(sp_instr *instr)
{
  for (Item *item= instr->free_list; item; item= item->next)
  {
    Rewritable_query_parameter *rqp= item->get_rewritable_query_parameter();
    if (rqp && rqp->pos_in_query && !item->fixed())
      return false;
  }
  return true;
}
#endif


/**
  Prepare LEX and thread for execution of instruction, if requested open
  and lock LEX's tables, execute instruction's core function, perform
  cleanup afterwards.

  @param thd           thread context
  @param nextp         out - next instruction
  @param open_tables   if true then check read access to tables in LEX's table
                       list and open and lock them (used in instructions which
                       need to calculate some expression and don't execute
                       complete statement).
  @param instr         instruction for which we prepare context, and which core
                       function execute by calling its exec_core() method.
  @param rerun_the_same_instr  true in case the instruction is re-run after
                               a SQL statement associated with it has been
                               re-parsed.

  @note
    We are not saving/restoring some parts of THD which may need this because
    we do this once for whole routine execution in sp_head::execute().

  @return
    0/non-0 - Success/Failure
*/

int
sp_lex_keeper::reset_lex_and_exec_core(THD *thd, uint *nextp,
                                       bool open_tables, sp_instr* instr,
                                       bool rerun_the_same_instr)
{
  int res= 0;
  DBUG_ENTER("reset_lex_and_exec_core");

  /*
    The flag is saved at the entry to the following substatement.
    It's reset further in the common code part.
    It's merged with the saved parent's value at the exit of this func.
  */
  bool parent_modified_non_trans_table=
    thd->transaction->stmt.modified_non_trans_table;
  unsigned int parent_unsafe_rollback_flags=
    thd->transaction->stmt.m_unsafe_rollback_flags;
  thd->transaction->stmt.modified_non_trans_table= false;
  thd->transaction->stmt.m_unsafe_rollback_flags= 0;

  DBUG_ASSERT(!thd->derived_tables);
  /*
    Item*::append_for_log() called from subst_spvars (which already happened
    at this point) can create new Items in some cases. For example:
      INSERT INTO t1 VALUES
       (assoc_array(spvar_latin1 || CONVERT(' ' USING ucs2)));
    wraps CONVERT into Item_func_conv_charset.
  */
  DBUG_ASSERT_NO_ASSUME(dbug_rqp_are_fixed(instr) ||
                        thd->Item_change_list::is_empty());
  /*
    Use our own lex.
    We should not save old value since it is saved/restored in
    sp_head::execute() when we are entering/leaving routine.
  */
  thd->lex= m_lex;

  /*
    If the instruction is re-run by a reason of metadata change, then re-use
    current query id rather than set a new one. Doing this way we retain
    warnings generated on running the SP instruction. If a new query id was set
    it would result in clearing all accumulated warnings in
      mysql_execute_command
    on calling
      thd->get_stmt_da()->opt_clear_warning_info(thd->query_id)
    since in this case Warning_info::m_warn_id != thd->query_id.

    @sa Warning_info::opt_clear()
  */
  if (!rerun_the_same_instr)
    thd->set_query_id(next_query_id());

  if (thd->locked_tables_mode <= LTM_LOCK_TABLES)
  {
    /*
      This statement will enter/leave prelocked mode on its own.
      Entering prelocked mode changes table list and related members
      of LEX, so we'll need to restore them.
    */
    if (lex_query_tables_own_last)
    {
      /*
        We've already entered/left prelocked mode with this statement.
        Attach the list of tables that need to be prelocked and mark m_lex
        as having such list attached.
      */
      *lex_query_tables_own_last= prelocking_tables;
      m_lex->mark_as_requiring_prelocking(lex_query_tables_own_last);
    }
  }

  reinit_stmt_before_use(thd, m_lex);

#ifndef EMBEDDED_LIBRARY
  /*
    If there was instruction which changed tracking state,
    the result of changed tracking state send to client in OK packed.
    So it changes result sent to client and probably can be different
    independent on query text. So we can't cache such results.
  */
  if ((thd->client_capabilities & CLIENT_SESSION_TRACK) &&
      (thd->server_status & SERVER_SESSION_STATE_CHANGED))
    thd->lex->safe_to_cache_query= 0;
#endif

  Opt_trace_start ots(thd);
  ots.init(thd, m_lex->query_tables, SQLCOM_SELECT, &m_lex->var_list,
           nullptr, 0, thd->variables.character_set_client);

  Json_writer_object trace_command(thd);
  Json_writer_array trace_command_steps(thd, "steps");
  if (open_tables)
    res= instr->exec_open_and_lock_tables(thd, m_lex->query_tables);

  if (likely(!res))
  {
    res= instr->exec_core(thd, nextp);
    DBUG_PRINT("info",("exec_core returned: %d", res));
  }

  /*
    Call after unit->cleanup() to close open table
    key read.
  */
  if (open_tables)
  {
    m_lex->unit.cleanup();
    /* Here we also commit or rollback the current statement. */
    if (! thd->in_sub_stmt)
    {
      thd->get_stmt_da()->set_overwrite_status(true);
      thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd);
      thd->get_stmt_da()->set_overwrite_status(false);
    }
    close_thread_tables(thd);
    thd_proc_info(thd, 0);

    if (! thd->in_sub_stmt)
    {
      if (thd->transaction_rollback_request)
      {
        trans_rollback_implicit(thd);
        thd->release_transactional_locks();
      }
      else if (! thd->in_multi_stmt_transaction_mode())
        thd->release_transactional_locks();
      else
        thd->mdl_context.release_statement_locks();
    }
  }
  //TODO: why is this here if log_slow_query is in sp_instr_stmt::execute?
  delete_explain_query(m_lex);

  if (m_lex->query_tables_own_last)
  {
    /*
      We've entered and left prelocking mode when executing statement
      stored in m_lex.
      m_lex->query_tables(->next_global)* list now has a 'tail' - a list
      of tables that are added for prelocking. (If this is the first
      execution, the 'tail' was added by open_tables(), otherwise we've
      attached it above in this function).
      Now we'll save the 'tail', and detach it.
    */
    lex_query_tables_own_last= m_lex->query_tables_own_last;
    prelocking_tables= *lex_query_tables_own_last;
    *lex_query_tables_own_last= nullptr;
    m_lex->query_tables_last= m_lex->query_tables_own_last;
    m_lex->mark_as_requiring_prelocking(nullptr);
  }
  thd->rollback_item_tree_changes();
  /*
    Update the state of the active arena if no errors on
    open_tables stage.
  */
  if (likely(!res) || likely(!thd->is_error()))
    thd->stmt_arena->state= Query_arena::STMT_EXECUTED;

  /*
    Merge here with the saved parent's values
    what is needed from the substatement gained
  */
  thd->transaction->stmt.modified_non_trans_table |= parent_modified_non_trans_table;
  thd->transaction->stmt.m_unsafe_rollback_flags |= parent_unsafe_rollback_flags;

  TRANSACT_TRACKER(add_trx_state_from_thd(thd));

  /*
    Unlike for PS we should not call Item's destructors for newly created
    items after execution of each instruction in stored routine. This is
    because SP often create Item (like Item_int, Item_string etc...) when
    they want to store some value in local variable, pass return value and
    etc... So their life time should be longer than one instruction.

    cleanup_items() is called in sp_head::execute()
  */
  thd->lex->restore_set_statement_var();
  DBUG_RETURN(res || thd->is_error());
}


void sp_lex_keeper::free_lex(THD *thd)
{
  /*
    Currently, m_lex_resp == false for sp_instr_cursor_copy_struct instructions
    and in some cases for sp_instr_set instructions. For these classes
    free_lex() returns control flow immediately and doesn't change m_lex.
  */
  if (!m_lex_resp || !m_lex) return;

  /* Prevent endless recursion. */
  m_lex->sphead= nullptr;
  lex_end(m_lex);

  sp_lex_cursor* cursor_lex= m_lex->get_lex_for_cursor();
  if (cursor_lex == nullptr)
  {
    delete (st_lex_local *)m_lex;
    /*
      In case it is not sp_lex_cursor set thd->lex to the null value
      if it points to a LEX object we just deleted in order to avoid
      dangling pointers problem.
    */
    if (thd->lex == m_lex)
      thd->lex= nullptr;

    m_lex= nullptr;
    m_lex_resp= false;
  }
  else
  {
    /*
      sp_lex_cursor has references to items allocated on parsing a cursor
      declaration statement. These items are deleted on re-parsing a failing
      cursor declaration statement at the method
        sp_lex_instr::cleanup_before_parsing.
      Remove the reference to items that will be deleted from sp_lex_cursor
      in order to avoid dangling pointers problem.
    */
    cleanup_items(cursor_lex->free_list);
    cursor_lex->free_list= nullptr;
  }

  lex_query_tables_own_last= nullptr;
}


void sp_lex_keeper::set_lex(LEX *lex)
{
  m_lex= lex;
  m_lex_resp= true;
  m_lex->sp_lex_in_use= true;
}


int sp_lex_keeper::validate_lex_and_exec_core(THD *thd, uint *nextp,
                                              bool open_tables,
                                              sp_lex_instr* instr)
{
  Reprepare_observer reprepare_observer;
  bool rerun_the_same_instr= false;

  while (true)
  {
    if (instr->is_invalid() || m_lex->needs_reprepare)
    {
      thd->clear_error();
      free_lex(thd);
      LEX *lex= instr->parse_expr(thd, thd->spcont->m_sp, m_lex);

      if (!lex) return true;

      /*
        m_lex != nullptr in case it points to sp_lex_cursor.
      */
      if (m_lex == nullptr)
        set_lex(lex);

      m_first_execution= true;
      rerun_the_same_instr= true;
    }

    Reprepare_observer *stmt_reprepare_observer= nullptr;

    if (!m_first_execution &&
        ((sql_command_flags[m_lex->sql_command] & CF_REEXECUTION_FRAGILE) ||
         m_lex->sql_command == SQLCOM_END))
    {
      reprepare_observer.reset_reprepare_observer();
      stmt_reprepare_observer= &reprepare_observer;
    }

    Reprepare_observer *save_reprepare_observer= thd->m_reprepare_observer;
    thd->m_reprepare_observer= stmt_reprepare_observer;

    bool rc= reset_lex_and_exec_core(thd, nextp, open_tables, instr,
                                     rerun_the_same_instr);

    thd->m_reprepare_observer= save_reprepare_observer;

    m_first_execution= false;

    if (!rc)
      break;

    /*
      Raise the error upper level in case:
        - we got an error and Reprepare_observer is not set
        - a fatal error has been got
        - the current execution thread has been killed
        - an error different from ER_NEED_REPREPARE has been got.
    */
    if (stmt_reprepare_observer == nullptr ||
        thd->is_fatal_error ||
        thd->killed ||
        thd->get_stmt_da()->get_sql_errno() != ER_NEED_REPREPARE)
      return 1;

    if (!stmt_reprepare_observer->can_retry())
    {
      /*
        Reprepare_observer sets error status in DA but Sql_condition is not
        added. Please check Reprepare_observer::report_error(). Pushing
        Sql_condition for ER_NEED_REPREPARE here.
      */
      Diagnostics_area *da= thd->get_stmt_da();
      da->push_warning(thd, da->get_sql_errno(), da->get_sqlstate(),
                       Sql_state_errno_level::WARN_LEVEL_ERROR, da->message());
      return 1;
    }

    instr->invalidate();
  }

  return 0;
}


int sp_lex_keeper::cursor_reset_lex_and_exec_core(THD *thd, uint *nextp,
                                                  bool open_tables,
                                                  sp_lex_instr *instr)
{
  Query_arena *old_arena= thd->stmt_arena;
  /*
    Get the Query_arena from the cursor statement LEX, which contains
    the free_list of the query, so new items (if any) are stored in
    the right free_list, and we can cleanup after each cursor operation,
    e.g. open or cursor_copy_struct (for cursor%ROWTYPE variables).
  */
  thd->stmt_arena= m_lex->query_arena();
  int res= validate_lex_and_exec_core(thd, nextp, open_tables, instr);
  cleanup_items(thd->stmt_arena->free_list);
  thd->stmt_arena= old_arena;
  return res;
}

/*
  sp_instr class functions
*/

void sp_instr::print_fetch_into(String *str, List<sp_fetch_target> varlist)
{
  List_iterator_fast<sp_fetch_target> li(varlist);
  sp_fetch_target *pv;
  while ((pv= li++))
  {
    const LEX_CSTRING *prefix= pv->rcontext_handler()->get_name_prefix();
    if (str->reserve(pv->name.length + prefix->length + SP_INSTR_UINT_MAXLEN+2))
      return;
    str->qs_append(' ');
    str->qs_append(prefix);
    str->qs_append(&pv->name);
    str->qs_append('@');
    str->qs_append(pv->offset());
  }
}


int sp_instr::exec_open_and_lock_tables(THD *thd, TABLE_LIST *tables)
{
  int result;

  /*
    Check whenever we have access to tables for this statement
    and open and lock them before executing instructions core function.
  */
  if (thd->open_temporary_tables(tables) ||
      check_table_access(thd, SELECT_ACL, tables, false, UINT_MAX, false)
      || open_and_lock_tables(thd, tables, true, 0))
    result= -1;
  else
    result= 0;
  /* Prepare all derived tables/views to catch possible errors. */
  if (!result)
    result= mysql_handle_derived(thd->lex, DT_PREPARE) ? -1 : 0;

  return result;
}

uint sp_instr::get_cont_dest() const
{
  return (m_ip+1);
}


int sp_instr::exec_core(THD *thd, uint *nextp)
{
  DBUG_ASSERT(0);
  return 0;
}

void sp_lex_instr::get_query(String *sql_query) const
{
  LEX_CSTRING expr_query= get_expr_query();

  /*
    the expression string must me initialized in constructor of a derived class
  */
  DBUG_ASSERT(expr_query.str != null_clex_str.str &&
              expr_query.length != null_clex_str.length);

  /*
    Leave the method in case of empty query string.
  */
  if (!expr_query.length)
    return;

  sql_query->append(C_STRING_WITH_LEN("SELECT "));
  sql_query->append(expr_query);
}


List<Item_param>
sp_lex_instr::cleanup_before_parsing(enum_sp_type sp_type)
{
  Item *current= free_list;
  List<Item_param> param_values{};

  while (current)
  {
    Item *next= current->next;

    if (current->is_stored_routine_parameter())
      /*
        `current` points to an instance of the class Item_param.
        Place an instance of the class Item_param into the list `param_values`
        and skip the item in free_list (don't invoke the method delete_self()
        on it). Since the `free_list` stores items in reverse order of creation
        (that is the last created item is the one pointed by the `free_list`),
        place items in the list `param_values` using push_front to save
        original ordering of items
      */
      param_values.push_front((Item_param*)current);
    else
      current->delete_self();

    current= next;
  }

  free_list= nullptr;

  if (sp_type == SP_TYPE_TRIGGER)
    /*
      Some of deleted items can be referenced from the list
      m_cur_trigger_stmt_items. Clean up the list content to avoid
      dangling references.
    */
    m_cur_trigger_stmt_items.empty();

  return param_values;
}


/**
  Set up field object for every NEW/OLD item of the trigger.

  @param thd  current thread
  @param sp   sp_head object of the trigger
*/

bool sp_lex_instr::setup_table_fields_for_trigger(
  THD *thd, sp_head *sp,
  SQL_I_List<Item_trigger_field> *next_trig_items_list)
{
  bool result= false;

  DBUG_ASSERT(sp->m_trg);

  for (Item_trigger_field *trg_field= sp->m_cur_instr_trig_field_items.first;
       trg_field;
       trg_field= trg_field->next_trg_field)
  {
    trg_field->setup_field(thd, sp->m_trg->base->get_subject_table(),
                           &sp->m_trg->subject_table_grants);
    result= trg_field->fix_fields_if_needed(thd, (Item **)0);
  }

  /*
    Move the list of Item_trigger_field objects, that have just been
    filled in on parsing the trigger's statement, into the instruction list
    owned by SP instruction.
  */
  if (sp->m_cur_instr_trig_field_items.elements)
  {
    sp->m_cur_instr_trig_field_items.save_and_clear(
        &m_cur_trigger_stmt_items);
    m_cur_trigger_stmt_items.first->next_trig_field_list= next_trig_items_list;
  }

  return result;
}


/**
  Initialize a new memory root for re-parsing a failed SP instruction's
  statement or free a memory allocated on re-parsing of the failed statement
  and re-initialize it again so to avoid memory leaks on repeating a statement
  re-parsing.

  @param sphead  The stored program.
  @param[out] new_memroot_allocated  true in case a new memory root for
                                     re-parsing was created, else false meaning
                                     that already allocated memory root is
                                     reused

  @return false on success, true on error (OOM)
*/

bool sp_lex_instr::setup_memroot_for_reparsing(sp_head *sphead,
                                               bool *new_memroot_allocated)
{
  if (!m_mem_root_for_reparsing)
  {
    DBUG_EXECUTE_IF("sp_instr_reparsing_2nd_time", DBUG_ASSERT(0););
    /*
      Allocate a memory for SP-instruction's mem_root on a mem_root of sp_head.
      Since the method sp_lex_instr::setup_memroot_for_reparsing() is called
      on failing execution of SP-instruction by the reason of changes in data
      dictionary objects metadata, the sp_head mem_root protection flag could
      has been already set on first execution of the stored routine. Therefore,
      clear the flag
        ROOT_FLAG_READ_ONLY
      in case it is set before allocating a memory for SP instruction's
      mem_root on sp_head's mem_root and restore its original value once
      the memory for the SP-instruction's new_root allocated. Read only
      property for the stored routine's mem_root can be not set after first
      invocation of a stored routine in case it was completed with error.
      So, check the flag is set before resetting its value and restoring its
      original value on return.
    */
    MEM_ROOT *sphead_mem_root= sphead->get_main_mem_root();

#ifdef PROTECT_STATEMENT_MEMROOT
    const bool read_only_mem_root=
      (sphead_mem_root->flags & ROOT_FLAG_READ_ONLY);

    if (read_only_mem_root)
      sphead_mem_root->flags&= ~ROOT_FLAG_READ_ONLY;
#endif

    m_mem_root_for_reparsing=
      (MEM_ROOT*)alloc_root(sphead_mem_root, sizeof(MEM_ROOT));

#ifdef PROTECT_STATEMENT_MEMROOT
    if (read_only_mem_root)
      /*
        Restore original read only property of sp_head' s mem_root
        in case it was set
      */
      sphead_mem_root->flags|= ROOT_FLAG_READ_ONLY;
#endif

    if (!m_mem_root_for_reparsing)
      return true;

    *new_memroot_allocated= true;
  }
  else
  {
    DBUG_EXECUTE_IF("sp_instr_reparsing_1st_time", DBUG_ASSERT(0););
    /*
      Free a memory allocated on SP-instruction's mem_root to avoid
      memory leaks could take place on recompilation of SP-instruction's
      statement.
    */
    free_root(m_mem_root_for_reparsing, MYF(0));
    *new_memroot_allocated= false;
  }

  init_sql_alloc(key_memory_sp_head_main_root, m_mem_root_for_reparsing,
                 MEM_ROOT_BLOCK_SIZE, MEM_ROOT_PREALLOC, MYF(0));

  mem_root= m_mem_root_for_reparsing;

  return false;
}


LEX* sp_lex_instr::parse_expr(THD *thd, sp_head *sp, LEX *sp_instr_lex)
{
  String sql_query;

  get_query(&sql_query);

  if (sql_query.length() == 0)
  {
    /**
      The instruction has returned zero-length query string. That means, the
      re-preparation of the instruction is not possible. We should not come
      here in the normal case.
    */
    assert(false);
    my_error(ER_UNKNOWN_ERROR, MYF(0));
    return nullptr;
  }

  /*
    Remember a pointer to the next list of Item_trigger_field objects.
    The current list of Item_trigger_field objects is cleared up in the
    method cleanup_before_parsing().
  */
  SQL_I_List<Item_trigger_field> *saved_ptr_to_next_trg_items_list= nullptr;

  if (m_cur_trigger_stmt_items.elements)
    saved_ptr_to_next_trg_items_list=
      m_cur_trigger_stmt_items.first->next_trig_field_list;

  /*
    Clean up items owned by this SP instruction except instances of Item_param.
    `sp_statement_param_values` stores instances of the class Item_param
    associated with the SP instruction's statement before the statement
    has been re-parsed.
  */
  List<Item_param> sp_statement_param_values=
    cleanup_before_parsing(sp->m_handler->type());
  DBUG_ASSERT(mem_root != thd->mem_root);
  /*
    Back up the current free_list pointer and reset it to nullptr.
    Set thd->mem_root pointing to a mem_root of SP instruction being re-parsed.
    In that way any items created on parsing a statement of the current
    instruction is allocated on SP instruction's mem_root and placed on its own
    free_list that later assigned to the current sp_instr. We use the separate
    free list for every instruction since at least at one place in the source
    code (the function subst_spvars() to be accurate) we iterate along the
    list sp_instr->free_list on executing of every SP instruction.
  */
  Query_arena backup;
  /*
    A statement of SP instruction is going to be re-parsed, so reset
    SP arena's state to STMT_INITIALIZED_FOR_SP as its initial state.
  */
  state= STMT_INITIALIZED_FOR_SP;

  /*
    First, set up a men_root for the statement is going to re-compile.
  */
  bool mem_root_allocated;
  if (setup_memroot_for_reparsing(sp, &mem_root_allocated))
    return nullptr;

  /*
    and then set it as the current mem_root. Any memory allocations can take
    place on re-parsing the SP-instruction's statement will be performed on
    this mem_root.
  */
  thd->set_n_backup_active_arena(this, &backup);
  thd->free_list= nullptr;

  Parser_state parser_state;

  if (parser_state.init(thd, sql_query.c_ptr(), sql_query.length()))
    return nullptr;

  /*
    Direct the parser to handle the '?' symbol in special way, that is as
    a positional parameter inside a prepared statement.
  */
  parser_state.m_lip.stmt_prepare_mode= true;

  // Create a new LEX and initialize it.

  LEX *lex_saved= thd->lex;
  Item **cursor_free_list= nullptr;
  st_lex_local *lex_local= nullptr;

  /*
    sp_instr_lex != nullptr for cursor relating SP instructions (sp_instr_cpush,
    sp_instr_cursor_copy_struct) and in some cases for sp_instr_set.
  */
  if (sp_instr_lex == nullptr)
  {
    lex_local= new (thd->mem_root) st_lex_local;
    thd->lex= lex_local;

    lex_local->sp_statement_param_values= std::move(sp_statement_param_values);
    lex_local->param_values_it= lex_local->sp_statement_param_values.begin();
    lex_start(thd);
    if (sp->m_handler->type() == SP_TYPE_TRIGGER)
    {
      /*
        In case the trigger's statement being re-parsed, the correct trigger's
        context (trigger event type and action time) should be copied from
        trigger's sp_head to the new lex object.
      */
      thd->lex->trg_chistics.action_time=
        thd->spcont->m_sp->m_trg->action_time;
      thd->lex->trg_chistics.events= thd->spcont->m_sp->m_trg->events;
    }
  }
  else
  {
    sp_lex_cursor* cursor_lex= sp_instr_lex->get_lex_for_cursor();
    /*
      In case sp_instr_cursor_copy_struct instruction being re-parsed
      the items stored in free_list of sp_lex_cursor are not cleaned up
      since the class sp_instr_cursor_copy_struct don't pass ownership of
      lex object to sp_lex_keeper. So, clean up items stored in free_list of
      sp_lex_cursor explicitly. For sp_instr_cpush instruction items stored
      in free_list of sp_lex_cursor are cleaned up in the method free_lex()
      since sp_instr_cpush owns a lex object stored in its sp_lex_keeper
      data member. So, for the sp_instr_cpush instruction by the time we reach
      this block cursor_lex->free_list is already empty.
    */
    if (mem_root_allocated)
      /*
        If the new memory root for re-parsing has been just created,
        then delete every item from the free item list of sp_lex_cursor.
        In case the memory root for re-parsing is re-used from previous
        re-parsing of failed instruction, don't do anything since all memory
        allocated for items were already released on calling free_root
        inside the method sp_lex_instr::setup_memroot_for_reparsing
      */
      cursor_lex->free_items();

    /* Nullify free_list to don't have a dangling pointer */
    cursor_lex->free_list= nullptr;

    cursor_free_list= &cursor_lex->free_list;
    cursor_lex->mem_root= m_mem_root_for_reparsing;
    DBUG_ASSERT(thd->lex == sp_instr_lex);
    /*
      Adjust mem_root of the cursor's Query_arena to point the just created
      memory root allocated for re-parsing, else we would have the pointer to
      sp_head's memory_root that has already been marked as read_only after
      the first successful execution of the stored routine.
    */
    cursor_lex->query_arena()->mem_root= m_mem_root_for_reparsing;
    lex_start(thd);
  }

  thd->lex->sphead= sp;
  thd->lex->spcont= m_ctx;

  sql_digest_state *parent_digest= thd->m_digest;
  PSI_statement_locker *parent_locker= thd->m_statement_psi;

  thd->m_digest= nullptr;
  thd->m_statement_psi= nullptr;

  /*
    sp_head::m_tmp_query is set by parser on parsing every statement of
    a stored routine. Since here we re-parse failed statement outside stored
    routine context, this data member isn't set. In result, the assert
      DBUG_ASSERT(sphead->m_tmp_query <= start)
    is fired in the constructor of the class Query_fragment.
    To fix the assert failure, reset this data member to point to beginning of
    the current statement being parsed.
  */
  const char *m_tmp_query_bak= sp->m_tmp_query;
  sp->m_tmp_query= sql_query.c_ptr();

  /*
    Hint the parser that re-parsing of a failed SP instruction is in progress
    and instances of the class Item_param associated with SP instruction
    should be handled carefully (re-used on re-parsing the instruction's
    statement).
    @sa param_push_or_clone
    @sa LEX::add_placeholder
  */
  thd->reparsing_sp_stmt= true;
  bool parsing_failed= parse_sql(thd, &parser_state, nullptr);
  thd->reparsing_sp_stmt= false;

  sp->m_tmp_query= m_tmp_query_bak;
  thd->m_digest= parent_digest;
  thd->m_statement_psi= parent_locker;

  if (!parsing_failed)
  {
    thd->lex->set_trg_event_type_for_tables();
    adjust_sql_command(thd->lex);
    parsing_failed= on_after_expr_parsing(thd);

    if (sp->m_handler->type() == SP_TYPE_TRIGGER)
      setup_table_fields_for_trigger(thd, sp,
                                     saved_ptr_to_next_trg_items_list);

    if (cursor_free_list)
      /*
        Update sp_lex_cursor::free_list to point to a list of items
        just created on re-parsing the cursor's statement.
      */
      *cursor_free_list= thd->free_list;
    else
    {
      /*
        Assign the list of items created on re-parsing the statement to
        the current stored routine's instruction.
      */
      free_list= thd->free_list;

      put_back_item_params(thd, thd->lex,
                           lex_local->sp_statement_param_values);
    }

    thd->free_list= nullptr;
  }

  Query_arena old;
  thd->restore_active_arena(&old, &backup);

  LEX *expr_lex= thd->lex;
  thd->lex= lex_saved;

  return parsing_failed ? nullptr : expr_lex;
}


/*
  sp_instr_stmt class functions
*/

PSI_statement_info sp_instr_stmt::psi_info=
{ 0, "stmt", 0};

int
sp_instr_stmt::execute(THD *thd, uint *nextp)
{
  int res;
  bool save_enable_slow_log;
  const CSET_STRING query_backup= thd->query_string;
  Sub_statement_state backup_state;
  DBUG_ENTER("sp_instr_stmt::execute");
  DBUG_PRINT("info", ("command: %d", m_lex_keeper.sql_command()));

  MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi, m_query.str, static_cast<uint>(m_query.length));

#if defined(ENABLED_PROFILING)
  /* This s-p instr is profilable and will be captured. */
  thd->profiling.set_query_source(m_query.str, m_query.length);
#endif

  save_enable_slow_log= thd->enable_slow_log;
  thd->store_slow_query_state(&backup_state);

  if (!(res= alloc_query(thd, m_query.str, m_query.length)) &&
      !(res=subst_spvars(thd, this, &m_query)))
  {
    /*
      (the order of query cache and subst_spvars calls is irrelevant because
      queries with SP vars can't be cached)
    */
    general_log_write(thd, COM_QUERY, thd->query(), thd->query_length());

    if (query_cache_send_result_to_client(thd, thd->query(),
                                          thd->query_length()) <= 0)
    {
      thd->reset_slow_query_state(&backup_state);
      res= m_lex_keeper.validate_lex_and_exec_core(thd, nextp, false, this);
      bool log_slow= !res && thd->enable_slow_log;

      /* Finalize server status flags after executing a statement. */
      if (log_slow || thd->get_stmt_da()->is_eof() ||
          mysql_audit_general_enabled())
        thd->update_server_status();

      if (thd->get_stmt_da()->is_eof())
        thd->protocol->end_statement();

      query_cache_end_of_result(thd);

      mysql_audit_general(thd, MYSQL_AUDIT_GENERAL_STATUS,
                          thd->get_stmt_da()->is_error() ?
                                 thd->get_stmt_da()->sql_errno() : 0,
                          command_name[COM_QUERY].str);

      if (log_slow)
        log_slow_statement(thd);

      /*
        Restore enable_slow_log, that can be changed by a admin or call
        command
      */
      thd->enable_slow_log= save_enable_slow_log;

      /* Add the number of rows to thd for the 'call' statistics */
      thd->add_slow_query_state(&backup_state);
    }
    else
    {
      /* change statistics */
      enum_sql_command save_sql_command= thd->lex->sql_command;
      thd->lex->sql_command= SQLCOM_SELECT;
      status_var_increment(thd->status_var.com_stat[SQLCOM_SELECT]);
      thd->update_stats();
      thd->lex->sql_command= save_sql_command;
      *nextp= m_ip+1;
#ifdef PROTECT_STATEMENT_MEMROOT
      mark_as_qc_used();
#endif
    }
    thd->set_query(query_backup);
    thd->query_name_consts= 0;

    if (likely(!thd->is_error()))
    {
      res= 0;
      thd->get_stmt_da()->reset_diagnostics_area();
    }
  }

  DBUG_RETURN(res || thd->is_error());
}


void
sp_instr_stmt::print(String *str)
{
  size_t i, len;

  /* stmt CMD "..." */
  if (str->reserve(SP_STMT_PRINT_MAXLEN+SP_INSTR_UINT_MAXLEN+8))
    return;
  str->qs_append(STRING_WITH_LEN("stmt "));
  str->qs_append((uint)m_lex_keeper.sql_command());
  str->qs_append(STRING_WITH_LEN(" \""));
  len= m_query.length;
  /*
    Print the query string (but not too much of it), just to indicate which
    statement it is.
  */
  if (len > SP_STMT_PRINT_MAXLEN)
    len= SP_STMT_PRINT_MAXLEN-3;
  /* Copy the query string and replace '\n' with ' ' in the process */
  for (i= 0 ; i < len ; i++)
  {
    char c= m_query.str[i];
    if (c == '\n')
      c= ' ';
    str->qs_append(c);
  }
  if (m_query.length > SP_STMT_PRINT_MAXLEN)
    str->qs_append(STRING_WITH_LEN("...")); /* Indicate truncated string */
  str->qs_append('"');
}


int
sp_instr_stmt::exec_core(THD *thd, uint *nextp)
{
  MYSQL_QUERY_EXEC_START(thd->query(),
                         thd->thread_id,
                         thd->get_db(),
                         &thd->security_ctx->priv_user[0],
                         (char *)thd->security_ctx->host_or_ip,
                         3);
  int res= mysql_execute_command(thd);
  MYSQL_QUERY_EXEC_DONE(res);
  *nextp= m_ip+1;
  return res;
}


/*
  sp_instr_set class functions
*/

PSI_statement_info sp_instr_set::psi_info=
{ 0, "set", 0};

int
sp_instr_set::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set::execute");
  DBUG_PRINT("info", ("offset: %u", m_offset));

  DBUG_RETURN(m_lex_keeper.validate_lex_and_exec_core(thd, nextp, true, this));
}


sp_rcontext *sp_instr_set::get_rcontext(THD *thd) const
{
  return m_rcontext_handler->get_rcontext(thd->spcont);
}


int
sp_instr_set::exec_core(THD *thd, uint *nextp)
{
  int res= get_rcontext(thd)->set_variable(thd, m_offset, &m_value);
  *nextp = m_ip+1;
  return res;
}

void
sp_instr_set::print(String *str)
{
  /* set name@offset ... */
  size_t rsrv = SP_INSTR_UINT_MAXLEN+6;
  sp_variable *var = m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();

  /* 'var' should always be non-null, but just in case... */
  if (var)
    rsrv+= var->name.length + prefix->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("set "));
  str->qs_append(prefix->str, prefix->length);
  if (var)
  {
    str->qs_append(&var->name);
    str->qs_append('@');
  }
  str->qs_append(m_offset);
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


int sp_instr_set_default_param::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set_default_param::execute");
  DBUG_PRINT("info", ("offset: %u", m_offset));

  auto rctx= get_rcontext(thd);
  if (m_offset < rctx->get_inited_param_count())
  {
    // NOP
    *nextp= m_ip + 1;
    DBUG_RETURN(0);  
  }

  DBUG_RETURN(m_lex_keeper.validate_lex_and_exec_core(thd, nextp, true, this));
}


void
sp_instr_set_default_param::print(String *str)
{
  /* set name@offset ... */
  size_t rsrv = SP_INSTR_UINT_MAXLEN+20;
  sp_variable *var = m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();

  /* 'var' should always be non-null, but just in case... */
  if (var)
    rsrv+= var->name.length + prefix->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("set default param "));
  str->qs_append(prefix->str, prefix->length);
  if (var)
  {
    str->qs_append(&var->name);
    str->qs_append('@');
  }
  str->qs_append(m_offset);
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
  sp_instr_set_field class functions
*/

int
sp_instr_set_row_field::exec_core(THD *thd, uint *nextp)
{
  int res= get_rcontext(thd)->set_variable_row_field(thd, m_offset,
                                                     m_field_offset,
                                                     &m_value);
  *nextp= m_ip + 1;
  return res;
}


void
sp_instr_set_row_field::print(String *str)
{
  /* set name@offset[field_offset] ... */
  size_t rsrv= SP_INSTR_UINT_MAXLEN + 6 + 6 + 3;
  sp_variable *var= m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  DBUG_ASSERT(var);
  DBUG_ASSERT(var->field_def.is_row());
  const Column_definition *def=
    var->field_def.row_field_definitions()->elem(m_field_offset);
  DBUG_ASSERT(def);

  rsrv+= var->name.length + def->field_name.length + prefix->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("set "));
  str->qs_append(prefix);
  str->qs_append(&var->name);
  str->qs_append('.');
  str->qs_append(&def->field_name);
  str->qs_append('@');
  str->qs_append(m_offset);
  str->qs_append('[');
  str->qs_append(m_field_offset);
  str->qs_append(']');
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
  sp_instr_set_composite_field_by_name class functions
*/

int
sp_instr_set_composite_field_by_name::exec_core(THD *thd, uint *nextp)
{
  StringBuffer<64> buffer;
  if (m_key)
  {
    auto var= get_rcontext(thd)->get_variable(m_offset);
    auto handler= var->type_handler()->to_composite();
    DBUG_ASSERT(handler);

    m_field_name= handler->key_to_lex_cstring(thd, *this, &m_key, &buffer);
    if (!m_field_name.str)
      return true;
  }

  int res= get_rcontext(thd)->set_variable_composite_by_name(thd, m_offset,
                                                             m_field_name,
                                                             &m_value);
  *nextp= m_ip + 1;
  return res;
}


void
sp_instr_set_composite_field_by_name::print(String *str)
{
  /* set name.field@offset["field"] ... */
  /* set name.field["key"] ... */
  sp_variable *var= m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  DBUG_ASSERT(var);
  DBUG_ASSERT(dynamic_cast<const Type_handler_composite*>(var->type_handler()));

  str->append(STRING_WITH_LEN("set "));
  str->append(prefix);
  str->append(&var->name);

  if (!m_key)
  {
    str->append('.');
    str->append(&m_field_name);
  }

  str->append('@');
  str->append_ulonglong(m_offset);

  if (!m_key)
  {
    str->append(STRING_WITH_LEN("[\""));
    str->append(&m_field_name);
    str->append(STRING_WITH_LEN("\"]"));
  }
  else
  {
    str->append('[');
    m_key->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
    str->append(']');
  }

  str->append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
  sp_instr_set_composite_field_by_key class functions
*/

int
sp_instr_set_composite_field_by_key::exec_core(THD *thd, uint *nextp)
{
  auto var= get_rcontext(thd)->get_variable(m_offset);
  auto handler= var->type_handler()->to_composite();
  DBUG_ASSERT(handler);

  StringBuffer<64> buffer;
  const LEX_CSTRING key= handler->key_to_lex_cstring(thd, *this, &m_key,
                                                     &buffer);
  if (!key.str)
    return true;

  int res= get_rcontext(thd)->set_variable_composite_field_by_key(thd,
                                                                  m_offset,
                                                                  key,
                                                                  m_field_name,
                                                                  &m_value);
  *nextp= m_ip + 1;
  return res;
}


void
sp_instr_set_composite_field_by_key::print(String *str)
{
  sp_variable *var= m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  DBUG_ASSERT(var);
  DBUG_ASSERT(dynamic_cast<const Type_handler_composite*>(var->type_handler()));

  str->append(STRING_WITH_LEN("set "));
  str->append(prefix);
  str->append(&var->name);
  str->append('@');
  str->append_ulonglong(m_offset);
  str->append('[');
  m_key->print(str, enum_query_type(QT_ORDINARY |
                                    QT_ITEM_ORIGINAL_FUNC_NULLIF));
  str->append(']');
  str->append('.');
  str->append(&m_field_name);
  str->append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
  sp_instr_set_trigger_field class functions
*/

PSI_statement_info sp_instr_set_trigger_field::psi_info=
{ 0, "set_trigger_field", 0};

int
sp_instr_set_trigger_field::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set_trigger_field::execute");
  thd->count_cuted_fields= CHECK_FIELD_ERROR_FOR_NULL;
  DBUG_RETURN(m_lex_keeper.validate_lex_and_exec_core(thd, nextp, true, this));
}


int
sp_instr_set_trigger_field::exec_core(THD *thd, uint *nextp)
{
  Abort_on_warning_instant_set aws(thd, thd->is_strict_mode() && !thd->lex->ignore);
  const int res= (trigger_field->set_value(thd, &value) ? -1 : 0);
  *nextp = m_ip+1;
  return res;
}

void
sp_instr_set_trigger_field::print(String *str)
{
  str->append(STRING_WITH_LEN("set_trigger_field "));
  trigger_field->print(str, enum_query_type(QT_ORDINARY |
                                            QT_ITEM_ORIGINAL_FUNC_NULLIF));
  str->append(STRING_WITH_LEN(":="));
  value->print(str, enum_query_type(QT_ORDINARY |
                                    QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
 sp_instr_jump class functions
*/

PSI_statement_info sp_instr_jump::psi_info=
{ 0, "jump", 0};

int
sp_instr_jump::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_jump::execute");
  DBUG_PRINT("info", ("destination: %u", m_dest));

  *nextp= m_dest;
  DBUG_RETURN(0);
}

void
sp_instr_jump::print(String *str)
{
  /* jump dest */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+5))
    return;
  str->qs_append(STRING_WITH_LEN("jump "));
  str->qs_append(m_dest);
}

uint
sp_instr_jump::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  m_dest= opt_shortcut_jump(sp, this);
  if (m_dest != m_ip+1)   /* Jumping to following instruction? */
    marked= 1;
  m_optdest= sp->get_instr(m_dest);
  return m_dest;
}

uint
sp_instr_jump::opt_shortcut_jump(sp_head *sp, sp_instr *start)
{
  uint dest= m_dest;
  sp_instr *i;

  while ((i= sp->get_instr(dest)))
  {
    uint ndest;

    if (start == i || this == i)
      break;
    ndest= i->opt_shortcut_jump(sp, start);
    if (ndest == dest)
      break;
    dest= ndest;
  }
  return dest;
}

void
sp_instr_jump::opt_move(uint dst, List<sp_instr_opt_meta> *bp)
{
  if (m_dest > m_ip)
    bp->push_back(this);      // Forward
  else if (m_optdest)
    m_dest= m_optdest->m_ip;  // Backward
  m_ip= dst;
}

bool sp_instr_set_trigger_field::on_after_expr_parsing(THD *thd)
{
  DBUG_ASSERT(thd->lex->current_select->item_list.elements == 1);

  Item *val= thd->lex->current_select->item_list.head();
  DBUG_ASSERT(val != nullptr);

  trigger_field = new (thd->mem_root)
    Item_trigger_field(thd, thd->lex->current_context(),
                       Item_trigger_field::NEW_ROW,
                       m_trigger_field_name, UPDATE_ACL, false);

  if (!val || !trigger_field)
    return true;

  thd->spcont->m_sp->m_cur_instr_trig_field_items.insert(
    trigger_field, &trigger_field->next_trg_field);

  value= val;

  return false;
}


/*
  sp_instr_destruct_variable class
*/
PSI_statement_info sp_instr_destruct_variable::psi_info=
{0, "destruct", 0};


void sp_instr_destruct_variable::print(String *str)
{
  const LEX_CSTRING instr_name= {STRING_WITH_LEN("destruct")};
  const sp_variable *spv= m_ctx->find_variable(m_offset);
  const LEX_CSTRING data_type= spv->type_handler()->name().lex_cstring();
  /* destruct datatype name@offset */
  size_t rsrv= instr_name.length + 1 +
               data_type.length + 1 +
               spv->name.length + 1 +
               SP_INSTR_UINT_MAXLEN;
  if (str->reserve(rsrv))
    return;
  str->qs_append(&instr_name);
  str->qs_append(' ');
  str->qs_append(&data_type);
  str->qs_append(' ');
  str->qs_append(&spv->name);
  str->qs_append('@');
  str->qs_append(spv->offset);
}


int sp_instr_destruct_variable::execute(THD *thd, uint *nextp)
{
  *nextp= m_ip + 1;
  thd->spcont->get_variable(m_offset)->
    field->expr_event_handler(thd, expr_event_t::DESTRUCT_OUT_OF_SCOPE);
  return 0;
}


/*
  sp_instr_jump_if_not class functions
*/

PSI_statement_info sp_instr_jump_if_not::psi_info=
{ 0, "jump_if_not", 0};

int
sp_instr_jump_if_not::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_jump_if_not::execute");
  DBUG_PRINT("info", ("destination: %u", m_dest));
  DBUG_RETURN(m_lex_keeper.validate_lex_and_exec_core(thd, nextp, true, this));
}


int
sp_instr_jump_if_not::exec_core(THD *thd, uint *nextp)
{
  Item *it;
  int res;

  it= thd->sp_prepare_func_item(&m_expr, 1);
  if (! it || it->check_type_can_return_bool({STRING_WITH_LEN("IF")}))
  {
    res= -1;
  }
  else
  {
    res= 0;
    if (! it->val_bool())
      *nextp = m_dest;
    else
      *nextp = m_ip+1;
  }

  return res;
}


void
sp_instr_jump_if_not::print(String *str)
{
  /* jump_if_not dest(cont) ... */
  if (str->reserve(2*SP_INSTR_UINT_MAXLEN+14+32)) // Add some for the expr. too
    return;
  str->qs_append(STRING_WITH_LEN("jump_if_not "));
  str->qs_append(m_dest);
  str->qs_append('(');
  str->qs_append(m_cont_dest);
  str->qs_append(STRING_WITH_LEN(") "));
  m_expr->print(str, enum_query_type(QT_ORDINARY |
                                     QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


uint
sp_instr_jump_if_not::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  sp_instr *i;

  marked= 1;
  if ((i= sp->get_instr(m_dest)))
  {
    m_dest= i->opt_shortcut_jump(sp, this);
    m_optdest= sp->get_instr(m_dest);
  }
  sp->add_mark_lead(m_dest, leads);
  if ((i= sp->get_instr(m_cont_dest)))
  {
    m_cont_dest= i->opt_shortcut_jump(sp, this);
    m_cont_optdest= sp->get_instr(m_cont_dest);
  }
  sp->add_mark_lead(m_cont_dest, leads);
  return m_ip+1;
}

void
sp_instr_jump_if_not::opt_move(uint dst, List<sp_instr_opt_meta> *bp)
{
  /*
    cont. destinations may point backwards after shortcutting jumps
    during the mark phase. If it's still pointing forwards, only
    push this for backpatching if sp_instr_jump::opt_move() will not
    do it (i.e. if the m_dest points backwards).
   */
  if (m_cont_dest > m_ip)
  {                             // Forward
    if (m_dest < m_ip)
      bp->push_back(this);
  }
  else if (m_cont_optdest)
    m_cont_dest= m_cont_optdest->m_ip; // Backward

  /*
    Take care about m_dest and m_ip
  */
  if (m_dest > m_ip)
    bp->push_back(this);      // Forward
  else if (m_optdest)
    m_dest= m_optdest->m_ip;  // Backward
  m_ip= dst;
}


/*
  sp_instr_freturn class functions
*/

PSI_statement_info sp_instr_freturn::psi_info=
{ 0, "freturn", 0};

int
sp_instr_freturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_freturn::execute");
  DBUG_RETURN(m_lex_keeper.validate_lex_and_exec_core(thd, nextp, true, this));
}


int
sp_instr_freturn::exec_core(THD *thd, uint *nextp)
{
  /*
    RETURN is a "procedure statement" (in terms of the SQL standard).
    That means, Diagnostics Area should be clean before its execution.
  */

  if (!(thd->variables.sql_mode & MODE_ORACLE))
  {
    /*
      Don't clean warnings in ORACLE mode,
      as they are needed for SQLCODE and SQLERRM:
        BEGIN
          SELECT a INTO a FROM t1;
          RETURN 'No exception ' || SQLCODE || ' ' || SQLERRM;
        EXCEPTION WHEN NO_DATA_FOUND THEN
          RETURN 'Exception ' || SQLCODE || ' ' || SQLERRM;
        END;
    */
    Diagnostics_area *da= thd->get_stmt_da();
    da->clear_warning_info(da->warning_info_id());
  }

  /*
    Change <next instruction pointer>, so that this will be the last
    instruction in the stored function.
  */

  *nextp= UINT_MAX;

  /*
    Evaluate the value of return expression and store it in current runtime
    context.

    NOTE: It's necessary to evaluate result item right here, because we must
    do it in scope of execution the current context/block.
  */

  return thd->spcont->set_return_value(thd, &m_value);
}

void
sp_instr_freturn::print(String *str)
{
  /* freturn type expr... */
  if (str->reserve(1024+8+32)) // Add some for the expr. too
    return;
  str->qs_append(STRING_WITH_LEN("freturn "));
  LEX_CSTRING name= m_type_handler->name().lex_cstring();
  str->qs_append(&name);
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}

/*
  sp_instr_preturn class functions
*/

PSI_statement_info sp_instr_preturn::psi_info=
{ 0, "preturn", 0};

int
sp_instr_preturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_preturn::execute");
  *nextp= UINT_MAX;
  DBUG_RETURN(0);
}

void
sp_instr_preturn::print(String *str)
{
  str->append(STRING_WITH_LEN("preturn"));
}

/*
  sp_instr_hpush_jump class functions
*/

PSI_statement_info sp_instr_hpush_jump::psi_info=
{ 0, "hpush_jump", 0};

int
sp_instr_hpush_jump::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hpush_jump::execute");

  int ret= thd->spcont->push_handler(this);

  *nextp= m_dest;

  DBUG_RETURN(ret);
}


void
sp_instr_hpush_jump::print(String *str)
{
  /* hpush_jump dest fsize type */
  if (str->reserve(SP_INSTR_UINT_MAXLEN*2 + 21))
    return;

  str->qs_append(STRING_WITH_LEN("hpush_jump "));
  str->qs_append(m_dest);
  str->qs_append(' ');
  str->qs_append(m_frame);

  switch (m_handler->type) {
  case sp_handler::EXIT:
    str->qs_append(STRING_WITH_LEN(" EXIT"));
    break;
  case sp_handler::CONTINUE:
    str->qs_append(STRING_WITH_LEN(" CONTINUE"));
    break;
  default:
    // The handler type must be either CONTINUE or EXIT.
    DBUG_ASSERT(0);
  }
}


uint
sp_instr_hpush_jump::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  sp_instr *i;

  marked= 1;
  if ((i= sp->get_instr(m_dest)))
  {
    m_dest= i->opt_shortcut_jump(sp, this);
    m_optdest= sp->get_instr(m_dest);
  }
  sp->add_mark_lead(m_dest, leads);

  /*
    For continue handlers, all instructions in the scope of the handler
    are possible leads. For example, the instruction after freturn might
    be executed if the freturn triggers the condition handled by the
    continue handler.

    m_dest marks the start of the handler scope. It's added as a lead
    above, so we start on m_dest+1 here.
    m_opt_hpop is the hpop marking the end of the handler scope.
  */
  if (m_handler->type == sp_handler::CONTINUE)
  {
    for (uint scope_ip= m_dest+1; scope_ip <= m_opt_hpop; scope_ip++)
      sp->add_mark_lead(scope_ip, leads);
  }

  return m_ip+1;
}


/*
  sp_instr_hpop class functions
*/

PSI_statement_info sp_instr_hpop::psi_info=
{ 0, "hpop", 0};

int
sp_instr_hpop::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hpop::execute");
  thd->spcont->pop_handlers(m_count);
  *nextp= m_ip+1;
  DBUG_RETURN(0);
}

void
sp_instr_hpop::print(String *str)
{
  /* hpop count */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+5))
    return;
  str->qs_append(STRING_WITH_LEN("hpop "));
  str->qs_append(m_count);
}


/*
  sp_instr_hreturn class functions
*/

PSI_statement_info sp_instr_hreturn::psi_info=
{ 0, "hreturn", 0};

int
sp_instr_hreturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hreturn::execute");

  uint continue_ip= thd->spcont->exit_handler(thd->get_stmt_da());

  *nextp= m_dest ? m_dest : continue_ip;

  DBUG_RETURN(0);
}


void
sp_instr_hreturn::print(String *str)
{
  /* hreturn framesize dest */
  if (str->reserve(SP_INSTR_UINT_MAXLEN*2 + 9))
    return;
  str->qs_append(STRING_WITH_LEN("hreturn "));
  if (m_dest)
  {
    // NOTE: this is legacy: hreturn instruction for EXIT handler
    // should print out 0 as frame index.
    str->qs_append(STRING_WITH_LEN("0 "));
    str->qs_append(m_dest);
  }
  else
  {
    str->qs_append(m_frame);
  }
}


uint
sp_instr_hreturn::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  marked= 1;

  if (m_dest)
  {
    /*
      This is an EXIT handler; next instruction step is in m_dest.
     */
    return m_dest;
  }

  /*
    This is a CONTINUE handler; next instruction step will come from
    the handler stack and not from opt_mark.
   */
  return UINT_MAX;
}


/*
  sp_instr_cpush class functions
*/

PSI_statement_info sp_instr_cpush::psi_info=
{ 0, "cpush", 0};

int
sp_instr_cpush::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cpush::execute");

  sp_cursor::reset(thd);
  m_lex_keeper.disable_query_cache();
  thd->spcont->push_cursor(this);

  *nextp= m_ip+1;

  DBUG_RETURN(false);
}


int
sp_instr_cpush::exec_core(THD *thd, uint *nextp)
{
  sp_cursor *c = thd->spcont->get_cursor(m_cursor);
  return c ? c->open(thd) : true;
}

void
sp_instr_cpush::print(String *str)
{
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* cpush name@offset */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+7;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("cpush "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
}


/*
  sp_instr_cpop class functions
*/

PSI_statement_info sp_instr_cpop::psi_info=
{ 0, "cpop", 0};

int
sp_instr_cpop::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cpop::execute");
  thd->spcont->pop_cursors(thd, m_count);
  *nextp= m_ip+1;
  DBUG_RETURN(0);
}


void
sp_instr_cpop::print(String *str)
{
  /* cpop count */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+5))
    return;
  str->qs_append(STRING_WITH_LEN("cpop "));
  str->qs_append(m_count);
}


/*
  sp_instr_copen class functions
*/

/**
  @todo
    Assert that we either have an error or a cursor
*/

PSI_statement_info sp_instr_copen::psi_info=
{ 0, "copen", 0};

int
sp_instr_copen::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_copen::execute");
  /*
    We don't store a pointer to the cursor in the instruction to be
    able to reuse the same instruction among different threads in future.
  */
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;

  if (! c)
    res= -1;
  else
  {
    sp_lex_keeper *lex_keeper= c->get_lex_keeper();
    /*
      The expression
        sp_cursor *c= thd->spcont->get_cursor(m_cursor);
      that has run above returns an instance of the class sp_instr_cpush
      that was added former on handling the statement DECLARE CURSOR.
      The class sp_instr_cpush implements the pure virtual method
        sp_cursor::get_lex_keeper()
      so the following DBUG_ASSERT must be ok. This DBUG_ASSERT is added
      in order to catch possible future changes in execution flow that could
      break implicit relationship between sp_instr_copen and sp_instr_cpush.
    */
    DBUG_ASSERT(lex_keeper);
    /*
      Get a pointer to a SP instruction sp_instr_cpush that was instantiated
      on handling the statement DECLARE CURSOR. The pointer to sp_instr_cpush
      is passed to the method cursor_reset_lex_and_exec_core() finishing
      a process of cursor opening by calling the method
        sp_instr_cpush::exec_core
      that does a real work for cursor opening.
    */
    sp_instr_cpush *cpush_instr= c->get_push_instr();
    /*
      For the same goal as previous DBUG_ASSERT, this DBUG_ASSERT ensure that
      sp_inst_cpush has been already added to SP, that is the statement
      DECLARE CURSOR occurred before the statement OPEN cursor_name.
    */
    DBUG_ASSERT(cpush_instr);
    res= lex_keeper->cursor_reset_lex_and_exec_core(thd, nextp, false,
                                                    cpush_instr);

    *nextp= m_ip + 1;
  }
  DBUG_RETURN(res);
}


void
sp_instr_copen::print(String *str)
{
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* copen name@offset */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+7;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("copen "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
}


/*
  sp_instr_cclose class functions
*/

PSI_statement_info sp_instr_cclose::psi_info=
{ 0, "cclose", 0};

int
sp_instr_cclose::execute(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  DBUG_ENTER("sp_instr_cclose::execute");

  if (! c)
    res= -1;
  else
    res= c->close(thd);
  *nextp= m_ip+1;
  DBUG_RETURN(res);
}


void
sp_instr_cclose::print(String *str)
{
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* cclose name@offset */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+8;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("cclose "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
}


/*
  sp_instr_cfetch class functions
*/

PSI_statement_info sp_instr_cfetch::psi_info=
{ 0, "cfetch", 0};

int
sp_instr_cfetch::execute(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  DBUG_ENTER("sp_instr_cfetch::execute");

  res= c ? c->fetch(thd, &m_fetch_target_list, m_error_on_no_data) : -1;

  *nextp= m_ip+1;
  DBUG_RETURN(res);
}


void
sp_instr_cfetch::print(String *str)
{
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* cfetch name@offset vars... */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+8;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("cfetch "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
  print_fetch_into(str, m_fetch_target_list);
}

/*
  sp_instr_agg_cfetch class functions
*/

PSI_statement_info sp_instr_agg_cfetch::psi_info=
{ 0, "agg_cfetch", 0};

int
sp_instr_agg_cfetch::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_agg_cfetch::execute");
  int res= 0;
  if (!thd->spcont->instr_ptr)
  {
    *nextp= m_ip+1;
    thd->spcont->instr_ptr= m_ip + 1;
  }
  else if (!thd->spcont->pause_state)
    thd->spcont->pause_state= true;
  else
  {
    thd->spcont->pause_state= false;
    if (thd->server_status & SERVER_STATUS_LAST_ROW_SENT)
    {
      my_message(ER_SP_FETCH_NO_DATA,
                 ER_THD(thd, ER_SP_FETCH_NO_DATA), MYF(0));
      res= -1;
      thd->spcont->quit_func= true;
    }
    else
      *nextp= m_ip + 1;
  }
  DBUG_RETURN(res);
}

void
sp_instr_agg_cfetch::print(String *str)
{

  uint rsrv= SP_INSTR_UINT_MAXLEN+11;

  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("agg_cfetch"));
}

/*
  sp_instr_cursor_copy_struct class functions
*/

/**
  This methods processes cursor %ROWTYPE declarations, e.g.:
    CURSOR cur IS SELECT * FROM t1;
    rec cur%ROWTYPE;
  and does the following:
  - opens the cursor without copying data (materialization).
  - copies the cursor structure to the associated %ROWTYPE variable.
*/

PSI_statement_info sp_instr_cursor_copy_struct::psi_info=
{ 0, "cursor_copy_struct", 0};

int
sp_instr_cursor_copy_struct::exec_core(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cursor_copy_struct::exec_core");
  int ret= 0;
  Item_field_row *row= (Item_field_row*) thd->spcont->get_variable(m_var);
  DBUG_ASSERT(row->type_handler() == &type_handler_row);
  DBUG_ASSERT(row->field);
  DBUG_ASSERT(dynamic_cast<Field_row*>(row->field));

  /*
    Copy structure only once. If the cursor%ROWTYPE variable is declared
    inside a LOOP block, it gets its structure on the first loop iteration
    and remembers the structure for all consequent loop iterations.
    It we recreated the structure on every iteration, we would get
    potential memory leaks, and it would be less efficient.
  */
  if (!row->arguments())
  {
    sp_cursor tmp(thd, true);
    // Open the cursor without copying data
    if (!(ret= tmp.open(thd)))
    {
      Row_definition_list defs;
      /*
        Create row elements on the caller arena.
        It's the same arena that was used during sp_rcontext::create().
        This puts cursor%ROWTYPE elements on the same mem_root
        where explicit ROW elements and table%ROWTYPE reside:
        - tmp.export_structure() allocates new Spvar_definition instances
          and their components (such as TYPELIBs).
        - field->row_create_fields() creates a new Virtual_tmp_table instance
          with Field instances, one Field instance per a ROW member.
        - row->add_array_of_item_field() creates Item_field instances
          corresponding to Field instances.
        They all are created on the same mem_root.
      */
      Query_arena current_arena;
      thd->set_n_backup_active_arena(thd->spcont->callers_arena, &current_arena);
      ret= tmp.export_structure(thd, &defs) ||
           static_cast<Field_row*>(row->field)->row_create_fields(thd, &defs) ||
           row->add_array_of_item_field(thd, *row->field->virtual_tmp_table());
      thd->restore_active_arena(thd->spcont->callers_arena, &current_arena);
      tmp.close(thd);
    }
  }
  *nextp= m_ip + 1;
  DBUG_RETURN(ret);
}


int
sp_instr_cursor_copy_struct::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cursor_copy_struct::execute");
  int ret= m_lex_keeper.cursor_reset_lex_and_exec_core(thd, nextp, false, this);
  DBUG_RETURN(ret);
}


void
sp_instr_cursor_copy_struct::print(String *str)
{
  sp_variable *var= m_ctx->find_variable(m_var);
  const LEX_CSTRING *name= m_ctx->find_cursor(m_cursor);
  str->append(STRING_WITH_LEN("cursor_copy_struct "));
  str->append(name);
  str->append(' ');
  str->append(&var->name);
  str->append('@');
  str->append_ulonglong(m_var);
}


/*
  sp_instr_copen_by_ref class functions.
  Handles the "OPEN sys_ref_cyrsor FOR stmt" statement.
*/

PSI_statement_info sp_instr_copen_by_ref::psi_info=
{ 0, "copen_by_ref", 0};


int
sp_instr_copen_by_ref::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_copen_by_ref::execute");
  m_lex_keeper.disable_query_cache();
  int res= m_lex_keeper.cursor_reset_lex_and_exec_core(thd, nextp, false, this);
  *nextp= m_ip + 1;
  DBUG_RETURN(res);
}


int sp_instr_copen_by_ref::exec_core(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_copen_by_ref::exec_core");
  sp_cursor *cursor;
  if (thd->open_cursors_counter() < thd->variables.max_open_cursors)
  {
    // The limit allows to open new cursors
    if (!(cursor= m_deref_rcontext_handler->get_cursor_by_ref(thd,
                                                              *this, true)))
      DBUG_RETURN(-1); // EOM
    /*
      The sp_rcontext_addr part of "this" points to an initialized sp_cursor.
      It can be a newly added cursor, or an old one (closed or open).
      Two consequent OPEN (without a CLOSE in between) are allowed
      for SYS_REFCURSORs (unlike for static CURSORs).
      Close the first cursor automatically if it's open, e.g.:
        OPEN c FOR SELECT 1;
        OPEN c FOR SELECT 2; -- this closes "c" and opens it for the new query
    */
    cursor->reset_for_reopen(thd);
    DBUG_ASSERT(thd->lex == m_lex_keeper.lex());
    // TODO: check with DmitryS if hiding ROOT_FLAG_READ_ONLY is OK:
    auto flags_backup= thd->lex->query_arena()->mem_root->flags;
    thd->lex->query_arena()->mem_root->flags&= ~ROOT_FLAG_READ_ONLY;
    int rc= cursor->open(thd);
    thd->lex->query_arena()->mem_root->flags= flags_backup;
    DBUG_RETURN(rc);
  }

  /*
    The limit does not allow to create new open cursors.
    Only an existing cursor pointed by the sp_rcontext_addr part of
    "this" can be reused, and it must be open.
  */
  if (!(cursor= m_deref_rcontext_handler->get_cursor_by_ref(thd,
                                                            *this, false)) ||
      !cursor->is_open())
  {
    /*
      - The SYS_REFCURSOR variable pointed by the sp_rcontext_addr
        part of "this" is not linked to any session cursors.
      - Or it is linked, but the referenced session cursor is not open.
    */
    my_error(ER_TOO_MANY_OPEN_CURSORS, MYF(0),
             thd->variables.max_open_cursors);
    DBUG_RETURN(-1);
  }
  cursor->reset_for_reopen(thd);
  DBUG_RETURN(cursor->open(thd, false/*don't check max_open_cursors*/));
}


void
sp_instr_copen_by_ref::print(String *str)
{
  static constexpr LEX_CSTRING instr{STRING_WITH_LEN("copen")};
  print_cmd_and_array_element(str, instr,
                              m_deref_rcontext_handler->get_name_prefix()[0],
                              cursor_str, m_offset);
}


/*
  sp_instr_cclose_by_ref class functions
*/

PSI_statement_info sp_instr_cclose_by_ref::psi_info
{ 0, "cclose_by_ref", 0};

int
sp_instr_cclose_by_ref::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cclose_by_ref::execute");
  sp_cursor *cursor= Sp_rcontext_handler::get_open_cursor_or_error(thd, *this);
  if (!cursor)
    DBUG_RETURN(-1);
  int res= cursor->close(thd);
  *nextp= m_ip + 1;
  DBUG_RETURN(res);
}


void
sp_instr_cclose_by_ref::print(String *str)
{
  static constexpr LEX_CSTRING instr{STRING_WITH_LEN("cclose")};
  print_cmd_and_array_element(str, instr,
                              m_deref_rcontext_handler->get_name_prefix()[0],
                              cursor_str, m_offset);
}


/*
  sp_instr_cfetch_by_ref class functions
*/

PSI_statement_info sp_instr_cfetch_by_ref::psi_info=
{ 0, "cfetch_by_ref", 0};

int
sp_instr_cfetch_by_ref::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cfetch_by_ref::execute");
  sp_cursor *cursor= Sp_rcontext_handler::get_open_cursor_or_error(thd, *this);
  if (!cursor)
    DBUG_RETURN(-1);
  int res= cursor->fetch(thd, &m_fetch_target_list, m_error_on_no_data);
  *nextp= m_ip + 1;
  DBUG_RETURN(res);
}


void
sp_instr_cfetch_by_ref::print(String *str)
{
  static constexpr LEX_CSTRING instr= LEX_CSTRING{STRING_WITH_LEN("cfetch")};
  print_cmd_and_array_element(str, instr,
                              m_deref_rcontext_handler->get_name_prefix()[0],
                              cursor_str, m_offset);
  print_fetch_into(str, m_fetch_target_list);
}


/*
  sp_instr_error class functions
*/

PSI_statement_info sp_instr_error::psi_info=
{ 0, "error", 0};

int
sp_instr_error::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_error::execute");
  my_message(m_errcode, ER_THD(thd, m_errcode), MYF(0));
  WSREP_DEBUG("sp_instr_error: %s %d", ER_THD(thd, m_errcode), thd->is_error());
  *nextp= m_ip+1;
  DBUG_RETURN(-1);
}


void
sp_instr_error::print(String *str)
{
  /* error code */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+6))
    return;
  str->qs_append(STRING_WITH_LEN("error "));
  str->qs_append(m_errcode);
}


/**************************************************************************
  sp_instr_set_case_expr class implementation
**************************************************************************/

PSI_statement_info sp_instr_set_case_expr::psi_info=
{ 0, "set_case_expr", 0};

int
sp_instr_set_case_expr::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set_case_expr::execute");

  DBUG_RETURN(m_lex_keeper.validate_lex_and_exec_core(thd, nextp, true, this));
}


int
sp_instr_set_case_expr::exec_core(THD *thd, uint *nextp)
{
  int res= thd->spcont->set_case_expr(thd, m_case_expr_id, &m_case_expr);

  if (res && !thd->spcont->get_case_expr(m_case_expr_id))
  {
    /*
      Failed to evaluate the value, the case expression is still not
      initialized. Set to NULL so we can continue.
    */

    Item *null_item= new (thd->mem_root) Item_null(thd);

    if (!null_item ||
        thd->spcont->set_case_expr(thd, m_case_expr_id, &null_item))
    {
      /* If this also failed, we have to abort. */
      my_error(ER_OUT_OF_RESOURCES, MYF(ME_FATAL));
    }
  }
  else
    *nextp= m_ip+1;

  return res;
}


void
sp_instr_set_case_expr::print(String *str)
{
  /* set_case_expr (cont) id ... */
  str->reserve(2*SP_INSTR_UINT_MAXLEN+18+32); // Add some extra for expr too
  str->qs_append(STRING_WITH_LEN("set_case_expr ("));
  str->qs_append(m_cont_dest);
  str->qs_append(STRING_WITH_LEN(") "));
  str->qs_append(m_case_expr_id);
  str->qs_append(' ');
  m_case_expr->print(str, enum_query_type(QT_ORDINARY |
                                          QT_ITEM_ORIGINAL_FUNC_NULLIF));
}

uint
sp_instr_set_case_expr::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  sp_instr *i;

  marked= 1;
  if ((i= sp->get_instr(m_cont_dest)))
  {
    m_cont_dest= i->opt_shortcut_jump(sp, this);
    m_cont_optdest= sp->get_instr(m_cont_dest);
  }
  sp->add_mark_lead(m_cont_dest, leads);
  return m_ip+1;
}

void
sp_instr_set_case_expr::opt_move(uint dst, List<sp_instr_opt_meta> *bp)
{
  if (m_cont_dest > m_ip)
    bp->push_back(this);        // Forward
  else if (m_cont_optdest)
    m_cont_dest= m_cont_optdest->m_ip; // Backward
  m_ip= dst;
}
