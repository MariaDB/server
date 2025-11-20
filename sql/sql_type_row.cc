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


class Type_collection_row: public Type_collection
{
public:
  bool init(Type_handler_data *data) override
  {
    return false;
  }
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }
  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    /*
      Allowed combinations:
        ROW+ROW, NULL+ROW, ROW+NULL
    */
    DBUG_ASSERT(a == &type_handler_row || a == &type_handler_null);
    DBUG_ASSERT(b == &type_handler_row || b == &type_handler_null);
    DBUG_ASSERT(a == &type_handler_row || b == &type_handler_row);
    return &type_handler_row;
  }
  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    return NULL;
  }
  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }
};


static Type_collection_row type_collection_row;

const Type_collection *Type_handler_row::type_collection() const
{
  return &type_collection_row;
}


const Type_handler *Type_handler_row::type_handler_for_comparison() const
{
  return &type_handler_row;
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


bool Type_handler_row::Spvar_definition_with_complex_data_types(
                                                 Spvar_definition *def) const
{
  if (def->row_field_definitions() && def->is_row())
  {
    List_iterator<Spvar_definition> it(*(def->row_field_definitions()));
    Spvar_definition *member;
    while ((member= it++))
    {
      if (member->type_handler()->is_complex())
        return true;
    }
  }
  return false;
}


bool
Type_handler_row::
  sp_variable_declarations_finalize(THD *thd, LEX *lex, int nvars,
                                    const Column_definition &cdef) const
{
  const sp_type_def_record *rec= dynamic_cast<const sp_type_def_record*>
                                     (cdef.get_attr_const_generic_ptr(0));
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
         Type_handler_composite::sp_variable_declarations_row_finalize(thd,
                                                                       lex,
                                                                       nvars,
                                                                       row);
}


bool Type_handler_row::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_row(thd);
}

Item_cache *
Type_handler_row::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_row(thd);
}

cmp_item *Type_handler_row::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_row;
}

in_vector *Type_handler_row::make_in_vector(THD *thd,
                                            const Item_func_in *func,
                                            uint nargs) const
{
  return new (thd->mem_root) in_row(thd, nargs, 0);
}

bool Type_handler_row::Item_func_in_fix_comparator_compatible_types(THD *thd,
                                              Item_func_in *func) const
{
  return func->compatible_types_row_bisection_possible() ?
         func->fix_for_row_comparison_using_bisection(thd) :
         func->fix_for_row_comparison_using_cmp_items(thd);
}

/**
  Get a string representation of the Item value.
  See sql_type.h for details.
*/
String *Type_handler_row::
          print_item_value(THD *thd, Item *item, String *str) const
{
  CHARSET_INFO *cs= thd->variables.character_set_client;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> val(cs);
  str->append(STRING_WITH_LEN("ROW("));
  for (uint i= 0 ; i < item->cols(); i++)
  {
    if (i > 0)
      str->append(',');
    Item *elem= item->element_index(i);
    String *tmp= elem->type_handler()->print_item_value(thd, elem, &val);
    if (tmp)
      str->append(*tmp);
    else
      str->append(NULL_clex_str);
  }
  str->append(')');
  return str;
}


Item *Type_handler_row::
  make_const_item_for_comparison(THD *thd, Item *item, const Item *cmp) const
{
  if (item->type() == Item::ROW_ITEM && cmp->type() == Item::ROW_ITEM)
  {
    /*
      Substitute constants only in Item_row's. Don't affect other Items
      with ROW_RESULT (eg Item_singlerow_subselect).

      For such Items more optimal is to detect if it is constant and replace
      it with Item_row. This would optimize queries like this:
      SELECT * FROM t1 WHERE (a,b) = (SELECT a,b FROM t2 LIMIT 1);
    */
    Item_row *item_row= (Item_row*) item;
    Item_row *comp_item_row= (Item_row*) cmp;
    uint col;
    /*
      If item and comp_item are both Item_row's and have same number of cols
      then process items in Item_row one by one.
      We can't ignore NULL values here as this item may be used with <=>, in
      which case NULL's are significant.
    */
    DBUG_ASSERT(item->result_type() == cmp->result_type());
    DBUG_ASSERT(item_row->cols() == comp_item_row->cols());
    col= item_row->cols();
    while (col-- > 0)
      resolve_const_item(thd, item_row->addr(col),
                         comp_item_row->element_index(col));
  }
  return NULL;
}

Field *Type_handler_row::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->length == 0);
  DBUG_ASSERT(f_maybe_null(attr->pack_flag));
  return new (mem_root) Field_row(rec.ptr(), name);
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


bool Type_handler_row::get_item_index(THD *thd,
                                      const Item_field *item,
                                      const LEX_CSTRING& name,
                                      uint& idx) const
{
  auto item_row=
    dynamic_cast<Item_field_row *>(const_cast<Item_field *> (item));
  DBUG_ASSERT(item_row);

  auto vtable= item_row->field->virtual_tmp_table();
  if (!vtable)
    return true;

  return vtable->sp_find_field_by_name_or_error(&idx,
                                                item_row->field->field_name,
                                                name);
}


Item_field *Type_handler_row::get_item(THD *thd,
                                       const Item_field *item,
                                       const LEX_CSTRING& name) const
{
  auto item_row=
    dynamic_cast<Item_field_row *>(const_cast<Item_field *> (item));
  DBUG_ASSERT(item_row);

  uint field_idx;
  if (get_item_index(thd, item_row, name, field_idx))
    return nullptr;

  return item_row->element_index(field_idx)->field_for_view_update();
}

Named_type_handler<Type_handler_row> type_handler_row_internal("row");
const Type_handler_composite &type_handler_row=
  type_handler_row_internal;
