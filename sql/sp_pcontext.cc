/* Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2009, 2020, MariaDB Corporation.

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

#include "sp_pcontext.h"
#include "sp_head.h"

bool sp_condition_value::equals(const sp_condition_value *cv) const
{
  DBUG_ASSERT(cv);

  /*
    The following test disallows duplicate handlers,
    including user defined exceptions with the same WHEN clause:
      DECLARE
        a EXCEPTION;
        b EXCEPTION;
      BEGIN
        RAUSE a;
      EXCEPTION
        WHEN a THEN RETURN 'a0';
        WHEN a THEN RETURN 'a1';
      END
  */
  if (this == cv)
    return true;

  /*
    The test below considers two conditions of the same type as equal
    (except for the user defined exceptions) to avoid declaring duplicate
    handlers.

    All user defined conditions have type==SQLSTATE
    with the same SQL state and error code.
    It's OK to have multiple user defined conditions:
    DECLARE
      a EXCEPTION;
      b EXCEPTION;
    BEGIN
      RAISE a;
    EXCEPTION
      WHEN a THEN RETURN 'a';
      WHEN b THEN RETURN 'b';
    END;
  */
  if (type != cv->type || m_is_user_defined || cv->m_is_user_defined)
    return false;

  switch (type)
  {
  case sp_condition_value::ERROR_CODE:
    return (get_sql_errno() == cv->get_sql_errno());

  case sp_condition_value::SQLSTATE:
    return Sql_state::eq(cv);

  default:
    return true;
  }
}


void sp_pcontext::init(uint var_offset,
                       uint cursor_offset,
                       int num_case_expressions)
{
  m_var_offset= var_offset;
  m_cursor_offset= cursor_offset;
  m_num_case_exprs= num_case_expressions;

  m_labels.empty();
  m_goto_labels.empty();
}


sp_pcontext::sp_pcontext()
  : Sql_alloc(),
  m_max_var_index(0), m_max_cursor_index(0),
  m_parent(NULL), m_pboundary(0),
  m_vars(PSI_INSTRUMENT_MEM), m_case_expr_ids(PSI_INSTRUMENT_MEM),
  m_conditions(PSI_INSTRUMENT_MEM), m_cursors(PSI_INSTRUMENT_MEM),
  m_handlers(PSI_INSTRUMENT_MEM), m_children(PSI_INSTRUMENT_MEM),
  m_scope(REGULAR_SCOPE)
{
  init(0, 0, 0);
}


sp_pcontext::sp_pcontext(sp_pcontext *prev, sp_pcontext::enum_scope scope)
  : Sql_alloc(),
  m_max_var_index(0), m_max_cursor_index(0),
  m_parent(prev), m_pboundary(0),
  m_vars(PSI_INSTRUMENT_MEM), m_case_expr_ids(PSI_INSTRUMENT_MEM),
  m_conditions(PSI_INSTRUMENT_MEM), m_cursors(PSI_INSTRUMENT_MEM),
  m_handlers(PSI_INSTRUMENT_MEM), m_children(PSI_INSTRUMENT_MEM),
  m_scope(scope)
{
  init(prev->m_var_offset + prev->m_max_var_index,
       prev->current_cursor_count(),
       prev->get_num_case_exprs());
}


sp_pcontext::~sp_pcontext()
{
  for (size_t i= 0; i < m_children.elements(); ++i)
    delete m_children.at(i);
}


sp_pcontext *sp_pcontext::push_context(THD *thd, sp_pcontext::enum_scope scope)
{
  sp_pcontext *child= new (thd->mem_root) sp_pcontext(this, scope);

  if (child)
    m_children.append(child);
  return child;
}


bool cmp_labels(sp_label *a, sp_label *b)
{
  return (lex_string_cmp(system_charset_info, &a->name, &b->name) == 0 &&
          a->type == b->type);
}

sp_pcontext *sp_pcontext::pop_context()
{
  m_parent->m_max_var_index+= m_max_var_index;

  uint submax= max_cursor_index();
  if (submax > m_parent->m_max_cursor_index)
    m_parent->m_max_cursor_index= submax;

  if (m_num_case_exprs > m_parent->m_num_case_exprs)
    m_parent->m_num_case_exprs= m_num_case_exprs;

  /*
  ** Push unresolved goto label to parent context
  */
  sp_label *label;
  List_iterator_fast<sp_label> li(m_goto_labels);
  while ((label= li++))
  {
    if (label->ip == 0)
    {
      m_parent->m_goto_labels.add_unique(label, &cmp_labels);
    }
  }
  return m_parent;
}


uint sp_pcontext::diff_handlers(const sp_pcontext *ctx, bool exclusive) const
{
  uint n= 0;
  const sp_pcontext *pctx= this;
  const sp_pcontext *last_ctx= NULL;

  while (pctx && pctx != ctx)
  {
    n+= (uint)pctx->m_handlers.elements();
    last_ctx= pctx;
    pctx= pctx->parent_context();
  }
  if (pctx)
    return (exclusive && last_ctx ? n -(uint) last_ctx->m_handlers.elements() : n);
  return 0;			// Didn't find ctx
}


uint sp_pcontext::diff_cursors(const sp_pcontext *ctx, bool exclusive) const
{
  uint n= 0;
  const sp_pcontext *pctx= this;
  const sp_pcontext *last_ctx= NULL;

  while (pctx && pctx != ctx)
  {
    n+= (uint)pctx->m_cursors.elements();
    last_ctx= pctx;
    pctx= pctx->parent_context();
  }
  if (pctx)
    return  (exclusive && last_ctx ? (uint)(n - last_ctx->m_cursors.elements()) : n);
  return 0;			// Didn't find ctx
}


sp_variable *sp_pcontext::find_variable(const LEX_CSTRING *name,
                                        bool current_scope_only) const
{
  size_t i= m_vars.elements() - m_pboundary;

  while (i--)
  {
    sp_variable *p= m_vars.at(i);

    if (system_charset_info->strnncoll(name->str, name->length,
		                       p->name.str, p->name.length) == 0)
    {
      return p;
    }
  }

  return (!current_scope_only && m_parent) ?
    m_parent->find_variable(name, false) :
    NULL;
}


/*
  Find a variable by its run-time offset.
  If the variable with a desired run-time offset is not found in this
  context frame, it's recursively searched on parent context frames.

  Note, context frames can have holes:
    CREATE PROCEDURE p1() AS
      x0 INT:=100;
      CURSOR cur(p0 INT, p1 INT) IS SELECT p0, p1;
      x1 INT:=101;
    BEGIN
      ...
    END;
  The variables (x0 and x1) and the cursor parameters (p0 and p1)
  reside in separate parse context frames.

  The variables reside on the top level parse context frame:
  - x0 has frame offset 0 and run-time offset 0
  - x1 has frame offset 1 and run-time offset 3

  The cursor parameters reside on the second level parse context frame:
  - p0 has frame offset 0 and run-time offset 1
  - p1 has frame offset 1 and run-time offset 2

  Run-time offsets on a frame can have holes, but offsets monotonocally grow,
  so run-time offsets of all variables are not greater than the run-time offset
  of the very last variable in this frame.
*/
sp_variable *sp_pcontext::find_variable(uint offset) const
{
  if (m_var_offset <= offset &&
      m_vars.elements() &&
      offset <= get_last_context_variable()->offset)
  {
    for (uint i= 0; i < m_vars.elements(); i++)
    {
      if (m_vars.at(i)->offset == offset)
        return m_vars.at(i); // This frame
    }
  }

  return m_parent ?
         m_parent->find_variable(offset) :    // Some previous frame
         NULL;                                // Index out of bounds
}


sp_variable *sp_pcontext::add_variable(THD *thd, const LEX_CSTRING *name)
{
  sp_variable *p=
    new (thd->mem_root) sp_variable(name, m_var_offset + m_max_var_index);

  if (!p)
    return NULL;

  ++m_max_var_index;

  return m_vars.append(p) ? NULL : p;
}

sp_label *sp_pcontext::push_label(THD *thd, const LEX_CSTRING *name, uint ip,
                                  sp_label::enum_type type,
                                  List<sp_label> *list)
{
  sp_label *label=
    new (thd->mem_root) sp_label(name, ip, type, this);

  if (!label)
    return NULL;

  list->push_front(label, thd->mem_root);

  return label;
}

sp_label *sp_pcontext::find_goto_label(const LEX_CSTRING *name, bool recusive)
{
  List_iterator_fast<sp_label> li(m_goto_labels);
  sp_label *lab;

  while ((lab= li++))
  {
    if (lex_string_cmp(system_charset_info, name, &lab->name) == 0)
      return lab;
  }

  if (!recusive)
    return NULL;

  /*
    Note about exception handlers.
    See SQL:2003 SQL/PSM (ISO/IEC 9075-4:2003),
    section 13.1 <compound statement>,
    syntax rule 4.
    In short, a DECLARE HANDLER block can not refer
    to labels from the parent context, as they are out of scope.
  */
  if (m_scope == HANDLER_SCOPE && m_parent)
  {
    if (m_parent->m_parent)
    {
      // Skip the parent context
      return m_parent->m_parent->find_goto_label(name);
    }
  }

  return m_parent && (m_scope == REGULAR_SCOPE) ?
         m_parent->find_goto_label(name) :
         NULL;
}


sp_label *sp_pcontext::find_label(const LEX_CSTRING *name)
{
  List_iterator_fast<sp_label> li(m_labels);
  sp_label *lab;

  while ((lab= li++))
  {
    if (lex_string_cmp(system_charset_info, name, &lab->name) == 0)
      return lab;
  }

  /*
    Note about exception handlers.
    See SQL:2003 SQL/PSM (ISO/IEC 9075-4:2003),
    section 13.1 <compound statement>,
    syntax rule 4.
    In short, a DECLARE HANDLER block can not refer
    to labels from the parent context, as they are out of scope.
  */
  return (m_parent && (m_scope == REGULAR_SCOPE)) ?
         m_parent->find_label(name) :
         NULL;
}


sp_label *sp_pcontext::find_label_current_loop_start()
{
  List_iterator_fast<sp_label> li(m_labels);
  sp_label *lab;

  while ((lab= li++))
  {
    if (lab->type == sp_label::ITERATION)
      return lab;
  }
  // See a comment in sp_pcontext::find_label()
  return (m_parent && (m_scope == REGULAR_SCOPE)) ?
         m_parent->find_label_current_loop_start() :
         NULL;
}


bool sp_pcontext::add_condition(THD *thd,
                                const LEX_CSTRING *name,
                                sp_condition_value *value)
{
  sp_condition *p= new (thd->mem_root) sp_condition(name, value);

  if (p == NULL)
    return true;

  return m_conditions.append(p);
}


sp_condition_value *sp_pcontext::find_condition(const LEX_CSTRING *name,
                                                bool current_scope_only) const
{
  size_t i= m_conditions.elements();

  while (i--)
  {
    sp_condition *p= m_conditions.at(i);

    if (p->eq_name(name))
    {
      return p->value;
    }
  }

  return (!current_scope_only && m_parent) ?
    m_parent->find_condition(name, false) :
    NULL;
}

sp_condition_value *
sp_pcontext::find_declared_or_predefined_condition(THD *thd,
                                                   const LEX_CSTRING *name)
                                                   const
{
  sp_condition_value *p= find_condition(name, false);
  if (p)
    return p;
  if (thd->variables.sql_mode & MODE_ORACLE)
    return find_predefined_condition(name);
  return NULL;
}


static sp_condition_value
  // Warnings
  cond_no_data_found(ER_SP_FETCH_NO_DATA, "01000"),
  // Errors
  cond_invalid_cursor(ER_SP_CURSOR_NOT_OPEN, "24000"),
  cond_dup_val_on_index(ER_DUP_ENTRY, "23000"),
  cond_dup_val_on_index2(ER_DUP_ENTRY_WITH_KEY_NAME, "23000"),
  cond_too_many_rows(ER_TOO_MANY_ROWS, "42000");


static sp_condition sp_predefined_conditions[]=
{
  // Warnings
  sp_condition(STRING_WITH_LEN("NO_DATA_FOUND"), &cond_no_data_found),
  // Errors
  sp_condition(STRING_WITH_LEN("INVALID_CURSOR"), &cond_invalid_cursor),
  sp_condition(STRING_WITH_LEN("DUP_VAL_ON_INDEX"), &cond_dup_val_on_index),
  sp_condition(STRING_WITH_LEN("DUP_VAL_ON_INDEX"), &cond_dup_val_on_index2),
  sp_condition(STRING_WITH_LEN("TOO_MANY_ROWS"), &cond_too_many_rows)
};


sp_condition_value *
sp_pcontext::find_predefined_condition(const LEX_CSTRING *name) const
{
  for (uint i= 0; i < array_elements(sp_predefined_conditions) ; i++)
  {
    if (sp_predefined_conditions[i].eq_name(name))
      return sp_predefined_conditions[i].value;
  }
  return NULL;
}


sp_handler *sp_pcontext::add_handler(THD *thd,
                                     sp_handler::enum_type type)
{
  sp_handler *h= new (thd->mem_root) sp_handler(type);

  if (!h)
    return NULL;

  return m_handlers.append(h) ? NULL : h;
}


bool sp_pcontext::check_duplicate_handler(
  const sp_condition_value *cond_value) const
{
  for (size_t i= 0; i < m_handlers.elements(); ++i)
  {
    sp_handler *h= m_handlers.at(i);

    List_iterator_fast<sp_condition_value> li(h->condition_values);
    sp_condition_value *cv;

    while ((cv= li++))
    {
      if (cond_value->equals(cv))
        return true;
    }
  }

  return false;
}


bool sp_condition_value::matches(const Sql_condition_identity &value,
                                 const sp_condition_value *found_cv) const
{
  bool user_value_matched= !value.get_user_condition_value() ||
                           this == value.get_user_condition_value();

  switch (type)
  {
  case sp_condition_value::ERROR_CODE:
    return user_value_matched &&
           value.get_sql_errno() == get_sql_errno() &&
           (!found_cv || found_cv->type > sp_condition_value::ERROR_CODE);

  case sp_condition_value::SQLSTATE:
    return user_value_matched &&
           Sql_state::eq(&value) &&
           (!found_cv || found_cv->type > sp_condition_value::SQLSTATE);

  case sp_condition_value::WARNING:
    return user_value_matched &&
           (value.Sql_state::is_warning() ||
            value.get_level() == Sql_condition::WARN_LEVEL_WARN) &&
           !found_cv;

  case sp_condition_value::NOT_FOUND:
    return user_value_matched &&
           value.Sql_state::is_not_found() &&
           !found_cv;

  case sp_condition_value::EXCEPTION:
    /*
      In sql_mode=ORACLE this construct should catch both errors and warnings:
        EXCEPTION
          WHEN OTHERS THEN ...;
      E.g. NO_DATA_FOUND is more like a warning than an error,
      and it should be caught.

      We don't check user_value_matched here.
      "WHEN OTHERS" catches all user defined exception.
    */
    return (((current_thd->variables.sql_mode & MODE_ORACLE) ||
           (value.Sql_state::is_exception() &&
            value.get_level() == Sql_condition::WARN_LEVEL_ERROR)) &&
           !found_cv);
  }
  return false;
}


sp_handler*
sp_pcontext::find_handler(const Sql_condition_identity &value) const
{
  sp_handler *found_handler= NULL;
  sp_condition_value *found_cv= NULL;

  for (size_t i= 0; i < m_handlers.elements(); ++i)
  {
    sp_handler *h= m_handlers.at(i);

    List_iterator_fast<sp_condition_value> li(h->condition_values);
    sp_condition_value *cv;

    while ((cv= li++))
    {
      if (cv->matches(value, found_cv))
      {
        found_cv= cv;
        found_handler= h;
      }
    }
  }

  if (found_handler)
    return found_handler;


  // There is no appropriate handler in this parsing context. We need to look up
  // in parent contexts. There might be two cases here:
  //
  // 1. The current context has REGULAR_SCOPE. That means, it's a simple
  // BEGIN..END block:
  //     ...
  //     BEGIN
  //       ... # We're here.
  //     END
  //     ...
  // In this case we simply call find_handler() on parent's context recursively.
  //
  // 2. The current context has HANDLER_SCOPE. That means, we're inside an
  // SQL-handler block:
  //   ...
  //   DECLARE ... HANDLER FOR ...
  //   BEGIN
  //     ... # We're here.
  //   END
  //   ...
  // In this case we can not just call parent's find_handler(), because
  // parent's handler don't catch conditions from this scope. Instead, we should
  // try to find first parent context (we might have nested handler
  // declarations), which has REGULAR_SCOPE (i.e. which is regular BEGIN..END
  // block).

  const sp_pcontext *p= this;

  while (p && p->m_scope == HANDLER_SCOPE)
    p= p->m_parent;

  if (!p || !p->m_parent)
    return NULL;

  return p->m_parent->find_handler(value);
}


bool sp_pcontext::add_cursor(const LEX_CSTRING *name, sp_pcontext *param_ctx,
                             sp_lex_cursor *lex)
{
  if (m_cursors.elements() == m_max_cursor_index)
    ++m_max_cursor_index;

  return m_cursors.append(sp_pcursor(name, param_ctx, lex));
}


const sp_pcursor *sp_pcontext::find_cursor(const LEX_CSTRING *name,
                                           uint *poff,
                                           bool current_scope_only) const
{
  uint i= (uint)m_cursors.elements();

  while (i--)
  {
    LEX_CSTRING n= m_cursors.at(i);

    if (system_charset_info->strnncoll(name->str, name->length,
		                       n.str, n.length) == 0)
    {
      *poff= m_cursor_offset + i;
      return &m_cursors.at(i);
    }
  }

  return (!current_scope_only && m_parent) ?
    m_parent->find_cursor(name, poff, false) :
    NULL;
}


void sp_pcontext::retrieve_field_definitions(
  List<Spvar_definition> *field_def_lst) const
{
  /* Put local/context fields in the result list. */

  size_t next_child= 0;
  for (size_t i= 0; i < m_vars.elements(); ++i)
  {
    sp_variable *var_def= m_vars.at(i);

    /*
      The context can have holes in run-time offsets,
      the missing offsets reside on the children contexts in such cases.
      Example:
        CREATE PROCEDURE p1() AS
          x0 INT:=100;        -- context 0, position 0, run-time 0
          CURSOR cur(
            p0 INT,           -- context 1, position 0, run-time 1
            p1 INT            -- context 1, position 1, run-time 2
          ) IS SELECT p0, p1;
          x1 INT:=101;        -- context 0, position 1, run-time 3
        BEGIN
          ...
        END;
      See more comments in sp_pcontext::find_variable().
      We must retrieve the definitions in the order of their run-time offsets.
      Check that there are children that should go before the current variable.
    */
    for ( ; next_child < m_children.elements(); next_child++)
    {
      sp_pcontext *child= m_children.at(next_child);
      if (!child->context_var_count() ||
          child->get_context_variable(0)->offset > var_def->offset)
        break;
      /*
        All variables on the embedded context (that fills holes of the parent)
        should have the run-time offset strictly less than var_def.
      */
      DBUG_ASSERT(child->get_context_variable(0)->offset < var_def->offset);
      DBUG_ASSERT(child->get_last_context_variable()->offset < var_def->offset);
      child->retrieve_field_definitions(field_def_lst);
    }
    field_def_lst->push_back(&var_def->field_def);
  }

  /* Put the fields of the remaining enclosed contexts in the result list. */

  for (size_t i= next_child; i < m_children.elements(); ++i)
    m_children.at(i)->retrieve_field_definitions(field_def_lst);
}


const sp_pcursor *sp_pcontext::find_cursor(uint offset) const
{
  if (m_cursor_offset <= offset &&
      offset < m_cursor_offset + m_cursors.elements())
  {
    return &m_cursors.at(offset - m_cursor_offset);   // This frame
  }

  return m_parent ?
         m_parent->find_cursor(offset) :  // Some previous frame
         NULL;                            // Index out of bounds
}


bool sp_pcursor::check_param_count_with_error(uint param_count) const
{
  if (param_count != (m_param_context ?
                      m_param_context->context_var_count() : 0))
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_CURSOR, MYF(0), LEX_CSTRING::str);
    return true;
  }
  return false;
}


const Spvar_definition *
sp_variable::find_row_field(const LEX_CSTRING *var_name,
                            const LEX_CSTRING *field_name,
                            uint *row_field_offset)
{
  if (!field_def.is_row())
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "'%s' is not a row variable", MYF(0), var_name->str);
    return NULL;
  }
  const Spvar_definition *def;
  if ((def= field_def.find_row_field_by_name(field_name, row_field_offset)))
    return def;
  my_error(ER_ROW_VARIABLE_DOES_NOT_HAVE_FIELD, MYF(0),
           var_name->str, field_name->str);
  return NULL;
}
