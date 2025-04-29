/*
   Copyright (c) 2025, Rakuten Securities
   Copyright (c) 2015, 2025, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/
#include "sql_type.h"
#include "sql_type_row.h"
#include "item.h"
#include "sql_select.h"
#include "field.h"
#include "sp_rcontext.h"
#include "sp_type_def.h"
#include "sp_head.h"


bool Type_handler_row::
  sp_variable_declarations_row_finalize(THD *thd, LEX *lex, int nvars,
                                        Row_definition_list *row)
{
  DBUG_ASSERT(row);
  /*
    Prepare all row fields.
    Note, we do it only one time outside of the below loop.
    The converted list in "row" is further reused by all variable
    declarations processed by the current call.
    Example:
      DECLARE
        a, b, c ROW(x VARCHAR(10) CHARACTER SET utf8);
      BEGIN
        ...
      END;
  */
  if (lex->sphead->row_fill_field_definitions(thd, row))
    return true;

  for (uint i= 0 ; i < (uint) nvars ; i++)
  {
    uint offset= (uint) nvars - 1 - i;
    sp_variable *spvar= lex->spcont->get_last_context_variable(offset);
    spvar->field_def.set_row_field_definitions(row);
    if (lex->sphead->fill_spvar_definition(thd, &spvar->field_def,
                                           &spvar->name))
      return true;
  }
  return false;
}


class my_var_sp_row: public my_var_sp
{
public:
  my_var_sp_row(const Lex_ident_sys_st &name, const sp_rcontext_addr &addr,
                sp_head *s)
   :my_var_sp(name, addr, &type_handler_row, s)
  { }
  bool check_assignability(THD *thd, const List<Item> &select_list,
                           bool *assign_as_row) const override
  {
    Item_field *item= get_rcontext(thd->spcont)->get_variable(offset());
    const Field_row *field= dynamic_cast<const Field_row*>(item->field);
    DBUG_ASSERT(field);
    *assign_as_row= true;
    return !field ||
           select_list.elements != field->virtual_tmp_table()->s->fields;
  }
  bool set_row(THD *thd, List<Item> &select_list) override
  {
    return get_rcontext(thd->spcont)->set_variable_row(thd, offset(),
                                                       select_list);
  }
};


/*
  This class handles fields of a ROW SP variable when it's used as a OUT
  parameter in a stored procedure.
*/
class my_var_sp_row_field: public my_var_sp
{
  uint m_field_offset;
public:
  my_var_sp_row_field(const Lex_ident_sys_st &varname,
                      const sp_rcontext_addr &varaddr,
                      uint field_idx, sp_head *s)
   :my_var_sp(varname, varaddr,
              &type_handler_double/*Not really used*/, s),
    m_field_offset(field_idx)
  { }
  bool check_assignability(THD *thd, const List<Item> &select_list,
                           bool *assign_as_row) const override
  {
    *assign_as_row= false;
    return select_list.elements == 1;
  }
  bool set(THD *thd, Item *item) override
  {
    return get_rcontext(thd->spcont)->
             set_variable_row_field(thd, offset(), m_field_offset, &item);
  }
};


my_var *Type_handler_row::make_outvar(THD *thd,
                                      const Lex_ident_sys_st &name,
                                      const sp_rcontext_addr &addr,
                                      sp_head *sphead,
                                      bool validate_only) const
{
  if (validate_only) // e.g. EXPLAIN SELECT .. INTO spvar_row;
    return nullptr;
  return new (thd->mem_root) my_var_sp_row(name, addr, sphead);
}


my_var *Type_handler_row::make_outvar_field(THD *thd,
                                            const Lex_ident_sys_st &name,
                                            const sp_rcontext_addr &addr,
                                            const Lex_ident_sys_st &field,
                                            sp_head *sphead,
                                            bool validate_only) const
{
  const Sp_rcontext_handler *rh;
  sp_variable *t= thd->lex->find_variable(&name, &rh);
  DBUG_ASSERT(t);
  DBUG_ASSERT(t->type_handler() == this);

  uint row_field_offset;
  if (!t->find_row_field(&name, &field, &row_field_offset))
  {
    DBUG_ASSERT(0);
    my_error(ER_ROW_VARIABLE_DOES_NOT_HAVE_FIELD, MYF(0), name.str, field.str);
    return NULL;
  }
  if (validate_only) // e.g. EXPLAIN SELECT .. INTO spvar_row.field;
    return nullptr;
  return new (thd->mem_root) my_var_sp_row_field(name, addr, row_field_offset,
                                                 sphead);
}


Item_field *Field_row::make_item_field_spvar(THD *thd,
                                             const Spvar_definition &def)
{
  Item_field_row *item= new (thd->mem_root) Item_field_row(thd, this);
  if (!item)
    return nullptr;

  if (row_create_fields(thd, def))
    return nullptr;

  // virtual_tmp_table() returns nullptr in case of ROW TYPE OF cursor
  if (virtual_tmp_table() &&
      item->add_array_of_item_field(thd, *virtual_tmp_table()))
    return nullptr;

  return item;
}


bool
Type_handler_row::
  sp_variable_declarations_finalize(THD *thd, LEX *lex, int nvars,
                                    const Column_definition &cdef) const
{
  const sp_type_def_record *rec= static_cast<const sp_type_def_record*>
                                     (cdef.get_attr_const_void_ptr(0));
  DBUG_ASSERT(!rec || rec->field);
  if (!rec || !rec->field)
  {
    // A variable with an explicit ROW data type
    return Type_handler::sp_variable_declarations_finalize(thd, lex,
                                                           nvars, cdef);
  }

  // TYPE row_t IS RECORD
  Row_definition_list *row= rec->field->deep_copy(thd);
  return row == nullptr ||
         Type_handler_row::sp_variable_declarations_row_finalize(thd,
                                                                 lex,
                                                                 nvars,
                                                                 row);
}


Item *Type_handler_row::make_typedef_constructor_item(THD *thd,
                                                      const sp_type_def &def,
                                                      List<Item> *args) const
{
  if (unlikely(args == nullptr))
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), def.get_name().str);
    return nullptr;
  }

  return new (thd->mem_root) Item_row(thd, *args);
}
