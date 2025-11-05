#ifndef SQL_TYPE_XMLTYPE_INCLUDED
#define SQL_TYPE_XMLTYPE_INCLUDED
/*
   Copyright (c) 2025 MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include "sql_type.h"

class Type_handler_xmltype: public Type_handler_long_blob
{
public:
  virtual ~Type_handler_xmltype() {}
  const Type_collection *type_collection() const override;
  uint get_column_attributes() const override { return ATTR_CHARSET; }
  const Type_handler *type_handler_for_comparison() const override;
  virtual Item *create_typecast_item(THD *thd, Item *item,
                  const Type_cast_attributes &attr) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  Log_event_data_type user_var_log_event_data_type(uint charset_nr)
                                                              const override
  {
    return Log_event_data_type(name().lex_cstring(), result_type(),
                               charset_nr, false);
  }

  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        column_definition_type_t type,
                                        const Column_derived_attributes
                                        *derived_attr) const override;

  Field *make_table_field(MEM_ROOT *root, const LEX_CSTRING *name,
           const Record_addr &addr, const Type_all_attributes &attr,
           TABLE_SHARE *share) const override;

  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
           const LEX_CSTRING *name, const Record_addr &addr,
           const Bit_addr &bit, const Column_definition_attributes *attr,
           uint32 flags) const override;

  Item *make_constructor_item(THD *thd, List<Item> *args) const override;

  bool Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &func_name,
         Type_handler_hybrid_field_type *handler, Type_all_attributes *func,
         Item **items, uint nitems) const override;

  const Type_handler *type_handler_for_tmp_table(const Item *item) const
    override;

  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }

};

extern Type_handler_xmltype type_handler_xmltype;

class Type_collection_xmltype: public Type_collection
{
public:
  const Type_handler *aggregate_for_result(
          const Type_handler *a, const Type_handler *b) const override;
  const Type_handler *aggregate_for_comparison(
          const Type_handler *a, const Type_handler *b) const override;
  const Type_handler *aggregate_for_min_max(
          const Type_handler *a, const Type_handler *b) const override;
  const Type_handler *aggregate_for_num_op(
          const Type_handler *a, const Type_handler *b) const override;
};

#include "field.h"

class Field_xmltype :public Field_blob
{
  int  report_wrong_value(const ErrConv &val);
public:
  Field_xmltype(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
                TABLE_SHARE *share, const DTCollation &collation)
     :Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                 field_name_arg, share, 4 /*blob_pack_length*/, collation)
  {}
  const Type_handler *type_handler() const override
  { return &type_handler_xmltype; }
  void sql_type(String &str) const override;
  uint size_of() const  override { return sizeof(*this); }
};


class Item_xmltype_typecast :public Item_char_typecast
{
public:
  Item_xmltype_typecast(THD *thd, Item *a, CHARSET_INFO *cs_arg):
    Item_char_typecast(thd, a, -1, cs_arg) {}

  const Type_handler *type_handler() const override
  { return &type_handler_xmltype; }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cast_as_xmltype")};
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_xmltype_typecast>(thd, this); }

  void print(String *str, enum_query_type query_type) override;
};

#endif // SQL_TYPE_XMLTYPE_INCLUDED
