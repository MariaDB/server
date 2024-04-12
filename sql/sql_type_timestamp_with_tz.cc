/*
   Copyright (c) 2024, MariaDB Corporation.

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
#include "sql_type.h"
#include "sql_type_timestamp_with_tz.h"
#include "tztime.h"
#include "compat56.h"
#include "item.h"
#include "sql_class.h"


/*** Timestamp_with_tz methods ***/

Timeval
Timestamp_with_tz::make_timeval_from_native_without_tz(const Native &native,
                                                       decimal_digit_t dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  DBUG_ASSERT(native.length() >= my_timestamp_binary_length(dec));
  struct timeval tv;
  my_timestamp_from_binary(&tv, (const uchar *) native.ptr(), dec);
  return Timeval(tv);
}


/*
  TODO: to_native(): the format will change: awaiting Field_timestamp2_with_tz
*/
bool Timestamp_with_tz::to_native(Native *to, decimal_digit_t decimals) const
{
  uint len= my_timestamp_binary_length(decimals);
  if (to->reserve(len))
    return true;
  my_timestamp_to_binary(this, (uchar *) to->ptr(), decimals);
  to->length(len);
  // TODO: guard length<=0xFF
  const String *name= m_tz->get_name();
  to->append(*name);
  to->append_char((char) name->length());
  return false;
}


/*
  TODO: make_from_native_with_tz(): the format will change: awaiting Field_timestamp2_with_tz
*/

Timestamp_with_tz
Timestamp_with_tz::make_from_native_with_tz(THD *thd, const Native &native)
{
  /*
    Need at least 6 bytes:
    - 4 bytes for tv_sec + 0 bytes for tv_usec,
    - 1 byte for tz name + 1 byte for tz length
  */
  if (native.length() < 6)
    return Timestamp_with_tz(Timeval(0,0), nullptr);
  uint32 length_tz_name= (uchar) native[native.length()-1];
  uint32 length_but_tv_usec= 4 + length_tz_name + 1;
  if (!length_tz_name || length_but_tv_usec > native.length())
    return Timestamp_with_tz(Timeval(0,0), nullptr);
  uint32 length_tv_usec= native.length() - length_but_tv_usec;
  if (length_tv_usec > 3)
    return Timestamp_with_tz(Timeval(0,0), nullptr);
  uint32 length_tv= 4 + length_tv_usec;
  Timeval tv= make_timeval_from_native_without_tz(native, length_tv_usec * 2);
  String name(native.ptr() + length_tv, length_tz_name, system_charset_info);
  return Timestamp_with_tz(tv, my_tz_find(thd, &name));
}


Timestamp_with_tz_null::Timestamp_with_tz_null(THD *thd,
                                               const Datetime &dt,
                                               const Time_zone *tz)
 :Timestamp_with_tz_null()
{
  if (dt.is_valid_datetime())
  {
    uint error_code;
    tv_sec= tz->TIME_to_gmt_sec(dt.get_mysql_time(), &error_code);
    tv_usec= dt.get_mysql_time()->second_part;
    m_tz= tz;
    m_is_null= error_code > 0;
  }
}


Timestamp_with_tz_null::Timestamp_with_tz_null(THD *thd,
                                               const Native &native,
                                               const Type_handler *th,
                                               decimal_digit_t dec)
 :Timestamp_with_tz_null()
{
  const Type_handler *fmt= th->type_handler_for_native_format();
  if (fmt == &type_handler_timestamp2_with_tz)
  {
    // TIMESTAMP WITH TIME ZONE
    Timestamp_with_tz::operator=(make_from_native_with_tz(thd, native));
    m_is_null= !m_tz;
  }
  else if (fmt == &type_handler_timestamp2)
  {
    // Convert from MariaDB TIMESTAMP (WITH LOCAL TIME ZONE)
    // TODO: catch format and zero datetime errors
    Timeval::operator=(make_timeval_from_native_without_tz(native, dec));
    thd->time_zone_used= true;
    m_tz= thd->variables.time_zone;
    m_is_null= false;
  }
  else
  {
    DBUG_ASSERT(0); // Unknown source data type
  }
}


uint Timestamp_with_tz::make_sort_key(uchar *to, size_t to_size,
                                      decimal_digit_t dec)
{
  NativeBuffer<native_size_std()> native;
  to_native(&native, dec);
  uint binlen= my_timestamp_binary_length(dec);
  DBUG_ASSERT(native.length() >= binlen);
  size_t copy_length= MY_MIN(to_size, binlen);
  memcpy((char *) to, native.ptr(), copy_length);
  return copy_length;
}


String *Timestamp_with_tz::val_str(String *to, decimal_digit_t dec) const
{
  const String *name= m_tz->get_name();
  MYSQL_TIME ltime;
  get_date(&ltime);

  to->set_charset(&my_charset_numeric);
  if (to->alloc(MAX_DATE_STRING_REP_LENGTH + 1 + name->length()))
    return nullptr;
  to->length(my_datetime_to_str(&ltime, const_cast<char*>(to->ptr()), dec));
  to->append(' ');
  to->append(name->ptr(), name->length(), name->charset());
  return to;
}


bool Timestamp_with_tz::get_date(MYSQL_TIME *ltime) const
{
  /*
    Here a TIMESTAMP WITH TIME ZONE to TIMESTAMP WITHOUT TIME ZONE
    conversion happens. According to the standard, the result is
    evaluated as SV.UTC + SV.TZ, where:
    - SV is the TIMESTAMP WITH TIME ZONE value
    - SV.TZ is the time zone component of SV
    - SV.UTC is the UTC component of SV,
      i.e. its 'YYYY-MM-DD hh:mm:ss' representation in UTC.
    To get the SQL standard compliant result, we convert the time_t
    value to 'YYYY-MM-DD hh:mm:ss' using m_tz.
  */
  m_tz->gmt_sec_to_TIME(ltime, tv_sec);
  ltime->second_part= tv_usec;
  return false;
}


longlong Timestamp_with_tz::to_longlong() const
{
  MYSQL_TIME ltime;
  get_date(&ltime);
  return (longlong) TIME_to_ulonglong_datetime(&ltime);
}


my_decimal *Timestamp_with_tz::to_decimal(my_decimal *to) const
{
  MYSQL_TIME ltime;
  get_date(&ltime);
  return date2my_decimal(&ltime, to);
}


/*** Basic Type_handler methods ************************************/

const Type_handler *
Type_handler_timestamp2_with_tz::type_handler_for_comparison() const
{
  return &type_handler_timestamp2_with_tz;
}


const Type_handler *
Type_handler_timestamp2_with_tz::type_handler_for_native_format() const
{
  return &type_handler_timestamp2_with_tz;
}


const Vers_type_handler *Type_handler_timestamp2_with_tz::vers() const
{
  // TODO: vers(): awaiting Field_xxx
  return NULL;
}


Session_env_dependency::Param
Type_handler_timestamp2_with_tz::type_conversion_dependency_from(
                                   const Type_handler *from) const
{
  const Type_handler *fmt= from->type_handler_for_native_format();
  return (fmt == &type_handler_timestamp2 ||
         fmt == &type_handler_timestamp2_with_tz) ?
         Session_env_dependency::NONE :
         Session_env_dependency::SYS_VAR_TIME_ZONE_TIME_TO_GMT_SEC;
}


/*** Type metadata methods *****************************************/
const Name &Type_handler_timestamp2_with_tz::default_value() const
{
  static Name def(STRING_WITH_LEN("1970-01-01 00:00:00 +00:00"));
  return def;
}


uint32 Type_handler_timestamp2_with_tz::max_display_length(
                                          const Item *item) const
{
  return item->max_length;
}


uint Type_handler_timestamp2_with_tz::Item_decimal_precision(
                                        const Item *item) const
{
  return 14 + MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS);
}


uint32 Type_handler_timestamp2_with_tz::Item_decimal_notation_int_digits(
                                          const Item *item) const
{
  return item->decimal_int_part();
}


/*** Native value extraction methods *******************************/


Timestamp_with_tz_null
Type_handler_timestamp2_with_tz::item_value_null(THD *thd, Item *item)
{
  const Type_handler *th= item->type_handler();
  const Type_handler *fmt= th->type_handler_for_native_format();
  if (fmt == &type_handler_timestamp2_with_tz ||
      fmt == &type_handler_timestamp2)
  {
    NativeBuffer<Timestamp_with_tz::native_size_std()> native;
    item->val_native(thd, &native);
    if (item->null_value)
      return Timestamp_with_tz_null();
    return Timestamp_with_tz_null(thd, native, th,
                                  item->datetime_precision(thd));
  }

  Datetime dt(thd, item, Datetime::Options(TIME_NO_ZEROS, thd));
  const Timestamp_with_tz_null ts(thd, dt, thd->variables.time_zone);
  thd->time_zone_used|= !ts.is_null();
  return ts;
}


Timestamp_with_tz_null
Type_handler_timestamp2_with_tz::item_result_value_null(THD *thd, Item *item)
{
  const Type_handler *th= item->type_handler();
  const Type_handler *fmt= th->type_handler_for_native_format();
  if (fmt == &type_handler_timestamp2_with_tz ||
      fmt == &type_handler_timestamp2)
  {
    NativeBuffer<Timestamp_with_tz::native_size_std()> native;
    item->val_native_result(thd, &native);
    if (item->null_value)
      return Timestamp_with_tz_null();
    return Timestamp_with_tz_null(thd, native, th,
                                  item->datetime_precision(thd));
  }

  DBUG_ASSERT(th == &type_handler_null);
  return Timestamp_with_tz_null();
}


bool
Type_handler_timestamp2_with_tz::Item_val_native_with_conversion(THD *thd,
                                                                 Item *item,
                                                                 Native *to)
                                                                 const
{
  return item->null_value= item_value_null(thd, item).
                             to_native(to, item->datetime_precision(thd));
}


bool Type_handler_timestamp2_with_tz::Item_val_native_with_conversion_result(
                                        THD *thd, Item *item, Native *to) const
{
  return item->null_value= item_result_value_null(thd, item).
                             to_native(to, item->datetime_precision(thd));
}


bool Type_handler_timestamp2_with_tz::Item_save_in_value(THD *thd,
                                                         Item *item,
                                                         st_value *value) const
{
  /*
     TODO: Item_save_in_value will change. Awaiting Field_timestamp2_with_tz
     and full-featured TIMESTAMP WITH TIME ZONE support. For now loosed the
     time zone information. Example query:
       EXECUTE IMMEDIATE 'SELECT ? AS c1'
         USING TIMESTAMP_TZ('2001-01-01 10:00:00','+04:00');
       -> 2001-01-01 10:00:00
  */
  value->m_type= DYN_COL_DATETIME;
  item->get_date(thd, &value->value.m_time,
                 Datetime::Options(thd, TIME_FRAC_NONE));
  return check_null(item, value);
}


int Type_handler_timestamp2_with_tz::Item_save_in_field(Item *item,
                                                        Field *field,
                                                        bool no_conversions)
                                                        const
{
  const Type_handler *th= field->type_handler();
  const Type_handler *fmt= th->type_handler_for_native_format();
  if (fmt == &type_handler_timestamp2)
  {
    // TODO: check Timeval(0,0) to zero datetime conversion
    Timestamp_or_zero_datetime_native_null tmp(field->table->in_use, item, true);
    if (tmp.is_null())
      return set_field_to_null_with_conversions(field, no_conversions);
    return tmp.save_in_field(field, item->decimals);
  }

  if (fmt == &type_handler_timestamp2_with_tz)
  {
    /*
      TODO: Item_save_in_field - awaiting Field_xxx
       store natively in the target column is also
      of the Type_handler_timestamp2_with_tz data type.
      We don't have such Field yet though.
    */
    MY_ASSERT_UNREACHABLE();
  }

  return
    dynamic_cast<const Type_handler_string_result*>(th) ?
    item->save_str_in_field(field, no_conversions) :
    item->save_date_in_field(field, no_conversions);
}


/*** Comparison methods ********************************************/

bool Type_handler_timestamp2_with_tz::set_comparator_func(Arg_comparator *cmp)
                                                                         const
{
  return cmp->set_cmp_func_native();
}

int Type_handler_timestamp2_with_tz::cmp_native(const Native &na,
                                                const Native &nb) const
{
  THD *thd= current_thd;
  const Timestamp_with_tz
    a= Timestamp_with_tz::make_from_native_with_tz(thd, na),
    b= Timestamp_with_tz::make_from_native_with_tz(thd, nb);
  return a.cmp(b);
}

bool Type_handler_timestamp2_with_tz::Item_eq_value(THD *thd,
                                        const Type_cmp_attributes *attr,
                                        Item *a, Item *b) const
{
  // TODO: Item_eq_value: awaiting Field_timestamp2_with_tz
  MY_ASSERT_UNREACHABLE();
  return false;
}


bool Type_handler_timestamp2_with_tz::Item_const_eq(const Item_const *a,
                                                    const Item_const *b,
                                                    bool binary_cmp) const
{
  // TODO: Item_const_eq: awaiting Item_literal_timestamp2_with_tz
  MY_ASSERT_UNREACHABLE();
  return false;
}



/*** Type collection and type aggregation methods ******************/

class Type_collection_timestamp2_with_tz: public Type_collection
{
  const Type_handler *aggregate_common(const Type_handler *a,
                                       const Type_handler *b) const
  {
    if (a == b)
      return a;
    return aggregate_if_null(a, b);
  }
  const Type_handler *aggregate_if_null(const Type_handler *a,
                                        const Type_handler *b) const
  {
    return a == &type_handler_null ? b :
           b == &type_handler_null ? a :
           NULL;
  }

public:
  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override
  {
    return NULL;
  }

  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return aggregate_common(a, b);
  }
  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    return aggregate_common(a, b);
  }

  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    return aggregate_common(a, b);
  }

  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return aggregate_common(a, b);
  }
};


static Type_collection_timestamp2_with_tz type_collection_timestamp2_with_tz;


const Type_collection *Type_handler_timestamp2_with_tz::type_collection() const
{
  return &type_collection_timestamp2_with_tz;
}


/*** Item methods **************************************************/


void Type_handler_timestamp2_with_tz::Item_update_null_value(Item *item) const
{
  NativeBufferTSwTZ tmp;
  item->val_native(current_thd, &tmp);
}


bool Type_handler_timestamp2_with_tz::Item_val_bool(Item *item) const
{
  return item_value_null(current_thd, item).to_bool();
}


void Type_handler_timestamp2_with_tz::Item_get_date(THD *thd,
                                                    Item *item,
                                                    Temporal::Warn *warn,
                                                    MYSQL_TIME *ltime,
                                                    date_mode_t fuzzydate) const
{
  item_value_null(thd, item).get_date(ltime);
}


String *Type_handler_timestamp2_with_tz::Item_func_hex_val_str_ascii(
                                           Item_func_hex *item,
                                           String *str) const
{
  return item->val_str_ascii_from_val_str(str);
}


/*** Cast to other data types **************************************/

longlong Type_handler_timestamp2_with_tz::Item_val_int_signed_typecast(
                                            Item *item) const
{
  return item_value_null(current_thd, item).to_longlong();
}


longlong Type_handler_timestamp2_with_tz::Item_val_int_unsigned_typecast(
                                            Item *item) const
{
  return item_value_null(current_thd, item).to_longlong();
}


/*** Item_param methods ********************************************/

void Type_handler_timestamp2_with_tz::Item_param_set_param_func(
                                        Item_param *param,
                                        uchar **pos, ulong len) const
{
  /*
    TODO: Item_param_set_param_func: awaiting for full WITH TIME ZONE support
    This should be fixed to store TIMESTAMP WITH TIME ZONE in Item_param
    natively. See also See MDEV-14271.
  */
  param->set_param_datetime(pos, len);
}


bool Type_handler_timestamp2_with_tz::Item_param_set_from_value(THD *thd,
                                        Item_param *param,
                                        const Type_all_attributes *attr,
                                        const st_value *value) const
{
  /*
    TODO: Item_param_set_from_value: awaiting for full WITH TIME ZONE support.
    Used in statements like this:
    EXECUTE IMMEDIATE 'SELECT ? AS c1'
      USING TIMESTAMP_TZ('2001-01-01 10:00:00','+04:00');
  */
  param->unsigned_flag= attr->unsigned_flag;
  param->set_time(&value->value.m_time, attr->max_length, attr->decimals);
  return false;
}


bool Type_handler_timestamp2_with_tz::Item_param_val_native(THD *thd,
                                                            Item_param *item,
                                                            Native *to) const
{
  DBUG_ASSERT(item->decimals <= TIME_SECOND_PART_DIGITS);
  /*
    TODO: Item_param_set_from_value: awaiting for full WITH TIME ZONE support.
  */
  Datetime dt(thd, item, Datetime::Options(TIME_NO_ZERO_IN_DATE, thd));
  Timestamp_with_tz_null ts(thd, dt, thd->variables.time_zone);
  thd->time_zone_used|= !ts.is_null();
  return item->null_value= ts.to_native(to, item->decimals);
}


/*** Literal *******************************************************/

Item_literal *Type_handler_timestamp2_with_tz::create_literal_item(THD *thd,
                                                 const char *str,
                                                 size_t length,
                                                 CHARSET_INFO *cs,
                                                 bool send_error) const
{
  /*
    TODO: create_literal_item: awaiting Item_literal_timestamp2_with_tz
    This method will be needed when we implement this syntax:
      SELECT TIMESTAMP WITH TIME ZONE '2001-01-10 10:00:00 +00:00';
  */
  MY_ASSERT_UNREACHABLE();
  return nullptr;
}


/*** SP variable ***************************************************/

String *Type_handler_timestamp2_with_tz::print_item_value(THD *thd,
                                                          Item *item,
                                                          String *str) const
{
  // TODO: print_item_value: awaiting Field_xxx for sp var support
  MY_ASSERT_UNREACHABLE();
  return str;
}


/*** Item_copy *****************************************************/

/*
  Example script:
  CREATE OR REPLACE TABLE t1 (a INT, b TIMESTAMP) ENGINE=MyISAM;
  INSERT INTO t1 VALUES (1,'2018-06-19 00:00:00');
  SELECT NULLIF(TIMESTAMP_TZ(b,'+00:00'), NULL) AS f, MAX(a) FROM t1 GROUP BY f;
*/

class Item_copy_timestamp2_with_tz: public Item_copy
{
  Timestamp_with_tz m_value;
  bool sane() const { return !null_value || !m_value.cmp(Timestamp_with_tz()); }
public:
  using TH = Type_handler_timestamp2_with_tz;
  Item_copy_timestamp2_with_tz(THD *thd, Item *arg): Item_copy(thd, arg) { }
  const Type_handler *type_handler() const override
  { return &type_handler_timestamp2_with_tz; }
  void copy() override
  {
    Timestamp_with_tz_null ts= TH::item_value_null(current_thd, item);
    m_value= (null_value= ts.is_null()) ? Timestamp_with_tz() : ts;
  }
  int save_in_field(Field *field, bool no_conversions) override
  {
    /*
      TODO: Item_copy_timestamp2_with_tz::save_in_field: awaiting Field_xxx
      This can go through a shorter path, like Item_copy_timestamp does.
      Let's add a method:
        bool Type_handler_timestamp2_with_tz::
          Timestamp_with_tz_save_in_field(const Timestamp_with_tz &ts) const;
      Let's do it togerther with adding a Field_xxx.
    */
    return type_handler_timestamp2_with_tz.
             Item_save_in_field(this, field, no_conversions);
  }
  longlong val_int() override
  {
    DBUG_ASSERT(sane());
    return null_value ? 0 : m_value.to_longlong();
  }
  double val_real() override
  {
    DBUG_ASSERT(sane());
    return null_value ? 0e0 : m_value.to_double();
  }
  String *val_str(String *to) override
  {
    DBUG_ASSERT(sane());
    return null_value ? NULL : m_value.val_str(to, decimals);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    DBUG_ASSERT(sane());
    return null_value ? NULL : m_value.to_decimal(to);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    DBUG_ASSERT(sane());
    return null_value ? true : m_value.get_date(ltime);
  }
  bool val_native(THD *thd, Native *to) override
  {
    DBUG_ASSERT(sane());
    return null_value || m_value.to_native(to, decimals);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_copy_timestamp2_with_tz>(thd, this); }
};


Item_copy *Type_handler_timestamp2_with_tz::create_item_copy(THD *thd,
                                                             Item *item) const
{
  return new (thd->mem_root) Item_copy_timestamp2_with_tz(thd, item);
}

/*** Cache *********************************************************/

class Item_cache_timestamp2_with_tz: public Item_cache
{
  Timestamp_with_tz_null m_native;
public:
  using TH = Type_handler_timestamp2_with_tz;
  Item_cache_timestamp2_with_tz(THD *thd)
   :Item_cache(thd, &type_handler_timestamp2_with_tz),
    m_native(Timeval(0,0), nullptr)
  { }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cache_timestamp2_with_tz>(thd, this); }
  bool cache_value() override
  {
    if (!example)
      return false;
    value_cached= true;
    THD *thd= current_thd;
    m_native= TH::item_value_null(thd, example);
    null_value_inside= null_value= m_native.is_null();
    return true;
  }
  String* val_str(String *to) override
  {
    return TH::item_value_null(current_thd, this).val_str(to, decimals);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return TH::item_value_null(current_thd, this).to_decimal(to);
  }
  longlong val_int() override
  {
    return TH::item_value_null(current_thd, this).to_longlong();
  }
  double val_real() override
  {
    return TH::item_value_null(current_thd, this).to_double();
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return TH::item_value_null(current_thd, this).get_date(ltime);
  }
  bool val_native(THD *thd, Native *to) override
  {
    if (!has_value())
    {
      null_value= true;
      return true;
    }
    return null_value= m_native.to_native(to, decimals);
  }
  longlong val_datetime_packed(THD *thd) override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  longlong val_time_packed(THD *) override
  {
    MY_ASSERT_UNREACHABLE();
    return 0;
  }
  int save_in_field(Field *field, bool no_conversions) override
  {
    if (!has_value())
      return set_field_to_null_with_conversions(field, no_conversions);
    return type_handler_timestamp2_with_tz.Item_save_in_field(this, field,
                                                              no_conversions);
  }
};


Item_cache *Type_handler_timestamp2_with_tz::Item_get_cache(THD *thd,
                                                            const Item *item)
                                                            const
{
  return new (thd->mem_root) Item_cache_timestamp2_with_tz(thd);
}


/*** BETWEEN methods ***********************************************/

bool Type_handler_timestamp2_with_tz::Item_func_between_fix_length_and_dec(
                                        Item_func_between *func) const
{
  return func->fix_length_and_dec_temporal(current_thd);
}


longlong Type_handler_timestamp2_with_tz::Item_func_between_val_int(
                                            Item_func_between *func) const
{
  return func->val_int_cmp_native();
}


/*** cmp_item - used in CASE and IN **********************************/

class cmp_item_timestamp2_with_tz: public cmp_item_scalar
{
  Timestamp_with_tz m_value;
public:
  using TH = Type_handler_timestamp2_with_tz;
  cmp_item_timestamp2_with_tz()
   :cmp_item_scalar(),
    m_value(Timeval(0,0), nullptr)
  { }

  void store_value(Item *item)
  {
    Timestamp_with_tz_null ts= TH::item_value_null(current_thd, item);
    if (!(m_null_value= ts.is_null()))
      m_value= ts;
  }

  int cmp_not_null(const Value *val)
  {
    /*
      This method will be implemented when we add this syntax:
        SELECT TIMESTAMP WITH TIME ZONE '2001-01-01 10:20:30'
      See also comments in in the same place in cmp_item_timestamp.
    */
    MY_ASSERT_UNREACHABLE();
    return 0;
  }

  int cmp(Item *arg)
  {
    THD *thd= current_thd;
    Timestamp_with_tz_null ts= TH::item_value_null(thd, arg);
    return m_null_value || ts.is_null() ? UNKNOWN : m_value.cmp(ts);
  }

  int compare(cmp_item *arg)
  {
    const cmp_item_timestamp2_with_tz *tmp=
      static_cast<cmp_item_timestamp2_with_tz*>(arg);
    return m_value.cmp(tmp->m_value);
  }

  cmp_item *make_same()
  {
    return new cmp_item_timestamp2_with_tz();
  }
};


cmp_item *Type_handler_timestamp2_with_tz::make_cmp_item(THD *thd,
                                                         CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_timestamp;
}


/**** IN classes and methods****************************************/

class in_timestamp2_with_tz :public in_vector
{
  Timestamp_with_tz m_value;

  static int cmp_timestamp2_with_tz(void *cmp_arg,
                                    Timestamp_with_tz *a,
                                    Timestamp_with_tz *b)
  {
    return a->cmp(*b);
  }

public:
  using TH = Type_handler_timestamp2_with_tz;
  in_timestamp2_with_tz(THD *thd, uint elements)
  // TODO: there is a bug in sizeof the same place in class in_timestam:
  :in_vector(thd, elements, sizeof(Timestamp_with_tz),
             (qsort2_cmp) cmp_timestamp2_with_tz, 0),
   m_value(Timeval(0,0), nullptr)
  { }
  const Type_handler *type_handler() const
  {
    return &type_handler_timestamp2_with_tz;
  }
  bool set(uint pos, Item *item)
  {
    Timestamp_with_tz *buff= &((Timestamp_with_tz *) base)[pos];
    const Timestamp_with_tz_null ts= TH::item_value_null(current_thd, item);
    if (ts.is_null())
    {
      *buff= Timestamp_with_tz(Timeval(0,0), nullptr);
      return true;
    }
    *buff= ts;
    return false;
  }
  uchar *get_value(Item *item)
  {
    const Timestamp_with_tz_null ts= TH::item_value_null(current_thd, item);
    if (ts.is_null())
      return 0;
    m_value= ts;
    return (uchar*) &m_value;
  }
  Item* create_item(THD *thd)
  {
    MY_ASSERT_UNREACHABLE();
    //return new (thd->mem_root) Item_timestamp_literal(thd);
    return nullptr;
  }
  void value_to_item(uint pos, Item *item)
  {
    MY_ASSERT_UNREACHABLE();
    //const Timestamp_with_tz &buff= (((Timestamp_with_tz*) base)[pos]);
    //static_cast<Item_timestamp_literal*>(item)->set_value(buff);
  }
};


in_vector *Type_handler_timestamp2_with_tz::make_in_vector(
                                              THD *thd,
                                              const Item_func_in *func,
                                              uint nargs) const
{
  return new (thd->mem_root) in_timestamp2_with_tz(thd, nargs);
}

bool
Type_handler_timestamp2_with_tz::Item_func_in_fix_comparator_compatible_types(
                                   THD *thd,
                                   Item_func_in *func) const
{
  // TODO: Item_func_in_fix_comparator_compatible_types: check when used
  return func->compatible_types_scalar_bisection_possible() ?
    (func->value_list_convert_const_to_int(thd) ||
     func->fix_for_scalar_comparison_using_bisection(thd)) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) TIME_RESULT);
}


/*** Unary operations: -, ABS. ROUND, TRUNCATE *********************/

bool Type_handler_timestamp2_with_tz::Item_func_abs_fix_length_and_dec(
                                        Item_func_abs *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


bool Type_handler_timestamp2_with_tz::Item_func_neg_fix_length_and_dec(
                                        Item_func_neg *func) const
{
  return Item_func_or_sum_illegal_param(func);
}

bool
Type_handler_timestamp2_with_tz::Item_func_int_val_fix_length_and_dec(
                                   Item_func_int_val *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


bool Type_handler_timestamp2_with_tz::Item_func_round_fix_length_and_dec(
                                        Item_func_round *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


/*** Hybrid function methods: CASE, COALESCE, etc ******************/

bool Type_handler_timestamp2_with_tz::Item_hybrid_func_fix_attributes(
                                     THD *thd,
                                     const char *name,
                                     Type_handler_hybrid_field_type *handler,
                                     Type_all_attributes *attr,
                                     Item **items, uint nitems)
                                     const
{
  // TODO: check maybe_null evaluation
  attr->aggregate_attributes_temporal(MAX_DATETIME_WIDTH, items, nitems);
  return false;
}


String *
Type_handler_timestamp2_with_tz::Item_func_hybrid_field_type_val_str(
                                   Item_func_hybrid_field_type *item,
                                     String *str) const
{
  THD *thd= current_thd;
  return item_value_null(thd, item).val_str(str, item->datetime_precision(thd));
}


double Type_handler_timestamp2_with_tz::Item_func_hybrid_field_type_val_real(
                                          Item_func_hybrid_field_type *item)
                                          const
{
  return item_value_null(current_thd, item).to_double();
}


longlong Type_handler_timestamp2_with_tz::Item_func_hybrid_field_type_val_int(
                                            Item_func_hybrid_field_type *item)
                                            const
{
  return item_value_null(current_thd, item).to_longlong();
}


my_decimal *
Type_handler_timestamp2_with_tz::Item_func_hybrid_field_type_val_decimal(
                                   Item_func_hybrid_field_type *item,
                                   my_decimal *to) const
{
  return item_value_null(current_thd, item).to_decimal(to);
}


void Type_handler_timestamp2_with_tz::Item_func_hybrid_field_type_get_date(
                                        THD *thd,
                                        Item_func_hybrid_field_type *item,
                                        Temporal::Warn *,
                                        MYSQL_TIME *ltime,
                                        date_mode_t) const
{
  item_value_null(thd, item).get_date(ltime);
}


/*** MIN/MAX methods ***********************************************/

bool Type_handler_timestamp2_with_tz::Item_sum_hybrid_fix_length_and_dec(
                                        Item_sum_hybrid *func) const
{
  return func->fix_length_and_dec_generic();
}


bool Type_handler_timestamp2_with_tz::Item_func_min_max_fix_attributes(
                                        THD *thd,
                                        Item_func_min_max *func,
                                        Item **items, uint nitems) const
{
  for (uint i= 0 ; i < nitems; i++)
  {
    /*
      Conversion from other types is not yet supported.
      See Type_collection_timestamp2_with_tz::aggregate_for_min_max()
    */
    DBUG_ASSERT(items[i]->type_handler() == this ||
                items[i]->type_handler() == &type_handler_null);
  }
  return Type_handler::Item_func_min_max_fix_attributes(thd, func,
                                                        items, nitems);
}


String *Type_handler_timestamp2_with_tz::Item_func_min_max_val_str(
                                           Item_func_min_max *func,
                                           String *str) const
{
  return item_value_null(current_thd, func).val_str(str, func->decimals);
}


double Type_handler_timestamp2_with_tz::Item_func_min_max_val_real(
                                          Item_func_min_max *func) const
{
  return item_value_null(current_thd, func).to_double();
}


longlong Type_handler_timestamp2_with_tz::Item_func_min_max_val_int(
                                            Item_func_min_max *func) const
{
  return item_value_null(current_thd, func).to_longlong();
}


my_decimal *Type_handler_timestamp2_with_tz::Item_func_min_max_val_decimal(
                                               Item_func_min_max *func,
                                               my_decimal *dec) const
{
  return item_value_null(current_thd, func).to_decimal(dec);
}


bool Type_handler_timestamp2_with_tz::Item_func_min_max_get_date(
                                        THD *thd, Item_func_min_max *func,
                                        MYSQL_TIME *ltime,
                                        date_mode_t fuzzydate) const
{
  return item_value_null(thd, func).get_date(ltime);
}


/*** Other Item_sum methods ***/

bool Type_handler_timestamp2_with_tz::Item_sum_sum_fix_length_and_dec(
                                        Item_sum_sum *func) const
{
  return Item_func_or_sum_illegal_param(func);
}

bool Type_handler_timestamp2_with_tz::Item_sum_avg_fix_length_and_dec(
                                        Item_sum_avg *func) const
{
  return Item_func_or_sum_illegal_param(func);
}

bool Type_handler_timestamp2_with_tz::Item_sum_variance_fix_length_and_dec(
                                        Item_sum_variance *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


bool Type_handler_timestamp2_with_tz::Item_func_plus_fix_length_and_dec(
                                        Item_func_plus *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


bool Type_handler_timestamp2_with_tz::Item_func_minus_fix_length_and_dec(
                                        Item_func_minus *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


bool Type_handler_timestamp2_with_tz::Item_func_mul_fix_length_and_dec(
                                        Item_func_mul *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


bool Type_handler_timestamp2_with_tz::Item_func_div_fix_length_and_dec(
                                        Item_func_div *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


bool Type_handler_timestamp2_with_tz::Item_func_mod_fix_length_and_dec(
                                        Item_func_mod *func) const
{
  return Item_func_or_sum_illegal_param(func);
}


/*** filesort methods **********************************************/

void Type_handler_timestamp2_with_tz::make_sort_key_part(
                                        uchar *to,
                                        Item *item,
                                        const SORT_FIELD_ATTR *sort_field,
                                        String *tmp) const
{
  THD *thd= current_thd;
  DBUG_ASSERT(item->decimals <= TIME_SECOND_PART_DIGITS);
  DBUG_ASSERT(sort_field->length == my_timestamp_binary_length(item->decimals));
  // Other types use _result functions to get the value
  Timestamp_with_tz_null ts= item_value_null(thd, item);
  if (ts.is_null())
  {
    bzero(to, sort_field->length + (item->maybe_null ? 0 : 1));
  }
  else
  {
    if (item->maybe_null)
      *to++= 1;
    ts.make_sort_key(to, sort_field->length, item->decimals);
  }
}


uint Type_handler_timestamp2_with_tz::make_packed_sort_key_part(
                                        uchar *to, Item *item,
                                        const SORT_FIELD_ATTR *sort_field,
                                        String *tmp) const
{
  THD *thd= current_thd;
  DBUG_ASSERT(item->decimals <= TIME_SECOND_PART_DIGITS);
  DBUG_ASSERT(sort_field->length == my_timestamp_binary_length(item->decimals));
  // Other types use _result functions to get the value
  Timestamp_with_tz_null ts= item_value_null(thd, item);
  if (ts.is_null())
  {
    if (item->maybe_null)
    {
      *to++= 0;
      return 0;
    }
    else
    {
      uint binlen= my_timestamp_binary_length(item->decimals);
      bzero(to, binlen);
      return binlen;
    }
  }
  else
  {
    if (item->maybe_null)
      *to++= 1;
    return ts.make_sort_key(to, sort_field->length, item->decimals);
  }
}


void Type_handler_timestamp2_with_tz::sort_length(THD *thd,
                                        const Type_std_attributes *item,
                                        SORT_FIELD_ATTR *sortorder) const
{
  DBUG_ASSERT(item->decimals <= TIME_SECOND_PART_DIGITS);
  sortorder->length= my_timestamp_binary_length(item->decimals);
  sortorder->original_length= sortorder->length;
}



/*** Column_definition methods *************************************/

bool Type_handler_timestamp2_with_tz::Column_definition_fix_attributes(
                                        Column_definition *def) const
{
  def->flags|= UNSIGNED_FLAG;
  return def->fix_attributes_temporal_with_time(MAX_DATETIME_WIDTH);
}


void Type_handler_timestamp2_with_tz::Column_definition_implicit_upgrade(
                                        Column_definition *c) const
{
}


bool
Type_handler_timestamp2_with_tz::Column_definition_attributes_frm_unpack(
                                          Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  // TODO: Column_definition_attributes_frm_unpack: awaiting Field_xxx
  return attr->frm_unpack_temporal_with_dec(share, MAX_DATETIME_WIDTH, buffer);
}


void Type_handler_timestamp2_with_tz::Column_definition_attributes_frm_pack(
                                        const Column_definition_attributes *def,
                                        uchar *buff) const
{
  // TODO: Column_definition_attributes_frm_pack: awaiting Field_xxx
  DBUG_ASSERT(f_decimals(def->pack_flag) == 0);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


bool Type_handler_timestamp2_with_tz::Column_definition_prepare_stage1(
                                        THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                            *derived_attr)
                                        const
{
  def->prepare_stage1_simple(&my_charset_numeric);
  return false;
}



/*** Field methods *************************************************/


uint32 Type_handler_timestamp2_with_tz::calc_pack_length(uint32 length) const
{
  // TODO: calc_pack_length: awaiting Field_xxx
  return length > MAX_DATETIME_WIDTH ?
         my_timestamp_binary_length(length - MAX_DATETIME_WIDTH - 1) : 4;
}


uint32
Type_handler_timestamp2_with_tz::max_display_length_for_field(
                                   const Conv_source &src) const
{
  // TODO: awaiting Field_xxx
  MY_ASSERT_UNREACHABLE();
  unsigned int metadata= src.metadata() & 0x00ff;
  return MAX_DATETIME_WIDTH + metadata + (metadata ? 1 : 0) + 6/*+00:00*/;
}


Field *Type_handler_timestamp2_with_tz::make_conversion_table_field(
                                                            MEM_ROOT *root,
                                                            TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  // TODO: make_conversion_table_field: awaiting Field_xxx
  return new(root)
         Field_timestampf(NULL, (uchar *) "", 1, Field::NONE,
                          &empty_clex_str, table->s, metadata);
}


Field *Type_handler_timestamp2_with_tz::make_table_field(MEM_ROOT *root,
                                          const LEX_CSTRING *field_name,
                                          const Record_addr &addr,
                                          const Type_all_attributes &attr,
                                          TABLE_SHARE *share) const
{
  // TODO: make_table_field: awaiting Field_xx
  if (current_thd->lex->sql_command == SQLCOM_CREATE_TABLE)
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             name().ptr(), "CREATE TABLE");
    return nullptr;
  }
  return new_Field_timestamp(root,
                             addr.ptr(), addr.null_ptr(), addr.null_bit(),
                             Field::NONE, field_name, share, attr.decimals);
}


Field *Type_handler_timestamp2_with_tz::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  // TODO: make_table_field_from_def: awaiting Field_xxx
  DBUG_ASSERT(attr->decimals == attr->temporal_dec(MAX_DATETIME_WIDTH));
  return new (mem_root)
    Field_timestampf(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                     attr->unireg_check,
                     name, share, attr->temporal_dec(MAX_DATETIME_WIDTH));
}


/*** Optimizer methods *********************************************/

Item *Type_handler_timestamp2_with_tz::make_const_item_for_comparison(
                                         THD *thd,
                                         Item *item,
                                         const Item *cmp) const
{
  // TODO: make_const_item_for_comparison: awaiting Field_xxx
  MY_ASSERT_UNREACHABLE(); // Mixing TS_W_TS with other types is not allowed yet
  return new (thd->mem_root) Item_null(thd, item->name.str);// will change
}


int Type_handler_timestamp2_with_tz::stored_field_cmp_to_item(THD *thd,
                                                              Field *field,
                                                              Item *item) const
{
  // TODO: stored_field_cmp_to_item: awaiting Field_xxx
  MY_ASSERT_UNREACHABLE();
  return 0;
}


bool Type_handler_timestamp2_with_tz::can_change_cond_ref_to_const(Item_bool_func2 *target,
                                        Item *target_expr, Item *target_value,
                                        Item_bool_func2 *source,
                                        Item *source_expr, Item *source_const)
                                        const
{
  // TODO: can_change_cond_ref_to_const: awaiting Field_xxx
  /*
    WHERE COALESCE(tstz_col)='val' AND COALESCE(tstz_col)=CONCAT(a);  -->
    WHERE COALESCE(tztz_col)='val' AND               'val'=CONCAT(a);
  */
  return target->compare_type_handler() == source->compare_type_handler();
}


bool Type_handler_timestamp2_with_tz::subquery_type_allows_materialization(
                                        const Item *inner,
                                        const Item *outer,
                                        bool is_in_predicate) const
{
  // TODO: subquery_type_allows_materialization: awaiting Field_xxx
  DBUG_ASSERT(inner->cmp_type() == TIME_RESULT);
  return outer->type_handler() == this;
}
