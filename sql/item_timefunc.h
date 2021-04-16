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


bool get_interval_value(THD *thd, Item *args,
                        interval_type int_type, INTERVAL *interval);


class Item_long_func_date_field: public Item_long_func
{
  bool check_arguments() const
  { return args[0]->check_type_can_return_date(func_name()); }
public:
  Item_long_func_date_field(THD *thd, Item *a)
   :Item_long_func(thd, a) { }
};


class Item_long_func_time_field: public Item_long_func
{
  bool check_arguments() const
  { return args[0]->check_type_can_return_time(func_name()); }
public:
  Item_long_func_time_field(THD *thd, Item *a)
   :Item_long_func(thd, a) { }
};


class Item_func_period_add :public Item_long_func
{
  bool check_arguments() const
  { return check_argument_types_can_return_int(0, 2); }
public:
  Item_func_period_add(THD *thd, Item *a, Item *b): Item_long_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "period_add"; }
  bool fix_length_and_dec()
  {
    max_length=6*MY_CHARSET_BIN_MB_MAXLEN;
    return FALSE;
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_period_add>(thd, this); }
};


class Item_func_period_diff :public Item_long_func
{
  bool check_arguments() const
  { return check_argument_types_can_return_int(0, 2); }
public:
  Item_func_period_diff(THD *thd, Item *a, Item *b): Item_long_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "period_diff"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    max_length=6*MY_CHARSET_BIN_MB_MAXLEN;
    return FALSE;
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_period_diff>(thd, this); }
};


class Item_func_to_days :public Item_long_func_date_field
{
public:
  Item_func_to_days(THD *thd, Item *a): Item_long_func_date_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_to_days>(thd, this); }
};


class Item_func_to_seconds :public Item_longlong_func
{
  bool check_arguments() const
  { return check_argument_types_can_return_date(0, arg_count); }
public:
  Item_func_to_seconds(THD *thd, Item *a): Item_longlong_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "to_seconds"; }
  bool fix_length_and_dec()
  {
    decimals=0; 
    fix_char_length(12);
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_to_seconds>(thd, this); }
};


class Item_func_dayofmonth :public Item_long_func_date_field
{
public:
  Item_func_dayofmonth(THD *thd, Item *a): Item_long_func_date_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_dayofmonth>(thd, this); }
};


class Item_func_month :public Item_long_func
{
public:
  Item_func_month(THD *thd, Item *a): Item_long_func(thd, a)
  { }
  longlong val_int();
  const char *func_name() const { return "month"; }
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_month>(thd, this); }
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_monthname>(thd, this); }
};


class Item_func_dayofyear :public Item_long_func_date_field
{
public:
  Item_func_dayofyear(THD *thd, Item *a): Item_long_func_date_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_dayofyear>(thd, this); }
};


class Item_func_hour :public Item_long_func_time_field
{
public:
  Item_func_hour(THD *thd, Item *a): Item_long_func_time_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_hour>(thd, this); }
};


class Item_func_minute :public Item_long_func_time_field
{
public:
  Item_func_minute(THD *thd, Item *a): Item_long_func_time_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_minute>(thd, this); }
};


class Item_func_quarter :public Item_long_func_date_field
{
public:
  Item_func_quarter(THD *thd, Item *a): Item_long_func_date_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_quarter>(thd, this); }
};


class Item_func_second :public Item_long_func_time_field
{
public:
  Item_func_second(THD *thd, Item *a): Item_long_func_time_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_second>(thd, this); }
};


class Item_func_week :public Item_long_func
{
  bool check_arguments() const
  {
    return args[0]->check_type_can_return_date(func_name()) ||
           (arg_count > 1 && args[1]->check_type_can_return_int(func_name()));
  }
public:
  Item_func_week(THD *thd, Item *a): Item_long_func(thd, a) {}
  Item_func_week(THD *thd, Item *a, Item *b): Item_long_func(thd, a, b) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_week>(thd, this); }
};

class Item_func_yearweek :public Item_long_func
{
  bool check_arguments() const
  {
    return args[0]->check_type_can_return_date(func_name()) ||
           args[1]->check_type_can_return_int(func_name());
  }
public:
  Item_func_yearweek(THD *thd, Item *a, Item *b)
   :Item_long_func(thd, a, b) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_yearweek>(thd, this); }
};


class Item_func_year :public Item_long_func_date_field
{
public:
  Item_func_year(THD *thd, Item *a): Item_long_func_date_field(thd, a) {}
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_year>(thd, this); }
};


class Item_func_weekday :public Item_long_func
{
  bool odbc_type;
public:
  Item_func_weekday(THD *thd, Item *a, bool type_arg):
    Item_long_func(thd, a), odbc_type(type_arg) { }
  longlong val_int();
  const char *func_name() const
  {
     return (odbc_type ? "dayofweek" : "weekday");
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_weekday>(thd, this); }
};

class Item_func_dayname :public Item_str_func
{
  MY_LOCALE *locale;
 public:
  Item_func_dayname(THD *thd, Item *a): Item_str_func(thd, a) {}
  const char *func_name() const { return "dayname"; }
  String *val_str(String *str);
  const Type_handler *type_handler() const { return &type_handler_varchar; }
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_dayname>(thd, this); }
};


class Item_func_seconds_hybrid: public Item_func_numhybrid
{
public:
  Item_func_seconds_hybrid(THD *thd): Item_func_numhybrid(thd) {}
  Item_func_seconds_hybrid(THD *thd, Item *a): Item_func_numhybrid(thd, a) {}
  void fix_length_and_dec_generic(uint dec)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
    decimals= dec;
    max_length=17 + (decimals ? decimals + 1 : 0);
    maybe_null= true;
    if (decimals)
      set_handler(&type_handler_newdecimal);
    else
      set_handler(type_handler_long_or_longlong());
  }
  double real_op() { DBUG_ASSERT(0); return 0; }
  String *str_op(String *str) { DBUG_ASSERT(0); return 0; }
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    DBUG_ASSERT(0);
    return true;
  }
};


class Item_func_unix_timestamp :public Item_func_seconds_hybrid
{
  bool get_timestamp_value(my_time_t *seconds, ulong *second_part);
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
  bool fix_length_and_dec()
  {
    fix_length_and_dec_generic(arg_count ?
                               args[0]->datetime_precision(current_thd) : 0);
    return FALSE;
  }
  longlong int_op();
  my_decimal *decimal_op(my_decimal* buf);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_unix_timestamp>(thd, this); }
};


class Item_func_time_to_sec :public Item_func_seconds_hybrid
{
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
  bool fix_length_and_dec()
  {
    fix_length_and_dec_generic(args[0]->time_precision(current_thd));
    return FALSE;
  }
  longlong int_op();
  my_decimal *decimal_op(my_decimal* buf);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_time_to_sec>(thd, this); }
};


class Item_datefunc :public Item_func
{
public:
  Item_datefunc(THD *thd): Item_func(thd) { }
  Item_datefunc(THD *thd, Item *a): Item_func(thd, a) { }
  Item_datefunc(THD *thd, Item *a, Item *b): Item_func(thd, a, b) { }
  const Type_handler *type_handler() const { return &type_handler_newdate; }
  longlong val_int() { return Date(this).to_longlong(); }
  double val_real() { return Date(this).to_double(); }
  String *val_str(String *to) { return Date(this).to_string(to); }
  my_decimal *val_decimal(my_decimal *to) { return Date(this).to_decimal(to); }
  bool fix_length_and_dec()
  {
    fix_attributes_date();
    maybe_null= (arg_count > 0);
    return FALSE;
  }
};


class Item_timefunc :public Item_func
{
public:
  Item_timefunc(THD *thd): Item_func(thd) {}
  Item_timefunc(THD *thd, Item *a): Item_func(thd, a) {}
  Item_timefunc(THD *thd, Item *a, Item *b): Item_func(thd, a, b) {}
  Item_timefunc(THD *thd, Item *a, Item *b, Item *c): Item_func(thd, a, b ,c) {}
  const Type_handler *type_handler() const { return &type_handler_time2; }
  longlong val_int() { return Time(this).to_longlong(); }
  double val_real() { return Time(this).to_double(); }
  String *val_str(String *to) { return Time(this).to_string(to, decimals); }
  my_decimal *val_decimal(my_decimal *to) { return Time(this).to_decimal(to); }
  bool val_native(THD *thd, Native *to)
  {
    return Time(thd, this).to_native(to, decimals);
  }
};


class Item_datetimefunc :public Item_func
{
public:
  Item_datetimefunc(THD *thd): Item_func(thd) {}
  Item_datetimefunc(THD *thd, Item *a): Item_func(thd, a) {}
  Item_datetimefunc(THD *thd, Item *a, Item *b): Item_func(thd, a, b) {}
  Item_datetimefunc(THD *thd, Item *a, Item *b, Item *c):
    Item_func(thd, a, b ,c) {}
  const Type_handler *type_handler() const { return &type_handler_datetime2; }
  longlong val_int() { return Datetime(this).to_longlong(); }
  double val_real() { return Datetime(this).to_double(); }
  String *val_str(String *to) { return Datetime(this).to_string(to, decimals); }
  my_decimal *val_decimal(my_decimal *to) { return Datetime(this).to_decimal(to); }
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
  bool fix_length_and_dec() { fix_attributes_time(decimals); return FALSE; }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_curtime_local>(thd, this); }
};


class Item_func_curtime_utc :public Item_func_curtime
{
public:
  Item_func_curtime_utc(THD *thd, uint dec): Item_func_curtime(thd, dec) {}
  const char *func_name() const { return "utc_time"; }
  virtual void store_now_in_TIME(THD *thd, MYSQL_TIME *now_time);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_curtime_utc>(thd, this); }
};


/* Abstract CURDATE function. See also Item_func_curtime. */

class Item_func_curdate :public Item_datefunc
{
  query_id_t last_query_id;
  MYSQL_TIME ltime;
public:
  Item_func_curdate(THD *thd): Item_datefunc(thd), last_query_id(0) {}
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_curdate_local>(thd, this); }
};


class Item_func_curdate_utc :public Item_func_curdate
{
public:
  Item_func_curdate_utc(THD *thd): Item_func_curdate(thd) {}
  const char *func_name() const { return "utc_date"; }
  void store_now_in_TIME(THD* thd, MYSQL_TIME *now_time);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_curdate_utc>(thd, this); }
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
  bool fix_length_and_dec()
  { fix_attributes_datetime(decimals); return FALSE;}
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_now_local>(thd, this); }
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_now_utc>(thd, this); }
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
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
  table_map used_tables() const { return RAND_TABLE_BIT; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_TIME_FUNC | VCOL_NON_DETERMINISTIC);
  }
  virtual enum Functype functype() const { return SYSDATE_FUNC; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_sysdate_local>(thd, this); }
};


class Item_func_from_days :public Item_datefunc
{
  bool check_arguments() const
  { return args[0]->check_type_can_return_int(func_name()); }
public:
  Item_func_from_days(THD *thd, Item *a): Item_datefunc(thd, a) {}
  const char *func_name() const { return "from_days"; }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return has_date_args() || has_time_args();
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_from_days>(thd, this); }
};


class Item_func_date_format :public Item_str_func
{
  bool check_arguments() const
  {
    return args[0]->check_type_can_return_date(func_name()) ||
           check_argument_types_can_return_text(1, arg_count);
  }
  const MY_LOCALE *locale;
  int fixed_length;
  String value;
protected:
  bool is_time_format;
public:
  Item_func_date_format(THD *thd, Item *a, Item *b):
    Item_str_func(thd, a, b), locale(0), is_time_format(false) {}
  Item_func_date_format(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c), locale(0), is_time_format(false) {}
  String *val_str(String *str);
  const char *func_name() const { return "date_format"; }
  bool fix_length_and_dec();
  uint format_length(const String *format);
  bool eq(const Item *item, bool binary_cmp) const;
  bool check_vcol_func_processor(void *arg)
  {
    if (arg_count > 2)
      return false;
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_date_format>(thd, this); }
};

class Item_func_time_format: public Item_func_date_format
{
public:
  Item_func_time_format(THD *thd, Item *a, Item *b):
    Item_func_date_format(thd, a, b) { is_time_format= true; }
  const char *func_name() const { return "time_format"; }
  bool check_vcol_func_processor(void *arg) { return false; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_time_format>(thd, this); }
};


class Item_func_from_unixtime :public Item_datetimefunc
{
  bool check_arguments() const
  { return args[0]->check_type_can_return_decimal(func_name()); }
  Time_zone *tz;
 public:
  Item_func_from_unixtime(THD *thd, Item *a): Item_datetimefunc(thd, a) {}
  const char *func_name() const { return "from_unixtime"; }
  bool fix_length_and_dec();
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_from_unixtime>(thd, this); }
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
  bool check_arguments() const
  {
    return args[0]->check_type_can_return_date(func_name()) ||
           check_argument_types_can_return_text(1, arg_count);
  }
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
  bool fix_length_and_dec()
  {
    fix_attributes_datetime(args[0]->datetime_precision(current_thd));
    maybe_null= true;
    return FALSE;
  }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
  void cleanup();
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_convert_tz>(thd, this); }
};


class Item_func_sec_to_time :public Item_timefunc
{
  bool check_arguments() const
  { return args[0]->check_type_can_return_decimal(func_name()); }
public:
  Item_func_sec_to_time(THD *thd, Item *item): Item_timefunc(thd, item) {}
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
  bool fix_length_and_dec()
  {
    fix_attributes_time(args[0]->decimals);
    maybe_null= true;
    return FALSE;
  }
  const char *func_name() const { return "sec_to_time"; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_sec_to_time>(thd, this); }
};


class Item_date_add_interval :public Item_handled_func
{
public:
  const interval_type int_type; // keep it public
  const bool date_sub_interval; // keep it public
  Item_date_add_interval(THD *thd, Item *a, Item *b, interval_type type_arg,
                         bool neg_arg):
    Item_handled_func(thd, a, b), int_type(type_arg),
    date_sub_interval(neg_arg) {}
  const char *func_name() const { return "date_add_interval"; }
  bool fix_length_and_dec();
  bool eq(const Item *item, bool binary_cmp) const;
  void print(String *str, enum_query_type query_type);
  enum precedence precedence() const { return INTERVAL_PRECEDENCE; }
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_date_add_interval>(thd, this); }
};


class Item_extract :public Item_int_func,
                    public Type_handler_hybrid_field_type
{
  date_mode_t m_date_mode;
  const Type_handler_int_result *handler_by_length(uint32 length,
                                                   uint32 threashold)
  {
    if (length >= threashold)
      return &type_handler_slonglong;
    return &type_handler_slong;
  }
  void set_date_length(uint32 length)
  {
    /*
      Although DATE components (e.g. YEAR, YEAR_MONTH, QUARTER, MONTH, WEEK)
      cannot have a sign, we should probably still add +1,
      because all around the code we assume that max_length is sign inclusive.
      Another options is to set unsigned_flag to "true".
    */
    set_handler(handler_by_length(max_length= length, 10)); // QQ: see above
    m_date_mode= date_mode_t(0);
  }
  void set_day_length(uint32 length)
  {
    /*
      Units starting with DAY can be negative:
        EXTRACT(DAY FROM '-24:00:00') -> -1
    */
    set_handler(handler_by_length(max_length= length + 1/*sign*/, 11));
    m_date_mode= Temporal::Options(TIME_INTERVAL_DAY, current_thd);
  }
  void set_time_length(uint32 length)
  {
    set_handler(handler_by_length(max_length= length + 1/*sign*/, 11));
    m_date_mode= Temporal::Options(TIME_INTERVAL_hhmmssff, current_thd);
  }
 public:
  const interval_type int_type; // keep it public
  Item_extract(THD *thd, interval_type type_arg, Item *a):
    Item_int_func(thd, a),
    Type_handler_hybrid_field_type(&type_handler_slonglong),
    m_date_mode(date_mode_t(0)),
    int_type(type_arg)
  { }
  const Type_handler *type_handler() const
  {
    return Type_handler_hybrid_field_type::type_handler();
  }
  longlong val_int();
  enum Functype functype() const { return EXTRACT_FUNC; }
  const char *func_name() const { return "extract"; }
  bool check_arguments() const;
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_extract>(thd, this); }
};


class Item_char_typecast :public Item_handled_func
{
  uint cast_length;
  CHARSET_INFO *cast_cs, *from_cs;
  bool charset_conversion;
  String tmp_value;
  bool m_suppress_warning_to_error_escalation;
public:
  bool has_explicit_length() const { return cast_length != ~0U; }
private:
  String *reuse(String *src, size_t length);
  String *copy(String *src, CHARSET_INFO *cs);
  uint adjusted_length_with_warn(uint length);
  void check_truncation_with_warn(String *src, size_t dstlen);
  void fix_length_and_dec_internal(CHARSET_INFO *fromcs);
public:
  // Methods used by ColumnStore
  uint get_cast_length() const { return cast_length; }
public:
  Item_char_typecast(THD *thd, Item *a, uint length_arg, CHARSET_INFO *cs_arg):
    Item_handled_func(thd, a), cast_length(length_arg), cast_cs(cs_arg),
    m_suppress_warning_to_error_escalation(false) {}
  enum Functype functype() const { return CHAR_TYPECAST_FUNC; }
  bool eq(const Item *item, bool binary_cmp) const;
  const char *func_name() const { return "cast_as_char"; }
  CHARSET_INFO *cast_charset() const { return cast_cs; }
  String *val_str_generic(String *a);
  String *val_str_binary_from_native(String *a);
  void fix_length_and_dec_generic();
  void fix_length_and_dec_numeric();
  void fix_length_and_dec_str();
  void fix_length_and_dec_native_to_binary(uint32 octet_length);
  bool fix_length_and_dec()
  {
    return args[0]->type_handler()->Item_char_typecast_fix_length_and_dec(this);
  }
  void print(String *str, enum_query_type query_type);
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_char_typecast>(thd, this); }
};


class Item_interval_DDhhmmssff_typecast :public Item_char_typecast
{
  uint m_fsp;
public:
  Item_interval_DDhhmmssff_typecast(THD *thd, Item *a, uint fsp)
   :Item_char_typecast(thd, a,Interval_DDhhmmssff::max_char_length(fsp),
                       &my_charset_latin1),
    m_fsp(fsp)
  { }
  String *val_str(String *to)
  {
    Interval_DDhhmmssff it(current_thd, args[0], m_fsp);
    null_value= !it.is_valid_interval_DDhhmmssff();
    return it.to_string(to, m_fsp);
  }
};


class Item_date_typecast :public Item_datefunc
{
public:
  Item_date_typecast(THD *thd, Item *a): Item_datefunc(thd, a) {}
  const char *func_name() const { return "cast_as_date"; }
  void print(String *str, enum_query_type query_type)
  {
    print_cast_temporal(str, query_type);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  bool fix_length_and_dec()
  {
    return args[0]->type_handler()->Item_date_typecast_fix_length_and_dec(this);
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_date_typecast>(thd, this); }
};


class Item_time_typecast :public Item_timefunc
{
public:
  Item_time_typecast(THD *thd, Item *a, uint dec_arg):
    Item_timefunc(thd, a) { decimals= dec_arg; }
  const char *func_name() const { return "cast_as_time"; }
  void print(String *str, enum_query_type query_type)
  {
    print_cast_temporal(str, query_type);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  bool fix_length_and_dec()
  {
    return args[0]->type_handler()->
           Item_time_typecast_fix_length_and_dec(this);
  }
  Sql_mode_dependency value_depends_on_sql_mode() const;
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_time_typecast>(thd, this); }
};


class Item_datetime_typecast :public Item_datetimefunc
{
public:
  Item_datetime_typecast(THD *thd, Item *a, uint dec_arg):
    Item_datetimefunc(thd, a) { decimals= dec_arg; }
  const char *func_name() const { return "cast_as_datetime"; }
  void print(String *str, enum_query_type query_type)
  {
    print_cast_temporal(str, query_type);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  bool fix_length_and_dec()
  {
    return args[0]->type_handler()->
           Item_datetime_typecast_fix_length_and_dec(this);
  }
  Sql_mode_dependency value_depends_on_sql_mode() const;
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_datetime_typecast>(thd, this); }
};


class Item_func_makedate :public Item_datefunc
{
  bool check_arguments() const
  { return check_argument_types_can_return_int(0, arg_count); }
public:
  Item_func_makedate(THD *thd, Item *a, Item *b):
    Item_datefunc(thd, a, b) {}
  const char *func_name() const { return "makedate"; }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_makedate>(thd, this); }
};


class Item_func_timestamp :public Item_datetimefunc
{
  bool check_arguments() const
  {
    return args[0]->check_type_can_return_date(func_name()) ||
           args[1]->check_type_can_return_time(func_name());
  }
public:
  Item_func_timestamp(THD *thd, Item *a, Item *b)
   :Item_datetimefunc(thd, a, b)
  { }
  const char *func_name() const { return "timestamp"; }
  bool fix_length_and_dec()
  {
    THD *thd= current_thd;
    uint dec0= args[0]->datetime_precision(thd);
    uint dec1= Interval_DDhhmmssff::fsp(thd, args[1]);
    fix_attributes_datetime(MY_MAX(dec0, dec1));
    maybe_null= true;
    return false;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    Datetime dt(thd, args[0], Datetime::Options(TIME_CONV_NONE, thd));
    if (!dt.is_valid_datetime())
      return null_value= true;
    Interval_DDhhmmssff it(thd, args[1]);
    if (!it.is_valid_interval_DDhhmmssff())
      return null_value= true;
    return (null_value= Sec6_add(dt.get_mysql_time(), it.get_mysql_time(), 1).
                           to_datetime(ltime));
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_timestamp>(thd, this); }
};


/**
  ADDTIME(t,a) and SUBTIME(t,a) are time functions that calculate a
  time/datetime value

  t: time_or_datetime_expression
  a: time_expression

  Result: Time value or datetime value
*/

class Item_func_add_time :public Item_handled_func
{
  int sign;
public:
  // Methods used by ColumnStore
  int get_sign() const { return sign; }
public:
  Item_func_add_time(THD *thd, Item *a, Item *b, bool neg_arg)
   :Item_handled_func(thd, a, b), sign(neg_arg ? -1 : 1)
  { }
  bool fix_length_and_dec();
  const char *func_name() const { return sign > 0 ? "addtime" : "subtime"; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_add_time>(thd, this); }
};


class Item_func_timediff :public Item_timefunc
{
  bool check_arguments() const
  { return check_argument_types_can_return_time(0, arg_count); }
public:
  Item_func_timediff(THD *thd, Item *a, Item *b): Item_timefunc(thd, a, b) {}
  const char *func_name() const { return "timediff"; }
  bool fix_length_and_dec()
  {
    THD *thd= current_thd;
    uint dec= MY_MAX(args[0]->time_precision(thd),
                     args[1]->time_precision(thd));
    fix_attributes_time(dec);
    maybe_null= true;
    return FALSE;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_timediff>(thd, this); }
};

class Item_func_maketime :public Item_timefunc
{
  bool check_arguments() const
  {
    return check_argument_types_can_return_int(0, 2) ||
           args[2]->check_type_can_return_decimal(func_name());
  }
public:
  Item_func_maketime(THD *thd, Item *a, Item *b, Item *c):
    Item_timefunc(thd, a, b, c)
  {}
  bool fix_length_and_dec()
  {
    fix_attributes_time(args[2]->decimals);
    maybe_null= true;
    return FALSE;
  }
  const char *func_name() const { return "maketime"; }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_maketime>(thd, this); }
};


class Item_func_microsecond :public Item_long_func_time_field
{
public:
  Item_func_microsecond(THD *thd, Item *a): Item_long_func_time_field(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "microsecond"; }
  bool fix_length_and_dec()
  {
    decimals=0;
    maybe_null=1;
    fix_char_length(6);
    return FALSE;
  }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool check_valid_arguments_processor(void *int_arg)
  {
    return !has_time_args();
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_microsecond>(thd, this); }
};


class Item_func_timestamp_diff :public Item_longlong_func
{
  bool check_arguments() const
  { return check_argument_types_can_return_date(0, arg_count); }
  const interval_type int_type;
public:
  // Methods used by ColumnStore
  interval_type get_int_type() const { return int_type; };
public:
  Item_func_timestamp_diff(THD *thd, Item *a, Item *b, interval_type type_arg):
    Item_longlong_func(thd, a, b), int_type(type_arg) {}
  const char *func_name() const { return "timestampdiff"; }
  longlong val_int();
  bool fix_length_and_dec()
  {
    decimals=0;
    maybe_null=1;
    return FALSE;
  }
  virtual void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_timestamp_diff>(thd, this); }
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
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_get_format>(thd, this); }
};


class Item_func_str_to_date :public Item_handled_func
{
  bool const_item;
  String subject_converter;
  String format_converter;
  CHARSET_INFO *internal_charset;
public:
  Item_func_str_to_date(THD *thd, Item *a, Item *b):
    Item_handled_func(thd, a, b), const_item(false),
    internal_charset(NULL)
  {}
  bool get_date_common(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate,
                       timestamp_type);
  const char *func_name() const { return "str_to_date"; }
  bool fix_length_and_dec();
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_str_to_date>(thd, this); }
};


class Item_func_last_day :public Item_datefunc
{
  bool check_arguments() const
  { return args[0]->check_type_can_return_date(func_name()); }
public:
  Item_func_last_day(THD *thd, Item *a): Item_datefunc(thd, a) {}
  const char *func_name() const { return "last_day"; }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_last_day>(thd, this); }
};


/*****************************************************************************/

class Func_handler_date_add_interval
{
protected:
  static uint interval_dec(const Item *item, interval_type int_type)
  {
    if (int_type == INTERVAL_MICROSECOND ||
        (int_type >= INTERVAL_DAY_MICROSECOND &&
         int_type <= INTERVAL_SECOND_MICROSECOND))
      return TIME_SECOND_PART_DIGITS;
    if (int_type == INTERVAL_SECOND && item->decimals > 0)
      return MY_MIN(item->decimals, TIME_SECOND_PART_DIGITS);
    return 0;
  }
  interval_type int_type(const Item_handled_func *item) const
  {
    return static_cast<const Item_date_add_interval*>(item)->int_type;
  }
  bool sub(const Item_handled_func *item) const
  {
    return static_cast<const Item_date_add_interval*>(item)->date_sub_interval;
  }
  bool add(THD *thd, Item *item, interval_type type, bool sub, MYSQL_TIME *to) const
  {
    INTERVAL interval;
    if (get_interval_value(thd, item, type, &interval))
      return true;
    if (sub)
      interval.neg = !interval.neg;
    return date_add_interval(thd, to, type, interval);
  }
};


class Func_handler_date_add_interval_datetime:
        public Item_handled_func::Handler_datetime,
        public Func_handler_date_add_interval
{
public:
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    uint dec= MY_MAX(item->arguments()[0]->datetime_precision(current_thd),
                     interval_dec(item->arguments()[1], int_type(item)));
    item->fix_attributes_datetime(dec);
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    Datetime::Options opt(TIME_CONV_NONE, thd);
    Datetime dt(thd, item->arguments()[0], opt);
    if (!dt.is_valid_datetime() ||
         dt.check_date_with_warn(thd, TIME_NO_ZERO_DATE | TIME_NO_ZERO_IN_DATE))
      return (item->null_value= true);
    dt.copy_to_mysql_time(to);
    return (item->null_value= add(thd, item->arguments()[1],
                                  int_type(item), sub(item), to));
  }
};


class Func_handler_date_add_interval_datetime_arg0_time:
        public Func_handler_date_add_interval_datetime
{
public:
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const;
};


class Func_handler_date_add_interval_date:
        public Item_handled_func::Handler_date,
        public Func_handler_date_add_interval
{
public:
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    /*
      The first argument is known to be of the DATE data type (not DATETIME).
      We don't need rounding here.
    */
    Date d(thd, item->arguments()[0], TIME_CONV_NONE);
    if (!d.is_valid_date() ||
         d.check_date_with_warn(thd, TIME_NO_ZERO_DATE | TIME_NO_ZERO_IN_DATE))
      return (item->null_value= true);
    d.copy_to_mysql_time(to);
    return (item->null_value= add(thd, item->arguments()[1],
                                  int_type(item), sub(item), to));
  }
};


class Func_handler_date_add_interval_time:
        public Item_handled_func::Handler_time,
        public Func_handler_date_add_interval
{
public:
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    uint dec= MY_MAX(item->arguments()[0]->time_precision(current_thd),
                     interval_dec(item->arguments()[1], int_type(item)));
    item->fix_attributes_time(dec);
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    Time t(thd, item->arguments()[0]);
    if (!t.is_valid_time())
      return (item->null_value= true);
    t.copy_to_mysql_time(to);
    return (item->null_value= add(thd, item->arguments()[1],
                                  int_type(item), sub(item), to));
  }
};


class Func_handler_date_add_interval_string:
        public Item_handled_func::Handler_temporal_string,
        public Func_handler_date_add_interval
{
public:
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    uint dec= MY_MAX(item->arguments()[0]->datetime_precision(current_thd),
                     interval_dec(item->arguments()[1], int_type(item)));
    item->Type_std_attributes::set(
      Type_temporal_attributes_not_fixed_dec(MAX_DATETIME_WIDTH, dec, false),
      DTCollation(item->default_charset(),
                  DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII));
    item->fix_char_length(item->max_length);
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    if (item->arguments()[0]->
          get_date(thd, to, Datetime::Options(TIME_CONV_NONE, thd)) ||
        (to->time_type != MYSQL_TIMESTAMP_TIME &&
         check_date_with_warn(thd, to, TIME_NO_ZEROS, MYSQL_TIMESTAMP_ERROR)))
      return (item->null_value= true);
    return (item->null_value= add(thd, item->arguments()[1],
                                  int_type(item), sub(item), to));
  }
};


class Func_handler_sign
{
protected:
  int m_sign;
  Func_handler_sign(int sign) :m_sign(sign) { }
};


class Func_handler_add_time_datetime:
        public Item_handled_func::Handler_datetime,
        public Func_handler_sign
{
public:
  Func_handler_add_time_datetime(int sign)
   :Func_handler_sign(sign)
  { }
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    THD *thd= current_thd;
    uint dec0= item->arguments()[0]->datetime_precision(thd);
    uint dec1= Interval_DDhhmmssff::fsp(thd, item->arguments()[1]);
    item->fix_attributes_datetime(MY_MAX(dec0, dec1));
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    DBUG_ASSERT(item->is_fixed());
    Datetime::Options opt(TIME_CONV_NONE, thd);
    Datetime dt(thd, item->arguments()[0], opt);
    if (!dt.is_valid_datetime())
      return item->null_value= true;
    Interval_DDhhmmssff it(thd, item->arguments()[1]);
    if (!it.is_valid_interval_DDhhmmssff())
      return item->null_value= true;
    return (item->null_value= (Sec6_add(dt.get_mysql_time(),
                                        it.get_mysql_time(), m_sign).
                                to_datetime(to)));
  }
};


class Func_handler_add_time_time:
        public Item_handled_func::Handler_time,
        public Func_handler_sign
{
public:
  Func_handler_add_time_time(int sign)
   :Func_handler_sign(sign)
  { }
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    THD *thd= current_thd;
    uint dec0= item->arguments()[0]->time_precision(thd);
    uint dec1= Interval_DDhhmmssff::fsp(thd, item->arguments()[1]);
    item->fix_attributes_time(MY_MAX(dec0, dec1));
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    DBUG_ASSERT(item->is_fixed());
    Time t(thd, item->arguments()[0]);
    if (!t.is_valid_time())
      return item->null_value= true;
    Interval_DDhhmmssff i(thd, item->arguments()[1]);
    if (!i.is_valid_interval_DDhhmmssff())
      return item->null_value= true;
    return (item->null_value= (Sec6_add(t.get_mysql_time(),
                                        i.get_mysql_time(), m_sign).
                                 to_time(thd, to, item->decimals)));
  }
};


class Func_handler_add_time_string:
        public Item_handled_func::Handler_temporal_string,
        public Func_handler_sign
{
public:
  Func_handler_add_time_string(int sign)
   :Func_handler_sign(sign)
  { }
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    uint dec0= item->arguments()[0]->decimals;
    uint dec1= Interval_DDhhmmssff::fsp(current_thd, item->arguments()[1]);
    uint dec= MY_MAX(dec0, dec1);
    item->Type_std_attributes::set(
      Type_temporal_attributes_not_fixed_dec(MAX_DATETIME_WIDTH, dec, false),
      DTCollation(item->default_charset(),
                  DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII));
    item->fix_char_length(item->max_length);
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    DBUG_ASSERT(item->is_fixed());
    // Detect a proper timestamp type based on the argument values
    Temporal_hybrid l_time1(thd, item->arguments()[0],
                            Temporal::Options(TIME_TIME_ONLY, thd));
    if (!l_time1.is_valid_temporal())
      return (item->null_value= true);
    Interval_DDhhmmssff l_time2(thd, item->arguments()[1]);
    if (!l_time2.is_valid_interval_DDhhmmssff())
      return (item->null_value= true);
    Sec6_add add(l_time1.get_mysql_time(), l_time2.get_mysql_time(), m_sign);
    return (item->null_value= (l_time1.get_mysql_time()->time_type ==
                                 MYSQL_TIMESTAMP_TIME ?
                               add.to_time(thd, to, item->decimals) :
                               add.to_datetime(to)));
  }
};


class Func_handler_str_to_date_datetime_sec:
        public Item_handled_func::Handler_datetime
{
public:
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    item->fix_attributes_datetime(0);
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    return static_cast<Item_func_str_to_date*>(item)->
             get_date_common(thd, to, fuzzy, MYSQL_TIMESTAMP_DATETIME);
  }
};


class Func_handler_str_to_date_datetime_usec:
        public Item_handled_func::Handler_datetime
{
public:
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    item->fix_attributes_datetime(TIME_SECOND_PART_DIGITS);
    return false;
  }
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    return static_cast<Item_func_str_to_date*>(item)->
             get_date_common(thd, to, fuzzy, MYSQL_TIMESTAMP_DATETIME);
  }
};


class Func_handler_str_to_date_date: public Item_handled_func::Handler_date
{
public:
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    return static_cast<Item_func_str_to_date*>(item)->
             get_date_common(thd, to, fuzzy, MYSQL_TIMESTAMP_DATE);
  }
};


class Func_handler_str_to_date_time: public Item_handled_func::Handler_time
{
public:
  bool get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
  {
    if (static_cast<Item_func_str_to_date*>(item)->
         get_date_common(thd, to, fuzzy, MYSQL_TIMESTAMP_TIME))
      return true;
    if (to->day)
    {
      /*
        Day part for time type can be nonzero value and so
        we should add hours from day part to hour part to
        keep valid time value.
      */
      to->hour+= to->day * 24;
      to->day= 0;
    }
    return false;
  }
};


class Func_handler_str_to_date_time_sec: public Func_handler_str_to_date_time
{
public:
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    item->fix_attributes_time(0);
    return false;
  }
};


class Func_handler_str_to_date_time_usec: public Func_handler_str_to_date_time
{
public:
  bool fix_length_and_dec(Item_handled_func *item) const
  {
    item->fix_attributes_time(TIME_SECOND_PART_DIGITS);
    return false;
  }
};


#endif /* ITEM_TIMEFUNC_INCLUDED */
