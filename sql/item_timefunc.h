#ifndef ITEM_TIMEFUNC_INCLUDED
#define ITEM_TIMEFUNC_INCLUDED
/* Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009-2011, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


/* Function items used by mysql */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

class MY_LOCALE;

enum date_time_format_types 
{ 
  TIME_ONLY= 0, TIME_MICROSECOND, DATE_ONLY, DATE_TIME, DATE_TIME_MICROSECOND
};


static inline uint
mysql_temporal_int_part_length(enum enum_field_types mysql_type)
{
  static uint max_time_type_width[5]=
  { MAX_DATETIME_WIDTH, MAX_DATETIME_WIDTH, MAX_DATE_WIDTH,
    MAX_DATETIME_WIDTH, MIN_TIME_WIDTH };
  return max_time_type_width[mysql_type_to_time_type(mysql_type)+2];
}


bool get_interval_value(Item *args,interval_type int_type, INTERVAL *interval);

class Item_func_period_add :public Item_int_func
{
public:
  Item_func_period_add(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "period_add"; }
  bool fix_length_and_dec()
  {
    max_length=6*MY_CHARSET_BIN_MB_MAXLEN;
    return FALSE;
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_period_add>(thd, mem_root, this); }
};


class Item_func_period_diff :public Item_int_func
{
public:
  Item_func_period_diff(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "period_diff"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=6*MY_CHARSET_BIN_MB_MAXLEN;
    return FALSE;
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_period_diff>(thd, mem_root, this); }
};


class Item_func_to_days :public Item_int_func
{
public:
  Item_func_to_days(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "to_days"; }
  bool fix_length_and_dec()
  {
    decimals=0; 
    max_length=6*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  enum_monotonicity_info get_monotonicity_info() const;
  longlong val_int_endpoint(bool left_endp, bool *incl_endp);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_to_days>(thd, mem_root, this); }
};


class Item_func_to_seconds :public Item_int_func
{
public:
  Item_func_to_seconds(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "to_seconds"; }
  bool fix_length_and_dec()
  {
    decimals=0; 
    max_length=6*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null= 1;
    return FALSE;
  }
  enum_monotonicity_info get_monotonicity_info() const;
  longlong val_int_endpoint(bool left_endp, bool *incl_endp);
  bool check_partition_func_processor(void *bool_arg) { return FALSE;}

  /* Only meaningful with date part and optional time part */
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_to_seconds>(thd, mem_root, this); }
};


class Item_func_dayofmonth :public Item_int_func
{
public:
  Item_func_dayofmonth(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "dayofmonth"; }
  bool fix_length_and_dec()
  {
    decimals=0; 
    max_length=2*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_dayofmonth>(thd, mem_root, this); }
};


class Item_func_month :public Item_func
{
public:
  Item_func_month(THD *thd, Item *a): Item_func(thd, a)
  { collation.set_numeric(); }
  longlong val_int();
  double val_real()
  { DBUG_ASSERT(fixed == 1); return (double) Item_func_month::val_int(); }
  String *val_str(String *str)
  {
    longlong nr= val_int();
    if (null_value)
      return 0;
    str->set(nr, collation.collation);
    return str;
  }
  const char *func_name() const { return "month"; }
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  bool fix_length_and_dec()
  {
    decimals= 0;
    fix_char_length(2);
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_month>(thd, mem_root, this); }
};


class Item_func_monthname :public Item_str_func
{
  MY_LOCALE *locale;
public:
  Item_func_monthname(THD *thd, Item *a): Item_str_func(thd, a) {}
  const char *func_name() const { return "monthname"; }
  String *val_str(String *str);
  bool fix_length_and_dec();
  bool check_partition_func_processor(void *int_arg) {return TRUE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_monthname>(thd, mem_root, this); }
};


class Item_func_dayofyear :public Item_int_func
{
public:
  Item_func_dayofyear(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "dayofyear"; }
  bool fix_length_and_dec()
  {
    decimals= 0;
    fix_char_length(3);
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_dayofyear>(thd, mem_root, this); }
};


class Item_func_hour :public Item_int_func
{
public:
  Item_func_hour(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "hour"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=2*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_time_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_hour>(thd, mem_root, this); }
};


class Item_func_minute :public Item_int_func
{
public:
  Item_func_minute(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "minute"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=2*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_time_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_minute>(thd, mem_root, this); }
};


class Item_func_quarter :public Item_int_func
{
public:
  Item_func_quarter(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "quarter"; }
  bool fix_length_and_dec()
  {
     decimals=0;
     max_length=1*MY_CHARSET_BIN_MB_MAXLEN;
     maybe_null=1;
     return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_quarter>(thd, mem_root, this); }
};


class Item_func_second :public Item_int_func
{
public:
  Item_func_second(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "second"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=2*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_time_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_second>(thd, mem_root, this); }
};


class Item_func_week :public Item_int_func
{
public:
  Item_func_week(THD *thd, Item *a): Item_int_func(thd, a) {}
  Item_func_week(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "week"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=2*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg)
  {
    if (arg_count == 2)
      return FALSE;
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  bool check_valid_arguments_processor(void *int_arg)
  {
    return arg_count == 2;
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_week>(thd, mem_root, this); }
};

class Item_func_yearweek :public Item_int_func
{
public:
  Item_func_yearweek(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "yearweek"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=6*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_yearweek>(thd, mem_root, this); }
};


class Item_func_year :public Item_int_func
{
public:
  Item_func_year(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "year"; }
  enum_monotonicity_info get_monotonicity_info() const;
  longlong val_int_endpoint(bool left_endp, bool *incl_endp);
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=4*MY_CHARSET_BIN_MB_MAXLEN;
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_year>(thd, mem_root, this); }
};


class Item_func_weekday :public Item_int_func
{
  bool odbc_type;
public:
  Item_func_weekday(THD *thd, Item *a, bool type_arg):
    Item_int_func(thd, a), odbc_type(type_arg) { }
  longlong val_int();
  const char *func_name() const
  {
     return (odbc_type ? "dayofweek" : "weekday");
  }
  bool fix_length_and_dec()
  {
    decimals= 0;
    fix_char_length(1);
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_weekday>(thd, mem_root, this); }
};

class Item_func_dayname :public Item_str_func
{
  MY_LOCALE *locale;
 public:
  Item_func_dayname(THD *thd, Item *a): Item_str_func(thd, a) {}
  const char *func_name() const { return "dayname"; }
  String *val_str(String *str);
  bool fix_length_and_dec();
  bool check_partition_func_processor(void *int_arg) {return TRUE;}
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_date_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_dayname>(thd, mem_root, this); }
};


class Item_func_seconds_hybrid: public Item_func_numhybrid
{
protected:
  virtual enum_field_types arg0_expected_type() const = 0;
public:
  Item_func_seconds_hybrid(THD *thd): Item_func_numhybrid(thd) {}
  Item_func_seconds_hybrid(THD *thd, Item *a): Item_func_numhybrid(thd, a) {}
  bool fix_length_and_dec()
  {
    if (arg_count)
      decimals= args[0]->temporal_precision(arg0_expected_type());
    set_if_smaller(decimals, TIME_SECOND_PART_DIGITS);
    max_length=17 + (decimals ? decimals + 1 : 0);
    maybe_null= true;
    set_handler_by_result_type(decimals ? DECIMAL_RESULT : INT_RESULT);
    return FALSE;
  }
  double real_op() { DBUG_ASSERT(0); return 0; }
  String *str_op(String *str) { DBUG_ASSERT(0); return 0; }
  bool date_op(MYSQL_TIME *ltime, uint fuzzydate) { DBUG_ASSERT(0); return true; }
};


class Item_func_unix_timestamp :public Item_func_seconds_hybrid
{
  bool get_timestamp_value(my_time_t *seconds, ulong *second_part);
protected:
  enum_field_types arg0_expected_type() const { return MYSQL_TYPE_DATETIME; }
public:
  Item_func_unix_timestamp(THD *thd): Item_func_seconds_hybrid(thd) {}
  Item_func_unix_timestamp(THD *thd, Item *a):
    Item_func_seconds_hybrid(thd, a) {}
  const char *func_name() const { return "unix_timestamp"; }
  enum_monotonicity_info get_monotonicity_info() const;
  longlong val_int_endpoint(bool left_endp, bool *incl_endp);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  /*
    UNIX_TIMESTAMP() depends on the current timezone
    (and thus may not be used as a partitioning function)
    when its argument is NOT of the TIMESTAMP type.
  */
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_timestamp_args();
  }
  bool check_vcol_func_processor(void *arg)
  {
    if (arg_count)
      return FALSE;
    return mark_unsupported_function(func_name(), "()", arg, VCOL_TIME_FUNC);
  }
  longlong int_op();
  my_decimal *decimal_op(my_decimal* buf);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_unix_timestamp>(thd, mem_root, this); }
};


class Item_func_time_to_sec :public Item_func_seconds_hybrid
{
protected:
  enum_field_types arg0_expected_type() const { return MYSQL_TYPE_TIME; }
public:
  Item_func_time_to_sec(THD *thd, Item *item):
    Item_func_seconds_hybrid(thd, item) {}
  const char *func_name() const { return "time_to_sec"; }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_time_args();
  }
  longlong int_op();
  my_decimal *decimal_op(my_decimal* buf);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_time_to_sec>(thd, mem_root, this); }
};


class Item_temporal_func: public Item_func
{
  sql_mode_t sql_mode;
public:
  Item_temporal_func(THD *thd): Item_func(thd) {}
  Item_temporal_func(THD *thd, Item *a): Item_func(thd, a) {}
  Item_temporal_func(THD *thd, Item *a, Item *b): Item_func(thd, a, b) {}
  Item_temporal_func(THD *thd, Item *a, Item *b, Item *c): Item_func(thd, a, b, c) {}
  enum Item_result result_type () const { return STRING_RESULT; }
  Item_result cmp_type() const { return TIME_RESULT; }
  String *val_str(String *str);
  longlong val_int() { return val_int_from_date(); }
  double val_real() { return val_real_from_date(); }
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date) { DBUG_ASSERT(0); return 1; }
  my_decimal *val_decimal(my_decimal *decimal_value)
  { return  val_decimal_from_date(decimal_value); }
  Field *create_field_for_create_select(TABLE *table)
  { return tmp_table_field_from_field_type(table, false, false); }
#if MARIADB_VERSION_ID > 100300
#error This code should be removed in 10.3, to use the derived save_in_field()
#else
  int save_in_field(Field *field, bool no_conversions)
  {
    return field_type() == MYSQL_TYPE_TIME ?
           save_time_in_field(field) :
           save_date_in_field(field);
  }
#endif
  bool fix_length_and_dec();
};


/**
  Abstract class for functions returning TIME, DATE, DATETIME or string values,
  whose data type depends on parameters and is set at fix_fields time.
*/
class Item_temporal_hybrid_func: public Item_temporal_func,
                                 public Type_handler_hybrid_field_type
{
protected:
  String ascii_buf; // Conversion buffer
public:
  Item_temporal_hybrid_func(THD *thd, Item *a, Item *b):
    Item_temporal_func(thd, a, b) {}
  enum_field_types field_type() const
  { return Type_handler_hybrid_field_type::field_type(); }
  enum Item_result result_type () const
  { return Type_handler_hybrid_field_type::result_type(); }
  enum Item_result cmp_type () const
  { return Type_handler_hybrid_field_type::cmp_type(); }
  CHARSET_INFO *charset_for_protocol() const
  {
    /*
      Can return TIME, DATE, DATETIME or VARCHAR depending on arguments.
      Send using "binary" when TIME, DATE or DATETIME,
      or using collation.collation when VARCHAR
      (which is fixed from @@collation_connection in fix_length_and_dec).
    */
    DBUG_ASSERT(fixed == 1);
    return Item_temporal_hybrid_func::field_type() == MYSQL_TYPE_STRING ?
           collation.collation : &my_charset_bin;
  }
  /**
    Fix the returned timestamp to match field_type(),
    which is important for val_str().
  */
  bool fix_temporal_type(MYSQL_TIME *ltime);
  /**
    Return string value in ASCII character set.
  */
  String *val_str_ascii(String *str);
  /**
    Return string value in @@character_set_connection.
  */
  String *val_str(String *str)
  {
    return val_str_from_val_str_ascii(str, &ascii_buf);
  }
};


class Item_datefunc :public Item_temporal_func
{
public:
  Item_datefunc(THD *thd): Item_temporal_func(thd) { }
  Item_datefunc(THD *thd, Item *a): Item_temporal_func(thd, a) { }
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
};


class Item_timefunc :public Item_temporal_func
{
public:
  Item_timefunc(THD *thd): Item_temporal_func(thd) {}
  Item_timefunc(THD *thd, Item *a): Item_temporal_func(thd, a) {}
  Item_timefunc(THD *thd, Item *a, Item *b): Item_temporal_func(thd, a, b) {}
  Item_timefunc(THD *thd, Item *a, Item *b, Item *c):
    Item_temporal_func(thd, a, b ,c) {}
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
};


class Item_datetimefunc :public Item_temporal_func
{
public:
  Item_datetimefunc(THD *thd): Item_temporal_func(thd) {}
  Item_datetimefunc(THD *thd, Item *a): Item_temporal_func(thd, a) {}
  Item_datetimefunc(THD *thd, Item *a, Item *b, Item *c):
    Item_temporal_func(thd, a, b ,c) {}
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
};


/* Abstract CURTIME function. Children should define what time zone is used */

class Item_func_curtime :public Item_timefunc
{
  MYSQL_TIME ltime;
  query_id_t last_query_id;
public:
  Item_func_curtime(THD *thd, uint dec): Item_timefunc(thd), last_query_id(0)
  { decimals= dec; }
  bool fix_fields(THD *, Item **);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  /* 
    Abstract method that defines which time zone is used for conversion.
    Converts time current time in my_time_t representation to broken-down
    MYSQL_TIME representation using UTC-SYSTEM or per-thread time zone.
  */
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)=0;
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_TIME_FUNC);
  }
  void print(String *str, enum_query_type query_type);
};


class Item_func_curtime_local :public Item_func_curtime
{
public:
  Item_func_curtime_local(THD *thd, uint dec): Item_func_curtime(thd, dec) {}
  const char *func_name() const { return "curtime"; }
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_curtime_local>(thd, mem_root, this); }
};


class Item_func_curtime_utc :public Item_func_curtime
{
public:
  Item_func_curtime_utc(THD *thd, uint dec): Item_func_curtime(thd, dec) {}
  const char *func_name() const { return "utc_time"; }
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_curtime_utc>(thd, mem_root, this); }
};


/* Abstract CURDATE function. See also Item_func_curtime. */

class Item_func_curdate :public Item_datefunc
{
  query_id_t last_query_id;
  MYSQL_TIME ltime;
public:
  Item_func_curdate(THD *thd): Item_datefunc(thd), last_query_id(0) {}
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)=0;
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_TIME_FUNC);
  }
};


class Item_func_curdate_local :public Item_func_curdate
{
public:
  Item_func_curdate_local(THD *thd): Item_func_curdate(thd) {}
  const char *func_name() const { return "curdate"; }
  void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_curdate_local>(thd, mem_root, this); }
};


class Item_func_curdate_utc :public Item_func_curdate
{
public:
  Item_func_curdate_utc(THD *thd): Item_func_curdate(thd) {}
  const char *func_name() const { return "utc_date"; }
  void store_now_in_TIME(THD* thd, MYSQL_TIME *now_time);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_curdate_utc>(thd, mem_root, this); }
};


/* Abstract CURRENT_TIMESTAMP function. See also Item_func_curtime */

class Item_func_now :public Item_datetimefunc
{
  MYSQL_TIME ltime;
  query_id_t last_query_id;
public:
  Item_func_now(THD *thd, uint dec): Item_datetimefunc(thd), last_query_id(0)
  { decimals= dec; }
  bool fix_fields(THD *, Item **);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)=0;
  bool check_vcol_func_processor(void *arg)
  {
    /*
      NOW is safe for replication as slaves will run with same time as
      master
    */
    return mark_unsupported_function(func_name(), "()", arg, VCOL_TIME_FUNC);
  }
  void print(String *str, enum_query_type query_type);
};


class Item_func_now_local :public Item_func_now
{
public:
  Item_func_now_local(THD *thd, uint dec): Item_func_now(thd, dec) {}
  const char *func_name() const { return "current_timestamp"; }
  int save_in_field(Field *field, bool no_conversions);
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time);
  virtual enum Functype functype() const { return NOW_FUNC; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_now_local>(thd, mem_root, this); }
};


class Item_func_now_utc :public Item_func_now
{
public:
  Item_func_now_utc(THD *thd, uint dec): Item_func_now(thd, dec) {}
  const char *func_name() const { return "utc_timestamp"; }
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time);
  virtual enum Functype functype() const { return NOW_UTC_FUNC; }
  virtual bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_TIME_FUNC | VCOL_NON_DETERMINISTIC);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_now_utc>(thd, mem_root, this); }
};


/*
  This is like NOW(), but always uses the real current time, not the
  query_start(). This matches the Oracle behavior.
*/
class Item_func_sysdate_local :public Item_func_now
{
public:
  Item_func_sysdate_local(THD *thd, uint dec): Item_func_now(thd, dec) {}
  bool const_item() const { return 0; }
  const char *func_name() const { return "sysdate"; }
  void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  table_map used_tables() const { return RAND_TABLE_BIT; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_TIME_FUNC | VCOL_NON_DETERMINISTIC);
  }
  virtual enum Functype functype() const { return SYSDATE_FUNC; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_sysdate_local>(thd, mem_root, this); }
};


class Item_func_from_days :public Item_datefunc
{
public:
  Item_func_from_days(THD *thd, Item *a): Item_datefunc(thd, a) {}
  const char *func_name() const { return "from_days"; }
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return has_date_args() || has_time_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_from_days>(thd, mem_root, this); }
};


class Item_func_date_format :public Item_str_func
{
  MY_LOCALE *locale;
  int fixed_length;
  const bool is_time_format;
  String value;
public:
  Item_func_date_format(THD *thd, Item *a, Item *b, bool is_time_format_arg):
    Item_str_func(thd, a, b), is_time_format(is_time_format_arg) {}
  String *val_str(String *str);
  const char *func_name() const
    { return is_time_format ? "time_format" : "date_format"; }
  bool fix_length_and_dec();
  uint format_length(const String *format);
  bool eq(const Item *item, bool binary_cmp) const;
  bool check_vcol_func_processor(void *arg)
  {
    if (is_time_format)
      return false;
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_date_format>(thd, mem_root, this); }
};


class Item_func_from_unixtime :public Item_datetimefunc
{
  Time_zone *tz;
 public:
  Item_func_from_unixtime(THD *thd, Item *a): Item_datetimefunc(thd, a) {}
  const char *func_name() const { return "from_unixtime"; }
  bool fix_length_and_dec();
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_from_unixtime>(thd, mem_root, this); }
};


/* 
  We need Time_zone class declaration for storing pointers in
  Item_func_convert_tz.
*/
class Time_zone;

/*
  This class represents CONVERT_TZ() function.
  The important fact about this function that it is handled in special way.
  When such function is met in expression time_zone system tables are added
  to global list of tables to open, so later those already opened and locked
  tables can be used during this function calculation for loading time zone
  descriptions.
*/
class Item_func_convert_tz :public Item_datetimefunc
{
  /*
    If time zone parameters are constants we are caching objects that
    represent them (we use separate from_tz_cached/to_tz_cached members
    to indicate this fact, since NULL is legal value for from_tz/to_tz
    members.
  */
  bool from_tz_cached, to_tz_cached;
  Time_zone *from_tz, *to_tz;
 public:
  Item_func_convert_tz(THD *thd, Item *a, Item *b, Item *c):
    Item_datetimefunc(thd, a, b, c), from_tz_cached(0), to_tz_cached(0) {}
  const char *func_name() const { return "convert_tz"; }
  bool fix_length_and_dec();
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  void cleanup();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_convert_tz>(thd, mem_root, this); }
};


class Item_func_sec_to_time :public Item_timefunc
{
public:
  Item_func_sec_to_time(THD *thd, Item *item): Item_timefunc(thd, item) {}
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  bool fix_length_and_dec()
  {
    decimals= MY_MIN(args[0]->decimals, TIME_SECOND_PART_DIGITS);
    return Item_timefunc::fix_length_and_dec();
  }
  const char *func_name() const { return "sec_to_time"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_sec_to_time>(thd, mem_root, this); }
};


class Item_date_add_interval :public Item_temporal_hybrid_func
{
public:
  const interval_type int_type; // keep it public
  const bool date_sub_interval; // keep it public
  Item_date_add_interval(THD *thd, Item *a, Item *b, interval_type type_arg,
                         bool neg_arg):
    Item_temporal_hybrid_func(thd, a, b),int_type(type_arg),
    date_sub_interval(neg_arg) {}
  const char *func_name() const { return "date_add_interval"; }
  bool fix_length_and_dec();
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  bool eq(const Item *item, bool binary_cmp) const;
  void print(String *str, enum_query_type query_type);
  enum precedence precedence() const { return INTERVAL_PRECEDENCE; }
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_date_add_interval>(thd, mem_root, this); }
};


class Item_extract :public Item_int_func
{
  bool date_value;
  void set_date_length(uint32 length)
  {
    /*
      Although DATE components (e.g. YEAR, YEAR_MONTH, QUARTER, MONTH, WEEK)
      cannot have a sign, we should probably still add +1,
      because all around the code we assume that max_length is sign inclusive.
      Another options is to set unsigned_flag to "true".
    */
    max_length= length; //QQ: see above
    date_value= true;
  }
  void set_time_length(uint32 length)
  {
    max_length= length + 1/*sign*/;
    date_value= false;
  }
 public:
  const interval_type int_type; // keep it public
  Item_extract(THD *thd, interval_type type_arg, Item *a):
    Item_int_func(thd, a), int_type(type_arg) {}
  enum_field_types field_type() const
  {
    switch (int_type) {
    case INTERVAL_YEAR:
    case INTERVAL_YEAR_MONTH:
    case INTERVAL_QUARTER:
    case INTERVAL_MONTH:
    case INTERVAL_WEEK:
    case INTERVAL_DAY:
    case INTERVAL_DAY_HOUR:
    case INTERVAL_DAY_MINUTE:
    case INTERVAL_DAY_SECOND:
    case INTERVAL_HOUR:
    case INTERVAL_HOUR_MINUTE:
    case INTERVAL_HOUR_SECOND:
    case INTERVAL_MINUTE:
    case INTERVAL_MINUTE_SECOND:
    case INTERVAL_SECOND:
    case INTERVAL_MICROSECOND:
    case INTERVAL_SECOND_MICROSECOND:
      return MYSQL_TYPE_LONG;
    case INTERVAL_DAY_MICROSECOND:
    case INTERVAL_HOUR_MICROSECOND:
    case INTERVAL_MINUTE_MICROSECOND:
      return MYSQL_TYPE_LONGLONG;
    case INTERVAL_LAST:
      break;
    }
    DBUG_ASSERT(0);
    return MYSQL_TYPE_LONGLONG;
  }
  longlong val_int();
  enum Functype functype() const { return EXTRACT_FUNC; }
  const char *func_name() const { return "extract"; }
  bool fix_length_and_dec();
  bool eq(const Item *item, bool binary_cmp) const;
  void print(String *str, enum_query_type query_type);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg)
  {
    if (int_type != INTERVAL_WEEK)
      return FALSE;
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  bool check_valid_arguments_processor(void *int_arg)
  {
    switch (int_type) {
    case INTERVAL_YEAR:
    case INTERVAL_YEAR_MONTH:
    case INTERVAL_QUARTER:
    case INTERVAL_MONTH:
    /* case INTERVAL_WEEK: Not allowed as partitioning function, bug#57071 */
    case INTERVAL_DAY:
      return !has_date_args();
    case INTERVAL_DAY_HOUR:
    case INTERVAL_DAY_MINUTE:
    case INTERVAL_DAY_SECOND:
    case INTERVAL_DAY_MICROSECOND:
      return !has_datetime_args();
    case INTERVAL_HOUR:
    case INTERVAL_HOUR_MINUTE:
    case INTERVAL_HOUR_SECOND:
    case INTERVAL_MINUTE:
    case INTERVAL_MINUTE_SECOND:
    case INTERVAL_SECOND:
    case INTERVAL_MICROSECOND:
    case INTERVAL_HOUR_MICROSECOND:
    case INTERVAL_MINUTE_MICROSECOND:
    case INTERVAL_SECOND_MICROSECOND:
      return !has_time_args();
    default:
      /*
        INTERVAL_LAST is only an end marker,
        INTERVAL_WEEK depends on default_week_format which is a session
        variable and cannot be used for partitioning. See bug#57071.
      */
      break;
    }
    return true;
  }
  Field *create_field_for_create_select(TABLE *table)
  { return tmp_table_field_from_field_type(table, false, false); }

  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_extract>(thd, mem_root, this); }
};


class Item_char_typecast :public Item_str_func
{
  uint cast_length;
  CHARSET_INFO *cast_cs, *from_cs;
  bool charset_conversion;
  String tmp_value;
  bool has_explicit_length() const { return cast_length != ~0U; }
  String *reuse(String *src, uint32 length);
  String *copy(String *src, CHARSET_INFO *cs);
  uint adjusted_length_with_warn(uint length);
  void check_truncation_with_warn(String *src, uint dstlen);
public:
  Item_char_typecast(THD *thd, Item *a, uint length_arg, CHARSET_INFO *cs_arg):
    Item_str_func(thd, a), cast_length(length_arg), cast_cs(cs_arg) {}
  enum Functype functype() const { return CHAR_TYPECAST_FUNC; }
  bool eq(const Item *item, bool binary_cmp) const;
  const char *func_name() const { return "cast_as_char"; }
  String *val_str(String *a);
  bool fix_length_and_dec();
  void print(String *str, enum_query_type query_type);
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_char_typecast>(thd, mem_root, this); }
};


class Item_temporal_typecast: public Item_temporal_func
{
public:
  Item_temporal_typecast(THD *thd, Item *a): Item_temporal_func(thd, a) {}
  virtual const char *cast_type() const = 0;
  void print(String *str, enum_query_type query_type);
  bool fix_length_and_dec()
  {
    if (decimals == NOT_FIXED_DEC)
      decimals= args[0]->temporal_precision(field_type());
    return Item_temporal_func::fix_length_and_dec();
  }
};

class Item_date_typecast :public Item_temporal_typecast
{
public:
  Item_date_typecast(THD *thd, Item *a): Item_temporal_typecast(thd, a) {}
  const char *func_name() const { return "cast_as_date"; }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  const char *cast_type() const { return "date"; }
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_date_typecast>(thd, mem_root, this); }
};


class Item_time_typecast :public Item_temporal_typecast
{
public:
  Item_time_typecast(THD *thd, Item *a, uint dec_arg):
    Item_temporal_typecast(thd, a) { decimals= dec_arg; }
  const char *func_name() const { return "cast_as_time"; }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  const char *cast_type() const { return "time"; }
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_time_typecast>(thd, mem_root, this); }
};


class Item_datetime_typecast :public Item_temporal_typecast
{
public:
  Item_datetime_typecast(THD *thd, Item *a, uint dec_arg):
    Item_temporal_typecast(thd, a) { decimals= dec_arg; }
  const char *func_name() const { return "cast_as_datetime"; }
  const char *cast_type() const { return "datetime"; }
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_datetime_typecast>(thd, mem_root, this); }
};


class Item_func_makedate :public Item_temporal_func
{
public:
  Item_func_makedate(THD *thd, Item *a, Item *b):
    Item_temporal_func(thd, a, b) {}
  const char *func_name() const { return "makedate"; }
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_makedate>(thd, mem_root, this); }
};


class Item_func_add_time :public Item_temporal_hybrid_func
{
  const bool is_date;
  int sign;

public:
  Item_func_add_time(THD *thd, Item *a, Item *b, bool type_arg, bool neg_arg):
    Item_temporal_hybrid_func(thd, a, b), is_date(type_arg)
  { sign= neg_arg ? -1 : 1; }
  bool fix_length_and_dec();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  void print(String *str, enum_query_type query_type);
  const char *func_name() const { return "add_time"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_add_time>(thd, mem_root, this); }
};

class Item_func_timediff :public Item_timefunc
{
public:
  Item_func_timediff(THD *thd, Item *a, Item *b): Item_timefunc(thd, a, b) {}
  const char *func_name() const { return "timediff"; }
  bool fix_length_and_dec()
  {
    decimals= MY_MAX(args[0]->temporal_precision(MYSQL_TYPE_TIME),
                     args[1]->temporal_precision(MYSQL_TYPE_TIME));
    return Item_timefunc::fix_length_and_dec();
  }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_timediff>(thd, mem_root, this); }
};

class Item_func_maketime :public Item_timefunc
{
public:
  Item_func_maketime(THD *thd, Item *a, Item *b, Item *c):
    Item_timefunc(thd, a, b, c)
  {}
  bool fix_length_and_dec()
  {
    decimals= MY_MIN(args[2]->decimals, TIME_SECOND_PART_DIGITS);
    return Item_timefunc::fix_length_and_dec();
  }
  const char *func_name() const { return "maketime"; }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_maketime>(thd, mem_root, this); }
};


class Item_func_microsecond :public Item_int_func
{
public:
  Item_func_microsecond(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "microsecond"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    maybe_null=1;
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_time_args();
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_microsecond>(thd, mem_root, this); }
};


class Item_func_timestamp_diff :public Item_int_func
{
  const interval_type int_type;
public:
  Item_func_timestamp_diff(THD *thd, Item *a, Item *b, interval_type type_arg):
    Item_int_func(thd, a, b), int_type(type_arg) {}
  const char *func_name() const { return "timestampdiff"; }
  longlong val_int();
  bool fix_length_and_dec()
  {
    decimals=0;
    maybe_null=1;
    return FALSE;
  }
  virtual void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_timestamp_diff>(thd, mem_root, this); }
};


enum date_time_format
{
  USA_FORMAT, JIS_FORMAT, ISO_FORMAT, EUR_FORMAT, INTERNAL_FORMAT
};

class Item_func_get_format :public Item_str_ascii_func
{
public:
  const timestamp_type type; // keep it public
  Item_func_get_format(THD *thd, timestamp_type type_arg, Item *a):
    Item_str_ascii_func(thd, a), type(type_arg)
  {}
  String *val_str_ascii(String *str);
  const char *func_name() const { return "get_format"; }
  bool fix_length_and_dec()
  {
    maybe_null= 1;
    decimals=0;
    fix_length_and_charset(17, default_charset());
    return FALSE;
  }
  virtual void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_get_format>(thd, mem_root, this); }
};


class Item_func_str_to_date :public Item_temporal_hybrid_func
{
  timestamp_type cached_timestamp_type;
  bool const_item;
  String subject_converter;
  String format_converter;
  CHARSET_INFO *internal_charset;
public:
  Item_func_str_to_date(THD *thd, Item *a, Item *b):
    Item_temporal_hybrid_func(thd, a, b), const_item(false),
    internal_charset(NULL)
  {}
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzy_date);
  const char *func_name() const { return "str_to_date"; }
  bool fix_length_and_dec();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_str_to_date>(thd, mem_root, this); }
};


class Item_func_last_day :public Item_datefunc
{
public:
  Item_func_last_day(THD *thd, Item *a): Item_datefunc(thd, a) {}
  const char *func_name() const { return "last_day"; }
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_last_day>(thd, mem_root, this); }
};

#endif /* ITEM_TIMEFUNC_INCLUDED */
