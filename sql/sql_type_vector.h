#ifndef SQL_TYPE_VECTOR_INCLUDED
#define SQL_TYPE_VECTOR_INCLUDED
/*
   Copyright (c) 2024 MariaDB

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

class Type_handler_vector: public Type_handler_varchar
{
public:
  virtual ~Type_handler_vector() {}
  const Type_collection *type_collection() const override;
  uint get_column_attributes() const override
  {
    return ATTR_LENGTH | ATTR_DEC;
  }
  const Type_handler *type_handler_for_comparison() const override;
  virtual Item *create_typecast_item(THD *thd, Item *item,
                  const Type_cast_attributes &attr) const override;
  bool type_can_have_key_part() const override { return false; }
  bool subquery_type_allows_materialization(const Item *, const Item *, bool)
    const override
  {
    return false; // XXX for simplicity
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  Log_event_data_type user_var_log_event_data_type(uint charset_nr)
                                                              const override
  {
    return Log_event_data_type(name().lex_cstring(), result_type(),
                               charset_nr, false);
  }

  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Key_part_spec_init_vector(Key_part_spec *part,
                                 const Column_definition &def) const override;
  Field *make_table_field(MEM_ROOT *root, const LEX_CSTRING *name,
           const Record_addr &addr, const Type_all_attributes &attr,
           TABLE_SHARE *share) const override;

  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
           const LEX_CSTRING *name, const Record_addr &addr,
           const Bit_addr &bit, const Column_definition_attributes *attr,
           uint32 flags) const override;

  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_text() const override { return false; } // XXX
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &name,
         Type_handler_hybrid_field_type *h, Type_all_attributes *attr,
         Item **items, uint nitems) const override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override;
  bool Item_func_signed_fix_length_and_dec(Item_func_signed *) const override;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const
         override;
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *) const
         override;
  bool Item_float_typecast_fix_length_and_dec(Item_float_typecast *) const
         override;
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *) const
         override;
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const
         override;
  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *) const
         override;
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *) const
         override;
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *) const
         override;

  static bool is_valid(const char *from, size_t length)
  {
    float abs2= 0.0f;
    for (const char *v= from, *end= from+length; v < end; v+= sizeof(float))
    {
      float val= get_float(v);
      abs2+= val*val;
    }
    return std::isfinite(abs2);
  }
};

extern Named_type_handler<Type_handler_vector> type_handler_vector;

class Type_collection_vector: public Type_collection
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

class Field_vector:public Field_varstring
{
  int  report_wrong_value(const ErrConv &val);
public:
  Field_vector(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
              enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
              TABLE_SHARE *share, uint len_arg)
     : Field_varstring(ptr_arg, len_arg, len_arg < 256 ? 1 :2, null_ptr_arg,
                       null_bit_arg, unireg_check_arg, field_name_arg, share,
                       &my_charset_bin) { }
  const Type_handler *type_handler() const override
  { return &type_handler_vector; }
  void sql_type(String &str) const override;
  int reset() override;
  Copy_func *get_copy_func(const Field *from) const override;
  int  store(const char *to, size_t length, CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_decimal(const my_decimal *) override;
  enum_conv_type rpl_conv_type_from(const Conv_source &, const Relay_log_info *,
                                    const Conv_param &) const override;
  uint size_of() const  override { return sizeof(*this); }
  bool update_min(Field *, bool) override { return false; } // disable EITS
  bool update_max(Field *, bool) override { return false; } // disable EITS
};

#endif // SQL_TYPE_VECTOR_INCLUDED
