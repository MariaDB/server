/* Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation
#endif

#include "mysql.h"
#include "sp_head.h"
#include "sql_cursor.h"
#include "sp_rcontext.h"
#include "sp_pcontext.h"
#include "sql_select.h"                     // create_virtual_tmp_table

///////////////////////////////////////////////////////////////////////////
// sp_rcontext implementation.
///////////////////////////////////////////////////////////////////////////


sp_rcontext::sp_rcontext(const sp_pcontext *root_parsing_ctx,
                         Field *return_value_fld,
                         bool in_sub_stmt)
  :end_partial_result_set(false),
   m_root_parsing_ctx(root_parsing_ctx),
   m_var_table(NULL),
   m_return_value_fld(return_value_fld),
   m_return_value_set(false),
   m_in_sub_stmt(in_sub_stmt),
   m_ccount(0)
{
}


sp_rcontext::~sp_rcontext()
{
  if (m_var_table)
    free_blobs(m_var_table);

  // Leave m_handlers, m_handler_call_stack, m_var_items, m_cstack
  // and m_case_expr_holders untouched.
  // They are allocated in mem roots and will be freed accordingly.
}


sp_rcontext *sp_rcontext::create(THD *thd,
                                 const sp_pcontext *root_parsing_ctx,
                                 Field *return_value_fld)
{
  sp_rcontext *ctx= new (thd->mem_root) sp_rcontext(root_parsing_ctx,
                                                    return_value_fld,
                                                    thd->in_sub_stmt);

  if (!ctx)
    return NULL;

  if (ctx->alloc_arrays(thd) ||
      ctx->init_var_table(thd) ||
      ctx->init_var_items(thd))
  {
    delete ctx;
    return NULL;
  }

  return ctx;
}


bool sp_rcontext::alloc_arrays(THD *thd)
{
  {
    size_t n= m_root_parsing_ctx->max_cursor_index();
    m_cstack.reset(
      static_cast<sp_cursor **> (
        thd->alloc(n * sizeof (sp_cursor*))),
      n);
  }

  {
    size_t n= m_root_parsing_ctx->get_num_case_exprs();
    m_case_expr_holders.reset(
      static_cast<Item_cache **> (
        thd->calloc(n * sizeof (Item_cache*))),
      n);
  }

  return !m_cstack.array() || !m_case_expr_holders.array();
}


bool sp_rcontext::init_var_table(THD *thd)
{
  List<Column_definition> field_def_lst;

  if (!m_root_parsing_ctx->max_var_index())
    return false;

  m_root_parsing_ctx->retrieve_field_definitions(&field_def_lst);

  DBUG_ASSERT(field_def_lst.elements == m_root_parsing_ctx->max_var_index());

  if (!(m_var_table= create_virtual_tmp_table(thd, field_def_lst)))
    return true;

  m_var_table->copy_blobs= true;
  m_var_table->alias.set("", 0, m_var_table->alias.charset());

  return false;
}


bool sp_rcontext::init_var_items(THD *thd)
{
  uint num_vars= m_root_parsing_ctx->max_var_index();

  m_var_items.reset(
    static_cast<Item **> (
      thd->alloc(num_vars * sizeof (Item *))),
    num_vars);

  if (!m_var_items.array())
    return true;

  for (uint idx = 0; idx < num_vars; ++idx)
  {
    if (!(m_var_items[idx]= new (thd->mem_root) Item_field(thd, m_var_table->field[idx])))
      return true;
  }

  return false;
}


bool sp_rcontext::set_return_value(THD *thd, Item **return_value_item)
{
  DBUG_ASSERT(m_return_value_fld);

  m_return_value_set = true;

  return sp_eval_expr(thd, m_return_value_fld, return_value_item);
}


bool sp_rcontext::push_cursor(THD *thd, sp_lex_keeper *lex_keeper,
                              sp_instr_cpush *i)
{
  /*
    We should create cursors in the callers arena, as
    it could be (and usually is) used in several instructions.
  */
  sp_cursor *c= new (callers_arena->mem_root) sp_cursor(thd, lex_keeper, i);

  if (c == NULL)
    return true;

  m_cstack[m_ccount++]= c;
  return false;
}


void sp_rcontext::pop_cursors(uint count)
{
  DBUG_ASSERT(m_ccount >= count);

  while (count--)
    delete m_cstack[--m_ccount];
}


bool sp_rcontext::push_handler(sp_handler *handler, uint first_ip)
{
  /*
    We should create handler entries in the callers arena, as
    they could be (and usually are) used in several instructions.
  */
  sp_handler_entry *he=
    new (callers_arena->mem_root) sp_handler_entry(handler, first_ip);

  if (he == NULL)
    return true;

  return m_handlers.append(he);
}


void sp_rcontext::pop_handlers(size_t count)
{
  DBUG_ASSERT(m_handlers.elements() >= count);

  for (size_t i= 0; i < count; ++i)
    m_handlers.pop();
}


bool sp_rcontext::handle_sql_condition(THD *thd,
                                       uint *ip,
                                       const sp_instr *cur_spi)
{
  DBUG_ENTER("sp_rcontext::handle_sql_condition");

  /*
    If this is a fatal sub-statement error, and this runtime
    context corresponds to a sub-statement, no CONTINUE/EXIT
    handlers from this context are applicable: try to locate one
    in the outer scope.
  */
  if (thd->is_fatal_sub_stmt_error && m_in_sub_stmt)
    DBUG_RETURN(false);

  Diagnostics_area *da= thd->get_stmt_da();
  const sp_handler *found_handler= NULL;
  const Sql_condition *found_condition= NULL;

  if (thd->is_error())
  {
    found_handler=
      cur_spi->m_ctx->find_handler(da->get_error_condition_identity());

    if (found_handler)
      found_condition= da->get_error_condition();

    /*
      Found condition can be NULL if the diagnostics area was full
      when the error was raised. It can also be NULL if
      Diagnostics_area::set_error_status(uint sql_error) was used.
      In these cases, make a temporary Sql_condition here so the
      error can be handled.
    */
    if (!found_condition)
    {
      found_condition=
        new (callers_arena->mem_root) Sql_condition(callers_arena->mem_root,
                                                    da->get_error_condition_identity(),
                                                    da->message());
    }
  }
  else if (da->current_statement_warn_count())
  {
    Diagnostics_area::Sql_condition_iterator it= da->sql_conditions();
    const Sql_condition *c;

    // Here we need to find the last warning/note from the stack.
    // In MySQL most substantial warning is the last one.
    // (We could have used a reverse iterator here if one existed)

    while ((c= it++))
    {
      if (c->get_level() == Sql_condition::WARN_LEVEL_WARN ||
          c->get_level() == Sql_condition::WARN_LEVEL_NOTE)
      {
        const sp_handler *handler= cur_spi->m_ctx->find_handler(*c);
        if (handler)
        {
          found_handler= handler;
          found_condition= c;
        }
      }
    }
  }

  if (!found_handler)
    DBUG_RETURN(false);

  // At this point, we know that:
  //  - there is a pending SQL-condition (error or warning);
  //  - there is an SQL-handler for it.

  DBUG_ASSERT(found_condition);

  sp_handler_entry *handler_entry= NULL;
  for (size_t i= 0; i < m_handlers.elements(); ++i)
  {
    sp_handler_entry *h= m_handlers.at(i);

    if (h->handler == found_handler)
    {
      handler_entry= h;
      break;
    }
  }

  /*
    handler_entry usually should not be NULL here, as that indicates
    that the parser context thinks a HANDLER should be activated,
    but the runtime context cannot find it.

    However, this can happen (and this is in line with the Standard)
    if SQL-condition has been raised before DECLARE HANDLER instruction
    is processed.

    For example:
    CREATE PROCEDURE p()
    BEGIN
      DECLARE v INT DEFAULT 'get'; -- raises SQL-warning here
      DECLARE EXIT HANDLER ...     -- this handler does not catch the warning
    END
  */
  if (!handler_entry)
    DBUG_RETURN(false);

  // Mark active conditions so that they can be deleted when the handler exits.
  da->mark_sql_conditions_for_removal();

  uint continue_ip= handler_entry->handler->type == sp_handler::CONTINUE ?
    cur_spi->get_cont_dest() : 0;

  /* End aborted result set. */
  if (end_partial_result_set)
    thd->protocol->end_partial_result_set(thd);

  /* Reset error state. */
  thd->clear_error();
  thd->killed= NOT_KILLED; // Some errors set thd->killed
                           // (e.g. "bad data").

  /* Add a frame to handler-call-stack. */
  Sql_condition_info *cond_info=
    new (callers_arena->mem_root) Sql_condition_info(found_condition,
                                                     callers_arena);
  Handler_call_frame *frame=
    new (callers_arena->mem_root) Handler_call_frame(cond_info, continue_ip);
  m_handler_call_stack.append(frame);

  *ip= handler_entry->first_ip;

  DBUG_RETURN(true);
}


uint sp_rcontext::exit_handler(Diagnostics_area *da)
{
  DBUG_ENTER("sp_rcontext::exit_handler");
  DBUG_ASSERT(m_handler_call_stack.elements() > 0);

  Handler_call_frame *f= m_handler_call_stack.pop();

  /*
    Remove the SQL conditions that were present in DA when the
    handler was activated.
  */
  da->remove_marked_sql_conditions();

  uint continue_ip= f->continue_ip;

  DBUG_RETURN(continue_ip);
}


int sp_rcontext::set_variable(THD *thd, Field *field, Item **value)
{
  if (!value)
  {
    field->set_null();
    return 0;
  }

  return sp_eval_expr(thd, field, value);
}


Item_cache *sp_rcontext::create_case_expr_holder(THD *thd,
                                                 const Item *item) const
{
  Item_cache *holder;
  Query_arena current_arena;

  thd->set_n_backup_active_arena(thd->spcont->callers_arena, &current_arena);

  holder= item->get_cache(thd);

  thd->restore_active_arena(thd->spcont->callers_arena, &current_arena);

  return holder;
}


bool sp_rcontext::set_case_expr(THD *thd, int case_expr_id,
                                Item **case_expr_item_ptr)
{
  Item *case_expr_item= sp_prepare_func_item(thd, case_expr_item_ptr);
  if (!case_expr_item)
    return true;

  if (!m_case_expr_holders[case_expr_id] ||
      m_case_expr_holders[case_expr_id]->result_type() !=
        case_expr_item->result_type())
  {
    m_case_expr_holders[case_expr_id]=
      create_case_expr_holder(thd, case_expr_item);
  }

  m_case_expr_holders[case_expr_id]->store(case_expr_item);
  m_case_expr_holders[case_expr_id]->cache_value();
  return false;
}


///////////////////////////////////////////////////////////////////////////
// sp_cursor implementation.
///////////////////////////////////////////////////////////////////////////


sp_cursor::sp_cursor(THD *thd_arg, sp_lex_keeper *lex_keeper, sp_instr_cpush *i):
   result(thd_arg),
   m_lex_keeper(lex_keeper),
   server_side_cursor(NULL),
   m_i(i),
   m_fetch_count(0),
   m_row_count(0),
   m_found(false)
{
  /*
    currsor can't be stored in QC, so we should prevent opening QC for
    try to write results which are absent.
  */
  lex_keeper->disable_query_cache();
}


/*
  Open an SP cursor

  SYNOPSIS
    open()
    THD		         Thread handler


  RETURN
   0 in case of success, -1 otherwise
*/

int sp_cursor::open(THD *thd)
{
  if (server_side_cursor)
  {
    my_message(ER_SP_CURSOR_ALREADY_OPEN,
               ER_THD(thd, ER_SP_CURSOR_ALREADY_OPEN),
               MYF(0));
    return -1;
  }
  if (mysql_open_cursor(thd, &result, &server_side_cursor))
    return -1;
  return 0;
}


int sp_cursor::close(THD *thd)
{
  if (! server_side_cursor)
  {
    my_message(ER_SP_CURSOR_NOT_OPEN, ER_THD(thd, ER_SP_CURSOR_NOT_OPEN),
               MYF(0));
    return -1;
  }
  m_row_count= m_fetch_count= 0;
  m_found= false;
  destroy();
  return 0;
}


void sp_cursor::destroy()
{
  delete server_side_cursor;
  server_side_cursor= NULL;
}


int sp_cursor::fetch(THD *thd, List<sp_variable> *vars)
{
  if (! server_side_cursor)
  {
    my_message(ER_SP_CURSOR_NOT_OPEN, ER_THD(thd, ER_SP_CURSOR_NOT_OPEN),
               MYF(0));
    return -1;
  }
  if (vars->elements != result.get_field_count())
  {
    my_message(ER_SP_WRONG_NO_OF_FETCH_ARGS,
               ER_THD(thd, ER_SP_WRONG_NO_OF_FETCH_ARGS), MYF(0));
    return -1;
  }

  m_fetch_count++;
  DBUG_EXECUTE_IF("bug23032_emit_warning",
                  push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                               ER_UNKNOWN_ERROR,
                               ER_THD(thd, ER_UNKNOWN_ERROR)););

  result.set_spvar_list(vars);

  /* Attempt to fetch one row */
  if (server_side_cursor->is_open())
    server_side_cursor->fetch(1);

  /*
    If the cursor was pointing after the last row, the fetch will
    close it instead of sending any rows.
  */
  if (! server_side_cursor->is_open())
  {
    m_found= false;
    if (thd->variables.sql_mode & MODE_ORACLE)
      return 0;
    my_message(ER_SP_FETCH_NO_DATA, ER_THD(thd, ER_SP_FETCH_NO_DATA), MYF(0));
    return -1;
  }

  m_found= true;
  m_row_count++;
  return 0;
}


///////////////////////////////////////////////////////////////////////////
// sp_cursor::Select_fetch_into_spvars implementation.
///////////////////////////////////////////////////////////////////////////


int sp_cursor::Select_fetch_into_spvars::prepare(List<Item> &fields,
                                                 SELECT_LEX_UNIT *u)
{
  /*
    Cache the number of columns in the result set in order to easily
    return an error if column count does not match value count.
  */
  field_count= fields.elements;
  return select_result_interceptor::prepare(fields, u);
}


int sp_cursor::Select_fetch_into_spvars::send_data(List<Item> &items)
{
  List_iterator_fast<sp_variable> spvar_iter(*spvar_list);
  List_iterator_fast<Item> item_iter(items);
  sp_variable *spvar;
  Item *item;

  /* Must be ensured by the caller */
  DBUG_ASSERT(spvar_list->elements == items.elements);

  /*
    Assign the row fetched from a server side cursor to stored
    procedure variables.
  */
  for (; spvar= spvar_iter++, item= item_iter++; )
  {
    if (thd->spcont->set_variable(thd, spvar->offset, &item))
      return true;
  }
  return false;
}
