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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
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


Sp_rcontext_handler_local sp_rcontext_handler_local;
Sp_rcontext_handler_package_body sp_rcontext_handler_package_body;

sp_rcontext *Sp_rcontext_handler_local::get_rcontext(sp_rcontext *ctx) const
{
  return ctx;
}

sp_rcontext *Sp_rcontext_handler_package_body::get_rcontext(sp_rcontext *ctx) const
{
  return ctx->m_sp->m_parent->m_rcontext;
}

const LEX_CSTRING *Sp_rcontext_handler_local::get_name_prefix() const
{
  return &empty_clex_str;
}

const LEX_CSTRING *Sp_rcontext_handler_package_body::get_name_prefix() const
{
  static const LEX_CSTRING sp_package_body_variable_prefix_clex_str=
                           {STRING_WITH_LEN("PACKAGE_BODY.")};
  return &sp_package_body_variable_prefix_clex_str;
}


///////////////////////////////////////////////////////////////////////////
// sp_rcontext implementation.
///////////////////////////////////////////////////////////////////////////


sp_rcontext::sp_rcontext(const sp_head *owner,
                         const sp_pcontext *root_parsing_ctx,
                         Field *return_value_fld,
                         bool in_sub_stmt)
  :end_partial_result_set(false),
   pause_state(false), quit_func(false), instr_ptr(0),
   m_sp(owner),
   m_root_parsing_ctx(root_parsing_ctx),
   m_var_table(NULL),
   m_return_value_fld(return_value_fld),
   m_return_value_set(false),
   m_in_sub_stmt(in_sub_stmt),
   m_handlers(PSI_INSTRUMENT_MEM), m_handler_call_stack(PSI_INSTRUMENT_MEM),
   m_ccount(0)
{
}


sp_rcontext::~sp_rcontext()
{
  delete m_var_table;
  // Leave m_handlers, m_handler_call_stack, m_var_items, m_cstack
  // and m_case_expr_holders untouched.
  // They are allocated in mem roots and will be freed accordingly.
}


sp_rcontext *sp_rcontext::create(THD *thd,
                                 const sp_head *owner,
                                 const sp_pcontext *root_parsing_ctx,
                                 Field *return_value_fld,
                                 Row_definition_list &field_def_lst)
{
  SELECT_LEX *save_current_select;
  sp_rcontext *ctx= new (thd->mem_root) sp_rcontext(owner,
                                                    root_parsing_ctx,
                                                    return_value_fld,
                                                    thd->in_sub_stmt);
  if (!ctx)
    return NULL;

  /* Reset current_select as it's checked in Item_ident::Item_ident */
  save_current_select= thd->lex->current_select;
  thd->lex->current_select= 0;

  if (ctx->alloc_arrays(thd) ||
      ctx->init_var_table(thd, field_def_lst) ||
      ctx->init_var_items(thd, field_def_lst))
  {
    delete ctx;
    ctx= 0;
  }

  thd->lex->current_select= save_current_select;
  return ctx;
}


bool Row_definition_list::append_uniq(MEM_ROOT *mem_root, Spvar_definition *var)
{
  DBUG_ASSERT(elements);
  uint unused;
  if (unlikely(find_row_field_by_name(&var->field_name, &unused)))
  {
    my_error(ER_DUP_FIELDNAME, MYF(0), var->field_name.str);
    return true;
  }
  return push_back(var, mem_root);
}


bool Row_definition_list::
       adjust_formal_params_to_actual_params(THD *thd, List<Item> *args)
{
  List_iterator<Spvar_definition> it(*this);
  List_iterator<Item> it_args(*args);
  DBUG_ASSERT(elements >= args->elements );
  Spvar_definition *def;
  Item *arg;
  while ((def= it++) && (arg= it_args++))
  {
    if (def->type_handler()->adjust_spparam_type(def, arg))
      return true;
  }
  return false;
}


bool Row_definition_list::
       adjust_formal_params_to_actual_params(THD *thd,
                                             Item **args, uint arg_count)
{
  List_iterator<Spvar_definition> it(*this);
  DBUG_ASSERT(elements >= arg_count );
  Spvar_definition *def;
  for (uint i= 0; (def= it++) && (i < arg_count) ; i++)
  {
    if (def->type_handler()->adjust_spparam_type(def, args[i]))
      return true;
  }
  return false;
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
                                const char *str, size_t length,
                                Field *fld)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  table_list->table->grant.want_privilege= SELECT_ACL;
  return check_column_grant_in_table_ref(thd, table_list, str, length, fld);
#else
  return false;
#endif
}


/**
  This method implementation is very close to fill_schema_table_by_open().
*/
bool Qualified_column_ident::resolve_type_ref(THD *thd, Column_definition *def)
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

  if ((table_list=
         lex.first_select_lex()->add_table_to_list(thd, this, NULL, 0,
                                                   TL_READ_NO_INSERT,
                                                   MDL_SHARED_READ)) &&
      !check_table_access(thd, SELECT_ACL, table_list, TRUE, UINT_MAX, FALSE) &&
      !open_tables_only_view_structure(thd, table_list,
                                       thd->mdl_context.has_locks()))
  {
    if (likely((src= lex.query_tables->table->find_field_by_name(&m_column))))
    {
      if (!(rc= check_column_grant_for_type_ref(thd, table_list,
                                                m_column.str,
                                                m_column.length, src)))
      {
        *def= Column_definition(thd, src, NULL/*No defaults,no constraints*/);
        def->flags&= (uint) ~NOT_NULL_FLAG;
        rc= def->sp_prepare_create_field(thd, thd->mem_root);
      }
    }
    else
      my_error(ER_BAD_FIELD_ERROR, MYF(0), m_column.str, table.str);
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
bool Table_ident::resolve_table_rowtype_ref(THD *thd,
                                            Row_definition_list &defs)
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

  if ((table_list=
         lex.first_select_lex()->add_table_to_list(thd, this, NULL, 0,
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
                                               tmp.str, tmp.length,src[0])) ||
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


bool Row_definition_list::resolve_type_refs(THD *thd)
{
  List_iterator<Spvar_definition> it(*this);
  Spvar_definition *def;
  while ((def= it++))
  {
    if (def->is_column_type_ref() &&
        def->column_type_ref()->resolve_type_ref(thd, def))
      return true;
  }
  return false;
};


bool sp_rcontext::init_var_items(THD *thd,
                                 List<Spvar_definition> &field_def_lst)
{
  uint num_vars= m_root_parsing_ctx->max_var_index();

  m_var_items.reset(
    static_cast<Item_field **> (
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
          def->table_rowtype_ref()->resolve_table_rowtype_ref(thd, defs) ||
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


bool Item_field_row::row_create_items(THD *thd, List<Spvar_definition> *list)
{
  DBUG_ASSERT(list);
  DBUG_ASSERT(field);
  Virtual_tmp_table **ptable= field->virtual_tmp_table_addr();
  DBUG_ASSERT(ptable);
  if (!(ptable[0]= create_virtual_tmp_table(thd, *list)))
    return true;

  if (alloc_arguments(thd, list->elements))
    return true;

  List_iterator<Spvar_definition> it(*list);
  Spvar_definition *def;
  for (arg_count= 0; (def= it++); arg_count++)
  {
    if (!(args[arg_count]= new (thd->mem_root)
                           Item_field(thd, ptable[0]->field[arg_count])))
      return true;
  }
  return false;
}


bool sp_rcontext::set_return_value(THD *thd, Item **return_value_item)
{
  DBUG_ASSERT(m_return_value_fld);

  m_return_value_set = true;

  return thd->sp_eval_expr(m_return_value_fld, return_value_item);
}


void sp_rcontext::push_cursor(sp_cursor *c)
{
  m_cstack[m_ccount++]= c;
}


void sp_rcontext::pop_cursor(THD *thd)
{
  DBUG_ASSERT(m_ccount > 0);
  if (m_cstack[m_ccount - 1]->is_open())
    m_cstack[m_ccount - 1]->close(thd);
  m_ccount--;
}


void sp_rcontext::pop_cursors(THD *thd, size_t count)
{
  DBUG_ASSERT(m_ccount >= count);
  while (count--)
    pop_cursor(thd);
}


bool sp_rcontext::push_handler(sp_instr_hpush_jump *entry)
{
  return m_handlers.append(entry);
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
  if (unlikely(thd->is_fatal_sub_stmt_error) && m_in_sub_stmt)
    DBUG_RETURN(false);

  Diagnostics_area *da= thd->get_stmt_da();
  const sp_handler *found_handler= NULL;
  const Sql_condition *found_condition= NULL;

  if (unlikely(thd->is_error()))
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
                                                    da->message(),
                                                    da->current_row_for_warning());
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

  sp_instr_hpush_jump *handler_entry= NULL;
  for (size_t i= 0; i < m_handlers.elements(); ++i)
  {
    sp_instr_hpush_jump *h= m_handlers.at(i);

    if (h->get_handler() == found_handler)
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

  uint continue_ip= handler_entry->get_handler()->type == sp_handler::CONTINUE ?
    cur_spi->get_cont_dest() : 0;

  /* End aborted result set. */
  if (end_partial_result_set)
    thd->protocol->end_partial_result_set(thd);

  /* Reset error state. */
  thd->clear_error();
  thd->reset_killed();      // Some errors set thd->killed, (e.g. "bad data").

  /* Add a frame to handler-call-stack. */
  Sql_condition_info *cond_info=
    new (callers_arena->mem_root) Sql_condition_info(found_condition,
                                                     callers_arena);
  Handler_call_frame *frame=
    new (callers_arena->mem_root) Handler_call_frame(cond_info, continue_ip);
  m_handler_call_stack.append(frame);

  *ip= handler_entry->m_ip + 1;

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
  DBUG_ENTER("sp_rcontext::set_variable");
  DBUG_ASSERT(value);
  DBUG_RETURN(thd->sp_eval_expr(m_var_table->field[idx], value));
}


int sp_rcontext::set_variable_row_field(THD *thd, uint var_idx, uint field_idx,
                                        Item **value)
{
  DBUG_ENTER("sp_rcontext::set_variable_row_field");
  DBUG_ASSERT(value);
  Virtual_tmp_table *vtable= virtual_tmp_table_for_row(var_idx);
  DBUG_RETURN(thd->sp_eval_expr(vtable->field[field_idx], value));
}


int sp_rcontext::set_variable_row_field_by_name(THD *thd, uint var_idx,
                                                const LEX_CSTRING &field_name,
                                                Item **value)
{
  DBUG_ENTER("sp_rcontext::set_variable_row_field_by_name");
  uint field_idx;
  if (find_row_field_by_name_or_error(&field_idx, var_idx, field_name))
    DBUG_RETURN(1);
  DBUG_RETURN(set_variable_row_field(thd, var_idx, field_idx, value));
}


int sp_rcontext::set_variable_row(THD *thd, uint var_idx, List<Item> &items)
{
  DBUG_ENTER("sp_rcontext::set_variable_row");
  DBUG_ASSERT(get_variable(var_idx)->cols() == items.elements);
  Virtual_tmp_table *vtable= virtual_tmp_table_for_row(var_idx);
  Sp_eval_expr_state state(thd);
  DBUG_RETURN(vtable->sp_set_all_fields_from_item_list(thd, items));
}


Virtual_tmp_table *sp_rcontext::virtual_tmp_table_for_row(uint var_idx)
{
  DBUG_ASSERT(get_variable(var_idx)->type() == Item::FIELD_ITEM);
  DBUG_ASSERT(get_variable(var_idx)->cmp_type() == ROW_RESULT);
  Field *field= m_var_table->field[var_idx];
  Virtual_tmp_table **ptable= field->virtual_tmp_table_addr();
  DBUG_ASSERT(ptable);
  DBUG_ASSERT(ptable[0]);
  return ptable[0];
}


bool sp_rcontext::find_row_field_by_name_or_error(uint *field_idx,
                                                  uint var_idx,
                                                  const LEX_CSTRING &field_name)
{
  Virtual_tmp_table *vtable= virtual_tmp_table_for_row(var_idx);
  Field *row= m_var_table->field[var_idx];
  return vtable->sp_find_field_by_name_or_error(field_idx,
                                                row->field_name, field_name);
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
  Item *case_expr_item= thd->sp_prepare_func_item(case_expr_item_ptr);
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
  sp_cursor_statistics::reset();
  destroy();
  return 0;
}


void sp_cursor::destroy()
{
  delete server_side_cursor;
  server_side_cursor= NULL;
}


int sp_cursor::fetch(THD *thd, List<sp_variable> *vars, bool error_on_no_data)
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
       thd->spcont->get_variable(vars->head()->offset)->cols()))
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

  DBUG_ASSERT(!thd->is_error());

  /* Attempt to fetch one row */
  if (server_side_cursor->is_open())
  {
    server_side_cursor->fetch(1);
    if (thd->is_error())
      return -1; // e.g. data type conversion failed
  }

  /*
    If the cursor was pointing after the last row, the fetch will
    close it instead of sending any rows.
  */
  if (! server_side_cursor->is_open())
  {
    m_found= false;
    if (!error_on_no_data)
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
         (item= thd->spcont->get_variable(spvar_list->head()->offset)) &&
         item->type_handler() == &type_handler_row &&
         item->cols() == items.elements ?
    thd->spcont->set_variable_row(thd, spvar_list->head()->offset, items) :
    send_data_to_variable_list(*spvar_list, items);
}
