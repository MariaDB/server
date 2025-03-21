/*
   Copyright (c) 2024, MariaDB

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

#include "sql_type_vector.h"
#include "sql_class.h"
#include "sql_lex.h"

Named_type_handler<Type_handler_vector> type_handler_vector("vector");
Type_collection_vector type_collection_vector;

const Type_collection *Type_handler_vector::type_collection() const
{
  return &type_collection_vector;
}

const Type_handler *Type_collection_vector::aggregate_for_comparison(
                       const Type_handler *a, const Type_handler *b) const
{
  if (a->type_collection() == this)
    swap_variables(const Type_handler *, a, b);
  if (a == &type_handler_vector      || a == &type_handler_hex_hybrid ||
      a == &type_handler_tiny_blob   || a == &type_handler_blob       ||
      a == &type_handler_medium_blob || a == &type_handler_long_blob  ||
      a == &type_handler_varchar     || a == &type_handler_string     ||
      a == &type_handler_null)
    return b;
  return NULL;
}

const Type_handler *Type_collection_vector::aggregate_for_result(
                       const Type_handler *a, const Type_handler *b) const
{
  return aggregate_for_comparison(a,b);
}

const Type_handler *Type_collection_vector::aggregate_for_min_max(
                       const Type_handler *a, const Type_handler *b) const
{
  return aggregate_for_comparison(a,b);
}

const Type_handler *Type_collection_vector::aggregate_for_num_op(
                      const Type_handler *a, const Type_handler *b) const
{
  return NULL;
}

const Type_handler *Type_handler_vector::type_handler_for_comparison() const
{
  return &type_handler_vector;
}

Field *Type_handler_vector::make_conversion_table_field(
      MEM_ROOT *root, TABLE *table, uint metadata, const Field *target) const
{
  return new (root) Field_vector(NULL, (uchar *) "", 1, Field::NONE,
                                 &empty_clex_str, table->s, metadata);
}

bool Type_handler_vector::Column_definition_fix_attributes(
       Column_definition *def) const
{
  if (def->length == 0 || def->charset != &my_charset_bin)
  {
    my_error(ER_WRONG_FIELD_SPEC, MYF(0), def->field_name.str);
    return true;
  }
  def->length*= sizeof(float);
  return false;
}

bool Type_handler_vector::Key_part_spec_init_vector(Key_part_spec *part,
       const Column_definition &def) const
{
  if (part->length)
  {
    my_error(ER_WRONG_SUB_KEY, MYF(0));
    return true;
  }
  return false;
}

Item *Type_handler_vector::create_typecast_item(THD *thd, Item *item,
        const Type_cast_attributes &attr) const
{
  //return new (thd->mem_root) Item_typecast_vector(thd, item);
  return NULL;
}

Field *Type_handler_vector::make_table_field(MEM_ROOT *root,
         const LEX_CSTRING *name, const Record_addr &addr,
         const Type_all_attributes &attr, TABLE_SHARE *share) const
{
  return new (root) Field_vector(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                                 Field::NONE, name, share, attr.max_length);
}

bool Type_handler_vector::Item_hybrid_func_fix_attributes(THD *thd,
       const LEX_CSTRING &func_name, Type_handler_hybrid_field_type *handler,
       Type_all_attributes *func, Item **items, uint nitems) const
{
  if (func->aggregate_attributes_string(func_name, items, nitems))
    return true;
  //func->set_type_maybe_null(true);
  return false;
}

bool Type_handler_vector::Item_sum_sum_fix_length_and_dec(
       Item_sum_sum *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("sum") };
  return Item_func_or_sum_illegal_param(name);
}

bool Type_handler_vector::Item_sum_avg_fix_length_and_dec(
       Item_sum_avg *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("avg") };
  return Item_func_or_sum_illegal_param(name);
}

bool Type_handler_vector::Item_sum_variance_fix_length_and_dec(
       Item_sum_variance *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_func_round_fix_length_and_dec(
       Item_func_round *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_func_int_val_fix_length_and_dec(
       Item_func_int_val *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_func_signed_fix_length_and_dec(
       Item_func_signed *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_func_unsigned_fix_length_and_dec(
       Item_func_unsigned *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_double_typecast_fix_length_and_dec(
       Item_double_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_float_typecast_fix_length_and_dec(
       Item_float_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_decimal_typecast_fix_length_and_dec(
       Item_decimal_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_time_typecast_fix_length_and_dec(
       Item_time_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_date_typecast_fix_length_and_dec(
       Item_date_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}

bool Type_handler_vector::Item_datetime_typecast_fix_length_and_dec(
       Item_datetime_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);

}

bool Type_handler_vector::Item_char_typecast_fix_length_and_dec(
       Item_char_typecast *item) const
{
  if (item->cast_charset() != &my_charset_bin) // XXX todo
    return Item_func_or_sum_illegal_param(item); // CAST(vector AS CHAR)
  item->fix_length_and_dec_str();
  return false; // CAST(vector AS BINARY)
}

Field *Type_handler_vector::make_table_field_from_def(TABLE_SHARE *share,
         MEM_ROOT *root, const LEX_CSTRING *name, const Record_addr &rec,
         const Bit_addr &bit, const Column_definition_attributes *attr,
         uint32 flags) const
{
  return new (root) Field_vector(rec.ptr(), rec.null_ptr(), rec.null_bit(),
            attr->unireg_check, name, share, static_cast<uint>(attr->length));
}

/*****************************************************************/
void Field_vector::sql_type(String &res) const
{
  res.set_ascii(STRING_WITH_LEN("vector"));
  res.append_parenthesized(field_length/sizeof(float));
}

int Field_vector::reset()
{
  if (m_embedding_generator_name)
  {
    my_free(m_embedding_generator_name);
    m_embedding_generator_name = nullptr;
  }
  
  if (m_embedding_source_field_name)
  {
    my_free(m_embedding_source_field_name);
    m_embedding_source_field_name = nullptr;
  }

  int res= Field_varstring::reset();
  store_length(field_length);
  return res;
}

static void do_copy_vec(const Copy_field *copy)
{
  uint from_length_bytes= static_cast<Field_vector*>(copy->from_field)->length_bytes;
  uint to_length_bytes=  static_cast<Field_vector*>(copy->to_field)->length_bytes;
  uint from_length= copy->from_length - from_length_bytes;
  uint to_length= copy->to_length - to_length_bytes;
  uchar *from= copy->from_ptr + from_length_bytes;
  uchar *to= copy->to_ptr + to_length_bytes;

  if (to_length_bytes == 1)
    *copy->to_ptr= to_length;
  else
    int2store(copy->to_ptr, to_length);

  if (from_length > to_length)
    memcpy(to, from, to_length);
  else
  {
    memcpy(to, from, from_length);
    bzero(to + from_length, to_length - from_length);
  }
}

Field::Copy_func *Field_vector::get_copy_func(const Field *from) const
{
  if (from->type_handler() != &type_handler_vector)
    return do_field_string;
  if (field_length == from->field_length &&
      length_bytes == static_cast<const Field_vector*>(from)->length_bytes)
    return do_field_eq;
  return do_copy_vec;
}

int Field_vector::report_wrong_value(const ErrConv &val)
{
  get_thd()->push_warning_truncated_value_for_field(
    Sql_condition::WARN_LEVEL_WARN, "vector", val.ptr(),
    table->s->db.str, table->s->table_name.str, field_name.str);
  reset();
  return 1;
}

int Field_vector::store(double nr)
{
  return report_wrong_value(ErrConvDouble(nr));
}

int Field_vector::store(longlong nr, bool unsigned_val)
{
  return report_wrong_value(ErrConvInteger(Longlong_hybrid(nr, unsigned_val)));
}

int Field_vector::store_decimal(const my_decimal *nr)
{
  return report_wrong_value(ErrConvDecimal(nr));
}

/* The method for storing data in a vector field */
int Field_vector::store(const char *from, size_t length, CHARSET_INFO *cs)
{
  if (table->in_use->count_cuted_fields != CHECK_FIELD_IGNORE)
  {
    if (cs != &my_charset_bin) // XXX todo
      return report_wrong_value(ErrConvString(from, length, cs));

    if (length != field_length)
      return report_wrong_value(ErrConvString(from, length, cs));

    if (!Type_handler_vector::is_valid(from, length))
      return report_wrong_value(ErrConvString(from, length, cs));
  }

  return Field_varstring::store(from, length, cs);
}

Field_vector* Field_vector::embedding_source_field() const
{
  if (!m_embedding_source_field_name)
    return nullptr;
    
  for (Field **field = table->field; *field; field++)
  {
    if (strcmp((*field)->field_name, m_embedding_source_field_name) == 0)
      return *field;
  }
  
  return nullptr;
}

void Field_vector::set_embedding_generator(const char *name)
{
  if (name)
    m_embedding_generator_name = my_strdup(PSI_NOT_INSTRUMENTED, name, MYF(MY_WME));
}

void Field_vector::set_embedding_source_field(const char *name)
{
  if (name)
    m_embedding_source_field_name = my_strdup(PSI_NOT_INSTRUMENTED, name, MYF(MY_WME));
}

void Field_vector::set_embedding_dimensions(uint dimensions)
{
  m_embedding_dimensions = dimensions;
}

enum_conv_type Field_vector::rpl_conv_type_from(const Conv_source &src,
                 const Relay_log_info *rli, const Conv_param &param) const
{
  if (src.type_handler() == &type_handler_varchar &&
      field_length == src.type_handler()->max_display_length_for_field(src))
    return rpl_conv_type_from_same_data_type(src.metadata(), rli, param);
  return CONV_TYPE_IMPOSSIBLE;
}
