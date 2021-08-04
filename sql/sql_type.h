#ifndef SQL_TYPE_H_INCLUDED
#define SQL_TYPE_H_INCLUDED
/*
   Copyright (c) 2015  MariaDB Foundation.
   Copyright (c) 2015, 2020, MariaDB Corporation.

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

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


#include "mysqld.h"
#include "lex_string.h"
#include "sql_array.h"
#include "sql_const.h"
#include "sql_time.h"
#include "sql_type_string.h"
#include "sql_type_real.h"
#include "compat56.h"
C_MODE_START
#include <ma_dyncol.h>
C_MODE_END

class Field;
class Column_definition;
class Column_definition_attributes;
class Key_part_spec;
class Item;
class Item_const;
class Item_literal;
class Item_param;
class Item_cache;
class Item_copy;
class Item_func_or_sum;
class Item_sum_hybrid;
class Item_sum_sum;
class Item_sum_avg;
class Item_sum_variance;
class Item_func_hex;
class Item_hybrid_func;
class Item_func_min_max;
class Item_func_hybrid_field_type;
class Item_bool_func2;
class Item_func_between;
class Item_func_in;
class Item_func_round;
class Item_func_int_val;
class Item_func_abs;
class Item_func_neg;
class Item_func_signed;
class Item_func_unsigned;
class Item_double_typecast;
class Item_float_typecast;
class Item_decimal_typecast;
class Item_char_typecast;
class Item_time_typecast;
class Item_date_typecast;
class Item_datetime_typecast;
class Item_func_plus;
class Item_func_minus;
class Item_func_mul;
class Item_func_div;
class Item_func_mod;
class Item_type_holder;
class cmp_item;
class in_vector;
class Type_handler_data;
class Type_handler_hybrid_field_type;
class Sort_param;
class Arg_comparator;
class Spvar_definition;
class st_value;
class Protocol;
class handler;
struct TABLE;
struct SORT_FIELD_ATTR;
struct SORT_FIELD;
class Vers_history_point;
class Virtual_column_info;
class Conv_source;
class ST_FIELD_INFO;
class Type_collection;
class Create_func;

#define my_charset_numeric      my_charset_latin1

enum protocol_send_type_t
{
  PROTOCOL_SEND_STRING,
  PROTOCOL_SEND_FLOAT,
  PROTOCOL_SEND_DOUBLE,
  PROTOCOL_SEND_TINY,
  PROTOCOL_SEND_SHORT,
  PROTOCOL_SEND_LONG,
  PROTOCOL_SEND_LONGLONG,
  PROTOCOL_SEND_DATETIME,
  PROTOCOL_SEND_DATE,
  PROTOCOL_SEND_TIME
};


enum scalar_comparison_op
{
  SCALAR_CMP_EQ,
  SCALAR_CMP_EQUAL,
  SCALAR_CMP_LT,
  SCALAR_CMP_LE,
  SCALAR_CMP_GE,
  SCALAR_CMP_GT
};


enum partition_value_print_mode_t
{
  PARTITION_VALUE_PRINT_MODE_SHOW= 0,
  PARTITION_VALUE_PRINT_MODE_FRM= 1
};


enum column_definition_type_t
{
  COLUMN_DEFINITION_TABLE_FIELD,
  COLUMN_DEFINITION_ROUTINE_PARAM,
  COLUMN_DEFINITION_ROUTINE_LOCAL,
  COLUMN_DEFINITION_FUNCTION_RETURN
};


class Send_field_extended_metadata
{
  LEX_CSTRING m_attr[MARIADB_FIELD_ATTR_LAST+1];
public:
  Send_field_extended_metadata()
  {
    bzero(this, sizeof(*this));
  }
  bool set_data_type_name(const LEX_CSTRING &str)
  {
    m_attr[MARIADB_FIELD_ATTR_DATA_TYPE_NAME]= str;
    return false;
  }
  bool set_format_name(const LEX_CSTRING &str)
  {
    m_attr[MARIADB_FIELD_ATTR_FORMAT_NAME]= str;
    return false;
  }
  bool has_extended_metadata() const
  {
    for (uint i= 0; i <= MARIADB_FIELD_ATTR_LAST; i++)
    {
      if (m_attr[i].str)
        return true;
    }
    return false;
  }
  const LEX_CSTRING &attr(uint i) const
  {
    DBUG_ASSERT(i <= MARIADB_FIELD_ATTR_LAST);
    return m_attr[i];
  }
};


class Data_type_statistics
{
public:
  uint m_uneven_bit_length;
  uint m_fixed_string_total_length;
  uint m_fixed_string_count;
  uint m_variable_string_total_length;
  uint m_variable_string_count;
  uint m_blob_count;
  Data_type_statistics()
   :m_uneven_bit_length(0),
    m_fixed_string_total_length(0),
    m_fixed_string_count(0),
    m_variable_string_total_length(0),
    m_variable_string_count(0),
    m_blob_count(0)
  { }
  uint string_count() const
  {
    return m_fixed_string_count + m_variable_string_count;
  }
  uint string_total_length() const
  {
    return m_fixed_string_total_length + m_variable_string_total_length;
  }
};


class Typelib: public TYPELIB
{
public:
  Typelib(uint count, const char **type_names, unsigned int *type_lengths)
  {
    TYPELIB::count= count;
    TYPELIB::name= "";
    TYPELIB::type_names= type_names;
    TYPELIB::type_lengths= type_lengths;
  }
  uint max_octet_length() const
  {
    uint max_length= 0;
    for (uint i= 0; i < TYPELIB::count; i++)
    {
      const uint length= TYPELIB::type_lengths[i];
      set_if_bigger(max_length, length);
    }
    return max_length;
  }
};


template<uint sz>
class TypelibBuffer: public Typelib
{
  const char *m_type_names[sz + 1];
  uint m_type_lengths[sz + 1];
public:
  TypelibBuffer(uint count, const LEX_CSTRING *values)
   :Typelib(count, m_type_names, m_type_lengths)
  {
    DBUG_ASSERT(sz >= count);
    for (uint i= 0; i <  count; i++)
    {
      DBUG_ASSERT(values[i].str != NULL);
      m_type_names[i]= values[i].str;
      m_type_lengths[i]= (uint) values[i].length;
    }
    m_type_names[sz]= NullS; // End marker
    m_type_lengths[sz]= 0;   // End marker
  }
  TypelibBuffer(const LEX_CSTRING *values)
   :TypelibBuffer(sz, values)
  { }
};


/*
  A helper class to store column attributes that are inherited
  by columns (from the table level) when not specified explicitly.
*/
class Column_derived_attributes
{
  /*
    Table level CHARACTER SET and COLLATE value:

      CREATE TABLE t1 (a VARCHAR(1), b CHAR(2)) CHARACTER SET latin1;

    All character string columns (CHAR, VARCHAR, TEXT)
    inherit CHARACTER SET from the table level.
  */
  CHARSET_INFO *m_charset;
public:
  explicit Column_derived_attributes(CHARSET_INFO *cs)
   :m_charset(cs)
  { }
  CHARSET_INFO *charset() const { return m_charset; }
};


/*
  A helper class to store requests for changes
  in multiple column data types during ALTER.
*/
class Column_bulk_alter_attributes
{
  /*
    Target CHARACTER SET specification in ALTER .. CONVERT, e.g.

      ALTER TABLE t1 CONVERT TO CHARACTER SET utf8;

    All character string columns (CHAR, VARCHAR, TEXT)
    get converted to the "CONVERT TO CHARACTER SET".
  */
  CHARSET_INFO *m_alter_table_convert_to_charset;
public:
  explicit Column_bulk_alter_attributes(CHARSET_INFO *convert)
   :m_alter_table_convert_to_charset(convert)
  { }
  CHARSET_INFO *alter_table_convert_to_charset() const
  { return m_alter_table_convert_to_charset; }
};


class Native: public Binary_string
{
public:
  Native(char *str, size_t len)
   :Binary_string(str, len)
  { }
};


template<size_t buff_sz>
class NativeBuffer: public Native
{
  char buff[buff_sz];
public:
  NativeBuffer() : Native(buff, buff_sz) { length(0); }
};


class String_ptr
{
protected:
  String *m_string_ptr;
public:
  String_ptr(String *str)
   :m_string_ptr(str)
  { }
  String_ptr(Item *item, String *buffer);
  const String *string() const
  {
    DBUG_ASSERT(m_string_ptr);
    return m_string_ptr;
  }
  bool is_null() const { return m_string_ptr == NULL; }
};


class Ascii_ptr: public String_ptr
{
public:
  Ascii_ptr(Item *item, String *buffer);
};


template<size_t buff_sz>
class String_ptr_and_buffer: public StringBuffer<buff_sz>,
                             public String_ptr
{
public:
  String_ptr_and_buffer(Item *item)
   :String_ptr(item, this)
  { }
};


template<size_t buff_sz>
class Ascii_ptr_and_buffer: public StringBuffer<buff_sz>,
                            public Ascii_ptr
{
public:
  Ascii_ptr_and_buffer(Item *item)
   :Ascii_ptr(item, this)
  { }
};


class Dec_ptr
{
protected:
  my_decimal *m_ptr;
  Dec_ptr() { }
public:
  Dec_ptr(my_decimal *ptr) :m_ptr(ptr) { }
  bool is_null() const { return m_ptr == NULL; }
  const my_decimal *ptr() const { return m_ptr; }
  const my_decimal *ptr_or(const my_decimal *def) const
  {
    return m_ptr ? m_ptr : def;
  }
  my_decimal *to_decimal(my_decimal *to) const
  {
    if (!m_ptr)
      return NULL;
    *to= *m_ptr;
    return to;
  }
  double to_double() const { return m_ptr ? m_ptr->to_double() : 0.0; }
  longlong to_longlong(bool unsigned_flag)
  { return m_ptr ? m_ptr->to_longlong(unsigned_flag) : 0; }
  Longlong_null to_xlonglong_null()
  {
    return m_ptr ? Longlong_null(m_ptr->to_xlonglong()) : Longlong_null();
  }
  bool to_bool() const { return m_ptr ? m_ptr->to_bool() : false; }
  String *to_string(String *to) const
  {
    return m_ptr ? m_ptr->to_string(to) : NULL;
  }
  String *to_string(String *to, uint prec, uint dec, char filler)
  {
    return m_ptr ? m_ptr->to_string(to, prec, dec, filler) : NULL;
  }
  int to_binary(uchar *bin, int prec, decimal_digits_t scale) const
  {
    return (m_ptr ? m_ptr : &decimal_zero)->to_binary(bin, prec, scale);
  }
  int cmp(const my_decimal *dec) const
  {
    DBUG_ASSERT(m_ptr);
    DBUG_ASSERT(dec);
    return m_ptr->cmp(dec);
  }
  int cmp(const Dec_ptr &other) const
  {
    return cmp(other.m_ptr);
  }
};


// A helper class to handle results of val_decimal(), date_op(), etc.
class Dec_ptr_and_buffer: public Dec_ptr
{
protected:
  my_decimal m_buffer;
public:
  /* scale is int as it can be negative here */
  int round_to(my_decimal *to, int scale, decimal_round_mode mode)
  {
    DBUG_ASSERT(m_ptr);
    return m_ptr->round_to(to, scale, mode);
  }
  int round_self(decimal_digits_t scale, decimal_round_mode mode)
  {
    return round_to(&m_buffer, scale, mode);
  }
  int round_self_if_needed(int scale, decimal_round_mode mode)
  {
    if (scale >= m_ptr->frac)
      return E_DEC_OK;
    int res= m_ptr->round_to(&m_buffer, scale, mode);
    m_ptr= &m_buffer;
    return res;
  }
  String *to_string_round(String *to, decimal_digits_t dec)
  {
    /*
      decimal_round() allows from==to
      So it's save even if m_ptr points to m_buffer before this call:
    */
    return m_ptr ? m_ptr->to_string_round(to, dec, &m_buffer) : NULL;
  }
};


// A helper class to handle val_decimal() results.
class VDec: public Dec_ptr_and_buffer
{
public:
  VDec(): Dec_ptr_and_buffer() { }
  VDec(Item *item);
  void set(Item *a);
};


// A helper class to handler decimal_op() results.
class VDec_op: public Dec_ptr_and_buffer
{
public:
  VDec_op(Item_func_hybrid_field_type *item);
};


/*
  Get and cache val_decimal() values for two items.
  If the first value appears to be NULL, the second value is not evaluated.
*/
class VDec2_lazy
{
public:
  VDec m_a;
  VDec m_b;
  VDec2_lazy(Item *a, Item *b) :m_a(a)
  {
    if (!m_a.is_null())
      m_b.set(b);
  }
  bool has_null() const
  {
    return m_a.is_null() || m_b.is_null();
  }
};


/**
  Class Sec6 represents a fixed point value with 6 fractional digits.
  Used e.g. to convert double and my_decimal values to TIME/DATETIME.
*/

class Sec6
{
protected:
  ulonglong m_sec;       // The integer part, between 0 and LONGLONG_MAX
  ulong     m_usec;      // The fractional part, between 0 and 999999
  bool      m_neg;       // false if positive, true of negative
  bool      m_truncated; // Indicates if the constructor truncated the value
  void make_from_decimal(const my_decimal *d, ulong *nanoseconds);
  void make_from_double(double d, ulong *nanoseconds);
  void make_from_int(const Longlong_hybrid &nr)
  {
    m_neg= nr.neg();
    m_sec= nr.abs();
    m_usec= 0;
    m_truncated= false;
  }
  void reset()
  {
    m_sec= m_usec= m_neg= m_truncated= 0;
  }
  Sec6() { }
  bool add_nanoseconds(uint nanoseconds)
  {
    DBUG_ASSERT(nanoseconds <= 1000000000);
    if (nanoseconds < 500)
      return false;
    m_usec+= (nanoseconds + 500) / 1000;
    if (m_usec < 1000000)
      return false;
    m_usec%= 1000000;
    return true;
  }
public:
  explicit Sec6(double nr)
  {
    ulong nanoseconds;
    make_from_double(nr, &nanoseconds);
  }
  explicit Sec6(const my_decimal *d)
  {
    ulong nanoseconds;
    make_from_decimal(d, &nanoseconds);
  }
  explicit Sec6(const Longlong_hybrid &nr)
  {
    make_from_int(nr);
  }
  explicit Sec6(longlong nr, bool unsigned_val)
  {
    make_from_int(Longlong_hybrid(nr, unsigned_val));
  }
  bool neg() const { return m_neg; }
  bool truncated() const { return m_truncated; }
  ulonglong sec() const { return m_sec; }
  long usec() const { return m_usec; }
  /**
    Converts Sec6 to MYSQL_TIME
    @param thd           current thd
    @param [out] warn    conversion warnings will be written here
    @param [out] ltime   converted value will be written here
    @param fuzzydate     conversion flags (TIME_INVALID_DATE, etc)
    @returns false for success, true for a failure
  */
  bool convert_to_mysql_time(THD *thd,
                             int *warn,
                             MYSQL_TIME *ltime,
                             date_mode_t fuzzydate) const;

protected:

  bool to_interval_hhmmssff_only(MYSQL_TIME *to, int *warn) const
  {
    return number_to_time_only(m_neg, m_sec, m_usec,
                               TIME_MAX_INTERVAL_HOUR, to, warn);
  }
  bool to_datetime_or_to_interval_hhmmssff(MYSQL_TIME *to, int *warn) const
  {
    /*
      Convert a number to a time interval.
      The following formats are understood:
      -            0 <= x <=   999999995959 - parse as hhhhmmss
      - 999999995959 <  x <= 99991231235959 - parse as YYYYMMDDhhmmss
       (YYMMDDhhmmss)       (YYYYMMDDhhmmss)

      Note, these formats are NOT understood:
      - YYMMDD       - overlaps with INTERVAL range
      - YYYYMMDD     - overlaps with INTERVAL range
      - YYMMDDhhmmss - overlaps with INTERVAL range, partially
                       (see TIME_MAX_INTERVAL_HOUR)

      If we ever need wider intervals, this code switching between
      full datetime and interval-only should be rewised.
    */
    DBUG_ASSERT(TIME_MAX_INTERVAL_HOUR <= 999999995959);
    /*            (YYMMDDhhmmss) */
    if (m_sec >    999999995959ULL &&
        m_sec <= 99991231235959ULL && m_neg == 0)
      return to_datetime_or_date(to, warn, TIME_INVALID_DATES);
    if (m_sec / 10000 > TIME_MAX_INTERVAL_HOUR)
    {
      *warn= MYSQL_TIME_WARN_OUT_OF_RANGE;
      return true;
    }
    return to_interval_hhmmssff_only(to, warn);
  }
public:
  // [-][DD]hhhmmss.ff,  YYMMDDhhmmss.ff, YYYYMMDDhhmmss.ff
  bool to_datetime_or_time(MYSQL_TIME *to, int *warn,
                           date_conv_mode_t mode) const
  {
    bool rc= m_sec > 9999999 && m_sec <= 99991231235959ULL && !m_neg ?
             ::number_to_datetime_or_date(m_sec, m_usec, to,
                        ulonglong(mode & TIME_MODE_FOR_XXX_TO_DATE), warn) < 0 :
             ::number_to_time_only(m_neg, m_sec, m_usec, TIME_MAX_HOUR, to, warn);
    DBUG_ASSERT(*warn || !rc);
    return rc;
  }
  /*
    Convert a number in formats YYYYMMDDhhmmss.ff or YYMMDDhhmmss.ff to
    TIMESTAMP'YYYY-MM-DD hh:mm:ss.ff'
  */
  bool to_datetime_or_date(MYSQL_TIME *to, int *warn,
                           date_conv_mode_t flags) const
  {
    if (m_neg)
    {
      *warn= MYSQL_TIME_WARN_OUT_OF_RANGE;
      return true;
    }
    bool rc= number_to_datetime_or_date(m_sec, m_usec, to,
                                ulonglong(flags & TIME_MODE_FOR_XXX_TO_DATE),
                                warn) == -1;
    DBUG_ASSERT(*warn || !rc);
    return rc;
  }
  // Convert elapsed seconds to TIME
  bool sec_to_time(MYSQL_TIME *ltime, uint dec) const
  {
    set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
    ltime->neg= m_neg;
    if (m_sec > TIME_MAX_VALUE_SECONDS)
    {
      // use check_time_range() to set ltime to the max value depending on dec
      int unused;
      ltime->hour= TIME_MAX_HOUR + 1;
      check_time_range(ltime, dec, &unused);
      return true;
    }
    DBUG_ASSERT(usec() <= TIME_MAX_SECOND_PART);
    ltime->hour=   (uint) (m_sec / 3600);
    ltime->minute= (uint) (m_sec % 3600) / 60;
    ltime->second= (uint) m_sec % 60;
    ltime->second_part= m_usec;
    return false;
  }
  Sec6 &trunc(uint dec)
  {
    m_usec-= my_time_fraction_remainder(m_usec, dec);
    return *this;
  }
  size_t to_string(char *to, size_t nbytes) const
  {
    return m_usec ?
      my_snprintf(to, nbytes, "%s%llu.%06lu",
                  m_neg ? "-" : "", m_sec, (uint) m_usec) :
      my_snprintf(to, nbytes, "%s%llu", m_neg ? "-" : "", m_sec);
  }
  void make_truncated_warning(THD *thd, const char *type_str) const;
};


class Sec9: public Sec6
{
protected:
  ulong m_nsec; // Nanoseconds 0..999
  void make_from_int(const Longlong_hybrid &nr)
  {
    Sec6::make_from_int(nr);
    m_nsec= 0;
  }
  Sec9() { }
public:
  Sec9(const my_decimal *d)
  {
    Sec6::make_from_decimal(d, &m_nsec);
  }
  Sec9(double d)
  {
    Sec6::make_from_double(d, &m_nsec);
  }
  ulong nsec() const { return m_nsec; }
  Sec9 &trunc(uint dec)
  {
    m_nsec= 0;
    Sec6::trunc(dec);
    return *this;
  }
  Sec9 &round(uint dec);
  Sec9 &round(uint dec, time_round_mode_t mode)
  {
    return mode == TIME_FRAC_TRUNCATE  ? trunc(dec) : round(dec);
  }
};


class VSec9: protected Sec9
{
  bool m_is_null;
  Sec9& to_sec9()
  {
    DBUG_ASSERT(!is_null());
    return *this;
  }
public:
  VSec9(THD *thd, Item *item, const char *type_str, ulonglong limit);
  bool is_null() const { return m_is_null; }
  const Sec9& to_const_sec9() const
  {
    DBUG_ASSERT(!is_null());
    return *this;
  }
  bool neg() const { return to_const_sec9().neg(); }
  bool truncated() const { return to_const_sec9().truncated(); }
  ulonglong sec() const { return to_const_sec9().sec(); }
  long usec() const { return to_const_sec9().usec(); }
  bool sec_to_time(MYSQL_TIME *ltime, uint dec) const
  {
    return to_const_sec9().sec_to_time(ltime, dec);
  }
  void make_truncated_warning(THD *thd, const char *type_str) const
  {
    return to_const_sec9().make_truncated_warning(thd, type_str);
  }
  Sec9 &round(uint dec)
  {
    return to_sec9().round(dec);
  }
  Sec9 &round(uint dec, time_round_mode_t mode)
  {
    return to_sec9().round(dec, mode);
  }
};


/*
  A heler class to perform additive operations between
  two MYSQL_TIME structures and return the result as a
  combination of seconds, microseconds and sign.
*/
class Sec6_add
{
  ulonglong m_sec; // number of seconds
  ulong m_usec;    // number of microseconds
  bool m_neg;      // false if positive, true if negative
  bool m_error;    // false if the value is OK, true otherwise
  void to_hh24mmssff(MYSQL_TIME *ltime, timestamp_type tstype) const
  {
    bzero(ltime, sizeof(*ltime));
    ltime->neg= m_neg;
    calc_time_from_sec(ltime, (ulong) (m_sec % SECONDS_IN_24H), m_usec);
    ltime->time_type= tstype;
  }
public:
  /*
    @param ltime1 - the first value to add (must be a valid DATE,TIME,DATETIME)
    @param ltime2 - the second value to add (must be a valid TIME)
    @param sign   - the sign of the operation
                    (+1 for addition, -1 for subtraction)
  */
  Sec6_add(const MYSQL_TIME *ltime1, const MYSQL_TIME *ltime2, int sign)
  {
    DBUG_ASSERT(sign == -1 || sign == 1);
    DBUG_ASSERT(!ltime1->neg || ltime1->time_type == MYSQL_TIMESTAMP_TIME);
    if (!(m_error= (ltime2->time_type != MYSQL_TIMESTAMP_TIME)))
    {
      if (ltime1->neg != ltime2->neg)
        sign= -sign;
      m_neg= calc_time_diff(ltime1, ltime2, -sign, &m_sec, &m_usec);
      if (ltime1->neg && (m_sec || m_usec))
        m_neg= !m_neg; // Swap sign
    }
  }
  bool to_time(THD *thd, MYSQL_TIME *ltime, uint decimals) const
  {
    if (m_error)
      return true;
    to_hh24mmssff(ltime, MYSQL_TIMESTAMP_TIME);
    ltime->hour+= static_cast<unsigned>(to_days_abs() * 24);
    return adjust_time_range_with_warn(thd, ltime, decimals);
  }
  bool to_datetime(MYSQL_TIME *ltime) const
  {
    if (m_error || m_neg)
      return true;
    to_hh24mmssff(ltime, MYSQL_TIMESTAMP_DATETIME);
    return get_date_from_daynr(to_days_abs(),
                               &ltime->year, &ltime->month, &ltime->day) ||
           !ltime->day;
  }
  long to_days_abs() const { return (long) (m_sec / SECONDS_IN_24H); }
};


class Year
{
protected:
  uint m_year;
  bool m_truncated;
  uint year_precision(const Item *item) const;
public:
  Year(): m_year(0), m_truncated(false) { }
  Year(longlong value, bool unsigned_flag, uint length);
  uint year() const { return m_year; }
  uint to_YYYYMMDD() const { return m_year * 10000; }
  bool truncated() const { return m_truncated; }
};


class Year_null: public Year, public Null_flag
{
public:
  Year_null(const Longlong_null &nr, bool unsigned_flag, uint length)
   :Year(nr.is_null() ? 0 : nr.value(), unsigned_flag, length),
    Null_flag(nr.is_null())
  { }
};


class VYear: public Year_null
{
public:
  VYear(Item *item);
};


class VYear_op: public Year_null
{
public:
  VYear_op(Item_func_hybrid_field_type *item);
};


class Double_null: public Null_flag
{
protected:
  double m_value;
public:
  Double_null(double value, bool is_null)
   :Null_flag(is_null), m_value(value)
  { }
  double value() const { return m_value; }
};


class Temporal: protected MYSQL_TIME
{
public:
  class Status: public MYSQL_TIME_STATUS
  {
  public:
    Status() { my_time_status_init(this); }
  };

  class Warn: public ErrBuff,
              public Status
  {
  public:
    void push_conversion_warnings(THD *thd, bool totally_useless_value,
                                  date_mode_t mode, timestamp_type tstype,
                                  const char *db_name, const char *table_name,
                                  const char *name)
    {
      const char *typestr= tstype >= 0 ? type_name_by_timestamp_type(tstype) :
                           mode & (TIME_INTERVAL_hhmmssff | TIME_INTERVAL_DAY) ?
                           "interval" :
                           mode & TIME_TIME_ONLY ? "time" : "datetime";
      Temporal::push_conversion_warnings(thd, totally_useless_value, warnings,
                                         typestr, db_name, table_name, name,
                                         ptr());
    }
  };

  class Warn_push: public Warn
  {
    THD * const m_thd;
    const char * const m_db_name;
    const char * const m_table_name;
    const char * const m_name;
    const MYSQL_TIME * const m_ltime;
    const date_mode_t m_mode;
  public:
    Warn_push(THD *thd, const char *db_name, const char *table_name,
              const char *name, const MYSQL_TIME *ltime, date_mode_t mode)
      : m_thd(thd), m_db_name(db_name), m_table_name(table_name), m_name(name),
        m_ltime(ltime), m_mode(mode)
    { }
    ~Warn_push()
    {
      if (warnings)
        push_conversion_warnings(m_thd, m_ltime->time_type < 0,
                                 m_mode, m_ltime->time_type,
                                 m_db_name, m_table_name, m_name);
    }
  };

public:
  static date_conv_mode_t sql_mode_for_dates(THD *thd);
  static time_round_mode_t default_round_mode(THD *thd);
  class Options: public date_mode_t
  {
  public:
    explicit Options(date_mode_t flags)
     :date_mode_t(flags)
    { }
    Options(date_conv_mode_t flags, time_round_mode_t round_mode)
     :date_mode_t(flags | round_mode)
    {
      DBUG_ASSERT(ulonglong(flags) <= UINT_MAX32);
    }
    Options(date_conv_mode_t flags, THD *thd)
     :Options(flags, default_round_mode(thd))
    { }
  };

  bool is_valid_temporal() const
  {
    DBUG_ASSERT(time_type != MYSQL_TIMESTAMP_ERROR);
    return time_type != MYSQL_TIMESTAMP_NONE;
  }
  static const char *type_name_by_timestamp_type(timestamp_type time_type)
  {
    switch (time_type) {
      case MYSQL_TIMESTAMP_DATE: return "date";
      case MYSQL_TIMESTAMP_TIME: return "time";
      case MYSQL_TIMESTAMP_DATETIME:  // FALLTHROUGH
      default:
        break;
    }
    return "datetime";
  }
  static void push_conversion_warnings(THD *thd, bool totally_useless_value, int warn,
                                       const char *type_name,
                                       const char *db_name,
                                       const char *table_name,
                                       const char *field_name,
                                       const char *value);
  /*
    This method is used if the item was not null but convertion to
    TIME/DATE/DATETIME failed. We return a zero date if allowed,
    otherwise - null.
  */
  void make_fuzzy_date(int *warn, date_conv_mode_t fuzzydate)
  {
    /*
      In the following scenario:
      - The caller expected to get a TIME value
      - Item returned a not NULL string or numeric value
      - But then conversion from string or number to TIME failed
      we need to change the default time_type from MYSQL_TIMESTAMP_DATE
      (which was set in bzero) to MYSQL_TIMESTAMP_TIME and therefore
      return TIME'00:00:00' rather than DATE'0000-00-00'.
      If we don't do this, methods like Item::get_time_with_conversion()
      will erroneously subtract CURRENT_DATE from '0000-00-00 00:00:00'
      and return TIME'-838:59:59' instead of TIME'00:00:00' as a result.
    */
    timestamp_type tstype= !(fuzzydate & TIME_FUZZY_DATES) ?
                           MYSQL_TIMESTAMP_NONE :
                           fuzzydate & TIME_TIME_ONLY ?
                           MYSQL_TIMESTAMP_TIME :
                           MYSQL_TIMESTAMP_DATETIME;
    set_zero_time(this, tstype);
  }

protected:
  my_decimal *bad_to_decimal(my_decimal *to) const;
  my_decimal *to_decimal(my_decimal *to) const;
  static double to_double(bool negate, ulonglong num, ulong frac)
  {
    double d= static_cast<double>(num) + static_cast<double>(frac) /
      TIME_SECOND_PART_FACTOR;
    return negate ? -d : d;
  }
  longlong to_packed() const { return ::pack_time(this); }
  void make_from_out_of_range(int *warn)
  {
    *warn= MYSQL_TIME_WARN_OUT_OF_RANGE;
    time_type= MYSQL_TIMESTAMP_NONE;
  }
  void make_from_sec6(THD *thd, MYSQL_TIME_STATUS *st,
                      const Sec6 &nr, date_mode_t mode)
  {
    if (nr.convert_to_mysql_time(thd, &st->warnings, this, mode))
      make_fuzzy_date(&st->warnings, date_conv_mode_t(mode));
  }
  void make_from_sec9(THD *thd, MYSQL_TIME_STATUS *st,
                      const Sec9 &nr, date_mode_t mode)
  {
    if (nr.convert_to_mysql_time(thd, &st->warnings, this, mode) ||
        add_nanoseconds(thd, &st->warnings, mode, nr.nsec()))
      make_fuzzy_date(&st->warnings, date_conv_mode_t(mode));
  }
  void make_from_str(THD *thd, Warn *warn,
                     const char *str, size_t length, CHARSET_INFO *cs,
                     date_mode_t fuzzydate);
  void make_from_double(THD *thd, Warn *warn, double nr, date_mode_t mode)
  {
    make_from_sec9(thd, warn, Sec9(nr), mode);
    if (warn->warnings)
      warn->set_double(nr);
  }
  void make_from_longlong_hybrid(THD *thd, Warn *warn,
                                 const Longlong_hybrid &nr, date_mode_t mode)
  {
    /*
      Note: conversion from an integer to TIME can overflow to
      '838:59:59.999999', so the conversion result can have fractional digits.
    */
    make_from_sec6(thd, warn, Sec6(nr), mode);
    if (warn->warnings)
      warn->set_longlong(nr);
  }
  void make_from_decimal(THD *thd, Warn *warn,
                         const my_decimal *nr, date_mode_t mode)
  {
    make_from_sec9(thd, warn, Sec9(nr), mode);
    if (warn->warnings)
      warn->set_decimal(nr);
  }
  bool ascii_to_temporal(MYSQL_TIME_STATUS *st,
                         const char *str, size_t length,
                         date_mode_t mode)
  {
    if (mode & (TIME_INTERVAL_hhmmssff | TIME_INTERVAL_DAY))
      return ascii_to_datetime_or_date_or_interval_DDhhmmssff(st, str, length,
                                                              mode);
    if (mode & TIME_TIME_ONLY)
      return ascii_to_datetime_or_date_or_time(st, str, length, mode);
    return ascii_to_datetime_or_date(st, str, length, mode);
  }
  bool ascii_to_datetime_or_date_or_interval_DDhhmmssff(MYSQL_TIME_STATUS *st,
                                                        const char *str,
                                                        size_t length,
                                                        date_mode_t mode)
  {
    longlong cflags= ulonglong(mode & TIME_MODE_FOR_XXX_TO_DATE);
    bool rc= mode & TIME_INTERVAL_DAY ?
      ::str_to_datetime_or_date_or_interval_day(str, length, this, cflags, st,
                                                TIME_MAX_INTERVAL_HOUR,
                                                TIME_MAX_INTERVAL_HOUR) :
      ::str_to_datetime_or_date_or_interval_hhmmssff(str, length, this,
                                                     cflags, st,
                                                     TIME_MAX_INTERVAL_HOUR,
                                                     TIME_MAX_INTERVAL_HOUR);
    DBUG_ASSERT(!rc || st->warnings);
    return rc;
  }
  bool ascii_to_datetime_or_date_or_time(MYSQL_TIME_STATUS *status,
                                         const char *str, size_t length,
                                         date_mode_t fuzzydate)
  {
    ulonglong cflags= ulonglong(fuzzydate & TIME_MODE_FOR_XXX_TO_DATE);
    bool rc= ::str_to_datetime_or_date_or_time(str, length, this,
                                               cflags, status,
                                               TIME_MAX_HOUR, UINT_MAX32);
    DBUG_ASSERT(!rc || status->warnings);
    return rc;
  }
  bool ascii_to_datetime_or_date(MYSQL_TIME_STATUS *status,
                                 const char *str, size_t length,
                                 date_mode_t fuzzydate)
  {
    DBUG_ASSERT(bool(fuzzydate & TIME_TIME_ONLY) == false);
    bool rc= ::str_to_datetime_or_date(str, length, this,
                             ulonglong(fuzzydate & TIME_MODE_FOR_XXX_TO_DATE),
                             status);
    DBUG_ASSERT(!rc || status->warnings);
    return rc;
  }
  // Character set aware versions for string conversion routines
  bool str_to_temporal(THD *thd, MYSQL_TIME_STATUS *st,
                       const char *str, size_t length,
                       CHARSET_INFO *cs, date_mode_t fuzzydate);
  bool str_to_datetime_or_date_or_time(THD *thd, MYSQL_TIME_STATUS *st,
                                       const char *str, size_t length,
                                       CHARSET_INFO *cs, date_mode_t mode);
  bool str_to_datetime_or_date(THD *thd, MYSQL_TIME_STATUS *st,
                               const char *str, size_t length,
                               CHARSET_INFO *cs, date_mode_t mode);

  bool has_valid_mmssff() const
  {
    return minute <= TIME_MAX_MINUTE &&
           second <= TIME_MAX_SECOND &&
           second_part <= TIME_MAX_SECOND_PART;
  }
  bool has_zero_YYYYMM() const
  {
    return year == 0 && month == 0;
  }
  bool has_zero_YYYYMMDD() const
  {
    return year == 0 && month == 0 && day == 0;
  }
  bool check_date(date_conv_mode_t flags, int *warn) const
  {
    return ::check_date(this, flags, warn);
  }
  void time_hhmmssff_set_max(uint max_hour)
  {
    hour= max_hour;
    minute= TIME_MAX_MINUTE;
    second= TIME_MAX_SECOND;
    second_part= TIME_MAX_SECOND_PART;
  }
  /*
    Add nanoseconds to ssff
    retval   true if seconds overflowed (the caller should increment minutes)
             false if no overflow happened
  */
  bool add_nanoseconds_ssff(uint nanoseconds)
  {
    DBUG_ASSERT(nanoseconds <= 1000000000);
    if (nanoseconds < 500)
      return false;
    second_part+= (nanoseconds + 500) / 1000;
    if (second_part < 1000000)
      return false;
    second_part%= 1000000;
    if (second < 59)
    {
      second++;
      return false;
    }
    second= 0;
    return true;
  }
  /*
    Add nanoseconds to mmssff
    retval   true if hours overflowed (the caller should increment hours)
             false if no overflow happened
  */
  bool add_nanoseconds_mmssff(uint nanoseconds)
  {
    if (!add_nanoseconds_ssff(nanoseconds))
      return false;
    if (minute < 59)
    {
      minute++;
      return false;
    }
    minute= 0;
    return true;
  }
  void time_round_or_set_max(uint dec, int *warn, ulong max_hour, ulong nsec);
  bool datetime_add_nanoseconds_or_invalidate(THD *thd, int *warn, ulong nsec);
  bool datetime_round_or_invalidate(THD *thd, uint dec, int *warn, ulong nsec);
  bool add_nanoseconds_with_round(THD *thd, int *warn,
                                  date_conv_mode_t mode, ulong nsec);
  bool add_nanoseconds(THD *thd, int *warn, date_mode_t mode, ulong nsec)
  {
    date_conv_mode_t cmode= date_conv_mode_t(mode);
    return time_round_mode_t(mode) == TIME_FRAC_ROUND ?
           add_nanoseconds_with_round(thd, warn, cmode, nsec) : false;
  }
public:
  static void *operator new(size_t size, MYSQL_TIME *ltime) throw()
  {
    DBUG_ASSERT(size == sizeof(MYSQL_TIME));
    return ltime;
  }
  static void operator delete(void *ptr, MYSQL_TIME *ltime) { }

  long fraction_remainder(uint dec) const
  {
    return my_time_fraction_remainder(second_part, dec);
  }
};


/*
  Use this class when you need to get a MYSQL_TIME from an Item
  using Item's native timestamp type, without automatic timestamp
  type conversion.
*/
class Temporal_hybrid: public Temporal
{
public:
  class Options: public Temporal::Options
  {
  public:
    Options(THD *thd)
     :Temporal::Options(sql_mode_for_dates(thd), default_round_mode(thd))
    { }
    Options(date_conv_mode_t flags, time_round_mode_t round_mode)
     :Temporal::Options(flags, round_mode)
    { }
    explicit Options(const Temporal::Options &opt)
     :Temporal::Options(opt)
    { }
    explicit Options(date_mode_t fuzzydate)
     :Temporal::Options(fuzzydate)
    { }
  };

public:
  // Contructors for Item
  Temporal_hybrid(THD *thd, Item *item, date_mode_t fuzzydate);
  Temporal_hybrid(THD *thd, Item *item)
   :Temporal_hybrid(thd, item, Options(thd))
  { }
  Temporal_hybrid(Item *item)
   :Temporal_hybrid(current_thd, item)
  { }

  // Constructors for non-NULL values
  Temporal_hybrid(THD *thd, Warn *warn,
                  const char *str, size_t length, CHARSET_INFO *cs,
                  date_mode_t fuzzydate)
  {
    make_from_str(thd, warn, str, length, cs, fuzzydate);
  }
  Temporal_hybrid(THD *thd, Warn *warn,
                  const Longlong_hybrid &nr, date_mode_t fuzzydate)
  {
    make_from_longlong_hybrid(thd, warn, nr, fuzzydate);
  }
  Temporal_hybrid(THD *thd, Warn *warn, double nr, date_mode_t fuzzydate)
  {
    make_from_double(thd, warn, nr, fuzzydate);
  }

  // Constructors for nullable values
  Temporal_hybrid(THD *thd, Warn *warn, const String *str, date_mode_t mode)
  {
    if (!str)
      time_type= MYSQL_TIMESTAMP_NONE;
    else
      make_from_str(thd, warn, str->ptr(), str->length(), str->charset(), mode);
  }
  Temporal_hybrid(THD *thd, Warn *warn,
                  const Longlong_hybrid_null &nr, date_mode_t fuzzydate)
  {
    if (nr.is_null())
      time_type= MYSQL_TIMESTAMP_NONE;
    else
      make_from_longlong_hybrid(thd, warn, nr, fuzzydate);
  }
  Temporal_hybrid(THD *thd, Warn *warn, const Double_null &nr, date_mode_t mode)
  {
    if (nr.is_null())
      time_type= MYSQL_TIMESTAMP_NONE;
    else
      make_from_double(thd, warn, nr.value(), mode);
  }
  Temporal_hybrid(THD *thd, Warn *warn, const my_decimal *nr, date_mode_t mode)
  {
    if (!nr)
      time_type= MYSQL_TIMESTAMP_NONE;
    else
      make_from_decimal(thd, warn, nr, mode);
  }
  // End of constuctors

  bool copy_valid_value_to_mysql_time(MYSQL_TIME *ltime) const
  {
    DBUG_ASSERT(is_valid_temporal());
    *ltime= *this;
    return false;
  }

  longlong to_longlong() const
  {
    if (!is_valid_temporal())
      return 0;
    ulonglong v= TIME_to_ulonglong(this);
    return neg ? -(longlong) v : (longlong) v;
  }
  double to_double() const
  {
    return is_valid_temporal() ? TIME_to_double(this) : 0;
  }
  my_decimal *to_decimal(my_decimal *to)
  {
    return is_valid_temporal() ? Temporal::to_decimal(to) : bad_to_decimal(to);
  }
  String *to_string(String *str, uint dec) const
  {
    if (!is_valid_temporal())
      return NULL;
    str->set_charset(&my_charset_numeric);
    if (!str->alloc(MAX_DATE_STRING_REP_LENGTH))
      str->length(my_TIME_to_str(this, const_cast<char*>(str->ptr()), dec));
    return str;
  }
  const MYSQL_TIME *get_mysql_time() const
  {
    DBUG_ASSERT(is_valid_temporal());
    return this;
  }
};


/*
  This class resembles the SQL standard <extract source>,
  used in extract expressions, e.g: EXTRACT(DAY FROM dt)
  <extract expression> ::=
    EXTRACT <left paren> <extract field> FROM <extract source> <right paren>
  <extract source> ::= <datetime value expression> | <interval value expression>
*/
class Extract_source: public Temporal_hybrid
{
  /*
    Convert a TIME value to DAY-TIME interval, e.g. for extraction:
      EXTRACT(DAY FROM x), EXTRACT(HOUR FROM x), etc.
    Moves full days from ltime->hour to ltime->day.
  */
  void time_to_daytime_interval()
  {
    DBUG_ASSERT(time_type == MYSQL_TIMESTAMP_TIME);
    DBUG_ASSERT(has_zero_YYYYMMDD());
    MYSQL_TIME::day= MYSQL_TIME::hour / 24;
    MYSQL_TIME::hour%= 24;
  }
  bool is_valid_extract_source_slow() const
  {
    return is_valid_temporal() && MYSQL_TIME::hour < 24 &&
           (has_zero_YYYYMM() || time_type != MYSQL_TIMESTAMP_TIME);
  }
  bool is_valid_value_slow() const
  {
    return time_type == MYSQL_TIMESTAMP_NONE || is_valid_extract_source_slow();
  }
public:
  Extract_source(THD *thd, Item *item, date_mode_t mode)
   :Temporal_hybrid(thd, item, mode)
  {
    if (MYSQL_TIME::time_type == MYSQL_TIMESTAMP_TIME)
      time_to_daytime_interval();
    DBUG_ASSERT(is_valid_value_slow());
  }
  inline const MYSQL_TIME *get_mysql_time() const
  {
    DBUG_ASSERT(is_valid_extract_source_slow());
    return this;
  }
  bool is_valid_extract_source() const { return is_valid_temporal(); }
  int sign() const { return get_mysql_time()->neg ? -1 : 1; }
  uint year() const { return get_mysql_time()->year; }
  uint month() const { return get_mysql_time()->month; }
  int day() const { return (int) get_mysql_time()->day * sign(); }
  int hour() const { return (int) get_mysql_time()->hour * sign(); }
  int minute() const { return (int) get_mysql_time()->minute * sign(); }
  int second() const { return (int) get_mysql_time()->second * sign(); }
  int microsecond() const { return (int) get_mysql_time()->second_part * sign(); }

  uint year_month() const { return year() * 100 + month(); }
  uint quarter() const { return (month() + 2)/3; }
  uint week(THD *thd) const;

  longlong second_microsecond() const
  {
    return (second() * 1000000LL + microsecond());
  }

  // DAY TO XXX
  longlong day_hour() const
  {
    return (longlong) day() * 100LL + hour();
  }
  longlong day_minute() const
  {
    return day_hour() * 100LL + minute();
  }
  longlong day_second() const
  {
    return day_minute() * 100LL + second();
  }
  longlong day_microsecond() const
  {
    return day_second() * 1000000LL + microsecond();
  }

  // HOUR TO XXX
  int hour_minute() const
  {
    return hour() * 100 + minute();
  }
  int hour_second() const
  {
    return hour_minute() * 100 + second();
  }
  longlong hour_microsecond() const
  {
    return hour_second() * 1000000LL + microsecond();
  }

  // MINUTE TO XXX
  int minute_second() const
  {
    return minute() * 100 + second();
  }
  longlong minute_microsecond() const
  {
    return minute_second() * 1000000LL + microsecond();
  }
};


/*
  This class is used for the "time_interval" argument of these SQL functions:
    TIMESTAMP(tm,time_interval)
    ADDTIME(tm,time_interval)
  Features:
  - DATE and DATETIME formats are treated as errors
  - Preserves hours for TIME format as is, without limiting to TIME_MAX_HOUR
*/
class Interval_DDhhmmssff: public Temporal
{
  static const LEX_CSTRING m_type_name;
  bool str_to_DDhhmmssff(MYSQL_TIME_STATUS *status,
                         const char *str, size_t length, CHARSET_INFO *cs,
                         ulong max_hour);
  void push_warning_wrong_or_truncated_value(THD *thd,
                                             const ErrConv &str,
                                             int warnings);
  bool is_valid_interval_DDhhmmssff_slow() const
  {
    return time_type == MYSQL_TIMESTAMP_TIME &&
           has_zero_YYYYMMDD() && has_valid_mmssff();
  }
  bool is_valid_value_slow() const
  {
    return time_type == MYSQL_TIMESTAMP_NONE ||
           is_valid_interval_DDhhmmssff_slow();
  }
public:
  // Get fractional second precision from an Item
  static uint fsp(THD *thd, Item *item);
  /*
    Maximum useful HOUR value:
    TIMESTAMP'0001-01-01 00:00:00' + '87649415:59:59' = '9999-12-31 23:59:59'
    This gives maximum possible interval values:
    - '87649415:59:59.999999'   (in 'hh:mm:ss.ff' format)
    - '3652058 23:59:59.999999' (in 'DD hh:mm:ss.ff' format)
  */
  static uint max_useful_hour()
  {
    return TIME_MAX_INTERVAL_HOUR;
  }
  static uint max_int_part_char_length()
  {
    // e.g. '+3652058 23:59:59'
    return 1/*sign*/ + TIME_MAX_INTERVAL_DAY_CHAR_LENGTH + 1 + 8/*hh:mm:ss*/;
  }
  static uint max_char_length(uint fsp)
  {
    DBUG_ASSERT(fsp <= TIME_SECOND_PART_DIGITS);
    return max_int_part_char_length() + (fsp ? 1 : 0) + fsp;
  }

public:
  Interval_DDhhmmssff(THD *thd, Status *st, bool push_warnings,
                      Item *item, ulong max_hour,
                      time_round_mode_t mode, uint dec);
  Interval_DDhhmmssff(THD *thd, Item *item, uint dec)
  {
    Status st;
    new(this) Interval_DDhhmmssff(thd, &st, true, item, max_useful_hour(),
                                  default_round_mode(thd), dec);
  }
  Interval_DDhhmmssff(THD *thd, Item *item)
   :Interval_DDhhmmssff(thd, item, TIME_SECOND_PART_DIGITS)
  { }
  const MYSQL_TIME *get_mysql_time() const
  {
    DBUG_ASSERT(is_valid_interval_DDhhmmssff_slow());
    return this;
  }
  bool is_valid_interval_DDhhmmssff() const
  {
    return time_type == MYSQL_TIMESTAMP_TIME;
  }
  bool is_valid_value() const
  {
    return time_type == MYSQL_TIMESTAMP_NONE || is_valid_interval_DDhhmmssff();
  }
  String *to_string(String *str, uint dec) const
  {
    if (!is_valid_interval_DDhhmmssff())
      return NULL;
    str->set_charset(&my_charset_numeric);
    if (!str->alloc(MAX_DATE_STRING_REP_LENGTH))
      str->length(my_interval_DDhhmmssff_to_str(this,
                                                const_cast<char*>(str->ptr()),
                                                dec));
    return str;
  }
};

class Schema;


/**
  Class Time is designed to store valid TIME values.

  1. Valid value:
    a. MYSQL_TIMESTAMP_TIME - a valid TIME within the supported TIME range
    b. MYSQL_TIMESTAMP_NONE - an undefined value

  2. Invalid value (internally only):
    a. MYSQL_TIMESTAMP_TIME outside of the supported TIME range
    a. MYSQL_TIMESTAMP_{DATE|DATETIME|ERROR}

  Temporarily Time is allowed to have an invalid value, but only internally,
  during initialization time. All constructors and modification methods must
  leave the Time value as described above (see "Valid values").

  Time derives from MYSQL_TIME privately to make sure it is accessed
  externally only in the valid state.
*/
class Time: public Temporal
{
  static uint binary_length_to_precision(uint length);
public:
  enum datetime_to_time_mode_t
  {
    DATETIME_TO_TIME_DISALLOW,
    DATETIME_TO_TIME_YYYYMMDD_000000DD_MIX_TO_HOURS,
    DATETIME_TO_TIME_YYYYMMDD_TRUNCATE,
    DATETIME_TO_TIME_YYYYMMDD_00000000_ONLY,
    DATETIME_TO_TIME_MINUS_CURRENT_DATE
  };
  class Options: public Temporal::Options
  {
    datetime_to_time_mode_t m_datetime_to_time_mode;
  public:
    Options(THD *thd)
     :Temporal::Options(default_flags_for_get_date(), default_round_mode(thd)),
      m_datetime_to_time_mode(default_datetime_to_time_mode())
    { }
    Options(date_conv_mode_t flags, THD *thd)
     :Temporal::Options(flags, default_round_mode(thd)),
      m_datetime_to_time_mode(default_datetime_to_time_mode())
    { }
    Options(date_conv_mode_t flags, THD *thd, datetime_to_time_mode_t dtmode)
     :Temporal::Options(flags, default_round_mode(thd)),
      m_datetime_to_time_mode(dtmode)
    { }
    Options(date_conv_mode_t fuzzydate, time_round_mode_t round_mode,
            datetime_to_time_mode_t datetime_to_time_mode)
     :Temporal::Options(fuzzydate, round_mode),
       m_datetime_to_time_mode(datetime_to_time_mode)
    { }

    datetime_to_time_mode_t datetime_to_time_mode() const
    { return m_datetime_to_time_mode; }

    static datetime_to_time_mode_t default_datetime_to_time_mode()
    {
      return DATETIME_TO_TIME_YYYYMMDD_000000DD_MIX_TO_HOURS;
    }
  };
  /*
    CAST(AS TIME) historically does not mix days to hours.
    This is different comparing to how implicit conversion
    in Field::store_time_dec() works (e.g. on INSERT).
  */
  class Options_for_cast: public Options
  {
  public:
    Options_for_cast(THD *thd)
     :Options(default_flags_for_get_date(), default_round_mode(thd),
              DATETIME_TO_TIME_YYYYMMDD_TRUNCATE)
    { }
    Options_for_cast(date_mode_t mode, THD *thd)
     :Options(default_flags_for_get_date() | (mode & TIME_FUZZY_DATES),
              default_round_mode(thd),
              DATETIME_TO_TIME_YYYYMMDD_TRUNCATE)
    { }
  };

  class Options_for_round: public Options
  {
  public:
    Options_for_round(time_round_mode_t round_mode= TIME_FRAC_TRUNCATE)
     :Options(Time::default_flags_for_get_date(), round_mode,
              Time::DATETIME_TO_TIME_DISALLOW)
    { }
  };
  class Options_cmp: public Options
  {
  public:
    Options_cmp(THD *thd)
     :Options(comparison_flags_for_get_date(), thd)
    { }
    Options_cmp(THD *thd, datetime_to_time_mode_t dtmode)
     :Options(comparison_flags_for_get_date(),
              default_round_mode(thd), dtmode)
    { }
  };
private:
  bool is_valid_value_slow() const
  {
    return time_type == MYSQL_TIMESTAMP_NONE || is_valid_time_slow();
  }
  bool is_valid_time_slow() const
  {
    return time_type == MYSQL_TIMESTAMP_TIME &&
           has_zero_YYYYMMDD() && has_valid_mmssff();
  }
  void hhmmssff_copy(const MYSQL_TIME *from)
  {
    hour= from->hour;
    minute= from->minute;
    second= from->second;
    second_part= from->second_part;
  }
  void datetime_to_time_YYYYMMDD_000000DD_mix_to_hours(int *warn,
                                                       uint from_year,
                                                       uint from_month,
                                                       uint from_day)
  {
    if (from_year != 0 || from_month != 0)
      *warn|= MYSQL_TIME_NOTE_TRUNCATED;
    else
      hour+= from_day * 24;
  }
  /*
    The result is calculated effectively similar to:
    TIMEDIFF(dt, CAST(CURRENT_DATE AS DATETIME))
    If the difference does not fit to the supported TIME range, it's truncated.
  */
  void datetime_to_time_minus_current_date(THD *thd)
  {
    MYSQL_TIME current_date, tmp;
    set_current_date(thd, &current_date);
    calc_time_diff(this, &current_date, 1, &tmp, date_mode_t(0));
    static_cast<MYSQL_TIME*>(this)[0]= tmp;
    int warnings= 0;
    (void) check_time_range(this, TIME_SECOND_PART_DIGITS, &warnings);
    DBUG_ASSERT(is_valid_time());
  }
  /*
    Convert a valid DATE or DATETIME to TIME.
    Before this call, "this" must be a valid DATE or DATETIME value,
    e.g. returned from Item::get_date(), str_to_xxx(), number_to_xxx().
    After this call, "this" is a valid TIME value.
  */
  void valid_datetime_to_valid_time(THD *thd, int *warn, const Options opt)
  {
    DBUG_ASSERT(time_type == MYSQL_TIMESTAMP_DATE ||
                time_type == MYSQL_TIMESTAMP_DATETIME);
    /*
      We're dealing with a DATE or DATETIME returned from
      str_to_xxx(), number_to_xxx() or unpack_time().
      Do some asserts to make sure the result hour value
      after mixing days to hours does not go out of the valid TIME range.
      The maximum hour value after mixing days will be 31*24+23=767,
      which is within the supported TIME range.
      Thus no adjust_time_range_or_invalidate() is needed here.
    */
    DBUG_ASSERT(day < 32);
    DBUG_ASSERT(hour < 24);
    if (opt.datetime_to_time_mode() == DATETIME_TO_TIME_MINUS_CURRENT_DATE)
    {
      datetime_to_time_minus_current_date(thd);
    }
    else
    {
      if (opt.datetime_to_time_mode() ==
          DATETIME_TO_TIME_YYYYMMDD_000000DD_MIX_TO_HOURS)
        datetime_to_time_YYYYMMDD_000000DD_mix_to_hours(warn, year, month, day);
      year= month= day= 0;
      time_type= MYSQL_TIMESTAMP_TIME;
    }
    DBUG_ASSERT(is_valid_time_slow());
  }
  /**
    Convert valid DATE/DATETIME to valid TIME if needed.
    This method is called after Item::get_date(),
    str_to_xxx(), number_to_xxx().
    which can return only valid TIME/DATE/DATETIME values.
    Before this call, "this" is:
    - either a valid TIME/DATE/DATETIME value
      (within the supported range for the corresponding type),
    - or MYSQL_TIMESTAMP_NONE
    After this call, "this" is:
    - either a valid TIME (within the supported TIME range),
    - or MYSQL_TIMESTAMP_NONE
  */
  void valid_MYSQL_TIME_to_valid_value(THD *thd, int *warn, const Options opt)
  {
    switch (time_type) {
    case MYSQL_TIMESTAMP_DATE:
    case MYSQL_TIMESTAMP_DATETIME:
      if (opt.datetime_to_time_mode() ==
          DATETIME_TO_TIME_YYYYMMDD_00000000_ONLY &&
          (year || month || day))
        make_from_out_of_range(warn);
      else if (opt.datetime_to_time_mode() == DATETIME_TO_TIME_DISALLOW)
        make_from_out_of_range(warn);
      else
        valid_datetime_to_valid_time(thd, warn, opt);
      break;
    case MYSQL_TIMESTAMP_NONE:
      break;
    case MYSQL_TIMESTAMP_ERROR:
      set_zero_time(this, MYSQL_TIMESTAMP_TIME);
      break;
    case MYSQL_TIMESTAMP_TIME:
      DBUG_ASSERT(is_valid_time_slow());
      break;
    }
  }

  /*
    This method is called after number_to_xxx() and str_to_xxx(),
    which can return DATE or DATETIME values. Convert to TIME if needed.
    We trust that xxx_to_time() returns a valid TIME/DATE/DATETIME value,
    so here we need to do only simple validation.
  */
  void xxx_to_time_result_to_valid_value(THD *thd, int *warn, const Options opt)
  {
    // str_to_xxx(), number_to_xxx() never return MYSQL_TIMESTAMP_ERROR
    DBUG_ASSERT(time_type != MYSQL_TIMESTAMP_ERROR);
    valid_MYSQL_TIME_to_valid_value(thd, warn, opt);
  }
  void adjust_time_range_or_invalidate(int *warn)
  {
    if (check_time_range(this, TIME_SECOND_PART_DIGITS, warn))
      time_type= MYSQL_TIMESTAMP_NONE;
    DBUG_ASSERT(is_valid_value_slow());
  }
public:
  void round_or_set_max(uint dec, int *warn, ulong nsec);
private:
  void round_or_set_max(uint dec, int *warn);

  /*
    All make_from_xxx() methods initialize *warn.
    The old value gets lost.
  */
  void make_from_datetime_move_day_to_hour(int *warn, const MYSQL_TIME *from);
  void make_from_datetime_with_days_diff(int *warn, const MYSQL_TIME *from,
                                         long curdays);
  void make_from_time(int *warn, const MYSQL_TIME *from);
  void make_from_datetime(int *warn, const MYSQL_TIME *from, long curdays);
  void make_from_item(THD *thd, int *warn, Item *item, const Options opt);
public:
  /*
    All constructors that accept an "int *warn" parameter initialize *warn.
    The old value gets lost.
  */
  Time(int *warn, bool neg, ulonglong hour, uint minute, const Sec6 &second);
  Time() { time_type= MYSQL_TIMESTAMP_NONE; }
  Time(const Native &native);
  Time(THD *thd, const MYSQL_TIME *ltime, const Options opt)
  {
    *(static_cast<MYSQL_TIME*>(this))= *ltime;
    DBUG_ASSERT(is_valid_temporal());
    int warn= 0;
    valid_MYSQL_TIME_to_valid_value(thd, &warn, opt);
  }
  Time(Item *item)
   :Time(current_thd, item)
  { }
  Time(THD *thd, Item *item, const Options opt)
  {
    int warn;
    make_from_item(thd, &warn, item, opt);
  }
  Time(THD *thd, Item *item)
   :Time(thd, item, Options(thd))
  { }
  Time(int *warn, const MYSQL_TIME *from, long curdays);
  Time(THD *thd, MYSQL_TIME_STATUS *status,
       const char *str, size_t len, CHARSET_INFO *cs,
       const Options opt)
  {
    if (str_to_datetime_or_date_or_time(thd, status, str, len, cs, opt))
      time_type= MYSQL_TIMESTAMP_NONE;
    // The below call will optionally add notes to already collected warnings:
    else
      xxx_to_time_result_to_valid_value(thd, &status->warnings, opt);
  }

protected:
  Time(THD *thd, int *warn, const Sec6 &nr, const Options opt)
  {
    if (nr.to_datetime_or_time(this, warn, TIME_INVALID_DATES))
      time_type= MYSQL_TIMESTAMP_NONE;
    xxx_to_time_result_to_valid_value(thd, warn, opt);
  }
  Time(THD *thd, int *warn, const Sec9 &nr, const Options &opt)
   :Time(thd, warn, static_cast<Sec6>(nr), opt)
  {
    if (is_valid_time() && time_round_mode_t(opt) == TIME_FRAC_ROUND)
      round_or_set_max(6, warn, nr.nsec());
  }

public:
  Time(THD *thd, int *warn, const Longlong_hybrid &nr, const Options &opt)
   :Time(thd, warn, Sec6(nr), opt)
  { }
  Time(THD *thd, int *warn, double nr, const Options &opt)
   :Time(thd, warn, Sec9(nr), opt)
  { }
  Time(THD *thd, int *warn, const my_decimal *d, const Options &opt)
   :Time(thd, warn, Sec9(d), opt)
  { }

  Time(THD *thd, Item *item, const Options opt, uint dec)
   :Time(thd, item, opt)
  {
    round(dec, time_round_mode_t(opt));
  }
  Time(int *warn, const MYSQL_TIME *from, long curdays,
       const Time::Options &opt, uint dec)
   :Time(warn, from, curdays)
  {
    round(dec, time_round_mode_t(opt), warn);
  }
  Time(int *warn, bool neg, ulonglong hour, uint minute, const Sec9 &second,
       time_round_mode_t mode, uint dec)
   :Time(warn, neg, hour, minute, second)
  {
    DBUG_ASSERT(is_valid_time());
    if ((ulonglong) mode == (ulonglong) TIME_FRAC_ROUND)
      round_or_set_max(6, warn, second.nsec());
    round(dec, mode, warn);
  }
  Time(THD *thd, MYSQL_TIME_STATUS *status,
       const char *str, size_t len, CHARSET_INFO *cs,
       const Options &opt, uint dec)
   :Time(thd, status, str, len, cs, opt)
  {
    round(dec, time_round_mode_t(opt), &status->warnings);
  }
  Time(THD *thd, int *warn, const Longlong_hybrid &nr,
       const Options &opt, uint dec)
   :Time(thd, warn, nr, opt)
  {
    /*
      Decimal digit truncation is needed here in case if nr was out
      of the supported TIME range, so "this" was set to '838:59:59.999999'.
      We always do truncation (not rounding) here, independently from "opt".
    */
    trunc(dec);
  }
  Time(THD *thd, int *warn, double nr, const Options &opt, uint dec)
   :Time(thd, warn, nr, opt)
  {
    round(dec, time_round_mode_t(opt), warn);
  }
  Time(THD *thd, int *warn, const my_decimal *d, const Options &opt, uint dec)
   :Time(thd, warn, d, opt)
  {
    round(dec, time_round_mode_t(opt), warn);
  }

  static date_conv_mode_t default_flags_for_get_date()
  { return TIME_TIME_ONLY | TIME_INVALID_DATES; }
  static date_conv_mode_t comparison_flags_for_get_date()
  { return TIME_TIME_ONLY | TIME_INVALID_DATES | TIME_FUZZY_DATES; }
  bool is_valid_time() const
  {
    DBUG_ASSERT(is_valid_value_slow());
    return time_type == MYSQL_TIMESTAMP_TIME;
  }
  const MYSQL_TIME *get_mysql_time() const
  {
    DBUG_ASSERT(is_valid_time_slow());
    return this;
  }
  bool copy_to_mysql_time(MYSQL_TIME *ltime) const
  {
    if (time_type == MYSQL_TIMESTAMP_NONE)
    {
      ltime->time_type= MYSQL_TIMESTAMP_NONE;
      return true;
    }
    DBUG_ASSERT(is_valid_time_slow());
    *ltime= *this;
    return false;
  }
  int cmp(const Time *other) const
  {
    DBUG_ASSERT(is_valid_time_slow());
    DBUG_ASSERT(other->is_valid_time_slow());
    longlong p0= to_packed();
    longlong p1= other->to_packed();
    if (p0 < p1)
      return -1;
    if (p0 > p1)
      return 1;
    return 0;
  }
  longlong to_seconds_abs() const
  {
    DBUG_ASSERT(is_valid_time_slow());
    return hour * 3600L + minute * 60 + second;
  }
  longlong to_seconds() const
  {
    return neg ? -to_seconds_abs() : to_seconds_abs();
  }
  longlong to_longlong() const
  {
    if (!is_valid_time())
      return 0;
    ulonglong v= TIME_to_ulonglong_time(this);
    return neg ? -(longlong) v : (longlong) v;
  }
  double to_double() const
  {
    return !is_valid_time() ? 0 :
           Temporal::to_double(neg, TIME_to_ulonglong_time(this), second_part);
  }
  bool to_native(Native *to, uint decimals) const;
  String *to_string(String *str, uint dec) const
  {
    if (!is_valid_time())
      return NULL;
    str->set_charset(&my_charset_numeric);
    if (!str->alloc(MAX_DATE_STRING_REP_LENGTH))
      str->length(my_time_to_str(this, const_cast<char*>(str->ptr()), dec));
    return str;
  }
  my_decimal *to_decimal(my_decimal *to)
  {
    return is_valid_time() ? Temporal::to_decimal(to) : bad_to_decimal(to);
  }
  longlong to_packed() const
  {
    return is_valid_time() ? Temporal::to_packed() : 0;
  }
  longlong valid_time_to_packed() const
  {
    DBUG_ASSERT(is_valid_time_slow());
    return Temporal::to_packed();
  }
  long fraction_remainder(uint dec) const
  {
    DBUG_ASSERT(is_valid_time());
    return Temporal::fraction_remainder(dec);
  }

  Time &trunc(uint dec)
  {
    if (is_valid_time())
      my_time_trunc(this, dec);
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
  }
  Time &ceiling(int *warn)
  {
    if (is_valid_time())
    {
      if (neg)
        my_time_trunc(this, 0);
      else if (second_part)
        round_or_set_max(0, warn, 999999999);
    }
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
  }
  Time &ceiling()
  {
    int warn= 0;
    return ceiling(&warn);
  }
  Time &floor(int *warn)
  {
    if (is_valid_time())
    {
      if (!neg)
        my_time_trunc(this, 0);
      else if (second_part)
        round_or_set_max(0, warn, 999999999);
    }
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
  }
  Time &floor()
  {
    int warn= 0;
    return floor(&warn);
  }
  Time &round(uint dec, int *warn)
  {
    if (is_valid_time())
      round_or_set_max(dec, warn);
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
  }
  Time &round(uint dec, time_round_mode_t mode, int *warn)
  {
    switch (mode.mode()) {
    case time_round_mode_t::FRAC_NONE:
      DBUG_ASSERT(fraction_remainder(dec) == 0);
      return trunc(dec);
    case time_round_mode_t::FRAC_TRUNCATE:
      return trunc(dec);
    case time_round_mode_t::FRAC_ROUND:
      return round(dec, warn);
    }
    return *this;
  }
  Time &round(uint dec, time_round_mode_t mode)
  {
    int warn= 0;
    return round(dec, mode, &warn);
  }

};


/**
  Class Temporal_with_date is designed to store valid DATE or DATETIME values.
  See also class Time.

  1. Valid value:
    a. MYSQL_TIMESTAMP_{DATE|DATETIME} - a valid DATE or DATETIME value
    b. MYSQL_TIMESTAMP_NONE            - an undefined value

  2. Invalid value (internally only):
    a. MYSQL_TIMESTAMP_{DATE|DATETIME} - a DATE or DATETIME value, but with
                                         MYSQL_TIME members outside of the
                                         valid/supported range
    b. MYSQL_TIMESTAMP_TIME            - a TIME value
    c. MYSQL_TIMESTAMP_ERROR           - error

  Temporarily is allowed to have an invalid value, but only internally,
  during initialization time. All constructors and modification methods must
  leave the value as described above (see "Valid value").

  Derives from MYSQL_TIME using "protected" inheritance to make sure
  it is accessed externally only in the valid state.
*/

class Temporal_with_date: public Temporal
{
public:
  class Options: public Temporal::Options
  {
  public:
    Options(date_conv_mode_t fuzzydate, time_round_mode_t mode):
     Temporal::Options(fuzzydate, mode)
    {}
    explicit Options(const Temporal::Options &opt)
     :Temporal::Options(opt)
    { }
    explicit Options(date_mode_t mode)
     :Temporal::Options(mode)
    { }
  };
protected:
  void check_date_or_invalidate(int *warn, date_conv_mode_t flags);
  void make_from_item(THD *thd, Item *item, date_mode_t flags);

  ulong daynr() const
  {
    return (ulong) ::calc_daynr((uint) year, (uint) month, (uint) day);
  }
  int weekday(bool sunday_first_day_of_week) const
  {
    return ::calc_weekday(daynr(), sunday_first_day_of_week);
  }
  ulong dayofyear() const
  {
    return (ulong) (daynr() - ::calc_daynr(year, 1, 1) + 1);
  }
  uint quarter() const
  {
    return (month + 2) / 3;
  }
  uint week(uint week_behaviour) const
  {
    uint year;
    return calc_week(this, week_behaviour, &year);
  }
  uint yearweek(uint week_behaviour) const
  {
    uint year;
    uint week= calc_week(this, week_behaviour, &year);
    return week + year * 100;
  }
public:
  Temporal_with_date()
  {
    time_type= MYSQL_TIMESTAMP_NONE;
  }
  Temporal_with_date(THD *thd, Item *item, date_mode_t fuzzydate)
  {
    make_from_item(thd, item, fuzzydate);
  }
  Temporal_with_date(int *warn, const Sec6 &nr, date_mode_t flags)
  {
    DBUG_ASSERT(bool(flags & TIME_TIME_ONLY) == false);
    if (nr.to_datetime_or_date(this, warn, date_conv_mode_t(flags)))
      time_type= MYSQL_TIMESTAMP_NONE;
  }
  Temporal_with_date(THD *thd, MYSQL_TIME_STATUS *status,
                     const char *str, size_t len, CHARSET_INFO *cs,
                     date_mode_t flags)
  {
    DBUG_ASSERT(bool(flags & TIME_TIME_ONLY) == false);
    if (str_to_datetime_or_date(thd, status, str, len, cs, flags))
      time_type= MYSQL_TIMESTAMP_NONE;
  }
public:
  bool check_date_with_warn(THD *thd, date_conv_mode_t flags)
  {
    return ::check_date_with_warn(thd, this, flags, MYSQL_TIMESTAMP_ERROR);
  }
  bool check_date_with_warn(THD *thd)
  {
    return ::check_date_with_warn(thd, this, Temporal::sql_mode_for_dates(thd),
                                  MYSQL_TIMESTAMP_ERROR);
  }
  static date_conv_mode_t comparison_flags_for_get_date()
  { return TIME_INVALID_DATES | TIME_FUZZY_DATES; }
};


/**
  Class Date is designed to store valid DATE values.
  All constructors and modification methods leave instances
  of this class in one of the following valid states:
    a. MYSQL_TIMESTAMP_DATE - a DATE with all MYSQL_TIME members properly set
    b. MYSQL_TIMESTAMP_NONE - an undefined value.
  Other MYSQL_TIMESTAMP_XXX are not possible.
  MYSQL_TIMESTAMP_DATE with MYSQL_TIME members improperly set is not possible.
*/
class Date: public Temporal_with_date
{
  bool is_valid_value_slow() const
  {
    return time_type == MYSQL_TIMESTAMP_NONE || is_valid_date_slow();
  }
  bool is_valid_date_slow() const
  {
    DBUG_ASSERT(time_type == MYSQL_TIMESTAMP_DATE);
    return !check_datetime_range(this);
  }
public:
  class Options: public Temporal_with_date::Options
  {
  public:
    explicit Options(date_conv_mode_t fuzzydate)
     :Temporal_with_date::Options(fuzzydate, TIME_FRAC_TRUNCATE)
    { }
    Options(THD *thd, time_round_mode_t mode)
     :Temporal_with_date::Options(sql_mode_for_dates(thd), mode)
    { }
    explicit Options(THD *thd)
     :Temporal_with_date::Options(sql_mode_for_dates(thd), TIME_FRAC_TRUNCATE)
    { }
    explicit Options(date_mode_t fuzzydate)
     :Temporal_with_date::Options(fuzzydate)
    { }
  };
public:
  Date(Item *item, date_mode_t fuzzydate)
   :Date(current_thd, item, fuzzydate)
  { }
  Date(THD *thd, Item *item, date_mode_t fuzzydate)
   :Temporal_with_date(thd, item, fuzzydate)
  {
    if (time_type == MYSQL_TIMESTAMP_DATETIME)
      datetime_to_date(this);
    DBUG_ASSERT(is_valid_value_slow());
  }
  Date(THD *thd, Item *item, date_conv_mode_t fuzzydate)
   :Date(thd, item, Options(fuzzydate))
  { }
  Date(THD *thd, Item *item)
   :Temporal_with_date(Date(thd, item, Options(thd, TIME_FRAC_TRUNCATE)))
  { }
  Date(Item *item)
   :Temporal_with_date(Date(current_thd, item))
  { }
  Date(const Temporal_with_date *d)
   :Temporal_with_date(*d)
  {
    datetime_to_date(this);
    DBUG_ASSERT(is_valid_date_slow());
  }
  explicit Date(const Temporal_hybrid *from)
  {
    from->copy_valid_value_to_mysql_time(this);
    DBUG_ASSERT(is_valid_date_slow());
  }
  bool is_valid_date() const
  {
    DBUG_ASSERT(is_valid_value_slow());
    return time_type == MYSQL_TIMESTAMP_DATE;
  }
  bool check_date(date_conv_mode_t flags, int *warnings) const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return ::check_date(this, (year || month || day),
                        ulonglong(flags & TIME_MODE_FOR_XXX_TO_DATE),
                        warnings);
  }
  bool check_date(THD *thd, int *warnings) const
  {
    return check_date(Temporal::sql_mode_for_dates(thd), warnings);
  }
  bool check_date(date_conv_mode_t flags) const
  {
    int dummy; /* unused */
    return check_date(flags, &dummy);
  }
  bool check_date(THD *thd) const
  {
    int dummy;
    return check_date(Temporal::sql_mode_for_dates(thd), &dummy);
  }
  const MYSQL_TIME *get_mysql_time() const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return this;
  }
  bool copy_to_mysql_time(MYSQL_TIME *ltime) const
  {
    if (time_type == MYSQL_TIMESTAMP_NONE)
    {
      ltime->time_type= MYSQL_TIMESTAMP_NONE;
      return true;
    }
    DBUG_ASSERT(is_valid_date_slow());
    *ltime= *this;
    return false;
  }
  ulong daynr() const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return Temporal_with_date::daynr();
  }
  ulong dayofyear() const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return Temporal_with_date::dayofyear();
  }
  uint quarter() const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return Temporal_with_date::quarter();
  }
  uint week(uint week_behaviour) const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return Temporal_with_date::week(week_behaviour);
  }
  uint yearweek(uint week_behaviour) const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return Temporal_with_date::yearweek(week_behaviour);
  }

  longlong valid_date_to_packed() const
  {
    DBUG_ASSERT(is_valid_date_slow());
    return Temporal::to_packed();
  }
  longlong to_longlong() const
  {
    return is_valid_date() ? (longlong) TIME_to_ulonglong_date(this) : 0LL;
  }
  double to_double() const
  {
    return (double) to_longlong();
  }
  String *to_string(String *str) const
  {
    if (!is_valid_date())
      return NULL;
    str->set_charset(&my_charset_numeric);
    if (!str->alloc(MAX_DATE_STRING_REP_LENGTH))
      str->length(my_date_to_str(this, const_cast<char*>(str->ptr())));
    return str;
  }
  my_decimal *to_decimal(my_decimal *to)
  {
    return is_valid_date() ? Temporal::to_decimal(to) : bad_to_decimal(to);
  }
};


/**
  Class Datetime is designed to store valid DATETIME values.
  All constructors and modification methods leave instances
  of this class in one of the following valid states:
    a. MYSQL_TIMESTAMP_DATETIME - a DATETIME with all members properly set
    b. MYSQL_TIMESTAMP_NONE     - an undefined value.
  Other MYSQL_TIMESTAMP_XXX are not possible.
  MYSQL_TIMESTAMP_DATETIME with MYSQL_TIME members
  improperly set is not possible.
*/
class Datetime: public Temporal_with_date
{
  bool is_valid_value_slow() const
  {
    return time_type == MYSQL_TIMESTAMP_NONE || is_valid_datetime_slow();
  }
  bool is_valid_datetime_slow() const
  {
    DBUG_ASSERT(time_type == MYSQL_TIMESTAMP_DATETIME);
    return !check_datetime_range(this);
  }
  bool add_nanoseconds_or_invalidate(THD *thd, int *warn, ulong nsec)
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    bool rc= Temporal::datetime_add_nanoseconds_or_invalidate(thd, warn, nsec);
    DBUG_ASSERT(is_valid_value_slow());
    return rc;
  }
  void date_to_datetime_if_needed()
  {
    if (time_type == MYSQL_TIMESTAMP_DATE)
      date_to_datetime(this);
  }
  void make_from_time(THD *thd, int *warn, const MYSQL_TIME *from,
                      date_conv_mode_t flags);
  void make_from_datetime(THD *thd, int *warn, const MYSQL_TIME *from,
                          date_conv_mode_t flags);
  bool round_or_invalidate(THD *thd, uint dec, int *warn);
  bool round_or_invalidate(THD *thd, uint dec, int *warn, ulong nsec)
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    bool rc= Temporal::datetime_round_or_invalidate(thd, dec, warn, nsec);
    DBUG_ASSERT(is_valid_value_slow());
    return rc;
  }
public:

  class Options: public Temporal_with_date::Options
  {
  public:
    Options(date_conv_mode_t fuzzydate, time_round_mode_t nanosecond_rounding)
     :Temporal_with_date::Options(fuzzydate, nanosecond_rounding)
    { }
    Options(THD *thd)
     :Temporal_with_date::Options(sql_mode_for_dates(thd), default_round_mode(thd))
    { }
    Options(THD *thd, time_round_mode_t rounding_mode)
     :Temporal_with_date::Options(sql_mode_for_dates(thd), rounding_mode)
    { }
    Options(date_conv_mode_t fuzzydate, THD *thd)
     :Temporal_with_date::Options(fuzzydate, default_round_mode(thd))
    { }
  };

  class Options_cmp: public Options
  {
  public:
    Options_cmp(THD *thd)
     :Options(comparison_flags_for_get_date(), thd)
    { }
  };

  static Datetime zero()
  {
    int warn;
    static Longlong_hybrid nr(0, false);
    return Datetime(&warn, nr, date_mode_t(0));
  }
public:
  Datetime() // NULL value
   :Temporal_with_date()
  { }
  Datetime(THD *thd, Item *item, date_mode_t fuzzydate)
   :Temporal_with_date(thd, item, fuzzydate)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }
  Datetime(THD *thd, Item *item)
   :Temporal_with_date(Datetime(thd, item, Options(thd)))
  { }
  Datetime(Item *item)
   :Datetime(current_thd, item)
  { }

  Datetime(THD *thd, int *warn, const MYSQL_TIME *from, date_conv_mode_t flags);
  Datetime(THD *thd, MYSQL_TIME_STATUS *status,
           const char *str, size_t len, CHARSET_INFO *cs,
           const date_mode_t fuzzydate)
   :Temporal_with_date(thd, status, str, len, cs, fuzzydate)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }

protected:
  Datetime(int *warn, const Sec6 &nr, date_mode_t flags)
   :Temporal_with_date(warn, nr, flags)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }
  Datetime(THD *thd, int *warn, const Sec9 &nr, date_mode_t fuzzydate)
   :Datetime(warn, static_cast<const Sec6>(nr), fuzzydate)
  {
    if (is_valid_datetime() &&
        time_round_mode_t(fuzzydate) == TIME_FRAC_ROUND)
      round_or_invalidate(thd, 6, warn, nr.nsec());
    DBUG_ASSERT(is_valid_value_slow());
  }

public:
  Datetime(int *warn, const Longlong_hybrid &nr, date_mode_t mode)
   :Datetime(warn, Sec6(nr), mode)
  { }
  Datetime(THD *thd, int *warn, double nr, date_mode_t fuzzydate)
   :Datetime(thd, warn, Sec9(nr), fuzzydate)
  { }
  Datetime(THD *thd, int *warn, const my_decimal *d, date_mode_t fuzzydate)
   :Datetime(thd, warn, Sec9(d), fuzzydate)
  { }
  Datetime(THD *thd, const timeval &tv);

  Datetime(THD *thd, Item *item, date_mode_t fuzzydate, uint dec)
   :Datetime(thd, item, fuzzydate)
  {
    int warn= 0;
    round(thd, dec, time_round_mode_t(fuzzydate), &warn);
  }
  Datetime(THD *thd, MYSQL_TIME_STATUS *status,
           const char *str, size_t len, CHARSET_INFO *cs,
           date_mode_t fuzzydate, uint dec)
   :Datetime(thd, status, str, len, cs, fuzzydate)
  {
    round(thd, dec, time_round_mode_t(fuzzydate), &status->warnings);
  }
  Datetime(THD *thd, int *warn, double nr, date_mode_t fuzzydate, uint dec)
   :Datetime(thd, warn, nr, fuzzydate)
  {
    round(thd, dec, time_round_mode_t(fuzzydate), warn);
  }
  Datetime(THD *thd, int *warn, const my_decimal *d, date_mode_t fuzzydate, uint dec)
   :Datetime(thd, warn, d, fuzzydate)
  {
    round(thd, dec, time_round_mode_t(fuzzydate), warn);
  }
  Datetime(THD *thd, int *warn, const MYSQL_TIME *from,
           date_mode_t fuzzydate, uint dec)
   :Datetime(thd, warn, from, date_conv_mode_t(fuzzydate) & ~TIME_TIME_ONLY)
  {
    round(thd, dec, time_round_mode_t(fuzzydate), warn);
  }
  explicit Datetime(const Temporal_hybrid *from)
  {
    from->copy_valid_value_to_mysql_time(this);
    DBUG_ASSERT(is_valid_datetime_slow());
  }
  explicit Datetime(const MYSQL_TIME *from)
  {
    *(static_cast<MYSQL_TIME*>(this))= *from;
    DBUG_ASSERT(is_valid_datetime_slow());
  }
  Datetime(my_time_t unix_time, ulong second_part,
           const Time_zone* time_zone);

  bool is_valid_datetime() const
  {
    /*
      Here we quickly check for the type only.
      If the type is valid, the rest of value must also be valid.
    */
    DBUG_ASSERT(is_valid_value_slow());
    return time_type == MYSQL_TIMESTAMP_DATETIME;
  }
  bool check_date(date_conv_mode_t flags, int *warnings) const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return ::check_date(this, (year || month || day),
                        ulonglong(flags & TIME_MODE_FOR_XXX_TO_DATE),
                        warnings);
  }
  bool check_date(date_conv_mode_t flags) const
  {
    int dummy; /* unused */
    return check_date(flags, &dummy);
  }
  bool check_date(THD *thd) const
  {
    return check_date(Temporal::sql_mode_for_dates(thd));
  }
  bool hhmmssff_is_zero() const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return hour == 0 && minute == 0 && second == 0 && second_part == 0;
  }
  ulong daynr() const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return Temporal_with_date::daynr();
  }
  int weekday(bool sunday_first_day_of_week) const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return Temporal_with_date::weekday(sunday_first_day_of_week);
  }
  ulong dayofyear() const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return Temporal_with_date::dayofyear();
  }
  uint quarter() const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return Temporal_with_date::quarter();
  }
  uint week(uint week_behaviour) const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return Temporal_with_date::week(week_behaviour);
  }
  uint yearweek(uint week_behaviour) const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return Temporal_with_date::yearweek(week_behaviour);
  }

  longlong hhmmss_to_seconds_abs() const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return hour * 3600L + minute * 60 + second;
  }
  longlong hhmmss_to_seconds() const
  {
    return neg ? -hhmmss_to_seconds_abs() : hhmmss_to_seconds_abs();
  }
  longlong to_seconds() const
  {
    return hhmmss_to_seconds() + (longlong) daynr() * 24L * 3600L;
  }

  const MYSQL_TIME *get_mysql_time() const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return this;
  }
  bool copy_to_mysql_time(MYSQL_TIME *ltime) const
  {
    if (time_type == MYSQL_TIMESTAMP_NONE)
    {
      ltime->time_type= MYSQL_TIMESTAMP_NONE;
      return true;
    }
    DBUG_ASSERT(is_valid_datetime_slow());
    *ltime= *this;
    return false;
  }
  /**
    Copy without data loss, with an optional DATETIME to DATE conversion.
    If the value of the "type" argument is MYSQL_TIMESTAMP_DATE,
    then "this" must be a datetime with a zero hhmmssff part.
  */
  bool copy_to_mysql_time(MYSQL_TIME *ltime, timestamp_type type)
  {
    DBUG_ASSERT(type == MYSQL_TIMESTAMP_DATE ||
                type == MYSQL_TIMESTAMP_DATETIME);
    if (copy_to_mysql_time(ltime))
      return true;
    DBUG_ASSERT(type != MYSQL_TIMESTAMP_DATE || hhmmssff_is_zero());
    ltime->time_type= type;
    return false;
  }
  longlong to_longlong() const
  {
    return is_valid_datetime() ?
           (longlong) TIME_to_ulonglong_datetime(this) : 0LL;
  }
  double to_double() const
  {
    return !is_valid_datetime() ? 0 :
      Temporal::to_double(neg, TIME_to_ulonglong_datetime(this), second_part);
  }
  String *to_string(String *str, uint dec) const
  {
    if (!is_valid_datetime())
      return NULL;
    str->set_charset(&my_charset_numeric);
    if (!str->alloc(MAX_DATE_STRING_REP_LENGTH))
      str->length(my_datetime_to_str(this, const_cast<char*>(str->ptr()), dec));
    return str;
  }
  my_decimal *to_decimal(my_decimal *to)
  {
    return is_valid_datetime() ? Temporal::to_decimal(to) : bad_to_decimal(to);
  }
  longlong to_packed() const
  {
    return is_valid_datetime() ? Temporal::to_packed() : 0;
  }
  longlong valid_datetime_to_packed() const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return Temporal::to_packed();
  }
  long fraction_remainder(uint dec) const
  {
    DBUG_ASSERT(is_valid_datetime());
    return Temporal::fraction_remainder(dec);
  }

  Datetime &trunc(uint dec)
  {
    if (is_valid_datetime())
      my_datetime_trunc(this, dec);
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
  }
  Datetime &ceiling(THD *thd, int *warn)
  {
    if (is_valid_datetime() && second_part)
      round_or_invalidate(thd, 0, warn, 999999999);
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
  }
  Datetime &ceiling(THD *thd)
  {
    int warn= 0;
    return ceiling(thd, &warn);
  }
  Datetime &round(THD *thd, uint dec, int *warn)
  {
    if (is_valid_datetime())
      round_or_invalidate(thd, dec, warn);
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
  }
  Datetime &round(THD *thd, uint dec, time_round_mode_t mode, int *warn)
  {
    switch (mode.mode()) {
    case time_round_mode_t::FRAC_NONE:
      DBUG_ASSERT(fraction_remainder(dec) == 0);
      return trunc(dec);
    case time_round_mode_t::FRAC_TRUNCATE:
      return trunc(dec);
    case time_round_mode_t::FRAC_ROUND:
      return round(thd, dec, warn);
    }
    return *this;
  }
  Datetime &round(THD *thd, uint dec, time_round_mode_t mode)
  {
    int warn= 0;
    return round(thd, dec, mode, &warn);
  }

};


/*
  Datetime to be created from an Item who is known to be of a temporal
  data type. For temporal data types we don't need nanosecond rounding
  or truncation, as their precision is limited.
*/
class Datetime_from_temporal: public Datetime
{
public:
  // The constructor DBUG_ASSERTs on a proper Item data type.
  Datetime_from_temporal(THD *thd, Item *temporal, date_conv_mode_t flags);
};


/*
  Datetime to be created from an Item who is known not to have digits outside
  of the specified scale. So it's not important which rounding method to use.
  TRUNCATE should work.
  Typically, Item is of a temporal data type, but this is not strictly required.
*/
class Datetime_truncation_not_needed: public Datetime
{
public:
  Datetime_truncation_not_needed(THD *thd, Item *item, date_conv_mode_t mode);
  Datetime_truncation_not_needed(THD *thd, Item *item, date_mode_t mode)
   :Datetime_truncation_not_needed(thd, item, date_conv_mode_t(mode))
  { }
};


class Timestamp: protected Timeval
{
  static uint binary_length_to_precision(uint length);
protected:
  void round_or_set_max(uint dec, int *warn);
  bool add_nanoseconds_usec(uint nanoseconds)
  {
    DBUG_ASSERT(nanoseconds <= 1000000000);
    if (nanoseconds < 500)
      return false;
    tv_usec+= (nanoseconds + 500) / 1000;
    if (tv_usec < 1000000)
      return false;
    tv_usec%= 1000000;
    return true;
  }
public:
  static date_conv_mode_t sql_mode_for_timestamp(THD *thd);
  static time_round_mode_t default_round_mode(THD *thd);
  class DatetimeOptions: public date_mode_t
  {
  public:
    DatetimeOptions(date_conv_mode_t fuzzydate, time_round_mode_t round_mode)
     :date_mode_t(fuzzydate | round_mode)
    { }
    DatetimeOptions(THD *thd)
     :DatetimeOptions(sql_mode_for_timestamp(thd), default_round_mode(thd))
    { }
  };
public:
  Timestamp(my_time_t timestamp, ulong sec_part)
   :Timeval(timestamp, sec_part)
  { }
  explicit Timestamp(const timeval &tv)
   :Timeval(tv)
  { }
  explicit Timestamp(const Native &native);
  Timestamp(THD *thd, const MYSQL_TIME *ltime, uint *error_code);
  const struct timeval &tv() const { return *this; }
  int cmp(const Timestamp &other) const
  {
    return tv_sec < other.tv_sec   ? -1 :
           tv_sec > other.tv_sec   ? +1 :
           tv_usec < other.tv_usec ? -1 :
           tv_usec > other.tv_usec ? +1 : 0;
  }
  bool to_TIME(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) const;
  bool to_native(Native *to, uint decimals) const;
  Datetime to_datetime(THD *thd) const
  {
    return Datetime(thd, *this);
  }
  long fraction_remainder(uint dec) const
  {
    return my_time_fraction_remainder(tv_usec, dec);
  }
  Timestamp &trunc(uint dec)
  {
    my_timeval_trunc(this, dec);
    return *this;
  }
  Timestamp &round(uint dec, int *warn)
  {
    round_or_set_max(dec, warn);
    return *this;
  }
  Timestamp &round(uint dec, time_round_mode_t mode, int *warn)
  {
    switch (mode.mode()) {
    case time_round_mode_t::FRAC_NONE:
      DBUG_ASSERT(fraction_remainder(dec) == 0);
      return trunc(dec);
    case time_round_mode_t::FRAC_TRUNCATE:
      return trunc(dec);
    case time_round_mode_t::FRAC_ROUND:
      return round(dec, warn);
    }
    return *this;
  }
  Timestamp &round(uint dec, time_round_mode_t mode)
  {
    int warn= 0;
    return round(dec, mode, &warn);
  }
};


/**
  A helper class to store MariaDB TIMESTAMP values, which can be:
  - real TIMESTAMP (seconds and microseconds since epoch), or
  - zero datetime '0000-00-00 00:00:00.000000'
*/
class Timestamp_or_zero_datetime: protected Timestamp
{
  bool m_is_zero_datetime;
public:
  Timestamp_or_zero_datetime()
   :Timestamp(0,0), m_is_zero_datetime(true)
  { }
  Timestamp_or_zero_datetime(const Native &native)
   :Timestamp(native.length() ? Timestamp(native) : Timestamp(0,0)),
    m_is_zero_datetime(native.length() == 0)
  { }
  Timestamp_or_zero_datetime(const Timestamp &tm, bool is_zero_datetime)
   :Timestamp(tm), m_is_zero_datetime(is_zero_datetime)
  { }
  Timestamp_or_zero_datetime(THD *thd, const MYSQL_TIME *ltime, uint *err_code);
  Datetime to_datetime(THD *thd) const
  {
    if (is_zero_datetime())
      return Datetime::zero();
    return Timestamp::to_datetime(thd);
  }
  bool is_zero_datetime() const { return m_is_zero_datetime; }
  void trunc(uint decimals)
  {
    if (!is_zero_datetime())
     Timestamp::trunc(decimals);
  }
  int cmp(const Timestamp_or_zero_datetime &other) const
  {
    if (is_zero_datetime())
      return other.is_zero_datetime() ? 0 : -1;
    if (other.is_zero_datetime())
      return 1;
    return Timestamp::cmp(other);
  }
  bool to_TIME(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate) const;
  /*
    Convert to native format:
    - Real timestamps are encoded in the same way how Field_timestamp2 stores
      values (big endian seconds followed by big endian microseconds)
    - Zero datetime '0000-00-00 00:00:00.000000' is encoded as empty string.
    Two native values are binary comparable.
  */
  bool to_native(Native *to, uint decimals) const;
};


/**
  A helper class to store non-null MariaDB TIMESTAMP values in
  the native binary encoded representation.
*/
class Timestamp_or_zero_datetime_native:
          public NativeBuffer<STRING_BUFFER_TIMESTAMP_BINARY_SIZE>
{
public:
  Timestamp_or_zero_datetime_native() { }
  Timestamp_or_zero_datetime_native(const Timestamp_or_zero_datetime &ts,
                                    uint decimals)
  {
    if (ts.to_native(this, decimals))
      length(0); // safety
  }
  int save_in_field(Field *field, uint decimals) const;
  Datetime to_datetime(THD *thd) const
  {
    return is_zero_datetime() ?
           Datetime::zero() :
           Datetime(thd, Timestamp(*this).tv());
  }
  bool is_zero_datetime() const
  {
    return length() == 0;
  }
};


/**
  A helper class to store nullable MariaDB TIMESTAMP values in
  the native binary encoded representation.
*/
class Timestamp_or_zero_datetime_native_null: public Timestamp_or_zero_datetime_native,
                                              public Null_flag
{
public:
  // With optional data type conversion
  Timestamp_or_zero_datetime_native_null(THD *thd, Item *item, bool conv);
  // Without data type conversion: item is known to be of the TIMESTAMP type
  Timestamp_or_zero_datetime_native_null(THD *thd, Item *item)
   :Timestamp_or_zero_datetime_native_null(thd, item, false)
  { }
  Datetime to_datetime(THD *thd) const
  {
    return is_null() ? Datetime() :
                       Timestamp_or_zero_datetime_native::to_datetime(thd);
  }
  void to_TIME(THD *thd, MYSQL_TIME *to)
  {
    DBUG_ASSERT(!is_null());
    Datetime::Options opt(TIME_CONV_NONE, TIME_FRAC_NONE);
    Timestamp_or_zero_datetime(*this).to_TIME(thd, to, opt);
  }
  bool is_zero_datetime() const
  {
    DBUG_ASSERT(!is_null());
    return Timestamp_or_zero_datetime_native::is_zero_datetime();
  }
};


/*
  Flags for collation aggregation modes, used in TDCollation::agg():

  MY_COLL_ALLOW_SUPERSET_CONV  - allow conversion to a superset
  MY_COLL_ALLOW_COERCIBLE_CONV - allow conversion of a coercible value
                                 (i.e. constant).
  MY_COLL_ALLOW_CONV           - allow any kind of conversion
                                 (combination of the above two)
  MY_COLL_ALLOW_NUMERIC_CONV   - if all items were numbers, convert to
                                 @@character_set_connection
  MY_COLL_DISALLOW_NONE        - don't allow return DERIVATION_NONE
                                 (e.g. when aggregating for comparison)
  MY_COLL_CMP_CONV             - combination of MY_COLL_ALLOW_CONV
                                 and MY_COLL_DISALLOW_NONE
*/

#define MY_COLL_ALLOW_SUPERSET_CONV   1
#define MY_COLL_ALLOW_COERCIBLE_CONV  2
#define MY_COLL_DISALLOW_NONE         4
#define MY_COLL_ALLOW_NUMERIC_CONV    8

#define MY_COLL_ALLOW_CONV (MY_COLL_ALLOW_SUPERSET_CONV | MY_COLL_ALLOW_COERCIBLE_CONV)
#define MY_COLL_CMP_CONV   (MY_COLL_ALLOW_CONV | MY_COLL_DISALLOW_NONE)


#define MY_REPERTOIRE_NUMERIC   MY_REPERTOIRE_ASCII


static inline my_repertoire_t operator|(const my_repertoire_t a,
                                        const my_repertoire_t b)
{
  return (my_repertoire_t) ((uint) a | (uint) b);
}

static inline my_repertoire_t &operator|=(my_repertoire_t &a,
                                          const my_repertoire_t b)
{
  return a= (my_repertoire_t) ((uint) a | (uint) b);
}


enum Derivation
{
  DERIVATION_IGNORABLE= 6,
  DERIVATION_NUMERIC= 5,
  DERIVATION_COERCIBLE= 4,
  DERIVATION_SYSCONST= 3,
  DERIVATION_IMPLICIT= 2,
  DERIVATION_NONE= 1,
  DERIVATION_EXPLICIT= 0
};


/**
   "Declared Type Collation"
   A combination of collation and its derivation.
*/

class DTCollation {
public:
  CHARSET_INFO     *collation;
  enum Derivation derivation;
  my_repertoire_t repertoire;

  void set_repertoire_from_charset(CHARSET_INFO *cs)
  {
    repertoire= cs->state & MY_CS_PUREASCII ?
                MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  }
  DTCollation()
  {
    collation= &my_charset_bin;
    derivation= DERIVATION_NONE;
    repertoire= MY_REPERTOIRE_UNICODE30;
  }
  DTCollation(CHARSET_INFO *collation_arg)
  {
    /*
      This constructor version is used in combination with Field constructors,
      to pass "CHARSET_INFO" instead of the full DTCollation.
      Therefore, derivation is set to DERIVATION_IMPLICIT, which is the
      proper derivation for table fields.
      We should eventually remove all code pieces that pass "CHARSET_INFO"
      (e.g. in storage engine sources) and fix to pass the full DTCollation
      instead. Then, this constructor can be removed.
    */
    collation= collation_arg;
    derivation= DERIVATION_IMPLICIT;
    repertoire= my_charset_repertoire(collation_arg);
  }
  DTCollation(CHARSET_INFO *collation_arg, Derivation derivation_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  DTCollation(CHARSET_INFO *collation_arg,
              Derivation derivation_arg,
              my_repertoire_t repertoire_arg)
   :collation(collation_arg),
    derivation(derivation_arg),
    repertoire(repertoire_arg)
  { }
  void set(const DTCollation &dt)
  {
    *this= dt;
  }
  void set(CHARSET_INFO *collation_arg, Derivation derivation_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  void set(CHARSET_INFO *collation_arg,
           Derivation derivation_arg,
           my_repertoire_t repertoire_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    repertoire= repertoire_arg;
  }
  void set(CHARSET_INFO *collation_arg)
  {
    collation= collation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  void set(Derivation derivation_arg)
  { derivation= derivation_arg; }
  bool aggregate(const DTCollation &dt, uint flags= 0);
  bool set(DTCollation &dt1, DTCollation &dt2, uint flags= 0)
  { set(dt1); return aggregate(dt2, flags); }
  const char *derivation_name() const
  {
    switch(derivation)
    {
      case DERIVATION_NUMERIC:   return "NUMERIC";
      case DERIVATION_IGNORABLE: return "IGNORABLE";
      case DERIVATION_COERCIBLE: return "COERCIBLE";
      case DERIVATION_IMPLICIT:  return "IMPLICIT";
      case DERIVATION_SYSCONST:  return "SYSCONST";
      case DERIVATION_EXPLICIT:  return "EXPLICIT";
      case DERIVATION_NONE:      return "NONE";
      default: return "UNKNOWN";
    }
  }
  int sortcmp(const Binary_string *s, const Binary_string *t) const
  {
    return collation->strnncollsp(s->ptr(), s->length(),
                                  t->ptr(), t->length());
  }
};


class DTCollation_numeric: public DTCollation
{
public:
  DTCollation_numeric()
   :DTCollation(charset_info(), DERIVATION_NUMERIC, MY_REPERTOIRE_NUMERIC)
  { }
  static const CHARSET_INFO *charset_info() { return &my_charset_numeric; }
  static const DTCollation & singleton();
};


static inline uint32
char_to_byte_length_safe(size_t char_length_arg, uint32 mbmaxlen_arg)
{
  ulonglong tmp= ((ulonglong) char_length_arg) * mbmaxlen_arg;
  return tmp > UINT_MAX32 ? (uint32) UINT_MAX32 : static_cast<uint32>(tmp);
}

class Type_numeric_attributes
{
public:
  static uint count_unsigned(Item **item, uint nitems);
  static uint32 find_max_char_length(Item **item, uint nitems);
  static uint32 find_max_octet_length(Item **item, uint nitems);
  static decimal_digits_t find_max_decimal_int_part(Item **item, uint nitems);
  static decimal_digits_t find_max_decimals(Item **item, uint nitems);
public:
  /*
    The maximum value length in characters multiplied by collation->mbmaxlen.
    Almost always it's the maximum value length in bytes.
  */
  uint32 max_length;
  decimal_digits_t decimals;
  bool unsigned_flag;
public:
  Type_numeric_attributes()
   :max_length(0), decimals(0), unsigned_flag(false)
  { }
  Type_numeric_attributes(uint32 max_length_arg, decimal_digits_t decimals_arg,
                          bool unsigned_flag_arg)
   :max_length(max_length_arg),
    decimals(decimals_arg),
    unsigned_flag(unsigned_flag_arg)
  { }
protected:
  void aggregate_numeric_attributes_real(Item **item, uint nitems);
  void aggregate_numeric_attributes_decimal(Item **item, uint nitems,
                                            bool unsigned_arg);
};



class Type_temporal_attributes: public Type_numeric_attributes
{
public:
  Type_temporal_attributes(uint32 int_part_length, decimal_digits_t dec, bool unsigned_arg)
   :Type_numeric_attributes(int_part_length + (dec ? 1 : 0),
                            MY_MIN(dec,
                                   (decimal_digits_t) TIME_SECOND_PART_DIGITS),
                            unsigned_arg)
  {
    max_length+= decimals;
  }
};


class Type_temporal_attributes_not_fixed_dec: public Type_numeric_attributes
{
public:
  Type_temporal_attributes_not_fixed_dec(uint32 int_part_length, decimal_digits_t dec,
                                         bool unsigned_flag)
   :Type_numeric_attributes(int_part_length, dec, unsigned_flag)
  {
    if (decimals == NOT_FIXED_DEC)
      max_length+= TIME_SECOND_PART_DIGITS + 1;
    else if (decimals)
    {
      set_if_smaller(decimals, TIME_SECOND_PART_DIGITS);
      max_length+= decimals + 1;
    }
  }
};


/**
  A class to store type attributes for the standard data types.
  Does not include attributes for the extended data types
  such as ENUM, SET, GEOMETRY.
*/
class Type_std_attributes: public Type_numeric_attributes
{
public:
  DTCollation collation;
  Type_std_attributes()
   :collation(&my_charset_bin, DERIVATION_COERCIBLE)
  { }
  Type_std_attributes(const Type_numeric_attributes &nattr,
                      const DTCollation &dtc)
   :Type_numeric_attributes(nattr), collation(dtc)
  { }
  void set(const Type_std_attributes *other)
  {
    *this= *other;
  }
  void set(const Type_std_attributes &other)
  {
    *this= other;
  }
  void set(const Type_numeric_attributes &nattr, const DTCollation &dtc)
  {
    *this= Type_std_attributes(nattr, dtc);
  }
  uint32 max_char_length() const
  { return max_length / collation.collation->mbmaxlen; }
  void fix_length_and_charset(uint32 max_char_length_arg, CHARSET_INFO *cs)
  {
    max_length= char_to_byte_length_safe(max_char_length_arg, cs->mbmaxlen);
    collation.collation= cs;
  }
  void fix_char_length(uint32 max_char_length_arg)
  {
    max_length= char_to_byte_length_safe(max_char_length_arg,
                                         collation.collation->mbmaxlen);
  }
  void fix_attributes_temporal(uint32 int_part_length, decimal_digits_t dec)
  {
    *this= Type_std_attributes(
             Type_temporal_attributes(int_part_length, dec, false),
             DTCollation_numeric());
  }
  void fix_attributes_date()
  {
    fix_attributes_temporal(MAX_DATE_WIDTH, 0);
  }
  void fix_attributes_time(decimal_digits_t dec)
  {
    fix_attributes_temporal(MIN_TIME_WIDTH, dec);
  }
  void fix_attributes_datetime(decimal_digits_t dec)
  {
    fix_attributes_temporal(MAX_DATETIME_WIDTH, dec);
  }

  void aggregate_attributes_int(Item **items, uint nitems)
  {
    collation= DTCollation_numeric();
    fix_char_length(find_max_char_length(items, nitems));
    unsigned_flag= count_unsigned(items, nitems) > 0;
    decimals= 0;
  }
  void aggregate_attributes_real(Item **items, uint nitems)
  {
    collation= DTCollation_numeric();
    aggregate_numeric_attributes_real(items, nitems);
  }
  void aggregate_attributes_decimal(Item **items, uint nitems,
                                    bool unsigned_arg)
  {
    collation= DTCollation_numeric();
    aggregate_numeric_attributes_decimal(items, nitems,
                                         (unsigned_flag= unsigned_arg));
  }
  bool aggregate_attributes_string(const LEX_CSTRING &func_name,
                                   Item **item, uint nitems);
  void aggregate_attributes_temporal(uint int_part_length,
                                     Item **item, uint nitems)
  {
    fix_attributes_temporal(int_part_length, find_max_decimals(item, nitems));
  }

  bool agg_item_collations(DTCollation &c, const LEX_CSTRING &name,
                           Item **items, uint nitems,
                           uint flags, int item_sep);
  bool agg_item_set_converter(const DTCollation &coll,
                              const LEX_CSTRING &name,
                              Item **args, uint nargs,
                              uint flags, int item_sep);

  /*
    Collect arguments' character sets together.
    We allow to apply automatic character set conversion in some cases.
    The conditions when conversion is possible are:
    - arguments A and B have different charsets
    - A wins according to coercibility rules
      (i.e. a column is stronger than a string constant,
       an explicit COLLATE clause is stronger than a column)
    - character set of A is either superset for character set of B,
      or B is a string constant which can be converted into the
      character set of A without data loss.

    If all of the above is true, then it's possible to convert
    B into the character set of A, and then compare according
    to the collation of A.

    For functions with more than two arguments:

      collect(A,B,C) ::= collect(collect(A,B),C)

    Since this function calls THD::change_item_tree() on the passed Item **
    pointers, it is necessary to pass the original Item **'s, not copies.
    Otherwise their values will not be properly restored (see BUG#20769).
    If the items are not consecutive (eg. args[2] and args[5]), use the
    item_sep argument, ie.

      agg_item_charsets(coll, fname, &args[2], 2, flags, 3)
  */
  bool agg_arg_charsets(DTCollation &c, const LEX_CSTRING &func_name,
                        Item **items, uint nitems,
                        uint flags, int item_sep)
  {
    if (agg_item_collations(c, func_name, items, nitems, flags, item_sep))
      return true;
    return agg_item_set_converter(c, func_name, items, nitems, flags, item_sep);
  }
  /*
    Aggregate arguments for string result, e.g: CONCAT(a,b)
    - convert to @@character_set_connection if all arguments are numbers
    - allow DERIVATION_NONE
  */
  bool agg_arg_charsets_for_string_result(DTCollation &c,
                                          const LEX_CSTRING &func_name,
                                          Item **items, uint nitems,
                                          int item_sep)
  {
    uint flags= MY_COLL_ALLOW_SUPERSET_CONV |
                MY_COLL_ALLOW_COERCIBLE_CONV |
                MY_COLL_ALLOW_NUMERIC_CONV;
    return agg_arg_charsets(c, func_name, items, nitems, flags, item_sep);
  }
  /*
    Aggregate arguments for string result, when some comparison
    is involved internally, e.g: REPLACE(a,b,c)
    - convert to @@character_set_connection if all arguments are numbers
    - disallow DERIVATION_NONE
  */
  bool agg_arg_charsets_for_string_result_with_comparison(DTCollation &c,
                                                          const LEX_CSTRING &func_name,
                                                          Item **items,
                                                          uint nitems,
                                                          int item_sep)
  {
    uint flags= MY_COLL_ALLOW_SUPERSET_CONV |
                MY_COLL_ALLOW_COERCIBLE_CONV |
                MY_COLL_ALLOW_NUMERIC_CONV |
                MY_COLL_DISALLOW_NONE;
    return agg_arg_charsets(c, func_name, items, nitems, flags, item_sep);
  }

  /*
    Aggregate arguments for comparison, e.g: a=b, a LIKE b, a RLIKE b
    - don't convert to @@character_set_connection if all arguments are numbers
    - don't allow DERIVATION_NONE
  */
  bool agg_arg_charsets_for_comparison(DTCollation &c,
                                       const LEX_CSTRING &func_name,
                                       Item **items, uint nitems,
                                       int item_sep)
  {
    uint flags= MY_COLL_ALLOW_SUPERSET_CONV |
                MY_COLL_ALLOW_COERCIBLE_CONV |
                MY_COLL_DISALLOW_NONE;
    return agg_arg_charsets(c, func_name, items, nitems, flags, item_sep);
  }

};


class Type_all_attributes: public Type_std_attributes
{
public:
  Type_all_attributes()
   :Type_std_attributes()
  { }
  Type_all_attributes(const Type_all_attributes &other)
   :Type_std_attributes(other)
  { }
  virtual ~Type_all_attributes() {}
  virtual void set_type_maybe_null(bool maybe_null_arg)= 0;
  // Returns total number of decimal digits
  virtual decimal_digits_t decimal_precision() const= 0;
  virtual const TYPELIB *get_typelib() const= 0;
  virtual void set_typelib(const TYPELIB *typelib)= 0;
};


class Type_cmp_attributes
{
public:
  virtual ~Type_cmp_attributes() { }
  virtual CHARSET_INFO *compare_collation() const= 0;
};


class Type_cast_attributes
{
  CHARSET_INFO *m_charset;
  ulonglong m_length;
  ulonglong m_decimals;
  bool m_length_specified;
  bool m_decimals_specified;
public:
  Type_cast_attributes(const char *c_len, const char *c_dec, CHARSET_INFO *cs)
    :m_charset(cs), m_length(0), m_decimals(0),
     m_length_specified(false), m_decimals_specified(false)
  {
    set_length_and_dec(c_len, c_dec);
  }
  Type_cast_attributes(CHARSET_INFO *cs)
    :m_charset(cs), m_length(0), m_decimals(0),
     m_length_specified(false), m_decimals_specified(false)
  { }
  void set_length_and_dec(const char *c_len, const char *c_dec)
  {
    int error;
    /*
      We don't have to check for error here as sql_yacc.yy has guaranteed
      that the values are in range of ulonglong
    */
    if ((m_length_specified= (c_len != NULL)))
      m_length= (ulonglong) my_strtoll10(c_len, NULL, &error);
    if ((m_decimals_specified= (c_dec != NULL)))
      m_decimals= (ulonglong) my_strtoll10(c_dec, NULL, &error);
  }
  CHARSET_INFO *charset() const { return m_charset; }
  bool length_specified() const { return m_length_specified; }
  bool decimals_specified() const { return m_decimals_specified; }
  ulonglong length() const { return m_length; }
  ulonglong decimals() const { return m_decimals; }
};


class Name: private LEX_CSTRING
{
public:
  Name(const char *str_arg, uint length_arg)
  {
    DBUG_ASSERT(length_arg < UINT_MAX32);
    LEX_CSTRING::str= str_arg;
    LEX_CSTRING::length= length_arg;
  }
  Name(const LEX_CSTRING &lcs)
  {
    LEX_CSTRING::str= lcs.str;
    LEX_CSTRING::length= lcs.length;
  }
  const char *ptr() const { return LEX_CSTRING::str; }
  uint length() const { return (uint) LEX_CSTRING::length; }
  const LEX_CSTRING &lex_cstring() const { return *this; }
  bool eq(const LEX_CSTRING &other) const
  {
    return !system_charset_info->strnncoll(LEX_CSTRING::str, LEX_CSTRING::length,
                                           other.str, other.length);
  }
};


class Bit_addr
{
  /**
    Byte where the bit is stored inside a record.
    If the corresponding Field is a NOT NULL field, this member is NULL.
  */
  uchar *m_ptr;
  /**
    Offset of the bit inside m_ptr[0], in the range 0..7.
  */
  uchar m_offs;
public:
  Bit_addr()
   :m_ptr(NULL),
    m_offs(0)
  { }
  Bit_addr(uchar *ptr, uchar offs)
   :m_ptr(ptr), m_offs(offs)
  {
    DBUG_ASSERT(ptr || offs == 0);
    DBUG_ASSERT(offs < 8);
  }
  Bit_addr(bool maybe_null)
   :m_ptr(maybe_null ? (uchar *) "" : NULL),
    m_offs(0)
  { }
  uchar *ptr() const { return m_ptr; }
  uchar offs() const { return m_offs; }
  uchar bit() const { return static_cast<uchar>(m_ptr ? 1U << m_offs : 0); }
  void inc()
  {
    DBUG_ASSERT(m_ptr);
    m_ptr+= (m_offs == 7);
    m_offs= (m_offs + 1) & 7;
  }
};


class Record_addr
{
  uchar *m_ptr;      // Position of the field in the record
  Bit_addr m_null;   // Position and offset of the null bit
public:
  Record_addr(uchar *ptr_arg,
              uchar *null_ptr_arg,
              uchar null_bit_arg)
   :m_ptr(ptr_arg),
    m_null(null_ptr_arg, null_bit_arg)
  { }
  Record_addr(uchar *ptr, const Bit_addr &null)
   :m_ptr(ptr),
    m_null(null)
  { }
  Record_addr(bool maybe_null)
   :m_ptr(NULL),
    m_null(maybe_null)
  { }
  uchar *ptr() const { return m_ptr; }
  const Bit_addr &null() const { return m_null; }
  uchar *null_ptr() const { return m_null.ptr(); }
  uchar null_bit() const { return m_null.bit(); }
};


class Information_schema_numeric_attributes
{
  enum enum_attr
  {
    ATTR_NONE= 0,
    ATTR_PRECISION= 1,
    ATTR_SCALE= 2,
    ATTR_PRECISION_AND_SCALE= (ATTR_PRECISION|ATTR_SCALE)
  };
  uint m_precision;
  decimal_digits_t m_scale;
  enum_attr m_available_attributes;
public:
  Information_schema_numeric_attributes()
   :m_precision(0), m_scale(0),
    m_available_attributes(ATTR_NONE)
  { }
  Information_schema_numeric_attributes(uint precision)
   :m_precision(precision), m_scale(0),
    m_available_attributes(ATTR_PRECISION)
  { }
  Information_schema_numeric_attributes(uint precision, decimal_digits_t scale)
   :m_precision(precision), m_scale(scale),
    m_available_attributes(ATTR_PRECISION_AND_SCALE)
  { }
  bool has_precision() const { return m_available_attributes & ATTR_PRECISION; }
  bool has_scale() const { return m_available_attributes & ATTR_SCALE; }
  uint precision() const
  {
    DBUG_ASSERT(has_precision());
    return (uint) m_precision;
  }
  decimal_digits_t scale() const
  {
    DBUG_ASSERT(has_scale());
    return m_scale;
  }
};


class Information_schema_character_attributes
{
  uint32 m_octet_length;
  uint32 m_char_length;
  bool m_is_set;
public:
  Information_schema_character_attributes()
   :m_octet_length(0), m_char_length(0), m_is_set(false)
  { }
  Information_schema_character_attributes(uint32 octet_length,
                                          uint32 char_length)
   :m_octet_length(octet_length), m_char_length(char_length), m_is_set(true)
  { }
  bool has_octet_length() const { return m_is_set; }
  bool has_char_length() const { return m_is_set; }
  uint32 octet_length() const
  {
    DBUG_ASSERT(has_octet_length());
    return m_octet_length;
  }
  uint char_length() const
  {
    DBUG_ASSERT(has_char_length());
    return m_char_length;
  }
};


enum vers_kind_t
{
  VERS_UNDEFINED= 0,
  VERS_TIMESTAMP,
  VERS_TRX_ID
};


class Vers_type_handler
{
protected:
  Vers_type_handler() {}
public:
  virtual ~Vers_type_handler() {}
  virtual vers_kind_t kind() const
  {
    DBUG_ASSERT(0);
    return VERS_UNDEFINED;
  }
  virtual bool check_sys_fields(const LEX_CSTRING &table_name,
                                const Column_definition *row_start,
                                const Column_definition *row_end) const= 0;
};


class Vers_type_timestamp: public Vers_type_handler
{
public:
  virtual vers_kind_t kind() const
  {
    return VERS_TIMESTAMP;
  }
  bool check_sys_fields(const LEX_CSTRING &table_name,
                        const Column_definition *row_start,
                        const Column_definition *row_end) const;
};
extern Vers_type_timestamp vers_type_timestamp;


class Vers_type_trx: public Vers_type_handler
{
public:
  virtual vers_kind_t kind() const
  {
    return VERS_TRX_ID;
  }
  bool check_sys_fields(const LEX_CSTRING &table_name,
                        const Column_definition *row_start,
                        const Column_definition *row_end) const;
};
extern MYSQL_PLUGIN_IMPORT Vers_type_trx vers_type_trx;


class Type_handler
{
  Name m_name;
protected:
  String *print_item_value_csstr(THD *thd, Item *item, String *str) const;
  String *print_item_value_temporal(THD *thd, Item *item, String *str,
                                     const Name &type_name, String *buf) const;
  void make_sort_key_longlong(uchar *to,
                              bool maybe_null, bool null_value,
                              bool unsigned_flag,
                              longlong value) const;
  void store_sort_key_longlong(uchar *to, bool unsigned_flag,
                               longlong value) const;

  uint make_packed_sort_key_longlong(uchar *to, bool maybe_null,
                                     bool null_value, bool unsigned_flag,
                                     longlong value,
                                     const SORT_FIELD_ATTR *sort_field) const;

  bool Item_func_or_sum_illegal_param(const LEX_CSTRING &name) const;
  bool Item_func_or_sum_illegal_param(const Item_func_or_sum *) const;
  bool check_null(const Item *item, st_value *value) const;
  bool Item_send_str(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_tiny(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_short(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_long(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_longlong(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_float(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_double(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_time(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_date(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_timestamp(Item *item, Protocol *protocol, st_value *buf) const;
  bool Item_send_datetime(Item *item, Protocol *protocol, st_value *buf) const;
  bool Column_definition_prepare_stage2_legacy(Column_definition *c,
                                               enum_field_types type)
                                               const;
  bool Column_definition_prepare_stage2_legacy_num(Column_definition *c,
                                                   enum_field_types type)
                                                   const;
  bool Column_definition_prepare_stage2_legacy_real(Column_definition *c,
                                                    enum_field_types type)
                                                    const;
public:
  static const Type_handler *handler_by_name(THD *thd, const LEX_CSTRING &name);
  static const Type_handler *handler_by_name_or_error(THD *thd,
                                                      const LEX_CSTRING &name);
  static const Type_handler *odbc_literal_type_handler(const LEX_CSTRING *str);
  static const Type_handler *blob_type_handler(uint max_octet_length);
  static const Type_handler *string_type_handler(uint max_octet_length);
  static const Type_handler *bit_and_int_mixture_handler(uint max_char_len);
  static const Type_handler *type_handler_long_or_longlong(uint max_char_len,
                                                           bool unsigned_flag);
  /**
    Return a string type handler for Item
    If too_big_for_varchar() returns a BLOB variant, according to length.
    If max_length > 0 create a VARCHAR(n)
    If max_length == 0 create a CHAR(0)
    @param item - the Item to get the handler to.
  */
  static const Type_handler *varstring_type_handler(const Item *item);
  static const Type_handler *blob_type_handler(const Item *item);
  static const Type_handler *get_handler_by_field_type(enum_field_types type);
  static const Type_handler *get_handler_by_real_type(enum_field_types type);
  static const Type_handler *get_handler_by_cmp_type(Item_result type);
  static const Type_collection *
    type_collection_for_aggregation(const Type_handler *h1,
                                    const Type_handler *h2);
  virtual const Type_collection *type_collection() const;
  static const
  Type_handler *aggregate_for_result_traditional(const Type_handler *h1,
                                                 const Type_handler *h2);
  virtual Schema *schema() const;
  static void partition_field_type_not_allowed(const LEX_CSTRING &field_name);
  static bool partition_field_check_result_type(Item *item,
                                                Item_result expected_type);
  static const Name & version_mysql56();
  static const Name & version_mariadb53();

  void set_name(Name n) { DBUG_ASSERT(!m_name.ptr()); m_name= n; }
  const Name name() const { return m_name; }
  virtual const Name version() const;
  virtual const Name &default_value() const= 0;
  virtual uint32 flags() const { return 0; }
  virtual ulong KEY_pack_flags(uint column_nr) const { return 0; }
  bool is_unsigned() const { return flags() & UNSIGNED_FLAG; }
  virtual enum_field_types field_type() const= 0;
  virtual enum_field_types real_field_type() const { return field_type(); }
  /**
    Type code which is used for merging of traditional data types for result
    (for UNION and for hybrid functions such as COALESCE).
    Mapping can be done both ways: old->new, new->old, depending
    on the particular data type implementation:
    - type_handler_var_string (MySQL-4.1 old VARCHAR) is converted to
      new VARCHAR before merging.
      field_type_merge_rules[][] returns new VARCHAR.
    - type_handler_newdate is converted to old DATE before merging.
      field_type_merge_rules[][] returns NEWDATE.
    - Temporal type_handler_xxx2 (new MySQL-5.6 types) are converted to
      corresponding old type codes before merging (e.g. TIME2->TIME).
      field_type_merge_rules[][] returns old type codes (e.g. TIME).
      Then old types codes are supposed to convert to new type codes somehow,
      but they do not. So UNION and COALESCE create old columns.
      This is a bug and should be fixed eventually.
  */
  virtual enum_field_types traditional_merge_field_type() const
  {
    DBUG_ASSERT(is_traditional_scalar_type());
    return field_type();
  }
  virtual enum_field_types type_code_for_protocol() const
  {
    return field_type();
  }
  virtual protocol_send_type_t protocol_send_type() const= 0;
  virtual bool Item_append_extended_type_info(Send_field_extended_metadata *to,
                                              const Item *item) const
  {
    return false;
  }
  virtual Item_result result_type() const= 0;
  virtual Item_result cmp_type() const= 0;
  virtual enum_dynamic_column_type
    dyncol_type(const Type_all_attributes *attr) const= 0;
  virtual enum_mysql_timestamp_type mysql_timestamp_type() const
  {
    return MYSQL_TIMESTAMP_ERROR;
  }
  /*
    Return true if the native format is fully implemented for a data type:
    - Field_xxx::val_native()
    - Item_xxx::val_native() for all classes supporting this data type
    - Type_handler_xxx::cmp_native()
  */
  virtual bool is_val_native_ready() const
  {
    return false;
  }
  /*
    If operations such as:
      UPDATE t1 SET binary_string_field=this_type_field;
    should store this_type_field->val_native() rather than
    this_type_field->val_str().
  */
  virtual bool convert_to_binary_using_val_native() const
  {
    return false;
  }
  virtual bool is_timestamp_type() const
  {
    return false;
  }
  virtual bool is_order_clause_position_type() const
  {
    return false;
  }
  virtual bool is_limit_clause_valid_type() const
  {
    return false;
  }
  /*
    Returns true if this data type supports a hack that
      WHERE notnull_column IS NULL
    finds zero values, e.g.:
      WHERE date_notnull_column IS NULL        ->
      WHERE date_notnull_column = '0000-00-00'
  */
  virtual bool cond_notnull_field_isnull_to_field_eq_zero() const
  {
    return false;
  }
  /**
    Check whether a field type can be partially indexed by a key.
    @param  type   field type
    @retval true   Type can have a prefixed key
    @retval false  Type can not have a prefixed key
  */
  virtual bool type_can_have_key_part() const
  {
    return false;
  }
  virtual bool type_can_have_auto_increment_attribute() const
  {
    return false;
  }
  virtual uint max_octet_length() const { return 0; }
  /**
    Prepared statement long data:
    Check whether this parameter data type is compatible with long data.
    Used to detect whether a long data stream has been supplied to a
    incompatible data type.
  */
  virtual bool is_param_long_data_type() const { return false; }
  virtual const Type_handler *type_handler_for_comparison() const= 0;
  virtual const Type_handler *type_handler_for_native_format() const
  {
    return this;
  }
  virtual const Type_handler *type_handler_for_item_field() const
  {
    return this;
  }
  virtual const Type_handler *type_handler_for_tmp_table(const Item *) const
  {
    return this;
  }
  virtual const Type_handler *type_handler_for_union(const Item *) const
  {
    return this;
  }
  virtual const Type_handler *cast_to_int_type_handler() const
  {
    return this;
  }
  virtual const Type_handler *type_handler_unsigned() const
  {
    return this;
  }
  virtual const Type_handler *type_handler_signed() const
  {
    return this;
  }
  virtual bool partition_field_check(const LEX_CSTRING &field_name,
                                     Item *item_expr) const
  {
    partition_field_type_not_allowed(field_name);
    return true;
  }
  virtual bool partition_field_append_value(String *str,
                                            Item *item_expr,
                                            CHARSET_INFO *field_cs,
                                            partition_value_print_mode_t mode)
                                            const;
  virtual int
  stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const= 0;
  virtual CHARSET_INFO *charset_for_protocol(const Item *item) const;
  virtual const Type_handler*
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const
  { return this; }
  virtual bool adjust_spparam_type(Spvar_definition *def, Item *from) const
  {
    return false;
  }
  Type_handler() : m_name(0,0) {}
  virtual ~Type_handler() {}
  /**
    Determines MariaDB traditional scalar data types that always present
    in the server.
  */
  bool is_traditional_scalar_type() const;
  virtual bool is_scalar_type() const { return true; }
  virtual bool can_return_int() const { return true; }
  virtual bool can_return_decimal() const { return true; }
  virtual bool can_return_real() const { return true; }
  virtual bool can_return_str() const { return true; }
  virtual bool can_return_text() const { return true; }
  virtual bool can_return_date() const { return true; }
  virtual bool can_return_time() const { return true; }
  virtual bool can_return_extract_source(interval_type type) const;
  virtual bool is_bool_type() const { return false; }
  virtual bool is_general_purpose_string_type() const { return false; }
  virtual decimal_digits_t Item_time_precision(THD *thd, Item *item) const;
  virtual decimal_digits_t Item_datetime_precision(THD *thd, Item *item) const;
  virtual decimal_digits_t Item_decimal_scale(const Item *item) const;
  virtual decimal_digits_t Item_decimal_precision(const Item *item) const= 0;
  /*
    Returns how many digits a divisor adds into a division result.
    See Item::divisor_precision_increment() in item.h for more comments.
  */
  virtual decimal_digits_t Item_divisor_precision_increment(const Item *) const;
  /**
    Makes a temporary table Field to handle numeric aggregate functions,
    e.g. SUM(DISTINCT expr), AVG(DISTINCT expr), etc.
  */
  virtual Field *make_num_distinct_aggregator_field(MEM_ROOT *,
                                                    const Item *) const;
  /**
    Makes a temporary table Field to handle RBR replication type conversion.
    @param TABLE    - The conversion table the field is going to be added to.
                      It's used to access to table->in_use->mem_root,
                      to create the new field on the table memory root,
                      as well as to increment statistics in table->share
                      (e.g. table->s->blob_count).
    @param metadata - Metadata from the binary log.
    @param target   - The field in the target table on the slave.

    Note, the data types of "target" and of "this" are not necessarily
    always the same, in general case it's possible that:
            this->field_type() != target->field_type()
    and/or
            this->real_type( ) != target->real_type()

    This method decodes metadata according to this->real_type()
    and creates a new field also according to this->real_type().

    In some cases it lurks into "target", to get some extra information, e.g.:
    - unsigned_flag for numeric fields
    - charset() for string fields
    - typelib and field_length for SET and ENUM
    - geom_type and srid for GEOMETRY
    This information is not available in the binary log, so
    we assume that these fields are the same on the master and on the slave.
  */
  virtual Field *make_conversion_table_field(MEM_ROOT *root,
                                             TABLE *table,
                                             uint metadata,
                                             const Field *target) const= 0;
  virtual void show_binlog_type(const Conv_source &src, const Field &dst,
                                String *str) const;
  virtual uint32 max_display_length_for_field(const Conv_source &src) const= 0;
  /*
    Performs the final data type validation for a UNION element,
    after the regular "aggregation for result" was done.
  */
  virtual bool union_element_finalize(Item_type_holder* item) const
  {
    return false;
  }
  virtual uint Column_definition_gis_options_image(uchar *buff,
                                                   const Column_definition &def)
                                                   const
  {
    return 0;
  }
  virtual bool Column_definition_data_type_info_image(Binary_string *to,
                                                   const Column_definition &def)
                                                   const;
  // Check if the implicit default value is Ok in the current sql_mode
  virtual bool validate_implicit_default_value(THD *thd,
                                               const Column_definition &def)
                                               const;
  // Automatic upgrade, e.g. for ALTER TABLE t1 FORCE
  virtual void Column_definition_implicit_upgrade(Column_definition *c) const
  { }
  // Validate CHECK constraint after the parser
  virtual bool Column_definition_validate_check_constraint(THD *thd,
                                                           Column_definition *c)
                                                           const;
  // Set attributes in the parser
  virtual bool Column_definition_set_attributes(THD *thd,
                                                Column_definition *def,
                                                const Lex_field_type_st &attr,
                                                CHARSET_INFO *cs,
                                                column_definition_type_t type)
                                                const;
  // Fix attributes after the parser
  virtual bool Column_definition_fix_attributes(Column_definition *c) const= 0;
  /*
    Fix attributes from an existing field. Used for:
    - ALTER TABLE (for columns that do not change)
    - DECLARE var TYPE OF t1.col1; (anchored SP variables)
  */
  virtual void Column_definition_reuse_fix_attributes(THD *thd,
                                                      Column_definition *c,
                                                      const Field *field) const
  { }
  virtual bool Column_definition_prepare_stage1(THD *thd,
                                                MEM_ROOT *mem_root,
                                                Column_definition *c,
                                                handler *file,
                                                ulonglong table_flags,
                                                const Column_derived_attributes
                                                      *derived_attr)
                                                const;
  virtual bool Column_definition_bulk_alter(Column_definition *c,
                                            const Column_derived_attributes
                                                  *derived_attr,
                                            const Column_bulk_alter_attributes
                                                  *bulk_alter_attr)
                                            const
  { return false; }
  /*
    This method is called on queries like:
      CREATE TABLE t2 (a INT) AS SELECT a FROM t1;
    I.e. column "a" is queried from another table,
    but its data type is redefined.
    @param OUT def   - The column definition to be redefined
    @param IN  dup   - The column definition to take the data type from
                       (i.e. "a INT" in the above example).
    @param IN file   - Table owner handler. If it does not support certain
                       data types, some conversion can be applied.
                       I.g. true BIT to BIT-AS-CHAR.
    @param IN schema - the owner schema definition, e.g. for the default
                       character set and collation.
    @retval true     - on error
    @retval false    - on success
  */
  virtual bool Column_definition_redefine_stage1(Column_definition *def,
                                                 const Column_definition *dup,
                                                 const handler *file)
                                                 const;
  virtual bool Column_definition_prepare_stage2(Column_definition *c,
                                                handler *file,
                                                ulonglong table_flags) const= 0;
  virtual bool Key_part_spec_init_primary(Key_part_spec *part,
                                          const Column_definition &def,
                                          const handler *file) const;
  virtual bool Key_part_spec_init_unique(Key_part_spec *part,
                                         const Column_definition &def,
                                         const handler *file,
                                         bool *has_key_needed) const;
  virtual bool Key_part_spec_init_multiple(Key_part_spec *part,
                                           const Column_definition &def,
                                           const handler *file) const;
  virtual bool Key_part_spec_init_foreign(Key_part_spec *part,
                                          const Column_definition &def,
                                          const handler *file) const;
  virtual bool Key_part_spec_init_spatial(Key_part_spec *part,
                                          const Column_definition &def) const;
  virtual bool Key_part_spec_init_ft(Key_part_spec *part,
                                     const Column_definition &def) const
  {
    return true; // Error
  }
  virtual Field *make_table_field(MEM_ROOT *root,
                                  const LEX_CSTRING *name,
                                  const Record_addr &addr,
                                  const Type_all_attributes &attr,
                                  TABLE_SHARE *share) const= 0;
  Field *make_and_init_table_field(MEM_ROOT *root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Type_all_attributes &attr,
                                   TABLE *table) const;
  virtual Field *make_schema_field(MEM_ROOT *root,
                                   TABLE *table,
                                   const Record_addr &addr,
                                   const ST_FIELD_INFO &def) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  virtual Field *
  make_table_field_from_def(TABLE_SHARE *share,
                            MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &addr,
                            const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const= 0;
  virtual void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const;
  virtual const Type_handler *type_handler_frm_unpack(const uchar *buffer) const
  {
    return this;
  }
  virtual bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options) const;

  /*
    Create a fixed size key part for a sort key
  */
  virtual void make_sort_key_part(uchar *to, Item *item,
                                  const SORT_FIELD_ATTR *sort_field,
                                  Sort_param *param) const= 0;

  /*
    create a compact size key part for a sort key
  */
  virtual uint make_packed_sort_key_part(uchar *to, Item *item,
                                         const SORT_FIELD_ATTR *sort_field,
                                         Sort_param *param) const=0;

  virtual void sort_length(THD *thd,
                          const Type_std_attributes *item,
                          SORT_FIELD_ATTR *attr) const= 0;
  virtual bool is_packable() const { return false; }

  virtual uint32 max_display_length(const Item *item) const= 0;
  virtual uint32 Item_decimal_notation_int_digits(const Item *item) const { return 0; }
  virtual uint32 calc_pack_length(uint32 length) const= 0;
  virtual uint calc_key_length(const Column_definition &def) const;
  virtual void Item_update_null_value(Item *item) const= 0;
  virtual bool Item_save_in_value(THD *thd, Item *item, st_value *value) const= 0;
  virtual void Item_param_setup_conversion(THD *thd, Item_param *) const {}
  virtual void Item_param_set_param_func(Item_param *param,
                                         uchar **pos, ulong len) const;
  virtual bool Item_param_set_from_value(THD *thd,
                                         Item_param *param,
                                         const Type_all_attributes *attr,
                                         const st_value *value) const= 0;
  virtual bool Item_param_val_native(THD *thd,
                                         Item_param *item,
                                         Native *to) const;
  virtual bool Item_send(Item *item, Protocol *p, st_value *buf) const= 0;
  virtual int Item_save_in_field(Item *item, Field *field,
                                 bool no_conversions) const= 0;

  /**
    Return a string representation of the Item value.

    @param thd     thread handle
    @param str     string buffer for representation of the value

    @note
      If the item has a string result type, the string is escaped
      according to its character set.

    @retval
      NULL      on error
    @retval
      non-NULL  a pointer to a a valid string on success
  */
  virtual String *print_item_value(THD *thd, Item *item, String *str) const= 0;

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
  virtual bool
  can_change_cond_ref_to_const(Item_bool_func2 *target,
                               Item *target_expr, Item *target_value,
                               Item_bool_func2 *source,
                               Item *source_expr, Item *source_const) const= 0;

  /*
    @brief
      Check if an IN subquery allows materialization or not
    @param
      inner              expression on the inner side of the IN subquery
      outer              expression on the outer side of the IN subquery
      is_in_predicate    SET to true if IN subquery was converted from an
                         IN predicate or we are checking if materialization
                         strategy can be used for an IN predicate
  */
  virtual bool
  subquery_type_allows_materialization(const Item *inner,
                                       const Item *outer,
                                       bool is_in_predicate) const= 0;
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
  virtual Item *make_const_item_for_comparison(THD *thd,
                                               Item *src,
                                               const Item *cmp) const= 0;
  virtual Item_cache *Item_get_cache(THD *thd, const Item *item) const= 0;
  virtual Item *make_constructor_item(THD *thd, List<Item> *args) const
  {
    return NULL;
  }
  /**
    A builder for literals with data type name prefix, e.g.:
      TIME'00:00:00', DATE'2001-01-01', TIMESTAMP'2001-01-01 00:00:00'.
    @param thd          The current thread
    @param str          Character literal
    @param length       Length of str
    @param cs           Character set of the string
    @param send_error   Whether to generate an error on failure

    @retval             A pointer to a new Item on success
                        NULL on error (wrong literal value, EOM)
  */
  virtual Item_literal *create_literal_item(THD *thd,
                                            const char *str, size_t length,
                                            CHARSET_INFO *cs,
                                            bool send_error) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  Item_literal *create_literal_item(THD *thd, const String *str,
                                    bool send_error) const
  {
    return create_literal_item(thd, str->ptr(), str->length(), str->charset(),
                               send_error);
  }
  virtual Item *create_typecast_item(THD *thd, Item *item,
                                     const Type_cast_attributes &attr) const
  {
    return NULL;
  }
  virtual Item_copy *create_item_copy(THD *thd, Item *item) const;
  virtual int cmp_native(const Native &a, const Native &b) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  virtual bool set_comparator_func(THD *thd, Arg_comparator *cmp) const= 0;
  virtual bool Item_const_eq(const Item_const *a, const Item_const *b,
                             bool binary_cmp) const
  {
    return false;
  }
  virtual bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                             Item *a, Item *b) const= 0;
  virtual bool Item_hybrid_func_fix_attributes(THD *thd,
                                               const LEX_CSTRING &name,
                                               Type_handler_hybrid_field_type *,
                                               Type_all_attributes *atrr,
                                               Item **items,
                                               uint nitems) const= 0;
  virtual bool Item_func_min_max_fix_attributes(THD *thd,
                                                Item_func_min_max *func,
                                                Item **items,
                                                uint nitems) const;
  virtual bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const= 0;
  virtual bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const= 0;
  virtual bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const= 0;
  virtual
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const= 0;

  virtual bool Item_val_native_with_conversion(THD *thd, Item *item,
                                               Native *to) const
  {
    return true;
  }
  virtual bool Item_val_native_with_conversion_result(THD *thd, Item *item,
                                                      Native *to) const
  {
    return true;
  }

  virtual bool Item_val_bool(Item *item) const= 0;
  virtual void Item_get_date(THD *thd, Item *item,
                             Temporal::Warn *buff, MYSQL_TIME *ltime,
                             date_mode_t fuzzydate) const= 0;
  bool Item_get_date_with_warn(THD *thd, Item *item, MYSQL_TIME *ltime,
                               date_mode_t fuzzydate) const;
  virtual longlong Item_val_int_signed_typecast(Item *item) const= 0;
  virtual longlong Item_val_int_unsigned_typecast(Item *item) const= 0;

  virtual String *Item_func_hex_val_str_ascii(Item_func_hex *item,
                                              String *str) const= 0;

  virtual
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const= 0;
  virtual
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const= 0;
  virtual
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const= 0;
  virtual
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const= 0;
  virtual
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const= 0;
  bool Item_func_hybrid_field_type_get_date_with_warn(THD *thd,
                                                Item_func_hybrid_field_type *,
                                                MYSQL_TIME *,
                                                date_mode_t) const;
  virtual
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const= 0;
  virtual
  double Item_func_min_max_val_real(Item_func_min_max *) const= 0;
  virtual
  longlong Item_func_min_max_val_int(Item_func_min_max *) const= 0;
  virtual
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const= 0;
  virtual
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate) const= 0;
  virtual bool
  Item_func_between_fix_length_and_dec(Item_func_between *func) const= 0;
  virtual longlong
  Item_func_between_val_int(Item_func_between *func) const= 0;

  virtual cmp_item *
  make_cmp_item(THD *thd, CHARSET_INFO *cs) const= 0;

  virtual in_vector *
  make_in_vector(THD *thd, const Item_func_in *func, uint nargs) const= 0;

  virtual bool
  Item_func_in_fix_comparator_compatible_types(THD *thd, Item_func_in *)
                                                               const= 0;

  virtual bool
  Item_func_round_fix_length_and_dec(Item_func_round *round) const= 0;

  virtual bool
  Item_func_int_val_fix_length_and_dec(Item_func_int_val *func) const= 0;

  virtual bool
  Item_func_abs_fix_length_and_dec(Item_func_abs *func) const= 0;

  virtual bool
  Item_func_neg_fix_length_and_dec(Item_func_neg *func) const= 0;

  virtual bool
  Item_func_signed_fix_length_and_dec(Item_func_signed *item) const;
  virtual bool
  Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const;
  virtual bool
  Item_double_typecast_fix_length_and_dec(Item_double_typecast *item) const;
  virtual bool
  Item_float_typecast_fix_length_and_dec(Item_float_typecast *item) const;
  virtual bool
  Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item) const;
  virtual bool
  Item_char_typecast_fix_length_and_dec(Item_char_typecast *item) const;
  virtual bool
  Item_time_typecast_fix_length_and_dec(Item_time_typecast *item) const;
  virtual bool
  Item_date_typecast_fix_length_and_dec(Item_date_typecast *item) const;
  virtual bool
  Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item) const;

  virtual bool
  Item_func_plus_fix_length_and_dec(Item_func_plus *func) const= 0;
  virtual bool
  Item_func_minus_fix_length_and_dec(Item_func_minus *func) const= 0;
  virtual bool
  Item_func_mul_fix_length_and_dec(Item_func_mul *func) const= 0;
  virtual bool
  Item_func_div_fix_length_and_dec(Item_func_div *func) const= 0;
  virtual bool
  Item_func_mod_fix_length_and_dec(Item_func_mod *func) const= 0;

  virtual const Vers_type_handler *vers() const { return NULL; }
};


/*
  Special handler for ROW
*/
class Type_handler_row: public Type_handler
{
public:
  virtual ~Type_handler_row() {}
  const Name &default_value() const override;
  bool validate_implicit_default_value(THD *thd,
                                       const Column_definition &def) const
                                       override
  {
    DBUG_ASSERT(0);
    return true;
  }
  const Type_collection *type_collection() const override;
  bool is_scalar_type() const override { return false; }
  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_str() const override { return false; }
  bool can_return_text() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
  enum_field_types field_type() const override
  {
    DBUG_ASSERT(0);
    return MYSQL_TYPE_NULL;
  };
  protocol_send_type_t protocol_send_type() const override
  {
    DBUG_ASSERT(0);
    return PROTOCOL_SEND_STRING;
  }
  Item_result result_type() const override
  {
    return ROW_RESULT;
  }
  Item_result cmp_type() const override
  {
    return ROW_RESULT;
  }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    DBUG_ASSERT(0);
    return DYN_COL_NULL;
  }
  const Type_handler *type_handler_for_comparison() const override;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  bool subquery_type_allows_materialization(const Item *, const Item *,
                                            bool) const override
  {
    DBUG_ASSERT(0);
    return false;
  }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table,
                                     uint metadata,
                                     const Field *target) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool Column_definition_fix_attributes(Column_definition *c) const override
  {
    return false;
  }
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field)
                                              const override
  {
    DBUG_ASSERT(0);
  }
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  {
    return false;
  }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          Sort_param *param) const override
  {
    DBUG_ASSERT(0);
  }
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 Sort_param *param) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  void sort_length(THD *thd, const Type_std_attributes *item,
                   SORT_FIELD_ATTR *attr) const override
  {
    DBUG_ASSERT(0);
  }
  uint32 max_display_length(const Item *item) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  uint32 calc_pack_length(uint32 length) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  decimal_digits_t Item_decimal_precision(const Item *item) const override
  {
    DBUG_ASSERT(0);
    return DECIMAL_MAX_PRECISION;
  }
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  void Item_update_null_value(Item *item) const override;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                         const override
  {
    DBUG_ASSERT(0);
    return 1;
  }
  String *print_item_value(THD *thd, Item *item, String *str) const override;
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const)
                                   const override
  {
    DBUG_ASSERT(0);
    return false;
  }
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp)
                                       const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  Item_copy *create_item_copy(THD *thd, Item *item) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
                                       const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_val_bool(Item *item) const override
  {
    DBUG_ASSERT(0);
    return false;
  }
  void Item_get_date(THD *thd, Item *item,
                     Temporal::Warn *warn, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const override
  {
    DBUG_ASSERT(0);
    set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
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
    DBUG_ASSERT(0);
    return NULL;
  }
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override
  {
    DBUG_ASSERT(0);
    return 0.0;
  }
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *ltime,
                                            date_mode_t fuzzydate)
                                            const override
  {
    DBUG_ASSERT(0);
    set_zero_time(ltime, MYSQL_TIMESTAMP_NONE);
  }

  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  double Item_func_min_max_val_real(Item_func_min_max *) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate)
                                  const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_func_between_fix_length_and_dec(Item_func_between *func)
                                            const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *thd, const Item_func_in *f, uint nargs)
                            const override;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd, Item_func_in *)
                                                    const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;

  bool Item_func_signed_fix_length_and_dec(Item_func_signed *) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *)
                                               const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_float_typecast_fix_length_and_dec(Item_float_typecast *)
                                              const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *)
                                                const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *)
                                             const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *)
                                             const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *)
                                             const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *)
                                                 const override
  {
    DBUG_ASSERT(0);
    return true;
  }

  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const override;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const override;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const override;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const override;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const override;
};


/*
  A common parent class for numeric data type handlers
*/
class Type_handler_numeric: public Type_handler
{
public:
  const Name &default_value() const override;
  String *print_item_value(THD *thd, Item *item, String *str) const override;
  double Item_func_min_max_val_real(Item_func_min_max *) const override;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const override;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate)
                                  const override;
  virtual ~Type_handler_numeric() { }
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const)
                                   const override;
  bool Item_func_between_fix_length_and_dec(Item_func_between *func) const override;
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const override;
};


/*** Abstract classes for every XXX_RESULT */

class Type_handler_real_result: public Type_handler_numeric
{
public:
  Item_result result_type() const override{ return REAL_RESULT; }
  Item_result cmp_type() const override { return REAL_RESULT; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_DOUBLE;
  }
  virtual ~Type_handler_real_result() {}
  const Type_handler *type_handler_for_comparison() const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field)
                                              const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const override;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item)
                               const override;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate)
                                            const override;
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          Sort_param *param) const override;
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 Sort_param *param) const override;
  void sort_length(THD *thd,
                   const Type_std_attributes *item,
                   SORT_FIELD_ATTR *attr) const override;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const override;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  void Item_update_null_value(Item *item) const override;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                         const override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp)
                                       const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
                                       const override;
  bool Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems)
                                        const override;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override;
  bool Item_func_signed_fix_length_and_dec(Item_func_signed *item)
                                           const override;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item)
                                             const override;
  bool Item_val_bool(Item *item) const override;
  void Item_get_date(THD *thd, Item *item, Temporal::Warn *warn,
                     MYSQL_TIME *ltime,  date_mode_t fuzzydate) const override;
  longlong Item_val_int_signed_typecast(Item *item) const override;
  longlong Item_val_int_unsigned_typecast(Item *item) const override;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str)
                                      const override;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override;
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate)
                                            const override;
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs)
                            const override;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd, Item_func_in *)
                                                    const override;

  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const override;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const override;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const override;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const override;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const override;
};


class Type_handler_decimal_result: public Type_handler_numeric
{
public:
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_STRING;
  }
  Item_result result_type() const override { return DECIMAL_RESULT; }
  Item_result cmp_type() const override { return DECIMAL_RESULT; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *) const
    override
  {
    return DYN_COL_DECIMAL;
  }
  virtual ~Type_handler_decimal_result() {};
  const Type_handler *type_handler_for_comparison() const override;
  int stored_field_cmp_to_item(THD *, Field *field, Item *item) const override
  {
    VDec item_val(item);
    return item_val.is_null() ? 0 : my_decimal(field).cmp(item_val.ptr());
  }
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate)
    const override;
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *)
    const override;
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          Sort_param *param) const override;
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 Sort_param *param) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const override;
  void sort_length(THD *thd,
                   const Type_std_attributes *item,
                   SORT_FIELD_ATTR *attr) const override;
  uint32 max_display_length(const Item *item) const override;
  uint32 Item_decimal_notation_int_digits(const Item *item) const override;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const override;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override
  {
    VDec va(a), vb(b);
    return va.ptr() && vb.ptr() && !va.cmp(vb);
  }
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_str(item, protocol, buf);
  }
  void Item_update_null_value(Item *item) const override;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const
    override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const
    override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
    const override;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance*) const override;
  bool Item_val_bool(Item *item) const override
  {
    return VDec(item).to_bool();
  }
  void Item_get_date(THD *thd, Item *item, Temporal::Warn *warn,
                     MYSQL_TIME *ltime,  date_mode_t fuzzydate) const override;
  longlong Item_val_int_signed_typecast(Item *item) const override;
  longlong Item_val_int_unsigned_typecast(Item *item) const override
  {
    return VDec(item).to_longlong(true);
  }
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str)
    const override;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
    const override;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
    const override;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override;
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate)
    const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *)
    const override;
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs)
    const override;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd, Item_func_in *)
    const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const override;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const override;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const override;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const override;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const override;
};


class Type_limits_int
{
private:
  uint32 m_precision;
  uint32 m_char_length;
public:
  Type_limits_int(uint32 prec, uint32 nchars)
   :m_precision(prec), m_char_length(nchars)
  { }
  uint32 precision() const { return m_precision; }
  uint32 char_length() const { return m_char_length; }
};


/*
  UNDIGNED TINYINT:    0..255   digits=3 nchars=3
  SIGNED TINYINT  : -128..127   digits=3 nchars=4
*/
class Type_limits_uint8: public Type_limits_int
{
public:
  Type_limits_uint8()
   :Type_limits_int(MAX_TINYINT_WIDTH, MAX_TINYINT_WIDTH)
  { }
};


class Type_limits_sint8: public Type_limits_int
{
public:
  Type_limits_sint8()
   :Type_limits_int(MAX_TINYINT_WIDTH, MAX_TINYINT_WIDTH + 1)
  { }
};


/*
  UNDIGNED SMALLINT:       0..65535  digits=5 nchars=5
  SIGNED SMALLINT:    -32768..32767  digits=5 nchars=6
*/
class Type_limits_uint16: public Type_limits_int
{
public:
  Type_limits_uint16()
   :Type_limits_int(MAX_SMALLINT_WIDTH, MAX_SMALLINT_WIDTH)
  { }
};


class Type_limits_sint16: public Type_limits_int
{
public:
  Type_limits_sint16()
   :Type_limits_int(MAX_SMALLINT_WIDTH, MAX_SMALLINT_WIDTH + 1)
  { }
};


/*
  MEDIUMINT UNSIGNED         0 .. 16777215  digits=8 char_length=8
  MEDIUMINT SIGNED:   -8388608 ..  8388607  digits=7 char_length=8
*/
class Type_limits_uint24: public Type_limits_int
{
public:
  Type_limits_uint24()
   :Type_limits_int(MAX_MEDIUMINT_WIDTH, MAX_MEDIUMINT_WIDTH)
  { }
};


class Type_limits_sint24: public Type_limits_int
{
public:
  Type_limits_sint24()
   :Type_limits_int(MAX_MEDIUMINT_WIDTH - 1, MAX_MEDIUMINT_WIDTH)
  { }
};


/*
  UNSIGNED INT:           0..4294967295  digits=10 nchars=10
  SIGNED INT:   -2147483648..2147483647  digits=10 nchars=11
*/
class Type_limits_uint32: public Type_limits_int
{
public:
  Type_limits_uint32()
   :Type_limits_int(MAX_INT_WIDTH, MAX_INT_WIDTH)
  { }
};



class Type_limits_sint32: public Type_limits_int
{
public:
  Type_limits_sint32()
   :Type_limits_int(MAX_INT_WIDTH, MAX_INT_WIDTH + 1)
  { }
};


/*
  UNSIGNED BIGINT:                  0..18446744073709551615 digits=20 nchars=20
  SIGNED BIGINT:  -9223372036854775808..9223372036854775807 digits=19 nchars=20
*/
class Type_limits_uint64: public Type_limits_int
{
public:
  Type_limits_uint64(): Type_limits_int(MAX_BIGINT_WIDTH, MAX_BIGINT_WIDTH)
  { }
};


class Type_limits_sint64: public Type_limits_int
{
public:
  Type_limits_sint64()
   :Type_limits_int(MAX_BIGINT_WIDTH - 1, MAX_BIGINT_WIDTH)
  { }
};



class Type_handler_int_result: public Type_handler_numeric
{
public:
  Item_result result_type() const override { return INT_RESULT; }
  Item_result cmp_type() const override { return INT_RESULT; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr) const override
  {
    return attr->unsigned_flag ? DYN_COL_UINT : DYN_COL_INT;
  }
  bool is_order_clause_position_type() const override { return true; }
  bool is_limit_clause_valid_type() const override { return true; }
  virtual ~Type_handler_int_result() {}
  const Type_handler *type_handler_for_comparison() const override;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const override;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate)
    const override;
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          Sort_param *param) const override;
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 Sort_param *param) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  void sort_length(THD *thd,
                   const Type_std_attributes *item,
                   SORT_FIELD_ATTR *attr) const override;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const override;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  void Item_update_null_value(Item *item) const override;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const override;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override;
  bool Item_val_bool(Item *item) const override;
  void Item_get_date(THD *thd, Item *item, Temporal::Warn *warn,
                     MYSQL_TIME *ltime,  date_mode_t fuzzydate) const override;
  longlong Item_val_int_signed_typecast(Item *item) const override;
  longlong Item_val_int_unsigned_typecast(Item *item) const override;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const override;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override;
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const override;
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const override;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const override;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const override;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const override;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const override;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const override;
  const Vers_type_handler *vers() const override { return &vers_type_trx; }
};


class Type_handler_general_purpose_int: public Type_handler_int_result
{
public:
  bool type_can_have_auto_increment_attribute() const override { return true; }
  virtual const Type_limits_int *type_limits_int() const= 0;
  uint32 max_display_length(const Item *item) const override
  {
    return type_limits_int()->char_length();
  }
  uint32 Item_decimal_notation_int_digits(const Item *item) const override;
  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    return partition_field_check_result_type(item_expr, INT_RESULT);
  }
  bool partition_field_append_value(String *str,
                                    Item *item_expr,
                                    CHARSET_INFO *field_cs,
                                    partition_value_print_mode_t)
                                    const override;
  const Vers_type_handler *vers() const override { return &vers_type_trx; }
};


class Type_handler_temporal_result: public Type_handler
{
protected:
  decimal_digits_t Item_decimal_scale_with_seconds(const Item *item) const;
  decimal_digits_t Item_divisor_precision_increment_with_seconds(const Item *) const;
public:
  Item_result result_type() const override { return STRING_RESULT; }
  Item_result cmp_type() const override { return TIME_RESULT; }
  virtual ~Type_handler_temporal_result() {}
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          Sort_param *param) const override;
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 Sort_param *param) const override;
  void sort_length(THD *thd,
                   const Type_std_attributes *item,
                   SORT_FIELD_ATTR *attr) const override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  uint32 max_display_length(const Item *item) const override;
  uint32 Item_decimal_notation_int_digits(const Item *item) const override;
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const)
    const override;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate)
    const override;
  bool Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems)
    const override;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *)const override;
  bool Item_val_bool(Item *item) const override;
  void Item_get_date(THD *thd, Item *item, Temporal::Warn *warn,
                     MYSQL_TIME *ltime,  date_mode_t fuzzydate) const override;
  longlong Item_val_int_signed_typecast(Item *item) const override;
  longlong Item_val_int_unsigned_typecast(Item *item) const override;
  String *Item_func_hex_val_str_ascii(Item_func_hex *, String *)const override;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override;
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *,
                                            date_mode_t) const override;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t) const override;
  bool Item_func_between_fix_length_and_dec(Item_func_between *)const override;
  bool Item_func_in_fix_comparator_compatible_types(THD *, Item_func_in *)
    const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const override;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const override;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const override;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const override;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const override;
  const Vers_type_handler *vers() const override;
};


class Type_handler_string_result: public Type_handler
{
  decimal_digits_t Item_temporal_precision(THD *thd, Item *item, bool is_time) const;
public:
  const Name &default_value() const override;
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_STRING;
  }
  Item_result result_type() const override { return STRING_RESULT; }
  Item_result cmp_type() const override { return STRING_RESULT; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *) const
    override
  {
    return DYN_COL_STRING;
  }
  CHARSET_INFO *charset_for_protocol(const Item *item) const override;
  virtual ~Type_handler_string_result() {}
  const Type_handler *type_handler_for_comparison() const override;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const
    override;
  const Type_handler *
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const override;
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          Sort_param *param) const override;
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 Sort_param *param) const override;
  void sort_length(THD *thd,
                   const Type_std_attributes *item,
                   SORT_FIELD_ATTR *attr) const override;
  bool is_packable() const override { return true; }
  bool union_element_finalize(Item_type_holder* item) const override;
  uint calc_key_length(const Column_definition &def) const override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  uint32 max_display_length(const Item *item) const override;
  /*
    The next method returns 309 for long stringified doubles in scientific
    notation, e.g. FORMAT('1e308', 2).
  */
  uint32 Item_decimal_notation_int_digits(const Item *item) const override
  { return 309; }
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const override;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  decimal_digits_t Item_time_precision(THD *thd, Item *item) const override
  {
    return Item_temporal_precision(thd, item, true);
  }
  decimal_digits_t Item_datetime_precision(THD *thd, Item *item) const override
  {
    return Item_temporal_precision(thd, item, false);
  }
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  void Item_update_null_value(Item *item) const override;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  void Item_param_setup_conversion(THD *thd, Item_param *) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_str(item, protocol, buf);
  }
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const
    override;
  String *print_item_value(THD *thd, Item *item, String *str) const override
  {
    return print_item_value_csstr(thd, item, str);
  }
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const
    override;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer,
                                            bool is_in_predicate)
    const override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const
    override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const
    override;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const override;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const override;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const override;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const override;
  bool Item_func_signed_fix_length_and_dec(Item_func_signed *item) const
    override;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const
    override;
  bool Item_val_bool(Item *item) const override;
  void Item_get_date(THD *thd, Item *item, Temporal::Warn *warn,
                     MYSQL_TIME *ltime,  date_mode_t fuzzydate) const override;
  longlong Item_val_int_signed_typecast(Item *item) const override;
  longlong Item_val_int_unsigned_typecast(Item *item) const override;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const
    override;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
    const override;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
    const override;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override;
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate)
    const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const
    override;
  double Item_func_min_max_val_real(Item_func_min_max *) const override;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const override;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate) const
    override;
  bool Item_func_between_fix_length_and_dec(Item_func_between *func) const
    override;
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const
    override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const
    override;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd, Item_func_in *)
    const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const override;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const override;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const override;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const override;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const override;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const override;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const override;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const override;
  const Vers_type_handler *vers() const override;
};


class Type_handler_general_purpose_string: public Type_handler_string_result
{
public:
  bool is_general_purpose_string_type() const override { return true; }
  bool Column_definition_bulk_alter(Column_definition *c,
                                    const Column_derived_attributes
                                          *derived_attr,
                                    const Column_bulk_alter_attributes
                                          *bulk_alter_attr)
                                    const override;
};


/***
  Instantiable classes for every MYSQL_TYPE_XXX

  There are no Type_handler_xxx for the following types:
  - MYSQL_TYPE_VAR_STRING (old VARCHAR) - mapped to MYSQL_TYPE_VARSTRING
  - MYSQL_TYPE_ENUM                     - mapped to MYSQL_TYPE_VARSTRING
  - MYSQL_TYPE_SET:                     - mapped to MYSQL_TYPE_VARSTRING

  because the functionality that currently uses Type_handler
  (e.g. hybrid type functions) does not need to distinguish between
  these types and VARCHAR.
  For example:
    CREATE TABLE t2 AS SELECT COALESCE(enum_column) FROM t1;
  creates a VARCHAR column.

  There most likely be Type_handler_enum and Type_handler_set later,
  when the Type_handler infrastructure gets used in more pieces of the code.
*/


class Type_handler_tiny: public Type_handler_general_purpose_int
{
public:
  virtual ~Type_handler_tiny() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_TINY; }
  const Type_handler *type_handler_unsigned() const override;
  const Type_handler *type_handler_signed() const override;
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_TINY;
  }
  const Type_limits_int *type_limits_int() const override;
  uint32 calc_pack_length(uint32 length) const override { return 1; }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 4; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_tiny(item, protocol, buf);
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_TINY); }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
};


class Type_handler_utiny: public Type_handler_tiny
{
public:
  uint flags() const override { return UNSIGNED_FLAG; }
  const Type_limits_int *type_limits_int() const override;
};


class Type_handler_short: public Type_handler_general_purpose_int
{
public:
  virtual ~Type_handler_short() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_SHORT; }
  const Type_handler *type_handler_unsigned() const override;
  const Type_handler *type_handler_signed() const override;
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_SHORT;
  }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_short(item, protocol, buf);
  }
  const Type_limits_int *type_limits_int() const override;
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 6; }
  uint32 calc_pack_length(uint32 length) const  override{ return 2; }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_SHORT); }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
};


class Type_handler_ushort: public Type_handler_short
{
public:
  uint flags() const override { return UNSIGNED_FLAG; }
  const Type_limits_int *type_limits_int() const override;
};


class Type_handler_long: public Type_handler_general_purpose_int
{
public:
  virtual ~Type_handler_long() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_LONG; }
  const Type_handler *type_handler_unsigned() const override;
  const Type_handler *type_handler_signed() const override;
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_LONG;
  }
  const Type_limits_int *type_limits_int() const override;
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 11; }
  uint32 calc_pack_length(uint32 length) const override { return 4; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_long(item, protocol, buf);
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_LONG); }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
};


class Type_handler_ulong: public Type_handler_long
{
public:
  uint flags() const override { return UNSIGNED_FLAG; }
  const Type_limits_int *type_limits_int() const override;
};


class Type_handler_bool: public Type_handler_long
{
public:
  bool is_bool_type() const override { return true; }
  const Type_handler *type_handler_unsigned() const override;
  const Type_handler *type_handler_signed() const override;
  void Item_update_null_value(Item *item) const override;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const override;
};


class Type_handler_longlong: public Type_handler_general_purpose_int
{
public:
  virtual ~Type_handler_longlong() {}
  enum_field_types field_type() const  override{ return MYSQL_TYPE_LONGLONG; }
  const Type_handler *type_handler_unsigned() const override;
  const Type_handler *type_handler_signed() const override;
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_LONGLONG;
  }
  const Type_limits_int *type_limits_int() const override;
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 20; }
  uint32 calc_pack_length(uint32 length) const override { return 8; }
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_longlong(item, protocol, buf);
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  {
    return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_LONGLONG);
  }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
};


class Type_handler_ulonglong: public Type_handler_longlong
{
public:
  uint flags() const override { return UNSIGNED_FLAG; }
  const Type_limits_int *type_limits_int() const override;
};


class Type_handler_vers_trx_id: public Type_handler_ulonglong
{
public:
  virtual ~Type_handler_vers_trx_id() {}
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
};


class Type_handler_int24: public Type_handler_general_purpose_int
{
public:
  virtual ~Type_handler_int24() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_INT24; }
  const Type_handler *type_handler_unsigned() const override;
  const Type_handler *type_handler_signed() const override;
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_LONG;
  }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_long(item, protocol, buf);
  }
  const Type_limits_int *type_limits_int() const override;
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 9; }
  uint32 calc_pack_length(uint32 length) const override { return 3; }
  Field *make_conversion_table_field(MEM_ROOT *mem_root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_INT24); }
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_uint24: public Type_handler_int24
{
public:
  uint flags() const override { return UNSIGNED_FLAG; }
  const Type_limits_int *type_limits_int() const override;
};


class Type_handler_year: public Type_handler_int_result
{
public:
  virtual ~Type_handler_year() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_YEAR; }
  uint flags() const override { return UNSIGNED_FLAG; }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_SHORT;
  }
  uint32 max_display_length(const Item *item) const override;
  uint32 Item_decimal_notation_int_digits(const Item *item) const override
  { return 4; };
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 4; }
  uint32 calc_pack_length(uint32 length) const override { return 1; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_short(item, protocol, buf);
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field)
                                              const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_YEAR); }
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *)const override;
  void Item_get_date(THD *thd, Item *item, Temporal::Warn *warn,
                     MYSQL_TIME *ltime,  date_mode_t fuzzydate) const override;
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *item,
                                            Temporal::Warn *,
                                            MYSQL_TIME *to,
                                            date_mode_t fuzzydate)
                                            const override;
  const Vers_type_handler *vers() const override { return NULL; }
};


class Type_handler_bit: public Type_handler_int_result
{
public:
  virtual ~Type_handler_bit() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_BIT; }
  uint flags() const override { return UNSIGNED_FLAG; }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_STRING;
  }
  uint32 max_display_length(const Item *item) const override;
  uint32 Item_decimal_notation_int_digits(const Item *item) const override;
  static uint32 Bit_decimal_notation_int_digits_by_nbits(uint nbits);
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override { return length / 8; }
  uint calc_key_length(const Column_definition &def) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_str(item, protocol, buf);
  }
  String *print_item_value(THD *thd, Item *item, String *str) const override
  {
    return print_item_value_csstr(thd, item, str);
  }
  void show_binlog_type(const Conv_source &src, const Field &, String *str)
    const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_float: public Type_handler_real_result
{
public:
  virtual ~Type_handler_float() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_FLOAT; }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_FLOAT;
  }
  bool type_can_have_auto_increment_attribute() const override { return true; }
  uint32 max_display_length(const Item *item) const override { return 25; }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 12; }
  uint32 Item_decimal_notation_int_digits(const Item *item) const override
  { return 39; }
  uint32 calc_pack_length(uint32 length) const override { return sizeof(float); }
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_float(item, protocol, buf);
  }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *)
                                            const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_real(c, MYSQL_TYPE_FLOAT); }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;

  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *)
                                    const override;
};


class Type_handler_double: public Type_handler_real_result
{
public:
  virtual ~Type_handler_double() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_DOUBLE; }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_DOUBLE;
  }
  bool type_can_have_auto_increment_attribute() const override { return true; }
  uint32 max_display_length(const Item *item) const override { return 53; }
  uint32 Item_decimal_notation_int_digits(const Item *item) const override
  { return 309; }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 22; }
  uint32 calc_pack_length(uint32 length) const override
  {
    return sizeof(double);
  }
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_double(item, protocol, buf);
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_real(c, MYSQL_TYPE_DOUBLE); }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;

  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *)
                                    const override;
};


class Type_handler_time_common: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_time_common() { }
  const Name &default_value() const override;
  enum_field_types field_type() const override { return MYSQL_TYPE_TIME; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_TIME;
  }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_TIME;
  }
  enum_mysql_timestamp_type mysql_timestamp_type() const override
  {
    return MYSQL_TIMESTAMP_TIME;
  }
  bool is_val_native_ready() const override { return true; }
  const Type_handler *type_handler_for_native_format() const override;
  int cmp_native(const Native &a, const Native &b) const override;
  bool Item_val_native_with_conversion(THD *thd, Item *, Native *to)
                                       const override;
  bool Item_val_native_with_conversion_result(THD *thd, Item *, Native *to)
                                              const override;
  bool Item_param_val_native(THD *thd, Item_param *item, Native *to)
                             const override;
  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    return partition_field_check_result_type(item_expr, STRING_RESULT);
  }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Item_literal *create_literal_item(THD *thd, const char *str, size_t length,
                                    CHARSET_INFO *cs, bool send_error)
                                    const override;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr)
                             const override;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  decimal_digits_t Item_decimal_scale(const Item *item) const override
  {
    return Item_decimal_scale_with_seconds(item);
  }
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  decimal_digits_t Item_divisor_precision_increment(const Item *item) const override
  {
    return Item_divisor_precision_increment_with_seconds(item);
  }
  const Type_handler *type_handler_for_comparison() const override;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item)
                               const override;
  void Column_definition_implicit_upgrade(Column_definition *c) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const override;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_time(item, protocol, buf);
  }
  void Item_update_null_value(Item *item) const override;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                         const override;
  String *print_item_value(THD *thd, Item *item, String *str) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  longlong Item_val_int_unsigned_typecast(Item *item) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
                                       const override;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const override;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const override;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const override;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const override;
  void Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            Temporal::Warn *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate)
                                            const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const override;
  double Item_func_min_max_val_real(Item_func_min_max *) const override;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const override;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate)
                                  const override;
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp)
                                       const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs)
                            const override;
  void Item_param_set_param_func(Item_param *param, uchar **pos, ulong len)
                                 const override;
};


class Type_handler_time: public Type_handler_time_common
{
  /* number of bytes to store TIME(N) */
  static uint m_hires_bytes[MAX_DATETIME_PRECISION+1];
public:
  static uint hires_bytes(uint dec) { return m_hires_bytes[dec]; }
  virtual ~Type_handler_time() {}
  const Name version() const override { return version_mariadb53(); }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return MIN_TIME_WIDTH; }
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_TIME); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_time2: public Type_handler_time_common
{
public:
  virtual ~Type_handler_time2() {}
  const Name version() const override { return version_mysql56(); }
  enum_field_types real_field_type() const override { return MYSQL_TYPE_TIME2; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_TIME2); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_temporal_with_date: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_temporal_with_date() {}
  Item_literal *create_literal_item(THD *thd, const char *str, size_t length,
                                    CHARSET_INFO *cs, bool send_error)
                                    const override;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item)
                               const override;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value)
                          const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_date(item, protocol, buf);
  }
  void Item_update_null_value(Item *item) const override;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                         const override;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp)
                                       const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs)
                            const override;
  longlong Item_func_between_val_int(Item_func_between *func) const override;
};


class Type_handler_date_common: public Type_handler_temporal_with_date
{
public:
  virtual ~Type_handler_date_common() {}
  const Name &default_value() const override;
  const Type_handler *type_handler_for_comparison() const override;
  enum_field_types field_type() const override { return MYSQL_TYPE_DATE; }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return 3; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_DATE;
  }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_DATE;
  }
  enum_mysql_timestamp_type mysql_timestamp_type() const override
  {
    return MYSQL_TIMESTAMP_DATE;
  }
  bool cond_notnull_field_isnull_to_field_eq_zero() const override
  {
    return true;
  }
  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    return partition_field_check_result_type(item_expr, STRING_RESULT);
  }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Item_literal *create_literal_item(THD *thd, const char *str, size_t length,
                                    CHARSET_INFO *cs, bool send_error)
                                    const override;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr)
                             const override;
  bool validate_implicit_default_value(THD *thd,
                                       const Column_definition &def)
                                       const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  String *print_item_value(THD *thd, Item *item, String *str) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const override;
  double Item_func_min_max_val_real(Item_func_min_max *) const override;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const
    override;
  bool Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const
    override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
};

class Type_handler_date: public Type_handler_date_common
{
public:
  virtual ~Type_handler_date() {}
  uint32 calc_pack_length(uint32 length) const override { return 4; }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_DATE); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_newdate: public Type_handler_date_common
{
public:
  virtual ~Type_handler_newdate() {}
  enum_field_types real_field_type() const override
  {
    return MYSQL_TYPE_NEWDATE;
  }
  uint32 calc_pack_length(uint32 length) const override { return 3; }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_NEWDATE); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_datetime_common: public Type_handler_temporal_with_date
{
public:
  virtual ~Type_handler_datetime_common() {}
  const Name &default_value() const override;
  const Type_handler *type_handler_for_comparison() const override;
  enum_field_types field_type() const override
  {
    return MYSQL_TYPE_DATETIME;
  }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_DATETIME;
  }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_DATETIME;
  }
  enum_mysql_timestamp_type mysql_timestamp_type() const override
  {
    return MYSQL_TIMESTAMP_DATETIME;
  }
  bool cond_notnull_field_isnull_to_field_eq_zero() const override
  {
    return true;
  }
  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    return partition_field_check_result_type(item_expr, STRING_RESULT);
  }
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
  bool validate_implicit_default_value(THD *thd, const Column_definition &def)
                                       const override;
  void Column_definition_implicit_upgrade(Column_definition *c) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const override;
  decimal_digits_t Item_decimal_scale(const Item *item) const override
  {
    return Item_decimal_scale_with_seconds(item);
  }
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  decimal_digits_t Item_divisor_precision_increment(const Item *item) const override
  {
    return Item_divisor_precision_increment_with_seconds(item);
  }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_datetime(item, protocol, buf);
  }
  String *print_item_value(THD *thd, Item *item, String *str) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const override;
  double Item_func_min_max_val_real(Item_func_min_max *) const override;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *, my_decimal *)
                                            const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
                                       const override;
  void Item_param_set_param_func(Item_param *param, uchar **pos, ulong len)
                                 const override;
};


class Type_handler_datetime: public Type_handler_datetime_common
{
  /* number of bytes to store DATETIME(N) */
  static uint m_hires_bytes[MAX_DATETIME_PRECISION + 1];
public:
  static uint hires_bytes(uint dec) { return m_hires_bytes[dec]; }
  virtual ~Type_handler_datetime() {}
  const Name version() const override { return version_mariadb53(); }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return MAX_DATETIME_WIDTH; }
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_DATETIME); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_datetime2: public Type_handler_datetime_common
{
public:
  virtual ~Type_handler_datetime2() {}
  const Name version() const override { return version_mysql56(); }
  enum_field_types real_field_type() const override
  {
    return MYSQL_TYPE_DATETIME2;
  }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_DATETIME2); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_timestamp_common: public Type_handler_temporal_with_date
{
protected:
  bool TIME_to_native(THD *, const MYSQL_TIME *from, Native *to, uint dec) const;
public:
  virtual ~Type_handler_timestamp_common() {}
  const Name &default_value() const override;
  const Type_handler *type_handler_for_comparison() const override;
  const Type_handler *type_handler_for_native_format() const override;
  enum_field_types field_type() const override { return MYSQL_TYPE_TIMESTAMP; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_DATETIME;
  }
  protocol_send_type_t protocol_send_type() const override
  {
    return PROTOCOL_SEND_DATETIME;
  }
  enum_mysql_timestamp_type mysql_timestamp_type() const override
  {
    return MYSQL_TIMESTAMP_DATETIME;
  }
  bool is_val_native_ready() const override
  {
    return true;
  }
  bool is_timestamp_type() const override
  {
    return true;
  }
  void Column_definition_implicit_upgrade(Column_definition *c) const override;
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options)
                                          const override;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override;
  bool Item_val_native_with_conversion(THD *thd, Item *, Native *to)
                                       const override;
  bool Item_val_native_with_conversion_result(THD *thd, Item *, Native *to)
                                              const override;
  bool Item_param_val_native(THD *thd, Item_param *item, Native *to)
                             const override;
  int cmp_native(const Native &a, const Native &b) const override;
  longlong Item_func_between_val_int(Item_func_between *func) const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const override;
  in_vector *make_in_vector(THD *thd, const Item_func_in *f, uint nargs)
                            const override;
  void make_sort_key_part(uchar *to, Item *item,
                          const SORT_FIELD_ATTR *sort_field,
                          Sort_param *param) const override;
  uint make_packed_sort_key_part(uchar *to, Item *item,
                                 const SORT_FIELD_ATTR *sort_field,
                                 Sort_param *param) const override;
  void sort_length(THD *thd,
                   const Type_std_attributes *item,
                   SORT_FIELD_ATTR *attr) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  decimal_digits_t Item_decimal_scale(const Item *item) const override
  {
    return Item_decimal_scale_with_seconds(item);
  }
  decimal_digits_t Item_decimal_precision(const Item *item) const override;
  decimal_digits_t Item_divisor_precision_increment(const Item *item) const override
  {
    return Item_divisor_precision_increment_with_seconds(item);
  }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return Item_send_timestamp(item, protocol, buf);
  }
  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                         const override;
  String *print_item_value(THD *thd, Item *item, String *str) const override;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const override;
  Item_copy *create_item_copy(THD *thd, Item *item) const override;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const override;
  double Item_func_min_max_val_real(Item_func_min_max *) const override;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const override;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const override;
  bool set_comparator_func(THD *thd, Arg_comparator *cmp) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
                                       const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate)
                                  const override;
  bool Column_definition_set_attributes(THD *thd,
                                        Column_definition *def,
                                        const Lex_field_type_st &attr,
                                        CHARSET_INFO *cs,
                                        column_definition_type_t type)
                                        const override;
};


class Type_handler_timestamp: public Type_handler_timestamp_common
{
  /* number of bytes to store second_part part of the TIMESTAMP(N) */
  static uint m_sec_part_bytes[MAX_DATETIME_PRECISION + 1];
public:
  static uint sec_part_bytes(uint dec) { return m_sec_part_bytes[dec]; }
  virtual ~Type_handler_timestamp() {}
  const Name version() const override { return version_mariadb53(); }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  { return MAX_DATETIME_WIDTH; }
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_TIMESTAMP); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_timestamp2: public Type_handler_timestamp_common
{
public:
  virtual ~Type_handler_timestamp2() {}
  const Name version() const override { return version_mysql56(); }
  enum_field_types real_field_type() const override
  {
    return MYSQL_TYPE_TIMESTAMP2;
  }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  {
    return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_TIMESTAMP2);
  }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_olddecimal: public Type_handler_decimal_result
{
public:
  virtual ~Type_handler_olddecimal() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_DECIMAL; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override { return length; }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const override;
  const Type_handler *type_handler_for_union(const Item *item) const override;
  void show_binlog_type(const Conv_source &src, const Field &, String *str)
    const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_DECIMAL); }
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_newdecimal: public Type_handler_decimal_result
{
public:
  virtual ~Type_handler_newdecimal() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_NEWDECIMAL; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  uint calc_key_length(const Column_definition &def) const override;
  void show_binlog_type(const Conv_source &src, const Field &, String *str)
    const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_null: public Type_handler_general_purpose_string
{
public:
  virtual ~Type_handler_null() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_NULL; }
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    return DYN_COL_NULL;
  }
  const Type_handler *type_handler_for_comparison() const override;
  const Type_handler *type_handler_for_tmp_table(const Item *item) const override;
  const Type_handler *type_handler_for_union(const Item *) const override;
  uint32 max_display_length(const Item *item) const override { return 0; }
  uint32 max_display_length_for_field(const Conv_source &src) const override
  {
    return 0;
  }
  uint32 calc_pack_length(uint32 length) const override { return 0; }
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const override;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const override;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool union_element_finalize(Item_type_holder* item) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_NULL); }
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


class Type_handler_longstr: public Type_handler_general_purpose_string
{
public:
  bool type_can_have_key_part() const override
  {
    return true;
  }
};


class Type_handler_string: public Type_handler_longstr
{
public:
  virtual ~Type_handler_string() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_STRING; }
  ulong KEY_pack_flags(uint column_nr) const override
  {
    return HA_PACK_KEY;
  }
  bool is_param_long_data_type() const override { return true; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override { return length; }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const override
  {
    return varstring_type_handler(item);
  }
  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    return partition_field_check_result_type(item_expr, STRING_RESULT);
  }
  void show_binlog_type(const Conv_source &src, const Field &dst, String *str)
    const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_set_attributes(THD *thd,
                                        Column_definition *def,
                                        const Lex_field_type_st &attr,
                                        CHARSET_INFO *cs,
                                        column_definition_type_t type)
                                        const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  bool Key_part_spec_init_ft(Key_part_spec *part,
                             const Column_definition &def) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


/* Old varchar */
class Type_handler_var_string: public Type_handler_string
{
public:
  virtual ~Type_handler_var_string() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_VAR_STRING; }
  enum_field_types real_field_type() const override { return MYSQL_TYPE_STRING; }
  enum_field_types traditional_merge_field_type() const override
  {
    return MYSQL_TYPE_VARCHAR;
  }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const override
  {
    return varstring_type_handler(item);
  }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  void show_binlog_type(const Conv_source &src, const Field &dst, String *str)
    const override;
  void Column_definition_implicit_upgrade(Column_definition *c) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_STRING); }
  const Type_handler *type_handler_for_union(const Item *item) const override
  {
    return varstring_type_handler(item);
  }
};


class Type_handler_varchar: public Type_handler_longstr
{
public:
  virtual ~Type_handler_varchar() {}
  enum_field_types field_type() const override { return MYSQL_TYPE_VARCHAR; }
  ulong KEY_pack_flags(uint column_nr) const override
  {
    if (column_nr == 0)
      return HA_BINARY_PACK_KEY | HA_VAR_LENGTH_KEY;
    return HA_PACK_KEY;
  }
  enum_field_types type_code_for_protocol() const override
  {
    return MYSQL_TYPE_VAR_STRING; // Keep things compatible for old clients
  }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override
  {
    return (length + (length < 256 ? 1: 2));
  }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const override
  {
    return varstring_type_handler(item);
  }
  const Type_handler *type_handler_for_union(const Item *item) const override
  {
    return varstring_type_handler(item);
  }
  bool is_param_long_data_type() const override { return true; }
  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    return partition_field_check_result_type(item_expr, STRING_RESULT);
  }
  void show_binlog_type(const Conv_source &src, const Field &dst, String *str)
    const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  bool Column_definition_set_attributes(THD *thd,
                                        Column_definition *def,
                                        const Lex_field_type_st &attr,
                                        CHARSET_INFO *cs,
                                        column_definition_type_t type)
                                        const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  bool Key_part_spec_init_ft(Key_part_spec *part,
                             const Column_definition &def) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  bool adjust_spparam_type(Spvar_definition *def, Item *from) const override;
};


class Type_handler_hex_hybrid: public Type_handler_varchar
{
public:
  virtual ~Type_handler_hex_hybrid() {}
  const Type_handler *cast_to_int_type_handler() const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
};


class Type_handler_varchar_compressed: public Type_handler_varchar
{
public:
  enum_field_types real_field_type() const override
  {
    return MYSQL_TYPE_VARCHAR_COMPRESSED;
  }
  ulong KEY_pack_flags(uint column_nr) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  bool partition_field_check(const LEX_CSTRING &field_name,
                             Item *item_expr) const override
  {
    partition_field_type_not_allowed(field_name);
    return true;
  }
  void show_binlog_type(const Conv_source &src, const Field &dst, String *str)
    const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *attr)
                                       const override
  {
    DBUG_ASSERT(0);
    return DYN_COL_STRING;
  }
};


class Type_handler_blob_common: public Type_handler_longstr
{
public:
  virtual ~Type_handler_blob_common() { }
  virtual uint length_bytes() const= 0;
  ulong KEY_pack_flags(uint column_nr) const override
  {
    if (column_nr == 0)
      return HA_BINARY_PACK_KEY | HA_VAR_LENGTH_KEY;
    return HA_PACK_KEY;
  }
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  const Type_handler *type_handler_for_tmp_table(const Item *item) const
    override
  {
    return blob_type_handler(item);
  }
  const Type_handler *type_handler_for_union(const Item *item) const override
  {
    return blob_type_handler(item);
  }
  bool subquery_type_allows_materialization(const Item *, const Item *, bool)
    const override
  {
    return false; // Materialization does not work with BLOB columns
  }
  bool is_param_long_data_type() const override { return true; }
  uint calc_key_length(const Column_definition &def) const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  bool Key_part_spec_init_ft(Key_part_spec *part,
                             const Column_definition &def) const override;
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
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const
    override;
  void Item_param_setup_conversion(THD *thd, Item_param *) const override;

  bool partition_field_check(const LEX_CSTRING &field_name,
                        Item *item_expr) const override;
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  const Vers_type_handler *vers() const override;
};


class Type_handler_tiny_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_tiny_blob() {}
  uint length_bytes() const override { return 1; }
  enum_field_types field_type() const override { return MYSQL_TYPE_TINY_BLOB; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  uint max_octet_length() const override { return UINT_MAX8; }
};


class Type_handler_medium_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_medium_blob() {}
  uint length_bytes() const override { return 3; }
  enum_field_types field_type() const override { return MYSQL_TYPE_MEDIUM_BLOB; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  uint max_octet_length() const override { return UINT_MAX24; }
};


class Type_handler_long_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_long_blob() {}
  uint length_bytes() const override { return 4; }
  enum_field_types field_type() const override { return MYSQL_TYPE_LONG_BLOB; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  uint max_octet_length() const override { return UINT_MAX32; }
};


class Type_handler_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_blob() {}
  uint length_bytes() const override { return 2; }
  enum_field_types field_type() const override { return MYSQL_TYPE_BLOB; }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  uint32 calc_pack_length(uint32 length) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  uint max_octet_length() const override { return UINT_MAX16; }
};


class Type_handler_blob_compressed: public Type_handler_blob
{
public:
  enum_field_types real_field_type() const override
  {
    return MYSQL_TYPE_BLOB_COMPRESSED;
  }
  ulong KEY_pack_flags(uint) const override
  {
    DBUG_ASSERT(0);
    return 0;
  }
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  void show_binlog_type(const Conv_source &src, const Field &, String *str)
    const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override;
  enum_dynamic_column_type dyncol_type(const Type_all_attributes *)
                                       const override
  {
    DBUG_ASSERT(0);
    return DYN_COL_STRING;
  }
};


class Type_handler_typelib: public Type_handler_general_purpose_string
{
public:
  virtual ~Type_handler_typelib() { }
  enum_field_types field_type() const override { return MYSQL_TYPE_STRING; }
  const Type_handler *type_handler_for_item_field() const override;
  const Type_handler *cast_to_int_type_handler() const override;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const override;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val*) const override;
  uint32 max_display_length_for_field(const Conv_source &src) const override;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems)
                                       const override;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field)
                                              const override;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const override;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file)
                                         const override;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override;
  const Vers_type_handler *vers() const override { return NULL; }
};


class Type_handler_enum: public Type_handler_typelib
{
public:
  virtual ~Type_handler_enum() {}
  enum_field_types real_field_type() const override { return MYSQL_TYPE_ENUM; }
  enum_field_types traditional_merge_field_type() const override
  {
    return MYSQL_TYPE_ENUM;
  }
  uint32 calc_pack_length(uint32 length) const override;
  uint calc_key_length(const Column_definition &def) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target)
                                     const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override;
};


class Type_handler_set: public Type_handler_typelib
{
public:
  virtual ~Type_handler_set() {}
  enum_field_types real_field_type() const override { return MYSQL_TYPE_SET; }
  enum_field_types traditional_merge_field_type() const override
  {
    return MYSQL_TYPE_SET;
  }
  uint32 calc_pack_length(uint32 length) const override;
  uint calc_key_length(const Column_definition &def) const override;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const override;
  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target)
                                     const override;
  bool Column_definition_fix_attributes(Column_definition *c) const override;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override;
  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override;
};


// A pseudo type handler, mostly for test purposes for now
class Type_handler_interval_DDhhmmssff: public Type_handler_long_blob
{
public:
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override;
};


class Function_collection
{
public:
  virtual ~Function_collection() {}
  virtual bool init()= 0;
  virtual void cleanup()= 0;
  virtual Create_func *find_native_function_builder(THD *thd,
                                                    const LEX_CSTRING &name)
                                                    const= 0;
};


class Type_collection
{
public:
  virtual ~Type_collection() {}
  virtual bool init(Type_handler_data *data)
  {
    return false;
  }
  virtual const Type_handler *handler_by_name(const LEX_CSTRING &name) const= 0;
  virtual const Type_handler *aggregate_for_result(const Type_handler *h1,
                                                   const Type_handler *h2)
                                                   const= 0;
  virtual const Type_handler *aggregate_for_comparison(const Type_handler *h1,
                                                       const Type_handler *h2)
                                                       const= 0;
  virtual const Type_handler *aggregate_for_min_max(const Type_handler *h1,
                                                    const Type_handler *h2)
                                                    const= 0;
  virtual const Type_handler *aggregate_for_num_op(const Type_handler *h1,
                                                   const Type_handler *h2)
                                                   const= 0;
};


/**
  A handler for hybrid type functions, e.g.
  COALESCE(), IF(), IFNULL(), NULLIF(), CASE,
  numeric operators,
  UNIX_TIMESTAMP(), TIME_TO_SEC().

  Makes sure that field_type(), cmp_type() and result_type()
  are always in sync to each other for hybrid functions.
*/
class Type_handler_hybrid_field_type
{
  const Type_handler *m_type_handler;
  bool aggregate_for_min_max(const Type_handler *other);

public:
  Type_handler_hybrid_field_type();
  Type_handler_hybrid_field_type(const Type_handler *handler)
   :m_type_handler(handler)
  { }
  Type_handler_hybrid_field_type(const Type_handler_hybrid_field_type *other)
    :m_type_handler(other->m_type_handler)
  { }
  void swap(Type_handler_hybrid_field_type &other)
  {
    swap_variables(const Type_handler *, m_type_handler, other.m_type_handler);
  }
  const Type_handler *type_handler() const { return m_type_handler; }
  enum_field_types real_field_type() const
  {
    return m_type_handler->real_field_type();
  }
  Item_result cmp_type() const { return m_type_handler->cmp_type(); }
  enum_mysql_timestamp_type mysql_timestamp_type() const
  {
    return m_type_handler->mysql_timestamp_type();
  }
  bool is_timestamp_type() const
  {
    return m_type_handler->is_timestamp_type();
  }
  void set_handler(const Type_handler *other)
  {
    m_type_handler= other;
  }
  const Type_handler *set_handler_by_field_type(enum_field_types type)
  {
    return (m_type_handler= Type_handler::get_handler_by_field_type(type));
  }
  const Type_handler *set_handler_by_real_type(enum_field_types type)
  {
    return (m_type_handler= Type_handler::get_handler_by_real_type(type));
  }
  bool aggregate_for_comparison(const Type_handler *other);
  bool aggregate_for_comparison(const LEX_CSTRING &funcname,
                                Item **items, uint nitems,
                                bool treat_int_to_uint_as_decimal);
  bool aggregate_for_result(const Type_handler *other);
  bool aggregate_for_result(const LEX_CSTRING &funcname,
                            Item **item, uint nitems, bool treat_bit_as_number);
  bool aggregate_for_min_max(const LEX_CSTRING &funcname, Item **item,
                             uint nitems);

  bool aggregate_for_num_op(const class Type_aggregator *aggregator,
                            const Type_handler *h0, const Type_handler *h1);
};

/*
  Helper template to simplify creating builtin types with names.
  Plugin types inherit from Type_handler_xxx types that do not set the name in
  the constructor, as sql_plugin.cc sets the type name from the plugin name.
*/
template <typename TypeHandler>
class Named_type_handler : public TypeHandler
{
  public:
  Named_type_handler(const char *n) : TypeHandler()
  { Type_handler::set_name(Name(n, static_cast<uint>(strlen(n)))); }
};

extern Named_type_handler<Type_handler_row>         type_handler_row;
extern Named_type_handler<Type_handler_null>        type_handler_null;

extern Named_type_handler<Type_handler_float>       type_handler_float;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_double>      type_handler_double;

extern Named_type_handler<Type_handler_bit>         type_handler_bit;

extern Named_type_handler<Type_handler_enum>        type_handler_enum;
extern Named_type_handler<Type_handler_set>         type_handler_set;

extern Named_type_handler<Type_handler_string>      type_handler_string;
extern Named_type_handler<Type_handler_var_string>  type_handler_var_string;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_varchar>     type_handler_varchar;
extern Named_type_handler<Type_handler_varchar_compressed> type_handler_varchar_compressed;
extern Named_type_handler<Type_handler_hex_hybrid>  type_handler_hex_hybrid;

extern Named_type_handler<Type_handler_tiny_blob>   type_handler_tiny_blob;
extern Named_type_handler<Type_handler_medium_blob> type_handler_medium_blob;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_long_blob>   type_handler_long_blob;
extern Named_type_handler<Type_handler_blob>        type_handler_blob;
extern Named_type_handler<Type_handler_blob_compressed> type_handler_blob_compressed;

extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_bool>        type_handler_bool;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_tiny>        type_handler_stiny;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_short>       type_handler_sshort;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_int24>       type_handler_sint24;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_long>        type_handler_slong;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_longlong>    type_handler_slonglong;

extern Named_type_handler<Type_handler_utiny>       type_handler_utiny;
extern Named_type_handler<Type_handler_ushort>      type_handler_ushort;
extern Named_type_handler<Type_handler_uint24>      type_handler_uint24;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_ulong>       type_handler_ulong;
extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_ulonglong>   type_handler_ulonglong;
extern Named_type_handler<Type_handler_vers_trx_id> type_handler_vers_trx_id;

extern MYSQL_PLUGIN_IMPORT Named_type_handler<Type_handler_newdecimal>  type_handler_newdecimal;
extern Named_type_handler<Type_handler_olddecimal>  type_handler_olddecimal;

extern Named_type_handler<Type_handler_year>        type_handler_year;
extern Named_type_handler<Type_handler_year>        type_handler_year2;
extern Named_type_handler<Type_handler_newdate>     type_handler_newdate;
extern Named_type_handler<Type_handler_date>        type_handler_date;
extern Named_type_handler<Type_handler_time>        type_handler_time;
extern Named_type_handler<Type_handler_time2>       type_handler_time2;
extern Named_type_handler<Type_handler_datetime>    type_handler_datetime;
extern Named_type_handler<Type_handler_datetime2>   type_handler_datetime2;
extern Named_type_handler<Type_handler_timestamp>   type_handler_timestamp;
extern Named_type_handler<Type_handler_timestamp2>  type_handler_timestamp2;

extern Type_handler_interval_DDhhmmssff type_handler_interval_DDhhmmssff;

class Type_aggregator
{
  bool m_is_commutative;
public:
  class Pair
  {
  public:
    const Type_handler *m_handler1;
    const Type_handler *m_handler2;
    const Type_handler *m_result;
    Pair() { }
    Pair(const Type_handler *handler1,
         const Type_handler *handler2,
         const Type_handler *result)
     :m_handler1(handler1), m_handler2(handler2), m_result(result)
    { }
    bool eq(const Type_handler *handler1, const Type_handler *handler2) const
    {
      return m_handler1 == handler1 && m_handler2 == handler2;
    }
  };
  static const Type_handler *
    find_handler_in_array(const Type_aggregator::Pair *pairs,
                          const Type_handler *h1,
                          const Type_handler *h2,
                          bool commutative)
  {
    for (const Type_aggregator::Pair *p= pairs; p->m_result; p++)
    {
      if (p->eq(h1, h2))
        return p->m_result;
      if (commutative && p->eq(h2, h1))
       return p->m_result;
    }
    return NULL;
  }

private:
  Dynamic_array<Pair> m_array;
  const Pair* find_pair(const Type_handler *handler1,
                        const Type_handler *handler2) const;
public:
  Type_aggregator(bool is_commutative= false)
   :m_is_commutative(is_commutative), m_array(PSI_INSTRUMENT_MEM)
  { }
  bool add(const Type_handler *handler1,
           const Type_handler *handler2,
           const Type_handler *result)
  {
    return m_array.append(Pair(handler1, handler2, result));
  }
  const Type_handler *find_handler(const Type_handler *handler1,
                                   const Type_handler *handler2) const
  {
    const Pair* el= find_pair(handler1, handler2);
    return el ? el->m_result : NULL;
  }
  bool is_commutative() const { return m_is_commutative; }
};


class Type_aggregator_commutative: public Type_aggregator
{
public:
  Type_aggregator_commutative()
   :Type_aggregator(true)
  { }
};


class Type_handler_data
{
public:
  Type_aggregator_commutative m_type_aggregator_for_result;
  Type_aggregator_commutative m_type_aggregator_for_comparison;

  Type_aggregator_commutative m_type_aggregator_for_plus;
  Type_aggregator_commutative m_type_aggregator_for_mul;

  Type_aggregator m_type_aggregator_for_minus;
  Type_aggregator m_type_aggregator_for_div;
  Type_aggregator m_type_aggregator_for_mod;
#ifndef DBUG_OFF
  // This is used for mtr purposes in debug builds
  Type_aggregator m_type_aggregator_non_commutative_test;
#endif
  bool init();
};

extern Type_handler_data *type_handler_data;

#endif /* SQL_TYPE_H_INCLUDED */
