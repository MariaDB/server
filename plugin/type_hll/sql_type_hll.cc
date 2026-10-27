/* Copyright (c) 2026, drrtuy@gmail.com
   Copyright (c) 2026, MariaDB Foundation

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

#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_type_hll.h"
#include "hll_codec.h"


Type_handler_hll type_handler_hll;
static Type_collection_hll type_collection_hll;

constexpr LEX_CSTRING Type_handler_hll::name_on_client;


static void report_invalid_hll(const char *from, size_t length)
{
  my_error(ER_WRONG_VALUE, MYF(0), "HLL",
           ErrConvString(from, length, &my_charset_bin).ptr());
}


static bool key_not_supported()
{
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           type_handler_hll.name().ptr(), "KEY");
  return true;
}


const Type_collection *Type_handler_hll::type_collection() const
{
  return &type_collection_hll;
}


const Type_handler *Type_collection_hll::aggregate_for_result(
                       const Type_handler *a, const Type_handler *b) const
{
  /* Byte strings may be assigned; Field_hll::store() validates the value */
  static const Type_aggregator::Pair agg[]=
  {
    {&type_handler_hll, &type_handler_hll,         &type_handler_hll},
    {&type_handler_hll, &type_handler_null,        &type_handler_hll},
    {&type_handler_hll, &type_handler_varchar,     &type_handler_hll},
    {&type_handler_hll, &type_handler_string,      &type_handler_hll},
    {&type_handler_hll, &type_handler_tiny_blob,   &type_handler_hll},
    {&type_handler_hll, &type_handler_blob,        &type_handler_hll},
    {&type_handler_hll, &type_handler_medium_blob, &type_handler_hll},
    {&type_handler_hll, &type_handler_long_blob,   &type_handler_hll},
    {&type_handler_hll, &type_handler_hex_hybrid,  &type_handler_hll},
    {NULL,NULL,NULL}
  };
  return Type_aggregator::find_handler_in_array(agg, a, b, true);
}


Field *Type_handler_hll::make_conversion_table_field(
      MEM_ROOT *root, TABLE *table, uint metadata, const Field *target) const
{
  uint pack_length= metadata & 0x00ff;
  if (pack_length != 4)
    return NULL; // Broken binary log?
  return new (root)
    Field_hll(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str, table->s);
}


Item *Type_handler_hll::create_typecast_item(THD *thd, Item *item,
        const Type_cast_attributes &attr) const
{
  return new (thd->mem_root) Item_hll_typecast(thd, item);
}


bool Type_handler_hll::Column_definition_prepare_stage1(THD *thd,
  MEM_ROOT *mem_root, Column_definition *def, column_definition_type_t type,
  const Column_derived_attributes *derived_attr) const
{
  def->charset= &my_charset_bin;
  def->create_length_to_internal_length_string();
  return def->prepare_blob_field(thd);
}


bool Type_handler_hll::Key_part_spec_init_primary(Key_part_spec *,
       const Column_definition &, const handler *) const
{
  return key_not_supported();
}


bool Type_handler_hll::Key_part_spec_init_unique(Key_part_spec *,
       const Column_definition &, const handler *, bool *) const
{
  return key_not_supported();
}


bool Type_handler_hll::Key_part_spec_init_multiple(Key_part_spec *,
       const Column_definition &, const handler *) const
{
  return key_not_supported();
}


bool Type_handler_hll::Key_part_spec_init_foreign(Key_part_spec *,
       const Column_definition &, const handler *) const
{
  return key_not_supported();
}


bool Type_handler_hll::Key_part_spec_init_spatial(Key_part_spec *,
       const Column_definition &) const
{
  return key_not_supported();
}


Field *Type_handler_hll::make_table_field(MEM_ROOT *root,
         const LEX_CSTRING *name, const Record_addr &addr,
         const Type_all_attributes &attr, TABLE_SHARE *share) const
{
  return new (root) Field_hll(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                              Field::NONE, name, share);
}


Field *Type_handler_hll::make_table_field_from_def(TABLE_SHARE *share,
         MEM_ROOT *root, const LEX_CSTRING *name, const Record_addr &rec,
         const Bit_addr &bit, const Column_definition_attributes *attr,
         uint32 flags) const
{
  return new (root) Field_hll(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                              attr->unireg_check, name, share);
}


bool Type_handler_hll::
  Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &func_name,
         Type_handler_hybrid_field_type *handler, Type_all_attributes *func,
         Item **items, uint nitems) const
{
  if (func->aggregate_attributes_string(func_name, items, nitems))
    return true;
  handler->set_handler(&type_handler_hll);
  return false;
}


bool Type_handler_hll::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("sum") };
  return Item_func_or_sum_illegal_param(name);
}


bool Type_handler_hll::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("avg") };
  return Item_func_or_sum_illegal_param(name);
}


bool Type_handler_hll::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_func_signed_fix_length_and_dec(Item_func_signed *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_double_typecast_fix_length_and_dec(Item_double_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_float_typecast_fix_length_and_dec(Item_float_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_time_typecast_fix_length_and_dec(Item_time_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_date_typecast_fix_length_and_dec(Item_date_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_hll::
       Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item)
                                                 const
{
  return Item_func_or_sum_illegal_param(item);
}


/*****************************************************************/
void Field_hll::sql_type(String &res) const
{
  res.set_ascii(STRING_WITH_LEN("hll"));
}


int Field_hll::reset()
{
  static uchar empty[1]= {(HLL_FORMAT_V1 << 4) | Hll_state::EMPTY};
  set_ptr(1, empty);
  return 0;
}


int Field_hll::store(const char *from, size_t length, CHARSET_INFO *cs)
{
  if (Hll_codec::decode((const uchar *) from, length, NULL))
  {
    report_invalid_hll(from, length);
    Field_blob::reset();
    return -1;
  }
  return Field_blob::store(from, length, &my_charset_bin);
}


/* Row-based replication applies rows through unpack(), not store() */
const uchar *Field_hll::unpack(uchar *to, const uchar *from,
                               const uchar *from_end, uint param_data)
{
  const uchar *end= Field_blob::unpack(to, from, from_end, param_data);
  if (end && Hll_codec::decode(get_ptr(), get_length(), NULL))
    return NULL;
  return end;
}


enum_conv_type
Field_hll::rpl_conv_type_from(const Conv_source &source,
                              const Relay_log_info *rli,
                              const Conv_param &param) const
{
  /*
    The binary log does not carry the HLL type name: an HLL column is
    logged as a 4-byte-length BLOB. unpack() validates the bytes.
  */
  return source.type_handler() == &type_handler_long_blob ?
         rpl_conv_type_from_same_data_type(source.metadata(), rli, param) :
         CONV_TYPE_IMPOSSIBLE;
}


class Item_hll_typecast_func_handler: public Item_handled_func::Handler_str
{
public:
  const Type_handler *
    return_type_handler(const Item_handled_func *item) const override
  { return &type_handler_hll; }

  const Type_handler *
    type_handler_for_create_select(const Item_handled_func *item) const override
  { return &type_handler_hll; }

  bool fix_length_and_dec(Item_handled_func *item) const override
  {
    return false;
  }
  String *val_str(Item_handled_func *item, String *to) const override
  {
    DBUG_ASSERT(dynamic_cast<const Item_hll_typecast*>(item));
    return static_cast<Item_hll_typecast*>(item)->val_str(to);
  }
};


static Item_hll_typecast_func_handler item_hll_typecast_func_handler;


bool Item_hll_typecast::fix_length_and_dec(THD *thd)
{
  Item_char_typecast::fix_length_and_dec_str();
  set_func_handler(&item_hll_typecast_func_handler);
  set_maybe_null();                   // NULL on malformed input
  return false;
}


String *Item_hll_typecast::val_str(String *to)
{
  String *res= val_str_generic(to);
  if (!res)
    return NULL;
  if (Hll_codec::decode((const uchar *) res->ptr(), res->length(), NULL))
  {
    report_invalid_hll(res->ptr(), res->length());
    null_value= true;
    return NULL;
  }
  return res;
}


void Item_hll_typecast::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as hll)"));
}
