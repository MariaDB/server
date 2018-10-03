#ifndef SQL_TYPE_H_INCLUDED
#define SQL_TYPE_H_INCLUDED
/*
   Copyright (c) 2015  MariaDB Foundation.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


#include "mysqld.h"
#include "sql_array.h"
#include "sql_const.h"
#include "sql_time.h"

class Field;
class Column_definition;
class Column_definition_attributes;
class Item;
class Item_const;
class Item_literal;
class Item_param;
class Item_cache;
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
class cmp_item;
class in_vector;
class Type_handler_hybrid_field_type;
class Sort_param;
class Arg_comparator;
class Spvar_definition;
struct st_value;
class Protocol;
class handler;
struct Schema_specification_st;
struct TABLE;
struct SORT_FIELD_ATTR;
class Vers_history_point;

#define my_charset_numeric      my_charset_latin1

enum scalar_comparison_op
{
  SCALAR_CMP_EQ,
  SCALAR_CMP_EQUAL,
  SCALAR_CMP_LT,
  SCALAR_CMP_LE,
  SCALAR_CMP_GE,
  SCALAR_CMP_GT
};


class Dec_ptr
{
protected:
  my_decimal *m_ptr;
  Dec_ptr() { }
public:
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
  bool to_bool() const { return m_ptr ? m_ptr->to_bool() : false; }
  bool to_datetime_with_warn(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate,
                             const char *field_name)
  {
    return m_ptr ? m_ptr->to_datetime_with_warn(thd, to, fuzzydate, field_name) :
                   true;
  }
  bool to_datetime_with_warn(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate,
                             Item *item);
  String *to_string(String *to) const
  {
    return m_ptr ? m_ptr->to_string(to) : NULL;
  }
  String *to_string(String *to, uint prec, uint dec, char filler)
  {
    return m_ptr ? m_ptr->to_string(to, prec, dec, filler) : NULL;
  }
  int to_binary(uchar *bin, int prec, int scale) const
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
  int round_to(my_decimal *to, uint scale, decimal_round_mode mode)
  {
    DBUG_ASSERT(m_ptr);
    return m_ptr->round_to(to, scale, mode);
  }
  int round_self(uint scale, decimal_round_mode mode)
  {
    return round_to(&m_buffer, scale, mode);
  }
  String *to_string_round(String *to, uint dec)
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
  void make_from_decimal(const my_decimal *d);
  void make_from_double(double d);
  void make_from_int(longlong nr, bool unsigned_val)
  {
    m_neg= nr < 0 && !unsigned_val;
    m_sec= m_neg ? (ulonglong) -nr : (ulonglong) nr;
    m_usec= 0;
    m_truncated= false;
  }
  void reset()
  {
    m_sec= m_usec= m_neg= m_truncated= 0;
  }
  Sec6() { }
public:
  Sec6(bool neg, ulonglong nr, ulong frac)
   :m_sec(nr), m_usec(frac), m_neg(neg), m_truncated(false)
  { }
  Sec6(double nr)
  {
    make_from_double(nr);
  }
  Sec6(const my_decimal *d)
  {
    make_from_decimal(d);
  }
  Sec6(longlong nr, bool unsigned_val)
  {
    make_from_int(nr, unsigned_val);
  }
  bool neg() const { return m_neg; }
  bool truncated() const { return m_truncated; }
  ulonglong sec() const { return m_sec; }
  long usec() const { return m_usec; }
  /**
    Converts Sec6 to MYSQL_TIME

    @param ltime         converted value will be written here
    @param fuzzydate     conversion flags (TIME_INVALID_DATE, etc)
    @param str           original number, as an ErrConv. For the warning
    @param field_name    field name or NULL if not a field. For the warning
    @returns false for success, true for a failure
  */
  bool convert_to_mysql_time(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate,
                             const ErrConv *str, const char *field_name) const;

  // Convert a number in format hhhmmss.ff to TIME'hhh:mm:ss.ff'
  bool to_time(MYSQL_TIME *to, int *warn) const
  {
    return number_to_time(m_neg, m_sec, m_usec, to, warn);
  }
  /*
    Convert a number in format YYYYMMDDhhmmss.ff to
    TIMESTAMP'YYYY-MM-DD hh:mm:ss.ff'
  */
  bool to_datetime(MYSQL_TIME *to, date_mode_t flags, int *warn) const
  {
    if (m_neg)
    {
      *warn= MYSQL_TIME_WARN_OUT_OF_RANGE;
      return true;
    }
    return number_to_datetime(m_sec, m_usec, to,
                              ulonglong(flags & TIME_MODE_FOR_XXX_TO_DATE),
                              warn) == -1;
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
  size_t to_string(char *to, size_t nbytes) const
  {
    return m_usec ?
      my_snprintf(to, nbytes, "%s%llu.%06lu",
                  m_neg ? "-" : "", m_sec, (uint) m_usec) :
      my_snprintf(to, nbytes, "%s%llu", m_neg ? "-" : "", m_sec);
  }
  void make_truncated_warning(THD *thd, const char *type_str) const;
};


class VSec6: public Sec6
{
  bool m_is_null;
public:
  VSec6(THD *thd, Item *item, const char *type_str, ulonglong limit);
  bool is_null() const { return m_is_null; }
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
    ltime->hour+= to_days_abs() * 24;
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
  bool to_mysql_time_with_warn(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate,
                               const char *field_name) const
  {
    longlong value= m_year * 10000; // Make it YYYYMMDD
    const ErrConvInteger str(value, true);
    Sec6 sec(false, value, 0);
    return sec.convert_to_mysql_time(thd, to, fuzzydate, &str, field_name);
  }
  uint year_precision(const Item *item) const;
public:
  Year(): m_year(0), m_truncated(false) { }
  Year(longlong value, bool unsigned_flag, uint length);
  uint year() const { return m_year; }
  bool truncated() const { return m_truncated; }
};


class Year_null: public Year
{
protected:
  bool m_is_null;
public:
  Year_null(const Year &other, bool is_null)
   :Year(is_null ? Year() : other),
    m_is_null(is_null)
  { }
  bool is_null() const { return m_is_null; }
  bool to_mysql_time_with_warn(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate,
                               const char *field_name) const
  {
    return m_is_null ? true :
           Year::to_mysql_time_with_warn(thd, to, fuzzydate, field_name);
  }
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


class Temporal: protected MYSQL_TIME
{
public:
  bool is_valid_temporal() const
  {
    DBUG_ASSERT(time_type != MYSQL_TIMESTAMP_ERROR);
    return time_type != MYSQL_TIMESTAMP_NONE;
  }
protected:
  my_decimal *bad_to_decimal(my_decimal *to) const;
  my_decimal *to_decimal(my_decimal *to) const;
  static double to_double(bool negate, ulonglong num, ulong frac)
  {
    double d= (double) num + frac / (double) TIME_SECOND_PART_FACTOR;
    return negate ? -d : d;
  }
  longlong to_packed() const { return ::pack_time(this); }
  void make_from_out_of_range(int *warn)
  {
    *warn= MYSQL_TIME_WARN_OUT_OF_RANGE;
    time_type= MYSQL_TIMESTAMP_NONE;
  }
  bool str_to_time(MYSQL_TIME_STATUS *st, const char *str, size_t length,
                   CHARSET_INFO *cs, date_mode_t fuzzydate);
  bool str_to_datetime(MYSQL_TIME_STATUS *st, const char *str, size_t length,
                       CHARSET_INFO *cs, date_mode_t fuzzydate);
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
  Temporal_hybrid(THD *thd, Item *item);
  Temporal_hybrid(Item *item): Temporal_hybrid(current_thd, item) { }
  Temporal_hybrid(MYSQL_TIME_STATUS *st, const char *str, size_t length,
                  CHARSET_INFO *cs, date_mode_t fuzzydate)
  {
    if (str_to_datetime(st, str, length, cs, fuzzydate))
      time_type= MYSQL_TIMESTAMP_NONE;
  }
  Temporal_hybrid(THD *thd, const Sec6 &sec, date_mode_t fuzzydate,
                  const ErrConv *str, const char *field_name)
  {
    if (sec.convert_to_mysql_time(thd, this, fuzzydate, str, field_name))
      time_type= MYSQL_TIMESTAMP_NONE;
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
public:
  enum datetime_to_time_mode_t
  {
    DATETIME_TO_TIME_DISALLOW,
    DATETIME_TO_TIME_YYYYMMDD_000000DD_MIX_TO_HOURS,
    DATETIME_TO_TIME_YYYYMMDD_TRUNCATE,
    DATETIME_TO_TIME_YYYYMMDD_00000000_ONLY,
    DATETIME_TO_TIME_MINUS_CURRENT_DATE
  };
  class Options
  {
    date_mode_t             m_get_date_flags;
    datetime_to_time_mode_t m_datetime_to_time_mode;
  public:
    Options()
     :m_get_date_flags(flags_for_get_date()),
      m_datetime_to_time_mode(DATETIME_TO_TIME_YYYYMMDD_000000DD_MIX_TO_HOURS)
    { }
    Options(date_mode_t flags)
     :m_get_date_flags(flags),
      m_datetime_to_time_mode(DATETIME_TO_TIME_YYYYMMDD_000000DD_MIX_TO_HOURS)
    { }
    Options(date_mode_t flags, datetime_to_time_mode_t dtmode)
     :m_get_date_flags(flags),
      m_datetime_to_time_mode(dtmode)
    { }
    date_mode_t get_date_flags() const
    { return m_get_date_flags; }
    datetime_to_time_mode_t datetime_to_time_mode() const
    { return m_datetime_to_time_mode; }
  };
  /*
    CAST(AS TIME) historically does not mix days to hours.
    This is different comparing to how implicit conversion
    in Field::store_time_dec() works (e.g. on INSERT).
  */
  class Options_for_cast: public Options
  {
  public:
    Options_for_cast()
     :Options(flags_for_get_date(), DATETIME_TO_TIME_YYYYMMDD_TRUNCATE)
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
           year == 0 && month == 0 && day == 0 &&
           minute <= TIME_MAX_MINUTE &&
           second <= TIME_MAX_SECOND &&
           second_part <= TIME_MAX_SECOND_PART;
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
    e.g. returned from Item::get_date(), str_to_time(), number_to_time().
    After this call, "this" is a valid TIME value.
  */
  void valid_datetime_to_valid_time(THD *thd, int *warn, const Options opt)
  {
    DBUG_ASSERT(time_type == MYSQL_TIMESTAMP_DATE ||
                time_type == MYSQL_TIMESTAMP_DATETIME);
    /*
      We're dealing with a DATE or DATETIME returned from
      str_to_time(), number_to_time() or unpack_time().
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
    str_to_time(), number_to_time().
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
    This method is called after number_to_time() and str_to_time(),
    which can return DATE or DATETIME values. Convert to TIME if needed.
    We trust that xxx_to_time() returns a valid TIME/DATE/DATETIME value,
    so here we need to do only simple validation.
  */
  void xxx_to_time_result_to_valid_value(THD *thd, int *warn, const Options opt)
  {
    // str_to_time(), number_to_time() never return MYSQL_TIMESTAMP_ERROR
    DBUG_ASSERT(time_type != MYSQL_TIMESTAMP_ERROR);
    valid_MYSQL_TIME_to_valid_value(thd, warn, opt);
  }
  void adjust_time_range_or_invalidate(int *warn)
  {
    if (check_time_range(this, TIME_SECOND_PART_DIGITS, warn))
      time_type= MYSQL_TIMESTAMP_NONE;
    DBUG_ASSERT(is_valid_value_slow());
  }
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
  Time() { time_type= MYSQL_TIMESTAMP_NONE; }
  Time(Item *item)
   :Time(current_thd, item, Options())
  { }
  Time(THD *thd, Item *item, const Options opt)
  {
    int warn;
    make_from_item(thd, &warn, item, opt);
  }
  Time(THD *thd, Item *item)
   :Time(thd, item, Options())
  { }
  Time(int *warn, const MYSQL_TIME *from, long curdays);
  Time(THD *thd, MYSQL_TIME_STATUS *status,
       const char *str, size_t len, CHARSET_INFO *cs,
       const Options opt)
  {
    if (str_to_time(status, str, len, cs, opt.get_date_flags()))
      time_type= MYSQL_TIMESTAMP_NONE;
    // The below call will optionally add notes to already collected warnings:
    xxx_to_time_result_to_valid_value(thd, &status->warnings, opt);
  }
  Time(THD *thd, int *warn, const Sec6 &nr, const Options opt)
  {
    if (nr.to_time(this, warn))
      time_type= MYSQL_TIMESTAMP_NONE;
    xxx_to_time_result_to_valid_value(thd, warn, opt);
  }
  Time(THD *thd, int *warn, double nr)
   :Time(thd, warn, Sec6(nr), Options())
  { }
  Time(THD *thd, int *warn, longlong nr, bool unsigned_val)
   :Time(thd, warn, Sec6(nr, unsigned_val), Options())
  { }
  Time(THD *thd, int *warn, const my_decimal *d)
   :Time(thd, warn, Sec6(d), Options())
  { }

  Time(THD *thd, Item *item, const Options opt, uint dec)
   :Time(thd, item, opt)
  {
    trunc(dec);
  }
  Time(int *warn, const MYSQL_TIME *from, long curdays, uint dec)
   :Time(warn, from, curdays)
  {
    trunc(dec);
  }
  Time(THD *thd, MYSQL_TIME_STATUS *status,
       const char *str, size_t len, CHARSET_INFO *cs,
       const Options &opt, uint dec)
   :Time(thd, status, str, len, cs, opt)
  {
    trunc(dec);
  }
  Time(THD *thd, int *warn, double nr, uint dec)
   :Time(thd, warn, nr)
  {
    trunc(dec);
  }
  Time(THD *thd, int *warn, longlong nr, bool unsigned_val, uint dec)
   :Time(thd, warn, nr, unsigned_val)
  {
    /*
      Decimal digit truncation is needed here in case if nr was out
      of the supported TIME range, so "this" was set to '838:59:59.999999'.
    */
    trunc(dec);
  }
  Time(THD *thd, int *warn, const my_decimal *d, uint dec)
   :Time(thd, warn, d)
  {
    trunc(dec);
  }

  static date_mode_t flags_for_get_date()
  { return TIME_TIME_ONLY | TIME_INVALID_DATES; }
  static date_mode_t comparison_flags_for_get_date()
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
  Time &trunc(uint dec)
  {
    if (is_valid_time())
      my_time_trunc(this, dec);
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
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
protected:
  void check_date_or_invalidate(int *warn, date_mode_t flags);
  void make_from_item(THD *thd, Item *item, date_mode_t flags);
  void make_from_item(THD *thd, Item *item);

  ulong daynr() const
  {
    return (ulong) ::calc_daynr((uint) year, (uint) month, (uint) day);
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

  Temporal_with_date()
  {
    time_type= MYSQL_TIMESTAMP_NONE;
  }
  Temporal_with_date(THD *thd, Item *item, date_mode_t flags)
  {
    make_from_item(thd, item, flags);
  }
  Temporal_with_date(THD *thd, Item *item)
  {
    make_from_item(thd, item);
  }
  Temporal_with_date(int *warn, const Sec6 &nr, date_mode_t flags)
  {
    DBUG_ASSERT(bool(flags & TIME_TIME_ONLY) == false);
    if (nr.to_datetime(this, flags, warn))
      time_type= MYSQL_TIMESTAMP_NONE;
  }
  Temporal_with_date(MYSQL_TIME_STATUS *status,
                     const char *str, size_t len, CHARSET_INFO *cs,
                     date_mode_t flags)
  {
    DBUG_ASSERT(bool(flags & TIME_TIME_ONLY) == false);
    if (str_to_datetime(status, str, len, cs, flags))
      time_type= MYSQL_TIMESTAMP_NONE;
  }
public:
  bool check_date_with_warn(THD *thd, date_mode_t flags)
  {
    return ::check_date_with_warn(thd, this, flags, MYSQL_TIMESTAMP_ERROR);
  }
  static date_mode_t comparison_flags_for_get_date()
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
  Date(THD *thd, Item *item, date_mode_t flags)
   :Temporal_with_date(thd, item, flags)
  {
    if (time_type == MYSQL_TIMESTAMP_DATETIME)
      datetime_to_date(this);
    DBUG_ASSERT(is_valid_value_slow());
  }
  Date(THD *thd, Item *item)
   :Temporal_with_date(thd, item)
  {
    if (time_type == MYSQL_TIMESTAMP_DATETIME)
      datetime_to_date(this);
    DBUG_ASSERT(is_valid_value_slow());
  }
  Date(Item *item)
   :Date(current_thd, item)
  { }
  Date(const Temporal_with_date *d)
   :Temporal_with_date(*d)
  {
    datetime_to_date(this);
    DBUG_ASSERT(is_valid_date_slow());
  }
  bool is_valid_date() const
  {
    DBUG_ASSERT(is_valid_value_slow());
    return time_type == MYSQL_TIMESTAMP_DATE;
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
  void date_to_datetime_if_needed()
  {
    if (time_type == MYSQL_TIMESTAMP_DATE)
      date_to_datetime(this);
  }
  void make_from_time(THD *thd, int *warn, const MYSQL_TIME *from,
                      date_mode_t flags);
  void make_from_datetime(THD *thd, int *warn, const MYSQL_TIME *from,
                          date_mode_t flags);
public:
  Datetime(THD *thd, Item *item, date_mode_t flags)
   :Temporal_with_date(thd, item, flags)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }
  Datetime(THD *thd, Item *item)
   :Temporal_with_date(thd, item)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }
  Datetime(Item *item)
   :Datetime(current_thd, item)
  { }
  Datetime(THD *thd, int *warn, const MYSQL_TIME *from, date_mode_t flags);
  Datetime()
  {
    set_zero_time(this, MYSQL_TIMESTAMP_DATETIME);
  }
  Datetime(MYSQL_TIME_STATUS *status,
           const char *str, size_t len, CHARSET_INFO *cs,
           date_mode_t flags)
   :Temporal_with_date(status, str, len, cs, flags)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }
  Datetime(int *warn, double nr, date_mode_t flags)
   :Temporal_with_date(warn, Sec6(nr), flags)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }
  Datetime(int *warn, const my_decimal *d, date_mode_t flags)
   :Temporal_with_date(warn, Sec6(d), flags)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }
  /*
    Create a Datime object from a longlong number.
    Note, unlike in Time(), we don't need an "unsigned_val" here,
    as it's not important if overflow happened because
    of a negative number, or because of a huge positive number.
  */
  Datetime(int *warn, longlong sec, ulong usec, date_mode_t flags)
   :Temporal_with_date(warn, Sec6(false, (ulonglong) sec, usec), flags)
  {
    date_to_datetime_if_needed();
    DBUG_ASSERT(is_valid_value_slow());
  }

  Datetime(THD *thd, Item *item, date_mode_t flags, uint dec)
   :Temporal_with_date(Datetime(thd, item, flags))
  {
    trunc(dec);
  }
  Datetime(MYSQL_TIME_STATUS *status,
           const char *str, size_t len, CHARSET_INFO *cs,
           date_mode_t fuzzydate, uint dec)
   :Temporal_with_date(Datetime(status, str, len, cs, fuzzydate))
  {
    trunc(dec);
  }
  Datetime(int *warn, double nr, date_mode_t fuzzydate, uint dec)
   :Temporal_with_date(Datetime(warn, nr, fuzzydate))
  {
    trunc(dec);
  }
  Datetime(int *warn, const my_decimal *d, date_mode_t fuzzydate, uint dec)
   :Temporal_with_date(Datetime(warn, d, fuzzydate))
  {
    trunc(dec);
  }
  Datetime(THD *thd, int *warn, const MYSQL_TIME *from,
           date_mode_t fuzzydate, uint dec)
   :Temporal_with_date(Datetime(thd, warn, from, fuzzydate))
  {
    trunc(dec);
  }

  bool is_valid_datetime() const
  {
    /*
      Here we quickly check for the type only.
      If the type is valid, the rest of value must also be valid.
    */
    DBUG_ASSERT(is_valid_value_slow());
    return time_type == MYSQL_TIMESTAMP_DATETIME;
  }
  bool check_date(date_mode_t flags, int *warnings) const
  {
    DBUG_ASSERT(is_valid_datetime_slow());
    return ::check_date(this, (year || month || day),
                        ulonglong(flags & TIME_MODE_FOR_XXX_TO_DATE),
                        warnings);
  }
  bool check_date(date_mode_t flags) const
  {
    int dummy; /* unused */
    return check_date(flags, &dummy);
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
  Datetime &trunc(uint dec)
  {
    if (is_valid_datetime())
      my_time_trunc(this, dec);
    DBUG_ASSERT(is_valid_value_slow());
    return *this;
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
  uint repertoire;

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
              uint repertoire_arg)
   :collation(collation_arg),
    derivation(derivation_arg),
    repertoire(repertoire_arg)
  { }
  void set(const DTCollation &dt)
  {
    collation= dt.collation;
    derivation= dt.derivation;
    repertoire= dt.repertoire;
  }
  void set(CHARSET_INFO *collation_arg, Derivation derivation_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  void set(CHARSET_INFO *collation_arg,
           Derivation derivation_arg,
           uint repertoire_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    repertoire= repertoire_arg;
  }
  void set_numeric()
  {
    collation= &my_charset_numeric;
    derivation= DERIVATION_NUMERIC;
    repertoire= MY_REPERTOIRE_NUMERIC;
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
  int sortcmp(const String *s, const String *t) const
  {
    return collation->coll->strnncollsp(collation,
                                        (uchar *) s->ptr(), s->length(),
                                        (uchar *) t->ptr(), t->length());
  }
};


static inline uint32
char_to_byte_length_safe(size_t char_length_arg, uint32 mbmaxlen_arg)
{
  ulonglong tmp= ((ulonglong) char_length_arg) * mbmaxlen_arg;
  return tmp > UINT_MAX32 ? (uint32) UINT_MAX32 : static_cast<uint32>(tmp);
}

/**
  A class to store type attributes for the standard data types.
  Does not include attributes for the extended data types
  such as ENUM, SET, GEOMETRY.
*/
class Type_std_attributes
{
public:
  DTCollation collation;
  uint decimals;
  /*
    The maximum value length in characters multiplied by collation->mbmaxlen.
    Almost always it's the maximum value length in bytes.
  */
  uint32 max_length;
  bool unsigned_flag;
  Type_std_attributes()
   :collation(&my_charset_bin, DERIVATION_COERCIBLE),
    decimals(0), max_length(0), unsigned_flag(false)
  { }
  Type_std_attributes(const Type_std_attributes *other)
   :collation(other->collation),
    decimals(other->decimals),
    max_length(other->max_length),
    unsigned_flag(other->unsigned_flag)
  { }
  Type_std_attributes(uint32 max_length_arg, uint decimals_arg,
                      bool unsigned_flag_arg, const DTCollation &dtc)
    :collation(dtc),
     decimals(decimals_arg),
     max_length(max_length_arg),
     unsigned_flag(unsigned_flag_arg)
  { }
  void set(const Type_std_attributes *other)
  {
    *this= *other;
  }
  void set(const Type_std_attributes &other)
  {
    *this= other;
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
  void fix_char_length_temporal_not_fixed_dec(uint int_part_length, uint dec)
  {
    uint char_length= int_part_length;
    if ((decimals= dec))
    {
      if (decimals == NOT_FIXED_DEC)
        char_length+= TIME_SECOND_PART_DIGITS + 1;
      else
      {
        set_if_smaller(decimals, TIME_SECOND_PART_DIGITS);
        char_length+= decimals + 1;
      }
    }
    fix_char_length(char_length);
  }
  void fix_attributes_temporal_not_fixed_dec(uint int_part_length, uint dec)
  {
    collation.set_numeric();
    unsigned_flag= 0;
    fix_char_length_temporal_not_fixed_dec(int_part_length, dec);
  }
  void fix_attributes_time_not_fixed_dec(uint dec)
  {
    fix_attributes_temporal_not_fixed_dec(MIN_TIME_WIDTH, dec);
  }
  void fix_attributes_datetime_not_fixed_dec(uint dec)
  {
    fix_attributes_temporal_not_fixed_dec(MAX_DATETIME_WIDTH, dec);
  }
  void fix_attributes_temporal(uint int_part_length, uint dec)
  {
    collation.set_numeric();
    unsigned_flag= 0;
    decimals= MY_MIN(dec, TIME_SECOND_PART_DIGITS);
    max_length= decimals + int_part_length + (dec ? 1 : 0);
  }
  void fix_attributes_date()
  {
    fix_attributes_temporal(MAX_DATE_WIDTH, 0);
  }
  void fix_attributes_time(uint dec)
  {
    fix_attributes_temporal(MIN_TIME_WIDTH, dec);
  }
  void fix_attributes_datetime(uint dec)
  {
    fix_attributes_temporal(MAX_DATETIME_WIDTH, dec);
  }

  void count_only_length(Item **item, uint nitems);
  void count_octet_length(Item **item, uint nitems);
  void count_real_length(Item **item, uint nitems);
  void count_decimal_length(Item **item, uint nitems);
  bool count_string_length(const char *func_name, Item **item, uint nitems);
  uint count_max_decimals(Item **item, uint nitems);

  void aggregate_attributes_int(Item **items, uint nitems)
  {
    collation.set_numeric();
    count_only_length(items, nitems);
    decimals= 0;
  }
  void aggregate_attributes_real(Item **items, uint nitems)
  {
    collation.set_numeric();
    count_real_length(items, nitems);
  }
  void aggregate_attributes_decimal(Item **items, uint nitems)
  {
    collation.set_numeric();
    count_decimal_length(items, nitems);
  }
  bool aggregate_attributes_string(const char *func_name,
                                   Item **item, uint nitems)
  {
    return count_string_length(func_name, item, nitems);
  }
  void aggregate_attributes_temporal(uint int_part_length,
                                     Item **item, uint nitems)
  {
    fix_attributes_temporal(int_part_length, count_max_decimals(item, nitems));
  }

  bool agg_item_collations(DTCollation &c, const char *name,
                           Item **items, uint nitems,
                           uint flags, int item_sep);
  bool agg_item_set_converter(const DTCollation &coll, const char *fname,
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
  bool agg_arg_charsets(DTCollation &c, const char *func_name,
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
  bool agg_arg_charsets_for_string_result(DTCollation &c, const char *func_name,
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
                                                          const char *func_name,
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
                                       const char *func_name,
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
  Type_all_attributes(const Type_all_attributes *other)
   :Type_std_attributes(other)
  { }
  virtual ~Type_all_attributes() {}
  virtual void set_maybe_null(bool maybe_null_arg)= 0;
  // Returns total number of decimal digits
  virtual uint decimal_precision() const= 0;
  /*
    Field::geometry_type is not visible here.
    Let's use an "uint" wrapper for now. Later when we move Field_geom
    into a plugin, this method will be replaced to some generic
    datatype indepented method.
  */
  virtual uint uint_geometry_type() const= 0;
  virtual void set_geometry_type(uint type)= 0;
  virtual TYPELIB *get_typelib() const= 0;
  virtual void set_typelib(TYPELIB *typelib)= 0;
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
  const char *ptr() const { return LEX_CSTRING::str; }
  uint length() const { return (uint) LEX_CSTRING::length; }
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
  uchar bit() const { return m_ptr ? ((uchar) 1) << m_offs : 0; }
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
  uint m_scale;
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
  Information_schema_numeric_attributes(uint precision, uint scale)
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
  uint scale() const
  {
    DBUG_ASSERT(has_scale());
    return (uint) m_scale;
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


class Type_handler
{
protected:
  static const Name m_version_default;
  static const Name m_version_mysql56;
  static const Name m_version_mariadb53;
  String *print_item_value_csstr(THD *thd, Item *item, String *str) const;
  String *print_item_value_temporal(THD *thd, Item *item, String *str,
                                     const Name &type_name, String *buf) const;
  void make_sort_key_longlong(uchar *to,
                              bool maybe_null, bool null_value,
                              bool unsigned_flag,
                              longlong value) const;
  bool
  Item_func_or_sum_illegal_param(const char *name) const;
  bool
  Item_func_or_sum_illegal_param(const Item_func_or_sum *) const;
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
  static const Type_handler *odbc_literal_type_handler(const LEX_CSTRING *str);
  static const Type_handler *blob_type_handler(uint max_octet_length);
  static const Type_handler *string_type_handler(uint max_octet_length);
  static const Type_handler *bit_and_int_mixture_handler(uint max_char_len);
  static const Type_handler *type_handler_long_or_longlong(uint max_char_len);
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
  static const Type_handler *get_handler_by_result_type(Item_result type)
  {
    /*
      As result_type() returns STRING_RESULT for temporal Items,
      type should never be equal to TIME_RESULT here.
    */
    DBUG_ASSERT(type != TIME_RESULT);
    return get_handler_by_cmp_type(type);
  }
  static const
  Type_handler *aggregate_for_result_traditional(const Type_handler *h1,
                                                 const Type_handler *h2);
  static const
  Type_handler *aggregate_for_num_op_traditional(const Type_handler *h1,
                                                 const Type_handler *h2);

  virtual const Name name() const= 0;
  virtual const Name version() const { return m_version_default; }
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
    DBUG_ASSERT(is_traditional_type());
    return field_type();
  }
  virtual Item_result result_type() const= 0;
  virtual Item_result cmp_type() const= 0;
  virtual enum_mysql_timestamp_type mysql_timestamp_type() const
  {
    return MYSQL_TIMESTAMP_ERROR;
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
  /**
    Prepared statement long data:
    Check whether this parameter data type is compatible with long data.
    Used to detect whether a long data stream has been supplied to a
    incompatible data type.
  */
  virtual bool is_param_long_data_type() const { return false; }
  virtual const Type_handler *type_handler_for_comparison() const= 0;
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
  virtual const Type_handler *type_handler_for_system_time() const
  {
    return this;
  }
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
  virtual ~Type_handler() {}
  /**
    Determines MariaDB traditional data types that always present
    in the server.
  */
  virtual bool is_traditional_type() const
  {
    return true;
  }
  virtual bool is_scalar_type() const { return true; }
  virtual bool can_return_int() const { return true; }
  virtual bool can_return_decimal() const { return true; }
  virtual bool can_return_real() const { return true; }
  virtual bool can_return_str() const { return true; }
  virtual bool can_return_text() const { return true; }
  virtual bool can_return_date() const { return true; }
  virtual bool can_return_time() const { return true; }
  virtual bool is_bool_type() const { return false; }
  virtual bool is_general_purpose_string_type() const { return false; }
  virtual uint Item_time_precision(THD *thd, Item *item) const;
  virtual uint Item_datetime_precision(THD *thd, Item *item) const;
  virtual uint Item_decimal_scale(const Item *item) const;
  virtual uint Item_decimal_precision(const Item *item) const= 0;
  /*
    Returns how many digits a divisor adds into a division result.
    See Item::divisor_precision_increment() in item.h for more comments.
  */
  virtual uint Item_divisor_precision_increment(const Item *) const;
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
  virtual Field *make_conversion_table_field(TABLE *TABLE,
                                             uint metadata,
                                             const Field *target) const= 0;
  // Automatic upgrade, e.g. for ALTER TABLE t1 FORCE
  virtual void Column_definition_implicit_upgrade(Column_definition *c) const
  { }
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
                                                ulonglong table_flags) const;
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
                                                 const handler *file,
                                                 const Schema_specification_st *
                                                       schema)
                                                 const;
  virtual bool Column_definition_prepare_stage2(Column_definition *c,
                                                handler *file,
                                                ulonglong table_flags) const= 0;
  virtual Field *make_table_field(const LEX_CSTRING *name,
                                  const Record_addr &addr,
                                  const Type_all_attributes &attr,
                                  TABLE *table) const= 0;
  Field *make_and_init_table_field(const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Type_all_attributes &attr,
                                   TABLE *table) const;
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
  virtual bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options) const;

  virtual void make_sort_key(uchar *to, Item *item,
                             const SORT_FIELD_ATTR *sort_field,
                             Sort_param *param) const= 0;
  virtual void sortlength(THD *thd,
                          const Type_std_attributes *item,
                          SORT_FIELD_ATTR *attr) const= 0;

  virtual uint32 max_display_length(const Item *item) const= 0;
  virtual uint32 calc_pack_length(uint32 length) const= 0;
  virtual void Item_update_null_value(Item *item) const= 0;
  virtual bool Item_save_in_value(THD *thd, Item *item, st_value *value) const= 0;
  virtual void Item_param_setup_conversion(THD *thd, Item_param *) const {}
  virtual void Item_param_set_param_func(Item_param *param,
                                         uchar **pos, ulong len) const;
  virtual bool Item_param_set_from_value(THD *thd,
                                         Item_param *param,
                                         const Type_all_attributes *attr,
                                         const st_value *value) const= 0;
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
  virtual bool
  subquery_type_allows_materialization(const Item *inner,
                                       const Item *outer) const= 0;
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
    DBUG_ASSERT(0);
    return NULL;
  }
  virtual bool set_comparator_func(Arg_comparator *cmp) const= 0;
  virtual bool Item_const_eq(const Item_const *a, const Item_const *b,
                             bool binary_cmp) const
  {
    return false;
  }
  virtual bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                             Item *a, Item *b) const= 0;
  virtual bool Item_hybrid_func_fix_attributes(THD *thd,
                                               const char *name,
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

  virtual bool Item_val_bool(Item *item) const= 0;
  virtual bool Item_get_date(THD *thd, Item *item, MYSQL_TIME *ltime,
                             date_mode_t fuzzydate) const= 0;
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
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const= 0;
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

  virtual bool
  Vers_history_point_resolve_unit(THD *thd, Vers_history_point *point) const;
};


/*
  Special handler for ROW
*/
class Type_handler_row: public Type_handler
{
  static const Name m_name_row;
public:
  virtual ~Type_handler_row() {}
  const Name name() const { return m_name_row; }
  bool is_scalar_type() const { return false; }
  bool can_return_int() const { return false; }
  bool can_return_decimal() const { return false; }
  bool can_return_real() const { return false; }
  bool can_return_str() const { return false; }
  bool can_return_text() const { return false; }
  bool can_return_date() const { return false; }
  bool can_return_time() const { return false; }
  enum_field_types field_type() const
  {
    DBUG_ASSERT(0);
    return MYSQL_TYPE_NULL;
  };
  Item_result result_type() const
  {
    return ROW_RESULT;
  }
  Item_result cmp_type() const
  {
    return ROW_RESULT;
  }
  const Type_handler *type_handler_for_comparison() const;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const
  {
    DBUG_ASSERT(0);
    return false;
  }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  Field *make_conversion_table_field(TABLE *TABLE,
                                     uint metadata,
                                     const Field *target) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool Column_definition_fix_attributes(Column_definition *c) const
  {
    return false;
  }
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field) const
  {
    DBUG_ASSERT(0);
  }
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file,
                                         const Schema_specification_st *schema)
                                         const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  {
    return false;
  }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const
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
                                   uint32 flags) const;
  void make_sort_key(uchar *to, Item *item,
                     const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const
  {
    DBUG_ASSERT(0);
  }
  void sortlength(THD *thd, const Type_std_attributes *item,
                            SORT_FIELD_ATTR *attr) const
  {
    DBUG_ASSERT(0);
  }
  uint32 max_display_length(const Item *item) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  uint32 calc_pack_length(uint32 length) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const;
  uint Item_decimal_precision(const Item *item) const
  {
    DBUG_ASSERT(0);
    return DECIMAL_MAX_PRECISION;
  }
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  void Item_update_null_value(Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const
  {
    DBUG_ASSERT(0);
    return 1;
  }
  String *print_item_value(THD *thd, Item *item, String *str) const;
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const
  {
    DBUG_ASSERT(0);
    return false;
  }
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_val_bool(Item *item) const
  {
    DBUG_ASSERT(0);
    return false;
  }
  bool Item_get_date(THD *thd, Item *item,
                     MYSQL_TIME *ltime, date_mode_t fuzzydate) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  longlong Item_val_int_signed_typecast(Item *item) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  longlong Item_val_int_unsigned_typecast(Item *item) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const
  {
    DBUG_ASSERT(0);
    return 0.0;
  }
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const
  {
    DBUG_ASSERT(0);
    return true;
  }

  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  double Item_func_min_max_val_real(Item_func_min_max *) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  longlong Item_func_min_max_val_int(Item_func_min_max *) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_func_between_fix_length_and_dec(Item_func_between *func) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *thd, const Item_func_in *f, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const;

  bool Item_func_signed_fix_length_and_dec(Item_func_signed *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *) const
  {
    DBUG_ASSERT(0);
    return true;
  }

  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const;
};


/*
  A common parent class for numeric data type handlers
*/
class Type_handler_numeric: public Type_handler
{
protected:
  bool Item_sum_hybrid_fix_length_and_dec_numeric(Item_sum_hybrid *func,
                                                  const Type_handler *handler)
                                                  const;
public:
  String *print_item_value(THD *thd, Item *item, String *str) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate) const;
  virtual ~Type_handler_numeric() { }
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const;
  bool Item_func_between_fix_length_and_dec(Item_func_between *func) const;
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const;
};


/*** Abstract classes for every XXX_RESULT */

class Type_handler_real_result: public Type_handler_numeric
{
public:
  Item_result result_type() const { return REAL_RESULT; }
  Item_result cmp_type() const { return REAL_RESULT; }
  virtual ~Type_handler_real_result() {}
  const Type_handler *type_handler_for_comparison() const;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field) const;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const;
  uint Item_decimal_precision(const Item *item) const;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const;
  void Item_update_null_value(Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  bool Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const;
  bool Item_func_signed_fix_length_and_dec(Item_func_signed *item) const;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const;
  bool Item_val_bool(Item *item) const;
  bool Item_get_date(THD *thd, Item *item, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const;
  longlong Item_val_int_signed_typecast(Item *item) const;
  longlong Item_val_int_unsigned_typecast(Item *item) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;

  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const;
};


class Type_handler_decimal_result: public Type_handler_numeric
{
public:
  Item_result result_type() const { return DECIMAL_RESULT; }
  Item_result cmp_type() const { return DECIMAL_RESULT; }
  virtual ~Type_handler_decimal_result() {};
  const Type_handler *type_handler_for_comparison() const;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const
  {
    VDec item_val(item);
    return item_val.is_null() ? 0 : my_decimal(field).cmp(item_val.ptr());
  }
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const;
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  uint32 max_display_length(const Item *item) const;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const
  {
    VDec va(a), vb(b);
    return va.ptr() && vb.ptr() && !va.cmp(vb);
  }
  uint Item_decimal_precision(const Item *item) const;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_str(item, protocol, buf);
  }
  void Item_update_null_value(Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const;
  bool Item_val_bool(Item *item) const
  {
    return VDec(item).to_bool();
  }
  bool Item_get_date(THD *thd, Item *item, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const
  {
    return VDec(item).to_datetime_with_warn(thd, ltime, fuzzydate, item);
  }
  longlong Item_val_int_signed_typecast(Item *item) const;
  longlong Item_val_int_unsigned_typecast(Item *item) const
  {
    return VDec(item).to_longlong(true);
  }
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const;
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
  Item_result result_type() const { return INT_RESULT; }
  Item_result cmp_type() const { return INT_RESULT; }
  bool is_order_clause_position_type() const { return true; }
  bool is_limit_clause_valid_type() const { return true; }
  virtual ~Type_handler_int_result() {}
  const Type_handler *type_handler_for_comparison() const;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const;
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const;
  uint Item_decimal_precision(const Item *item) const;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const;
  void Item_update_null_value(Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const;
  bool Item_val_bool(Item *item) const;
  bool Item_get_date(THD *thd, Item *item, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const;
  longlong Item_val_int_signed_typecast(Item *item) const;
  longlong Item_val_int_unsigned_typecast(Item *item) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const;

};


class Type_handler_general_purpose_int: public Type_handler_int_result
{
public:
  bool type_can_have_auto_increment_attribute() const { return true; }
  virtual const Type_limits_int *
    type_limits_int_by_unsigned_flag(bool unsigned_flag) const= 0;
  uint32 max_display_length(const Item *item) const;
  bool Vers_history_point_resolve_unit(THD *thd, Vers_history_point *p) const;
};


class Type_handler_temporal_result: public Type_handler
{
protected:
  uint Item_decimal_scale_with_seconds(const Item *item) const;
  uint Item_divisor_precision_increment_with_seconds(const Item *) const;
public:
  Item_result result_type() const { return STRING_RESULT; }
  Item_result cmp_type() const { return TIME_RESULT; }
  virtual ~Type_handler_temporal_result() {}
  void make_sort_key(uchar *to, Item *item,  const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const;
  uint32 max_display_length(const Item *item) const;
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const;
  bool Item_func_min_max_fix_attributes(THD *thd, Item_func_min_max *func,
                                        Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const;
  bool Item_val_bool(Item *item) const;
  bool Item_get_date(THD *thd, Item *item, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const;
  longlong Item_val_int_signed_typecast(Item *item) const;
  longlong Item_val_int_unsigned_typecast(Item *item) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate) const;
  bool Item_func_between_fix_length_and_dec(Item_func_between *func) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const;
  bool Vers_history_point_resolve_unit(THD *thd, Vers_history_point *p) const;
};


class Type_handler_string_result: public Type_handler
{
  uint Item_temporal_precision(THD *thd, Item *item, bool is_time) const;
public:
  Item_result result_type() const { return STRING_RESULT; }
  Item_result cmp_type() const { return STRING_RESULT; }
  CHARSET_INFO *charset_for_protocol(const Item *item) const;
  virtual ~Type_handler_string_result() {}
  const Type_handler *type_handler_for_comparison() const;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const;
  const Type_handler *
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file,
                                         const Schema_specification_st *schema)
                                         const;
  uint32 max_display_length(const Item *item) const;
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const;
  uint Item_time_precision(THD *thd, Item *item) const
  {
    return Item_temporal_precision(thd, item, true);
  }
  uint Item_datetime_precision(THD *thd, Item *item) const
  {
    return Item_temporal_precision(thd, item, false);
  }
  uint Item_decimal_precision(const Item *item) const;
  void Item_update_null_value(Item *item) const;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  void Item_param_setup_conversion(THD *thd, Item_param *) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_str(item, protocol, buf);
  }
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  String *print_item_value(THD *thd, Item *item, String *str) const
  {
    return print_item_value_csstr(thd, item, str);
  }
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const;
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const;
  bool Item_func_signed_fix_length_and_dec(Item_func_signed *item) const;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const;
  bool Item_val_bool(Item *item) const;
  bool Item_get_date(THD *thd, Item *item, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const;
  longlong Item_val_int_signed_typecast(Item *item) const;
  longlong Item_val_int_unsigned_typecast(Item *item) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate) const;
  bool Item_func_between_fix_length_and_dec(Item_func_between *func) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const;
  bool Item_func_plus_fix_length_and_dec(Item_func_plus *) const;
  bool Item_func_minus_fix_length_and_dec(Item_func_minus *) const;
  bool Item_func_mul_fix_length_and_dec(Item_func_mul *) const;
  bool Item_func_div_fix_length_and_dec(Item_func_div *) const;
  bool Item_func_mod_fix_length_and_dec(Item_func_mod *) const;
};


class Type_handler_general_purpose_string: public Type_handler_string_result
{
public:
  bool is_general_purpose_string_type() const { return true; }
  bool Vers_history_point_resolve_unit(THD *thd, Vers_history_point *p) const;
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
  static const Name m_name_tiny;
  static const Type_limits_int m_limits_sint8;
  static const Type_limits_int m_limits_uint8;
public:
  virtual ~Type_handler_tiny() {}
  const Name name() const { return m_name_tiny; }
  enum_field_types field_type() const { return MYSQL_TYPE_TINY; }
  const Type_limits_int *type_limits_int_by_unsigned_flag(bool unsigned_fl) const
  {
    return unsigned_fl ? &m_limits_uint8 : &m_limits_sint8;
  }
  uint32 calc_pack_length(uint32 length) const { return 1; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_tiny(item, protocol, buf);
  }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_TINY); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_short: public Type_handler_general_purpose_int
{
  static const Name m_name_short;
  static const Type_limits_int m_limits_sint16;
  static const Type_limits_int m_limits_uint16;
public:
  virtual ~Type_handler_short() {}
  const Name name() const { return m_name_short; }
  enum_field_types field_type() const { return MYSQL_TYPE_SHORT; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_short(item, protocol, buf);
  }
  const Type_limits_int *type_limits_int_by_unsigned_flag(bool unsigned_fl) const
  {
    return unsigned_fl ? &m_limits_uint16 : &m_limits_sint16;
  }
  uint32 calc_pack_length(uint32 length) const { return 2; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_SHORT); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_long: public Type_handler_general_purpose_int
{
  static const Name m_name_int;
  static const Type_limits_int m_limits_sint32;
  static const Type_limits_int m_limits_uint32;
public:
  virtual ~Type_handler_long() {}
  const Name name() const { return m_name_int; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONG; }
  const Type_limits_int *type_limits_int_by_unsigned_flag(bool unsigned_fl) const
  {
    return unsigned_fl ? &m_limits_uint32 : &m_limits_sint32;
  }
  uint32 calc_pack_length(uint32 length) const { return 4; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_long(item, protocol, buf);
  }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_LONG); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_bool: public Type_handler_long
{
  static const Name m_name_bool;
public:
  const Name name() const { return m_name_bool; }
  bool is_bool_type() const { return true; }
  void Item_update_null_value(Item *item) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const;
};


class Type_handler_longlong: public Type_handler_general_purpose_int
{
  static const Name m_name_longlong;
  static const Type_limits_int m_limits_sint64;
  static const Type_limits_int m_limits_uint64;
public:
  virtual ~Type_handler_longlong() {}
  const Name name() const { return m_name_longlong; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  const Type_limits_int *type_limits_int_by_unsigned_flag(bool unsigned_fl) const
  {
    return unsigned_fl ? &m_limits_uint64 : &m_limits_sint64;
  }
  uint32 calc_pack_length(uint32 length) const { return 8; }
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_longlong(item, protocol, buf);
  }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  {
    return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_LONGLONG);
  }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_vers_trx_id: public Type_handler_longlong
{
public:
  virtual ~Type_handler_vers_trx_id() {}
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
};


class Type_handler_int24: public Type_handler_general_purpose_int
{
  static const Name m_name_mediumint;
  static const Type_limits_int m_limits_sint24;
  static const Type_limits_int m_limits_uint24;
public:
  virtual ~Type_handler_int24() {}
  const Name name() const { return m_name_mediumint; }
  enum_field_types field_type() const { return MYSQL_TYPE_INT24; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_long(item, protocol, buf);
  }
  const Type_limits_int *type_limits_int_by_unsigned_flag(bool unsigned_fl) const
  {
    return unsigned_fl ? &m_limits_uint24 : &m_limits_sint24;
  }
  uint32 calc_pack_length(uint32 length) const { return 3; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_INT24); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_year: public Type_handler_int_result
{
  static const Name m_name_year;
public:
  virtual ~Type_handler_year() {}
  const Name name() const { return m_name_year; }
  enum_field_types field_type() const { return MYSQL_TYPE_YEAR; }
  uint32 max_display_length(const Item *item) const;
  uint32 calc_pack_length(uint32 length) const { return 1; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_short(item, protocol, buf);
  }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_YEAR); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool Item_get_date(THD *thd, Item *item, MYSQL_TIME *ltime,
                     date_mode_t fuzzydate) const;
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *item,
                                            MYSQL_TIME *to,
                                            date_mode_t fuzzydate) const;
};


class Type_handler_bit: public Type_handler_int_result
{
  static const Name m_name_bit;
public:
  virtual ~Type_handler_bit() {}
  const Name name() const { return m_name_bit; }
  enum_field_types field_type() const { return MYSQL_TYPE_BIT; }
  uint32 max_display_length(const Item *item) const;
  uint32 calc_pack_length(uint32 length) const { return length / 8; }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_str(item, protocol, buf);
  }
  String *print_item_value(THD *thd, Item *item, String *str) const
  {
    return print_item_value_csstr(thd, item, str);
  }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file,
                                         const Schema_specification_st *schema)
                                         const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  bool Vers_history_point_resolve_unit(THD *thd, Vers_history_point *p) const;
};


class Type_handler_float: public Type_handler_real_result
{
  static const Name m_name_float;
public:
  virtual ~Type_handler_float() {}
  const Name name() const { return m_name_float; }
  enum_field_types field_type() const { return MYSQL_TYPE_FLOAT; }
  bool type_can_have_auto_increment_attribute() const { return true; }
  uint32 max_display_length(const Item *item) const { return 25; }
  uint32 calc_pack_length(uint32 length) const { return sizeof(float); }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_float(item, protocol, buf);
  }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_real(c, MYSQL_TYPE_FLOAT); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_double: public Type_handler_real_result
{
  static const Name m_name_double;
public:
  virtual ~Type_handler_double() {}
  const Name name() const { return m_name_double; }
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  bool type_can_have_auto_increment_attribute() const { return true; }
  uint32 max_display_length(const Item *item) const { return 53; }
  uint32 calc_pack_length(uint32 length) const { return sizeof(double); }
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_double(item, protocol, buf);
  }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_real(c, MYSQL_TYPE_DOUBLE); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_time_common: public Type_handler_temporal_result
{
  static const Name m_name_time;
public:
  virtual ~Type_handler_time_common() { }
  const Name name() const { return m_name_time; }
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
  enum_mysql_timestamp_type mysql_timestamp_type() const
  {
    return MYSQL_TIMESTAMP_TIME;
  }
  Item_literal *create_literal_item(THD *thd, const char *str, size_t length,
                                    CHARSET_INFO *cs, bool send_error) const;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const;
  uint Item_decimal_scale(const Item *item) const
  {
    return Item_decimal_scale_with_seconds(item);
  }
  uint Item_decimal_precision(const Item *item) const;
  uint Item_divisor_precision_increment(const Item *item) const
  {
    return Item_divisor_precision_increment_with_seconds(item);
  }
  const Type_handler *type_handler_for_comparison() const;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const;
  void Column_definition_implicit_upgrade(Column_definition *c) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_time(item, protocol, buf);
  }
  void Item_update_null_value(Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  String *print_item_value(THD *thd, Item *item, String *str) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(THD *,
                                            Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            date_mode_t fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_func_min_max_get_date(THD *thd, Item_func_min_max*,
                                  MYSQL_TIME *, date_mode_t fuzzydate) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_time: public Type_handler_time_common
{
  /* number of bytes to store TIME(N) */
  static uint m_hires_bytes[MAX_DATETIME_PRECISION+1];
public:
  static uint hires_bytes(uint dec) { return m_hires_bytes[dec]; }
  virtual ~Type_handler_time() {}
  const Name version() const { return m_version_mariadb53; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_TIME); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_time2: public Type_handler_time_common
{
public:
  virtual ~Type_handler_time2() {}
  const Name version() const { return m_version_mysql56; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_TIME2; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_TIME2); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_temporal_with_date: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_temporal_with_date() {}
  Item_literal *create_literal_item(THD *thd, const char *str, size_t length,
                                    CHARSET_INFO *cs, bool send_error) const;
  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const;
  int stored_field_cmp_to_item(THD *thd, Field *field, Item *item) const;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_date(item, protocol, buf);
  }
  void Item_update_null_value(Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  Item *make_const_item_for_comparison(THD *, Item *src, const Item *cmp) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
};


class Type_handler_date_common: public Type_handler_temporal_with_date
{
  static const Name m_name_date;
public:
  virtual ~Type_handler_date_common() {}
  const Name name() const { return m_name_date; }
  const Type_handler *type_handler_for_comparison() const;
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
  enum_mysql_timestamp_type mysql_timestamp_type() const
  {
    return MYSQL_TIMESTAMP_DATE;
  }
  bool cond_notnull_field_isnull_to_field_eq_zero() const
  {
    return true;
  }
  Item_literal *create_literal_item(THD *thd, const char *str, size_t length,
                                    CHARSET_INFO *cs, bool send_error) const;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  uint Item_decimal_precision(const Item *item) const;
  String *print_item_value(THD *thd, Item *item, String *str) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};

class Type_handler_date: public Type_handler_date_common
{
public:
  virtual ~Type_handler_date() {}
  uint32 calc_pack_length(uint32 length) const { return 4; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_DATE); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_newdate: public Type_handler_date_common
{
public:
  virtual ~Type_handler_newdate() {}
  enum_field_types real_field_type() const { return MYSQL_TYPE_NEWDATE; }
  uint32 calc_pack_length(uint32 length) const { return 3; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_NEWDATE); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_datetime_common: public Type_handler_temporal_with_date
{
  static const Name m_name_datetime;
public:
  virtual ~Type_handler_datetime_common() {}
  const Name name() const { return m_name_datetime; }
  const Type_handler *type_handler_for_comparison() const;
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
  enum_mysql_timestamp_type mysql_timestamp_type() const
  {
    return MYSQL_TIMESTAMP_DATETIME;
  }
  bool cond_notnull_field_isnull_to_field_eq_zero() const
  {
    return true;
  }
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  void Column_definition_implicit_upgrade(Column_definition *c) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  uint Item_decimal_scale(const Item *item) const
  {
    return Item_decimal_scale_with_seconds(item);
  }
  uint Item_decimal_precision(const Item *item) const;
  uint Item_divisor_precision_increment(const Item *item) const
  {
    return Item_divisor_precision_increment_with_seconds(item);
  }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_datetime(item, protocol, buf);
  }
  String *print_item_value(THD *thd, Item *item, String *str) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_datetime: public Type_handler_datetime_common
{
  /* number of bytes to store DATETIME(N) */
  static uint m_hires_bytes[MAX_DATETIME_PRECISION + 1];
public:
  static uint hires_bytes(uint dec) { return m_hires_bytes[dec]; }
  virtual ~Type_handler_datetime() {}
  const Name version() const { return m_version_mariadb53; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_DATETIME); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_datetime2: public Type_handler_datetime_common
{
public:
  virtual ~Type_handler_datetime2() {}
  const Name version() const { return m_version_mysql56; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_DATETIME2; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_DATETIME2); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_timestamp_common: public Type_handler_temporal_with_date
{
  static const Name m_name_timestamp;
public:
  virtual ~Type_handler_timestamp_common() {}
  const Name name() const { return m_name_timestamp; }
  const Type_handler *type_handler_for_comparison() const;
  enum_field_types field_type() const { return MYSQL_TYPE_TIMESTAMP; }
  enum_mysql_timestamp_type mysql_timestamp_type() const
  {
    return MYSQL_TIMESTAMP_DATETIME;
  }
  bool is_timestamp_type() const
  {
    return true;
  }
  void Column_definition_implicit_upgrade(Column_definition *c) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  uint Item_decimal_scale(const Item *item) const
  {
    return Item_decimal_scale_with_seconds(item);
  }
  uint Item_decimal_precision(const Item *item) const;
  uint Item_divisor_precision_increment(const Item *item) const
  {
    return Item_divisor_precision_increment_with_seconds(item);
  }
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const
  {
    return Item_send_datetime(item, protocol, buf);
  }
  String *print_item_value(THD *thd, Item *item, String *str) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
};


class Type_handler_timestamp: public Type_handler_timestamp_common
{
  /* number of bytes to store second_part part of the TIMESTAMP(N) */
  static uint m_sec_part_bytes[MAX_DATETIME_PRECISION + 1];
public:
  static uint sec_part_bytes(uint dec) { return m_sec_part_bytes[dec]; }
  virtual ~Type_handler_timestamp() {}
  const Name version() const { return m_version_mariadb53; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_TIMESTAMP); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_timestamp2: public Type_handler_timestamp_common
{
public:
  virtual ~Type_handler_timestamp2() {}
  const Name version() const { return m_version_mysql56; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_TIMESTAMP2; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  {
    return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_TIMESTAMP2);
  }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_olddecimal: public Type_handler_decimal_result
{
  static const Name m_name_decimal;
public:
  virtual ~Type_handler_olddecimal() {}
  const Name name() const { return m_name_decimal; }
  enum_field_types field_type() const { return MYSQL_TYPE_DECIMAL; }
  uint32 calc_pack_length(uint32 length) const { return length; }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const;
  const Type_handler *type_handler_for_union(const Item *item) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_DECIMAL); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_newdecimal: public Type_handler_decimal_result
{
  static const Name m_name_decimal;
public:
  virtual ~Type_handler_newdecimal() {}
  const Name name() const { return m_name_decimal; }
  enum_field_types field_type() const { return MYSQL_TYPE_NEWDECIMAL; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file,
                                         const Schema_specification_st *schema)
                                         const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_null: public Type_handler_general_purpose_string
{
  static const Name m_name_null;
public:
  virtual ~Type_handler_null() {}
  const Name name() const { return m_name_null; }
  enum_field_types field_type() const { return MYSQL_TYPE_NULL; }
  const Type_handler *type_handler_for_comparison() const;
  const Type_handler *type_handler_for_tmp_table(const Item *item) const;
  const Type_handler *type_handler_for_union(const Item *) const;
  uint32 max_display_length(const Item *item) const { return 0; }
  uint32 calc_pack_length(uint32 length) const { return 0; }
  bool Item_const_eq(const Item_const *a, const Item_const *b,
                     bool binary_cmp) const;
  bool Item_save_in_value(THD *thd, Item *item, st_value *value) const;
  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file,
                                         const Schema_specification_st *schema)
                                         const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy(c, MYSQL_TYPE_NULL); }
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_longstr: public Type_handler_general_purpose_string
{
public:
  bool type_can_have_key_part() const
  {
    return true;
  }
};


class Type_handler_string: public Type_handler_longstr
{
  static const Name m_name_char;
public:
  virtual ~Type_handler_string() {}
  const Name name() const { return m_name_char; }
  enum_field_types field_type() const { return MYSQL_TYPE_STRING; }
  bool is_param_long_data_type() const { return true; }
  uint32 calc_pack_length(uint32 length) const { return length; }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const
  {
    return varstring_type_handler(item);
  }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


/* Old varchar */
class Type_handler_var_string: public Type_handler_string
{
  static const Name m_name_var_string;
public:
  virtual ~Type_handler_var_string() {}
  const Name name() const { return m_name_var_string; }
  enum_field_types field_type() const { return MYSQL_TYPE_VAR_STRING; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_STRING; }
  enum_field_types traditional_merge_field_type() const
  {
    return MYSQL_TYPE_VARCHAR;
  }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const
  {
    return varstring_type_handler(item);
  }
  void Column_definition_implicit_upgrade(Column_definition *c) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const
  { return Column_definition_prepare_stage2_legacy_num(c, MYSQL_TYPE_STRING); }
  const Type_handler *type_handler_for_union(const Item *item) const
  {
    return varstring_type_handler(item);
  }
};


class Type_handler_varchar: public Type_handler_longstr
{
  static const Name m_name_varchar;
public:
  virtual ~Type_handler_varchar() {}
  const Name name() const { return m_name_varchar; }
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  uint32 calc_pack_length(uint32 length) const
  {
    return (length + (length < 256 ? 1: 2));
  }
  const Type_handler *type_handler_for_tmp_table(const Item *item) const
  {
    return varstring_type_handler(item);
  }
  const Type_handler *type_handler_for_union(const Item *item) const
  {
    return varstring_type_handler(item);
  }
  bool is_param_long_data_type() const { return true; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
  bool adjust_spparam_type(Spvar_definition *def, Item *from) const;
};


class Type_handler_hex_hybrid: public Type_handler_varchar
{
  static const Name m_name_hex_hybrid;
public:
  virtual ~Type_handler_hex_hybrid() {}
  const Name name() const { return m_name_hex_hybrid; }
  const Type_handler *cast_to_int_type_handler() const;
  const Type_handler *type_handler_for_system_time() const;
};


class Type_handler_varchar_compressed: public Type_handler_varchar
{
public:
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_blob_common: public Type_handler_longstr
{
public:
  virtual ~Type_handler_blob_common() { }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  const Type_handler *type_handler_for_tmp_table(const Item *item) const
  {
    return blob_type_handler(item);
  }
  const Type_handler *type_handler_for_union(const Item *item) const
  {
    return blob_type_handler(item);
  }
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const
  {
    return false; // Materialization does not work with BLOB columns
  }
  bool is_param_long_data_type() const { return true; }
  bool Column_definition_fix_attributes(Column_definition *c) const;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  void Item_param_setup_conversion(THD *thd, Item_param *) const;

  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_tiny_blob: public Type_handler_blob_common
{
  static const Name m_name_tinyblob;
public:
  virtual ~Type_handler_tiny_blob() {}
  const Name name() const { return m_name_tinyblob; }
  enum_field_types field_type() const { return MYSQL_TYPE_TINY_BLOB; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
};


class Type_handler_medium_blob: public Type_handler_blob_common
{
  static const Name m_name_mediumblob;
public:
  virtual ~Type_handler_medium_blob() {}
  const Name name() const { return m_name_mediumblob; }
  enum_field_types field_type() const { return MYSQL_TYPE_MEDIUM_BLOB; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
};


class Type_handler_long_blob: public Type_handler_blob_common
{
  static const Name m_name_longblob;
public:
  virtual ~Type_handler_long_blob() {}
  const Name name() const { return m_name_longblob; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONG_BLOB; }
  uint32 calc_pack_length(uint32 length) const;
  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
};


class Type_handler_blob: public Type_handler_blob_common
{
  static const Name m_name_blob;
public:
  virtual ~Type_handler_blob() {}
  const Name name() const { return m_name_blob; }
  enum_field_types field_type() const { return MYSQL_TYPE_BLOB; }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
};


class Type_handler_blob_compressed: public Type_handler_blob
{
public:
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


#ifdef HAVE_SPATIAL
class Type_handler_geometry: public Type_handler_string_result
{
  static const Name m_name_geometry;
public:
  virtual ~Type_handler_geometry() {}
  const Name name() const { return m_name_geometry; }
  enum_field_types field_type() const { return MYSQL_TYPE_GEOMETRY; }
  bool is_param_long_data_type() const { return true; }
  uint32 calc_pack_length(uint32 length) const;
  const Type_handler *type_handler_for_comparison() const;
  bool type_can_have_key_part() const
  {
    return true;
  }
  bool subquery_type_allows_materialization(const Item *inner,
                                            const Item *outer) const
  {
    return false; // Materialization does not work with GEOMETRY columns
  }
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
  bool Item_param_set_from_value(THD *thd,
                                 Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *value) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  void
  Column_definition_attributes_frm_pack(const Column_definition_attributes *at,
                                        uchar *buff) const;
  bool
  Column_definition_attributes_frm_unpack(Column_definition_attributes *attr,
                                          TABLE_SHARE *share,
                                          const uchar *buffer,
                                          LEX_CUSTRING *gis_options) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field) const;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;

  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;

  bool can_return_int() const { return false; }
  bool can_return_decimal() const { return false; }
  bool can_return_real() const { return false; }
  bool can_return_text() const { return false; }
  bool can_return_date() const { return false; }
  bool can_return_time() const { return false; }
  bool is_traditional_type() const
  {
    return false;
  }
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *) const;
  bool Item_func_abs_fix_length_and_dec(Item_func_abs *) const;
  bool Item_func_neg_fix_length_and_dec(Item_func_neg *) const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *h,
                                       Type_all_attributes *attr,
                                       Item **items, uint nitems) const;
  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *) const;
  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *) const;
  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *) const;

  bool Item_func_signed_fix_length_and_dec(Item_func_signed *) const;
  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *) const;
  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *) const;
  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *) const;
  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *) const;
  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *) const;
  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *) const;
  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *) const;
};

extern MYSQL_PLUGIN_IMPORT Type_handler_geometry type_handler_geometry;
#endif


class Type_handler_typelib: public Type_handler_general_purpose_string
{
public:
  virtual ~Type_handler_typelib() { }
  enum_field_types field_type() const { return MYSQL_TYPE_STRING; }
  const Type_handler *type_handler_for_item_field() const;
  const Type_handler *cast_to_int_type_handler() const;
  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const char *name,
                                       Type_handler_hybrid_field_type *,
                                       Type_all_attributes *atrr,
                                       Item **items, uint nitems) const;
  void Column_definition_reuse_fix_attributes(THD *thd,
                                              Column_definition *c,
                                              const Field *field) const;
  bool Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  bool Column_definition_redefine_stage1(Column_definition *def,
                                         const Column_definition *dup,
                                         const handler *file,
                                         const Schema_specification_st *schema)
                                         const;
  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const;
  bool Vers_history_point_resolve_unit(THD *thd, Vers_history_point *p) const;
};


class Type_handler_enum: public Type_handler_typelib
{
  static const Name m_name_enum;
public:
  virtual ~Type_handler_enum() {}
  const Name name() const { return m_name_enum; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_ENUM; }
  enum_field_types traditional_merge_field_type() const
  {
    return MYSQL_TYPE_ENUM;
  }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
};


class Type_handler_set: public Type_handler_typelib
{
  static const Name m_name_set;
public:
  virtual ~Type_handler_set() {}
  const Name name() const { return m_name_set; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_SET; }
  enum_field_types traditional_merge_field_type() const
  {
    return MYSQL_TYPE_SET;
  }
  uint32 calc_pack_length(uint32 length) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool Column_definition_fix_attributes(Column_definition *c) const;
  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const;
  Field *make_table_field(const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE *table) const;
  Field *make_table_field_from_def(TABLE_SHARE *share,
                                   MEM_ROOT *mem_root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &addr,
                                   const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const;
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
  const Type_handler *set_handler_by_result_type(Item_result type)
  {
    return (m_type_handler= Type_handler::get_handler_by_result_type(type));
  }
  const Type_handler *set_handler_by_cmp_type(Item_result type)
  {
    return (m_type_handler= Type_handler::get_handler_by_cmp_type(type));
  }
  const Type_handler *set_handler_by_result_type(Item_result type,
                                                 uint max_octet_length,
                                                 CHARSET_INFO *cs)
  {
    m_type_handler= Type_handler::get_handler_by_result_type(type);
    return m_type_handler=
      m_type_handler->type_handler_adjusted_to_max_octet_length(max_octet_length,
                                                                cs);
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
  bool aggregate_for_comparison(const char *funcname,
                                Item **items, uint nitems,
                                bool treat_int_to_uint_as_decimal);
  bool aggregate_for_result(const Type_handler *other);
  bool aggregate_for_result(const char *funcname,
                            Item **item, uint nitems, bool treat_bit_as_number);
  bool aggregate_for_min_max(const char *funcname, Item **item, uint nitems);

  bool aggregate_for_num_op(const class Type_aggregator *aggregator,
                            const Type_handler *h0, const Type_handler *h1);
};


extern MYSQL_PLUGIN_IMPORT Type_handler_row         type_handler_row;
extern MYSQL_PLUGIN_IMPORT Type_handler_null        type_handler_null;

extern MYSQL_PLUGIN_IMPORT Type_handler_float       type_handler_float;
extern MYSQL_PLUGIN_IMPORT Type_handler_double      type_handler_double;

extern MYSQL_PLUGIN_IMPORT Type_handler_bit         type_handler_bit;

extern MYSQL_PLUGIN_IMPORT Type_handler_enum        type_handler_enum;
extern MYSQL_PLUGIN_IMPORT Type_handler_set         type_handler_set;

extern MYSQL_PLUGIN_IMPORT Type_handler_string      type_handler_string;
extern MYSQL_PLUGIN_IMPORT Type_handler_var_string  type_handler_var_string;
extern MYSQL_PLUGIN_IMPORT Type_handler_varchar     type_handler_varchar;
extern MYSQL_PLUGIN_IMPORT Type_handler_hex_hybrid  type_handler_hex_hybrid;

extern MYSQL_PLUGIN_IMPORT Type_handler_tiny_blob   type_handler_tiny_blob;
extern MYSQL_PLUGIN_IMPORT Type_handler_medium_blob type_handler_medium_blob;
extern MYSQL_PLUGIN_IMPORT Type_handler_long_blob   type_handler_long_blob;
extern MYSQL_PLUGIN_IMPORT Type_handler_blob        type_handler_blob;

extern MYSQL_PLUGIN_IMPORT Type_handler_bool        type_handler_bool;
extern MYSQL_PLUGIN_IMPORT Type_handler_tiny        type_handler_tiny;
extern MYSQL_PLUGIN_IMPORT Type_handler_short       type_handler_short;
extern MYSQL_PLUGIN_IMPORT Type_handler_int24       type_handler_int24;
extern MYSQL_PLUGIN_IMPORT Type_handler_long        type_handler_long;
extern MYSQL_PLUGIN_IMPORT Type_handler_longlong    type_handler_longlong;
extern MYSQL_PLUGIN_IMPORT Type_handler_longlong    type_handler_ulonglong;
extern MYSQL_PLUGIN_IMPORT Type_handler_vers_trx_id type_handler_vers_trx_id;

extern MYSQL_PLUGIN_IMPORT Type_handler_newdecimal  type_handler_newdecimal;
extern MYSQL_PLUGIN_IMPORT Type_handler_olddecimal  type_handler_olddecimal;

extern MYSQL_PLUGIN_IMPORT Type_handler_year        type_handler_year;
extern MYSQL_PLUGIN_IMPORT Type_handler_year        type_handler_year2;
extern MYSQL_PLUGIN_IMPORT Type_handler_newdate     type_handler_newdate;
extern MYSQL_PLUGIN_IMPORT Type_handler_date        type_handler_date;
extern MYSQL_PLUGIN_IMPORT Type_handler_time        type_handler_time;
extern MYSQL_PLUGIN_IMPORT Type_handler_time2       type_handler_time2;
extern MYSQL_PLUGIN_IMPORT Type_handler_datetime    type_handler_datetime;
extern MYSQL_PLUGIN_IMPORT Type_handler_datetime2   type_handler_datetime2;
extern MYSQL_PLUGIN_IMPORT Type_handler_timestamp   type_handler_timestamp;
extern MYSQL_PLUGIN_IMPORT Type_handler_timestamp2  type_handler_timestamp2;

extern MYSQL_PLUGIN_IMPORT Type_handler_tiny_blob   type_handler_tiny_blob;
extern MYSQL_PLUGIN_IMPORT Type_handler_blob        type_handler_blob;
extern MYSQL_PLUGIN_IMPORT Type_handler_medium_blob type_handler_medium_blob;
extern MYSQL_PLUGIN_IMPORT Type_handler_long_blob   type_handler_long_blob;

class Type_aggregator
{
  bool m_is_commutative;
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
  Dynamic_array<Pair> m_array;
  const Pair* find_pair(const Type_handler *handler1,
                        const Type_handler *handler2) const;
public:
  Type_aggregator(bool is_commutative= false)
   :m_is_commutative(is_commutative)
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
