/*
   Copyright (c) 2025, Rakuten Securities
   Copyright (c) 2025, MariaDB plc

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
#ifndef SQL_TYPE_ROW_INCLUDED
#define SQL_TYPE_ROW_INCLUDED


#include "sql_type_composite.h"

class Row_definition_list;


class RowTypeBuffer: public CharBuffer<6 + MAX_BIGINT_WIDTH>
{
public:
  RowTypeBuffer(uint sz)
  { copy("row<"_LEX_CSTRING).append_ulonglong(sz).append_char('>'); }
};


/*
  Special handler for ROW
*/
class Type_handler_row: public Type_handler_composite
{
public:
  virtual ~Type_handler_row() = default;
  const Type_collection *type_collection() const override;
  const Type_handler *type_handler_for_comparison() const override;
  bool has_null_predicate() const override { return false; }
  bool Spvar_definition_with_complex_data_types(Spvar_definition *def)
                                                       const override;
  bool sp_variable_declarations_finalize(THD *thd,
                                         LEX *lex, int nvars,
                                         const Column_definition &def)
                                                        const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  // SELECT 1,2,3 INTO spvar_row;
  my_var *make_outvar(THD *thd,
                      const Lex_ident_sys_st &name,
                      const sp_rcontext_addr &addr,
                      sp_head *sphead,
                      bool validate_only) const override;
  // SELECT 1 INTO spvar_row.field;
  virtual my_var *make_outvar_field(THD *thd,
                                    const Lex_ident_sys_st &name,
                                    const sp_rcontext_addr &addr,
                                    const Lex_ident_sys_st &field,
                                    sp_head *sphead,
                                    bool validate_only) const override;
  String *print_item_value(THD *thd, Item *item, String *str) const override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const
    override;
  Item *make_typedef_constructor_item(THD *thd, const sp_type_def &def,
                                      List<Item> *arg_list) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *thd, const Item_func_in *f, uint nargs) const
    override;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const
    override;
  bool get_item_index(THD *thd,
                      const Item_field *item,
                      const LEX_CSTRING& name,
                      uint& idx) const override;
  Item_field *get_item(THD *thd,
                       const Item_field *item,
                       const LEX_CSTRING& name) const override;
  Item_field *get_or_create_item(THD *thd,
                                 Item_field *item,
                                 const LEX_CSTRING& name) const override
  {
    return get_item(thd, const_cast<const Item_field *>(item), name);
  }
};


#endif /* SQL_TYPE_ROW_INCLUDED */
