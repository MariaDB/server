#ifndef SQL_TYPE_HLL_INCLUDED
#define SQL_TYPE_HLL_INCLUDED
/* Copyright (c) 2026, MariaDB

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

class Type_handler_hll: public Type_handler_long_blob
{
public:
  static constexpr LEX_CSTRING name_on_client{STRING_WITH_LEN("hll")};

  virtual ~Type_handler_hll() {}
  const Type_collection *type_collection() const override;
  uint get_column_attributes() const override { return ATTR_NONE; }
  const Type_handler *type_handler_for_comparison() const override
  { return this; }
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;

  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        column_definition_type_t type,
                                        const Column_derived_attributes
                                        *derived_attr) const override;

  bool Key_part_spec_init_primary(Key_part_spec *part,
                                  const Column_definition &def,
                                  const handler *file) const override;
  bool Key_part_spec_init_unique(Key_part_spec *part,
                                 const Column_definition &def,
                                 const handler *file,
                                 bool *has_key_needed) const override;
  bool Key_part_spec_init_multiple(Key_part_spec *part,
                                   const Column_definition &def,
                                   const handler *file) const override;
  bool Key_part_spec_init_foreign(Key_part_spec *part,
                                  const Column_definition &def,
                                  const handler *file) const override;
  bool Key_part_spec_init_spatial(Key_part_spec *part,
                                  const Column_definition &def) const override;
  bool Key_part_spec_init_ft(Key_part_spec *part,
                             const Column_definition &def) const override
  { return true; }

  Field *make_table_field(MEM_ROOT *root, const LEX_CSTRING *name,
           const Record_addr &addr, const Type_all_attributes &attr,
           TABLE_SHARE *share) const override;

  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
           const LEX_CSTRING *name, const Record_addr &addr,
           const Bit_addr &bit, const Column_definition_attributes *attr,
           uint32 flags) const override;

  bool Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &func_name,
         Type_handler_hybrid_field_type *handler, Type_all_attributes *func,
         Item **items, uint nitems) const override;

  const Type_handler *type_handler_for_tmp_table(const Item *item,
                                                 const Tmp_field_param *param)
    const override
  { return this; }

  bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                      const Item *item) const override
  { return to->set_data_type_name(name_on_client); }

  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }

  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const override;
  bool Item_func_signed_fix_length_and_dec(Item_func_signed *) const override;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const
    override;
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *) const
    override;
  bool Item_float_typecast_fix_length_and_dec(Item_float_typecast *) const
    override;
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *) const
    override;
  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *) const
    override;
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *) const
    override;
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *) const
    override;
};

extern Type_handler_hll type_handler_hll;


class Type_collection_hll: public Type_collection
{
public:
  const Type_handler *aggregate_for_result(
          const Type_handler *a, const Type_handler *b) const override;
  const Type_handler *aggregate_for_comparison(
          const Type_handler *a, const Type_handler *b) const override
  { return NULL; }
  const Type_handler *aggregate_for_min_max(
          const Type_handler *a, const Type_handler *b) const override
  { return NULL; }
  const Type_handler *aggregate_for_num_op(
          const Type_handler *a, const Type_handler *b) const override
  { return NULL; }
};


#include "field.h"

class Field_hll :public Field_blob
{
public:
  Field_hll(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
            enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
            TABLE_SHARE *share)
     :Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                 field_name_arg, share, 4 /*blob_pack_length*/,
                 DTCollation(&my_charset_bin))
  {}
  const Type_handler *type_handler() const override
  { return &type_handler_hll; }

  void make_send_field(Send_field *to) override
  {
    Field_blob::make_send_field(to);
    to->set_data_type_name(Type_handler_hll::name_on_client);
  }

  void sql_type(String &str) const override;
  uint size_of() const  override { return sizeof(*this); }
  int  store(const char *to, size_t length, CHARSET_INFO *charset) override;
  using Field_str::store;
  const uchar *unpack(uchar *to, const uchar *from, const uchar *from_end,
                      uint param_data) override;
  /* Values coming from a non-HLL field must pass through store() */
  Copy_func *get_copy_func(const Field *from) const override
  {
    return from->type_handler() == &type_handler_hll ?
           Field_blob::get_copy_func(from) : do_conv_blob;
  }
  bool memcpy_field_possible(const Field *from) const override
  {
    return from->type_handler() == &type_handler_hll &&
           Field_blob::memcpy_field_possible(from);
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
};


class Item_hll_typecast :public Item_char_typecast
{
public:
  Item_hll_typecast(THD *thd, Item *a)
   :Item_char_typecast(thd, a, -1, &my_charset_bin) {}

  const Type_handler *type_handler() const override
  { return &type_handler_hll; }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cast_as_hll")};
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *to) override;
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_hll_typecast>(thd, this); }

  void print(String *str, enum_query_type query_type) override;
};

#endif // SQL_TYPE_HLL_INCLUDED
