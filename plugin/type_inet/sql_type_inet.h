#ifndef SQL_TYPE_INET_H
#define SQL_TYPE_INET_H
/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation
   Copyright (c) 2019 MariaDB Corporation

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


static const size_t IN_ADDR_SIZE= 4;
static const size_t IN_ADDR_MAX_CHAR_LENGTH= 15;

static const size_t IN6_ADDR_SIZE= 16;
static const size_t IN6_ADDR_NUM_WORDS= IN6_ADDR_SIZE / 2;

/**
  Non-abbreviated syntax is 8 groups, up to 4 digits each,
  plus 7 delimiters between the groups.
  Abbreviated syntax is even shorter.
*/
static const uint IN6_ADDR_MAX_CHAR_LENGTH= 8 * 4 + 7;


class NativeBufferInet6: public NativeBuffer<IN6_ADDR_SIZE+1>
{
};

class StringBufferInet6: public StringBuffer<IN6_ADDR_MAX_CHAR_LENGTH+1>
{
};

/***********************************************************************/

class Inet4
{
  char m_buffer[IN_ADDR_SIZE];
protected:
  bool ascii_to_ipv4(const char *str, size_t length);
  bool character_string_to_ipv4(const char *str, size_t str_length,
                                CHARSET_INFO *cs)
  {
    if (cs->state & MY_CS_NONASCII)
    {
      char tmp[IN_ADDR_MAX_CHAR_LENGTH];
      String_copier copier;
      uint length= copier.well_formed_copy(&my_charset_latin1, tmp, sizeof(tmp),
                                           cs, str, str_length);
      return ascii_to_ipv4(tmp, length);
    }
    return ascii_to_ipv4(str, str_length);
  }
  bool binary_to_ipv4(const char *str, size_t length)
  {
    if (length != sizeof(m_buffer))
      return true;
    memcpy(m_buffer, str, length);
    return false;
  }

  Inet4() = default;
public:
  void to_binary(char *dst, size_t dstsize) const
  {
    DBUG_ASSERT(dstsize >= sizeof(m_buffer));
    memcpy(dst, m_buffer, sizeof(m_buffer));
  }
  bool to_binary(String *to) const
  {
    return to->copy(m_buffer, sizeof(m_buffer), &my_charset_bin);
  }
  size_t to_string(char *dst, size_t dstsize) const;
  bool to_string(String *to) const
  {
    to->set_charset(&my_charset_latin1);
    if (to->alloc(INET_ADDRSTRLEN))
      return true;
    to->length((uint32) to_string((char*) to->ptr(), INET_ADDRSTRLEN));
    return false;
  }
};


class Inet4_null: public Inet4, public Null_flag
{
public:
  // Initialize from a text representation
  Inet4_null(const char *str, size_t length, CHARSET_INFO *cs)
   :Null_flag(character_string_to_ipv4(str, length, cs))
  { }
  Inet4_null(const String &str)
   :Inet4_null(str.ptr(), str.length(), str.charset())
  { }
  // Initialize from a binary representation
  Inet4_null(const char *str, size_t length)
   :Null_flag(binary_to_ipv4(str, length))
  { }
  Inet4_null(const Binary_string &str)
   :Inet4_null(str.ptr(), str.length())
  { }
public:
  const Inet4& to_inet4() const
  {
    DBUG_ASSERT(!is_null());
    return *this;
  }
  void to_binary(char *dst, size_t dstsize) const
  {
    to_inet4().to_binary(dst, dstsize);
  }
  bool to_binary(String *to) const
  {
    return to_inet4().to_binary(to);
  }
  size_t to_string(char *dst, size_t dstsize) const
  {
    return to_inet4().to_string(dst, dstsize);
  }
  bool to_string(String *to) const
  {
    return to_inet4().to_string(to);
  }
};


class Inet6
{
protected:
  char m_buffer[IN6_ADDR_SIZE];
  bool make_from_item(Item *item, bool warn);
  bool ascii_to_ipv6(const char *str, size_t str_length);
  bool character_string_to_ipv6(const char *str, size_t str_length,
                                CHARSET_INFO *cs)
  {
    if (cs->state & MY_CS_NONASCII)
    {
      char tmp[IN6_ADDR_MAX_CHAR_LENGTH];
      String_copier copier;
      uint length= copier.well_formed_copy(&my_charset_latin1, tmp, sizeof(tmp),
                                           cs, str, str_length);
      return ascii_to_ipv6(tmp, length);
    }
    return ascii_to_ipv6(str, str_length);
  }
  bool make_from_character_or_binary_string(const String *str, bool warn);
  bool binary_to_ipv6(const char *str, size_t length)
  {
    if (length != sizeof(m_buffer))
      return true;
    memcpy(m_buffer, str, length);
    return false;
  }

  Inet6() = default;

public:
  static uint binary_length() { return IN6_ADDR_SIZE; }
  /**
    Non-abbreviated syntax is 8 groups, up to 4 digits each,
    plus 7 delimiters between the groups.
    Abbreviated syntax is even shorter.
  */
  static uint max_char_length() { return IN6_ADDR_MAX_CHAR_LENGTH; }

  static bool only_zero_bytes(const char *ptr, uint length)
  {
    for (uint i= 0 ; i < length; i++)
    {
      if (ptr[i] != 0)
        return false;
    }
    return true;
  }

  /*
    Check at Item's fix_fields() time if "item" can return a nullable value
    on conversion to INET6, or conversion produces a NOT NULL INET6 value.
  */
  static bool fix_fields_maybe_null_on_conversion_to_inet6(Item *item);

public:

  Inet6(Item *item, bool *error, bool warn= true)
  {
    *error= make_from_item(item, warn);
  }
  void to_binary(char *str, size_t str_size) const
  {
    DBUG_ASSERT(str_size >= sizeof(m_buffer));
    memcpy(str, m_buffer, sizeof(m_buffer));
  }
  bool to_binary(String *to) const
  {
    return to->copy(m_buffer, sizeof(m_buffer), &my_charset_bin);
  }
  bool to_native(Native *to) const
  {
    return to->copy(m_buffer, sizeof(m_buffer));
  }
  size_t to_string(char *dst, size_t dstsize) const;
  bool to_string(String *to) const
  {
    to->set_charset(&my_charset_latin1);
    if (to->alloc(INET6_ADDRSTRLEN))
      return true;
    to->length((uint32) to_string((char*) to->ptr(), INET6_ADDRSTRLEN));
    return false;
  }
  bool is_v4compat() const
  {
    static_assert(sizeof(in6_addr) == IN6_ADDR_SIZE, "unexpected in6_addr size");
    return IN6_IS_ADDR_V4COMPAT((struct in6_addr *) m_buffer);
  }
  bool is_v4mapped() const
  {
    static_assert(sizeof(in6_addr) == IN6_ADDR_SIZE, "unexpected in6_addr size");
    return IN6_IS_ADDR_V4MAPPED((struct in6_addr *) m_buffer);
  }
  int cmp(const char *str, size_t length) const
  {
    DBUG_ASSERT(length == sizeof(m_buffer));
    return memcmp(m_buffer, str, length);
  }
  int cmp(const Binary_string &other) const
  {
    return cmp(other.ptr(), other.length());
  }
  int cmp(const Inet6 &other) const
  {
    return memcmp(m_buffer, other.m_buffer, sizeof(m_buffer));
  }
};


class Inet6_zero: public Inet6
{
public:
  Inet6_zero()
  {
    bzero(&m_buffer, sizeof(m_buffer));
  }
};


class Inet6_null: public Inet6, public Null_flag
{
public:
  // Initialize from a text representation
  Inet6_null(const char *str, size_t length, CHARSET_INFO *cs)
   :Null_flag(character_string_to_ipv6(str, length, cs))
  { }
  Inet6_null(const String &str)
   :Inet6_null(str.ptr(), str.length(), str.charset())
  { }
  // Initialize from a binary representation
  Inet6_null(const char *str, size_t length)
   :Null_flag(binary_to_ipv6(str, length))
  { }
  Inet6_null(const Binary_string &str)
   :Inet6_null(str.ptr(), str.length())
  { }
  // Initialize from an Item
  Inet6_null(Item *item, bool warn= true)
   :Null_flag(make_from_item(item, warn))
  { }
public:
  const Inet6& to_inet6() const
  {
    DBUG_ASSERT(!is_null());
    return *this;
  }
  void to_binary(char *str, size_t str_size) const
  {
    to_inet6().to_binary(str, str_size);
  }
  bool to_binary(String *to) const
  {
    return to_inet6().to_binary(to);
  }
  size_t to_string(char *dst, size_t dstsize) const
  {
    return to_inet6().to_string(dst, dstsize);
  }
  bool to_string(String *to) const
  {
    return to_inet6().to_string(to);
  }
  bool is_v4compat() const
  {
    return to_inet6().is_v4compat();
  }
  bool is_v4mapped() const
  {
    return to_inet6().is_v4mapped();
  }
};


class Type_std_attributes_inet6: public Type_std_attributes
{
public:
  Type_std_attributes_inet6()
   :Type_std_attributes(
      Type_numeric_attributes(Inet6::max_char_length(), 0, true),
      DTCollation_numeric())
  { }
};


class Type_handler_inet6: public Type_handler
{
  bool character_or_binary_string_to_native(THD *thd, const String *str,
                                            Native *to) const;
public:
  ~Type_handler_inet6() override {}

  const Type_collection *type_collection() const override;
  const Name &default_value() const override
  {
    static Name def(STRING_WITH_LEN("::"));
    return def;
  }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_STRING;
  }
  bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                      const Item *item) const override
  {
    return to->set_data_type_name(name().lex_cstring());
  }

  enum_field_types field_type() const override
  {
    return MYSQL_TYPE_STRING;
  }

  Item_result result_type() const override
  {
    return STRING_RESULT;
  }

  Item_result cmp_type() const override
  {
    return STRING_RESULT;
  }

  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_STRING;
  }

  uint32 max_display_length_for_field(const Conv_source &src) const override
  {
    return Inet6::max_char_length();
  }

  const Type_handler *type_handler_for_comparison() const override
  {
    return this;
  }

  int
  stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const override
  {
    DBUG_ASSERT(field->type_handler() == this);
    Inet6_null ni(item); // Convert Item to INET6
    if (ni.is_null())
      return 0;
    NativeBufferInet6 tmp;
    if (field->val_native(&tmp))
    {
      DBUG_ASSERT(0);
      return 0;
    }
    return -ni.cmp(tmp);
  }
  CHARSET_INFO *charset_for_protocol(const Item *item) const override
  {
    return item->collation.collation;
  }

  bool is_scalar_type() const override { return true; }
  bool is_val_native_ready() const override { return true; }
  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_str() const override { return true; }
  bool can_return_text() const override { return true; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
  bool convert_to_binary_using_val_native() const override { return true; }

  uint Item_time_precision(THD *thd, Item *item) const override
  {
    return 0;
  }
  uint Item_datetime_precision(THD *thd, Item *item) const override
  {
    return 0;
  }
  uint Item_decimal_scale(const Item *item) const override
  {
    return 0;
  }
  uint Item_decimal_precision(const Item *item) const override
  {
    /*
      This will be needed if we ever allow cast from INET6 to DECIMAL.
      Decimal precision of INET6 is 39 digits:
      'ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff' =
       340282366920938463463374607431768211456  = 39 digits
    */
    return 39;
  }

  /*
    Returns how many digits a divisor adds into a division result.
    See Item::divisor_precision_increment() in item.h for more comments.
  */
  uint Item_divisor_precision_increment(const Item *) const override
  {
    return 0;
  }
  /**
    Makes a temporary table Field to handle numeric aggregate functions,
    e.g. SUM(DISTINCT expr), AVG(DISTINCT expr), etc.
  */
  Field *make_num_distinct_aggregator_field(MEM_ROOT *,
                                            const Item *) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *TABLE,
                                     uint metadata,
                                     const Field *target) const override;
  // Fix attributes after the parser
  bool Column_definition_fix_attributes(Column_definition *c) const override
  {
    c->length= Inet6::max_char_length();
    return false;
  }

  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override
  {
    def->prepare_stage1_simple(&my_charset_numeric);
    return false;
  }

  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const override
  {
    def->redefine_stage1_common(dup, file);
    def->set_compression_method(dup->compression_method());
    def->create_length_to_internal_length_string();
    return false;
  }

  bool Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const override
  {
    def->pack_flag= FIELDFLAG_BINARY;
    return false;
  }

  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override;

  bool partition_field_append_value(String *to,
                                    Item *item_expr,
                                    CHARSET_INFO *field_cs,
                                    partition_value_print_mode_t mode)
                                    const override;

  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *table) const override;

  Field *
  make_table_field_from_def(TABLE_SHARE *share,
                            MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &addr,
                            const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const override
  {
    def->frm_pack_basic(buff);
    def->frm_pack_charset(buff);
  }
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *def,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const override
  {
    def->frm_unpack_basic(buffer);
    return def->frm_unpack_charset(share, buffer);
  }
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          String *tmp)
                     const override;
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 String *tmp) const override;
  void sort_length(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const override;
  uint32 max_display_length(const Item *item) const override
  {
    return Inet6::max_char_length();
  }
  uint32 calc_pack_length(uint32 length) const override
  {
    return Inet6::binary_length();
  }
  void Item_update_null_value(Item *item) const override
  {
    NativeBufferInet6 tmp;
    item->val_native(current_thd, &tmp);
  }
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  void Item_param_setup_conversion(THD *thd, Item_param *param) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override
  {
    param->set_param_str(pos, len);
  }
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *val) const override
  {
    param->unsigned_flag= false;//QQ
    param->setup_conversion_string(thd, attr->collation.collation);
    /*
      Exact value of max_length is not known unless data is converted to
      charset of connection, so we have to set it later.
    */
    return param->set_str(val->m_string.ptr(), val->m_string.length(),
                          attr->collation.collation,
                          attr->collation.collation);
  }
  bool Item_param_val_native(THD *thd, Item_param *item, Native *to)
                             const override
  {
    StringBufferInet6 buffer;
    String *str= item->val_str(&buffer);
    if (!str)
      return true;
    Inet6_null tmp(*str);
    return tmp.is_null() || tmp.to_native(to);
  }
  bool Item_send(Item *item, Protocol *p, st_value *buf) const override
  {
    return Item_send_str(item, p, buf);
  }
  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                         const override
  {
    if (field->type_handler() == this)
    {
      NativeBuffer<MAX_FIELD_WIDTH> tmp;
      bool rc= item->val_native(current_thd, &tmp);
      if (rc || item->null_value)
        return set_field_to_null_with_conversions(field, no_conversions);
      field->set_notnull();
      return field->store_native(tmp);
    }
    return item->save_str_in_field(field, no_conversions);
  }

  String *print_item_value(THD *thd, Item *item, String *str) const override
  {
    StringBufferInet6 buf;
    String *result= item->val_str(&buf);
    /*
      TODO: This should eventually use one of these notations:
      1. CAST('::' AS INET6)
         Problem: CAST is not supported as a NAME_CONST() argument.
      2. INET6'::
         Problem: This syntax is not supported by the parser yet.
    */
    return !result ||
           str->realloc(result->length() + 2) ||
           str->append(STRING_WITH_LEN("'")) ||
           str->append(result->ptr(), result->length()) ||
           str->append(STRING_WITH_LEN("'")) ?
           NULL :
           str;
  }

  /**
    Check if
      WHERE expr=value AND expr=const
    can be rewritten as:
      WHERE const=value AND expr=const

    "this" is the comparison handler that is used by "target".

    @param target       - the predicate expr=value,
                          whose "expr" argument will be replaced to "const".
    @param target_expr  - the target's "expr" which will be replaced to "const".
    @param target_value - the target's second argument, it will remain unchanged.
    @param source       - the equality predicate expr=const (or expr<=>const)
                          that can be used to rewrite the "target" part
                          (under certain conditions, see the code).
    @param source_expr  - the source's "expr". It should be exactly equal to
                          the target's "expr" to make condition rewrite possible.
    @param source_const - the source's "const" argument, it will be inserted
                          into "target" instead of "expr".
  */
  bool
  can_change_cond_ref_to_const(Item_bool_func2 *target,
                               Item *target_expr, Item *target_value,
                               Item_bool_func2 *source,
                               Item *source_expr, Item *source_const)
                               const override
  {
    /*
      WHERE COALESCE(inet6_col)='::1' AND COALESCE(inet6_col)=CONCAT(a);  -->
      WHERE COALESCE(inet6_col)='::1' AND               '::1'=CONCAT(a);
    */
    return target->compare_type_handler() == source->compare_type_handler();
  }
  bool
  subquery_type_allows_materialization(const Item *inner,
                                       const Item *outer, bool) const override
  {
    /*
      Example:
        SELECT * FROM t1 WHERE a IN (SELECT inet6col FROM t1 GROUP BY inet6col);
      Allow materialization only if the outer column is also INET6.
      This can be changed for more relaxed rules in the future.
    */
    DBUG_ASSERT(inner->type_handler() == this);
    return outer->type_handler() == this;
  }
  /**
    Make a simple constant replacement item for a constant "src",
    so the new item can futher be used for comparison with "cmp", e.g.:
      src = cmp   ->  replacement = cmp

    "this" is the type handler that is used to compare "src" and "cmp".

    @param thd - current thread, for mem_root
    @param src - The item that we want to replace. It's a const item,
                 but it can be complex enough to calculate on every row.
    @param cmp - The src's comparand.
    @retval    - a pointer to the created replacement Item
    @retval    - NULL, if could not create a replacement (e.g. on EOM).
                 NULL is also returned for ROWs, because instead of replacing
                 a Item_row to a new Item_row, Type_handler_row just replaces
                 its elements.
  */
  Item *make_const_item_for_comparison(THD *thd,
                                       Item *src,
                                       const Item *cmp) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;

  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;

  Item_copy *create_item_copy(THD *thd, Item *item) const override;
  int cmp_native(const Native &a, const Native &b) const override
  {
    DBUG_ASSERT(a.length() == Inet6::binary_length());
    DBUG_ASSERT(b.length() == Inet6::binary_length());
    return memcmp(a.ptr(), b.ptr(), Inet6::binary_length());
  }
  bool set_comparator_func(Arg_comparator *cmp) const override
  {
    return cmp->set_cmp_func_native();
  }
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                             bool binary_cmp) const override
  {
    return false;//QQ
  }
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override
  {
    Inet6_null na(a);
    Inet6_null nb(b);
    return !na.is_null() && !nb.is_null() && !na.cmp(nb);
  }
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *h,
                                       Type_all_attributes *attr,
                                       Item **items,
                                       uint nitems) const override
  {
    attr->Type_std_attributes::operator=(Type_std_attributes_inet6());
    h->set_handler(this);
    /*
      If some of the arguments cannot be safely converted to "INET6 NOT NULL",
      then mark the entire function nullability as NULL-able.
      Otherwise, keep the generic nullability calculated by earlier stages:
      - either by the most generic way in Item_func::fix_fields()
      - or by Item_func_xxx::fix_length_and_dec() before the call of
        Item_hybrid_func_fix_attributes()
      IFNULL() is special. It does not need to test args[0].
    */
    uint first= dynamic_cast<Item_func_ifnull*>(attr) ? 1 : 0;
    for (uint i= first; i < nitems; i++)
    {
      if (Inet6::fix_fields_maybe_null_on_conversion_to_inet6(items[i]))
      {
        attr->set_maybe_null(true);
        break;
      }
    }
    return false;
  }
  bool Item_func_min_max_fix_attributes(THD *thd,
                                        Item_func_min_max *func,
                                        Item **items,
                                        uint nitems) const override
  {
    return Item_hybrid_func_fix_attributes(thd, func->func_name(),
                                           func, func, items, nitems);

  }
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const override
  {
    func->Type_std_attributes::operator=(Type_std_attributes_inet6());
    func->set_handler(this);
    return false;
  }
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool Item_val_native_with_conversion(THD *thd, Item *item,
                                       Native *to) const override
  {
    if (item->type_handler() == this)
      return item->val_native(thd, to); // No conversion needed
    StringBufferInet6 buffer;
    String *str= item->val_str(&buffer);
    return str ? character_or_binary_string_to_native(thd, str, to) : true;
  }
  bool Item_val_native_with_conversion_result(THD *thd, Item *item,
                                              Native *to) const override
  {
    if (item->type_handler() == this)
      return item->val_native_result(thd, to); // No conversion needed
    StringBufferInet6 buffer;
    String *str= item->str_result(&buffer);
    return str ? character_or_binary_string_to_native(thd, str, to) : true;
  }

  bool Item_val_bool(Item *item) const override
  {
    NativeBufferInet6 tmp;
    if (item->val_native(current_thd, &tmp))
      return false;
    return !Inet6::only_zero_bytes(tmp.ptr(), tmp.length());
  }
  void Item_get_date(THD *thd, Item *item,
                     Temporal::Warn *buff, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const override
  {
    set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
  }

  longlong Item_val_int_signed_typecast(Item *item) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }

  longlong Item_val_int_unsigned_typecast(Item *item) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }

  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str)
                                      const override
  {
    NativeBufferInet6 tmp;
    if ((item->null_value= item->arguments()[0]->val_native(current_thd, &tmp)))
      return NULL;
    DBUG_ASSERT(tmp.length() == Inet6::binary_length());
    if (str->set_hex(tmp.ptr(), tmp.length()))
    {
      str->length(0);
      str->set_charset(item->collation.collation);
    }
    return str;
  }

  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *item,
                                              String *str) const override
  {
    NativeBufferInet6 native;
    if (item->val_native(current_thd, &native))
    {
      DBUG_ASSERT(item->null_value);
      return NULL;
    }
    DBUG_ASSERT(native.length() == Inet6::binary_length());
    Inet6_null tmp(native.ptr(), native.length());
    return tmp.is_null() || tmp.to_string(str) ? NULL : str;
  }
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override
  {
    return 0;
  }
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override
  {
    return 0;
  }
  my_decimal *
  Item_func_hybrid_field_type_val_decimal(Item_func_hybrid_field_type *,
                                          my_decimal *to) const override
  {
    my_decimal_set_zero(to);
    return to;
  }
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *to,
                                            date_mode_t fuzzydate)
                                            const override
  {
    set_zero_time(to, MYSQL_TIMESTAMP_TIME);
  }
  // WHERE is Item_func_min_max_val_native???
  String *Item_func_min_max_val_str(Item_func_min_max *func, String *str)
                                    const override
  {
    Inet6_null tmp(func);
    return tmp.is_null() || tmp.to_string(str) ? NULL : str;
  }
  double Item_func_min_max_val_real(Item_func_min_max *) const override
  {
    return 0;
  }
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override
  {
    return 0;
  }
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *to) const override
  {
    my_decimal_set_zero(to);
    return to;
  }
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *to, date_mode_t fuzzydate)
                                  const override
  {
    set_zero_time(to, MYSQL_TIMESTAMP_TIME);
    return false;
  }

  bool
  Item_func_between_fix_length_and_dec(Item_func_between *func) const override
  {
    return false;
  }
  longlong Item_func_between_val_int(Item_func_between *func) const override
  {
    return func->val_int_cmp_native();
  }

  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;

  in_vector *make_in_vector(THD *thd, const Item_func_in *func,
                            uint nargs) const override;

  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func)
                                                    const override
  {
    if (func->compatible_types_scalar_bisection_possible())
    {
      return func->value_list_convert_const_to_int(thd) ||
             func->fix_for_scalar_comparison_using_bisection(thd);
    }
    return
      func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                      1U << (uint) STRING_RESULT);
  }
  bool
  Item_func_round_fix_length_and_dec(Item_func_round *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }
  bool
  Item_func_int_val_fix_length_and_dec(Item_func_int_val *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool Item_func_abs_fix_length_and_dec(Item_func_abs *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool Item_func_neg_fix_length_and_dec(Item_func_neg *func) const override
  {
    return Item_func_or_sum_illegal_param(func);
  }

  bool
  Item_func_signed_fix_length_and_dec(Item_func_signed *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_double_typecast_fix_length_and_dec(Item_double_typecast *item)
                                          const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_float_typecast_fix_length_and_dec(Item_float_typecast *item)
                                         const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item)
                                           const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_char_typecast_fix_length_and_dec(Item_char_typecast *item)
                                        const override;
  bool
  Item_time_typecast_fix_length_and_dec(Item_time_typecast *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_date_typecast_fix_length_and_dec(Item_date_typecast *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item)
                                            const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_func_plus_fix_length_and_dec(Item_func_plus *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_func_minus_fix_length_and_dec(Item_func_minus *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_func_mul_fix_length_and_dec(Item_func_mul *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_func_div_fix_length_and_dec(Item_func_div *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
  bool
  Item_func_mod_fix_length_and_dec(Item_func_mod *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }
};


extern MYSQL_PLUGIN_IMPORT Type_handler_inet6  type_handler_inet6;


#endif /* SQL_TYPE_INET_H */
