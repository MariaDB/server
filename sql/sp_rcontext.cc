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
#include "sql_base.h"                       // open_tables_only_view_structure
#include "sql_acl.h"                        // SELECT_ACL
#include "sql_parse.h"                      // check_table_access

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
                                 Field *return_value_fld,
                                 bool resolve_type_refs)
{
  sp_rcontext *ctx= new (thd->mem_root) sp_rcontext(root_parsing_ctx,
                                                    return_value_fld,
                                                    thd->in_sub_stmt);

  if (!ctx)
    return NULL;

  List<Spvar_definition> field_def_lst;
  ctx->m_root_parsing_ctx->retrieve_field_definitions(&field_def_lst);

  if (ctx->alloc_arrays(thd) ||
      (resolve_type_refs && ctx->resolve_type_refs(thd, field_def_lst)) ||
      ctx->init_var_table(thd, field_def_lst) ||
      ctx->init_var_items(thd, field_def_lst))
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


bool sp_rcontext::init_var_table(THD *thd,
                                 List<Spvar_definition> &field_def_lst)
{
  if (!m_root_parsing_ctx->max_var_index())
    return false;

  DBUG_ASSERT(field_def_lst.elements == m_root_parsing_ctx->max_var_index());

  if (!(m_var_table= create_virtual_tmp_table(thd, field_def_lst)))
    return true;

  return false;
}


/**
  Check if we have access to use a column as a %TYPE reference.
  @return false - OK
  @return true  - access denied
*/
static inline bool
check_column_grant_for_type_ref(THD *thd, TABLE_LIST *table_list,
                                const char *str, size_t length)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  table_list->table->grant.want_privilege= SELECT_ACL;
  return check_column_grant_in_table_ref(thd, table_list, str, length);
#else
  return false;
#endif
}


/**
  This method implementation is very close to fill_schema_table_by_open().
*/
bool sp_rcontext::resolve_type_ref(THD *thd, Column_definition *def,
                                   Qualified_column_ident *ref)
{
  Open_tables_backup open_tables_state_backup;
  thd->reset_n_backup_open_tables_state(&open_tables_state_backup);

  TABLE_LIST *table_list;
  Field *src;
  LEX *save_lex= thd->lex;
  bool rc= true;

  sp_lex_local lex(thd, thd->lex);
  thd->lex= &lex;

  lex.context_analysis_only= CONTEXT_ANALYSIS_ONLY_VIEW;
  // Make %TYPE variables see temporary tables that shadow permanent tables
  thd->temporary_tables= open_tables_state_backup.temporary_tables;

  if ((table_list= lex.select_lex.add_table_to_list(thd, ref, NULL, 0,
                                                    TL_READ_NO_INSERT,
                                                    MDL_SHARED_READ)) &&
      !check_table_access(thd, SELECT_ACL, table_list, TRUE, UINT_MAX, FALSE) &&
      !open_tables_only_view_structure(thd, table_list,
                                       thd->mdl_context.has_locks()))
  {
    if ((src= lex.query_tables->table->find_field_by_name(&ref->m_column)))
    {
      if (!(rc= check_column_grant_for_type_ref(thd, table_list,
                                                ref->m_column.str,
                                                ref->m_column.length)))
      {
        *def= Column_definition(thd, src, NULL/*No defaults,no constraints*/);
        def->flags&= (uint) ~NOT_NULL_FLAG;
        rc= def->sp_prepare_create_field(thd, thd->mem_root);
      }
    }
    else
      my_error(ER_BAD_FIELD_ERROR, MYF(0), ref->m_column.str, ref->table.str);
  }

  lex.unit.cleanup();
  thd->temporary_tables= NULL; // Avoid closing temporary tables
  close_thread_tables(thd);
  thd->lex= save_lex;
  thd->restore_backup_open_tables_state(&open_tables_state_backup);
  return rc;
}


/**
  This method resolves the structure of a variable declared as:
     rec t1%ROWTYPE;
  It opens the table "t1" and copies its structure to %ROWTYPE variable.
*/
bool sp_rcontext::resolve_table_rowtype_ref(THD *thd,
                                            Row_definition_list &defs,
                                            Table_ident *ref)
{
  Open_tables_backup open_tables_state_backup;
  thd->reset_n_backup_open_tables_state(&open_tables_state_backup);

  TABLE_LIST *table_list;
  LEX *save_lex= thd->lex;
  bool rc= true;

  /*
    Create a temporary LEX on stack and switch to it.
    In case of VIEW, open_tables_only_view_structure() will open more
    tables/views recursively. We want to avoid them to stick to the current LEX.
  */
  sp_lex_local lex(thd, thd->lex);
  thd->lex= &lex;

  lex.context_analysis_only= CONTEXT_ANALYSIS_ONLY_VIEW;
  // Make %ROWTYPE variables see temporary tables that shadow permanent tables
  thd->temporary_tables= open_tables_state_backup.temporary_tables;

  if ((table_list= lex.select_lex.add_table_to_list(thd, ref, NULL, 0,
                                                    TL_READ_NO_INSERT,
                                                    MDL_SHARED_READ)) &&
      !check_table_access(thd, SELECT_ACL, table_list, TRUE, UINT_MAX, FALSE) &&
      !open_tables_only_view_structure(thd, table_list,
                                       thd->mdl_context.has_locks()))
  {
    for (Field **src= lex.query_tables->table->field; *src; src++)
    {
      /*
         Make field names on the THD memory root,
         as the table will be closed and freed soon,
         in the end of this method.
      */
      LEX_CSTRING tmp= src[0]->field_name;
      Spvar_definition *def;
      if ((rc= check_column_grant_for_type_ref(thd, table_list,
                                               tmp.str, tmp.length)) ||
          (rc= !(src[0]->field_name.str= thd->strmake(tmp.str, tmp.length))) ||
          (rc= !(def= new (thd->mem_root) Spvar_definition(thd, *src))))
        break;
      src[0]->field_name.str= tmp.str; // Restore field name, just in case.
      def->flags&= (uint) ~NOT_NULL_FLAG;
      if ((rc= def->sp_prepare_create_field(thd, thd->mem_root)))
        break;
      defs.push_back(def, thd->mem_root);
    }
  }

  lex.unit.cleanup();
  thd->temporary_tables= NULL; // Avoid closing temporary tables
  close_thread_tables(thd);
  thd->lex= save_lex;
  thd->restore_backup_open_tables_state(&open_tables_state_backup);
  return rc;
}


bool sp_rcontext::resolve_type_refs(THD *thd, List<Spvar_definition> &defs)
{
  List_iterator<Spvar_definition> it(defs);
  Spvar_definition *def;
  while ((def= it++))
  {
    if (def->is_column_type_ref() &&
        resolve_type_ref(thd, def, def->column_type_ref()))
      return true;
  }
  return false;
};


bool sp_rcontext::init_var_items(THD *thd,
                                 List<Spvar_definition> &field_def_lst)
{
  uint num_vars= m_root_parsing_ctx->max_var_index();

  m_var_items.reset(
    static_cast<Item **> (
      thd->alloc(num_vars * sizeof (Item *))),
    num_vars);

  if (!m_var_items.array())
    return true;

  DBUG_ASSERT(field_def_lst.elements == num_vars);
  List_iterator<Spvar_definition> it(field_def_lst);
  Spvar_definition *def= it++;

  for (uint idx= 0; idx < num_vars; ++idx, def= it++)
  {
    Field *field= m_var_table->field[idx];
    if (def->is_table_rowtype_ref())
    {
      Row_definition_list defs;
      Item_field_row *item= new (thd->mem_root) Item_field_row(thd, field);
      if (!(m_var_items[idx]= item) ||
          resolve_table_rowtype_ref(thd, defs, def->table_rowtype_ref()) ||
          item->row_create_items(thd, &defs))
        return true;
    }
    else if (def->is_cursor_rowtype_ref())
    {
      Row_definition_list defs;
      Item_field_row *item= new (thd->mem_root) Item_field_row(thd, field);
      if (!(m_var_items[idx]= item))
        return true;
    }
    else if (def->is_row())
    {
      Item_field_row *item= new (thd->mem_root) Item_field_row(thd, field);
      if (!(m_var_items[idx]= item) ||
          item->row_create_items(thd, def->row_field_definitions()))
        return true;
    }
    else
    {
      if (!(m_var_items[idx]= new (thd->mem_root) Item_field(thd, field)))
        return true;
    }
  }
  return false;
}


bool Item_spvar_args::row_create_items(THD *thd, List<Spvar_definition> *list)
{
  DBUG_ASSERT(list);
  if (!(m_table= create_virtual_tmp_table(thd, *list)))
    return true;

  if (alloc_arguments(thd, list->elements))
    return true;

  List_iterator<Spvar_definition> it(*list);
  Spvar_definition *def;
  for (arg_count= 0; (def= it++); arg_count++)
  {
    if (!(args[arg_count]= new (thd->mem_root)
                           Item_field(thd, m_table->field[arg_count])))
      return true;
  }
  return false;
}


Item_spvar_args::~Item_spvar_args()
{
  if (m_table)
    free_blobs(m_table);
}


bool sp_rcontext::set_return_value(THD *thd, Item **return_value_item)
{
  DBUG_ASSERT(m_return_value_fld);

  m_return_value_set = true;

  return sp_eval_expr(thd, NULL, m_return_value_fld, return_value_item);
}


bool sp_rcontext::push_cursor(THD *thd, sp_lex_keeper *lex_keeper)
{
  /*
    We should create cursors in the callers arena, as
    it could be (and usually is) used in several instructions.
  */
  sp_cursor *c= new (callers_arena->mem_root) sp_cursor(thd, lex_keeper);

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


int sp_rcontext::set_variable(THD *thd, uint idx, Item **value)
{
  Field *field= m_var_table->field[idx];
  if (!value)
  {
    field->set_null();
    return 0;
  }
  Item *dst= m_var_items[idx];

  if (dst->cmp_type() != ROW_RESULT)
    return sp_eval_expr(thd, dst, m_var_table->field[idx], value);

  DBUG_ASSERT(dst->type() == Item::FIELD_ITEM);
  if (value[0]->type() == Item::NULL_ITEM)
  {
    /*
      We're in a auto-generated sp_inst_set, to assign
      the explicit default NULL value to a ROW variable.
    */
    for (uint i= 0; i < dst->cols(); i++)
    {
      Item_field_row *item_field_row= (Item_field_row*) dst;
      item_field_row->get_row_field(i)->set_null();
    }
    return false;
  }

  /**
    - In case if we're assigning a ROW variable from another ROW variable,
      value[0] points to Item_splocal. sp_prepare_func_item() will return the
      fixed underlying Item_field_spvar with ROW members in its aguments().
    - In case if we're assigning from a ROW() value, src and value[0] will
      point to the same Item_row.
  */
  Item *src;
  if (!(src= sp_prepare_func_item(thd, value, dst->cols())) ||
      src->cmp_type() != ROW_RESULT)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), dst->cols());
    return true;
  }
  DBUG_ASSERT(dst->cols() == src->cols());
  for (uint i= 0; i < src->cols(); i++)
    set_variable_row_field(thd, idx, i, src->addr(i));
  return false;
}


void sp_rcontext::set_variable_row_field_to_null(THD *thd,
                                                 uint var_idx,
                                                 uint field_idx)
{
  Item *dst= get_item(var_idx);
  DBUG_ASSERT(dst->type() == Item::FIELD_ITEM);
  DBUG_ASSERT(dst->cmp_type() == ROW_RESULT);
  Item_field_row *item_field_row= (Item_field_row*) dst;
  item_field_row->get_row_field(field_idx)->set_null();
}


int sp_rcontext::set_variable_row_field(THD *thd, uint var_idx, uint field_idx,
                                        Item **value)
{
  DBUG_ASSERT(value);
  Item *dst= get_item(var_idx);
  DBUG_ASSERT(dst->type() == Item::FIELD_ITEM);
  DBUG_ASSERT(dst->cmp_type() == ROW_RESULT);
  Item_field_row *item_field_row= (Item_field_row*) dst;

  Item *expr_item= sp_prepare_func_item(thd, value);
  if (!expr_item)
  {
    DBUG_ASSERT(thd->is_error());
    return true;
  }
  return sp_eval_expr(thd,
                      item_field_row->arguments()[field_idx],
                      item_field_row->get_row_field(field_idx),
                      value);
}


int sp_rcontext::set_variable_row(THD *thd, uint var_idx, List<Item> &items)
{
  DBUG_ENTER("sp_rcontext::set_variable_row");
  DBUG_ASSERT(thd->spcont->get_item(var_idx)->cols() == items.elements);
  List_iterator<Item> it(items);
  Item *item;
  for (uint i= 0 ; (item= it++) ; i++)
  {
    int rc;
    if ((rc= thd->spcont->set_variable_row_field(thd, var_idx, i, &item)))
      DBUG_RETURN(rc);
  }
  DBUG_RETURN(0);
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


sp_cursor::sp_cursor(THD *thd_arg, sp_lex_keeper *lex_keeper):
   result(thd_arg),
   m_lex_keeper(lex_keeper),
   server_side_cursor(NULL),
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


/**
  Open the cursor, but do not copy data.
  This method is used to fetch the cursor structure
  to cursor%ROWTYPE routine variables.
  Data copying is suppressed by setting thd->lex->limit_rows_examined to 0.
*/
int sp_cursor::open_view_structure_only(THD *thd)
{
  int res;
  int thd_no_errors_save= thd->no_errors;
  Item *limit_rows_examined= thd->lex->limit_rows_examined; // No data copying
  if (!(thd->lex->limit_rows_examined= new (thd->mem_root) Item_uint(thd, 0)))
    return -1;
  thd->no_errors= true; // Suppress ER_QUERY_EXCEEDED_ROWS_EXAMINED_LIMIT
  res= open(thd);
  thd->no_errors= thd_no_errors_save;
  thd->lex->limit_rows_examined= limit_rows_examined;
  return res;
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
  if (vars->elements != result.get_field_count() &&
      (vars->elements != 1 ||
       result.get_field_count() !=
       thd->spcont->get_item(vars->head()->offset)->cols()))
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


bool sp_cursor::export_structure(THD *thd, Row_definition_list *list)
{
  return server_side_cursor->export_structure(thd, list);
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


bool sp_cursor::Select_fetch_into_spvars::
       send_data_to_variable_list(List<sp_variable> &vars, List<Item> &items)
{
  List_iterator_fast<sp_variable> spvar_iter(vars);
  List_iterator_fast<Item> item_iter(items);
  sp_variable *spvar;
  Item *item;

  /* Must be ensured by the caller */
  DBUG_ASSERT(vars.elements == items.elements);

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


int sp_cursor::Select_fetch_into_spvars::send_data(List<Item> &items)
{
  Item *item;
  /*
    If we have only one variable in spvar_list, and this is a ROW variable,
    and the number of fields in the ROW variable matches the number of
    fields in the query result, we fetch to this ROW variable.

    If there is one variable, and it is a ROW variable, but its number
    of fields does not match the number of fields in the query result,
    we go through send_data_to_variable_list(). It will report an error
    on attempt to assign a scalar value to a ROW variable.
  */
  return spvar_list->elements == 1 &&
         (item= thd->spcont->get_item(spvar_list->head()->offset)) &&
         item->type_handler() == &type_handler_row &&
         item->cols() == items.elements ?
    thd->spcont->set_variable_row(thd, spvar_list->head()->offset, items) :
    send_data_to_variable_list(*spvar_list, items);
}
