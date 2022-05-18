/*
   Copyright (c) 2015, 2021, MariaDB

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
#include "sql_type_geom.h"
#include "sql_const.h"
#include "sql_class.h"
#include "sql_time.h"
#include "sql_string.h"
#include "item.h"
#include "log.h"
#include "tztime.h"
#include <mysql/plugin_data_type.h>


const DTCollation &DTCollation_numeric::singleton()
{
  static const DTCollation_numeric tmp;
  return tmp;
}

Named_type_handler<Type_handler_row> type_handler_row("row");

Named_type_handler<Type_handler_null> type_handler_null("null");

Named_type_handler<Type_handler_bool> type_handler_bool("boolean");
Named_type_handler<Type_handler_tiny> type_handler_stiny("tinyint");
Named_type_handler<Type_handler_short> type_handler_sshort("smallint");
Named_type_handler<Type_handler_long> type_handler_slong("int");
Named_type_handler<Type_handler_int24> type_handler_sint24("mediumint");
Named_type_handler<Type_handler_longlong> type_handler_slonglong("bigint");
Named_type_handler<Type_handler_utiny> type_handler_utiny("tiny unsigned");
Named_type_handler<Type_handler_ushort> type_handler_ushort("smallint unsigned");
Named_type_handler<Type_handler_ulong> type_handler_ulong("int unsigned");
Named_type_handler<Type_handler_uint24> type_handler_uint24("mediumint unsigned");
Named_type_handler<Type_handler_ulonglong> type_handler_ulonglong("bigint unsigned");
Named_type_handler<Type_handler_vers_trx_id> type_handler_vers_trx_id("bigint unsigned");
Named_type_handler<Type_handler_float> type_handler_float("float");
Named_type_handler<Type_handler_double> type_handler_double("double");
Named_type_handler<Type_handler_bit> type_handler_bit("bit");

Named_type_handler<Type_handler_olddecimal> type_handler_olddecimal("decimal");
Named_type_handler<Type_handler_newdecimal> type_handler_newdecimal("decimal");

Named_type_handler<Type_handler_year> type_handler_year("year");
Named_type_handler<Type_handler_year> type_handler_year2("year");
Named_type_handler<Type_handler_time> type_handler_time("time");
Named_type_handler<Type_handler_date> type_handler_date("date");
Named_type_handler<Type_handler_timestamp> type_handler_timestamp("timestamp");
Named_type_handler<Type_handler_timestamp2> type_handler_timestamp2("timestamp");
Named_type_handler<Type_handler_datetime> type_handler_datetime("datetime");
Named_type_handler<Type_handler_time2> type_handler_time2("time");
Named_type_handler<Type_handler_newdate> type_handler_newdate("date");
Named_type_handler<Type_handler_datetime2> type_handler_datetime2("datetime");

Named_type_handler<Type_handler_enum> type_handler_enum("enum");
Named_type_handler<Type_handler_set> type_handler_set("set");

Named_type_handler<Type_handler_string> type_handler_string("char");
Named_type_handler<Type_handler_var_string> type_handler_var_string("varchar");
Named_type_handler<Type_handler_varchar> type_handler_varchar("varchar");
Named_type_handler<Type_handler_hex_hybrid> type_handler_hex_hybrid("hex_hybrid");
Named_type_handler<Type_handler_varchar_compressed> type_handler_varchar_compressed("varchar");

Named_type_handler<Type_handler_tiny_blob> type_handler_tiny_blob("tinyblob");
Named_type_handler<Type_handler_medium_blob> type_handler_medium_blob("mediumblob");
Named_type_handler<Type_handler_long_blob> type_handler_long_blob("longblob");
Named_type_handler<Type_handler_blob> type_handler_blob("blob");
Named_type_handler<Type_handler_blob_compressed> type_handler_blob_compressed("blob");

Type_handler_interval_DDhhmmssff type_handler_interval_DDhhmmssff;

Vers_type_timestamp       vers_type_timestamp;
Vers_type_trx             vers_type_trx;

/***************************************************************************/



class Type_collection_std: public Type_collection
{
public:
  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override
  {
    return NULL;
  }
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return Type_handler::aggregate_for_result_traditional(a, b);
  }
  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override;
  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override;
  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override;
};


static Type_collection_std type_collection_std;

const Type_collection *Type_handler::type_collection() const
{
  return &type_collection_std;
}


bool Type_handler::is_traditional_scalar_type() const
{
  return type_collection() == &type_collection_std;
}


class Type_collection_row: public Type_collection
{
public:
  bool init(Type_handler_data *data) override
  {
    return false;
  }
  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override
  {
    return NULL;
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
    DBUG_ASSERT(a == &type_handler_row);
    DBUG_ASSERT(b == &type_handler_row);
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


bool Type_handler_data::init()
{
#ifdef HAVE_SPATIAL
  return type_collection_geometry.init(this);
#endif
  return false;
}


Schema *Type_handler::schema() const
{
  return &mariadb_schema;
}


const Type_handler *
Type_handler::handler_by_name(THD *thd, const LEX_CSTRING &name)
{
  plugin_ref plugin;
  if ((plugin= my_plugin_lock_by_name(thd, &name, MariaDB_DATA_TYPE_PLUGIN)))
  {
    /*
      Data type plugins do not maintain ref_count yet.
      For now we have only mandatory built-in plugins
      and dynamic plugins for test purposes.
      It should be safe to unlock the plugin immediately.
    */
    const Type_handler *ph= reinterpret_cast<st_mariadb_data_type*>
                              (plugin_decl(plugin)->info)->type_handler;
    plugin_unlock(thd, plugin);
    return ph;
  }

#ifdef HAVE_SPATIAL
  const Type_handler *ha= type_collection_geometry.handler_by_name(name);
  if (ha)
    return ha;
#endif
  return NULL;
}


#ifndef DBUG_OFF
static const Type_handler *frm_data_type_info_emulate(const LEX_CSTRING &name)
{
  if (Name(STRING_WITH_LEN("xchar")).eq(name))
    return &type_handler_string;
  if (Name(STRING_WITH_LEN("xblob")).eq(name))
     return &type_handler_blob;
  return NULL;
}
#endif


const Type_handler *
Type_handler::handler_by_name_or_error(THD *thd, const LEX_CSTRING &name)
{
  const Type_handler *h= handler_by_name(thd, name);
  DBUG_EXECUTE_IF("emulate_handler_by_name_or_error_failure", h= NULL;);
  if (!h)
  {
    DBUG_EXECUTE_IF("frm_data_type_info_emulate",
      if ((h= frm_data_type_info_emulate(name)))
        return h;
    );
    my_error(ER_UNKNOWN_DATA_TYPE, MYF(0),
             ErrConvString(name.str, name.length, system_charset_info).ptr());
  }
  return h;
}


Type_handler_data *type_handler_data= NULL;


bool Float::to_string(String *val_buffer, uint dec) const
{
  uint to_length= 70;
  if (val_buffer->alloc(to_length))
    return true;

  char *to=(char*) val_buffer->ptr();
  size_t len;

  if (dec >= FLOATING_POINT_DECIMALS)
    len= my_gcvt(m_value, MY_GCVT_ARG_FLOAT, to_length - 1, to, NULL);
  else
  {
    /*
      We are safe here because the buffer length is 70, and
      fabs(float) < 10^39, dec < FLOATING_POINT_DECIMALS. So the resulting string
      will be not longer than 69 chars + terminating '\0'.
    */
    len= my_fcvt(m_value, (int) dec, to, NULL);
  }
  val_buffer->length((uint) len);
  val_buffer->set_charset(&my_charset_numeric);
  return false;
}


String_ptr::String_ptr(Item *item, String *buffer)
 :m_string_ptr(item->val_str(buffer))
{ }


Ascii_ptr::Ascii_ptr(Item *item, String *buffer)
 :String_ptr(item->val_str_ascii(buffer))
{ }


void VDec::set(Item *item)
{
  m_ptr= item->val_decimal(&m_buffer);
  DBUG_ASSERT((m_ptr == NULL) == item->null_value);
}


VDec::VDec(Item *item)
{
  m_ptr= item->val_decimal(&m_buffer);
  DBUG_ASSERT((m_ptr == NULL) == item->null_value);
}


VDec_op::VDec_op(Item_func_hybrid_field_type *item)
{
  m_ptr= item->decimal_op(&m_buffer);
  DBUG_ASSERT((m_ptr == NULL) == item->null_value);
}


date_conv_mode_t Temporal::sql_mode_for_dates(THD *thd)
{
  return ::sql_mode_for_dates(thd);
}


time_round_mode_t Temporal::default_round_mode(THD *thd)
{
  return thd->temporal_round_mode();
}


time_round_mode_t Timestamp::default_round_mode(THD *thd)
{
  return thd->temporal_round_mode();
}


my_decimal *Temporal::to_decimal(my_decimal *to) const
{
  return date2my_decimal(this, to);
}


my_decimal *Temporal::bad_to_decimal(my_decimal *to) const
{
  my_decimal_set_zero(to);
  return NULL;
}


void Temporal::make_from_str(THD *thd, Warn *warn,
                                   const char *str, size_t length,
                                   CHARSET_INFO *cs, date_mode_t fuzzydate)
{
  DBUG_EXECUTE_IF("str_to_datetime_warn",
                  push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                               ER_YES, ErrConvString(str, length,cs).ptr()););

  if (str_to_temporal(thd, warn, str, length, cs, fuzzydate))
    make_fuzzy_date(&warn->warnings, date_conv_mode_t(fuzzydate));
  if (warn->warnings)
    warn->set_str(str, length, &my_charset_bin);
}


Temporal_hybrid::Temporal_hybrid(THD *thd, Item *item, date_mode_t fuzzydate)
{
  if (item->get_date(thd, this, fuzzydate))
    time_type= MYSQL_TIMESTAMP_NONE;
}


uint Timestamp::binary_length_to_precision(uint length)
{
  switch (length) {
  case 4: return 0;
  case 5: return 2;
  case 6: return 4;
  case 7: return 6;
  }
  DBUG_ASSERT(0);
  return 0;
}


Timestamp::Timestamp(const Native &native)
{
  DBUG_ASSERT(native.length() >= 4 && native.length() <= 7);
  uint dec= binary_length_to_precision(native.length());
  my_timestamp_from_binary(this, (const uchar *) native.ptr(), dec);
}


bool Timestamp::to_native(Native *to, uint decimals) const
{
  uint len= my_timestamp_binary_length(decimals);
  if (to->reserve(len))
    return true;
  my_timestamp_to_binary(this, (uchar *) to->ptr(), decimals);
  to->length(len);
  return false;
}


bool Timestamp::to_TIME(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate) const
{
  return thd->timestamp_to_TIME(to, tv_sec, tv_usec, fuzzydate);
}


Timestamp::Timestamp(THD *thd, const MYSQL_TIME *ltime, uint *error_code)
 :Timeval(TIME_to_timestamp(thd, ltime, error_code), ltime->second_part)
{ }


Timestamp_or_zero_datetime::Timestamp_or_zero_datetime(THD *thd,
                                                       const MYSQL_TIME *ltime,
                                                       uint *error_code)
 :Timestamp(thd, ltime, error_code),
  m_is_zero_datetime(*error_code == ER_WARN_DATA_OUT_OF_RANGE)
{
  if (m_is_zero_datetime)
  {
    if (!non_zero_date(ltime))
      *error_code= 0;  // ltime was '0000-00-00 00:00:00'
  }
  else if (*error_code == ER_WARN_INVALID_TIMESTAMP)
    *error_code= 0; // ltime fell into spring time gap, adjusted.
}


bool Timestamp_or_zero_datetime::to_TIME(THD *thd, MYSQL_TIME *to,
                                         date_mode_t fuzzydate) const
{
  if (m_is_zero_datetime)
  {
    set_zero_time(to, MYSQL_TIMESTAMP_DATETIME);
    return false;
  }
  return Timestamp::to_TIME(thd, to, fuzzydate);
}


bool Timestamp_or_zero_datetime::to_native(Native *to, uint decimals) const
{
  if (m_is_zero_datetime)
  {
    to->length(0);
    return false;
  }
  return Timestamp::to_native(to, decimals);
}


int Timestamp_or_zero_datetime_native::save_in_field(Field *field,
                                                     uint decimals) const
{
  field->set_notnull();
  if (field->type_handler()->type_handler_for_native_format() ==
      &type_handler_timestamp2)
    return field->store_native(*this);
  if (is_zero_datetime())
  {
    static Datetime zero(Datetime::zero());
    return field->store_time_dec(zero.get_mysql_time(), decimals);
  }
  return field->store_timestamp_dec(Timestamp(*this).tv(), decimals);
}


void Sec6::make_from_decimal(const my_decimal *d, ulong *nanoseconds)
{
  m_neg= my_decimal2seconds(d, &m_sec, &m_usec, nanoseconds);
  m_truncated= (m_sec >= LONGLONG_MAX);
}


void Sec6::make_from_double(double nr, ulong *nanoseconds)
{
  if ((m_neg= nr < 0))
    nr= -nr;
  if ((m_truncated= nr > (double) LONGLONG_MAX))
  {
    m_sec= LONGLONG_MAX;
    m_usec= 0;
    *nanoseconds= 0;
  }
  else
  {
    m_sec= (ulonglong) nr;
    m_usec= (ulong) ((nr - floor(nr)) * 1000000000);
    *nanoseconds= m_usec % 1000;
    m_usec/= 1000;
  }
}


void Sec6::make_truncated_warning(THD *thd, const char *type_str) const
{
  char buff[1 + MAX_BIGINT_WIDTH + 1 + 6 + 1]; // '-' int '.' frac '\0'
  to_string(buff, sizeof(buff));
  thd->push_warning_truncated_wrong_value(type_str, buff);
}


bool Sec6::convert_to_mysql_time(THD *thd, int *warn, MYSQL_TIME *ltime,
                                 date_mode_t fuzzydate) const
{
  bool rc= fuzzydate & (TIME_INTERVAL_hhmmssff | TIME_INTERVAL_DAY) ?
             to_datetime_or_to_interval_hhmmssff(ltime, warn) :
           fuzzydate & TIME_TIME_ONLY ?
             to_datetime_or_time(ltime, warn, date_conv_mode_t(fuzzydate)) :
             to_datetime_or_date(ltime, warn, date_conv_mode_t(fuzzydate));
  DBUG_ASSERT(*warn || !rc);
  if (truncated())
    *warn|= MYSQL_TIME_WARN_TRUNCATED;
  return rc;
}


void Temporal::push_conversion_warnings(THD *thd, bool totally_useless_value,
                                        int warn,
                                        const char *typestr,
                                        const char *db_name,
                                        const char *table_name,
                                        const char *field_name,
                                        const char *value)
{
  if (MYSQL_TIME_WARN_HAVE_WARNINGS(warn))
    thd->push_warning_wrong_or_truncated_value(Sql_condition::WARN_LEVEL_WARN,
                                               totally_useless_value,
                                               typestr, value,
                                               db_name, table_name,
                                               field_name);
  else if (MYSQL_TIME_WARN_HAVE_NOTES(warn))
    thd->push_warning_wrong_or_truncated_value(Sql_condition::WARN_LEVEL_NOTE,
                                               false, typestr, value,
                                               db_name, table_name,
                                               field_name);
}


VSec9::VSec9(THD *thd, Item *item, const char *type_str, ulonglong limit)
{
  if (item->decimals == 0)
  { // optimize for an important special case
    Longlong_hybrid nr(item->val_int(), item->unsigned_flag);
    make_from_int(nr);
    m_is_null= item->null_value;
    if (!m_is_null && m_sec > limit)
    {
      m_sec= limit;
      m_truncated= true;
      ErrConvInteger err(nr);
      thd->push_warning_truncated_wrong_value(type_str, err.ptr());
    }
  }
  else if (item->cmp_type() == REAL_RESULT)
  {
    double nr= item->val_real();
    make_from_double(nr, &m_nsec);
    m_is_null= item->null_value;
    if (!m_is_null && m_sec > limit)
    {
      m_sec= limit;
      m_truncated= true;
    }
    if (m_truncated)
    {
      ErrConvDouble err(nr);
      thd->push_warning_truncated_wrong_value(type_str, err.ptr());
    }   
  }
  else
  {
    VDec tmp(item);
    (m_is_null= tmp.is_null()) ? reset() : make_from_decimal(tmp.ptr(), &m_nsec);
    if (!m_is_null && m_sec > limit)
    {
      m_sec= limit;
      m_truncated= true;
    }
    if (m_truncated)
    {
      ErrConvDecimal err(tmp.ptr());
      thd->push_warning_truncated_wrong_value(type_str, err.ptr());
    }
  }
}


Year::Year(longlong value, bool unsigned_flag, uint length)
{
  if ((m_truncated= (value < 0))) // Negative or huge unsigned
    m_year= unsigned_flag ? 9999 : 0;
  else if (value > 9999)
  {
    m_truncated= true;
    m_year= 9999;
  }
  else if (length == 2)
  {
    m_year= value < 70 ? (uint) value + 2000 :
             value <= 1900 ? (uint) value + 1900 :
             (uint) value;
  }
  else
    m_year= (uint) value;
  DBUG_ASSERT(m_year <= 9999);
}


uint Year::year_precision(const Item *item) const
{
  return item->type_handler() == &type_handler_year2 ? 2 : 4;
}


VYear::VYear(Item *item)
 :Year_null(item->to_longlong_null(), item->unsigned_flag, year_precision(item))
{ }


VYear_op::VYear_op(Item_func_hybrid_field_type *item)
 :Year_null(item->to_longlong_null_op(), item->unsigned_flag,
            year_precision(item))
{ }


const LEX_CSTRING Interval_DDhhmmssff::m_type_name=
  {STRING_WITH_LEN("INTERVAL DAY TO SECOND")};


Interval_DDhhmmssff::Interval_DDhhmmssff(THD *thd, Status *st,
                                         bool push_warnings,
                                         Item *item, ulong max_hour,
                                         time_round_mode_t mode, uint dec)
{
  switch (item->cmp_type()) {
  case ROW_RESULT:
    DBUG_ASSERT(0);
    time_type= MYSQL_TIMESTAMP_NONE;
    break;
  case TIME_RESULT:
    {
      // Rounding mode is not important here
      if (item->get_date(thd, this, Options(TIME_TIME_ONLY, TIME_FRAC_NONE)))
        time_type= MYSQL_TIMESTAMP_NONE;
      else if (time_type != MYSQL_TIMESTAMP_TIME)
      {
        st->warnings|= MYSQL_TIME_WARN_OUT_OF_RANGE;
        push_warning_wrong_or_truncated_value(thd, ErrConvTime(this),
                                              st->warnings);
        time_type= MYSQL_TIMESTAMP_NONE;
      }
      break;
    }
  case INT_RESULT:
  case REAL_RESULT:
  case DECIMAL_RESULT:
  case STRING_RESULT:
    {
      StringBuffer<STRING_BUFFER_USUAL_SIZE> tmp;
      String *str= item->val_str(&tmp);
      if (!str)
        time_type= MYSQL_TIMESTAMP_NONE;
      else if (str_to_DDhhmmssff(st, str->ptr(), str->length(), str->charset(),
                                 UINT_MAX32))
      {
        if (push_warnings)
          thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                        m_type_name.str,
                                        ErrConvString(str).ptr());
        time_type= MYSQL_TIMESTAMP_NONE;
      }
      else
      {
        if (mode == TIME_FRAC_ROUND)
          time_round_or_set_max(dec, &st->warnings, max_hour, st->nanoseconds);
        if (hour > max_hour)
        {
          st->warnings|= MYSQL_TIME_WARN_OUT_OF_RANGE;
          time_type= MYSQL_TIMESTAMP_NONE;
        }
        // Warn if hour or nanosecond truncation happened
        if (push_warnings)
          push_warning_wrong_or_truncated_value(thd, ErrConvString(str),
                                                st->warnings);
      }
    }
    break;
  }
  DBUG_ASSERT(is_valid_value_slow());
}


void
Interval_DDhhmmssff::push_warning_wrong_or_truncated_value(THD *thd,
                                                           const ErrConv &str,
                                                           int warnings)
{
  if (warnings & MYSQL_TIME_WARN_OUT_OF_RANGE)
  {
    thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                  m_type_name.str, str.ptr());
  }
  else if (MYSQL_TIME_WARN_HAVE_WARNINGS(warnings))
  {
    thd->push_warning_truncated_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                            m_type_name.str, str.ptr());
  }
  else if (MYSQL_TIME_WARN_HAVE_NOTES(warnings))
  {
    thd->push_warning_truncated_wrong_value(Sql_condition::WARN_LEVEL_NOTE,
                                            m_type_name.str, str.ptr());
  }
}


uint Interval_DDhhmmssff::fsp(THD *thd, Item *item)
{
  switch (item->cmp_type()) {
  case INT_RESULT:
  case TIME_RESULT:
    return item->decimals;
  case REAL_RESULT:
  case DECIMAL_RESULT:
    return MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS);
  case ROW_RESULT:
    DBUG_ASSERT(0);
    return 0;
  case STRING_RESULT:
    break;
  }
  if (!item->can_eval_in_optimize())
    return TIME_SECOND_PART_DIGITS;
  Status st;
  Interval_DDhhmmssff it(thd, &st, false/*no warnings*/, item, UINT_MAX32,
                         TIME_FRAC_TRUNCATE, TIME_SECOND_PART_DIGITS);
  return it.is_valid_interval_DDhhmmssff() ? st.precision :
                                             TIME_SECOND_PART_DIGITS;
}


bool Time::to_native(Native *to, uint decimals) const
{
  if (!is_valid_time())
  {
    to->length(0);
    return true;
  }
  uint len= my_time_binary_length(decimals);
  if (to->reserve(len))
    return true;
  longlong tmp= TIME_to_longlong_time_packed(get_mysql_time());
  my_time_packed_to_binary(tmp, (uchar*) to->ptr(), decimals);
  to->length(len);
  return false;
}


void Time::make_from_item(THD *thd, int *warn, Item *item, const Options opt)
{
  *warn= 0;
  if (item->get_date(thd, this, opt))
    time_type= MYSQL_TIMESTAMP_NONE;
  else
    valid_MYSQL_TIME_to_valid_value(thd, warn, opt);
}


static uint msec_round_add[7]=
{
  500000000,
  50000000,
  5000000,
  500000,
  50000,
  5000,
  0
};


Sec9 & Sec9::round(uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  if (Sec6::add_nanoseconds(m_nsec + msec_round_add[dec]))
    m_sec++;
  m_nsec= 0;
  Sec6::trunc(dec);
  return *this;
}


void Timestamp::round_or_set_max(uint dec, int *warn)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  if (add_nanoseconds_usec(msec_round_add[dec]) &&
      tv_sec++ >= TIMESTAMP_MAX_VALUE)
  {
    tv_sec= TIMESTAMP_MAX_VALUE;
    tv_usec= TIME_MAX_SECOND_PART;
    *warn|= MYSQL_TIME_WARN_OUT_OF_RANGE;
  }
  my_timeval_trunc(this, dec);
}


bool Temporal::add_nanoseconds_with_round(THD *thd, int *warn,
                                          date_conv_mode_t mode,
                                          ulong nsec)
{
  switch (time_type) {
  case MYSQL_TIMESTAMP_TIME:
  {
    ulong max_hour= (mode & (TIME_INTERVAL_DAY | TIME_INTERVAL_hhmmssff)) ?
                    TIME_MAX_INTERVAL_HOUR : TIME_MAX_HOUR;
    time_round_or_set_max(6, warn, max_hour, nsec);
    return false;
  }
  case MYSQL_TIMESTAMP_DATETIME:
    return datetime_round_or_invalidate(thd, 6, warn, nsec);
  case MYSQL_TIMESTAMP_DATE:
    return false;
  case MYSQL_TIMESTAMP_NONE:
    return false;
  case MYSQL_TIMESTAMP_ERROR:
    break;
  }
  DBUG_ASSERT(0);
  return false;
}


void Temporal::time_round_or_set_max(uint dec, int *warn,
                                     ulong max_hour, ulong nsec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  if (add_nanoseconds_mmssff(nsec) && ++hour > max_hour)
  {
    time_hhmmssff_set_max(max_hour);
    *warn|= MYSQL_TIME_WARN_OUT_OF_RANGE;
  }
  my_time_trunc(this, dec);
}


void Time::round_or_set_max(uint dec, int *warn, ulong nsec)
{
  Temporal::time_round_or_set_max(dec, warn, TIME_MAX_HOUR, nsec);
  DBUG_ASSERT(is_valid_time_slow());
}


void Time::round_or_set_max(uint dec, int *warn)
{
  round_or_set_max(dec, warn, msec_round_add[dec]);
}

/**
  Create from a DATETIME by subtracting a given number of days,
  implementing an optimized version of calc_time_diff().
*/
void Time::make_from_datetime_with_days_diff(int *warn, const MYSQL_TIME *from,
                                             long days)
{
  *warn= 0;
  DBUG_ASSERT(from->time_type == MYSQL_TIMESTAMP_DATETIME ||
              from->time_type == MYSQL_TIMESTAMP_DATE);
  long daynr= calc_daynr(from->year, from->month, from->day);
  long daydiff= daynr - days;
  if (!daynr) // Zero date
  {
    set_zero_time(this, MYSQL_TIMESTAMP_TIME);
    neg= true;
    hour= TIME_MAX_HOUR + 1; // to report "out of range" in "warn"
  }
  else if (daydiff >=0)
  {
    neg= false;
    year= month= day= 0;
    hhmmssff_copy(from);
    hour+= daydiff * 24;
    time_type= MYSQL_TIMESTAMP_TIME;
  }
  else
  {
    longlong timediff= ((((daydiff * 24LL +
                           from->hour)   * 60LL +
                           from->minute) * 60LL +
                           from->second) * 1000000LL +
                           from->second_part);
    unpack_time(timediff, this, MYSQL_TIMESTAMP_TIME);
    if (year || month)
    {
      *warn|= MYSQL_TIME_WARN_OUT_OF_RANGE;
      year= month= day= 0;
      hour= TIME_MAX_HOUR + 1;
    }
  }
  // The above code can generate TIME values outside of the valid TIME range.
  adjust_time_range_or_invalidate(warn);
}


void Time::make_from_datetime_move_day_to_hour(int *warn,
                                               const MYSQL_TIME *from)
{
  *warn= 0;
  DBUG_ASSERT(from->time_type == MYSQL_TIMESTAMP_DATE ||
              from->time_type == MYSQL_TIMESTAMP_DATETIME);
  time_type= MYSQL_TIMESTAMP_TIME;
  neg= false;
  year= month= day= 0;
  hhmmssff_copy(from);
  datetime_to_time_YYYYMMDD_000000DD_mix_to_hours(warn, from->year,
                                                  from->month, from->day);
  adjust_time_range_or_invalidate(warn);
}


void Time::make_from_datetime(int *warn, const MYSQL_TIME *from, long curdays)
{
  if (!curdays)
    make_from_datetime_move_day_to_hour(warn, from);
  else
    make_from_datetime_with_days_diff(warn, from, curdays);
}


void Time::make_from_time(int *warn, const MYSQL_TIME *from)
{
  DBUG_ASSERT(from->time_type == MYSQL_TIMESTAMP_TIME);
  if (from->year || from->month)
    make_from_out_of_range(warn);
  else
  {
    *warn= 0;
    DBUG_ASSERT(from->day == 0);
    *(static_cast<MYSQL_TIME*>(this))= *from;
    adjust_time_range_or_invalidate(warn);
  }
}


uint Time::binary_length_to_precision(uint length)
{
  switch (length) {
  case 3: return 0;
  case 4: return 2;
  case 5: return 4;
  case 6: return 6;
  }
  DBUG_ASSERT(0);
  return 0;
}


Time::Time(const Native &native)
{
  uint dec= binary_length_to_precision(native.length());
  longlong tmp= my_time_packed_from_binary((const uchar *) native.ptr(), dec);
  TIME_from_longlong_time_packed(this, tmp);
  DBUG_ASSERT(is_valid_time());
}


Time::Time(int *warn, const MYSQL_TIME *from, long curdays)
{
  switch (from->time_type) {
  case MYSQL_TIMESTAMP_NONE:
  case MYSQL_TIMESTAMP_ERROR:
    make_from_out_of_range(warn);
    break;
  case MYSQL_TIMESTAMP_DATE:
  case MYSQL_TIMESTAMP_DATETIME:
    make_from_datetime(warn, from, curdays);
    break;
  case MYSQL_TIMESTAMP_TIME:
    make_from_time(warn, from);
    break;
  }
  DBUG_ASSERT(is_valid_value_slow());
}


Time::Time(int *warn, bool neg, ulonglong hour, uint minute, const Sec6 &second)
{
  DBUG_ASSERT(second.sec() <= 59);
  *warn= 0;
  set_zero_time(this, MYSQL_TIMESTAMP_TIME);
  MYSQL_TIME::neg= neg;
  MYSQL_TIME::hour= hour > TIME_MAX_HOUR ? (uint) (TIME_MAX_HOUR + 1) :
                                           (uint) hour;
  MYSQL_TIME::minute= minute;
  MYSQL_TIME::second= (uint) second.sec();
  MYSQL_TIME::second_part= second.usec();
  adjust_time_range_or_invalidate(warn);
}


void Temporal_with_date::make_from_item(THD *thd, Item *item,
                                        date_mode_t fuzzydate)
{
  date_conv_mode_t flags= date_conv_mode_t(fuzzydate) & ~TIME_TIME_ONLY;
  /*
    Some TIME type items return error when trying to do get_date()
    without TIME_TIME_ONLY set (e.g. Item_field for Field_time).
    In the SQL standard time->datetime conversion mode we add TIME_TIME_ONLY.
    In the legacy time->datetime conversion mode we do not add TIME_TIME_ONLY
    and leave it to get_date() to check date.
  */
  date_conv_mode_t time_flag= (item->field_type() == MYSQL_TYPE_TIME &&
              !(thd->variables.old_behavior & OLD_MODE_ZERO_DATE_TIME_CAST)) ?
              TIME_TIME_ONLY : TIME_CONV_NONE;
  Options opt(flags | time_flag, time_round_mode_t(fuzzydate));
  if (item->get_date(thd, this, opt))
    time_type= MYSQL_TIMESTAMP_NONE;
  else if (time_type == MYSQL_TIMESTAMP_TIME)
  {
    MYSQL_TIME tmp;
    if (time_to_datetime_with_warn(thd, this, &tmp, flags))
      time_type= MYSQL_TIMESTAMP_NONE;
    else
      *(static_cast<MYSQL_TIME*>(this))= tmp;
  }
}


void Temporal_with_date::check_date_or_invalidate(int *warn,
                                                  date_conv_mode_t flags)
{
  if (::check_date(this, pack_time(this) != 0,
                   ulonglong(flags & TIME_MODE_FOR_XXX_TO_DATE), warn))
    time_type= MYSQL_TIMESTAMP_NONE;
}


void Datetime::make_from_time(THD *thd, int *warn, const MYSQL_TIME *from,
                              date_conv_mode_t flags)
{
  DBUG_ASSERT(from->time_type == MYSQL_TIMESTAMP_TIME);
  if (time_to_datetime(thd, from, this))
    make_from_out_of_range(warn);
  else
  {
    *warn= 0;
    check_date_or_invalidate(warn, flags);
  }
}


void Datetime::make_from_datetime(THD *thd, int *warn, const MYSQL_TIME *from,
                                  date_conv_mode_t flags)
{
  DBUG_ASSERT(from->time_type == MYSQL_TIMESTAMP_DATE ||
              from->time_type == MYSQL_TIMESTAMP_DATETIME);
  if (from->neg || check_datetime_range(from))
    make_from_out_of_range(warn);
  else
  {
    *warn= 0;
    *(static_cast<MYSQL_TIME*>(this))= *from;
    date_to_datetime(this);
    check_date_or_invalidate(warn, flags);
  }
}


Datetime::Datetime(THD *thd, const timeval &tv)
{
  thd->variables.time_zone->gmt_sec_to_TIME(this, tv.tv_sec);
  second_part= tv.tv_usec;
  thd->time_zone_used= 1;
  DBUG_ASSERT(is_valid_value_slow());
}


Datetime::Datetime(THD *thd, int *warn, const MYSQL_TIME *from,
                   date_conv_mode_t flags)
{
  DBUG_ASSERT(bool(flags & TIME_TIME_ONLY) == false);
  switch (from->time_type) {
  case MYSQL_TIMESTAMP_ERROR:
  case MYSQL_TIMESTAMP_NONE:
    make_from_out_of_range(warn);
    break;
  case MYSQL_TIMESTAMP_TIME:
    make_from_time(thd, warn, from, flags);
    break;
  case MYSQL_TIMESTAMP_DATETIME:
  case MYSQL_TIMESTAMP_DATE:
    make_from_datetime(thd, warn, from, flags);
    break;
  }
  DBUG_ASSERT(is_valid_value_slow());
}

Datetime::Datetime(my_time_t unix_time, ulong second_part_arg,
                   const Time_zone* time_zone)
{
  time_zone->gmt_sec_to_TIME(this, unix_time);
  second_part= second_part_arg;
}


bool Temporal::datetime_add_nanoseconds_or_invalidate(THD *thd, int *warn, ulong nsec)
{
  if (!add_nanoseconds_mmssff(nsec))
    return false;
  /*
    Overflow happened on minutes. Now we need to add 1 hour to the value.
    Catch a special case for the maximum possible date and hour==23, to
    truncate '9999-12-31 23:59:59.9999999' (with 7 fractional digits)
          to '9999-12-31 23:59:59.999999'  (with 6 fractional digits),
    with a warning, instead of returning an error, so this statement:
      INSERT INTO (datetime_column) VALUES ('9999-12-31 23:59:59.9999999');
    inserts a value truncated to 6 fractional digits, instead of zero
    date '0000-00-00 00:00:00.000000'.
  */
  if (year == 9999 && month == 12 && day == 31 && hour == 23)
  {
    minute= 59;
    second= 59;
    second_part= 999999;
    *warn= MYSQL_TIME_WARN_OUT_OF_RANGE;
    return false;
  }
  INTERVAL interval;
  memset(&interval, 0, sizeof(interval));
  interval.hour= 1;
  /*
    date_add_interval cannot handle bad dates with zero YYYY or MM.
    Note, check_date(NO_ZERO_XX) does not check YYYY against zero,
    so let's additionally check it.
  */
  if (year == 0 ||
      check_date(TIME_NO_ZERO_IN_DATE | TIME_NO_ZERO_DATE, warn) ||
      date_add_interval(thd, this, INTERVAL_HOUR, interval, false/*no warn*/))
  {
    char buf[MAX_DATE_STRING_REP_LENGTH];
    my_date_to_str(this, buf);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WRONG_VALUE_FOR_TYPE,
                        ER_THD(thd, ER_WRONG_VALUE_FOR_TYPE),
                        "date", buf, "round(datetime)");
    make_from_out_of_range(warn);
    return true;
  }
  return false;
}


bool Temporal::datetime_round_or_invalidate(THD *thd, uint dec, int *warn, ulong nsec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  if (datetime_add_nanoseconds_or_invalidate(thd, warn, nsec))
    return true;
  my_datetime_trunc(this, dec);
  return false;

}


bool Datetime::round_or_invalidate(THD *thd, uint dec, int *warn)
{
  return round_or_invalidate(thd, dec, warn, msec_round_add[dec]);
}


Datetime_from_temporal::Datetime_from_temporal(THD *thd, Item *temporal,
                                               date_conv_mode_t fuzzydate)
 :Datetime(thd, temporal, Options(fuzzydate, TIME_FRAC_NONE))
{
  // Exact rounding mode does not matter
  DBUG_ASSERT(temporal->cmp_type() == TIME_RESULT);
}


Datetime_truncation_not_needed::Datetime_truncation_not_needed(THD *thd, Item *item,
                                                               date_conv_mode_t mode)
 :Datetime(thd, item, Options(mode, TIME_FRAC_NONE))
{
  /*
    The called Datetime() constructor only would truncate nanoseconds if they
    existed (but we know there were no nanoseconds). Here we assert that there
    are also no microsecond digits outside of the scale specified in "dec".
  */
  DBUG_ASSERT(!is_valid_datetime() ||
              fraction_remainder(MY_MIN(item->decimals,
                                        TIME_SECOND_PART_DIGITS)) == 0);
}

/********************************************************************/

decimal_digits_t Type_numeric_attributes::find_max_decimals(Item **item, uint nitems)
{
  decimal_digits_t res= 0;
  for (uint i= 0; i < nitems; i++)
    set_if_bigger(res, item[i]->decimals);
  return res;
}


uint Type_numeric_attributes::count_unsigned(Item **item, uint nitems)
{
  uint res= 0;
  for (uint i= 0 ; i < nitems ; i++)
  {
    if (item[i]->unsigned_flag)
      res++;
  }
  return res;
}


uint32 Type_numeric_attributes::find_max_char_length(Item **item, uint nitems)
{
  uint32 char_length= 0;
  for (uint i= 0; i < nitems ; i++)
    set_if_bigger(char_length, item[i]->max_char_length());
  return char_length;
}


uint32 Type_numeric_attributes::find_max_octet_length(Item **item, uint nitems)
{
  uint32 octet_length= 0;
  for (uint i= 0; i < nitems ; i++)
    set_if_bigger(octet_length, item[i]->max_length);
  return octet_length;
}


decimal_digits_t Type_numeric_attributes::
find_max_decimal_int_part(Item **item, uint nitems)
{
  decimal_digits_t max_int_part= 0;
  for (uint i=0 ; i < nitems ; i++)
    set_if_bigger(max_int_part, item[i]->decimal_int_part());
  return max_int_part;
}


/**
  Set max_length/decimals of function if function is fixed point and
  result length/precision depends on argument ones.
*/

void
Type_numeric_attributes::aggregate_numeric_attributes_decimal(Item **item,
                                                              uint nitems,
                                                              bool unsigned_arg)
{
  decimal_digits_t max_int_part= find_max_decimal_int_part(item, nitems);
  decimals= find_max_decimals(item, nitems);
  decimal_digits_t precision= (decimal_digits_t)
    MY_MIN(max_int_part + decimals, DECIMAL_MAX_PRECISION);
  max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                           decimals,
                                                           unsigned_flag);
}


/**
  Set max_length/decimals of function if function is floating point and
  result length/precision depends on argument ones.
*/

void
Type_numeric_attributes::aggregate_numeric_attributes_real(Item **items,
                                                           uint nitems)
{
  uint32 length= 0;
  decimals= 0;
  max_length= 0;
  unsigned_flag= false;
  for (uint i=0 ; i < nitems ; i++)
  {
    if (decimals < FLOATING_POINT_DECIMALS)
    {
      set_if_bigger(decimals, items[i]->decimals);
      /* Will be ignored if items[i]->decimals >= FLOATING_POINT_DECIMALS */
      set_if_bigger(length, (items[i]->max_length - items[i]->decimals));
    }
    set_if_bigger(max_length, items[i]->max_length);
  }
  if (decimals < FLOATING_POINT_DECIMALS)
  {
    max_length= length;
    length+= decimals;
    if (length < max_length)  // If previous operation gave overflow
      max_length= UINT_MAX32;
    else
      max_length= length;
  }
  // Corner case: COALESCE(DOUBLE(255,4), DOUBLE(255,3)) -> FLOAT(255, 4)
  set_if_smaller(max_length, MAX_FIELD_CHARLENGTH);
}


/**
  Calculate max_length and decimals for string functions.

  @param field_type  Field type.
  @param items       Argument array.
  @param nitems      Number of arguments.

  @retval            False on success, true on error.
*/
bool Type_std_attributes::
aggregate_attributes_string(const LEX_CSTRING &func_name,
                            Item **items, uint nitems)
{
  if (agg_arg_charsets_for_string_result(collation, func_name,
                                         items, nitems, 1))
    return true;
  if (collation.collation == &my_charset_bin)
    max_length= find_max_octet_length(items, nitems);
  else
    fix_char_length(find_max_char_length(items, nitems));
  unsigned_flag= false;
  decimals= max_length ? NOT_FIXED_DEC : 0;
  return false;
}


/*
  Find a handler by its ODBC literal data type.

  @param type_str  - data type name, not necessarily 0-terminated
  @retval          - a pointer to data type handler if type_str points
                     to a known ODBC literal data type, or NULL otherwise
*/
const Type_handler *
Type_handler::odbc_literal_type_handler(const LEX_CSTRING *type_str)
{
  if (type_str->length == 1)
  {
    if (type_str->str[0] == 'd')      // {d'2001-01-01'}
      return &type_handler_newdate;
    else if (type_str->str[0] == 't') // {t'10:20:30'}
      return &type_handler_time2;
  }
  else if (type_str->length == 2)     // {ts'2001-01-01 10:20:30'}
  {
    if (type_str->str[0] == 't' && type_str->str[1] == 's')
      return &type_handler_datetime2;
  }
  return NULL; // Not a known ODBC literal type
}


/**
  This method is used by:
  - Item_user_var_as_out_param::field_type()
  - Item_func_udf_str::field_type()
  - Item_empty_string::make_send_field()

  TODO: type_handler_adjusted_to_max_octet_length() and string_type_handler()
  provide very similar functionality, to properly choose between
  VARCHAR/VARBINARY vs TEXT/BLOB variations taking into accoung maximum
  possible octet length.

  We should probably get rid of either of them and use the same method
  all around the code.
*/
const Type_handler *
Type_handler::string_type_handler(uint max_octet_length)
{
  if (max_octet_length >= 16777216)
    return &type_handler_long_blob;
  else if (max_octet_length >= 65536)
    return &type_handler_medium_blob;
  else if (max_octet_length >= MAX_FIELD_VARCHARLENGTH)
    return &type_handler_blob;
  return &type_handler_varchar;
}


const Type_handler *
Type_handler::varstring_type_handler(const Item *item)
{
  if (!item->max_length)
    return &type_handler_string;
  if (item->too_big_for_varchar())
    return blob_type_handler(item->max_length);
  return &type_handler_varchar;
}


const Type_handler *
Type_handler::blob_type_handler(uint max_octet_length)
{
  if (max_octet_length <= 255)
    return &type_handler_tiny_blob;
  if (max_octet_length <= 65535)
    return &type_handler_blob;
  if (max_octet_length <= 16777215)
    return &type_handler_medium_blob;
  return &type_handler_long_blob;
}


const Type_handler *
Type_handler::blob_type_handler(const Item *item)
{
  return blob_type_handler(item->max_length);
}

/**
  This method is used by:
  - Item_sum_hybrid, e.g. MAX(item), MIN(item).
  - Item_func_set_user_var
*/
const Type_handler *
Type_handler_string_result::type_handler_adjusted_to_max_octet_length(
                                                        uint max_octet_length,
                                                        CHARSET_INFO *cs) const
{
  if (max_octet_length / cs->mbmaxlen <= CONVERT_IF_BIGGER_TO_BLOB)
    return &type_handler_varchar; // See also Item::too_big_for_varchar()
  if (max_octet_length >= 16777216)
    return &type_handler_long_blob;
  else if (max_octet_length >= 65536)
    return &type_handler_medium_blob;
  return &type_handler_blob;
}


CHARSET_INFO *Type_handler::charset_for_protocol(const Item *item) const
{
  /*
    For backward compatibility, to make numeric
    data types return "binary" charset in client-side metadata.
  */
  return &my_charset_bin;
}


bool
Type_handler::Item_func_or_sum_illegal_param(const LEX_CSTRING &funcname) const
{
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           name().ptr(), funcname.str);
  return true;
}


bool
Type_handler::Item_func_or_sum_illegal_param(const Item_func_or_sum *it) const
{
  return Item_func_or_sum_illegal_param(it->func_name_cstring());
}


CHARSET_INFO *
Type_handler_string_result::charset_for_protocol(const Item *item) const
{
  return item->collation.collation;
}


const Type_handler *
Type_handler::get_handler_by_cmp_type(Item_result type)
{
  switch (type) {
  case REAL_RESULT:       return &type_handler_double;
  case INT_RESULT:        return &type_handler_slonglong;
  case DECIMAL_RESULT:    return &type_handler_newdecimal;
  case STRING_RESULT:     return &type_handler_long_blob;
  case TIME_RESULT:       return &type_handler_datetime;
  case ROW_RESULT:        return &type_handler_row;
  }
  DBUG_ASSERT(0);
  return &type_handler_string;
}


/*
  If we have a mixture of:
  - a MariaDB standard (built-in permanent) data type, and
  - a non-standard (optionally compiled or pluggable) data type,
  then we ask the type collection of the non-standard type to aggregate
  the mixture.
  The standard type collection type_collection_std knows nothing
  about non-standard types, while non-standard type collections
  know everything about standard data types.
*/
const Type_collection *
Type_handler::type_collection_for_aggregation(const Type_handler *h0,
                                              const Type_handler *h1)
{
  const Type_collection *c0= h0->type_collection();
  const Type_collection *c1= h1->type_collection();
  if (c0 == c1)
    return c0;
  if (c0 == &type_collection_std)
    return c1;
  if (c1 == &type_collection_std)
    return c0;
  /*
    A mixture of two non-standard collections.
    The caller code will continue to aggregate through
    the type aggregators in Type_handler_data.
  */
  return NULL;
}


Type_handler_hybrid_field_type::Type_handler_hybrid_field_type()
  :m_type_handler(&type_handler_double)
{
}


/***************************************************************************/

/* number of bytes to store second_part part of the TIMESTAMP(N) */
uint Type_handler_timestamp::m_sec_part_bytes[MAX_DATETIME_PRECISION + 1]=
     { 0, 1, 1, 2, 2, 3, 3 };

/* number of bytes to store DATETIME(N) */
uint Type_handler_datetime::m_hires_bytes[MAX_DATETIME_PRECISION + 1]=
     { 5, 6, 6, 7, 7, 7, 8 };

/* number of bytes to store TIME(N) */
uint Type_handler_time::m_hires_bytes[MAX_DATETIME_PRECISION + 1]=
     { 3, 4, 4, 5, 5, 5, 6 };

/***************************************************************************/

const Name Type_handler::version() const
{
  static const Name ver(STRING_WITH_LEN(""));
  return ver;
}

const Name & Type_handler::version_mariadb53()
{
  static const Name ver(STRING_WITH_LEN("mariadb-5.3"));
  return ver;
}

const Name & Type_handler::version_mysql56()
{
  static const Name ver(STRING_WITH_LEN("mysql-5.6"));
  return ver;
}


/***************************************************************************/

const Type_limits_int *Type_handler_tiny::type_limits_int() const
{
  static const Type_limits_sint8 limits_sint8;
  return &limits_sint8;
}

const Type_limits_int *Type_handler_utiny::type_limits_int() const
{
  static const Type_limits_uint8 limits_uint8;
  return &limits_uint8;
}

const Type_limits_int *Type_handler_short::type_limits_int() const
{
  static const Type_limits_sint16 limits_sint16;
  return &limits_sint16;
}

const Type_limits_int *Type_handler_ushort::type_limits_int() const
{
  static const Type_limits_uint16 limits_uint16;
  return &limits_uint16;
}

const Type_limits_int *Type_handler_int24::type_limits_int() const
{
  static const Type_limits_sint24 limits_sint24;
  return &limits_sint24;
}

const Type_limits_int *Type_handler_uint24::type_limits_int() const
{
  static const Type_limits_uint24 limits_uint24;
  return &limits_uint24;
}

const Type_limits_int *Type_handler_long::type_limits_int() const
{
  static const Type_limits_sint32 limits_sint32;
  return &limits_sint32;
}

const Type_limits_int *Type_handler_ulong::type_limits_int() const
{
  static const Type_limits_uint32 limits_uint32;
  return &limits_uint32;
}

const Type_limits_int *Type_handler_longlong::type_limits_int() const
{
  static const Type_limits_sint64 limits_sint64;
  return &limits_sint64;
}

const Type_limits_int *Type_handler_ulonglong::type_limits_int() const
{
  static const Type_limits_uint64 limits_uint64;
  return &limits_uint64;
}


/***************************************************************************/
const Type_handler *Type_handler_bool::type_handler_signed() const
{
  return &type_handler_bool;
}

const Type_handler *Type_handler_bool::type_handler_unsigned() const
{
  return &type_handler_bool;
}

const Type_handler *Type_handler_tiny::type_handler_signed() const
{
  return &type_handler_stiny;
}

const Type_handler *Type_handler_tiny::type_handler_unsigned() const
{
  return &type_handler_utiny;
}

const Type_handler *Type_handler_short::type_handler_signed() const
{
  return &type_handler_sshort;
}

const Type_handler *Type_handler_short::type_handler_unsigned() const
{
  return &type_handler_ushort;
}

const Type_handler *Type_handler_int24::type_handler_signed() const
{
  return &type_handler_sint24;
}

const Type_handler *Type_handler_int24::type_handler_unsigned() const
{
  return &type_handler_uint24;
}

const Type_handler *Type_handler_long::type_handler_signed() const
{
  return &type_handler_slong;
}

const Type_handler *Type_handler_long::type_handler_unsigned() const
{
  return &type_handler_ulong;
}

const Type_handler *Type_handler_longlong::type_handler_signed() const
{
  return &type_handler_slonglong;
}

const Type_handler *Type_handler_longlong::type_handler_unsigned() const
{
  return &type_handler_ulonglong;
}

/***************************************************************************/

const Type_handler *Type_handler_null::type_handler_for_comparison() const
{
  return &type_handler_null;
}


const Type_handler *Type_handler_int_result::type_handler_for_comparison() const
{
  return &type_handler_slonglong;
}


const Type_handler *Type_handler_string_result::type_handler_for_comparison() const
{
  return &type_handler_long_blob;
}


const Type_handler *Type_handler_decimal_result::type_handler_for_comparison() const
{
  return &type_handler_newdecimal;
}


const Type_handler *Type_handler_real_result::type_handler_for_comparison() const
{
  return &type_handler_double;
}


const Type_handler *Type_handler_time_common::type_handler_for_comparison() const
{
  return &type_handler_time;
}

const Type_handler *Type_handler_date_common::type_handler_for_comparison() const
{
  return &type_handler_newdate;
}


const Type_handler *Type_handler_datetime_common::type_handler_for_comparison() const
{
  return &type_handler_datetime;
}


const Type_handler *Type_handler_timestamp_common::type_handler_for_comparison() const
{
  return &type_handler_timestamp;
}


const Type_handler *Type_handler_row::type_handler_for_comparison() const
{
  return &type_handler_row;
}

/***************************************************************************/

const Type_handler *
Type_handler_timestamp_common::type_handler_for_native_format() const
{
  return &type_handler_timestamp2;
}


const Type_handler *
Type_handler_time_common::type_handler_for_native_format() const
{
  return &type_handler_time2;
}


/***************************************************************************/

const Type_handler *Type_handler_typelib::type_handler_for_item_field() const
{
  return &type_handler_string;
}


const Type_handler *Type_handler_typelib::cast_to_int_type_handler() const
{
  return &type_handler_slonglong;
}


/***************************************************************************/

bool
Type_handler_hybrid_field_type::aggregate_for_result(const Type_handler *other)
{
  Type_handler_pair tp(m_type_handler, other);
  do
  {
    const Type_handler *hres;
    const Type_collection *c;
    if (((c= Type_handler::type_collection_for_aggregation(tp.a(), tp.b())) &&
         (hres= c->aggregate_for_result(tp.a(), tp.b()))) ||
        (hres= type_handler_data->
                m_type_aggregator_for_result.find_handler(tp.a(), tp.b())))
    {
      m_type_handler= hres;
      return false;
    }
  } while (tp.to_base());
  return true;
}


const Type_handler *
Type_handler::type_handler_long_or_longlong(uint max_char_length,
                                            bool unsigned_flag)
{
  if (unsigned_flag)
  {
    if (max_char_length <= MY_INT32_NUM_DECIMAL_DIGITS - 2)
      return &type_handler_ulong;
    return &type_handler_ulonglong;
  }
  if (max_char_length <= MY_INT32_NUM_DECIMAL_DIGITS - 2)
    return &type_handler_slong;
  return &type_handler_slonglong;
}


/*
  This method is called for CASE (and its abbreviations) and LEAST/GREATEST
  when data type aggregation returned LONGLONG and there were some BIT
  expressions. This helps to adjust the data type from LONGLONG to LONG
  if all expressions fit.
*/
const Type_handler *
Type_handler::bit_and_int_mixture_handler(uint max_char_length)
{
  if (max_char_length <= MY_INT32_NUM_DECIMAL_DIGITS)
    return &type_handler_slong;
  return &type_handler_slonglong;
}


/**
  @brief Aggregates field types from the array of items.

  @param[in] items  array of items to aggregate the type from
  @param[in] nitems number of items in the array
  @param[in] treat_bit_as_number - if BIT should be aggregated to a non-BIT
             counterpart as a LONGLONG number or as a VARBINARY string.

             Currently behaviour depends on the function:
             - LEAST/GREATEST treat BIT as VARBINARY when
               aggregating with a non-BIT counterpart.
               Note, UNION also works this way.

             - CASE, COALESCE, IF, IFNULL treat BIT as LONGLONG when
               aggregating with a non-BIT counterpart;

             This inconsistency may be changed in the future. See MDEV-8867.

             Note, independently from "treat_bit_as_number":
             - a single BIT argument gives BIT as a result
             - two BIT couterparts give BIT as a result

  @details This function aggregates field types from the array of items.
    Found type is supposed to be used later as the result field type
    of a multi-argument function.
    Aggregation itself is performed by Type_handler::aggregate_for_result().

  @note The term "aggregation" is used here in the sense of inferring the
    result type of a function from its argument types.

  @retval false - on success
  @retval true  - on error
*/

bool Type_handler_hybrid_field_type::
aggregate_for_result(const LEX_CSTRING &funcname, Item **items, uint nitems,
                     bool treat_bit_as_number)
{
  bool bit_and_non_bit_mixture_found= false;
  uint32 max_display_length;
  if (!nitems || items[0]->result_type() == ROW_RESULT)
  {
    DBUG_ASSERT(0);
    set_handler(&type_handler_null);
    return true;
  }
  set_handler(items[0]->type_handler());
  max_display_length= items[0]->max_display_length();
  for (uint i= 1 ; i < nitems ; i++)
  {
    const Type_handler *cur= items[i]->type_handler();
    set_if_bigger(max_display_length, items[i]->max_display_length());
    if (treat_bit_as_number &&
        ((type_handler() == &type_handler_bit) ^ (cur == &type_handler_bit)))
    {
      bit_and_non_bit_mixture_found= true;
      if (type_handler() == &type_handler_bit)
        set_handler(&type_handler_slonglong); // BIT + non-BIT
      else
        cur= &type_handler_slonglong; // non-BIT + BIT
    }
    if (aggregate_for_result(cur))
    {
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
               type_handler()->name().ptr(), cur->name().ptr(), funcname.str);
      return true;
    }
  }
  if (bit_and_non_bit_mixture_found && type_handler() == &type_handler_slonglong)
    set_handler(Type_handler::bit_and_int_mixture_handler(max_display_length));
  return false;
}

/**
  Collect built-in data type handlers for comparison.
  This method is very similar to item_cmp_type() defined in item.cc.
  Now they coexist. Later item_cmp_type() will be removed.
  In addition to item_cmp_type(), this method correctly aggregates
  TIME with DATETIME/TIMESTAMP/DATE, so no additional find_date_time_item()
  is needed after this call.
*/

bool
Type_handler_hybrid_field_type::aggregate_for_comparison(const Type_handler *h)
{
  DBUG_ASSERT(m_type_handler == m_type_handler->type_handler_for_comparison());
  DBUG_ASSERT(h == h->type_handler_for_comparison());
  const Type_handler *hres;
  const Type_collection *c;
  if (!(c= Type_handler::type_collection_for_aggregation(m_type_handler, h)) ||
      !(hres= c->aggregate_for_comparison(m_type_handler, h)))
    hres= type_handler_data->
            m_type_aggregator_for_comparison.find_handler(m_type_handler, h);
  if (!hres)
    return true;
  m_type_handler= hres;
  DBUG_ASSERT(m_type_handler == m_type_handler->type_handler_for_comparison());
  return false;
}


const Type_handler *
Type_collection_std::aggregate_for_comparison(const Type_handler *ha,
                                              const Type_handler *hb) const
{
  Item_result a= ha->cmp_type();
  Item_result b= hb->cmp_type();
  if (a == STRING_RESULT && b == STRING_RESULT)
    return &type_handler_long_blob;
  if (a == INT_RESULT && b == INT_RESULT)
    return &type_handler_slonglong;
  if (a == ROW_RESULT || b == ROW_RESULT)
    return &type_handler_row;
  if (a == TIME_RESULT || b == TIME_RESULT)
  {
    if ((a == TIME_RESULT) + (b == TIME_RESULT) == 1)
    {
      /*
        We're here if there's only one temporal data type:
        either m_type_handler or h.
        Temporal types bit non-temporal types.
      */
      const Type_handler *res= b == TIME_RESULT ? hb : ha;
      /*
        Compare TIMESTAMP to a non-temporal type as DATETIME.
        This is needed to make queries with fuzzy dates work:
        SELECT * FROM t1
        WHERE
          ts BETWEEN '0000-00-00' AND '2010-00-01 00:00:00';
      */
      if (res->type_handler_for_native_format() == &type_handler_timestamp2)
        return &type_handler_datetime;
      return res;
    }
    else
    {
      /*
        We're here if both m_type_handler and h are temporal data types.
        - If both data types are TIME, we preserve TIME.
        - If both data types are DATE, we preserve DATE.
          Preserving DATE is needed for EXPLAIN FORMAT=JSON,
          to print DATE constants using proper format:
          'YYYY-MM-DD' rather than 'YYYY-MM-DD 00:00:00'.
      */
      if (ha->field_type() != hb->field_type())
        return &type_handler_datetime;
      return ha;
    }
  }
  if ((a == INT_RESULT || a == DECIMAL_RESULT) &&
      (b == INT_RESULT || b == DECIMAL_RESULT))
    return &type_handler_newdecimal;
  return &type_handler_double;
}


/**
  Aggregate data type handler for LEAST/GRATEST.
  aggregate_for_min_max() is close to aggregate_for_comparison(),
  but tries to preserve the exact type handler for string, int and temporal
  data types (instead of converting to super-types).
  FLOAT is not preserved and is converted to its super-type (DOUBLE).
  This should probably fixed eventually, for symmetry.
*/

bool
Type_handler_hybrid_field_type::aggregate_for_min_max(const Type_handler *h)
{
  Type_handler_pair tp(m_type_handler, h);
  do
  {
    const Type_handler *hres;
    const Type_collection *c;
    if (((c= Type_handler::type_collection_for_aggregation(tp.a(), tp.b())) &&
         (hres= c->aggregate_for_min_max(tp.a(), tp.b()))) ||
        (hres= type_handler_data->
                m_type_aggregator_for_result.find_handler(tp.a(), tp.b())))
    {
      /*
        For now we suppose that these two expressions:
          - LEAST(type1, type2)
          - COALESCE(type1, type2)
        return the same data type (or both expressions return error)
        if type1 and/or type2 are non-traditional.
        This may change in the future.
      */
      m_type_handler= hres;
      return false;
    }
  } while (tp.to_base());
  return true;
}


const Type_handler *
Type_collection_std::aggregate_for_min_max(const Type_handler *ha,
                                           const Type_handler *hb) const
{
  Item_result a= ha->cmp_type();
  Item_result b= hb->cmp_type();
  DBUG_ASSERT(a != ROW_RESULT); // Disallowed by check_cols() in fix_fields()
  DBUG_ASSERT(b != ROW_RESULT); // Disallowed by check_cols() in fix_fields()

  if (a == STRING_RESULT && b == STRING_RESULT)
    return Type_collection_std::aggregate_for_result(ha, hb);
  if (a == INT_RESULT && b == INT_RESULT)
  {
    // BIT aggregates with non-BIT as BIGINT
    if (ha != hb)
    {
      if (ha == &type_handler_bit)
        ha= &type_handler_slonglong;
      else if (hb == &type_handler_bit)
        hb= &type_handler_slonglong;
    }
    return Type_collection_std::aggregate_for_result(ha, hb);
  }
  if (a == TIME_RESULT || b == TIME_RESULT)
  {
    if ((ha->type_handler_for_native_format() == &type_handler_timestamp2) +
        (hb->type_handler_for_native_format() == &type_handler_timestamp2) == 1)
    {
      /*
        Handle LEAST(TIMESTAMP, non-TIMESTAMP) as DATETIME,
        to make sure fuzzy dates work in this context:
          LEAST('2001-00-00', timestamp_field)
      */
      return &type_handler_datetime2;
    }
    if ((a == TIME_RESULT) + (b == TIME_RESULT) == 1)
    {
      /*
        We're here if there's only one temporal data type:
        either m_type_handler or h.
        Temporal types bit non-temporal types.
      */
      return (b == TIME_RESULT) ? hb : ha;
    }
    /*
      We're here if both m_type_handler and h are temporal data types.
    */
    return Type_collection_std::aggregate_for_result(ha, hb);
  }
  if ((a == INT_RESULT || a == DECIMAL_RESULT) &&
      (b == INT_RESULT || b == DECIMAL_RESULT))
  {
    return &type_handler_newdecimal;
  }
  // Preserve FLOAT if two FLOATs, set to DOUBLE otherwise.
  if (ha == &type_handler_float && hb == &type_handler_float)
    return &type_handler_float;
  return &type_handler_double;
}


bool
Type_handler_hybrid_field_type::aggregate_for_min_max(const LEX_CSTRING &funcname,
                                                      Item **items, uint nitems)
{
  bool bit_and_non_bit_mixture_found= false;
  // LEAST/GREATEST require at least two arguments
  DBUG_ASSERT(nitems > 1);
  set_handler(items[0]->type_handler());
  for (uint i= 1; i < nitems;  i++)
  {
    const Type_handler *cur= items[i]->type_handler();
    // Check if BIT + non-BIT, or non-BIT + BIT
    bit_and_non_bit_mixture_found|= (m_type_handler == &type_handler_bit) !=
                                    (cur == &type_handler_bit);
    if (aggregate_for_min_max(cur))
    {
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
               type_handler()->name().ptr(), cur->name().ptr(), funcname.str);
      return true;
    }
  }
  if (bit_and_non_bit_mixture_found && type_handler() == &type_handler_slonglong)
  {
    uint32 max_display_length= items[0]->max_display_length();
    for (uint i= 1; i < nitems; i++)
      set_if_bigger(max_display_length, items[i]->max_display_length());
    set_handler(Type_handler::bit_and_int_mixture_handler(max_display_length));
  }
  return false;
}


const Type_handler *
Type_collection_std::aggregate_for_num_op(const Type_handler *h0,
                                          const Type_handler *h1) const
{
  Item_result r0= h0->cmp_type();
  Item_result r1= h1->cmp_type();

  if (r0 == REAL_RESULT || r1 == REAL_RESULT ||
      r0 == STRING_RESULT || r1 ==STRING_RESULT)
    return &type_handler_double;

  if (r0 == TIME_RESULT || r1 == TIME_RESULT)
    return &type_handler_datetime;

  if (r0 == DECIMAL_RESULT || r1 == DECIMAL_RESULT)
    return &type_handler_newdecimal;

  DBUG_ASSERT(r0 == INT_RESULT && r1 == INT_RESULT);
  return &type_handler_slonglong;
}


const Type_aggregator::Pair*
Type_aggregator::find_pair(const Type_handler *handler1,
                           const Type_handler *handler2) const
{
  for (uint i= 0; i < m_array.elements(); i++)
  {
    const Pair& el= m_array.at(i);
    if (el.eq(handler1, handler2) ||
        (m_is_commutative && el.eq(handler2, handler1)))
      return &el;
  }
  return NULL;
}


bool
Type_handler_hybrid_field_type::aggregate_for_num_op(const Type_aggregator *agg,
                                                     const Type_handler *h0,
                                                     const Type_handler *h1)
{
  Type_handler_pair tp(h0, h1);
  do
  {
    const Type_handler *hres;
    const Type_collection *c;
    if (((c= Type_handler::type_collection_for_aggregation(tp.a(), tp.b())) &&
         (hres= c->aggregate_for_num_op(tp.a(), tp.b()))) ||
        (hres= agg->find_handler(tp.a(), tp.b())))
    {
      m_type_handler= hres;
      return false;
    }
  } while (tp.to_base());
  return true;
}


/***************************************************************************/

const Type_handler *
Type_handler::get_handler_by_field_type(enum_field_types type)
{
  switch (type) {
  case MYSQL_TYPE_DECIMAL:     return &type_handler_olddecimal;
  case MYSQL_TYPE_NEWDECIMAL:  return &type_handler_newdecimal;
  case MYSQL_TYPE_TINY:        return &type_handler_stiny;
  case MYSQL_TYPE_SHORT:       return &type_handler_sshort;
  case MYSQL_TYPE_LONG:        return &type_handler_slong;
  case MYSQL_TYPE_LONGLONG:    return &type_handler_slonglong;
  case MYSQL_TYPE_INT24:       return &type_handler_sint24;
  case MYSQL_TYPE_YEAR:        return &type_handler_year;
  case MYSQL_TYPE_BIT:         return &type_handler_bit;
  case MYSQL_TYPE_FLOAT:       return &type_handler_float;
  case MYSQL_TYPE_DOUBLE:      return &type_handler_double;
  case MYSQL_TYPE_NULL:        return &type_handler_null;
  case MYSQL_TYPE_VARCHAR:     return &type_handler_varchar;
  case MYSQL_TYPE_TINY_BLOB:   return &type_handler_tiny_blob;
  case MYSQL_TYPE_MEDIUM_BLOB: return &type_handler_medium_blob;
  case MYSQL_TYPE_LONG_BLOB:   return &type_handler_long_blob;
  case MYSQL_TYPE_BLOB:        return &type_handler_blob;
  case MYSQL_TYPE_VAR_STRING:  return &type_handler_varchar; // Map to VARCHAR 
  case MYSQL_TYPE_STRING:      return &type_handler_string;
  case MYSQL_TYPE_ENUM:        return &type_handler_varchar; // Map to VARCHAR
  case MYSQL_TYPE_SET:         return &type_handler_varchar; // Map to VARCHAR
  case MYSQL_TYPE_GEOMETRY:
#ifdef HAVE_SPATIAL
    return &type_handler_geometry;
#else
    return NULL;
#endif
  case MYSQL_TYPE_TIMESTAMP:   return &type_handler_timestamp2;// Map to timestamp2
  case MYSQL_TYPE_TIMESTAMP2:  return &type_handler_timestamp2;
  case MYSQL_TYPE_DATE:        return &type_handler_newdate;   // Map to newdate
  case MYSQL_TYPE_TIME:        return &type_handler_time2;     // Map to time2
  case MYSQL_TYPE_TIME2:       return &type_handler_time2;
  case MYSQL_TYPE_DATETIME:    return &type_handler_datetime2; // Map to datetime2
  case MYSQL_TYPE_DATETIME2:   return &type_handler_datetime2;
  case MYSQL_TYPE_NEWDATE:
    /*
      NEWDATE is actually a real_type(), not a field_type(),
      but it's used around the code in field_type() context.
      We should probably clean up the code not to use MYSQL_TYPE_NEWDATE
      in field_type() context and add DBUG_ASSERT(0) here.
    */
    return &type_handler_newdate;
  case MYSQL_TYPE_VARCHAR_COMPRESSED:
  case MYSQL_TYPE_BLOB_COMPRESSED:
    break;
  };
  DBUG_ASSERT(0);
  return &type_handler_string;
}


const Type_handler *
Type_handler::get_handler_by_real_type(enum_field_types type)
{
  switch (type) {
  case MYSQL_TYPE_DECIMAL:     return &type_handler_olddecimal;
  case MYSQL_TYPE_NEWDECIMAL:  return &type_handler_newdecimal;
  case MYSQL_TYPE_TINY:        return &type_handler_stiny;
  case MYSQL_TYPE_SHORT:       return &type_handler_sshort;
  case MYSQL_TYPE_LONG:        return &type_handler_slong;
  case MYSQL_TYPE_LONGLONG:    return &type_handler_slonglong;
  case MYSQL_TYPE_INT24:       return &type_handler_sint24;
  case MYSQL_TYPE_YEAR:        return &type_handler_year;
  case MYSQL_TYPE_BIT:         return &type_handler_bit;
  case MYSQL_TYPE_FLOAT:       return &type_handler_float;
  case MYSQL_TYPE_DOUBLE:      return &type_handler_double;
  case MYSQL_TYPE_NULL:        return &type_handler_null;
  case MYSQL_TYPE_VARCHAR:     return &type_handler_varchar;
  case MYSQL_TYPE_VARCHAR_COMPRESSED: return &type_handler_varchar_compressed;
  case MYSQL_TYPE_TINY_BLOB:   return &type_handler_tiny_blob;
  case MYSQL_TYPE_MEDIUM_BLOB: return &type_handler_medium_blob;
  case MYSQL_TYPE_LONG_BLOB:   return &type_handler_long_blob;
  case MYSQL_TYPE_BLOB:        return &type_handler_blob;
  case MYSQL_TYPE_BLOB_COMPRESSED: return &type_handler_blob_compressed;
  case MYSQL_TYPE_VAR_STRING:  return &type_handler_var_string;
  case MYSQL_TYPE_STRING:      return &type_handler_string;
  case MYSQL_TYPE_ENUM:        return &type_handler_enum;
  case MYSQL_TYPE_SET:         return &type_handler_set;
  case MYSQL_TYPE_GEOMETRY:
#ifdef HAVE_SPATIAL
    return &type_handler_geometry;
#else
    return NULL;
#endif
  case MYSQL_TYPE_TIMESTAMP:   return &type_handler_timestamp;
  case MYSQL_TYPE_TIMESTAMP2:  return &type_handler_timestamp2;
  case MYSQL_TYPE_DATE:        return &type_handler_date;
  case MYSQL_TYPE_TIME:        return &type_handler_time;
  case MYSQL_TYPE_TIME2:       return &type_handler_time2;
  case MYSQL_TYPE_DATETIME:    return &type_handler_datetime;
  case MYSQL_TYPE_DATETIME2:   return &type_handler_datetime2;
  case MYSQL_TYPE_NEWDATE:     return &type_handler_newdate;
  };
  return NULL;
}


/**
  Create a DOUBLE field by default.
*/
Field *
Type_handler::make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                                 const Item *item) const
{
  return new(mem_root)
         Field_double(NULL, item->max_length,
                      (uchar *) (item->maybe_null() ? "" : 0),
                      item->maybe_null() ? 1 : 0, Field::NONE,
                      &item->name, (uint8) item->decimals,
                      0, item->unsigned_flag);
}


Field *
Type_handler_float::make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                                       const Item *item)
                                                       const
{
  return new(mem_root)
         Field_float(NULL, item->max_length,
                     (uchar *) (item->maybe_null() ? "" : 0),
                     item->maybe_null() ? 1 : 0, Field::NONE,
                     &item->name, (uint8) item->decimals,
                     0, item->unsigned_flag);
}


Field *
Type_handler_decimal_result::make_num_distinct_aggregator_field(
                                                            MEM_ROOT *mem_root,
                                                            const Item *item)
                                                            const
{
  return new (mem_root)
         Field_new_decimal(NULL, item->max_length,
                           (uchar *) (item->maybe_null() ? "" : 0),
                           item->maybe_null() ? 1 : 0, Field::NONE,
                           &item->name, (uint8) item->decimals,
                           0, item->unsigned_flag);
}


Field *
Type_handler_int_result::make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                                            const Item *item)
                                                            const
{
  /**
    Make a longlong field for all INT-alike types. It could create
    smaller fields for TINYINT, SMALLINT, MEDIUMINT, INT though.
  */
  return new(mem_root)
         Field_longlong(NULL, item->max_length,
                        (uchar *) (item->maybe_null() ? "" : 0),
                        item->maybe_null() ? 1 : 0, Field::NONE,
                        &item->name, 0, item->unsigned_flag);
}


/***********************************************************************/

Field *Type_handler_tiny::make_conversion_table_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  /*
    As we don't know if the integer was signed or not on the master,
    assume we have same sign on master and slave.  This is true when not
    using conversions so it should be true also when using conversions.
  */
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (root)
         Field_tiny(NULL, 4 /*max_length*/, (uchar *) "", 1, Field::NONE,
                    &empty_clex_str, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_short::make_conversion_table_field(MEM_ROOT *root,
                                                       TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (root)
         Field_short(NULL, 6 /*max_length*/, (uchar *) "", 1, Field::NONE,
                     &empty_clex_str, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_int24::make_conversion_table_field(MEM_ROOT *root,
                                                       TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (root)
         Field_medium(NULL, 9 /*max_length*/, (uchar *) "", 1, Field::NONE,
                      &empty_clex_str, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_long::make_conversion_table_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (root)
         Field_long(NULL, 11 /*max_length*/, (uchar *) "", 1, Field::NONE,
         &empty_clex_str, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_longlong::make_conversion_table_field(MEM_ROOT *root,
                                                          TABLE *table,
                                                          uint metadata,
                                                          const Field *target)
                                                          const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (root)
         Field_longlong(NULL, 20 /*max_length*/,(uchar *) "", 1, Field::NONE,
                        &empty_clex_str, 0/*zerofill*/, unsigned_flag);
}



Field *Type_handler_float::make_conversion_table_field(MEM_ROOT *root,
                                                       TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  return new (root)
         Field_float(NULL, 12 /*max_length*/, (uchar *) "", 1, Field::NONE,
                     &empty_clex_str, 0/*dec*/, 0/*zerofill*/, 0/*unsigned_flag*/);
}


Field *Type_handler_double::make_conversion_table_field(MEM_ROOT *root,
                                                        TABLE *table,
                                                        uint metadata,
                                                        const Field *target)
                                                        const
{
  return new (root)
         Field_double(NULL, 22 /*max_length*/, (uchar *) "", 1, Field::NONE,
                      &empty_clex_str, 0/*dec*/, 0/*zerofill*/, 0/*unsigned_flag*/);
}


Field *Type_handler_newdecimal::make_conversion_table_field(MEM_ROOT *root, 
                                                            TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  int  precision= metadata >> 8;
  uint8 decimals= metadata & 0x00ff;
  uint32 max_length= my_decimal_precision_to_length(precision, decimals, false);
  DBUG_ASSERT(decimals <= DECIMAL_MAX_SCALE);
  return new (root)
         Field_new_decimal(NULL, max_length, (uchar *) "", 1, Field::NONE,
                           &empty_clex_str, decimals, 0/*zerofill*/, 0/*unsigned*/);
}


Field *Type_handler_olddecimal::make_conversion_table_field(MEM_ROOT *root,
                                                            TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  sql_print_error("In RBR mode, Slave received incompatible DECIMAL field "
                  "(old-style decimal field) from Master while creating "
                  "conversion table. Please consider changing datatype on "
                  "Master to new style decimal by executing ALTER command for"
                  " column Name: %s.%s.%s.",
                  target->table->s->db.str,
                  target->table->s->table_name.str,
                  target->field_name.str);
  return NULL;
}


Field *Type_handler_year::make_conversion_table_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new(root)
         Field_year(NULL, 4, (uchar *) "", 1, Field::NONE, &empty_clex_str);
}


Field *Type_handler_null::make_conversion_table_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new(root)
         Field_null(NULL, 0, Field::NONE, &empty_clex_str, target->charset());
}


Field *Type_handler_timestamp::make_conversion_table_field(MEM_ROOT *root,
                                                           TABLE *table,
                                                           uint metadata,
                                                           const Field *target)
                                                           const
{
  return new_Field_timestamp(root, NULL, (uchar *) "", 1,
                             Field::NONE, &empty_clex_str,
                             table->s, target->decimals());
}


Field *Type_handler_timestamp2::make_conversion_table_field(MEM_ROOT *root,
                                                            TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  return new(root)
         Field_timestampf(NULL, (uchar *) "", 1, Field::NONE,
                          &empty_clex_str, table->s, metadata);
}


Field *Type_handler_newdate::make_conversion_table_field(MEM_ROOT *root,
                                                         TABLE *table,
                                                         uint metadata,
                                                         const Field *target)
                                                         const
{
  return new(root)
         Field_newdate(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str);
}


Field *Type_handler_date::make_conversion_table_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new(root)
         Field_date(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str);
}


Field *Type_handler_time::make_conversion_table_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new_Field_time(root, NULL, (uchar *) "", 1,
                        Field::NONE, &empty_clex_str, target->decimals());
}


Field *Type_handler_time2::make_conversion_table_field(MEM_ROOT *root,
                                                       TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  return new(root)
         Field_timef(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str, metadata);
}


Field *Type_handler_datetime::make_conversion_table_field(MEM_ROOT *root,
                                                          TABLE *table,
                                                          uint metadata,
                                                          const Field *target)
                                                          const
{
  return new_Field_datetime(root, NULL, (uchar *) "", 1,
                            Field::NONE, &empty_clex_str, target->decimals());
}


Field *Type_handler_datetime2::make_conversion_table_field(MEM_ROOT *root,
                                                           TABLE *table,
                                                           uint metadata,
                                                           const Field *target)
                                                           const
{
  return new(root)
         Field_datetimef(NULL, (uchar *) "", 1,
                         Field::NONE, &empty_clex_str, metadata);
}


Field *Type_handler_bit::make_conversion_table_field(MEM_ROOT *root,
                                                     TABLE *table,
                                                     uint metadata,
                                                     const Field *target)
                                                     const
{
  DBUG_ASSERT((metadata & 0xff) <= 7);
  uint32 max_length= 8 * (metadata >> 8U) + (metadata & 0x00ff);
  return new(root)
         Field_bit_as_char(NULL, max_length, (uchar *) "", 1,
                           Field::NONE, &empty_clex_str);
}


Field *Type_handler_string::make_conversion_table_field(MEM_ROOT *root,
                                                        TABLE *table,
                                                        uint metadata,
                                                        const Field *target)
                                                        const
{
  /* This is taken from Field_string::unpack. */
  uint32 max_length= (((metadata >> 4) & 0x300) ^ 0x300) + (metadata & 0x00ff);
  return new(root)
         Field_string(NULL, max_length, (uchar *) "", 1,
                      Field::NONE, &empty_clex_str, target->charset());
}


Field *Type_handler_varchar::make_conversion_table_field(MEM_ROOT *root,
                                                         TABLE *table,
                                                         uint metadata,
                                                         const Field *target)
                                                         const
{
  DBUG_ASSERT(HA_VARCHAR_PACKLENGTH(metadata) <= MAX_FIELD_VARCHARLENGTH);
  return new(root)
         Field_varstring(NULL, metadata, HA_VARCHAR_PACKLENGTH(metadata),
                         (uchar *) "", 1, Field::NONE, &empty_clex_str,
                         table->s, target->charset());
}


Field *Type_handler_varchar_compressed::make_conversion_table_field(
                                                         MEM_ROOT *root, 
                                                         TABLE *table,
                                                         uint metadata,
                                                         const Field *target)
                                                         const
{
  return new(root)
         Field_varstring_compressed(NULL, metadata,
                                    HA_VARCHAR_PACKLENGTH(metadata),
                                    (uchar *) "", 1, Field::NONE,
                                    &empty_clex_str,
                                    table->s, target->charset(),
                                    zlib_compression_method);
}



Field *Type_handler_blob_compressed::make_conversion_table_field(
                                                      MEM_ROOT *root, 
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  uint pack_length= metadata & 0x00ff;
  if (pack_length < 1 || pack_length > 4)
    return NULL; // Broken binary log?
  return new(root)
         Field_blob_compressed(NULL, (uchar *) "", 1, Field::NONE,
                               &empty_clex_str,
                               table->s, pack_length, target->charset(),
                               zlib_compression_method);
}


Field *Type_handler_enum::make_conversion_table_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  DBUG_ASSERT(target->type() == MYSQL_TYPE_STRING);
  DBUG_ASSERT(target->real_type() == MYSQL_TYPE_ENUM);
  return new(root)
         Field_enum(NULL, target->field_length,
                    (uchar *) "", 1, Field::NONE, &empty_clex_str,
                    metadata & 0x00ff/*pack_length()*/,
                    ((const Field_enum*) target)->typelib, target->charset());
}


Field *Type_handler_set::make_conversion_table_field(MEM_ROOT *root,
                                                     TABLE *table,
                                                     uint metadata,
                                                     const Field *target)
                                                     const
{
  DBUG_ASSERT(target->type() == MYSQL_TYPE_STRING);
  DBUG_ASSERT(target->real_type() == MYSQL_TYPE_SET);
  return new(root)
         Field_set(NULL, target->field_length,
                   (uchar *) "", 1, Field::NONE, &empty_clex_str,
                   metadata & 0x00ff/*pack_length()*/,
                   ((const Field_enum*) target)->typelib, target->charset());
}


Field *Type_handler_enum::make_schema_field(MEM_ROOT *root, TABLE *table,
                                            const Record_addr &addr,
                                            const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  const Typelib *typelib= def.typelib();
  DBUG_ASSERT(typelib);
  /*
    Assume I_S columns don't have non-ASCII characters in names.
    If we eventually want to, Typelib::max_char_length() must be implemented.
  */
  return new (root)
         Field_enum(addr.ptr(), (uint32) typelib->max_octet_length(),
                    addr.null_ptr(), addr.null_bit(),
                    Field::NONE, &name,
                    get_enum_pack_length(typelib->count),
                    typelib, system_charset_info);

}


/*************************************************************************/

bool Type_handler::
       Column_definition_validate_check_constraint(THD *thd,
                                                   Column_definition * c) const
{
  return c->validate_check_constraint(thd);
}


/*************************************************************************/

bool
Type_handler::Column_definition_set_attributes(THD *thd,
                                               Column_definition *def,
                                               const Lex_field_type_st &attr,
                                               column_definition_type_t type)
                                               const
{
  def->set_lex_charset_collation(attr.lex_charset_collation());
  def->set_length_and_dec(attr);
  return false;
}


/*
  In sql_mode=ORACLE, real size of VARCHAR and CHAR with no length
  in SP parameters is fixed at runtime with the length of real args.
  Let's translate VARCHAR to VARCHAR(4000) for return value.

  Since Oracle 9, maximum size for VARCHAR in PL/SQL is 32767.

  In MariaDB the limit for VARCHAR is 65535 bytes.
  We could translate VARCHAR with no length to VARCHAR(65535), but
  it would mean that for multi-byte character sets we'd have to translate
  VARCHAR to MEDIUMTEXT, to guarantee 65535 characters.

  Also we could translate VARCHAR to VARCHAR(16383), where 16383 is
  the maximum possible length in characters in case of mbmaxlen=4
  (e.g. utf32, utf16, utf8mb4). However, we'll have character sets with
  mbmaxlen=5 soon (e.g. gb18030).
*/

bool
Type_handler_string::Column_definition_set_attributes(
                                                 THD *thd,
                                                 Column_definition *def,
                                                 const Lex_field_type_st &attr,
                                                 column_definition_type_t type)
                                                 const
{
  Type_handler::Column_definition_set_attributes(thd, def, attr, type);
  if (attr.has_explicit_length())
    return false;
  switch (type) {
  case COLUMN_DEFINITION_ROUTINE_PARAM:
  case COLUMN_DEFINITION_FUNCTION_RETURN:
    if (thd->variables.sql_mode & MODE_ORACLE)
    {
      // See Type_handler_varchar::Column_definition_set_attributes()
      def->length= def->decimals= 2000;
      def->set_handler(&type_handler_varchar);
      return false;
    }
    break;
  case COLUMN_DEFINITION_ROUTINE_LOCAL:
  case COLUMN_DEFINITION_TABLE_FIELD:
    break;
  }
  def->length= 1;
  return false;
}


bool
Type_handler_varchar::Column_definition_set_attributes(
                                                 THD *thd,
                                                 Column_definition *def,
                                                 const Lex_field_type_st &attr,
                                                 column_definition_type_t type)
                                                 const
{
  Type_handler::Column_definition_set_attributes(thd, def, attr, type);
  if (attr.has_explicit_length())
    return false;
  switch (type) {
  case COLUMN_DEFINITION_ROUTINE_PARAM:
  case COLUMN_DEFINITION_FUNCTION_RETURN:
    if (thd->variables.sql_mode & MODE_ORACLE)
    {
      /*
        Type_handler_varchar::adjust_spparam_type() tests "decimals"
        to detect if the formal parameter length needs to be adjusted to
        the actual parameter length. Non-zero decimals means that the length
        was set implicitly to the default value and needs to be adjusted.
      */
      def->length= def->decimals= 4000;
      return false;
    }
    break;
  case COLUMN_DEFINITION_ROUTINE_LOCAL:
  case COLUMN_DEFINITION_TABLE_FIELD:
    break;
  }
  thd->parse_error();
  return true;
}


/*************************************************************************/
bool Type_handler_null::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return false;
}

bool Type_handler_tiny::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_int(MAX_TINYINT_WIDTH + def->sign_length());
}

bool Type_handler_short::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_int(MAX_SMALLINT_WIDTH + def->sign_length());
}

bool Type_handler_int24::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_int(MAX_MEDIUMINT_WIDTH + def->sign_length());
}

bool Type_handler_long::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_int(MAX_INT_WIDTH + def->sign_length());
}

bool Type_handler_longlong::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_int(MAX_BIGINT_WIDTH/*no sign_length() added*/);
}

bool Type_handler_newdecimal::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_decimal();
}

bool Type_handler_olddecimal::
       Column_definition_fix_attributes(Column_definition *def) const
{
  DBUG_ASSERT(0); // Obsolete
  return true;
}

bool Type_handler_var_string::
       Column_definition_fix_attributes(Column_definition *def) const
{
  DBUG_ASSERT(0); // Obsolete
  return true;
}

bool Type_handler_varchar::
       Column_definition_fix_attributes(Column_definition *def) const
{
  /*
    Long VARCHAR's are automaticly converted to blobs in mysql_prepare_table
    if they don't have a default value
  */
  return def->check_length(ER_TOO_BIG_DISPLAYWIDTH, MAX_FIELD_BLOBLENGTH);
}

bool Type_handler_string::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->check_length(ER_TOO_BIG_FIELDLENGTH, MAX_FIELD_CHARLENGTH);
}

bool Type_handler_blob_common::
       Column_definition_fix_attributes(Column_definition *def) const
{
  def->flags|= BLOB_FLAG;
  return def->check_length(ER_TOO_BIG_DISPLAYWIDTH, MAX_FIELD_BLOBLENGTH);
}


bool Type_handler_year::
       Column_definition_fix_attributes(Column_definition *def) const
{
  if (!def->length || def->length != 2)
    def->length= 4; // Default length
  def->flags|= ZEROFILL_FLAG | UNSIGNED_FLAG;
  return false;
}

bool Type_handler_float::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_real(MAX_FLOAT_STR_LENGTH);
}


bool Type_handler_double::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_real(DBL_DIG + 7);
}

bool Type_handler_timestamp_common::
       Column_definition_fix_attributes(Column_definition *def) const
{
  def->flags|= UNSIGNED_FLAG;
  return def->fix_attributes_temporal_with_time(MAX_DATETIME_WIDTH);
}

bool Type_handler_date_common::
       Column_definition_fix_attributes(Column_definition *def) const
{
  // We don't support creation of MYSQL_TYPE_DATE anymore
  def->set_handler(&type_handler_newdate);
  def->length= MAX_DATE_WIDTH;
  return false;
}

bool Type_handler_time_common::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_temporal_with_time(MIN_TIME_WIDTH);
}

bool Type_handler_datetime_common::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_temporal_with_time(MAX_DATETIME_WIDTH);
}

bool Type_handler_set::
       Column_definition_fix_attributes(Column_definition *def) const
{
  def->pack_length= get_set_pack_length(def->interval_list.elements);
  return false;
}

bool Type_handler_enum::
       Column_definition_fix_attributes(Column_definition *def) const
{
  def->pack_length= get_enum_pack_length(def->interval_list.elements);
  return false;
}

bool Type_handler_bit::
       Column_definition_fix_attributes(Column_definition *def) const
{
  return def->fix_attributes_bit();
}

/*************************************************************************/

void Type_handler_typelib::
       Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *def,
                                              const Field *field) const
{
  DBUG_ASSERT(def->flags & (ENUM_FLAG | SET_FLAG));
  def->interval= field->get_typelib();
}


void Type_handler_year::
       Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *def,
                                              const Field *field) const
{
  if (def->length != 4)
  {
    char buff[sizeof("YEAR()") + MY_INT64_NUM_DECIMAL_DIGITS + 1];
    my_snprintf(buff, sizeof(buff), "YEAR(%llu)", def->length);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_WARN_DEPRECATED_SYNTAX,
                        ER_THD(thd, ER_WARN_DEPRECATED_SYNTAX),
                        buff, "YEAR(4)");
  }
}


void Type_handler_real_result::
       Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *def,
                                              const Field *field) const
{
  /*
    Floating points are stored with FLOATING_POINT_DECIMALS but internally
    in MariaDB used with NOT_FIXED_DEC, which is >= FLOATING_POINT_DECIMALS.
  */
  if (def->decimals >= FLOATING_POINT_DECIMALS)
    def->decimals= NOT_FIXED_DEC;
}


/*************************************************************************/

bool Type_handler::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  def->prepare_stage1_simple(&my_charset_bin);
  return false;
}

bool Type_handler_null::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  def->prepare_charset_for_string(derived_attr);
  def->create_length_to_internal_length_null();
  return false;
}

bool Type_handler_row::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  def->charset= &my_charset_bin;
  def->create_length_to_internal_length_null();
  return false;
}

bool Type_handler_temporal_result::
       Column_definition_prepare_stage1(THD *thd,
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


bool Type_handler_numeric::
       Column_definition_prepare_stage1(THD *thd,
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

bool Type_handler_newdecimal::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  def->charset= &my_charset_numeric;
  def->create_length_to_internal_length_newdecimal();
  return false;
}

bool Type_handler_bit::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  def->charset= &my_charset_numeric;
  return def->prepare_stage1_bit(thd, mem_root, file, table_flags);
}

bool Type_handler_typelib::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  return def->prepare_charset_for_string(derived_attr) ||
         def->prepare_stage1_typelib(thd, mem_root, file, table_flags);
}


bool Type_handler_string_result::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  return def->prepare_charset_for_string(derived_attr) ||
         def->prepare_stage1_string(thd, mem_root, file, table_flags);
}


/*************************************************************************/

bool Type_handler_general_purpose_string::
       Column_definition_bulk_alter(Column_definition *def,
                                    const Column_derived_attributes
                                          *derived_attr,
                                    const Column_bulk_alter_attributes
                                          *bulk_alter_attr)
                                    const
{
  if (!bulk_alter_attr->alter_table_convert_to_charset())
    return false; // No "CONVERT TO" clause.
  CHARSET_INFO *defcs= def->explicit_or_derived_charset(derived_attr);
  DBUG_ASSERT(defcs);
  /*
    Handle 'ALTER TABLE t1 CONVERT TO CHARACTER SET csname'.
    Change character sets for all varchar/char/text columns,
    but do not touch varbinary/binary/blob columns.
  */
  if (!(def->flags & CONTEXT_COLLATION_FLAG) && defcs != &my_charset_bin)
    def->charset= bulk_alter_attr->alter_table_convert_to_charset();
  return false;
};


/*************************************************************************/

bool Type_handler::
       Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const
{
  def->redefine_stage1_common(dup, file);
  def->create_length_to_internal_length_simple();
  return false;
}


bool Type_handler_null::
       Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const
{
  def->redefine_stage1_common(dup, file);
  def->create_length_to_internal_length_null();
  return false;
}


bool Type_handler_newdecimal::
       Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const
{
  def->redefine_stage1_common(dup, file);
  def->create_length_to_internal_length_newdecimal();
  return false;
}


bool Type_handler_string_result::
       Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const
{
  def->redefine_stage1_common(dup, file);
  def->set_compression_method(dup->compression_method());
  def->create_length_to_internal_length_string();
  return false;
}


bool Type_handler_typelib::
       Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const
{
  def->redefine_stage1_common(dup, file);
  def->create_length_to_internal_length_typelib();
  return false;
}


bool Type_handler_bit::
       Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const
{
  def->redefine_stage1_common(dup, file);
  /*
    If we are replacing a field with a BIT field, we need
    to initialize pack_flag.
  */
  def->pack_flag= FIELDFLAG_NUMBER;
  if (!(file->ha_table_flags() & HA_CAN_BIT_FIELD))
    def->pack_flag|= FIELDFLAG_TREAT_BIT_AS_CHAR;
  def->create_length_to_internal_length_bit();
  return false;
}


/*************************************************************************/

bool Type_handler::
       Column_definition_prepare_stage2_legacy(Column_definition *def,
                                               enum_field_types type) const
{
  def->pack_flag= f_settype((uint) type);
  return false;
}

bool Type_handler::
       Column_definition_prepare_stage2_legacy_num(Column_definition *def,
                                                   enum_field_types type) const
{
  def->pack_flag= def->pack_flag_numeric() | f_settype((uint) type);
  return false;
}

bool Type_handler::
       Column_definition_prepare_stage2_legacy_real(Column_definition *def,
                                                    enum_field_types type) const
{
  uint dec= def->decimals;
  /*
    User specified FLOAT() or DOUBLE() without precision. Change to
    FLOATING_POINT_DECIMALS to keep things compatible with earlier MariaDB
    versions.
  */
  if (dec >= FLOATING_POINT_DECIMALS)
    dec= FLOATING_POINT_DECIMALS;
  def->decimals= dec;
  def->pack_flag= def->pack_flag_numeric() | f_settype((uint) type);
  return false;
}

bool Type_handler_newdecimal::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  def->pack_flag= def->pack_flag_numeric();
  return false;
}

bool Type_handler_blob_common::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  return def->prepare_stage2_blob(file, table_flags, FIELDFLAG_BLOB);
}

bool Type_handler_varchar::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  return def->prepare_stage2_varchar(table_flags);
}

bool Type_handler_string::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  def->pack_flag= (def->charset->state & MY_CS_BINSORT) ? FIELDFLAG_BINARY : 0;
  return false;
}

bool Type_handler_enum::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  uint dummy;
  return def->prepare_stage2_typelib("ENUM", FIELDFLAG_INTERVAL, &dummy);
}

bool Type_handler_set::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  uint dup_count;
  if (def->prepare_stage2_typelib("SET", FIELDFLAG_BITFIELD, &dup_count))
    return true;
  /* Check that count of unique members is not more then 64 */
  if (def->interval->count - dup_count > sizeof(longlong)*8)
  {
     my_error(ER_TOO_BIG_SET, MYF(0), def->field_name.str);
     return true;
  }
  return false;
}

bool Type_handler_bit::
       Column_definition_prepare_stage2(Column_definition *def,
                                        handler *file,
                                        ulonglong table_flags) const
{
  /* 
    We have sql_field->pack_flag already set here, see
    mysql_prepare_create_table().
  */
  return false;
}


/*************************************************************************/
bool Type_handler::Key_part_spec_init_primary(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  part->length*= def.charset->mbmaxlen;
  return false;
}


bool Type_handler::Key_part_spec_init_unique(Key_part_spec *part,
                                             const Column_definition &def,
                                             const handler *file,
                                             bool *has_field_needed) const
{
  part->length*= def.charset->mbmaxlen;
  return false;
}


bool Type_handler::Key_part_spec_init_multiple(Key_part_spec *part,
                                               const Column_definition &def,
                                               const handler *file) const
{
  part->length*= def.charset->mbmaxlen;
  return false;
}


bool Type_handler::Key_part_spec_init_foreign(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  part->length*= def.charset->mbmaxlen;
  return false;
}


bool Type_handler::Key_part_spec_init_spatial(Key_part_spec *part,
                                              const Column_definition &def)
                                              const
{
  my_error(ER_WRONG_ARGUMENTS, MYF(0), "SPATIAL INDEX");
  return true;
}


bool Type_handler_blob_common::Key_part_spec_init_primary(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file) const
{
  part->length*= def.charset->mbmaxlen;
  return part->check_primary_key_for_blob(file);
}


bool Type_handler_blob_common::Key_part_spec_init_unique(Key_part_spec *part,
                                              const Column_definition &def,
                                              const handler *file,
                                              bool *hash_field_needed) const
{
  if (!(part->length*= def.charset->mbmaxlen))
    *hash_field_needed= true;
  return part->check_key_for_blob(file);
}


bool Type_handler_blob_common::Key_part_spec_init_multiple(Key_part_spec *part,
                                               const Column_definition &def,
                                               const handler *file) const
{
  part->length*= def.charset->mbmaxlen;
  return part->init_multiple_key_for_blob(file);
}


bool Type_handler_blob_common::Key_part_spec_init_foreign(Key_part_spec *part,
                                               const Column_definition &def,
                                               const handler *file) const
{
  part->length*= def.charset->mbmaxlen;
  return part->check_foreign_key_for_blob(file);
}



/*************************************************************************/

uint32 Type_handler_time::calc_pack_length(uint32 length) const
{
  return length > MIN_TIME_WIDTH ?
         hires_bytes(length - 1 - MIN_TIME_WIDTH) : 3;
}

uint32 Type_handler_time2::calc_pack_length(uint32 length) const
{
  return length > MIN_TIME_WIDTH ?
         my_time_binary_length(length - MIN_TIME_WIDTH - 1) : 3;
}

uint32 Type_handler_timestamp::calc_pack_length(uint32 length) const
{
  return length > MAX_DATETIME_WIDTH ?
         4 + sec_part_bytes(length - 1 - MAX_DATETIME_WIDTH) : 4;
}

uint32 Type_handler_timestamp2::calc_pack_length(uint32 length) const
{
  return length > MAX_DATETIME_WIDTH ?
         my_timestamp_binary_length(length - MAX_DATETIME_WIDTH - 1) : 4;
}

uint32 Type_handler_datetime::calc_pack_length(uint32 length) const
{
  return length > MAX_DATETIME_WIDTH ?
         hires_bytes(length - 1 - MAX_DATETIME_WIDTH) : 8;
}

uint32 Type_handler_datetime2::calc_pack_length(uint32 length) const
{
  return length > MAX_DATETIME_WIDTH ?
         my_datetime_binary_length(length - MAX_DATETIME_WIDTH - 1) : 5;
}

uint32 Type_handler_tiny_blob::calc_pack_length(uint32 length) const
{
  return 1 + portable_sizeof_char_ptr;
}

uint32 Type_handler_blob::calc_pack_length(uint32 length) const
{
  return 2 + portable_sizeof_char_ptr;
}

uint32 Type_handler_medium_blob::calc_pack_length(uint32 length) const
{
  return 3 + portable_sizeof_char_ptr;
}

uint32 Type_handler_long_blob::calc_pack_length(uint32 length) const
{
  return 4 + portable_sizeof_char_ptr;
}

uint32 Type_handler_newdecimal::calc_pack_length(uint32 length) const
{
  abort();  // This shouldn't happen
  return 0;
}

uint32 Type_handler_set::calc_pack_length(uint32 length) const
{
  abort();  // This shouldn't happen
  return 0;
}

uint32 Type_handler_enum::calc_pack_length(uint32 length) const
{
  abort();  // This shouldn't happen
  return 0;
}


/*************************************************************************/
uint Type_handler::calc_key_length(const Column_definition &def) const
{
  DBUG_ASSERT(def.pack_length == calc_pack_length((uint32) def.length));
  return def.pack_length;
}

uint Type_handler_bit::calc_key_length(const Column_definition &def) const
{
  if (f_bit_as_char(def.pack_flag))
    return def.pack_length;
  /* We need one extra byte to store the bits we save among the null bits */
  return def.pack_length + MY_TEST(def.length & 7);
}

uint Type_handler_newdecimal::calc_key_length(const Column_definition &def) const
{
  return def.pack_length;
}

uint
Type_handler_string_result::calc_key_length(const Column_definition &def) const
{
  return (uint) def.length;
}

uint Type_handler_enum::calc_key_length(const Column_definition &def) const
{
  DBUG_ASSERT(def.interval);
  return get_enum_pack_length(def.interval->count);
}

uint Type_handler_set::calc_key_length(const Column_definition &def) const
{
  DBUG_ASSERT(def.interval);
  return get_set_pack_length(def.interval->count);
}

uint Type_handler_blob_common::calc_key_length(const Column_definition &def) const
{
  return 0;
}

/*************************************************************************/
Field *Type_handler::make_and_init_table_field(MEM_ROOT *root,
                                               const LEX_CSTRING *name,
                                               const Record_addr &addr,
                                               const Type_all_attributes &attr,
                                               TABLE *table) const
{
  Field *field= make_table_field(root, name, addr, attr, table->s);
  if (field)
    field->init(table);
  return field;
}


Field *Type_handler_int_result::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const
{
  DBUG_ASSERT(is_unsigned() == attr.unsigned_flag);
  Column_definition_attributes dattr(attr);
  return make_table_field_from_def(share, root, name, addr,
                                   Bit_addr(), &dattr, 0);
}


Field *Type_handler_vers_trx_id::make_table_field(MEM_ROOT *root,
                                               const LEX_CSTRING *name,
                                               const Record_addr &addr,
                                               const Type_all_attributes &attr,
                                               TABLE_SHARE *share) const
{
  DBUG_ASSERT(is_unsigned() == attr.unsigned_flag);
  return new (root)
         Field_vers_trx_id(addr.ptr(), attr.max_char_length(),
                        addr.null_ptr(), addr.null_bit(),
                        Field::NONE, name,
                        0/*zerofill*/, attr.unsigned_flag);
}


Field *
Type_handler_real_result::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const
{
  Column_definition_attributes dattr(attr);
  return make_table_field_from_def(share, root, name, addr,
                                   Bit_addr(), &dattr, 0);
}


Field *
Type_handler_olddecimal::make_table_field(MEM_ROOT *root,
                                          const LEX_CSTRING *name,
                                          const Record_addr &addr,
                                          const Type_all_attributes &attr,
                                          TABLE_SHARE *share) const
{
  /*
    Currently make_table_field() is used for Item purpose only.
    On Item level we have type_handler_newdecimal only.
    For now we have DBUG_ASSERT(0).
    It will be removed when we reuse Type_handler::make_table_field()
    in make_field() in field.cc, to open old tables with old decimal.
  */
  DBUG_ASSERT(0);
  Column_definition_attributes dattr(attr);
  return make_table_field_from_def(share, root, name, addr,
                                   Bit_addr(), &dattr, 0);
}


Field *
Type_handler_newdecimal::make_table_field(MEM_ROOT *root,
                                          const LEX_CSTRING *name,
                                          const Record_addr &addr,
                                          const Type_all_attributes &attr,
                                          TABLE_SHARE *share) const
{
  uint8 dec= (uint8) attr.decimals;
  uint8 intg= (uint8) (attr.decimal_precision() - dec);
  uint32 len= attr.max_char_length();

  /*
    Trying to put too many digits overall in a DECIMAL(prec,dec)
    will always throw a warning. We must limit dec to
    DECIMAL_MAX_SCALE however to prevent an assert() later.
  */

  if (dec > 0)
  {
    signed int overflow;

    dec= MY_MIN(dec, DECIMAL_MAX_SCALE);

    /*
      If the value still overflows the field with the corrected dec,
      we'll throw out decimals rather than integers. This is still
      bad and of course throws a truncation warning.
      +1: for decimal point
      */

    const int required_length=
      my_decimal_precision_to_length(intg + dec, dec, attr.unsigned_flag);

    overflow= required_length - len;

    if (overflow > 0)
      dec= MY_MAX(0, dec - overflow);            // too long, discard fract
    else
      /* Corrected value fits. */
      len= required_length;
  }
  return new (root)
         Field_new_decimal(addr.ptr(), len, addr.null_ptr(), addr.null_bit(),
                           Field::NONE, name,
                           dec, 0/*zerofill*/, attr.unsigned_flag);
}


Field *Type_handler_null::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const

{
  return new (root)
         Field_null(addr.ptr(), attr.max_length,
                    Field::NONE, name, attr.collation.collation);
}


Field *Type_handler_timestamp::make_table_field(MEM_ROOT *root,
                                                const LEX_CSTRING *name,
                                                const Record_addr &addr,
                                                const Type_all_attributes &attr,
                                                TABLE_SHARE *share) const

{
  return new_Field_timestamp(root,
                             addr.ptr(), addr.null_ptr(), addr.null_bit(),
                             Field::NONE, name, share, attr.decimals);
}


Field *Type_handler_timestamp2::make_table_field(MEM_ROOT *root,
                                                 const LEX_CSTRING *name,
                                                 const Record_addr &addr,
                                                 const Type_all_attributes &attr,
                                                 TABLE_SHARE *share) const

{
  /*
    Will be changed to "new Field_timestampf" when we reuse
    make_table_field() for make_field() purposes in field.cc.
  */
  return new_Field_timestamp(root,
                             addr.ptr(), addr.null_ptr(), addr.null_bit(),
                             Field::NONE, name, share, attr.decimals);
}


Field *Type_handler_newdate::make_table_field(MEM_ROOT *root,
                                              const LEX_CSTRING *name,
                                              const Record_addr &addr,
                                              const Type_all_attributes &attr,
                                              TABLE_SHARE *share) const

{
  return new (root)
         Field_newdate(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                       Field::NONE, name);
}


Field *Type_handler_date::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const

{
  /*
    DBUG_ASSERT will be removed when we reuse make_table_field()
    for make_field() in field.cc
  */
  DBUG_ASSERT(0);
  return new (root)
         Field_date(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name);
}


Field *Type_handler_time::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const

{
  return new_Field_time(root,
                        addr.ptr(), addr.null_ptr(), addr.null_bit(),
                        Field::NONE, name, attr.decimals);
}


Field *Type_handler_time2::make_table_field(MEM_ROOT *root,
                                            const LEX_CSTRING *name,
                                            const Record_addr &addr,
                                            const Type_all_attributes &attr,
                                            TABLE_SHARE *share) const


{
  /*
    Will be changed to "new Field_timef" when we reuse
    make_table_field() for make_field() purposes in field.cc.
  */
  return new_Field_time(root,
                        addr.ptr(), addr.null_ptr(), addr.null_bit(),
                        Field::NONE, name, attr.decimals);
}


Field *Type_handler_datetime::make_table_field(MEM_ROOT *root,
                                               const LEX_CSTRING *name,
                                               const Record_addr &addr,
                                               const Type_all_attributes &attr,
                                               TABLE_SHARE *share) const

{
  return new_Field_datetime(root,
                            addr.ptr(), addr.null_ptr(), addr.null_bit(),
                            Field::NONE, name, attr.decimals);
}


Field *Type_handler_datetime2::make_table_field(MEM_ROOT *root,
                                                const LEX_CSTRING *name,
                                                const Record_addr &addr,
                                                const Type_all_attributes &attr,
                                                TABLE_SHARE *share) const
{
  /*
    Will be changed to "new Field_datetimef" when we reuse
    make_table_field() for make_field() purposes in field.cc.
  */
  return new_Field_datetime(root,
                            addr.ptr(), addr.null_ptr(), addr.null_bit(),
                            Field::NONE, name, attr.decimals);
}


Field *Type_handler_bit::make_table_field(MEM_ROOT *root,
                                          const LEX_CSTRING *name,
                                          const Record_addr &addr,
                                          const Type_all_attributes &attr,
                                          TABLE_SHARE *share) const

{
  return new (root)
         Field_bit_as_char(addr.ptr(), attr.max_length,
                           addr.null_ptr(), addr.null_bit(),
                           Field::NONE, name);
}


Field *Type_handler_string::make_table_field(MEM_ROOT *root,
                                             const LEX_CSTRING *name,
                                             const Record_addr &addr,
                                             const Type_all_attributes &attr,
                                             TABLE_SHARE *share) const

{
  return new (root)
         Field_string(addr.ptr(), attr.max_length,
                      addr.null_ptr(), addr.null_bit(),
                      Field::NONE, name, attr.collation);
}


Field *Type_handler_varchar::make_table_field(MEM_ROOT *root,
                                              const LEX_CSTRING *name,
                                              const Record_addr &addr,
                                              const Type_all_attributes &attr,
                                              TABLE_SHARE *share) const

{
  DBUG_ASSERT(HA_VARCHAR_PACKLENGTH(attr.max_length) <=
              MAX_FIELD_VARCHARLENGTH);
  return new (root)
         Field_varstring(addr.ptr(), attr.max_length,
                         HA_VARCHAR_PACKLENGTH(attr.max_length),
                         addr.null_ptr(), addr.null_bit(),
                         Field::NONE, name,
                         share, attr.collation);
}


Field *Type_handler_tiny_blob::make_table_field(MEM_ROOT *root,
                                                const LEX_CSTRING *name,
                                                const Record_addr &addr,
                                                const Type_all_attributes &attr,
                                                TABLE_SHARE *share) const

{
  return new (root)
         Field_blob(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name, share,
                    1, attr.collation);
}


Field *Type_handler_blob::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const

{
  return new (root)
         Field_blob(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name, share,
                    2, attr.collation);
}


Field *
Type_handler_medium_blob::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const

{
  return new (root)
         Field_blob(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name, share,
                    3, attr.collation);
}


Field *Type_handler_long_blob::make_table_field(MEM_ROOT *root,
                                                const LEX_CSTRING *name,
                                                const Record_addr &addr,
                                                const Type_all_attributes &attr,
                                                TABLE_SHARE *share) const

{
  return new (root)
         Field_blob(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name, share,
                    4, attr.collation);
}


Field *Type_handler_enum::make_table_field(MEM_ROOT *root,
                                           const LEX_CSTRING *name,
                                           const Record_addr &addr,
                                           const Type_all_attributes &attr,
                                           TABLE_SHARE *share) const
{
  const TYPELIB *typelib= attr.get_typelib();
  DBUG_ASSERT(typelib);
  return new (root)
         Field_enum(addr.ptr(), attr.max_length,
                    addr.null_ptr(), addr.null_bit(),
                    Field::NONE, name,
                    get_enum_pack_length(typelib->count), typelib,
                    attr.collation);
}


Field *Type_handler_set::make_table_field(MEM_ROOT *root,
                                          const LEX_CSTRING *name,
                                          const Record_addr &addr,
                                          const Type_all_attributes &attr,
                                          TABLE_SHARE *share) const

{
  const TYPELIB *typelib= attr.get_typelib();
  DBUG_ASSERT(typelib);
  return new (root)
         Field_set(addr.ptr(), attr.max_length,
                   addr.null_ptr(), addr.null_bit(),
                   Field::NONE, name,
                   get_enum_pack_length(typelib->count), typelib,
                   attr.collation);
}


/*************************************************************************/

Field *Type_handler_float::make_schema_field(MEM_ROOT *root, TABLE *table,
                                             const Record_addr &addr,
                                             const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
     Field_float(addr.ptr(), def.char_length(),
                  addr.null_ptr(), addr.null_bit(),
                  Field::NONE, &name,
                  (uint8) NOT_FIXED_DEC,
                  0/*zerofill*/, def.unsigned_flag());
}


Field *Type_handler_double::make_schema_field(MEM_ROOT *root, TABLE *table,
                                              const Record_addr &addr,
                                              const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
     Field_double(addr.ptr(), def.char_length(),
                  addr.null_ptr(), addr.null_bit(),
                  Field::NONE, &name,
                  (uint8) NOT_FIXED_DEC,
                  0/*zerofill*/, def.unsigned_flag());
}


Field *Type_handler_decimal_result::make_schema_field(MEM_ROOT *root,
                                                      TABLE *table,
                                                      const Record_addr &addr,
                                                      const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  uint dec= def.decimal_scale();
  uint prec= def.decimal_precision();
  DBUG_ASSERT(dec <= DECIMAL_MAX_SCALE);
  uint32 len= my_decimal_precision_to_length(prec, dec, def.unsigned_flag());
  return new (root)
     Field_new_decimal(addr.ptr(), len, addr.null_ptr(), addr.null_bit(),
                       Field::NONE, &name,
                       (uint8) dec, 0/*zerofill*/, def.unsigned_flag());
}


Field *Type_handler_blob_common::make_schema_field(MEM_ROOT *root, TABLE *table,
                                                   const Record_addr &addr,
                                                   const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
     Field_blob(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                Field::NONE, &name, table->s,
                length_bytes(),
                &my_charset_bin);
}


Field *Type_handler_varchar::make_schema_field(MEM_ROOT *root, TABLE *table,
                                               const Record_addr &addr,
                                               const ST_FIELD_INFO &def) const
{
  DBUG_ASSERT(def.char_length());
  LEX_CSTRING name= def.name();
  uint32 octet_length= (uint32) def.char_length() * 3;
  if (octet_length > MAX_FIELD_VARCHARLENGTH)
  {
    Field *field= new (root)
      Field_blob(addr.ptr(), addr.null_ptr(), addr.null_bit(), Field::NONE,
                 &name, table->s, 4, system_charset_info);
    if (field)
      field->field_length= octet_length;
    return field;
  }
  else
  {
    return new (root)
      Field_varstring(addr.ptr(), octet_length,
                      HA_VARCHAR_PACKLENGTH(octet_length),
                      addr.null_ptr(), addr.null_bit(),
                      Field::NONE, &name,
                      table->s, system_charset_info);
  }
}


Field *Type_handler_tiny::make_schema_field(MEM_ROOT *root, TABLE *table,
                                            const Record_addr &addr,
                                            const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
           Field_tiny(addr.ptr(), def.char_length(),
                      addr.null_ptr(), addr.null_bit(), Field::NONE, &name,
                      0/*zerofill*/, def.unsigned_flag());
}


Field *Type_handler_short::make_schema_field(MEM_ROOT *root, TABLE *table,
                                             const Record_addr &addr,
                                             const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
           Field_short(addr.ptr(), def.char_length(),
                       addr.null_ptr(), addr.null_bit(), Field::NONE, &name,
                       0/*zerofill*/, def.unsigned_flag());
}


Field *Type_handler_long::make_schema_field(MEM_ROOT *root, TABLE *table,
                                            const Record_addr &addr,
                                            const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
           Field_long(addr.ptr(), def.char_length(),
                      addr.null_ptr(), addr.null_bit(), Field::NONE, &name,
                      0/*zerofill*/, def.unsigned_flag());
}


Field *Type_handler_longlong::make_schema_field(MEM_ROOT *root, TABLE *table,
                                                const Record_addr &addr,
                                                const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
           Field_longlong(addr.ptr(), def.char_length(),
                          addr.null_ptr(), addr.null_bit(), Field::NONE, &name,
                          0/*zerofill*/, def.unsigned_flag());
}


Field *Type_handler_date_common::make_schema_field(MEM_ROOT *root, TABLE *table,
                                                   const Record_addr &addr,
                                                   const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root)
           Field_newdate(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                         Field::NONE, &name);
}


Field *Type_handler_time_common::make_schema_field(MEM_ROOT *root, TABLE *table,
                                                   const Record_addr &addr,
                                                   const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new_Field_time(root,
                        addr.ptr(), addr.null_ptr(), addr.null_bit(),
                        Field::NONE, &name, def.fsp());
}


Field *Type_handler_datetime_common::make_schema_field(MEM_ROOT *root,
                                                       TABLE *table,
                                                       const Record_addr &addr,
                                                       const ST_FIELD_INFO &def) const
{
  LEX_CSTRING name= def.name();
  return new (root) Field_datetimef(addr.ptr(),
                                    addr.null_ptr(), addr.null_bit(),
                                    Field::NONE, &name, def.fsp());
}


/*************************************************************************/

/*
   If length is not specified for a varchar parameter, set length to the
   maximum length of the actual argument. Goals are:
   - avoid to allocate too much unused memory for m_var_table
   - allow length check inside the callee rather than during copy of
     returned values in output variables.
   - allow varchar parameter size greater than 4000
   Default length has been stored in "decimal" member during parse.
*/
bool Type_handler_varchar::adjust_spparam_type(Spvar_definition *def,
                                               Item *from) const
{
  if (def->decimals)
  {
    uint def_max_char_length= MAX_FIELD_VARCHARLENGTH / def->charset->mbmaxlen;
    uint arg_max_length= from->max_char_length();
    set_if_smaller(arg_max_length, def_max_char_length);
    def->length= arg_max_length > 0 ? arg_max_length : def->decimals;
    def->create_length_to_internal_length_string();
  }
  return false;
}

/*************************************************************************/

uint32 Type_handler_decimal_result::max_display_length(const Item *item) const
{
  return item->max_length;
}


uint32 Type_handler_temporal_result::max_display_length(const Item *item) const
{
  return item->max_length;
}

uint32 Type_handler_string_result::max_display_length(const Item *item) const
{
  return item->max_length;
}


uint32 Type_handler_year::max_display_length(const Item *item) const
{
  return item->max_length;
}


uint32 Type_handler_bit::max_display_length(const Item *item) const
{
  return item->max_length;
}

/*************************************************************************/

uint32 
Type_handler_decimal_result::Item_decimal_notation_int_digits(const Item *item)
                                                              const
{
  return item->decimal_int_part();
}


uint32 
Type_handler_temporal_result::Item_decimal_notation_int_digits(const Item *item)
                                                               const
{
  return item->decimal_int_part();
}


uint32 
Type_handler_bit::Item_decimal_notation_int_digits(const Item *item)
                                                              const
{
  return Bit_decimal_notation_int_digits_by_nbits(item->max_length);
}


uint32
Type_handler_general_purpose_int::Item_decimal_notation_int_digits(
                                                   const Item *item) const
{
  return type_limits_int()->precision();
}

/*************************************************************************/

/*
    Binary to Decimal digits ratio converges to log2(10) thus using 3 as
    a divisor.
*/
uint32
Type_handler_bit::Bit_decimal_notation_int_digits_by_nbits(uint nbits)
{
  DBUG_ASSERT(nbits > 0);
  DBUG_ASSERT(nbits <= 64);
  set_if_smaller(nbits, 64); // Safety
  static uint ndigits[65]=
  {0,
   1,1,1,2,2,2,3,3,         // 1..8 bits
   3,4,4,4,4,5,5,5,         // 9..16 bits
   6,6,6,7,7,7,7,8,         // 17..24 bits
   8,8,9,9,9,10,10,10,      // 25..32 bits
   10,11,11,11,12,12,12,13, // 33..40 bits
   13,13,13,14,14,14,15,15, // 41..48 bits
   15,16,16,16,16,17,17,17, // 49..56 bits
   18,18,18,19,19,19,19,20  // 57..64 bits
  };
  return ndigits[nbits];
}

/*************************************************************************/

void Type_handler_row::Item_update_null_value(Item *item) const
{
  DBUG_ASSERT(0);
  item->null_value= true;
}


void Type_handler_time_common::Item_update_null_value(Item *item) const
{
  MYSQL_TIME ltime;
  THD *thd= current_thd;
  (void) item->get_date(thd, &ltime, Time::Options(TIME_TIME_ONLY, thd));
}


void Type_handler_temporal_with_date::Item_update_null_value(Item *item) const
{
  MYSQL_TIME ltime;
  THD *thd= current_thd;
  (void) item->get_date(thd, &ltime, Datetime::Options(thd));
}

bool
Type_handler_timestamp_common::
Column_definition_set_attributes(THD *thd,
                                 Column_definition *def,
                                 const Lex_field_type_st &attr,
                                 column_definition_type_t type) const
{
  Type_handler::Column_definition_set_attributes(thd, def, attr, type);
  if (!opt_explicit_defaults_for_timestamp)
    def->flags|= NOT_NULL_FLAG;
  return false;
}

void Type_handler_string_result::Item_update_null_value(Item *item) const
{
  StringBuffer<MAX_FIELD_WIDTH> tmp;
  (void) item->val_str(&tmp);
}


void Type_handler_real_result::Item_update_null_value(Item *item) const
{
  (void) item->val_real();
}


void Type_handler_decimal_result::Item_update_null_value(Item *item) const
{
  my_decimal tmp;
  (void) item->val_decimal(&tmp);
}


void Type_handler_int_result::Item_update_null_value(Item *item) const
{
  (void) item->val_int();
}


void Type_handler_bool::Item_update_null_value(Item *item) const
{
  (void) item->val_bool();
}


/*************************************************************************/

int Type_handler_time_common::Item_save_in_field(Item *item, Field *field,
                                                 bool no_conversions) const
{
  return item->save_time_in_field(field, no_conversions);
}

int Type_handler_temporal_with_date::Item_save_in_field(Item *item,
                                                        Field *field,
                                                        bool no_conversions)
                                                        const
{
  return item->save_date_in_field(field, no_conversions);
}


int Type_handler_timestamp_common::Item_save_in_field(Item *item,
                                                      Field *field,
                                                      bool no_conversions)
                                                      const
{
  Timestamp_or_zero_datetime_native_null tmp(field->table->in_use, item, true);
  if (tmp.is_null())
    return set_field_to_null_with_conversions(field, no_conversions);
  return tmp.save_in_field(field, item->decimals);
}


int Type_handler_string_result::Item_save_in_field(Item *item, Field *field,
                                                   bool no_conversions) const
{
  return item->save_str_in_field(field, no_conversions);
}


int Type_handler_real_result::Item_save_in_field(Item *item, Field *field,
                                                 bool no_conversions) const
{
  return item->save_real_in_field(field, no_conversions);
}


int Type_handler_decimal_result::Item_save_in_field(Item *item, Field *field,
                                                    bool no_conversions) const
{
  return item->save_decimal_in_field(field, no_conversions);
}


int Type_handler_int_result::Item_save_in_field(Item *item, Field *field,
                                                bool no_conversions) const
{
  return item->save_int_in_field(field, no_conversions);
}


/***********************************************************************/

bool Type_handler_row::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_row(thd);
}

bool Type_handler_int_result::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_int(thd);
}

bool Type_handler_real_result::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_real(thd);
}

bool Type_handler_decimal_result::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_decimal(thd);
}

bool Type_handler_string_result::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_string(thd);
}

bool Type_handler_time_common::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_time(thd);
}

bool
Type_handler_temporal_with_date::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_datetime(thd);
}

bool
Type_handler_timestamp_common::
set_comparator_func(THD *thd, Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_native(thd);
}


/*************************************************************************/

bool Type_handler_temporal_result::
       can_change_cond_ref_to_const(Item_bool_func2 *target,
                                    Item *target_expr, Item *target_value,
                                    Item_bool_func2 *source,
                                    Item *source_expr, Item *source_const)
                                    const
{
  if (source->compare_type_handler()->cmp_type() != TIME_RESULT)
    return false;

  /*
    Can't rewrite:
      WHERE COALESCE(time_column)='00:00:00'
        AND COALESCE(time_column)=DATE'2015-09-11'
    to
      WHERE DATE'2015-09-11'='00:00:00'
        AND COALESCE(time_column)=DATE'2015-09-11'
    because the left part will erroneously try to parse '00:00:00'
    as DATE, not as TIME.

    TODO: It could still be rewritten to:
      WHERE DATE'2015-09-11'=TIME'00:00:00'
        AND COALESCE(time_column)=DATE'2015-09-11'
    i.e. we need to replace both target_expr and target_value
    at the same time. This is not supported yet.
  */
  return target_value->cmp_type() == TIME_RESULT;
}


bool Type_handler_string_result::
       can_change_cond_ref_to_const(Item_bool_func2 *target,
                                    Item *target_expr, Item *target_value,
                                    Item_bool_func2 *source,
                                    Item *source_expr, Item *source_const)
                                    const
{
  if (source->compare_type_handler()->cmp_type() != STRING_RESULT)
    return false;
  /*
    In this example:
      SET NAMES utf8 COLLATE utf8_german2_ci;
      DROP TABLE IF EXISTS t1;
      CREATE TABLE t1 (a CHAR(10) CHARACTER SET utf8);
      INSERT INTO t1 VALUES ('o-umlaut'),('oe');
      SELECT * FROM t1 WHERE a='oe' COLLATE utf8_german2_ci AND a='oe';

    the query should return only the row with 'oe'.
    It should not return 'o-umlaut', because 'o-umlaut' does not match
    the right part of the condition: a='oe'
    ('o-umlaut' is not equal to 'oe' in utf8mb3_general_ci,
     which is the collation of the field "a").

    If we change the right part from:
       ... AND a='oe'
    to
       ... AND 'oe' COLLATE utf8_german2_ci='oe'
    it will be evalulated to TRUE and removed from the condition,
    so the overall query will be simplified to:

      SELECT * FROM t1 WHERE a='oe' COLLATE utf8_german2_ci;

    which will erroneously start to return both 'oe' and 'o-umlaut'.
    So changing "expr" to "const" is not possible if the effective
    collations of "target" and "source" are not exactly the same.

    Note, the code before the fix for MDEV-7152 only checked that
    collations of "source_const" and "target_value" are the same.
    This was not enough, as the bug report demonstrated.
  */
  return
    target->compare_collation() == source->compare_collation() &&
    target_value->collation.collation == source_const->collation.collation;
}


bool Type_handler_numeric::
       can_change_cond_ref_to_const(Item_bool_func2 *target,
                                    Item *target_expr, Item *target_value,
                                    Item_bool_func2 *source,
                                    Item *source_expr, Item *source_const)
                                    const
{
  /*
   The collations of "target" and "source" do not make sense for numeric
   data types.
  */
  return target->compare_type_handler() == source->compare_type_handler();
}


/*************************************************************************/

Item_cache *
Type_handler_row::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_row(thd);
}

Item_cache *
Type_handler_int_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_int(thd, item->type_handler());
}

Item_cache *
Type_handler_year::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_year(thd, item->type_handler());
}

Item_cache *
Type_handler_double::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_double(thd);
}

Item_cache *
Type_handler_float::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_float(thd);
}

Item_cache *
Type_handler_decimal_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_decimal(thd);
}

Item_cache *
Type_handler_string_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_str(thd, item);
}

Item_cache *
Type_handler_timestamp_common::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_timestamp(thd);
}

Item_cache *
Type_handler_datetime_common::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_datetime(thd);
}

Item_cache *
Type_handler_time_common::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_time(thd);
}

Item_cache *
Type_handler_date_common::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_date(thd);
}


/*************************************************************************/

Item_copy *
Type_handler::create_item_copy(THD *thd, Item *item) const
{
  return new (thd->mem_root) Item_copy_string(thd, item);
}


Item_copy *
Type_handler_timestamp_common::create_item_copy(THD *thd, Item *item) const
{
  return new (thd->mem_root) Item_copy_timestamp(thd, item);
}

/*************************************************************************/

bool Type_handler_int_result::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  bool unsigned_flag= items[0]->unsigned_flag;
  for (uint i= 1; i < nitems; i++)
  {
    if (unsigned_flag != items[i]->unsigned_flag)
    {
      // Convert a mixture of signed and unsigned int to decimal
      handler->set_handler(&type_handler_newdecimal);
      func->aggregate_attributes_decimal(items, nitems, false);
      return false;
    }
  }
  func->aggregate_attributes_int(items, nitems);
  handler->set_handler(func->unsigned_flag ?
                       handler->type_handler()->type_handler_unsigned() :
                       handler->type_handler()->type_handler_signed());
  return false;
}


bool Type_handler_real_result::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_real(items, nitems);
  return false;
}


bool Type_handler_decimal_result::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  uint unsigned_count= func->count_unsigned(items, nitems);
  func->aggregate_attributes_decimal(items, nitems, unsigned_count == nitems);
  return false;
}


bool Type_handler_string_result::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &func_name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  return func->aggregate_attributes_string(func_name, items, nitems);
}



/*
  We can have enum/set type after merging only if we have one enum|set
  field (or MIN|MAX(enum|set field)) and number of NULL fields
*/
bool Type_handler_typelib::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &func_name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  const TYPELIB *typelib= NULL;
  for (uint i= 0; i < nitems; i++)
  {
    const TYPELIB *typelib2;
    if ((typelib2= items[i]->get_typelib()))
    {
      if (typelib)
      {
        /*
          Two ENUM/SET columns found. We convert such combinations to VARCHAR.
          This may change in the future to preserve ENUM/SET
          if typelib definitions are equal.
        */
        handler->set_handler(&type_handler_varchar);
        return func->aggregate_attributes_string(func_name, items, nitems);
      }
      typelib= typelib2;
    }
  }
  DBUG_ASSERT(typelib); // There must be at least one typelib
  func->set_typelib(typelib);
  return func->aggregate_attributes_string(func_name, items, nitems);
}


bool Type_handler_blob_common::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &func_name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  if (func->aggregate_attributes_string(func_name, items, nitems))
    return true;
  handler->set_handler(blob_type_handler(func->max_length));
  return false;
}


bool Type_handler_date_common::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  func->fix_attributes_date();
  return false;
}


bool Type_handler_time_common::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_temporal(MIN_TIME_WIDTH, items, nitems);
  return false;
}


bool Type_handler_datetime_common::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_temporal(MAX_DATETIME_WIDTH, items, nitems);
  return false;
}


bool Type_handler_timestamp_common::
       Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_temporal(MAX_DATETIME_WIDTH, items, nitems);
  return false;
}

/*************************************************************************/

bool Type_handler::
       Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const
{
  /*
    Aggregating attributes for LEAST/GREATES is exactly the same
    with aggregating for CASE-alike functions (e.g. COALESCE)
    for the majority of data type handlers.
  */
  return Item_hybrid_func_fix_attributes(thd, func->func_name_cstring(),
                                         func, func, items, nitems);
}


bool Type_handler_temporal_result::
       Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const
{
  DBUG_ASSERT(func->field_type() != MYSQL_TYPE_DATE);
  bool rc= Type_handler::Item_func_min_max_fix_attributes(thd, func,
                                                          items, nitems);
  bool is_time= func->field_type() == MYSQL_TYPE_TIME;
  func->decimals= 0;
  for (uint i= 0; i < nitems; i++)
  {
    uint deci= is_time ? items[i]->time_precision(thd) :
                         items[i]->datetime_precision(thd);
    set_if_bigger(func->decimals, deci);
  }

  if (rc || func->maybe_null())
    return rc;
  /*
    LEAST/GREATES(non-temporal, temporal) can return NULL.
    CAST functions Item_{time|datetime|date}_typecast always set maybe_full
    to true. Here we try to detect nullability more thoroughly.
    Perhaps CAST functions should also reuse this idea eventually.
  */
  const Type_handler *hf= func->type_handler();
  for (uint i= 0; i < nitems; i++)
  {
    /*
      If items[i] does not need conversion to the current temporal data
      type, then we trust items[i]->maybe_null, which was already ORred
      to func->maybe_null in the argument loop in fix_fields().
      If items[i] requires conversion to the current temporal data type,
      then conversion can fail and return NULL even for NOT NULL items.
    */
    const Type_handler *ha= items[i]->type_handler();
    if (hf == ha)
      continue; // No conversion.
    if (ha->cmp_type() != TIME_RESULT)
    {
      // Conversion from non-temporal is not safe
      func->set_maybe_null();
      break;
    }
    timestamp_type tf= hf->mysql_timestamp_type();
    timestamp_type ta= ha->mysql_timestamp_type();
    if (tf == ta ||
        (tf == MYSQL_TIMESTAMP_DATETIME && ta == MYSQL_TIMESTAMP_DATE))
    {
      /*
        If handlers have the same mysql_timestamp_type(),
        then conversion is NULL safe. Conversion from DATE to DATETIME
        is also safe. This branch includes data type pairs:
        Function return type Argument type  Comment
        -------------------- -------------  -------------
        TIMESTAMP            TIMESTAMP      no conversion
        TIMESTAMP            DATETIME       not possible
        TIMESTAMP            DATE           not possible
        DATETIME             DATETIME       no conversion
        DATETIME             TIMESTAMP      safe conversion
        DATETIME             DATE           safe conversion
        TIME                 TIME           no conversion

        Note, a function cannot return TIMESTAMP if it has non-TIMESTAMP
        arguments (it would return DATETIME in such case).
      */
      DBUG_ASSERT(hf->field_type() != MYSQL_TYPE_TIMESTAMP || tf == ta);
      continue;
    }
    /*
      Here we have the following data type pairs that did not match
      the condition above:

      Function return type Argument type Comment
      -------------------- ------------- -------
      TIMESTAMP            TIME          Not possible
      DATETIME             TIME          depends on OLD_MODE_ZERO_DATE_TIME_CAST
      TIME                 TIMESTAMP     Not possible
      TIME                 DATETIME      Not possible
      TIME                 DATE          Not possible

      Most pairs are not possible, because the function data type
      would be DATETIME (according to LEAST/GREATEST aggregation rules).
      Conversion to DATETIME from TIME is not safe when
      OLD_MODE_ZERO_DATE_TIME_CAST is set:
      - negative TIME values cannot be converted to not-NULL DATETIME values
      - TIME values can produce DATETIME values that do not pass
        NO_ZERO_DATE and NO_ZERO_IN_DATE tests.
    */
    DBUG_ASSERT(hf->field_type() == MYSQL_TYPE_DATETIME);
    if (!(thd->variables.old_behavior & OLD_MODE_ZERO_DATE_TIME_CAST))
      continue;
    func->set_maybe_null();
    break;
  }
  return rc;
}


bool Type_handler_date_common::
       Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const
{
  func->fix_attributes_date();
  if (func->maybe_null())
    return false;
  /*
    We cannot trust the generic maybe_null value calculated during
    fix_fields().
    If a conversion from non-temoral types to DATE happens,
    then the result can be NULL (even if all arguments are not NULL).
  */
  for (uint i= 0; i < nitems; i++)
  {
    if (items[i]->type_handler()->cmp_type() != TIME_RESULT)
    {
      func->set_maybe_null();
      break;
    }
  }
  return false;
}


bool Type_handler_real_result::
       Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const
{
  /*
    DOUBLE is an exception and aggregates attributes differently
    for LEAST/GREATEST vs CASE-alike functions. See the comment in
    Item_func_min_max::aggregate_attributes_real().
  */
  func->aggregate_attributes_real(items, nitems);
  return false;
}

/*************************************************************************/

bool Type_handler_int_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  /*
    "this" is equal func->args[0]->type_handler() here, e.g. for MIN()/MAX().
    func->unsigned_flag is not reliably set yet.
    It will be set by the call below (copied from args[0]).
  */
  const Type_handler *h= is_unsigned()
                           ? (Type_handler *)&type_handler_ulonglong
                           : (Type_handler *)&type_handler_slonglong;
  return func->fix_length_and_dec_numeric(h);
}


bool Type_handler_bool::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  return func->fix_length_and_dec_numeric(&type_handler_bool);
}


bool Type_handler_real_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  (void) func->fix_length_and_dec_numeric(&type_handler_double);
  func->max_length= func->float_length(func->decimals);
  return false;
}


bool Type_handler_decimal_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  return func->fix_length_and_dec_numeric(&type_handler_newdecimal);
}


bool Type_handler_string_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  return func->fix_length_and_dec_string();
}


bool Type_handler_temporal_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  return func->fix_length_and_dec_generic();
}


/*************************************************************************/

bool Type_handler_int_result::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_decimal_result::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_real_result::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_string_result::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  item->fix_length_and_dec_double();
  return false;
}




/*************************************************************************/

bool Type_handler_int_result::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_decimal_result::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_real_result::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_string_result::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  item->fix_length_and_dec_double();
  return false;
}




/*************************************************************************/

bool Type_handler_int_result::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_decimal_result::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_real_result::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_string_result::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


/*************************************************************************/

bool Type_handler_real_result::Item_val_bool(Item *item) const
{
  return item->val_real() != 0.0;
}

bool Type_handler_int_result::Item_val_bool(Item *item) const
{
  return item->val_int() != 0;
}

bool Type_handler_temporal_result::Item_val_bool(Item *item) const
{
  return item->val_real() != 0.0;
}

bool Type_handler_string_result::Item_val_bool(Item *item) const
{
  return item->val_real() != 0.0;
}


/*************************************************************************/


bool Type_handler::Item_get_date_with_warn(THD *thd, Item *item,
                                           MYSQL_TIME *ltime,
                                           date_mode_t fuzzydate) const
{
  const TABLE_SHARE *s= item->field_table_or_null();
  Temporal::Warn_push warn(thd, s ? s->db.str : nullptr,
                           s ? s->table_name.str : nullptr,
                           item->field_name_or_null(), ltime, fuzzydate);
  Item_get_date(thd, item, &warn, ltime, fuzzydate);
  return ltime->time_type < 0;
}


bool Type_handler::Item_func_hybrid_field_type_get_date_with_warn(THD *thd,
                                              Item_func_hybrid_field_type *item,
                                              MYSQL_TIME *ltime,
                                              date_mode_t mode) const
{
  const TABLE_SHARE *s= item->field_table_or_null();
  Temporal::Warn_push warn(thd, s ? s->db.str : nullptr,
                           s ? s->table_name.str : nullptr,
                           item->field_name_or_null(), ltime, mode);
  Item_func_hybrid_field_type_get_date(thd, item, &warn, ltime, mode);
  return ltime->time_type < 0;
}


/************************************************************************/
void Type_handler_decimal_result::Item_get_date(THD *thd, Item *item,
                                                Temporal::Warn *warn,
                                                MYSQL_TIME *ltime,
                                                date_mode_t fuzzydate) const
{
  new(ltime) Temporal_hybrid(thd, warn, VDec(item).ptr(), fuzzydate);
}


void Type_handler_int_result::Item_get_date(THD *thd, Item *item,
                                            Temporal::Warn *warn,
                                            MYSQL_TIME *to,
                                            date_mode_t mode) const
{
  new(to) Temporal_hybrid(thd, warn, item->to_longlong_hybrid_null(), mode);
}


void Type_handler_year::Item_get_date(THD *thd, Item *item,
                                      Temporal::Warn *warn,
                                      MYSQL_TIME *ltime,
                                      date_mode_t fuzzydate) const
{
  VYear year(item);
  DBUG_ASSERT(!year.truncated());
  Longlong_hybrid_null nr(Longlong_null(year.to_YYYYMMDD(), year.is_null()),
                          item->unsigned_flag);
  new(ltime) Temporal_hybrid(thd, warn, nr, fuzzydate);
}


void Type_handler_real_result::Item_get_date(THD *thd, Item *item,
                                             Temporal::Warn *warn,
                                             MYSQL_TIME *ltime,
                                             date_mode_t fuzzydate) const
{
  new(ltime) Temporal_hybrid(thd, warn, item->to_double_null(), fuzzydate);
}


void Type_handler_string_result::Item_get_date(THD *thd, Item *item,
                                               Temporal::Warn *warn,
                                               MYSQL_TIME *ltime,
                                               date_mode_t mode) const
{
  StringBuffer<40> tmp;
  new(ltime) Temporal_hybrid(thd, warn, item->val_str(&tmp), mode);
}


void Type_handler_temporal_result::Item_get_date(THD *thd, Item *item,
                                                 Temporal::Warn *warn,
                                                 MYSQL_TIME *ltime,
                                                 date_mode_t fuzzydate) const
{
  DBUG_ASSERT(0); // Temporal type items must implement native get_date()
  item->null_value= true;
  set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
}


/*************************************************************************/

longlong Type_handler_real_result::
           Item_val_int_signed_typecast(Item *item) const
{
  return item->val_int_signed_typecast_from_real();
}

longlong Type_handler_int_result::
           Item_val_int_signed_typecast(Item *item) const
{
  return item->val_int();
}

longlong Type_handler_decimal_result::
           Item_val_int_signed_typecast(Item *item) const
{
  return VDec(item).to_longlong(false);
}

longlong Type_handler_temporal_result::
           Item_val_int_signed_typecast(Item *item) const
{
  return item->val_int();
}

longlong Type_handler_string_result::
           Item_val_int_signed_typecast(Item *item) const
{
  return item->val_int_signed_typecast_from_str();
}

/*************************************************************************/

longlong Type_handler_real_result::
           Item_val_int_unsigned_typecast(Item *item) const
{
  return item->val_int_unsigned_typecast_from_real();
}

longlong Type_handler_int_result::
           Item_val_int_unsigned_typecast(Item *item) const
{
  return item->val_int_unsigned_typecast_from_int();
}

longlong Type_handler_temporal_result::
           Item_val_int_unsigned_typecast(Item *item) const
{
  return item->val_int_unsigned_typecast_from_int();
}

longlong Type_handler_time_common::
           Item_val_int_unsigned_typecast(Item *item) const
{
  /*
    TODO: this should eventually be fixed to do rounding
    when TIME_ROUND_FRACTIONAL is enabled, together with
    Field_{tiny|short|long|longlong}::store_time_dec().
    See MDEV-19502.
  */
  THD *thd= current_thd;
  Time tm(thd, item);
  DBUG_ASSERT(!tm.is_valid_time() == item->null_value);
  if (!tm.is_valid_time())
    return 0;
  longlong res= tm.to_longlong();
  if (res < 0)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_DATA_OVERFLOW, ER_THD(thd, ER_DATA_OVERFLOW),
                        ErrConvTime(tm.get_mysql_time()).ptr(),
                        "UNSIGNED BIGINT");
    return 0;
  }
  return res;
}

longlong Type_handler_string_result::
           Item_val_int_unsigned_typecast(Item *item) const
{
  return item->val_int_unsigned_typecast_from_str();
}

/*************************************************************************/

String *
Type_handler_real_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                      String *str) const
{
  return item->val_str_ascii_from_val_real(str);
}


String *
Type_handler_decimal_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                         String *str) const
{
  return item->val_str_ascii_from_val_real(str);
}


String *
Type_handler_int_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                     String *str) const
{
  return item->val_str_ascii_from_val_int(str);
}


String *
Type_handler_temporal_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                          String *str) const
{
  return item->val_str_ascii_from_val_str(str);
}


String *
Type_handler_string_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                        String *str) const
{
  return item->val_str_ascii_from_val_str(str);
}

/***************************************************************************/

String *
Type_handler_decimal_result::Item_func_hybrid_field_type_val_str(
                                              Item_func_hybrid_field_type *item,
                                              String *str) const
{
  return VDec_op(item).to_string_round(str, item->decimals);
}


double
Type_handler_decimal_result::Item_func_hybrid_field_type_val_real(
                                              Item_func_hybrid_field_type *item)
                                              const
{
  return VDec_op(item).to_double();
}


longlong
Type_handler_decimal_result::Item_func_hybrid_field_type_val_int(
                                              Item_func_hybrid_field_type *item)
                                              const
{
  return VDec_op(item).to_longlong(item->unsigned_flag);
}


my_decimal *
Type_handler_decimal_result::Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *item,
                                              my_decimal *dec) const
{
  return VDec_op(item).to_decimal(dec);
}


void
Type_handler_decimal_result::Item_func_hybrid_field_type_get_date(
                                             THD *thd,
                                             Item_func_hybrid_field_type *item,
                                             Temporal::Warn *warn,
                                             MYSQL_TIME *ltime,
                                             date_mode_t fuzzydate) const
{
  new (ltime) Temporal_hybrid(thd, warn, VDec_op(item).ptr(), fuzzydate);
}


void
Type_handler_year::Item_func_hybrid_field_type_get_date(
                                             THD *thd,
                                             Item_func_hybrid_field_type *item,
                                             Temporal::Warn *warn,
                                             MYSQL_TIME *ltime,
                                             date_mode_t fuzzydate) const
{
  VYear_op year(item);
  DBUG_ASSERT(!year.truncated());
  Longlong_hybrid_null nr(Longlong_null(year.to_YYYYMMDD(), year.is_null()),
                          item->unsigned_flag);
  new(ltime) Temporal_hybrid(thd, warn, nr, fuzzydate);
}


/***************************************************************************/


String *
Type_handler_int_result::Item_func_hybrid_field_type_val_str(
                                          Item_func_hybrid_field_type *item,
                                          String *str) const
{
  return item->val_str_from_int_op(str);
}


double
Type_handler_int_result::Item_func_hybrid_field_type_val_real(
                                          Item_func_hybrid_field_type *item)
                                          const
{
  return item->val_real_from_int_op();
}


longlong
Type_handler_int_result::Item_func_hybrid_field_type_val_int(
                                          Item_func_hybrid_field_type *item)
                                          const
{
  return item->val_int_from_int_op();
}


my_decimal *
Type_handler_int_result::Item_func_hybrid_field_type_val_decimal(
                                          Item_func_hybrid_field_type *item,
                                          my_decimal *dec) const
{
  return item->val_decimal_from_int_op(dec);
}


void
Type_handler_int_result::Item_func_hybrid_field_type_get_date(
                                          THD *thd,
                                          Item_func_hybrid_field_type *item,
                                          Temporal::Warn *warn,
                                          MYSQL_TIME *to,
                                          date_mode_t mode) const
{
  new(to) Temporal_hybrid(thd, warn, item->to_longlong_hybrid_null_op(), mode);
}


/***************************************************************************/

String *
Type_handler_double::Item_func_hybrid_field_type_val_str(
                                           Item_func_hybrid_field_type *item,
                                           String *str) const
{
  return item->val_str_from_real_op(str);
}

String *
Type_handler_float::Item_func_hybrid_field_type_val_str(
                                           Item_func_hybrid_field_type *item,
                                           String *str) const
{
  Float nr(item->real_op());
  if (item->null_value)
    return 0;
  nr.to_string(str, item->decimals);
  return str;
}

double
Type_handler_real_result::Item_func_hybrid_field_type_val_real(
                                           Item_func_hybrid_field_type *item)
                                           const
{
  return item->val_real_from_real_op();
}


longlong
Type_handler_real_result::Item_func_hybrid_field_type_val_int(
                                           Item_func_hybrid_field_type *item)
                                           const
{
  return item->val_int_from_real_op();
}


my_decimal *
Type_handler_real_result::Item_func_hybrid_field_type_val_decimal(
                                           Item_func_hybrid_field_type *item,
                                           my_decimal *dec) const
{
  return item->val_decimal_from_real_op(dec);
}


void
Type_handler_real_result::Item_func_hybrid_field_type_get_date(
                                             THD *thd,
                                             Item_func_hybrid_field_type *item,
                                             Temporal::Warn *warn,
                                             MYSQL_TIME *to,
                                             date_mode_t mode) const
{
  new(to) Temporal_hybrid(thd, warn, item->to_double_null_op(), mode);
}


/***************************************************************************/

String *
Type_handler_temporal_result::Item_func_hybrid_field_type_val_str(
                                        Item_func_hybrid_field_type *item,
                                        String *str) const
{
  return item->val_str_from_date_op(str);
}


double
Type_handler_temporal_result::Item_func_hybrid_field_type_val_real(
                                        Item_func_hybrid_field_type *item)
                                        const
{
  return item->val_real_from_date_op();
}


longlong
Type_handler_temporal_result::Item_func_hybrid_field_type_val_int(
                                        Item_func_hybrid_field_type *item)
                                        const
{
  return item->val_int_from_date_op();
}


my_decimal *
Type_handler_temporal_result::Item_func_hybrid_field_type_val_decimal(
                                        Item_func_hybrid_field_type *item,
                                        my_decimal *dec) const
{
  return item->val_decimal_from_date_op(dec);
}


void
Type_handler_temporal_result::Item_func_hybrid_field_type_get_date(
                                        THD *thd,
                                        Item_func_hybrid_field_type *item,
                                        Temporal::Warn *warn,
                                        MYSQL_TIME *ltime,
                                        date_mode_t fuzzydate) const
{
  if (item->date_op(thd, ltime, fuzzydate))
    set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
}


/***************************************************************************/

String *
Type_handler_time_common::Item_func_hybrid_field_type_val_str(
                                    Item_func_hybrid_field_type *item,
                                    String *str) const
{
  return item->val_str_from_time_op(str);
}


double
Type_handler_time_common::Item_func_hybrid_field_type_val_real(
                                    Item_func_hybrid_field_type *item)
                                    const
{
  return item->val_real_from_time_op();
}


longlong
Type_handler_time_common::Item_func_hybrid_field_type_val_int(
                                    Item_func_hybrid_field_type *item)
                                    const
{
  return item->val_int_from_time_op();
}


my_decimal *
Type_handler_time_common::Item_func_hybrid_field_type_val_decimal(
                                    Item_func_hybrid_field_type *item,
                                    my_decimal *dec) const
{
  return item->val_decimal_from_time_op(dec);
}


void
Type_handler_time_common::Item_func_hybrid_field_type_get_date(
                                    THD *thd,
                                    Item_func_hybrid_field_type *item,
                                    Temporal::Warn *warn,
                                    MYSQL_TIME *ltime,
                                    date_mode_t fuzzydate) const
{
  if (item->time_op(thd, ltime))
    set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
}


/***************************************************************************/

String *
Type_handler_string_result::Item_func_hybrid_field_type_val_str(
                                             Item_func_hybrid_field_type *item,
                                             String *str) const
{
  return item->val_str_from_str_op(str);
}


double
Type_handler_string_result::Item_func_hybrid_field_type_val_real(
                                             Item_func_hybrid_field_type *item)
                                             const
{
  return item->val_real_from_str_op();
}


longlong
Type_handler_string_result::Item_func_hybrid_field_type_val_int(
                                             Item_func_hybrid_field_type *item)
                                             const
{
  return item->val_int_from_str_op();
}


my_decimal *
Type_handler_string_result::Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *item,
                                              my_decimal *dec) const
{
  return item->val_decimal_from_str_op(dec);
}


void
Type_handler_string_result::Item_func_hybrid_field_type_get_date(
                                             THD *thd,
                                             Item_func_hybrid_field_type *item,
                                             Temporal::Warn *warn,
                                             MYSQL_TIME *ltime,
                                             date_mode_t mode) const
{
  StringBuffer<40> tmp;
  String *res= item->str_op(&tmp);
  DBUG_ASSERT((res == NULL) == item->null_value);
  new(ltime) Temporal_hybrid(thd, warn, res, mode);
}

/***************************************************************************/

bool Type_handler_numeric::
       Item_func_between_fix_length_and_dec(Item_func_between *func) const
{
  return func->fix_length_and_dec_numeric(current_thd);
}

bool Type_handler_temporal_result::
       Item_func_between_fix_length_and_dec(Item_func_between *func) const
{
  return func->fix_length_and_dec_temporal(current_thd);
}

bool Type_handler_string_result::
       Item_func_between_fix_length_and_dec(Item_func_between *func) const
{
  return func->fix_length_and_dec_string(current_thd);
}


longlong Type_handler_row::
           Item_func_between_val_int(Item_func_between *func) const
{
  DBUG_ASSERT(0);
  func->null_value= true;
  return 0;
}

longlong Type_handler_string_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_string();
}

longlong Type_handler_temporal_with_date::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_datetime();
}

longlong Type_handler_time_common::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_time();
}

longlong Type_handler_timestamp_common::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_native();
}

longlong Type_handler_int_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_int();
}

longlong Type_handler_real_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_real();
}

longlong Type_handler_decimal_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_decimal();
}

/***************************************************************************/

cmp_item *Type_handler_int_result::make_cmp_item(THD *thd,
                                                 CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_int;
}

cmp_item *Type_handler_real_result::make_cmp_item(THD *thd,
                                                 CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_real;
}

cmp_item *Type_handler_decimal_result::make_cmp_item(THD *thd,
                                                     CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_decimal;
}


cmp_item *Type_handler_string_result::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_sort_string(cs);
}

cmp_item *Type_handler_row::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_row;
}

cmp_item *Type_handler_time_common::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_time;
}

cmp_item *Type_handler_temporal_with_date::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_datetime;
}

cmp_item *Type_handler_timestamp_common::make_cmp_item(THD *thd,
                                                       CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_timestamp;
}

/***************************************************************************/

static int srtcmp_in(const void *cs_, const void *x_, const void *y_)
{
  const CHARSET_INFO *cs= static_cast<const CHARSET_INFO *>(cs_);
  const String *x= static_cast<const String *>(x_);
  const String *y= static_cast<const String *>(y_);
  return cs->strnncollsp(x->ptr(), x->length(), y->ptr(), y->length());
}

in_vector *Type_handler_string_result::make_in_vector(THD *thd,
                                                      const Item_func_in *func,
                                                      uint nargs) const
{
  return new (thd->mem_root) in_string(thd, nargs, (qsort2_cmp) srtcmp_in,
                                       func->compare_collation());

}


in_vector *Type_handler_int_result::make_in_vector(THD *thd,
                                                   const Item_func_in *func,
                                                   uint nargs) const
{
  return new (thd->mem_root) in_longlong(thd, nargs);
}


in_vector *Type_handler_real_result::make_in_vector(THD *thd,
                                                    const Item_func_in *func,
                                                    uint nargs) const
{
  return new (thd->mem_root) in_double(thd, nargs);
}


in_vector *Type_handler_decimal_result::make_in_vector(THD *thd,
                                                       const Item_func_in *func,
                                                       uint nargs) const
{
  return new (thd->mem_root) in_decimal(thd, nargs);
}


in_vector *Type_handler_time_common::make_in_vector(THD *thd,
                                                    const Item_func_in *func,
                                                    uint nargs) const
{
  return new (thd->mem_root) in_time(thd, nargs);
}


in_vector *
Type_handler_temporal_with_date::make_in_vector(THD *thd,
                                                const Item_func_in *func,
                                                uint nargs) const
{
  return new (thd->mem_root) in_datetime(thd, nargs);
}


in_vector *
Type_handler_timestamp_common::make_in_vector(THD *thd,
                                              const Item_func_in *func,
                                              uint nargs) const
{
  return new (thd->mem_root) in_timestamp(thd, nargs);
}


in_vector *Type_handler_row::make_in_vector(THD *thd,
                                            const Item_func_in *func,
                                            uint nargs) const
{
  return new (thd->mem_root) in_row(thd, nargs, 0);
}

/***************************************************************************/

bool Type_handler_string_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  if (func->agg_all_arg_charsets_for_comparison())
    return true;
  if (func->compatible_types_scalar_bisection_possible())
  {
    return func->value_list_convert_const_to_int(thd) ||
           func->fix_for_scalar_comparison_using_bisection(thd);
  }
  return
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) STRING_RESULT);
}


bool Type_handler_int_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  /*
     Does not need to call value_list_convert_const_to_int()
     as already handled by int handler.
  */
  return func->compatible_types_scalar_bisection_possible() ?
    func->fix_for_scalar_comparison_using_bisection(thd) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) INT_RESULT);
}


bool Type_handler_real_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  return func->compatible_types_scalar_bisection_possible() ?
    (func->value_list_convert_const_to_int(thd) ||
     func->fix_for_scalar_comparison_using_bisection(thd)) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) REAL_RESULT);
}


bool Type_handler_decimal_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  return func->compatible_types_scalar_bisection_possible() ?
    (func->value_list_convert_const_to_int(thd) ||
     func->fix_for_scalar_comparison_using_bisection(thd)) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) DECIMAL_RESULT);
}


bool Type_handler_temporal_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  return func->compatible_types_scalar_bisection_possible() ?
    (func->value_list_convert_const_to_int(thd) ||
     func->fix_for_scalar_comparison_using_bisection(thd)) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) TIME_RESULT);
}


bool Type_handler_row::Item_func_in_fix_comparator_compatible_types(THD *thd,
                                              Item_func_in *func) const
{
  return func->compatible_types_row_bisection_possible() ?
         func->fix_for_row_comparison_using_bisection(thd) :
         func->fix_for_row_comparison_using_cmp_items(thd);
}

/***************************************************************************/

String *Type_handler_string_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_str_native(str);
}


String *Type_handler_time_common::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return Time(func).to_string(str, func->decimals);
}


String *Type_handler_date_common::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return Date(func).to_string(str);
}


String *Type_handler_datetime_common::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return Datetime(func).to_string(str, func->decimals);
}


String *Type_handler_timestamp_common::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  THD *thd= current_thd;
  return Timestamp_or_zero_datetime_native_null(thd, func).
           to_datetime(thd).to_string(str, func->decimals);
}


String *Type_handler_int_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_string_from_int(str);
}


String *Type_handler_decimal_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return VDec(func).to_string_round(str, func->decimals);
}


String *Type_handler_double::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_string_from_real(str);
}


String *Type_handler_float::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  Float nr(func->val_real());
  if (func->null_value)
    return 0;
  nr.to_string(str, func->decimals);
  return str;
}


double Type_handler_string_result::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  return func->val_real_native();
}


double Type_handler_time_common::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  return Time(current_thd, func).to_double();
}


double Type_handler_date_common::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  return Date(current_thd, func).to_double();
}


double Type_handler_datetime_common::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  return Datetime(current_thd, func).to_double();
}


double Type_handler_timestamp_common::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  THD *thd= current_thd;
  return Timestamp_or_zero_datetime_native_null(thd, func).
           to_datetime(thd).to_double();
}


double Type_handler_numeric::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  return func->val_real_native();
}


longlong Type_handler_string_result::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  return func->val_int_native();
}


longlong Type_handler_time_common::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  return Time(current_thd, func).to_longlong();
}


longlong Type_handler_date_common::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  return Date(current_thd, func).to_longlong();
}


longlong Type_handler_datetime_common::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  return Datetime(current_thd, func).to_longlong();
}


longlong Type_handler_timestamp_common::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  THD *thd= current_thd;
  return Timestamp_or_zero_datetime_native_null(thd, func).
           to_datetime(thd).to_longlong();
}


longlong Type_handler_numeric::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  return func->val_int_native();
}


my_decimal *Type_handler_string_result::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  return func->val_decimal_native(dec);
}


my_decimal *Type_handler_numeric::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  return func->val_decimal_native(dec);
}


my_decimal *Type_handler_time_common::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  return Time(current_thd, func).to_decimal(dec);
}


my_decimal *Type_handler_date_common::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  return Date(current_thd, func).to_decimal(dec);
}


my_decimal *Type_handler_datetime_common::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  return Datetime(current_thd, func).to_decimal(dec);
}


my_decimal *Type_handler_timestamp_common::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  THD *thd= current_thd;
  return Timestamp_or_zero_datetime_native_null(thd, func).
           to_datetime(thd).to_decimal(dec);
}


bool Type_handler_string_result::
       Item_func_min_max_get_date(THD *thd, Item_func_min_max *func,
                                  MYSQL_TIME *ltime, date_mode_t fuzzydate) const
{
  /*
    just like ::val_int() method of a string item can be called,
    for example, SELECT CONCAT("10", "12") + 1,
    ::get_date() can be called for non-temporal values,
    for example, SELECT MONTH(GREATEST("2011-11-21", "2010-10-09"))
  */
  return func->get_date_from_string(thd, ltime, fuzzydate);
}


bool Type_handler_numeric::
       Item_func_min_max_get_date(THD *thd, Item_func_min_max *func,
                                  MYSQL_TIME *ltime, date_mode_t fuzzydate) const
{
  return Item_get_date_with_warn(thd, func, ltime, fuzzydate);
}


bool Type_handler_temporal_result::
       Item_func_min_max_get_date(THD *thd, Item_func_min_max *func,
                                  MYSQL_TIME *ltime, date_mode_t fuzzydate) const
{
  /*
    - If the caller specified TIME_TIME_ONLY, then it's going to convert
      a DATETIME or DATE to TIME. So we pass the default flags for date. This is
      exactly the same with what Item_func_min_max_val_{int|real|decimal|str} or
      Item_send_datetime() do. We return the value in accordance with the
      current session date flags and let the caller further convert it to TIME.
    - If the caller did not specify TIME_TIME_ONLY, then return the value
      according to the flags supplied by the caller.
  */
  return func->get_date_native(thd, ltime,
                               fuzzydate & TIME_TIME_ONLY ?
                               Datetime::Options(thd) :
                               fuzzydate);
}

bool Type_handler_time_common::
       Item_func_min_max_get_date(THD *thd, Item_func_min_max *func,
                                  MYSQL_TIME *ltime, date_mode_t fuzzydate) const
{
  return func->get_time_native(thd, ltime);
}


bool Type_handler_timestamp_common::
       Item_func_min_max_get_date(THD *thd, Item_func_min_max *func,
                                  MYSQL_TIME *ltime, date_mode_t fuzzydate) const
{
  return Timestamp_or_zero_datetime_native_null(thd, func).
           to_datetime(thd).copy_to_mysql_time(ltime);
}

/***************************************************************************/

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


/**
  Get a string representation of the Item value,
  using the character string format with its charset and collation, e.g.
    latin1 'string' COLLATE latin1_german2_ci
*/
String *Type_handler::
          print_item_value_csstr(THD *thd, Item *item, String *str) const
{
  String *result= item->val_str(str);

  if (!result)
    return NULL;

  StringBuffer<STRING_BUFFER_USUAL_SIZE> buf(result->charset());
  CHARSET_INFO *cs= thd->variables.character_set_client;

  buf.append('_');
  buf.append(result->charset()->cs_name);
  if (cs->escape_with_backslash_is_dangerous)
    buf.append(' ');
  append_query_string(cs, &buf, result->ptr(), result->length(),
                     thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES);
  buf.append(STRING_WITH_LEN(" COLLATE '"));
  buf.append(item->collation.collation->coll_name);
  buf.append('\'');
  str->copy(buf);

  return str;
}


String *Type_handler_numeric::
          print_item_value(THD *thd, Item *item, String *str) const
{
  return item->val_str(str);
}


String *Type_handler::
          print_item_value_temporal(THD *thd, Item *item, String *str,
                                    const Name &type_name, String *buf) const
{
  String *result= item->val_str(buf);
  return !result ||
         str->realloc(type_name.length() + result->length() + 2) ||
         str->copy(type_name.ptr(), type_name.length(), &my_charset_latin1) ||
         str->append('\'') ||
         str->append(result->ptr(), result->length()) ||
         str->append('\'') ?
         NULL :
         str;
}


String *Type_handler_time_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_TIME_FULL_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(STRING_WITH_LEN("TIME")), &buf);
}


String *Type_handler_date_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_DATE_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(STRING_WITH_LEN("DATE")), &buf);
}


String *Type_handler_datetime_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_DATETIME_FULL_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(STRING_WITH_LEN("TIMESTAMP")), &buf);
}


String *Type_handler_timestamp_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_DATETIME_FULL_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(STRING_WITH_LEN("TIMESTAMP")), &buf);
}


/***************************************************************************/

bool Type_handler_row::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_int(this, item->arguments()[0],
                    field_type() == MYSQL_TYPE_LONGLONG);
  return false;
}


bool Type_handler_year::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_int(&type_handler_ulong, item->arguments()[0], false);
  return false;
}


bool Type_handler_hex_hybrid::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_hex_hybrid();
  return false;
}


bool Type_handler_bit::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  uint nbits= item->arguments()[0]->max_length;
  item->fix_length_and_dec_ulong_or_ulonglong_by_nbits(nbits);
  return false;
}


bool Type_handler_typelib::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_length_and_dec_long_or_longlong(5, true);
  return false;
}


bool Type_handler_real_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_decimal();
  return false;
}


bool Type_handler_date_common::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  static const Type_std_attributes attr(Type_numeric_attributes(8, 0, true),
                                        DTCollation_numeric());
  item->fix_arg_int(&type_handler_ulong, &attr, false);
  return false;
}


bool Type_handler_time_common::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_time();
  return false;
}


bool Type_handler_datetime_common::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_datetime();
  return false;
}


bool Type_handler_timestamp_common::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_datetime();
  return false;
}


bool Type_handler_string_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_double();
  return false;
}


/***************************************************************************/

bool Type_handler_row::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->Type_std_attributes::set(item->arguments()[0]);
  item->set_handler(this);
  return false;
}


bool Type_handler_year::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->Type_std_attributes::set(item->arguments()[0]);
  item->set_handler(&type_handler_ulong);
  return false;
}


bool Type_handler_bit::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  uint nbits= item->arguments()[0]->max_length;
  item->fix_length_and_dec_ulong_or_ulonglong_by_nbits(nbits);
  return false;
}


bool Type_handler_typelib::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_long_or_longlong(5, true);
  return false;
}


bool Type_handler_hex_hybrid::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  uint nchars= item->arguments()[0]->decimal_precision();
  item->fix_length_and_dec_long_or_longlong(nchars, true);
  return false;
}


bool Type_handler_real_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_int_or_decimal();
  return false;
}


bool Type_handler_date_common::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  static const Type_numeric_attributes attr(8, 0/*dec*/, true/*unsigned*/);
  item->Type_std_attributes::set(attr, DTCollation_numeric());
  item->set_handler(&type_handler_ulong);
  return false;
}


bool Type_handler_time_common::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_time();
  return false;
}


bool Type_handler_datetime_common::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_datetime();
  return false;
}


bool Type_handler_timestamp_common::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_datetime();
  return false;
}


bool Type_handler_string_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


/***************************************************************************/

bool Type_handler_row::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_string_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


/***************************************************************************/

bool Type_handler_row::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_string_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


/***************************************************************************/

bool Type_handler::
       Item_func_signed_fix_length_and_dec(Item_func_signed *item) const
{
  item->fix_length_and_dec_generic();
  return false;
}


bool Type_handler::
       Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const
{
  const Item *arg= item->arguments()[0];
  if (!arg->unsigned_flag && arg->val_int_min() < 0)
  {
    /*
      Negative arguments produce long results:
        CAST(1-2 AS UNSIGNED) -> 18446744073709551615
    */
    item->max_length= MAX_BIGINT_WIDTH;
    return false;
  }
  item->fix_length_and_dec_generic();
  return false;
}


bool Type_handler_string_result::
       Item_func_signed_fix_length_and_dec(Item_func_signed *item) const
{
  item->fix_length_and_dec_string();
  return false;
}


bool Type_handler_string_result::
       Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const
{
  const Item *arg= item->arguments()[0];
  if (!arg->unsigned_flag &&       // Not HEX hybrid
      arg->max_char_length() > 1)  // Can be negative
  {
    // String arguments can give long results: '-1' -> 18446744073709551614
    item->max_length= MAX_BIGINT_WIDTH;
    return false;
  }
  item->fix_length_and_dec_string();
  return false;
}

bool Type_handler_real_result::
       Item_func_signed_fix_length_and_dec(Item_func_signed *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_real_result::
       Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler::
       Item_double_typecast_fix_length_and_dec(Item_double_typecast *item) const
{
  item->fix_length_and_dec_generic();
  return false;
}


bool Type_handler::
       Item_float_typecast_fix_length_and_dec(Item_float_typecast *item) const
{
  item->fix_length_and_dec_generic();
  return false;
}


bool Type_handler::
       Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item) const
{
  item->fix_length_and_dec_generic();
  return false;
}


bool Type_handler::
       Item_char_typecast_fix_length_and_dec(Item_char_typecast *item) const
{
  item->fix_length_and_dec_generic();
  return false;
}


bool Type_handler_numeric::
       Item_char_typecast_fix_length_and_dec(Item_char_typecast *item) const
{
  item->fix_length_and_dec_numeric();
  return false;
}


bool Type_handler_string_result::
       Item_char_typecast_fix_length_and_dec(Item_char_typecast *item) const
{
  item->fix_length_and_dec_str();
  return false;
}


bool Type_handler::
       Item_time_typecast_fix_length_and_dec(Item_time_typecast *item) const
{
  uint dec= item->decimals == NOT_FIXED_DEC ?
            item->arguments()[0]->time_precision(current_thd) :
            item->decimals;
  item->fix_attributes_temporal(MIN_TIME_WIDTH, dec);
  item->set_maybe_null();
  return false;
}


bool Type_handler::
       Item_date_typecast_fix_length_and_dec(Item_date_typecast *item) const
{
  item->fix_attributes_temporal(MAX_DATE_WIDTH, 0);
  item->set_maybe_null();
  return false;
}


bool Type_handler::
       Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item)
                                                 const
{
  uint dec= item->decimals == NOT_FIXED_DEC ?
            item->arguments()[0]->datetime_precision(current_thd) :
            item->decimals;
  item->fix_attributes_temporal(MAX_DATETIME_WIDTH, dec);
  item->set_maybe_null();
  return false;
}


/***************************************************************************/

bool Type_handler_row::
       Item_func_plus_fix_length_and_dec(Item_func_plus *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_int_result::
       Item_func_plus_fix_length_and_dec(Item_func_plus *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_plus_fix_length_and_dec(Item_func_plus *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_plus_fix_length_and_dec(Item_func_plus *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_plus_fix_length_and_dec(Item_func_plus *item) const
{
  item->fix_length_and_dec_temporal(true);
  return false;
}


bool Type_handler_string_result::
       Item_func_plus_fix_length_and_dec(Item_func_plus *item) const
{
  item->fix_length_and_dec_double();
  return false;
}

/***************************************************************************/

bool Type_handler_row::
       Item_func_minus_fix_length_and_dec(Item_func_minus *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_int_result::
       Item_func_minus_fix_length_and_dec(Item_func_minus *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_minus_fix_length_and_dec(Item_func_minus *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_minus_fix_length_and_dec(Item_func_minus *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_minus_fix_length_and_dec(Item_func_minus *item) const
{
  item->fix_length_and_dec_temporal(true);
  return false;
}


bool Type_handler_string_result::
       Item_func_minus_fix_length_and_dec(Item_func_minus *item) const
{
  item->fix_length_and_dec_double();
  return false;
}

/***************************************************************************/

bool Type_handler_row::
       Item_func_mul_fix_length_and_dec(Item_func_mul *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_int_result::
       Item_func_mul_fix_length_and_dec(Item_func_mul *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_mul_fix_length_and_dec(Item_func_mul *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_mul_fix_length_and_dec(Item_func_mul *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_mul_fix_length_and_dec(Item_func_mul *item) const
{
  item->fix_length_and_dec_temporal(true);
  return false;
}


bool Type_handler_string_result::
       Item_func_mul_fix_length_and_dec(Item_func_mul *item) const
{
  item->fix_length_and_dec_double();
  return false;
}

/***************************************************************************/

bool Type_handler_row::
       Item_func_div_fix_length_and_dec(Item_func_div *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_int_result::
       Item_func_div_fix_length_and_dec(Item_func_div *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_div_fix_length_and_dec(Item_func_div *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_div_fix_length_and_dec(Item_func_div *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_div_fix_length_and_dec(Item_func_div *item) const
{
  item->fix_length_and_dec_temporal(false);
  return false;
}


bool Type_handler_string_result::
       Item_func_div_fix_length_and_dec(Item_func_div *item) const
{
  item->fix_length_and_dec_double();
  return false;
}

/***************************************************************************/

bool Type_handler_row::
       Item_func_mod_fix_length_and_dec(Item_func_mod *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_int_result::
       Item_func_mod_fix_length_and_dec(Item_func_mod *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_mod_fix_length_and_dec(Item_func_mod *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_mod_fix_length_and_dec(Item_func_mod *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_mod_fix_length_and_dec(Item_func_mod *item) const
{
  item->fix_length_and_dec_temporal(true);
  return false;
}


bool Type_handler_string_result::
       Item_func_mod_fix_length_and_dec(Item_func_mod *item) const
{
  item->fix_length_and_dec_double();
  return false;
}

/***************************************************************************/

const Vers_type_handler* Type_handler_temporal_result::vers() const
{
  return &vers_type_timestamp;
}

const Vers_type_handler* Type_handler_string_result::vers() const
{
  return &vers_type_timestamp;
}

const Vers_type_handler* Type_handler_blob_common::vers() const

{
  return &vers_type_timestamp;
}

/***************************************************************************/

decimal_digits_t Type_handler::Item_time_precision(THD *thd, Item *item) const
{
  return MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS);
}


decimal_digits_t Type_handler::Item_datetime_precision(THD *thd, Item *item) const
{
  return MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS);
}


decimal_digits_t Type_handler_string_result::
Item_temporal_precision(THD *thd, Item *item, bool is_time) const
{
  StringBuffer<64> buf;
  String *tmp;
  MYSQL_TIME_STATUS status;
  DBUG_ASSERT(item->fixed());
  // Nanosecond rounding is not needed here, for performance purposes
  if ((tmp= item->val_str(&buf)) &&
      (is_time ?
       Time(thd, &status, tmp->ptr(), tmp->length(), tmp->charset(),
            Time::Options(TIME_TIME_ONLY, TIME_FRAC_TRUNCATE,
                          Time::DATETIME_TO_TIME_YYYYMMDD_TRUNCATE)).
         is_valid_time() :
       Datetime(thd, &status, tmp->ptr(), tmp->length(), tmp->charset(),
                Datetime::Options(TIME_FUZZY_DATES, TIME_FRAC_TRUNCATE)).
         is_valid_datetime()))
    return (decimal_digits_t) MY_MIN(status.precision, TIME_SECOND_PART_DIGITS);
  return (decimal_digits_t) MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS);
}

/***************************************************************************/

decimal_digits_t Type_handler::Item_decimal_scale(const Item *item) const
{
  return (item->decimals < NOT_FIXED_DEC ?
          item->decimals :
          (decimal_digits_t) MY_MIN(item->max_length, DECIMAL_MAX_SCALE));
}

decimal_digits_t Type_handler_temporal_result::
Item_decimal_scale_with_seconds(const Item *item) const
{
  return (item->decimals < NOT_FIXED_DEC ?
          item->decimals :
          TIME_SECOND_PART_DIGITS);
}

decimal_digits_t Type_handler::Item_divisor_precision_increment(const Item *item) const
{
  return item->decimals;
}

decimal_digits_t Type_handler_temporal_result::
Item_divisor_precision_increment_with_seconds(const Item *item) const
{
  return item->decimals <  NOT_FIXED_DEC ?
         item->decimals :
         TIME_SECOND_PART_DIGITS;
}

/***************************************************************************/

decimal_digits_t Type_handler_string_result::Item_decimal_precision(const Item *item) const
{
  uint res= item->max_char_length();
  /*
    Return at least one decimal digit, even if Item::max_char_length()
    returned  0. This is important to avoid attempts to create fields of types
    INT(0) or DECIMAL(0,0) when converting NULL or empty strings to INT/DECIMAL:
      CREATE TABLE t1 AS SELECT CONVERT(NULL,SIGNED) AS a;
  */
  return res ? (decimal_digits_t) MY_MIN(res, DECIMAL_MAX_PRECISION) : (decimal_digits_t) 1;
}

decimal_digits_t Type_handler_real_result::Item_decimal_precision(const Item *item) const
{
  uint res= item->max_char_length();
  return res ? (decimal_digits_t) MY_MIN(res, DECIMAL_MAX_PRECISION) : (decimal_digits_t) 1;
}

decimal_digits_t Type_handler_decimal_result::Item_decimal_precision(const Item *item) const
{
  uint prec= my_decimal_length_to_precision(item->max_char_length(),
                                            item->decimals,
                                            item->unsigned_flag);
  return (decimal_digits_t) MY_MIN(prec, DECIMAL_MAX_PRECISION);
}

decimal_digits_t Type_handler_int_result::Item_decimal_precision(const Item *item) const
{
 uint prec= my_decimal_length_to_precision(item->max_char_length(),
                                           item->decimals,
                                           item->unsigned_flag);
 return (decimal_digits_t) MY_MIN(prec, DECIMAL_MAX_PRECISION);
}

decimal_digits_t Type_handler_time_common::Item_decimal_precision(const Item *item) const
{
  return (decimal_digits_t) (7 + MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS));
}

decimal_digits_t Type_handler_date_common::Item_decimal_precision(const Item *item) const
{
  return 8;
}

decimal_digits_t Type_handler_datetime_common::
Item_decimal_precision(const Item *item) const
{
  return (decimal_digits_t) (14 + MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS));
}

decimal_digits_t Type_handler_timestamp_common::
Item_decimal_precision(const Item *item) const
{
  return (decimal_digits_t) (14 + MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS));
}

/***************************************************************************/

bool Type_handler_real_result::
       subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate) const
{
  DBUG_ASSERT(inner->cmp_type() == REAL_RESULT);
  return outer->cmp_type() == REAL_RESULT;
}


bool Type_handler_int_result::
       subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate) const
{
  DBUG_ASSERT(inner->cmp_type() == INT_RESULT);
  return outer->cmp_type() == INT_RESULT;
}


bool Type_handler_decimal_result::
       subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate) const
{
  DBUG_ASSERT(inner->cmp_type() == DECIMAL_RESULT);
  return outer->cmp_type() == DECIMAL_RESULT;
}


bool Type_handler_string_result::
       subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate) const
{
  DBUG_ASSERT(inner->cmp_type() == STRING_RESULT);
  if (outer->cmp_type() == STRING_RESULT &&
      /*
        Materialization also is unable to work when create_tmp_table() will
        create a blob column because item->max_length is too big.
        The following test is copied from varstring_type_handler().
      */
      !inner->too_big_for_varchar())
  {
    if (outer->collation.collation == inner->collation.collation)
      return true;
    if (is_in_predicate)
    {
      Charset inner_col(inner->collation.collation);
      if (inner_col.encoding_allows_reinterpret_as(outer->
                                                   collation.collation) &&
          inner_col.eq_collation_specific_names(outer->collation.collation))
        return true;
    }
  }
  return false;
}


bool Type_handler_temporal_result::
       subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate) const
{
  DBUG_ASSERT(inner->cmp_type() == TIME_RESULT);
  return mysql_timestamp_type() ==
         outer->type_handler()->mysql_timestamp_type();
}

/***************************************************************************/


const Type_handler *
Type_handler_null::type_handler_for_tmp_table(const Item *item) const
{
  return &type_handler_string;
}


const Type_handler *
Type_handler_null::type_handler_for_union(const Item *item) const
{
  return &type_handler_string;
}


const Type_handler *
Type_handler_olddecimal::type_handler_for_tmp_table(const Item *item) const
{
  return &type_handler_newdecimal;
}

const Type_handler *
Type_handler_olddecimal::type_handler_for_union(const Item *item) const
{
  return &type_handler_newdecimal;
}


/***************************************************************************/

bool Type_handler::check_null(const Item *item, st_value *value) const
{
  if (item->null_value)
  {
    value->m_type= DYN_COL_NULL;
    return true;
  }
  return false;
}


bool Type_handler_null::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  value->m_type= DYN_COL_NULL;
  return true;
}


bool Type_handler_row::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  DBUG_ASSERT(0);
  value->m_type= DYN_COL_NULL;
  return true;
}


bool Type_handler_int_result::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  value->m_type= item->unsigned_flag ? DYN_COL_UINT : DYN_COL_INT;
  value->value.m_longlong= item->val_int();
  return check_null(item, value);
}


bool Type_handler_real_result::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  value->m_type= DYN_COL_DOUBLE;
  value->value.m_double= item->val_real();
  return check_null(item, value);
}


bool Type_handler_decimal_result::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  value->m_type= DYN_COL_DECIMAL;
  my_decimal *dec= item->val_decimal(&value->m_decimal);
  if (dec != &value->m_decimal && !item->null_value)
    my_decimal2decimal(dec, &value->m_decimal);
  return check_null(item, value);
}


bool Type_handler_string_result::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  value->m_type= DYN_COL_STRING;
  String *str= item->val_str(&value->m_string);
  if (str != &value->m_string && !item->null_value)
    value->m_string.set(str->ptr(), str->length(), str->charset());
  return check_null(item, value);
}


bool Type_handler_temporal_with_date::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  value->m_type= DYN_COL_DATETIME;
  item->get_date(thd, &value->value.m_time,
                 Datetime::Options(thd, TIME_FRAC_NONE));
  return check_null(item, value);
}


bool Type_handler_time_common::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  value->m_type= DYN_COL_DATETIME;
  item->get_time(thd, &value->value.m_time);
  return check_null(item, value);
}

/***************************************************************************/

bool Type_handler_row::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  DBUG_ASSERT(0);
  param->set_null();
  return true;
}


bool Type_handler_real_result::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  param->unsigned_flag= attr->unsigned_flag;
  param->set_double(val->value.m_double);
  return false;
}


bool Type_handler_int_result::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  param->unsigned_flag= attr->unsigned_flag;
  param->set_int(val->value.m_longlong, attr->max_length);
  return false;
}


bool Type_handler_decimal_result::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  param->unsigned_flag= attr->unsigned_flag;
  param->set_decimal(&val->m_decimal, attr->unsigned_flag);
  return false;
}


bool Type_handler_string_result::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  param->unsigned_flag= false;
  param->setup_conversion_string(thd, attr->collation.collation);
  /*
    Exact value of max_length is not known unless data is converted to
    charset of connection, so we have to set it later.
  */
  return param->set_str(val->m_string.ptr(), val->m_string.length(),
                        attr->collation.collation,
                        attr->collation.collation);
}


bool Type_handler_temporal_result::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  param->unsigned_flag= attr->unsigned_flag;
  param->set_time(&val->value.m_time, attr->max_length, attr->decimals);
  return false;
}


/***************************************************************************/

bool Type_handler_null::
      Item_send(Item *item, Protocol *protocol, st_value *buf) const
{
  return protocol->store_null();
}


bool Type_handler::
       Item_send_str(Item *item, Protocol *protocol, st_value *buf) const
{
  String *res;
  if ((res= item->val_str(&buf->m_string)))
  {
    DBUG_ASSERT(!item->null_value);
    return protocol->store(res->ptr(), res->length(), res->charset());
  }
  DBUG_ASSERT(item->null_value);
  return protocol->store_null();
}


bool Type_handler::
       Item_send_tiny(Item *item, Protocol *protocol, st_value *buf) const
{
  longlong nr= item->val_int();
  if (!item->null_value)
    return protocol->store_tiny(nr);
  return protocol->store_null();
}


bool Type_handler::
       Item_send_short(Item *item, Protocol *protocol, st_value *buf) const
{
  longlong nr= item->val_int();
  if (!item->null_value)
    return protocol->store_short(nr);
  return protocol->store_null();
}


bool Type_handler::
       Item_send_long(Item *item, Protocol *protocol, st_value *buf) const
{
  longlong nr= item->val_int();
  if (!item->null_value)
    return protocol->store_long(nr);
  return protocol->store_null();
}

bool Type_handler::
       Item_send_longlong(Item *item, Protocol *protocol, st_value *buf) const
{
  longlong nr= item->val_int();
  if (!item->null_value)
    return protocol->store_longlong(nr, item->unsigned_flag);
  return protocol->store_null();
}


bool Type_handler::
       Item_send_float(Item *item, Protocol *protocol, st_value *buf) const
{
  float nr= (float) item->val_real();
  if (!item->null_value)
    return protocol->store_float(nr, item->decimals);
  return protocol->store_null();
}


bool Type_handler::
       Item_send_double(Item *item, Protocol *protocol, st_value *buf) const
{
  double nr= item->val_real();
  if (!item->null_value)
    return protocol->store_double(nr, item->decimals);
  return protocol->store_null();
}


bool Type_handler::Item_send_timestamp(Item *item,
                                       Protocol *protocol,
                                       st_value *buf) const
{
  Timestamp_or_zero_datetime_native_null native(protocol->thd, item);
  if (native.is_null())
    return protocol->store_null();
  native.to_TIME(protocol->thd, &buf->value.m_time);
  return protocol->store_datetime(&buf->value.m_time, item->decimals);
}


bool Type_handler::
       Item_send_datetime(Item *item, Protocol *protocol, st_value *buf) const
{
  item->get_date(protocol->thd, &buf->value.m_time,
                 Datetime::Options(protocol->thd));
  if (!item->null_value)
    return protocol->store_datetime(&buf->value.m_time, item->decimals);
  return protocol->store_null();
}


bool Type_handler::
       Item_send_date(Item *item, Protocol *protocol, st_value *buf) const
{
  item->get_date(protocol->thd, &buf->value.m_time,
                 Date::Options(protocol->thd));
  if (!item->null_value)
    return protocol->store_date(&buf->value.m_time);
  return protocol->store_null();
}


bool Type_handler::
       Item_send_time(Item *item, Protocol *protocol, st_value *buf) const
{
  item->get_time(protocol->thd, &buf->value.m_time);
  if (!item->null_value)
    return protocol->store_time(&buf->value.m_time, item->decimals);
  return protocol->store_null();
}

/***************************************************************************/

Item *Type_handler_int_result::
  make_const_item_for_comparison(THD *thd, Item *item, const Item *cmp) const
{
  longlong result= item->val_int();
  if (item->null_value)
    return new (thd->mem_root) Item_null(thd, item->name.str);
  return  new (thd->mem_root) Item_int(thd, item->name.str, result,
                                       item->max_length);
}


Item *Type_handler_real_result::
  make_const_item_for_comparison(THD *thd, Item *item, const Item *cmp) const
{
  double result= item->val_real();
  if (item->null_value)
    return new (thd->mem_root) Item_null(thd, item->name.str);
  return new (thd->mem_root) Item_float(thd, item->name.str, result,
                                        item->decimals, item->max_length);
}


Item *Type_handler_decimal_result::
  make_const_item_for_comparison(THD *thd, Item *item, const Item *cmp) const
{
  VDec result(item);
  if (result.is_null())
    return new (thd->mem_root) Item_null(thd, item->name.str);
  return new (thd->mem_root) Item_decimal(thd, item->name.str, result.ptr(),
                                          item->max_length, item->decimals);
}


Item *Type_handler_string_result::
  make_const_item_for_comparison(THD *thd, Item *item, const Item *cmp) const
{
  StringBuffer<MAX_FIELD_WIDTH> tmp;
  String *result= item->val_str(&tmp);
  if (item->null_value)
    return new (thd->mem_root) Item_null(thd, item->name.str);
  LEX_CSTRING value;
  thd->make_lex_string(&value, result->ptr(), result->length());
  return new (thd->mem_root) Item_string(thd, item->name, value,
                                         result->charset());
}


Item *Type_handler_time_common::
  make_const_item_for_comparison(THD *thd, Item *item, const Item *cmp) const
{
  Item_cache_temporal *cache;
  longlong value= item->val_time_packed(thd);
  if (item->null_value)
    return new (thd->mem_root) Item_null(thd, item->name.str);
  cache= new (thd->mem_root) Item_cache_time(thd);
  if (cache)
    cache->store_packed(value, item);
  return cache;
}


Item *Type_handler_temporal_with_date::
  make_const_item_for_comparison(THD *thd, Item *item, const Item *cmp) const
{
  Item_cache_temporal *cache;
  longlong value= item->val_datetime_packed(thd);
  if (item->null_value)
    return new (thd->mem_root) Item_null(thd, item->name.str);
  cache= new (thd->mem_root) Item_cache_datetime(thd);
  if (cache)
    cache->store_packed(value, item);
  return cache;
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

/***************************************************************************/

static const char* item_name(Item *a, String *str)
{
  if (a->name.str)
    return a->name.str;
  str->length(0);
  a->print(str, QT_ORDINARY);
  return str->c_ptr_safe();
}


static void wrong_precision_error(uint errcode, Item *a, uint maximum)
{
  StringBuffer<1024> buf(system_charset_info);
  my_error(errcode, MYF(0), item_name(a, &buf), maximum);
}


/**
  Get precision and scale for a declaration
 
  return
    0  ok
    1  error
*/

bool get_length_and_scale(ulonglong length, ulonglong decimals,
                          uint *out_length, decimal_digits_t *out_decimals,
                          uint max_precision, uint max_scale,
                          Item *a)
{
  if (length > (ulonglong) max_precision)
  {
    wrong_precision_error(ER_TOO_BIG_PRECISION, a, max_precision);
    return 1;
  }
  if (decimals > (ulonglong) max_scale)
  {
    wrong_precision_error(ER_TOO_BIG_SCALE, a, max_scale);
    return 1;
  }

  *out_decimals=  (decimal_digits_t)  decimals;
  my_decimal_trim(&length, out_decimals);
  *out_length=  (uint) length;
  
  if (*out_length < *out_decimals)
  {
    my_error(ER_M_BIGGER_THAN_D, MYF(0), "");
    return 1;
  }
  return 0;
}


Item *Type_handler_longlong::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  if (this != &type_handler_ulonglong)
    return new (thd->mem_root) Item_func_signed(thd, item);
  return new (thd->mem_root) Item_func_unsigned(thd, item);

}


Item *Type_handler_date_common::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  return new (thd->mem_root) Item_date_typecast(thd, item);
}



Item *Type_handler_time_common::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  if (attr.decimals() > MAX_DATETIME_PRECISION)
  {
    wrong_precision_error(ER_TOO_BIG_PRECISION, item, MAX_DATETIME_PRECISION);
    return 0;
  }
  return new (thd->mem_root)
         Item_time_typecast(thd, item, (uint) attr.decimals());
}


Item *Type_handler_datetime_common::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  if (attr.decimals() > MAX_DATETIME_PRECISION)
  {
    wrong_precision_error(ER_TOO_BIG_PRECISION, item, MAX_DATETIME_PRECISION);
    return 0;
  }
  return new (thd->mem_root)
         Item_datetime_typecast(thd, item, (uint) attr.decimals());

}


Item *Type_handler_decimal_result::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  uint len;
  decimal_digits_t dec;
  if (get_length_and_scale(attr.length(), attr.decimals(), &len, &dec,
                           DECIMAL_MAX_PRECISION, DECIMAL_MAX_SCALE, item))
    return NULL;
  return new (thd->mem_root) Item_decimal_typecast(thd, item, len, dec);
}


Item *Type_handler_double::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  uint len;
  decimal_digits_t dec;
  if (!attr.length_specified())
    return new (thd->mem_root) Item_double_typecast(thd, item,
                                                    DBL_DIG + 7,
                                                    NOT_FIXED_DEC);

  if (get_length_and_scale(attr.length(), attr.decimals(), &len, &dec,
                           DECIMAL_MAX_PRECISION, NOT_FIXED_DEC - 1, item))
    return NULL;
  return new (thd->mem_root) Item_double_typecast(thd, item, len, dec);
}


Item *Type_handler_float::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  DBUG_ASSERT(!attr.length_specified());
  return new (thd->mem_root) Item_float_typecast(thd, item);
}


Item *Type_handler_long_blob::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  int len= -1;
  CHARSET_INFO *real_cs= attr.charset() ?
                         attr.charset() :
                         thd->variables.collation_connection;
  if (attr.length_specified())
  {
    if (attr.length() > MAX_FIELD_BLOBLENGTH)
    {
      char buff[1024];
      String buf(buff, sizeof(buff), system_charset_info);
      my_error(ER_TOO_BIG_DISPLAYWIDTH, MYF(0), item_name(item, &buf),
               MAX_FIELD_BLOBLENGTH);
      return NULL;
    }
    len= (int) attr.length();
  }
  return new (thd->mem_root) Item_char_typecast(thd, item, len, real_cs);
}

Item *Type_handler_interval_DDhhmmssff::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  if (attr.decimals() > MAX_DATETIME_PRECISION)
  {
    wrong_precision_error(ER_TOO_BIG_PRECISION, item, MAX_DATETIME_PRECISION);
    return 0;
  }
  return new (thd->mem_root) Item_interval_DDhhmmssff_typecast(thd, item,
                                                               (uint)
                                                               attr.decimals());
}

/***************************************************************************/

void Type_handler_string_result::Item_param_setup_conversion(THD *thd,
                                                             Item_param *param)
                                                             const
{
  param->setup_conversion_string(thd, thd->variables.character_set_client);
}


void Type_handler_blob_common::Item_param_setup_conversion(THD *thd,
                                                           Item_param *param)
                                                           const
{
  param->setup_conversion_blob(thd);
}


void Type_handler_tiny::Item_param_set_param_func(Item_param *param,
                                                  uchar **pos, ulong len) const
{
  param->set_param_tiny(pos, len);
}


void Type_handler_short::Item_param_set_param_func(Item_param *param,
                                                   uchar **pos, ulong len) const
{
  param->set_param_short(pos, len);
}


void Type_handler_long::Item_param_set_param_func(Item_param *param,
                                                  uchar **pos, ulong len) const
{
  param->set_param_int32(pos, len);
}


void Type_handler_longlong::Item_param_set_param_func(Item_param *param,
                                                      uchar **pos,
                                                      ulong len) const
{
  param->set_param_int64(pos, len);
}


void Type_handler_float::Item_param_set_param_func(Item_param *param,
                                                   uchar **pos,
                                                   ulong len) const
{
  param->set_param_float(pos, len);
}


void Type_handler_double::Item_param_set_param_func(Item_param *param,
                                                   uchar **pos,
                                                   ulong len) const
{
  param->set_param_double(pos, len);
}


void Type_handler_decimal_result::Item_param_set_param_func(Item_param *param,
                                                            uchar **pos,
                                                            ulong len) const
{
  param->set_param_decimal(pos, len);
}


void Type_handler_string_result::Item_param_set_param_func(Item_param *param,
                                                           uchar **pos,
                                                           ulong len) const
{
  param->set_param_str(pos, len);
}


void Type_handler_time_common::Item_param_set_param_func(Item_param *param,
                                                         uchar **pos,
                                                         ulong len) const
{
  param->set_param_time(pos, len);
}


void Type_handler_date_common::Item_param_set_param_func(Item_param *param,
                                                         uchar **pos,
                                                         ulong len) const
{
  param->set_param_date(pos, len);
}


void Type_handler_datetime_common::Item_param_set_param_func(Item_param *param,
                                                             uchar **pos,
                                                             ulong len) const
{
  param->set_param_datetime(pos, len);
}

Field *Type_handler_blob_common::make_conversion_table_field(MEM_ROOT *root,
                                                            TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  uint pack_length= metadata & 0x00ff;
  if (pack_length < 1 || pack_length > 4)
    return NULL; // Broken binary log?
  return new(root)
         Field_blob(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str,
                    table->s, pack_length, target->charset());
}


void Type_handler_timestamp_common::Item_param_set_param_func(Item_param *param,
                                                              uchar **pos,
                                                              ulong len) const
{
  param->set_param_datetime(pos, len);
}


void Type_handler::Item_param_set_param_func(Item_param *param,
                                             uchar **pos,
                                             ulong len) const
{
  param->set_null(); // Not possible type code in the client-server protocol
}


void Type_handler_typelib::Item_param_set_param_func(Item_param *param,
                                                     uchar **pos,
                                                     ulong len) const
{
  param->set_null(); // Not possible type code in the client-server protocol
}


/***************************************************************************/

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


Field *Type_handler_olddecimal::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(f_decimals(attr->pack_flag) == 0);
  return new (mem_root)
    Field_decimal(rec.ptr(), (uint32) attr->length,
                  rec.null_ptr(), rec.null_bit(),
                  attr->unireg_check, name,
                  (uint8) attr->decimals,
                  f_is_zerofill(attr->pack_flag) != 0,
                  f_is_dec(attr->pack_flag) == 0);
}


Field *Type_handler_newdecimal::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(f_decimals(attr->pack_flag) == 0);
  return new (mem_root)
    Field_new_decimal(rec.ptr(), (uint32) attr->length,
                      rec.null_ptr(), rec.null_bit(),
                      attr->unireg_check, name,
                      (uint8) attr->decimals,
                      f_is_zerofill(attr->pack_flag) != 0,
                      f_is_dec(attr->pack_flag) == 0);
}


Field *Type_handler_float::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(f_decimals(attr->pack_flag) == 0);
  uint decimals= attr->decimals;
  if (decimals == FLOATING_POINT_DECIMALS)
    decimals= NOT_FIXED_DEC;
  return new (mem_root)
    Field_float(rec.ptr(), (uint32) attr->length,
                rec.null_ptr(), rec.null_bit(),
                attr->unireg_check, name, decimals,
                f_is_zerofill(attr->pack_flag) != 0,
                f_is_dec(attr->pack_flag)== 0);
}


Field *Type_handler_double::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(f_decimals(attr->pack_flag) == 0);
  uint decimals= attr->decimals;
  if (decimals == FLOATING_POINT_DECIMALS)
    decimals= NOT_FIXED_DEC;
  return new (mem_root)
    Field_double(rec.ptr(), (uint32) attr->length,
                 rec.null_ptr(), rec.null_bit(),
                 attr->unireg_check, name, decimals,
                 f_is_zerofill(attr->pack_flag) != 0,
                 f_is_dec(attr->pack_flag)== 0);
}


Field *Type_handler_tiny::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_tiny(rec.ptr(), (uint32) attr->length, rec.null_ptr(), rec.null_bit(),
               attr->unireg_check, name,
               f_is_zerofill(attr->pack_flag) != 0,
               f_is_dec(attr->pack_flag) == 0);
}


Field *Type_handler_short::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_short(rec.ptr(), (uint32) attr->length,
                rec.null_ptr(), rec.null_bit(),
                attr->unireg_check, name,
                f_is_zerofill(attr->pack_flag) != 0,
                f_is_dec(attr->pack_flag) == 0);
}


Field *Type_handler_int24::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_medium(rec.ptr(), (uint32) attr->length,
                 rec.null_ptr(), rec.null_bit(),
                 attr->unireg_check, name,
                 f_is_zerofill(attr->pack_flag) != 0,
                 f_is_dec(attr->pack_flag) == 0);
}


Field *Type_handler_long::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_long(rec.ptr(), (uint32) attr->length, rec.null_ptr(), rec.null_bit(),
               attr->unireg_check, name,
               f_is_zerofill(attr->pack_flag) != 0,
               f_is_dec(attr->pack_flag) == 0);
}


Field *Type_handler_longlong::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  if (flags & (VERS_ROW_START|VERS_ROW_END))
    return new (mem_root)
      Field_vers_trx_id(rec.ptr(), (uint32) attr->length,
                        rec.null_ptr(), rec.null_bit(),
                        attr->unireg_check, name,
                        f_is_zerofill(attr->pack_flag) != 0,
                        f_is_dec(attr->pack_flag) == 0);
  return new (mem_root)
    Field_longlong(rec.ptr(), (uint32) attr->length,
                   rec.null_ptr(), rec.null_bit(),
                   attr->unireg_check, name,
                   f_is_zerofill(attr->pack_flag) != 0,
                   f_is_dec(attr->pack_flag) == 0);
}


Field *Type_handler_timestamp::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->decimals == attr->temporal_dec(MAX_DATETIME_WIDTH));
  return new_Field_timestamp(mem_root,
                             rec.ptr(), rec.null_ptr(), rec.null_bit(),
                             attr->unireg_check, name, share,
                             attr->temporal_dec(MAX_DATETIME_WIDTH));
}


Field *Type_handler_timestamp2::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->decimals == attr->temporal_dec(MAX_DATETIME_WIDTH));
  return new (mem_root)
    Field_timestampf(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                     attr->unireg_check,
                     name, share, attr->temporal_dec(MAX_DATETIME_WIDTH));
}


Field *Type_handler_year::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                                   uint32 flags) const
{
  return new (mem_root)
    Field_year(rec.ptr(), (uint32) attr->length, rec.null_ptr(), rec.null_bit(),
               attr->unireg_check, name);
}


Field *Type_handler_date::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_date(rec.ptr(),rec.null_ptr(),rec.null_bit(),
               attr->unireg_check, name);
}


Field *Type_handler_newdate::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_newdate(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                  attr->unireg_check, name);
}


Field *Type_handler_time::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->decimals == attr->temporal_dec(MIN_TIME_WIDTH));
  return new_Field_time(mem_root, rec.ptr(), rec.null_ptr(), rec.null_bit(),
                        attr->unireg_check, name,
                        attr->temporal_dec(MIN_TIME_WIDTH));
}


Field *Type_handler_time2::
  make_table_field_from_def(TABLE_SHARE *share,  MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->decimals == attr->temporal_dec(MIN_TIME_WIDTH));
  return new (mem_root)
    Field_timef(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                attr->unireg_check, name,
                attr->temporal_dec(MIN_TIME_WIDTH));
}


Field *Type_handler_datetime::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->decimals == attr->temporal_dec(MAX_DATETIME_WIDTH));
  return new_Field_datetime(mem_root, rec.ptr(), rec.null_ptr(), rec.null_bit(),
                            attr->unireg_check, name,
                            attr->temporal_dec(MAX_DATETIME_WIDTH));
}


Field *Type_handler_datetime2::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  DBUG_ASSERT(attr->decimals == attr->temporal_dec(MAX_DATETIME_WIDTH));
  return new (mem_root)
    Field_datetimef(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                    attr->unireg_check, name,
                    attr->temporal_dec(MAX_DATETIME_WIDTH));
}


Field *Type_handler_null::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_null(rec.ptr(), (uint32) attr->length, attr->unireg_check,
               name, attr->charset);
}


Field *Type_handler_bit::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return f_bit_as_char(attr->pack_flag) ?
     new (mem_root) Field_bit_as_char(rec.ptr(), (uint32) attr->length,
                                      rec.null_ptr(), rec.null_bit(),
                                      attr->unireg_check, name) :
     new (mem_root) Field_bit(rec.ptr(), (uint32) attr->length,
                              rec.null_ptr(), rec.null_bit(),
                              bit.ptr(), bit.offs(), attr->unireg_check, name);
}




Field *Type_handler_string::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_string(rec.ptr(), (uint32) attr->length,
                 rec.null_ptr(), rec.null_bit(),
                 attr->unireg_check, name, attr->charset);
}


Field *Type_handler_varchar::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  if (attr->unireg_check == Field::TMYSQL_COMPRESSED)
    return new (mem_root)
      Field_varstring_compressed(rec.ptr(), (uint32) attr->length,
                                 HA_VARCHAR_PACKLENGTH((uint32) attr->length),
                                 rec.null_ptr(), rec.null_bit(),
                                 attr->unireg_check, name, share, attr->charset,
                                 zlib_compression_method);
  return new (mem_root)
    Field_varstring(rec.ptr(), (uint32) attr->length,
                    HA_VARCHAR_PACKLENGTH((uint32) attr->length),
                    rec.null_ptr(), rec.null_bit(),
                    attr->unireg_check, name, share, attr->charset);
}


Field *Type_handler_blob_common::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  if (attr->unireg_check == Field::TMYSQL_COMPRESSED)
    return new (mem_root)
      Field_blob_compressed(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                            attr->unireg_check, name, share,
                            attr->pack_flag_to_pack_length(), attr->charset,
                            zlib_compression_method);
  return new (mem_root)
    Field_blob(rec.ptr(), rec.null_ptr(), rec.null_bit(),
               attr->unireg_check, name, share,
               attr->pack_flag_to_pack_length(), attr->charset);
}


Field *Type_handler_enum::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_enum(rec.ptr(), (uint32) attr->length, rec.null_ptr(), rec.null_bit(),
               attr->unireg_check, name, attr->pack_flag_to_pack_length(),
               attr->interval, attr->charset);
}


Field *Type_handler_set::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_set(rec.ptr(), (uint32) attr->length, rec.null_ptr(), rec.null_bit(),
              attr->unireg_check, name, attr->pack_flag_to_pack_length(),
              attr->interval, attr->charset);
}


/***************************************************************************/

void Type_handler::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  def->frm_pack_basic(buff);
  def->frm_pack_charset(buff);
}


void Type_handler_real_result::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  def->frm_pack_numeric_with_dec(buff);
}


void Type_handler_decimal_result::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  def->frm_pack_numeric_with_dec(buff);
}


void Type_handler_int_result::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag) == 0);
  DBUG_ASSERT(def->decimals == 0);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_date_common::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag) == 0);
  DBUG_ASSERT(def->decimals == 0);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_bit::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag & ~FIELDFLAG_TREAT_BIT_AS_CHAR) == 0);
  DBUG_ASSERT(def->decimals == 0);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_blob_common::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag & ~FIELDFLAG_BLOB) == 0);
  DBUG_ASSERT(def->decimals == 0 ||
              def->decimals == NOT_FIXED_DEC);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_null::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag) == 0);
  DBUG_ASSERT(def->decimals == NOT_FIXED_DEC);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_string_result::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag) == 0);
  DBUG_ASSERT(def->decimals == 0 || def->decimals == NOT_FIXED_DEC);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_enum::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag & ~FIELDFLAG_INTERVAL) == 0);
  DBUG_ASSERT(def->decimals == 0);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_set::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag & ~FIELDFLAG_BITFIELD) == 0);
  DBUG_ASSERT(def->decimals == 0);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


void Type_handler_temporal_result::
  Column_definition_attributes_frm_pack(const Column_definition_attributes *def,
                                        uchar *buff) const
{
  DBUG_ASSERT(f_decimals(def->pack_flag) == 0);
  Type_handler::Column_definition_attributes_frm_pack(def, buff);
}


/***************************************************************************/

bool Type_handler::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  attr->frm_unpack_basic(buffer);
  return attr->frm_unpack_charset(share, buffer);
}


bool Type_handler_real_result::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  return attr->frm_unpack_numeric_with_dec(share, buffer);
}


bool Type_handler_decimal_result::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  return attr->frm_unpack_numeric_with_dec(share, buffer);
}


bool Type_handler_time_common::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  return attr->frm_unpack_temporal_with_dec(share, MIN_TIME_WIDTH, buffer);
}


bool Type_handler_datetime_common::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  return attr->frm_unpack_temporal_with_dec(share, MAX_DATETIME_WIDTH, buffer);
}


bool Type_handler_timestamp_common::
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const
{
  return attr->frm_unpack_temporal_with_dec(share, MAX_DATETIME_WIDTH, buffer);
}


bool Type_handler_null::Item_const_eq(const Item_const *a,
                                      const Item_const *b,
                                      bool binary_cmp) const
{
  return true;
}


bool Type_handler_real_result::Item_const_eq(const Item_const *a,
                                             const Item_const *b,
                                             bool binary_cmp) const
{
  const double *va= a->const_ptr_double();
  const double *vb= b->const_ptr_double();
  return va[0] == vb[0];
}


bool Type_handler_int_result::Item_const_eq(const Item_const *a,
                                            const Item_const *b,
                                            bool binary_cmp) const
{
  const longlong *va= a->const_ptr_longlong();
  const longlong *vb= b->const_ptr_longlong();
  bool res= va[0] == vb[0] &&
            (va[0] >= 0 ||
             (a->get_type_all_attributes_from_const()->unsigned_flag ==
              b->get_type_all_attributes_from_const()->unsigned_flag));
  return res;
}


bool Type_handler_string_result::Item_const_eq(const Item_const *a,
                                               const Item_const *b,
                                               bool binary_cmp) const
{
  const String *sa= a->const_ptr_string();
  const String *sb= b->const_ptr_string();
  return binary_cmp ? sa->bin_eq(sb) :
    a->get_type_all_attributes_from_const()->collation.collation ==
    b->get_type_all_attributes_from_const()->collation.collation &&
    sa->eq(sb, a->get_type_all_attributes_from_const()->collation.collation);
}


bool
Type_handler_decimal_result::Item_const_eq(const Item_const *a,
                                           const Item_const *b,
                                           bool binary_cmp) const
{
  const my_decimal *da= a->const_ptr_my_decimal();
  const my_decimal *db= b->const_ptr_my_decimal();
  return !da->cmp(db) &&
         (!binary_cmp ||
          a->get_type_all_attributes_from_const()->decimals ==
          b->get_type_all_attributes_from_const()->decimals);
}


bool
Type_handler_temporal_result::Item_const_eq(const Item_const *a,
                                            const Item_const *b,
                                            bool binary_cmp) const
{
  const MYSQL_TIME *ta= a->const_ptr_mysql_time();
  const MYSQL_TIME *tb= b->const_ptr_mysql_time();
  return !my_time_compare(ta, tb) &&
         (!binary_cmp ||
          a->get_type_all_attributes_from_const()->decimals ==
          b->get_type_all_attributes_from_const()->decimals);
}

/***************************************************************************/

const Type_handler *
Type_handler_hex_hybrid::cast_to_int_type_handler() const
{
  return &type_handler_ulonglong;
}


/***************************************************************************/

bool Type_handler_row::Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                                     Item *a, Item *b) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::Item_eq_value(THD *thd,
                                            const Type_cmp_attributes *attr,
                                            Item *a, Item *b) const
{
  longlong value0= a->val_int();
  longlong value1= b->val_int();
  return !a->null_value && !b->null_value && value0 == value1 &&
         (value0 >= 0 || a->unsigned_flag == b->unsigned_flag);
}


bool Type_handler_real_result::Item_eq_value(THD *thd,
                                             const Type_cmp_attributes *attr,
                                             Item *a, Item *b) const
{
  double value0= a->val_real();
  double value1= b->val_real();
  return !a->null_value && !b->null_value && value0 == value1;
}


bool Type_handler_time_common::Item_eq_value(THD *thd,
                                             const Type_cmp_attributes *attr,
                                             Item *a, Item *b) const
{
  longlong value0= a->val_time_packed(thd);
  longlong value1= b->val_time_packed(thd);
  return !a->null_value && !b->null_value && value0 == value1;
}


bool Type_handler_temporal_with_date::Item_eq_value(THD *thd,
                                                    const Type_cmp_attributes *attr,
                                                    Item *a, Item *b) const
{
  longlong value0= a->val_datetime_packed(thd);
  longlong value1= b->val_datetime_packed(thd);
  return !a->null_value && !b->null_value && value0 == value1;
}


bool Type_handler_timestamp_common::Item_eq_value(THD *thd,
                                                  const Type_cmp_attributes *attr,
                                                  Item *a, Item *b) const
{
  Timestamp_or_zero_datetime_native_null na(thd, a, true);
  Timestamp_or_zero_datetime_native_null nb(thd, b, true);
  return !na.is_null() && !nb.is_null() && !cmp_native(na, nb);
}


bool Type_handler_string_result::Item_eq_value(THD *thd,
                                               const Type_cmp_attributes *attr,
                                               Item *a, Item *b) const
{
  String *va, *vb;
  StringBuffer<128> cmp_value1, cmp_value2;
  return (va= a->val_str(&cmp_value1)) &&
         (vb= b->val_str(&cmp_value2)) &&
          va->eq(vb, attr->compare_collation());
}


/***************************************************************************/

bool Type_handler_string_result::union_element_finalize(Item_type_holder* item) const
{
  if (item->collation.derivation == DERIVATION_NONE)
  {
    my_error(ER_CANT_AGGREGATE_NCOLLATIONS, MYF(0), "UNION");
    return true;
  }
  return false;
}


/***************************************************************************/

void Type_handler_var_string::
  Column_definition_implicit_upgrade(Column_definition *c) const
{
  // Change old VARCHAR to new VARCHAR
  c->set_handler(&type_handler_varchar);
}


void Type_handler_time_common::
  Column_definition_implicit_upgrade(Column_definition *c) const
{
  if (opt_mysql56_temporal_format)
    c->set_handler(&type_handler_time2);
  else
    c->set_handler(&type_handler_time);
}


void Type_handler_datetime_common::
  Column_definition_implicit_upgrade(Column_definition *c) const
{
  if (opt_mysql56_temporal_format)
    c->set_handler(&type_handler_datetime2);
  else
    c->set_handler(&type_handler_datetime);
}


void Type_handler_timestamp_common::
  Column_definition_implicit_upgrade(Column_definition *c) const
{
  if (opt_mysql56_temporal_format)
    c->set_handler(&type_handler_timestamp2);
  else
    c->set_handler(&type_handler_timestamp);
}


/***************************************************************************/


int Type_handler_temporal_with_date::stored_field_cmp_to_item(THD *thd,
                                                              Field *field,
                                                              Item *item) const
{
  MYSQL_TIME field_time, item_time, item_time2, *item_time_cmp= &item_time;
  field->get_date(&field_time, Datetime::Options(TIME_INVALID_DATES, thd));
  item->get_date(thd, &item_time, Datetime::Options(TIME_INVALID_DATES, thd));
  if (item_time.time_type == MYSQL_TIMESTAMP_TIME &&
      time_to_datetime(thd, &item_time, item_time_cmp= &item_time2))
    return 1;
  return my_time_compare(&field_time, item_time_cmp);
}


int Type_handler_time_common::stored_field_cmp_to_item(THD *thd,
                                                       Field *field,
                                                       Item *item) const
{
  MYSQL_TIME field_time, item_time;
  field->get_date(&field_time, Time::Options(thd));
  item->get_date(thd, &item_time, Time::Options(thd));
  return my_time_compare(&field_time, &item_time);
}


int Type_handler_string_result::stored_field_cmp_to_item(THD *thd,
                                                         Field *field,
                                                         Item *item) const
{
  StringBuffer<MAX_FIELD_WIDTH> item_tmp;
  StringBuffer<MAX_FIELD_WIDTH> field_tmp;
  String *item_result= item->val_str(&item_tmp);
  /*
    Some implementations of Item::val_str(String*) actually modify
    the field Item::null_value, hence we can't check it earlier.
  */
  if (item->null_value)
    return 0;
  String *field_result= field->val_str(&field_tmp);
  return sortcmp(field_result, item_result, field->charset());
}


int Type_handler_int_result::stored_field_cmp_to_item(THD *thd,
                                                      Field *field,
                                                      Item *item) const
{
  DBUG_ASSERT(0); // Not used yet
  return 0;
}


int Type_handler_real_result::stored_field_cmp_to_item(THD *thd,
                                                       Field *field,
                                                       Item *item) const
{
  /*
    The patch for Bug#13463415 started using this function for comparing
    BIGINTs. That uncovered a bug in Visual Studio 32bit optimized mode.
    Prefixing the auto variables with volatile fixes the problem....
  */
  volatile double result= item->val_real();
  if (item->null_value)
    return 0;
  volatile double field_result= field->val_real();
  if (field_result < result)
    return -1;
  else if (field_result > result)
    return 1;
  return 0;
}


/***************************************************************************/


static bool have_important_literal_warnings(const MYSQL_TIME_STATUS *status)
{
  return (status->warnings & ~MYSQL_TIME_NOTE_TRUNCATED) != 0;
}


static void literal_warn(THD *thd, const Item *item,
                         const char *str, size_t length, CHARSET_INFO *cs,
                         const MYSQL_TIME_STATUS *st,
                         const char *typestr, bool send_error)
{
  if (likely(item))
  {
    if (st->warnings) // e.g. a note on nanosecond truncation
    {
      ErrConvString err(str, length, cs);
      thd->push_warning_wrong_or_truncated_value(
                                   Sql_condition::time_warn_level(st->warnings),
                                   false, typestr, err.ptr(),
                                   nullptr, nullptr, nullptr);
    }
  }
  else if (send_error)
  {
    ErrConvString err(str, length, cs);
    my_error(ER_WRONG_VALUE, MYF(0), typestr, err.ptr());
  }
}


Item_literal *
Type_handler_date_common::create_literal_item(THD *thd,
                                              const char *str,
                                              size_t length,
                                              CHARSET_INFO *cs,
                                              bool send_error) const
{
  Temporal::Warn st;
  Item_literal *item= NULL;
  Temporal_hybrid tmp(thd, &st, str, length, cs, Temporal_hybrid::Options(thd));
  if (tmp.is_valid_temporal() &&
      tmp.get_mysql_time()->time_type == MYSQL_TIMESTAMP_DATE &&
      !have_important_literal_warnings(&st))
  {
    Date d(&tmp);
    item= new (thd->mem_root) Item_date_literal(thd, &d);
  }
  literal_warn(thd, item, str, length, cs, &st, "DATE", send_error);
  return item;
}


Item_literal *
Type_handler_temporal_with_date::create_literal_item(THD *thd,
                                                     const char *str,
                                                     size_t length,
                                                     CHARSET_INFO *cs,
                                                     bool send_error) const
{
  Temporal::Warn st;
  Item_literal *item= NULL;
  Temporal_hybrid tmp(thd, &st, str, length, cs, Temporal_hybrid::Options(thd));
  if (tmp.is_valid_temporal() &&
      tmp.get_mysql_time()->time_type == MYSQL_TIMESTAMP_DATETIME &&
      !have_important_literal_warnings(&st))
  {
    Datetime dt(&tmp);
    item= new (thd->mem_root) Item_datetime_literal(thd, &dt, st.precision);
  }
  literal_warn(thd, item, str, length, cs, &st, "DATETIME", send_error);
  return item;
}


Item_literal *
Type_handler_time_common::create_literal_item(THD *thd,
                                              const char *str,
                                              size_t length,
                                              CHARSET_INFO *cs,
                                              bool send_error) const
{
  MYSQL_TIME_STATUS st;
  Item_literal *item= NULL;
  Time::Options opt(TIME_TIME_ONLY, thd, Time::DATETIME_TO_TIME_DISALLOW);
  Time tmp(thd, &st, str, length, cs, opt);
  if (tmp.is_valid_time() &&
      !have_important_literal_warnings(&st))
    item= new (thd->mem_root) Item_time_literal(thd, &tmp, st.precision);
  literal_warn(thd, item, str, length, cs, &st, "TIME", send_error);
  return item;
}


bool
Type_handler_time_common::Item_val_native_with_conversion(THD *thd,
                                                          Item *item,
                                                          Native *to) const
{
  if (item->type_handler()->type_handler_for_native_format() ==
      &type_handler_time2)
    return item->val_native(thd, to);
  return Time(thd, item).to_native(to, item->time_precision(thd));
}


bool
Type_handler_time_common::Item_val_native_with_conversion_result(THD *thd,
                                                                 Item *item,
                                                                 Native *to)
                                                                 const
{
  if (item->type_handler()->type_handler_for_native_format() ==
      &type_handler_time2)
    return item->val_native_result(thd, to);
  MYSQL_TIME ltime;
  if (item->get_date_result(thd, &ltime, Time::Options(thd)))
    return true;
  int warn;
  return Time(&warn, &ltime, 0).to_native(to, item->time_precision(thd));
}


int Type_handler_time_common::cmp_native(const Native &a,
                                         const Native &b) const
{
  // Optimize a simple case: equal fractional precision:
  if (a.length() == b.length())
    return memcmp(a.ptr(), b.ptr(), a.length());
  longlong lla= Time(a).to_packed();
  longlong llb= Time(b).to_packed();
  if (lla < llb)
    return -1;
  if (lla> llb)
    return 1;
  return 0;
}


bool Type_handler_timestamp_common::TIME_to_native(THD *thd,
                                                   const MYSQL_TIME *ltime,
                                                   Native *to,
                                                   uint decimals) const
{
  uint error_code;
  Timestamp_or_zero_datetime tm(thd, ltime, &error_code);
  if (error_code)
    return true;
  tm.trunc(decimals);
  return tm.to_native(to, decimals);
}


bool
Type_handler_timestamp_common::Item_val_native_with_conversion(THD *thd,
                                                               Item *item,
                                                               Native *to) const
{
  MYSQL_TIME ltime;
  if (item->type_handler()->type_handler_for_native_format() ==
      &type_handler_timestamp2)
    return item->val_native(thd, to);
  return
    item->get_date(thd, &ltime, Datetime::Options(TIME_NO_ZERO_IN_DATE, thd)) ||
    TIME_to_native(thd, &ltime, to, item->datetime_precision(thd));
}

bool Type_handler_null::union_element_finalize(Item_type_holder *item) const
{
  item->set_handler(&type_handler_string);
  return false;
}


bool
Type_handler_timestamp_common::Item_val_native_with_conversion_result(THD *thd,
                                                                      Item *item,
                                                                      Native *to)
                                                                      const
{
  MYSQL_TIME ltime;
  if (item->type_handler()->type_handler_for_native_format() ==
      &type_handler_timestamp2)
    return item->val_native_result(thd, to);
  return
    item->get_date_result(thd, &ltime,
                          Datetime::Options(TIME_NO_ZERO_IN_DATE, thd)) ||
    TIME_to_native(thd, &ltime, to, item->datetime_precision(thd));
}


int Type_handler_timestamp_common::cmp_native(const Native &a,
                                              const Native &b) const
{
  /*
    Optimize a simple case:
    Either both timeatamp values have the same fractional precision,
    or both values are zero datetime '0000-00-00 00:00:00.000000',
  */
  if (a.length() == b.length())
    return memcmp(a.ptr(), b.ptr(), a.length());
  return Timestamp_or_zero_datetime(a).cmp(Timestamp_or_zero_datetime(b));
}


Timestamp_or_zero_datetime_native_null::
  Timestamp_or_zero_datetime_native_null(THD *thd, Item *item, bool conv)
   :Null_flag(false)
{
  DBUG_ASSERT(item->type_handler()->type_handler_for_native_format() ==
              &type_handler_timestamp2 || conv);
  if (conv ?
      type_handler_timestamp2.Item_val_native_with_conversion(thd, item, this) :
      item->val_native(thd, this))
    Null_flag::operator=(true);
  // If no conversion, then is_null() should be equal to item->null_value
  DBUG_ASSERT(is_null() == item->null_value || conv);
  /*
    is_null() can be true together with item->null_value==false, which means
    a non-NULL item was evaluated, but then the conversion to TIMESTAMP failed.
    But is_null() can never be false if item->null_value==true.
  */
  DBUG_ASSERT(is_null() >= item->null_value);
}


bool
Type_handler::Item_param_val_native(THD *thd,
                                        Item_param *item,
                                        Native *to) const
{
  DBUG_ASSERT(0); // TODO-TYPE: MDEV-14271
  return item->null_value= true;
}


bool
Type_handler_timestamp_common::Item_param_val_native(THD *thd,
                                                         Item_param *item,
                                                         Native *to) const
{
  /*
    The below code may not run well in corner cases.
    This will be fixed under terms of MDEV-14271.
    Item_param should:
    - either remember @@time_zone at bind time
    - or store TIMESTAMP in my_time_t format, rather than in MYSQL_TIME format.
  */
  MYSQL_TIME ltime;
  return
    item->get_date(thd, &ltime, Datetime::Options(TIME_NO_ZERO_IN_DATE, thd)) ||
    TIME_to_native(thd, &ltime, to, item->datetime_precision(thd));
}


bool
Type_handler_time_common::Item_param_val_native(THD *thd,
                                                Item_param *item,
                                                Native *to) const
{
  return Time(thd, item).to_native(to, item->decimals);
}


/***************************************************************************/

bool Type_handler::validate_implicit_default_value(THD *thd,
                                       const Column_definition &def) const
{
  DBUG_EXECUTE_IF("validate_implicit_default_value_error", return true;);
  return false;
}


bool Type_handler_date_common::validate_implicit_default_value(THD *thd,
                                       const Column_definition &def) const
{
  return thd->variables.sql_mode & MODE_NO_ZERO_DATE;
}


bool Type_handler_datetime_common::validate_implicit_default_value(THD *thd,
                                       const Column_definition &def) const
{
  return thd->variables.sql_mode & MODE_NO_ZERO_DATE;
}


/***************************************************************************/

const Name & Type_handler_row::default_value() const
{
  DBUG_ASSERT(0);
  static Name def(STRING_WITH_LEN(""));
  return def;
}

const Name & Type_handler_numeric::default_value() const
{
  static Name def(STRING_WITH_LEN("0"));
  return def;
}

const Name & Type_handler_string_result::default_value() const
{
  static Name def(STRING_WITH_LEN(""));
  return def;
}

const Name & Type_handler_time_common::default_value() const
{
  static Name def(STRING_WITH_LEN("00:00:00"));
  return def;
}

const Name & Type_handler_date_common::default_value() const
{
  static Name def(STRING_WITH_LEN("0000-00-00"));
  return def;
}

const Name & Type_handler_datetime_common::default_value() const
{
  static Name def(STRING_WITH_LEN("0000-00-00 00:00:00"));
  return def;
}

const Name & Type_handler_timestamp_common::default_value() const
{
  static Name def(STRING_WITH_LEN("0000-00-00 00:00:00"));
  return def;
}

/***************************************************************************/

bool Type_handler::Column_definition_data_type_info_image(Binary_string *to,
                                                   const Column_definition &def)
                                                   const
{
  // Have *some* columns write type info (let's use string fields as an example)
  DBUG_EXECUTE_IF("frm_data_type_info_emulate",
                  if (cmp_type() == STRING_RESULT)
                    return to->append_char('x') ||
                           to->append(name().lex_cstring()););
  if (type_collection() != &type_collection_std)
    return to->append(name().lex_cstring());
  return false;
}


/***************************************************************************/

void
Type_handler::partition_field_type_not_allowed(const LEX_CSTRING &field_name)
{
  my_error(ER_FIELD_TYPE_NOT_ALLOWED_AS_PARTITION_FIELD, MYF(0),
           field_name.str);
}


bool
Type_handler::partition_field_check_result_type(Item *item,
                                                Item_result expected_type)
{
  if (item->result_type() != expected_type)
  {
    my_error(ER_WRONG_TYPE_COLUMN_VALUE_ERROR, MYF(0));
    return TRUE;
  }
  return false;
}


bool
Type_handler_blob_common::partition_field_check(const LEX_CSTRING &field_name,
                                                Item *item_expr) const
{
  my_error(ER_BLOB_FIELD_IN_PART_FUNC_ERROR, MYF(0));
  return true;
}


bool
Type_handler_general_purpose_int::partition_field_append_value(
                                            String *str,
                                            Item *item_expr,
                                            CHARSET_INFO *field_cs,
                                            partition_value_print_mode_t mode)
                                            const
{
  DBUG_ASSERT(item_expr->cmp_type() == INT_RESULT);
  StringBuffer<LONGLONG_BUFFER_SIZE> tmp;
  longlong value= item_expr->val_int();
  tmp.set(value, system_charset_info);
  return str->append(tmp);
}


/*
  Append an Item value to a String using a desired mode.

  @param [OUT] str          The string to append the value to.
  @param       item_expr    The item to get the value from
  @param       field_cs     The character set of the value owner field.
  @param       mode         The mode.
  @retval      true         on error
  @retval      false        on success

  The value is added using system_charset_info (no matter what mode is).

 (1) If mode is equal to PARTITION_VALUE_PRINT_MODE_FRM,
     the value is appended as a pure ASCII string in the format '_latin1 0xdf',
     i.e. a character set introducer followed by a hex hybrid.

     Before appending, we value is first converted to field_cs.
     a) If the conversion succeeds, the value is printed in its field_cs
        represenation.
     b) If the conversion fails, the value is printed without conversion,
        using the original character set introducer followed by the original
        string hex representation.
        In this case, open_table_from_share() will later notice that
        the value cannot be actually stored to the field, and report
        the error. So here we don't need to report errors such as
        ER_PARTITION_FUNCTION_IS_NOT_ALLOWED.

 (2) If the mode is equal to PARTITION_VALUE_PRINT_SHOW,
     then the value is needed for:
     - SHOW CREATE TABLE, or
     - the PARTITION_DESCRIPTION column in a
       INFORMATION_SCHEMA.PARTITION query.

     The value generated here will be later sent to the client and
     therefore will be converted to the client character set in the protocol.

     We try to generate the value as a simple quoted utf8 string without
     introducers (e.g. 'utf8-string') when possible, to make it:
     - as human readable as possible
     - but still safe for mysqldump purposes.

     Simple quoted utf8 string is generated when these two conditions are true
     at the same time:
       a) The value can be safely converted to utf8,
          so we can return it without data loss from this function.
       b) The value can be safely converted to the client character set,
          so we can convert it later without data loss to the client character
          set in the protocol.

     If one of the conditions fail, the value is returned using
     PARTITION_VALUE_PRINT_MODE_FRM representation. See (1).
*/
bool Type_handler::partition_field_append_value(
                                            String *str,
                                            Item *item_expr,
                                            CHARSET_INFO *field_cs,
                                            partition_value_print_mode_t mode)
                                            const
{
  DBUG_ASSERT(cmp_type() != INT_RESULT);
  StringBuffer<MAX_KEY_LENGTH> buf;
  String *res;

  if (!(res= item_expr->val_str(&buf)))
    return str->append(NULL_clex_str, system_charset_info);

  if (!res->length())
    return str->append(STRING_WITH_LEN("''"), system_charset_info);

  if (mode == PARTITION_VALUE_PRINT_MODE_FRM ||
      !res->can_be_safely_converted_to(current_thd->
                                         variables.character_set_client) ||
      !res->can_be_safely_converted_to(system_charset_info))
  {
    StringBuffer<64> buf2;
    uint cnverr2= 0;
    buf2.copy(res->ptr(), res->length(), res->charset(), field_cs, &cnverr2);
    if (!cnverr2)
      return str->append_introducer_and_hex(&buf2);
    return str->append_introducer_and_hex(res);
  }

  StringBuffer<64> val(system_charset_info);
  uint cnverr= 0;
  val.copy(res->ptr(), res->length(), res->charset(),
           system_charset_info, &cnverr);
  append_unescaped(str, val.ptr(), val.length());
  return false;
}


bool Type_handler::can_return_extract_source(interval_type int_type) const
{
  return type_collection() == &type_collection_std;
}

/***************************************************************************/

LEX_CSTRING Charset::collation_specific_name() const
{
  /*
    User defined collations can provide arbitrary names
    for character sets and collations, so a collation
    name not necessarily starts with the character set name.
  */
  size_t cs_name_length= m_charset->cs_name.length;
  if (strncmp(m_charset->coll_name.str, m_charset->cs_name.str,
              cs_name_length))
    return {NULL, 0};
  const char *ptr= m_charset->coll_name.str + cs_name_length;
  return {ptr, m_charset->coll_name.length - cs_name_length };
}


bool
Charset::encoding_allows_reinterpret_as(const CHARSET_INFO *cs) const
{
  if (my_charset_same(m_charset, cs))
    return true;

  if (!strcmp(m_charset->cs_name.str, MY_UTF8MB3) &&
      !strcmp(cs->cs_name.str, MY_UTF8MB4))
    return true;

  /*
    Originally we allowed here instant ALTER for ASCII-to-LATIN1
    and UCS2-to-UTF16, but this was wrong:
    - MariaDB's ascii is not a subset for 8-bit character sets
      like latin1, because it allows storing bytes 0x80..0xFF as
      "unassigned" characters (see MDEV-19285).
    - MariaDB's ucs2 (as in Unicode-1.1) is not a subset for UTF16,
      because they treat surrogate codes differently (MDEV-19284).
  */
  return false;
}


bool
Charset::eq_collation_specific_names(CHARSET_INFO *cs) const
{
  LEX_CSTRING name0= collation_specific_name();
  LEX_CSTRING name1= Charset(cs).collation_specific_name();
  return name0.length && !cmp(&name0, &name1);
}

int initialize_data_type_plugin(st_plugin_int *plugin)
{
  st_mariadb_data_type *data= (st_mariadb_data_type*) plugin->plugin->info;
  data->type_handler->set_name(Name(plugin->name));
  if (plugin->plugin->init && plugin->plugin->init(NULL))
  {
    sql_print_error("Plugin '%s' init function returned error.",
                    plugin->name.str);
    return 1;
  }
  return 0;
}
