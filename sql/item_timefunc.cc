/*
   Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB

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


/**
  @file

  @brief
  This file defines all time functions

  @todo
    Move month and days to language files
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "sql_locale.h"          // MY_LOCALE my_locale_en_US
#include "strfunc.h"             // check_word
#include "sql_type_int.h"        // Longlong_hybrid
#include "sql_time.h"            // make_truncated_value_warning,
                                 // get_date_from_daynr,
                                 // calc_weekday, calc_week,
                                 // convert_month_to_period,
                                 // convert_period_to_month,
                                 // TIME_to_timestamp,
                                 // calc_time_diff,
                                 // calc_time_from_sec,
                                 // get_date_time_format_str
#include "tztime.h"              // struct Time_zone
#include "sql_class.h"           // THD
#include <m_ctype.h>
#include <time.h>

/** Day number for Dec 31st, 9999. */
#define MAX_DAY_NUMBER 3652424L

Func_handler_date_add_interval_datetime_arg0_time
  func_handler_date_add_interval_datetime_arg0_time;

Func_handler_date_add_interval_datetime func_handler_date_add_interval_datetime;
Func_handler_date_add_interval_date     func_handler_date_add_interval_date;
Func_handler_date_add_interval_time     func_handler_date_add_interval_time;
Func_handler_date_add_interval_string   func_handler_date_add_interval_string;

Func_handler_add_time_datetime func_handler_add_time_datetime_add(1);
Func_handler_add_time_datetime func_handler_add_time_datetime_sub(-1);
Func_handler_add_time_time     func_handler_add_time_time_add(1);
Func_handler_add_time_time     func_handler_add_time_time_sub(-1);
Func_handler_add_time_string   func_handler_add_time_string_add(1);
Func_handler_add_time_string   func_handler_add_time_string_sub(-1);

Func_handler_str_to_date_datetime_sec  func_handler_str_to_date_datetime_sec;
Func_handler_str_to_date_datetime_usec func_handler_str_to_date_datetime_usec;
Func_handler_str_to_date_date          func_handler_str_to_date_date;
Func_handler_str_to_date_time_sec      func_handler_str_to_date_time_sec;
Func_handler_str_to_date_time_usec     func_handler_str_to_date_time_usec;


/*
  Date formats corresponding to compound %r and %T conversion specifiers

  Note: We should init at least first element of "positions" array
        (first member) or hpux11 compiler will die horribly.
*/
static DATE_TIME_FORMAT time_ampm_format= {{0}, '\0', 0,
                                           {(char *)"%I:%i:%S %p", 11}};
static DATE_TIME_FORMAT time_24hrs_format= {{0}, '\0', 0,
                                            {(char *)"%H:%i:%S", 8}};

/**
  Extract datetime value to MYSQL_TIME struct from string value
  according to format string.

  @param format		date/time format specification
  @param val			String to decode
  @param length		Length of string
  @param l_time		Store result here
  @param cached_timestamp_type  It uses to get an appropriate warning
                                in the case when the value is truncated.
  @param sub_pattern_end    if non-zero then we are parsing string which
                            should correspond compound specifier (like %T or
                            %r) and this parameter is pointer to place where
                            pointer to end of string matching this specifier
                            should be stored.

  @note
    Possibility to parse strings matching to patterns equivalent to compound
    specifiers is mainly intended for use from inside of this function in
    order to understand %T and %r conversion specifiers, so number of
    conversion specifiers that can be used in such sub-patterns is limited.
    Also most of checks are skipped in this case.

  @note
    If one adds new format specifiers to this function he should also
    consider adding them to get_date_time_result_type() function.

  @retval
    0	ok
  @retval
    1	error
*/

static bool extract_date_time(THD *thd, DATE_TIME_FORMAT *format,
			      const char *val, uint length, MYSQL_TIME *l_time,
                              timestamp_type cached_timestamp_type,
                              const char **sub_pattern_end,
                              const char *date_time_type,
                              date_conv_mode_t fuzzydate)
{
  int weekday= 0, yearday= 0, daypart= 0;
  int week_number= -1;
  int error= 0;
  int  strict_week_number_year= -1;
  int frac_part;
  bool usa_time= 0;
  bool UNINIT_VAR(sunday_first_n_first_week_non_iso);
  bool UNINIT_VAR(strict_week_number);
  bool UNINIT_VAR(strict_week_number_year_type);
  const char *val_begin= val;
  const char *val_end= val + length;
  const char *ptr= format->format.str;
  const char *end= ptr + format->format.length;
  CHARSET_INFO *cs= &my_charset_bin;
  DBUG_ENTER("extract_date_time");

  if (!sub_pattern_end)
    bzero((char*) l_time, sizeof(*l_time));

  l_time->time_type= cached_timestamp_type;

  for (; ptr != end && val != val_end; ptr++)
  {
    /* Skip pre-space between each argument */
    if ((val+= cs->scan(val, val_end, MY_SEQ_SPACES)) >= val_end)
      break;

    if (*ptr == '%' && ptr+1 != end)
    {
      int val_len;
      char *tmp;

      error= 0;

      val_len= (uint) (val_end - val);
      switch (*++ptr) {
	/* Year */
      case 'Y':
	tmp= (char*) val + MY_MIN(4, val_len);
	l_time->year= (int) my_strtoll10(val, &tmp, &error);
        if ((int) (tmp-val) <= 2)
          l_time->year= year_2000_handling(l_time->year);
	val= tmp;
	break;
      case 'y':
	tmp= (char*) val + MY_MIN(2, val_len);
	l_time->year= (int) my_strtoll10(val, &tmp, &error);
	val= tmp;
        l_time->year= year_2000_handling(l_time->year);
	break;

	/* Month */
      case 'm':
      case 'c':
	tmp= (char*) val + MY_MIN(2, val_len);
	l_time->month= (int) my_strtoll10(val, &tmp, &error);
	val= tmp;
	break;
      case 'M':
	if ((l_time->month= check_word(my_locale_en_US.month_names,
				       val, val_end, &val)) <= 0)
	  goto err;
	break;
      case 'b':
	if ((l_time->month= check_word(my_locale_en_US.ab_month_names,
				       val, val_end, &val)) <= 0)
	  goto err;
	break;
	/* Day */
      case 'd':
      case 'e':
	tmp= (char*) val + MY_MIN(2, val_len);
	l_time->day= (int) my_strtoll10(val, &tmp, &error);
	val= tmp;
	break;
      case 'D':
	tmp= (char*) val + MY_MIN(2, val_len);
	l_time->day= (int) my_strtoll10(val, &tmp, &error);
	/* Skip 'st, 'nd, 'th .. */
	val= tmp + MY_MIN((int) (val_end-tmp), 2);
	break;

	/* Hour */
      case 'h':
      case 'I':
      case 'l':
	usa_time= 1;
	/* fall through */
      case 'k':
      case 'H':
	tmp= (char*) val + MY_MIN(2, val_len);
	l_time->hour= (int) my_strtoll10(val, &tmp, &error);
	val= tmp;
	break;

	/* Minute */
      case 'i':
	tmp= (char*) val + MY_MIN(2, val_len);
	l_time->minute= (int) my_strtoll10(val, &tmp, &error);
	val= tmp;
	break;

	/* Second */
      case 's':
      case 'S':
	tmp= (char*) val + MY_MIN(2, val_len);
	l_time->second= (int) my_strtoll10(val, &tmp, &error);
	val= tmp;
	break;

	/* Second part */
      case 'f':
	tmp= (char*) val_end;
	if (tmp - val > 6)
	  tmp= (char*) val + 6;
	l_time->second_part= (int) my_strtoll10(val, &tmp, &error);
	frac_part= 6 - (int) (tmp - val);
	if (frac_part > 0)
	  l_time->second_part*= (ulong) log_10_int[frac_part];
	val= tmp;
	break;

	/* AM / PM */
      case 'p':
	if (val_len < 2 || ! usa_time)
	  goto err;
	if (!my_charset_latin1.strnncoll(val, 2, "PM", 2))
	  daypart= 12;
	else if (my_charset_latin1.strnncoll(val, 2, "AM", 2))
	  goto err;
	val+= 2;
	break;

	/* Exotic things */
      case 'W':
	if ((weekday= check_word(my_locale_en_US.day_names, val, val_end, &val)) <= 0)
	  goto err;
	break;
      case 'a':
	if ((weekday= check_word(my_locale_en_US.ab_day_names, val, val_end, &val)) <= 0)
	  goto err;
	break;
      case 'w':
	tmp= (char*) val + 1;
	if (unlikely((weekday= (int) my_strtoll10(val, &tmp, &error)) < 0 ||
                     weekday >= 7))
	  goto err;
        /* We should use the same 1 - 7 scale for %w as for %W */
        if (!weekday)
          weekday= 7;
	val= tmp;
	break;
      case 'j':
	tmp= (char*) val + MY_MIN(val_len, 3);
	yearday= (int) my_strtoll10(val, &tmp, &error);
	val= tmp;
	break;

        /* Week numbers */
      case 'V':
      case 'U':
      case 'v':
      case 'u':
        sunday_first_n_first_week_non_iso= (*ptr=='U' || *ptr== 'V');
        strict_week_number= (*ptr=='V' || *ptr=='v');
	tmp= (char*) val + MY_MIN(val_len, 2);
	if (unlikely((week_number=
                      (int) my_strtoll10(val, &tmp, &error)) < 0 ||
                     (strict_week_number && !week_number) ||
                     week_number > 53))
          goto err;
	val= tmp;
	break;

        /* Year used with 'strict' %V and %v week numbers */
      case 'X':
      case 'x':
        strict_week_number_year_type= (*ptr=='X');
        tmp= (char*) val + MY_MIN(4, val_len);
        strict_week_number_year= (int) my_strtoll10(val, &tmp, &error);
        val= tmp;
        break;

        /* Time in AM/PM notation */
      case 'r':
        /*
          We can't just set error here, as we don't want to generate two
          warnings in case of errors
        */
        if (extract_date_time(thd, &time_ampm_format, val,
                              (uint)(val_end - val), l_time,
                              cached_timestamp_type, &val, "time", fuzzydate))
          DBUG_RETURN(1);
        break;

        /* Time in 24-hour notation */
      case 'T':
        if (extract_date_time(thd, &time_24hrs_format, val,
                              (uint)(val_end - val), l_time,
                              cached_timestamp_type, &val, "time", fuzzydate))
          DBUG_RETURN(1);
        break;

        /* Conversion specifiers that match classes of characters */
      case '.':
	while (my_ispunct(cs, *val) && val != val_end)
	  val++;
	break;
      case '@':
	while (my_isalpha(cs, *val) && val != val_end)
	  val++;
	break;
      case '#':
	while (my_isdigit(cs, *val) && val != val_end)
	  val++;
	break;
      default:
	goto err;
      }
      if (unlikely(error))                  // Error from my_strtoll10
	goto err;
    }
    else if (!my_isspace(cs, *ptr))
    {
      if (*val != *ptr)
	goto err;
      val++;
    }
  }
  if (usa_time)
  {
    if (l_time->hour > 12 || l_time->hour < 1)
      goto err;
    l_time->hour= l_time->hour%12+daypart;
  }

  /*
    If we are recursively called for parsing string matching compound
    specifiers we are already done.
  */
  if (sub_pattern_end)
  {
    *sub_pattern_end= val;
    DBUG_RETURN(0);
  }

  if (yearday > 0)
  {
    uint days;
    days= calc_daynr(l_time->year,1,1) +  yearday - 1;
    if (get_date_from_daynr(days,&l_time->year,&l_time->month,&l_time->day))
      goto err;
  }

  if (week_number >= 0 && weekday)
  {
    int days;
    uint weekday_b;

    /*
      %V,%v require %X,%x resprectively,
      %U,%u should be used with %Y and not %X or %x
    */
    if ((strict_week_number &&
         (strict_week_number_year < 0 ||
          strict_week_number_year_type !=
          sunday_first_n_first_week_non_iso)) ||
        (!strict_week_number && strict_week_number_year >= 0))
      goto err;

    /* Number of days since year 0 till 1st Jan of this year */
    days= calc_daynr((strict_week_number ? strict_week_number_year :
                                           l_time->year),
                     1, 1);
    /* Which day of week is 1st Jan of this year */
    weekday_b= calc_weekday(days, sunday_first_n_first_week_non_iso);

    /*
      Below we are going to sum:
      1) number of days since year 0 till 1st day of 1st week of this year
      2) number of days between 1st week and our week
      3) and position of our day in the week
    */
    if (sunday_first_n_first_week_non_iso)
    {
      days+= ((weekday_b == 0) ? 0 : 7) - weekday_b +
             (week_number - 1) * 7 +
             weekday % 7;
    }
    else
    {
      days+= ((weekday_b <= 3) ? 0 : 7) - weekday_b +
             (week_number - 1) * 7 +
             (weekday - 1);
    }

    if (get_date_from_daynr(days,&l_time->year,&l_time->month,&l_time->day))
      goto err;
  }

  if (l_time->month > 12 || l_time->day > 31 || l_time->hour > 23 || 
      l_time->minute > 59 || l_time->second > 59)
    goto err;

  int was_cut;
  if (check_date(l_time, fuzzydate | TIME_INVALID_DATES, &was_cut))
    goto err;

  if (val != val_end)
  {
    do
    {
      if (!my_isspace(&my_charset_latin1,*val))
      {
        ErrConvString err(val_begin, length, &my_charset_bin);
        make_truncated_value_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                                     &err, cached_timestamp_type,
                                     nullptr, nullptr, nullptr);
	break;
      }
    } while (++val != val_end);
  }
  DBUG_RETURN(0);

err:
  {
    char buff[128];
    strmake(buff, val_begin, MY_MIN(length, sizeof(buff)-1));
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WRONG_VALUE_FOR_TYPE,
                        ER_THD(thd, ER_WRONG_VALUE_FOR_TYPE),
                        date_time_type, buff, "str_to_date");
  }
  DBUG_RETURN(1);
}


/**
  Create a formatted date/time value in a string.
*/

static bool make_date_time(const String *format, const MYSQL_TIME *l_time,
                           timestamp_type type, const MY_LOCALE *locale,
                           String *str)
{
  char intbuff[15];
  uint hours_i;
  uint weekday;
  ulong length;
  const char *ptr, *end;

  str->length(0);

  if (l_time->neg)
    str->append('-');
  
  end= (ptr= format->ptr()) + format->length();
  for (; ptr != end ; ptr++)
  {
    if (*ptr != '%' || ptr+1 == end)
      str->append(*ptr);
    else
    {
      switch (*++ptr) {
      case 'M':
        if (type == MYSQL_TIMESTAMP_TIME || !l_time->month)
          return 1;
        str->append(locale->month_names->type_names[l_time->month-1],
                    (uint) strlen(locale->month_names->type_names[l_time->month-1]),
                    system_charset_info);
        break;
      case 'b':
        if (type == MYSQL_TIMESTAMP_TIME || !l_time->month)
          return 1;
        str->append(locale->ab_month_names->type_names[l_time->month-1],
                    (uint) strlen(locale->ab_month_names->type_names[l_time->month-1]),
                    system_charset_info);
        break;
      case 'W':
        if (type == MYSQL_TIMESTAMP_TIME || !(l_time->month || l_time->year))
          return 1;
        weekday= calc_weekday(calc_daynr(l_time->year,l_time->month,
                              l_time->day),0);
        str->append(locale->day_names->type_names[weekday],
                    (uint) strlen(locale->day_names->type_names[weekday]),
                    system_charset_info);
        break;
      case 'a':
        if (type == MYSQL_TIMESTAMP_TIME || !(l_time->month || l_time->year))
          return 1;
        weekday=calc_weekday(calc_daynr(l_time->year,l_time->month,
                             l_time->day),0);
        str->append(locale->ab_day_names->type_names[weekday],
                    (uint) strlen(locale->ab_day_names->type_names[weekday]),
                    system_charset_info);
        break;
      case 'D':
	if (type == MYSQL_TIMESTAMP_TIME)
	  return 1;
	length= (uint) (int10_to_str(l_time->day, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 1, '0');
	if (l_time->day >= 10 &&  l_time->day <= 19)
	  str->append(STRING_WITH_LEN("th"));
	else
	{
	  switch (l_time->day %10) {
	  case 1:
	    str->append(STRING_WITH_LEN("st"));
	    break;
	  case 2:
	    str->append(STRING_WITH_LEN("nd"));
	    break;
	  case 3:
	    str->append(STRING_WITH_LEN("rd"));
	    break;
	  default:
	    str->append(STRING_WITH_LEN("th"));
	    break;
	  }
	}
	break;
      case 'Y':
        if (type == MYSQL_TIMESTAMP_TIME)
          return 1;
	length= (uint) (int10_to_str(l_time->year, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 4, '0');
	break;
      case 'y':
        if (type == MYSQL_TIMESTAMP_TIME)
          return 1;
	length= (uint) (int10_to_str(l_time->year%100, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
	break;
      case 'm':
        if (type == MYSQL_TIMESTAMP_TIME)
          return 1;
	length= (uint) (int10_to_str(l_time->month, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
	break;
      case 'c':
        if (type == MYSQL_TIMESTAMP_TIME)
          return 1;
	length= (uint) (int10_to_str(l_time->month, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 1, '0');
	break;
      case 'd':
	if (type == MYSQL_TIMESTAMP_TIME)
	  return 1;
	length= (uint) (int10_to_str(l_time->day, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
	break;
      case 'e':
	if (type == MYSQL_TIMESTAMP_TIME)
	  return 1;
	length= (uint) (int10_to_str(l_time->day, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 1, '0');
	break;
      case 'f':
	length= (uint) (int10_to_str(l_time->second_part, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 6, '0');
	break;
      case 'H':
	length= (uint) (int10_to_str(l_time->hour, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
	break;
      case 'h':
      case 'I':
	hours_i= (l_time->hour%24 + 11)%12+1;
	length= (uint) (int10_to_str(hours_i, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
	break;
      case 'i':					/* minutes */
	length= (uint) (int10_to_str(l_time->minute, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
	break;
      case 'j':
	if (type == MYSQL_TIMESTAMP_TIME || !l_time->month || !l_time->year)
	  return 1;
	length= (uint) (int10_to_str(calc_daynr(l_time->year,l_time->month,
					l_time->day) - 
		     calc_daynr(l_time->year,1,1) + 1, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 3, '0');
	break;
      case 'k':
	length= (uint) (int10_to_str(l_time->hour, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 1, '0');
	break;
      case 'l':
	hours_i= (l_time->hour%24 + 11)%12+1;
	length= (uint) (int10_to_str(hours_i, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 1, '0');
	break;
      case 'p':
	hours_i= l_time->hour%24;
	str->append(hours_i < 12 ? "AM" : "PM",2);
	break;
      case 'r':
	length= sprintf(intbuff, ((l_time->hour % 24) < 12) ?
                    "%02d:%02d:%02d AM" : "%02d:%02d:%02d PM",
		    (l_time->hour+11)%12+1,
		    l_time->minute,
		    l_time->second);
	str->append(intbuff, length);
	break;
      case 'S':
      case 's':
	length= (uint) (int10_to_str(l_time->second, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
	break;
      case 'T':
	length= sprintf(intbuff, "%02d:%02d:%02d",
		    l_time->hour, l_time->minute, l_time->second);
	str->append(intbuff, length);
	break;
      case 'U':
      case 'u':
      {
	uint year;
	if (type == MYSQL_TIMESTAMP_TIME)
	  return 1;
	length= (uint) (int10_to_str(calc_week(l_time,
				       (*ptr) == 'U' ?
				       WEEK_FIRST_WEEKDAY : WEEK_MONDAY_FIRST,
				       &year),
			     intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
      }
      break;
      case 'v':
      case 'V':
      {
	uint year;
	if (type == MYSQL_TIMESTAMP_TIME)
	  return 1;
	length= (uint) (int10_to_str(calc_week(l_time,
				       ((*ptr) == 'V' ?
					(WEEK_YEAR | WEEK_FIRST_WEEKDAY) :
					(WEEK_YEAR | WEEK_MONDAY_FIRST)),
				       &year),
			     intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 2, '0');
      }
      break;
      case 'x':
      case 'X':
      {
	uint year;
	if (type == MYSQL_TIMESTAMP_TIME)
	  return 1;
	(void) calc_week(l_time,
			 ((*ptr) == 'X' ?
			  WEEK_YEAR | WEEK_FIRST_WEEKDAY :
			  WEEK_YEAR | WEEK_MONDAY_FIRST),
			 &year);
	length= (uint) (int10_to_str(year, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 4, '0');
      }
      break;
      case 'w':
	if (type == MYSQL_TIMESTAMP_TIME || !(l_time->month || l_time->year))
	  return 1;
	weekday=calc_weekday(calc_daynr(l_time->year,l_time->month,
					l_time->day),1);
	length= (uint) (int10_to_str(weekday, intbuff, 10) - intbuff);
	str->append_with_prefill(intbuff, length, 1, '0');
	break;

      default:
	str->append(*ptr);
	break;
      }
    }
  }
  return 0;
}


/**
  @details
  Get a array of positive numbers from a string object.
  Each number is separated by 1 non digit character
  Return error if there is too many numbers.
  If there is too few numbers, assume that the numbers are left out
  from the high end. This allows one to give:
  DAY_TO_SECOND as "D MM:HH:SS", "MM:HH:SS" "HH:SS" or as seconds.

  @param length:         length of str
  @param cs:             charset of str
  @param values:         array of results
  @param count:          count of elements in result array
  @param transform_msec: if value is true we suppose
                         that the last part of string value is microseconds
                         and we should transform value to six digit value.
                         For example, '1.1' -> '1.100000'
*/

#define MAX_DIGITS_IN_TIME_SPEC 20

static bool get_interval_info(const char *str, size_t length,CHARSET_INFO *cs,
                              size_t count, ulonglong *values,
                              bool transform_msec)
{
  const char *end=str+length;
  uint i;
  size_t field_length= 0;

  while (str != end && !my_isdigit(cs,*str))
    str++;

  for (i=0 ; i < count ; i++)
  {
    ulonglong value;
    const char *start= str;
    const char *local_end= end;

    /*
      We limit things to 19 digits to not get an overflow. This is ok as
      this function is meant to read up to microseconds
    */
    if ((local_end-str) > MAX_DIGITS_IN_TIME_SPEC)
      local_end= str+ MAX_DIGITS_IN_TIME_SPEC;

    for (value= 0; str != local_end && my_isdigit(cs, *str) ; str++)
      value= value*10 + *str - '0';

    if ((field_length= (size_t)(str - start)) >= MAX_DIGITS_IN_TIME_SPEC)
      return true;
    values[i]= value;
    while (str != end && !my_isdigit(cs,*str))
      str++;
    if (str == end && i != count-1)
    {
      i++;
      /* Change values[0...i-1] -> values[0...count-1] */
      bmove_upp((uchar*) (values+count), (uchar*) (values+i),
		sizeof(*values)*i);
      bzero((uchar*) values, sizeof(*values)*(count-i));
      break;
    }
  }

  if (transform_msec && field_length > 0)
  {
    if (field_length < 6)
      values[count - 1] *= log_10_int[6 - field_length];
    else if (field_length > 6)
      values[count - 1] /= log_10_int[field_length - 6];
  }

  return (str != end);
}


longlong Item_func_period_add::val_int()
{
  DBUG_ASSERT(fixed());
  ulong period=(ulong) args[0]->val_int();
  int months=(int) args[1]->val_int();

  if ((null_value=args[0]->null_value || args[1]->null_value) ||
      period == 0L)
    return 0; /* purecov: inspected */
  return (longlong)
    convert_month_to_period((uint) ((int) convert_period_to_month(period)+
				    months));
}


longlong Item_func_period_diff::val_int()
{
  DBUG_ASSERT(fixed());
  ulong period1=(ulong) args[0]->val_int();
  ulong period2=(ulong) args[1]->val_int();

  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0; /* purecov: inspected */
  return (longlong) ((long) convert_period_to_month(period1)-
		     (long) convert_period_to_month(period2));
}



longlong Item_func_to_days::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_NO_ZEROS, thd));
  return (null_value= !d.is_valid_datetime()) ? 0 : d.daynr();
}


longlong Item_func_to_seconds::val_int_endpoint(bool left_endp,
                                                bool *incl_endp)
{
  DBUG_ASSERT(fixed());
  // val_int_endpoint() is called only if args[0] is a temporal Item_field
  Datetime_from_temporal dt(current_thd, args[0], TIME_FUZZY_DATES);
  if ((null_value= !dt.is_valid_datetime()))
  {
    /* got NULL, leave the incl_endp intact */
    return LONGLONG_MIN;
  }
  /* Set to NULL if invalid date, but keep the value */
  null_value= dt.check_date(TIME_NO_ZEROS);
  /*
    Even if the evaluation return NULL, seconds is useful for pruning
  */
  return dt.to_seconds();
}

longlong Item_func_to_seconds::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  /*
    Unlike val_int_endpoint(), we cannot use Datetime_from_temporal here.
    The argument can be of a non-temporal data type.
  */
  Datetime dt(thd, args[0], Datetime::Options(TIME_NO_ZEROS, thd));
  return (null_value= !dt.is_valid_datetime()) ? 0 : dt.to_seconds();
}

/*
  Get information about this Item tree monotonicity

  SYNOPSIS
    Item_func_to_days::get_monotonicity_info()

  DESCRIPTION
  Get information about monotonicity of the function represented by this item
  tree.

  RETURN
    See enum_monotonicity_info.
*/

enum_monotonicity_info Item_func_to_days::get_monotonicity_info() const
{
  if (args[0]->type() == Item::FIELD_ITEM)
  {
    if (args[0]->field_type() == MYSQL_TYPE_DATE)
      return MONOTONIC_STRICT_INCREASING_NOT_NULL;
    if (args[0]->field_type() == MYSQL_TYPE_DATETIME)
      return MONOTONIC_INCREASING_NOT_NULL;
  }
  return NON_MONOTONIC;
}

enum_monotonicity_info Item_func_to_seconds::get_monotonicity_info() const
{
  if (args[0]->type() == Item::FIELD_ITEM)
  {
    if (args[0]->field_type() == MYSQL_TYPE_DATE ||
        args[0]->field_type() == MYSQL_TYPE_DATETIME)
      return MONOTONIC_STRICT_INCREASING_NOT_NULL;
  }
  return NON_MONOTONIC;
}


longlong Item_func_to_days::val_int_endpoint(bool left_endp, bool *incl_endp)
{
  DBUG_ASSERT(fixed());
  // val_int_endpoint() is only called if args[0] is a temporal Item_field
  Datetime_from_temporal dt(current_thd, args[0], TIME_CONV_NONE);
  longlong res;
  if ((null_value= !dt.is_valid_datetime()))
  {
    /* got NULL, leave the incl_endp intact */
    return LONGLONG_MIN;
  }
  res= (longlong) dt.daynr();
  /* Set to NULL if invalid date, but keep the value */
  null_value= dt.check_date(TIME_NO_ZEROS);
  if (null_value)
  {
    /*
      Even if the evaluation return NULL, the calc_daynr is useful for pruning
    */
    if (args[0]->field_type() != MYSQL_TYPE_DATE)
      *incl_endp= TRUE;
    return res;
  }
  
  if (args[0]->field_type() == MYSQL_TYPE_DATE)
  {
    // TO_DAYS() is strictly monotonic for dates, leave incl_endp intact
    return res;
  }
 
  /*
    Handle the special but practically useful case of datetime values that
    point to day bound ("strictly less" comparison stays intact):

      col < '2007-09-15 00:00:00'  -> TO_DAYS(col) <  TO_DAYS('2007-09-15')
      col > '2007-09-15 23:59:59'  -> TO_DAYS(col) >  TO_DAYS('2007-09-15')

    which is different from the general case ("strictly less" changes to
    "less or equal"):

      col < '2007-09-15 12:34:56'  -> TO_DAYS(col) <= TO_DAYS('2007-09-15')
  */
  const MYSQL_TIME &ltime= dt.get_mysql_time()[0];
  if ((!left_endp && dt.hhmmssff_is_zero()) ||
       (left_endp && ltime.hour == 23 && ltime.minute == 59 &&
        ltime.second == 59))
    /* do nothing */
    ;
  else
    *incl_endp= TRUE;
  return res;
}


longlong Item_func_dayofyear::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_NO_ZEROS, thd));
  return (null_value= !d.is_valid_datetime()) ? 0 : d.dayofyear();
}

longlong Item_func_dayofmonth::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_CONV_NONE, thd));
  return (null_value= !d.is_valid_datetime()) ? 0 : d.get_mysql_time()->day;
}

longlong Item_func_month::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_CONV_NONE, thd));
  return (null_value= !d.is_valid_datetime()) ? 0 : d.get_mysql_time()->month;
}


bool Item_func_monthname::fix_length_and_dec(THD *thd)
{
  CHARSET_INFO *cs= thd->variables.collation_connection;
  locale= thd->variables.lc_time_names;
  collation.set(cs, DERIVATION_COERCIBLE, locale->repertoire());
  decimals=0;
  max_length= locale->max_month_name_length * collation.collation->mbmaxlen;
  set_maybe_null();
  return FALSE;
}


String* Item_func_monthname::val_str(String* str)
{
  DBUG_ASSERT(fixed());
  const char *month_name;
  uint err;
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_CONV_NONE, thd));
  if ((null_value= (!d.is_valid_datetime() || !d.get_mysql_time()->month)))
    return (String *) 0;

  month_name= locale->month_names->type_names[d.get_mysql_time()->month - 1];
  str->copy(month_name, (uint) strlen(month_name), &my_charset_utf8mb3_bin,
	    collation.collation, &err);
  return str;
}


/**
  Returns the quarter of the year.
*/

longlong Item_func_quarter::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_CONV_NONE, thd));
  return (null_value= !d.is_valid_datetime()) ? 0 : d.quarter();
}

longlong Item_func_hour::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Time tm(thd, args[0], Time::Options_for_cast(thd));
  return (null_value= !tm.is_valid_time()) ? 0 : tm.get_mysql_time()->hour;
}

longlong Item_func_minute::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Time tm(thd, args[0], Time::Options_for_cast(thd));
  return (null_value= !tm.is_valid_time()) ? 0 : tm.get_mysql_time()->minute;
}

/**
  Returns the second in time_exp in the range of 0 - 59.
*/
longlong Item_func_second::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Time tm(thd, args[0], Time::Options_for_cast(thd));
  return (null_value= !tm.is_valid_time()) ? 0 : tm.get_mysql_time()->second;
}


uint week_mode(uint mode)
{
  uint week_format= (mode & 7);
  if (!(week_format & WEEK_MONDAY_FIRST))
    week_format^= WEEK_FIRST_WEEKDAY;
  return week_format;
}

/**
 @verbatim
  The bits in week_format(for calc_week() function) has the following meaning:
   WEEK_MONDAY_FIRST (0)  If not set	Sunday is first day of week
      		   	  If set	Monday is first day of week
   WEEK_YEAR (1)	  If not set	Week is in range 0-53

   	Week 0 is returned for the the last week of the previous year (for
	a date at start of january) In this case one can get 53 for the
	first week of next year.  This flag ensures that the week is
	relevant for the given year. Note that this flag is only
	relevant if WEEK_JANUARY is not set.

			  If set	 Week is in range 1-53.

	In this case one may get week 53 for a date in January (when
	the week is that last week of previous year) and week 1 for a
	date in December.

  WEEK_FIRST_WEEKDAY (2)  If not set	Weeks are numbered according
			   		to ISO 8601:1988
			  If set	The week that contains the first
					'first-day-of-week' is week 1.
	
	ISO 8601:1988 means that if the week containing January 1 has
	four or more days in the new year, then it is week 1;
	Otherwise it is the last week of the previous year, and the
	next week is week 1.
 @endverbatim
*/

longlong Item_func_week::val_int()
{
  DBUG_ASSERT(fixed());
  uint week_format;
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_NO_ZEROS, thd));
  if ((null_value= !d.is_valid_datetime()))
    return 0;
  if (arg_count > 1)
    week_format= (uint)args[1]->val_int();
  else
    week_format= thd->variables.default_week_format;
  return d.week(week_mode(week_format));
}


longlong Item_func_yearweek::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_NO_ZEROS, thd));
  return (null_value= !d.is_valid_datetime()) ? 0 :
         d.yearweek((week_mode((uint) args[1]->val_int()) | WEEK_YEAR));
}


longlong Item_func_weekday::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime dt(thd, args[0], Datetime::Options(TIME_NO_ZEROS, thd));
  if ((null_value= !dt.is_valid_datetime()))
    return 0;
  return dt.weekday(odbc_type) + MY_TEST(odbc_type);
}

bool Item_func_dayname::fix_length_and_dec(THD *thd)
{
  CHARSET_INFO *cs= thd->variables.collation_connection;
  locale= thd->variables.lc_time_names;  
  collation.set(cs, DERIVATION_COERCIBLE, locale->repertoire());
  decimals=0;
  max_length= locale->max_day_name_length * collation.collation->mbmaxlen;
  set_maybe_null();
  return FALSE;
}


String* Item_func_dayname::val_str(String* str)
{
  DBUG_ASSERT(fixed());
  const char *day_name;
  uint err;
  THD *thd= current_thd;
  Datetime dt(thd, args[0], Datetime::Options(TIME_NO_ZEROS, thd));

  if ((null_value= !dt.is_valid_datetime()))
    return (String*) 0;

  day_name= locale->day_names->type_names[dt.weekday(false)];
  str->copy(day_name, (uint) strlen(day_name), &my_charset_utf8mb3_bin,
	    collation.collation, &err);
  return str;
}


longlong Item_func_year::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Datetime d(thd, args[0], Datetime::Options(TIME_CONV_NONE, thd));
  return (null_value= !d.is_valid_datetime()) ? 0 : d.get_mysql_time()->year;
}


/*
  Get information about this Item tree monotonicity

  SYNOPSIS
    Item_func_year::get_monotonicity_info()

  DESCRIPTION
  Get information about monotonicity of the function represented by this item
  tree.

  RETURN
    See enum_monotonicity_info.
*/

enum_monotonicity_info Item_func_year::get_monotonicity_info() const
{
  if (args[0]->type() == Item::FIELD_ITEM &&
      (args[0]->field_type() == MYSQL_TYPE_DATE ||
       args[0]->field_type() == MYSQL_TYPE_DATETIME))
    return MONOTONIC_INCREASING;
  return NON_MONOTONIC;
}


longlong Item_func_year::val_int_endpoint(bool left_endp, bool *incl_endp)
{
  DBUG_ASSERT(fixed());
  // val_int_endpoint() is cally only if args[0] is a temporal Item_field
  Datetime_from_temporal dt(current_thd, args[0], TIME_CONV_NONE);
  if ((null_value= !dt.is_valid_datetime()))
  {
    /* got NULL, leave the incl_endp intact */
    return LONGLONG_MIN;
  }

  /*
    Handle the special but practically useful case of datetime values that
    point to year bound ("strictly less" comparison stays intact) :

      col < '2007-01-01 00:00:00'  -> YEAR(col) <  2007

    which is different from the general case ("strictly less" changes to
    "less or equal"):

      col < '2007-09-15 23:00:00'  -> YEAR(col) <= 2007
  */
  const MYSQL_TIME &ltime= dt.get_mysql_time()[0];
  if (!left_endp && ltime.day == 1 && ltime.month == 1 && 
      dt.hhmmssff_is_zero())
    ; /* do nothing */
  else
    *incl_endp= TRUE;
  return ltime.year;
}


bool Item_func_unix_timestamp::get_timestamp_value(my_time_t *seconds,
                                                   ulong *second_part)
{
  DBUG_ASSERT(fixed());
  if (args[0]->type() == FIELD_ITEM)
  {						// Optimize timestamp field
    Field *field=((Item_field*) args[0])->field;
    if (field->type() == MYSQL_TYPE_TIMESTAMP)
    {
      if ((null_value= field->is_null()))
        return 1;
      *seconds= field->get_timestamp(second_part);
      return 0;
    }
  }

  Timestamp_or_zero_datetime_native_null native(current_thd, args[0], true);
  if ((null_value= native.is_null() || native.is_zero_datetime()))
    return true;
  Timestamp tm(native);
  *seconds= tm.tv().tv_sec;
  *second_part= tm.tv().tv_usec;
  return false;
}


longlong Item_func_unix_timestamp::int_op()
{
  if (arg_count == 0)
    return (longlong) current_thd->query_start();
  
  ulong second_part;
  my_time_t seconds;
  if (get_timestamp_value(&seconds, &second_part))
    return 0;

  return seconds;
}


my_decimal *Item_func_unix_timestamp::decimal_op(my_decimal* buf)
{
  ulong second_part;
  my_time_t seconds;
  if (get_timestamp_value(&seconds, &second_part))
    return 0;

  return seconds2my_decimal(seconds < 0, seconds < 0 ? -seconds : seconds,
                            second_part, buf);
}


enum_monotonicity_info Item_func_unix_timestamp::get_monotonicity_info() const
{
  if (args[0]->type() == Item::FIELD_ITEM &&
      (args[0]->field_type() == MYSQL_TYPE_TIMESTAMP))
    return MONOTONIC_INCREASING;
  return NON_MONOTONIC;
}


longlong Item_func_unix_timestamp::val_int_endpoint(bool left_endp, bool *incl_endp)
{
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(arg_count == 1 &&
              args[0]->type() == Item::FIELD_ITEM &&
              args[0]->field_type() == MYSQL_TYPE_TIMESTAMP);
  Field *field= ((Item_field*)args[0])->field;
  /* Leave the incl_endp intact */
  ulong unused;
  my_time_t ts= field->get_timestamp(&unused);
  null_value= field->is_null();
  return ts;
}


longlong Item_func_time_to_sec::int_op()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Time tm(thd, args[0], Time::Options_for_cast(thd));
  return ((null_value= !tm.is_valid_time())) ? 0 : tm.to_seconds();
}


my_decimal *Item_func_time_to_sec::decimal_op(my_decimal* buf)
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Time tm(thd, args[0], Time::Options_for_cast(thd));
  if ((null_value= !tm.is_valid_time()))
    return 0;
  const MYSQL_TIME *ltime= tm.get_mysql_time();
  longlong seconds= tm.to_seconds_abs();
  return seconds2my_decimal(ltime->neg, seconds, ltime->second_part, buf);
}


/**
  Convert a string to a interval value.

  To make code easy, allow interval objects without separators.
*/

bool get_interval_value(THD *thd, Item *args,
                        interval_type int_type, INTERVAL *interval)
{
  ulonglong array[5];
  longlong UNINIT_VAR(value);
  const char *UNINIT_VAR(str);
  size_t UNINIT_VAR(length);
  CHARSET_INFO *UNINIT_VAR(cs);
  char buf[100];
  String str_value(buf, sizeof(buf), &my_charset_bin);

  bzero((char*) interval,sizeof(*interval));
  if (int_type == INTERVAL_SECOND && args->decimals)
  {
    VDec val(args);
    if (val.is_null())
      return true;
    Sec6 d(val.ptr());
    interval->neg= d.neg();
    if (d.sec() >= LONGLONG_MAX)
    {
      ErrConvDecimal err(val.ptr());
      thd->push_warning_truncated_wrong_value("seconds", err.ptr());
      return true;
    }
    interval->second= d.sec();
    interval->second_part= d.usec();
    return false;
  }
  else if ((int) int_type <= INTERVAL_MICROSECOND)
  {
    value= args->val_int();
    if (args->null_value)
      return 1;
    if (value < 0)
    {
      interval->neg=1;
      value= -value;
    }
  }
  else
  {
    String *res;
    if (!(res= args->val_str_ascii(&str_value)))
      return (1);

    /* record negative intervals in interval->neg */
    str=res->ptr();
    cs= res->charset();
    const char *end=str+res->length();
    while (str != end && my_isspace(cs,*str))
      str++;
    if (str != end && *str == '-')
    {
      interval->neg=1;
      str++;
    }
    length= (size_t) (end-str);		// Set up pointers to new str
  }

  switch (int_type) {
  case INTERVAL_YEAR:
    interval->year= (ulong) value;
    break;
  case INTERVAL_QUARTER:
    interval->month= (ulong)(value*3);
    break;
  case INTERVAL_MONTH:
    interval->month= (ulong) value;
    break;
  case INTERVAL_WEEK:
    interval->day= (ulong)(value*7);
    break;
  case INTERVAL_DAY:
    interval->day= (ulong) value;
    break;
  case INTERVAL_HOUR:
    interval->hour= (ulong) value;
    break;
  case INTERVAL_MICROSECOND:
    interval->second_part=value;
    break;
  case INTERVAL_MINUTE:
    interval->minute=value;
    break;
  case INTERVAL_SECOND:
    interval->second=value;
    break;
  case INTERVAL_YEAR_MONTH:			// Allow YEAR-MONTH YYYYYMM
    if (get_interval_info(str,length,cs,2,array,0))
      return (1);
    interval->year=  (ulong) array[0];
    interval->month= (ulong) array[1];
    break;
  case INTERVAL_DAY_HOUR:
    if (get_interval_info(str,length,cs,2,array,0))
      return (1);
    interval->day=  (ulong) array[0];
    interval->hour= (ulong) array[1];
    break;
  case INTERVAL_DAY_MICROSECOND:
    if (get_interval_info(str,length,cs,5,array,1))
      return (1);
    interval->day=    (ulong) array[0];
    interval->hour=   (ulong) array[1];
    interval->minute= array[2];
    interval->second= array[3];
    interval->second_part= array[4];
    break;
  case INTERVAL_DAY_MINUTE:
    if (get_interval_info(str,length,cs,3,array,0))
      return (1);
    interval->day=    (ulong) array[0];
    interval->hour=   (ulong) array[1];
    interval->minute= array[2];
    break;
  case INTERVAL_DAY_SECOND:
    if (get_interval_info(str,length,cs,4,array,0))
      return (1);
    interval->day=    (ulong) array[0];
    interval->hour=   (ulong) array[1];
    interval->minute= array[2];
    interval->second= array[3];
    break;
  case INTERVAL_HOUR_MICROSECOND:
    if (get_interval_info(str,length,cs,4,array,1))
      return (1);
    interval->hour=   (ulong) array[0];
    interval->minute= array[1];
    interval->second= array[2];
    interval->second_part= array[3];
    break;
  case INTERVAL_HOUR_MINUTE:
    if (get_interval_info(str,length,cs,2,array,0))
      return (1);
    interval->hour=   (ulong) array[0];
    interval->minute= array[1];
    break;
  case INTERVAL_HOUR_SECOND:
    if (get_interval_info(str,length,cs,3,array,0))
      return (1);
    interval->hour=   (ulong) array[0];
    interval->minute= array[1];
    interval->second= array[2];
    break;
  case INTERVAL_MINUTE_MICROSECOND:
    if (get_interval_info(str,length,cs,3,array,1))
      return (1);
    interval->minute= array[0];
    interval->second= array[1];
    interval->second_part= array[2];
    break;
  case INTERVAL_MINUTE_SECOND:
    if (get_interval_info(str,length,cs,2,array,0))
      return (1);
    interval->minute= array[0];
    interval->second= array[1];
    break;
  case INTERVAL_SECOND_MICROSECOND:
    if (get_interval_info(str,length,cs,2,array,1))
      return (1);
    interval->second= array[0];
    interval->second_part= array[1];
    break;
  case INTERVAL_LAST: /* purecov: begin deadcode */
    DBUG_ASSERT(0); 
    break;            /* purecov: end */
  }
  return 0;
}


bool Item_func_from_days::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  longlong value=args[0]->val_int();
  if ((null_value= (args[0]->null_value ||
                    ((fuzzydate & TIME_NO_ZERO_DATE) && value == 0))))
    return true;
  bzero(ltime, sizeof(MYSQL_TIME));
  if (get_date_from_daynr((long) value, &ltime->year, &ltime->month,
                          &ltime->day))
    return 0;

  ltime->time_type= MYSQL_TIMESTAMP_DATE;
  return 0;
}


/**
    Converts current time in my_time_t to MYSQL_TIME representation for local
    time zone. Defines time zone (local) used for whole CURDATE function.
*/
void Item_func_curdate_local::store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)
{
  thd->variables.time_zone->gmt_sec_to_TIME(now_time, thd->query_start());
  thd->time_zone_used= 1;
}


/**
    Converts current time in my_time_t to MYSQL_TIME representation for UTC
    time zone. Defines time zone (UTC) used for whole UTC_DATE function.
*/
void Item_func_curdate_utc::store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)
{
  my_tz_UTC->gmt_sec_to_TIME(now_time, thd->query_start());
  /* 
    We are not flagging this query as using time zone, since it uses fixed
    UTC-SYSTEM time-zone.
  */
}


bool Item_func_curdate::get_date(THD *thd, MYSQL_TIME *res,
				 date_mode_t fuzzydate __attribute__((unused)))
{
  query_id_t query_id= thd->query_id;
  /* Cache value for this query */
  if (last_query_id != query_id)
  {
    last_query_id= query_id;
    store_now_in_TIME(thd, &ltime);
    /* We don't need to set second_part and neg because they already 0 */
    ltime.hour= ltime.minute= ltime.second= 0;
    ltime.time_type= MYSQL_TIMESTAMP_DATE;
  }
  *res=ltime;
  return 0;
}


bool Item_func_curtime::fix_fields(THD *thd, Item **items)
{
  if (decimals > TIME_SECOND_PART_DIGITS)
  {
    my_error(ER_TOO_BIG_PRECISION, MYF(0),
             func_name(), TIME_SECOND_PART_DIGITS);
    return 1;
  }
  return Item_timefunc::fix_fields(thd, items);
}

bool Item_func_curtime::get_date(THD *thd, MYSQL_TIME *res,
                                 date_mode_t fuzzydate __attribute__((unused)))
{
  query_id_t query_id= thd->query_id;
  /* Cache value for this query */
  if (last_query_id != query_id)
  {
    last_query_id= query_id;
    store_now_in_TIME(thd, &ltime);
  }
  *res= ltime;
  return 0;
}

void Item_func_curtime::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  if (decimals)
    str->append_ulonglong(decimals);
  str->append(')');
}

static void set_sec_part(ulong sec_part, MYSQL_TIME *ltime, Item *item)
{
  DBUG_ASSERT(item->decimals == AUTO_SEC_PART_DIGITS ||
              item->decimals <= TIME_SECOND_PART_DIGITS);
  if (item->decimals)
  {
    ltime->second_part= sec_part;
    if (item->decimals < TIME_SECOND_PART_DIGITS)
      my_datetime_trunc(ltime, item->decimals);
  }
}

/**
    Converts current time in my_time_t to MYSQL_TIME representation for local
    time zone. Defines time zone (local) used for whole CURTIME function.
*/
void Item_func_curtime_local::store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)
{
  thd->variables.time_zone->gmt_sec_to_TIME(now_time, thd->query_start());
  now_time->year= now_time->month= now_time->day= 0;
  now_time->time_type= MYSQL_TIMESTAMP_TIME;
  set_sec_part(thd->query_start_sec_part(), now_time, this);
  thd->time_zone_used= 1;
}


/**
    Converts current time in my_time_t to MYSQL_TIME representation for UTC
    time zone. Defines time zone (UTC) used for whole UTC_TIME function.
*/
void Item_func_curtime_utc::store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)
{
  my_tz_UTC->gmt_sec_to_TIME(now_time, thd->query_start());
  now_time->year= now_time->month= now_time->day= 0;
  now_time->time_type= MYSQL_TIMESTAMP_TIME;
  set_sec_part(thd->query_start_sec_part(), now_time, this);
  /* 
    We are not flagging this query as using time zone, since it uses fixed
    UTC-SYSTEM time-zone.
  */
}

bool Item_func_now::fix_fields(THD *thd, Item **items)
{
  if (decimals > TIME_SECOND_PART_DIGITS)
  {
    my_error(ER_TOO_BIG_PRECISION, MYF(0),
             func_name(), TIME_SECOND_PART_DIGITS);
    return 1;
  }
  return Item_datetimefunc::fix_fields(thd, items);
}

void Item_func_now::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  if (decimals)
    str->append_ulonglong(decimals);
  str->append(')');
}


int Item_func_now_local::save_in_field(Field *field, bool no_conversions)
{
  if (field->type() == MYSQL_TYPE_TIMESTAMP)
  {
    THD *thd= field->get_thd();
    my_time_t ts= thd->query_start();
    ulong sec_part= decimals ? thd->query_start_sec_part() : 0;
    sec_part-= my_time_fraction_remainder(sec_part, decimals);
    field->set_notnull();
    field->store_timestamp(ts, sec_part);
    return 0;
  }
  else
    return Item_datetimefunc::save_in_field(field, no_conversions);
}


/**
    Converts current time in my_time_t to MYSQL_TIME representation for local
    time zone. Defines time zone (local) used for whole NOW function.
*/
void Item_func_now_local::store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)
{
  thd->variables.time_zone->gmt_sec_to_TIME(now_time, thd->query_start());
  set_sec_part(thd->query_start_sec_part(), now_time, this);
  thd->time_zone_used= 1;
}


/**
    Converts current time in my_time_t to MYSQL_TIME representation for UTC
    time zone. Defines time zone (UTC) used for whole UTC_TIMESTAMP function.
*/
void Item_func_now_utc::store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)
{
  my_tz_UTC->gmt_sec_to_TIME(now_time, thd->query_start());
  set_sec_part(thd->query_start_sec_part(), now_time, this);
  /* 
    We are not flagging this query as using time zone, since it uses fixed
    UTC-SYSTEM time-zone.
  */
}


bool Item_func_now::get_date(THD *thd, MYSQL_TIME *res,
                             date_mode_t fuzzydate __attribute__((unused)))
{
  query_id_t query_id= thd->query_id;
  /* Cache value for this query */
  if (last_query_id != query_id)
  {
    last_query_id= query_id;
    store_now_in_TIME(thd, &ltime);
  }
  *res= ltime;
  return 0;
}


/**
    Converts current time in my_time_t to MYSQL_TIME representation for local
    time zone. Defines time zone (local) used for whole SYSDATE function.
*/
void Item_func_sysdate_local::store_now_in_TIME(THD *thd, MYSQL_TIME *now_time)
{
  my_hrtime_t now= my_hrtime();
  thd->variables.time_zone->gmt_sec_to_TIME(now_time, hrtime_to_my_time(now));
  set_sec_part(hrtime_sec_part(now), now_time, this);
  thd->time_zone_used= 1;
}


bool Item_func_sysdate_local::get_date(THD *thd, MYSQL_TIME *res,
                                       date_mode_t fuzzydate __attribute__((unused)))
{
  store_now_in_TIME(thd, res);
  return 0;
}

bool Item_func_sec_to_time::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  VSec9 sec(thd, args[0], "seconds", LONGLONG_MAX);
  if ((null_value= sec.is_null()))
    return true;
  sec.round(decimals, thd->temporal_round_mode());
  if (sec.sec_to_time(ltime, decimals) && !sec.truncated())
    sec.make_truncated_warning(thd, "seconds");
  return false;
}

bool Item_func_date_format::fix_length_and_dec(THD *thd)
{
  if (!is_time_format)
  {
    if (arg_count < 3)
      locale= thd->variables.lc_time_names;
    else
      if (args[2]->basic_const_item())
        locale= args[2]->locale_from_val_str();
  }

  /*
    Must use this_item() in case it's a local SP variable
    (for ->max_length and ->str_value)
  */
  Item *arg1= args[1]->this_item();

  decimals=0;
  CHARSET_INFO *cs= thd->variables.collation_connection;
  my_repertoire_t repertoire= arg1->collation.repertoire;
  if (!thd->variables.lc_time_names->is_ascii)
    repertoire|= MY_REPERTOIRE_EXTENDED;
  collation.set(cs, arg1->collation.derivation, repertoire);
  StringBuffer<STRING_BUFFER_USUAL_SIZE> buffer;
  String *str;
  if (args[1]->basic_const_item() && (str= args[1]->val_str(&buffer)))
  {						// Optimize the normal case
    fixed_length=1;
    max_length= format_length(str) * collation.collation->mbmaxlen;
  }
  else
  {
    fixed_length=0;
    max_length=MY_MIN(arg1->max_length, MAX_BLOB_WIDTH) * 10 *
                   collation.collation->mbmaxlen;
    set_if_smaller(max_length,MAX_BLOB_WIDTH);
  }
  set_maybe_null(); // If wrong date
  return FALSE;
}


bool Item_func_date_format::eq(const Item *item, bool binary_cmp) const
{
  Item_func_date_format *item_func;

  if (item->type() != FUNC_ITEM)
    return 0;
  if (func_name() != ((Item_func*) item)->func_name())
    return 0;
  if (this == item)
    return 1;
  item_func= (Item_func_date_format*) item;
  if (arg_count != item_func->arg_count)
    return 0;
  if (!args[0]->eq(item_func->args[0], binary_cmp))
    return 0;
  /*
    We must compare format string case sensitive.
    This needed because format modifiers with different case,
    for example %m and %M, have different meaning.
  */
  if (!args[1]->eq(item_func->args[1], 1))
    return 0;
  if (arg_count > 2 && !args[2]->eq(item_func->args[2], 1))
    return 0;
  return 1;
}



uint Item_func_date_format::format_length(const String *format)
{
  uint size=0;
  const char *ptr=format->ptr();
  const char *end=ptr+format->length();

  for (; ptr != end ; ptr++)
  {
    if (*ptr != '%' || ptr == end-1)
      size++;
    else
    {
      switch(*++ptr) {
      case 'M': /* month, textual */
      case 'W': /* day (of the week), textual */
	size += 64; /* large for UTF8 locale data */
	break;
      case 'D': /* day (of the month), numeric plus english suffix */
      case 'Y': /* year, numeric, 4 digits */
      case 'x': /* Year, used with 'v' */
      case 'X': /* Year, used with 'v, where week starts with Monday' */
	size += 4;
	break;
      case 'a': /* locale's abbreviated weekday name (Sun..Sat) */
      case 'b': /* locale's abbreviated month name (Jan.Dec) */
	size += 32; /* large for UTF8 locale data */
	break;
      case 'j': /* day of year (001..366) */
	size += 3;
	break;
      case 'U': /* week (00..52) */
      case 'u': /* week (00..52), where week starts with Monday */
      case 'V': /* week 1..53 used with 'x' */
      case 'v': /* week 1..53 used with 'x', where week starts with Monday */
      case 'y': /* year, numeric, 2 digits */
      case 'm': /* month, numeric */
      case 'd': /* day (of the month), numeric */
      case 'h': /* hour (01..12) */
      case 'I': /* --||-- */
      case 'i': /* minutes, numeric */
      case 'l': /* hour ( 1..12) */
      case 'p': /* locale's AM or PM */
      case 'S': /* second (00..61) */
      case 's': /* seconds, numeric */
      case 'c': /* month (0..12) */
      case 'e': /* day (0..31) */
	size += 2;
	break;
      case 'k': /* hour ( 0..23) */
      case 'H': /* hour (00..23; value > 23 OK, padding always 2-digit) */
	size += 7; /* docs allow > 23, range depends on sizeof(unsigned int) */
	break;
      case 'r': /* time, 12-hour (hh:mm:ss [AP]M) */
	size += 11;
	break;
      case 'T': /* time, 24-hour (hh:mm:ss) */
	size += 8;
	break;
      case 'f': /* microseconds */
	size += 6;
	break;
      case 'w': /* day (of the week), numeric */
      case '%':
      default:
	size++;
	break;
      }
    }
  }
  return size;
}


String *Item_func_date_format::val_str(String *str)
{
  StringBuffer<64> format_buffer;
  String *format;
  MYSQL_TIME l_time;
  uint size;
  const MY_LOCALE *lc= 0;
  DBUG_ASSERT(fixed());
  date_conv_mode_t mode= is_time_format ? TIME_TIME_ONLY : TIME_CONV_NONE;
  THD *thd= current_thd;

  if ((null_value= args[0]->get_date(thd, &l_time,
                                     Temporal::Options(mode, thd))))
    return 0;
  
  if (!(format= args[1]->val_str(&format_buffer)) || !format->length())
    goto null_date;

  if (!is_time_format && !(lc= locale) && !(lc= args[2]->locale_from_val_str()))
    goto null_date; // invalid locale

  if (fixed_length)
    size=max_length;
  else
    size=format_length(format);

  if (size < MAX_DATE_STRING_REP_LENGTH)
    size= MAX_DATE_STRING_REP_LENGTH;

  DBUG_ASSERT(format != str);
  if (str->alloc(size))
    goto null_date;

  /* Create the result string */
  str->set_charset(collation.collation);
  if (!make_date_time(format, &l_time,
                      is_time_format ? MYSQL_TIMESTAMP_TIME :
                                       MYSQL_TIMESTAMP_DATE,
                      lc, str))
    return str;

null_date:
  null_value=1;
  return 0;
}

/*
  Oracle has many formatting models, we list all but only part of them
  are implemented, because some models depend on oracle functions
  which mariadb is not supported.

  Models for datetime, used by TO_CHAR/TO_DATE. Normal format characters are
  stored as short integer < 128, while format characters are stored as a
  integer > 128
*/

enum enum_tochar_formats
{
  FMT_BASE= 128,
  FMT_AD,
  FMT_AD_DOT,
  FMT_AM,
  FMT_AM_DOT,
  FMT_BC,
  FMT_BC_DOT,
  FMT_CC,
  FMT_SCC,
  FMT_D,
  FMT_DAY,
  FMT_DD,
  FMT_DDD,
  FMT_DL,
  FMT_DS,
  FMT_DY,
  FMT_E,
  FMT_EE,
  FMT_FF,
  FMT_FM,
  FMT_FX,
  FMT_HH,
  FMT_HH12,
  FMT_HH24,
  FMT_IW,
  FMT_I,
  FMT_IY,
  FMT_IYY,
  FMT_IYYY,
  FMT_J,
  FMT_MI,
  FMT_MM,
  FMT_MON,
  FMT_MONTH,
  FMT_PM,
  FMT_PM_DOT,
  FMT_RM,
  FMT_RR,
  FMT_RRRR,
  FMT_SS,
  FMT_SSSSSS,
  FMT_TS,
  FMT_TZD,
  FMT_TZH,
  FMT_TZM,
  FMT_TZR,
  FMT_W,
  FMT_WW,
  FMT_X,
  FMT_Y,
  FMT_YY,
  FMT_YYY,
  FMT_YYYY,
  FMT_YYYY_COMMA,
  FMT_YEAR,
  FMT_SYYYY,
  FMT_SYEAR
};

/**
   Flip 'quotation_flag' if we found a quote (") character.

   @param cftm             Character or FMT... format descriptor
   @param quotation_flag   Points to 'true' if we are inside a quoted string

   @return true  If we are inside a quoted string or if we found a '"' character
   @return false Otherwise
*/

static inline bool check_quotation(uint16 cfmt, bool *quotation_flag)
{
  if (cfmt == '"')
  {
    *quotation_flag= !*quotation_flag;
    return true;
  }
  return *quotation_flag;
}

#define INVALID_CHARACTER(x) (((x) >= 'A' && (x) <= 'Z') ||((x) >= '0' && (x) <= '9') || (x) >= 127 || ((x) < 32))


/**
  Special characters are directly output in the result

  @return 0  If found not acceptable character
  @return #  Number of copied characters
*/

static uint parse_special(char cfmt, const char *ptr, const char *end,
                         uint16 *array)
{
  int offset= 0;
  char tmp1;

  /* Non-printable character and Multibyte encoded characters */
  if (INVALID_CHARACTER(cfmt))
    return 0;

  /*
   * '&' with text is used for variable input, but '&' with other
   * special charaters like '|'. '*' is used as separator
   */
  if (cfmt == '&' && ptr + 1 < end)
  {
    tmp1= my_toupper(system_charset_info, *(ptr+1));
    if (tmp1 >= 'A' && tmp1 <= 'Z')
      return 0;
  }

  do {
    /*
      Continuously store the special characters in fmt_array until non-special
      characters appear
     */
    *array++= (uint16) (uchar) *ptr++;
    offset++;
    if (ptr == end)
      break;
    tmp1= my_toupper(system_charset_info, *ptr);
  } while (!INVALID_CHARACTER(tmp1) && tmp1 != '"');
  return offset;
}


/**
  Parse the format string, convert it to an compact array and calculate the
  length of output string

  @param format   Format string
  @param fmt_len  Function will store max length of formated date string here

  @return 0 ok. fmt_len is updated
  @return 1 error.  In this case 'warning_string' is set to error message
*/

bool Item_func_tochar::parse_format_string(const String *format, uint *fmt_len)
{
  const char *ptr, *end;
  uint16 *tmp_fmt= fmt_array;
  uint tmp_len= 0;
  int offset= 0;
  bool quotation_flag= false;

  ptr= format->ptr();
  end= ptr + format->length();

  if (format->length() > MAX_DATETIME_FORMAT_MODEL_LEN)
  {
    warning_message.append(STRING_WITH_LEN("datetime format string is too "
                                           "long"));
    return 1;
  }

  for (; ptr < end; ptr++, tmp_fmt++)
  {
    uint ulen;
    char cfmt, next_char;

    cfmt= my_toupper(system_charset_info, *ptr);

    /*
      Oracle datetime format support text in double quotation marks like
      'YYYY"abc"MM"xyz"DD', When this happens, store the text and quotation
      marks, and use the text as a separator in make_date_time_oracle.

      NOTE: the quotation mark is not print in return value. for example:
      select TO_CHAR(sysdate, 'YYYY"abc"MM"xyzDD"') will return 2021abc01xyz11
     */
    if (check_quotation(cfmt, &quotation_flag))
    {
      *tmp_fmt= *ptr;
      tmp_len+= 1;
      continue;
    }

    switch (cfmt) {
    case 'A':                                   // AD/A.D./AM/A.M.
      if (ptr+1 >= end)
        goto error;
      next_char= my_toupper(system_charset_info, *(ptr+1));
      if (next_char == 'D')
      {
        *tmp_fmt= FMT_AD;
        ptr+= 1;
        tmp_len+= 2;
      }
      else if (next_char == 'M')
      {
        *tmp_fmt= FMT_AM;
        ptr+= 1;
        tmp_len+= 2;
      }
      else if (next_char == '.' && ptr+3 < end && *(ptr+3) == '.')
      {
        if (my_toupper(system_charset_info, *(ptr+2)) == 'D')
        {
          *tmp_fmt= FMT_AD_DOT;
          ptr+= 3;
          tmp_len+= 4;
        }
        else if (my_toupper(system_charset_info, *(ptr+2)) == 'M')
        {
          *tmp_fmt= FMT_AM_DOT;
          ptr+= 3;
          tmp_len+= 4;
        }
        else
          goto error;
      }
      else
        goto error;
      break;
    case 'B':                                     // BC and B.C
      if (ptr+1 >= end)
        goto error;
      next_char= my_toupper(system_charset_info, *(ptr+1));
      if (next_char == 'C')
      {
        *tmp_fmt= FMT_BC;
        ptr+= 1;
        tmp_len+= 2;
      }
      else if (next_char == '.' && ptr+3 < end &&
               my_toupper(system_charset_info, *(ptr+2)) == 'C' &&
               *(ptr+3) == '.')
      {
        *tmp_fmt= FMT_BC_DOT;
        ptr+= 3;
        tmp_len+= 4;
      }
      else
        goto error;
      break;
    case 'P':                                   // PM or P.M.
      next_char= my_toupper(system_charset_info, *(ptr+1));
      if (next_char == 'M')
      {
        *tmp_fmt= FMT_PM;
        ptr+= 1;
        tmp_len+= 2;
      }
      else if (next_char == '.' &&
               my_toupper(system_charset_info, *(ptr+2)) == 'M' &&
               my_toupper(system_charset_info, *(ptr+3)) == '.')
      {
        *tmp_fmt= FMT_PM_DOT;
        ptr+= 3;
        tmp_len+= 4;
      }
      else
        goto error;
      break;
    case 'Y':                                   // Y, YY, YYY o YYYYY
      if (ptr + 1 == end || my_toupper(system_charset_info, *(ptr+1)) != 'Y')
      {
        *tmp_fmt= FMT_Y;
        tmp_len+= 1;
        break;
      }
      if (ptr + 2 == end ||
          my_toupper(system_charset_info, *(ptr+2)) != 'Y') /* YY */
      {
        *tmp_fmt= FMT_YY;
        ulen= 2;
      }
      else
      {
        if (ptr + 3 < end && my_toupper(system_charset_info, *(ptr+3)) == 'Y')
        {
          *tmp_fmt= FMT_YYYY;
          ulen= 4;
        }
        else
        {
          *tmp_fmt= FMT_YYY;
          ulen= 3;
        }
      }
      ptr+= ulen-1;
      tmp_len+= ulen;
      break;

    case 'R':                                   // RR or RRRR
      if (ptr + 1 == end || my_toupper(system_charset_info, *(ptr+1)) != 'R')
        goto error;

      if (ptr + 2 == end || my_toupper(system_charset_info, *(ptr+2)) != 'R')
      {
        *tmp_fmt= FMT_RR;
        ulen= 2;
      }
      else
      {
        if (ptr + 3 >= end || my_toupper(system_charset_info, *(ptr+3)) != 'R')
          goto error;
        *tmp_fmt= FMT_RRRR;
        ulen= 4;
      }
      ptr+= ulen-1;
      tmp_len+= ulen;
      break;
    case 'M':
    {
      char tmp1;
      if (ptr + 1 >= end)
        goto error;

      tmp1= my_toupper(system_charset_info, *(ptr+1));
      if (tmp1 == 'M')
      {
        *tmp_fmt= FMT_MM;
        tmp_len+= 2;
        ptr+= 1;
      }
      else if (tmp1 == 'I')
      {
        *tmp_fmt= FMT_MI;
        tmp_len+= 2;
        ptr+= 1;
      }
      else if (tmp1 == 'O')
      {
        if (ptr + 2 >= end)
          goto error;
        char tmp2= my_toupper(system_charset_info, *(ptr+2));
        if (tmp2 != 'N')
          goto error;

        if (ptr + 4 >= end ||
            my_toupper(system_charset_info, *(ptr+3)) != 'T' ||
            my_toupper(system_charset_info, *(ptr+4)) != 'H')
        {
          *tmp_fmt= FMT_MON;
          tmp_len+= 3;
          ptr+= 2;
        }
        else
        {
          *tmp_fmt= FMT_MONTH;
          tmp_len+= (locale->max_month_name_length *
                     my_charset_utf8mb3_bin.mbmaxlen);
          ptr+= 4;
        }
      }
      else
        goto error;
    }
    break;
    case 'D':                                   // DD, DY, or DAY
    {
      if (ptr + 1 >= end)
        goto error;
      char tmp1= my_toupper(system_charset_info, *(ptr+1));

      if (tmp1 == 'D')
      {
        *tmp_fmt= FMT_DD;
        tmp_len+= 2;
      }
      else if (tmp1 == 'Y')
      {
        *tmp_fmt= FMT_DY;
        tmp_len+= 3;
      }
      else if (tmp1 == 'A')                     // DAY
      {
        if (ptr + 2 == end || my_toupper(system_charset_info, *(ptr+2)) != 'Y')
          goto error;
        *tmp_fmt= FMT_DAY;
        tmp_len+= locale->max_day_name_length * my_charset_utf8mb3_bin.mbmaxlen;
        ptr+= 1;
      }
      else
        goto error;
      ptr+= 1;
    }
    break;
    case 'H':                                   // HH, HH12 or HH23
    {
      char tmp1, tmp2, tmp3;
      if (ptr + 1 >= end)
        goto error;
      tmp1= my_toupper(system_charset_info, *(ptr+1));

      if (tmp1 != 'H')
        goto error;

      if (ptr+3 >= end)
      {
        *tmp_fmt= FMT_HH;
        ptr+= 1;
      }
      else
      {
        tmp2= *(ptr+2);
        tmp3= *(ptr+3);

        if (tmp2 == '1' && tmp3 == '2')
        {
          *tmp_fmt= FMT_HH12;
          ptr+= 3;
        }
        else if (tmp2 == '2' && tmp3 == '4')
        {
          *tmp_fmt= FMT_HH24;
          ptr+= 3;
        }
        else
        {
          *tmp_fmt= FMT_HH;
          ptr+= 1;
        }
      }
      tmp_len+= 2;
      break;
    }
    case 'S':                                   // SS
      if (ptr + 1 == end || my_toupper(system_charset_info, *(ptr+1)) != 'S')
        goto error;

      *tmp_fmt= FMT_SS;
      tmp_len+= 2;
      ptr+= 1;
      break;
    case '|':
      /*
        If only one '|' just ignore it, else append others, for example:
        TO_CHAR('2000-11-05', 'YYYY|MM||||DD') --> 200011|||05
      */
      if (ptr + 1 == end || *(ptr+1) != '|')
      {
        tmp_fmt--;
        break;
      }
      ptr++;                                    // Skip first '|'
      do
      {
        *tmp_fmt++= *ptr++;
        tmp_len++;
      } while ((ptr < end) && *ptr == '|');
      ptr--;                                    // Fix ptr for above for loop
      tmp_fmt--;
      break;

    default:
      offset= parse_special(cfmt, ptr, end, tmp_fmt);
      if (!offset)
        goto error;
      /* ptr++ is in the for loop, so we must move ptr to offset-1 */
      ptr+= (offset-1);
      tmp_fmt+= (offset-1);
      tmp_len+= offset;
      break;
    }
  }
  *fmt_len= tmp_len;
  *tmp_fmt= 0;
  return 0;

error:
  warning_message.append(STRING_WITH_LEN("date format not recognized at "));
  warning_message.append(ptr, MY_MIN(8, end- ptr));
  return 1;
}


static inline bool append_val(int val, int size, String *str)
{
  ulong len= 0;
  char intbuff[15];

  len= (ulong) (int10_to_str(val, intbuff, 10) - intbuff);
  return str->append_with_prefill(intbuff, len, size, '0');
}


static bool make_date_time_oracle(const uint16 *fmt_array,
                                  const MYSQL_TIME *l_time,
                                  const MY_LOCALE *locale,
                                  String *str)
{
  bool quotation_flag= false;
  const uint16 *ptr= fmt_array;
  uint hours_i;
  uint weekday;

  str->length(0);

  while (*ptr)
  {
    if (check_quotation(*ptr, &quotation_flag))
    {
      /* don't display '"' in the result, so if it is '"', skip it */
      if (*ptr != '"')
      {
        DBUG_ASSERT(*ptr <= 255);
        str->append((char) *ptr);
      }
      ptr++;
      continue;
    }

    switch (*ptr) {

    case FMT_AM:
    case FMT_PM:
      if (l_time->hour > 11)
        str->append("PM", 2);
      else
        str->append("AM", 2);
      break;

    case FMT_AM_DOT:
    case FMT_PM_DOT:
      if (l_time->hour > 11)
        str->append(STRING_WITH_LEN("P.M."));
      else
        str->append(STRING_WITH_LEN("A.M."));
      break;

    case FMT_AD:
    case FMT_BC:
      if (l_time->year > 0)
        str->append(STRING_WITH_LEN("AD"));
      else
        str->append(STRING_WITH_LEN("BC"));
      break;

    case FMT_AD_DOT:
    case FMT_BC_DOT:
      if (l_time->year > 0)
        str->append(STRING_WITH_LEN("A.D."));
      else
        str->append(STRING_WITH_LEN("B.C."));
      break;

    case FMT_Y:
      if (append_val(l_time->year%10, 1, str))
        goto err_exit;
      break;

    case FMT_YY:
    case FMT_RR:
      if (append_val(l_time->year%100, 2, str))
        goto err_exit;
      break;

    case FMT_YYY:
      if (append_val(l_time->year%1000, 3, str))
        goto err_exit;
      break;

    case FMT_YYYY:
    case FMT_RRRR:
      if (append_val(l_time->year, 4, str))
        goto err_exit;
      break;

    case FMT_MM:
      if (append_val(l_time->month, 2, str))
        goto err_exit;
      break;

    case FMT_MON:
      {
        if (l_time->month == 0)
        {
          str->append("00", 2);
        }
        else
        {
          const char *month_name= (locale->ab_month_names->
                                   type_names[l_time->month-1]);
          size_t m_len= strlen(month_name);
          str->append(month_name, m_len, system_charset_info);
        }
      }
      break;

    case FMT_MONTH:
      {
        if (l_time->month == 0)
        {
          str->append("00", 2);
        }
        else
        {
          const char *month_name= (locale->month_names->
                                   type_names[l_time->month-1]);
          size_t month_byte_len= strlen(month_name);
          size_t month_char_len;
          str->append(month_name, month_byte_len, system_charset_info);
          month_char_len= my_numchars_mb(&my_charset_utf8mb3_general_ci,
                                         month_name, month_name +
                                         month_byte_len);
          if (str->fill(str->length() + locale->max_month_name_length -
                        month_char_len, ' '))
            goto err_exit;
        }
      }
      break;

    case FMT_DD:
      if (append_val(l_time->day, 2, str))
        goto err_exit;
      break;

    case FMT_DY:
      {
        if (l_time->day == 0)
          str->append("00", 2);
        else
        {
          weekday= calc_weekday(calc_daynr(l_time->year,l_time->month,
                                          l_time->day), 0);
          const char *day_name= locale->ab_day_names->type_names[weekday];
          str->append(day_name, strlen(day_name), system_charset_info);
        }
      }
      break;

    case FMT_DAY:
      {
        if (l_time->day == 0)
          str->append("00", 2, system_charset_info);
        else
        {
          const char *day_name;
          size_t day_byte_len, day_char_len;
          weekday=calc_weekday(calc_daynr(l_time->year,l_time->month,
                                          l_time->day), 0);
          day_name= locale->day_names->type_names[weekday];
          day_byte_len= strlen(day_name);
          str->append(day_name, day_byte_len, system_charset_info);
          day_char_len= my_numchars_mb(&my_charset_utf8mb3_general_ci,
                                       day_name, day_name + day_byte_len);
          if (str->fill(str->length() + locale->max_day_name_length -
                        day_char_len, ' '))
            goto err_exit;
        }
      }
      break;

    case FMT_HH12:
    case FMT_HH:
      hours_i= (l_time->hour%24 + 11)%12+1;
      if (append_val(hours_i, 2, str))
        goto err_exit;
      break;

    case FMT_HH24:
      if (append_val(l_time->hour, 2, str))
        goto err_exit;
      break;

    case FMT_MI:
      if (append_val(l_time->minute, 2, str))
        goto err_exit;
      break;

    case FMT_SS:
      if (append_val(l_time->second, 2, str))
        goto err_exit;
      break;

    default:
      str->append((char) *ptr);
    }

    ptr++;
  };
  return false;

err_exit:
  return true;
}


bool Item_func_tochar::fix_length_and_dec(THD *thd)
{
  CHARSET_INFO *cs= thd->variables.collation_connection;
  Item *arg1= args[1]->this_item();
  my_repertoire_t repertoire= arg1->collation.repertoire;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> buffer;
  String *str;

  locale= thd->variables.lc_time_names;
  if (!thd->variables.lc_time_names->is_ascii)
    repertoire|= MY_REPERTOIRE_EXTENDED;
  collation.set(cs, arg1->collation.derivation, repertoire);

  /* first argument must be datetime or string */
  enum_field_types arg0_mysql_type= args[0]->field_type();

  max_length= 0;
  switch (arg0_mysql_type) {
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_STRING:
    break;
  default:
  {
    my_printf_error(ER_STD_INVALID_ARGUMENT,
                    ER(ER_STD_INVALID_ARGUMENT),
                    MYF(0),
                    "data type of first argument must be type "
                    "date/datetime/time or string",
                    func_name());
    return TRUE;
  }
  }
  if (args[1]->basic_const_item() && (str= args[1]->val_str(&buffer)))
  {
    uint ulen;
    fixed_length= 1;
    if (parse_format_string(str, &ulen))
    {
      my_printf_error(ER_STD_INVALID_ARGUMENT,
                      ER(ER_STD_INVALID_ARGUMENT),
                      MYF(0),
                      warning_message.c_ptr(),
                      func_name());
      return TRUE;
    }
    max_length= (uint32) (ulen * collation.collation->mbmaxlen);
  }
  else
  {
    fixed_length= 0;
    max_length= (uint32) MY_MIN(arg1->max_length * 10 *
                                collation.collation->mbmaxlen,
                                MAX_BLOB_WIDTH);
  }
  set_maybe_null();
  return FALSE;
}


String *Item_func_tochar::val_str(String* str)
 {
  THD *thd= current_thd;
  StringBuffer<64> format_buffer;
  String *format;
  MYSQL_TIME l_time;
  const MY_LOCALE *lc= locale;
  date_conv_mode_t mode= TIME_CONV_NONE;
  size_t max_result_length= max_length;

  if (warning_message.length())
    goto null_date;

  if ((null_value= args[0]->get_date(thd, &l_time,
                                     Temporal::Options(mode, thd))))
    return 0;

  if (!fixed_length)
  {
    uint ulen;
    if (!(format= args[1]->val_str(&format_buffer)) || !format->length() ||
        parse_format_string(format, &ulen))
      goto null_date;
    max_result_length= ((size_t) ulen) * collation.collation->mbmaxlen;
  }

  if (str->alloc(max_result_length))
    goto null_date;

  /* Create the result string */
  str->set_charset(collation.collation);
  if (!make_date_time_oracle(fmt_array, &l_time, lc, str))
    return str;

null_date:

  if (warning_message.length())
  {
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_WARN,
                        ER_STD_INVALID_ARGUMENT,
                        ER_THD(thd, ER_STD_INVALID_ARGUMENT),
                        warning_message.c_ptr(),
                        func_name());
    if (!fixed_length)
      warning_message.length(0);
  }

  null_value= 1;
  return 0;
}


bool Item_func_from_unixtime::fix_length_and_dec(THD *thd)
{
  thd->time_zone_used= 1;
  tz= thd->variables.time_zone;
  Type_std_attributes::set(
    Type_temporal_attributes_not_fixed_dec(MAX_DATETIME_WIDTH,
                                           args[0]->decimals, false),
    DTCollation_numeric());
  set_maybe_null();
  return FALSE;
}


bool Item_func_from_unixtime::get_date(THD *thd, MYSQL_TIME *ltime,
				       date_mode_t fuzzydate __attribute__((unused)))
{
  bzero((char *)ltime, sizeof(*ltime));
  ltime->time_type= MYSQL_TIMESTAMP_TIME;

  VSec9 sec(thd, args[0], "unixtime", TIMESTAMP_MAX_VALUE);
  DBUG_ASSERT(sec.is_null() || sec.sec() <= TIMESTAMP_MAX_VALUE);

  if (sec.is_null() || sec.truncated() || sec.neg())
    return (null_value= 1);

  sec.round(MY_MIN(decimals, TIME_SECOND_PART_DIGITS), thd->temporal_round_mode());
  if (sec.sec() > TIMESTAMP_MAX_VALUE)
    return (null_value= true); // Went out of range after rounding

  tz->gmt_sec_to_TIME(ltime, (my_time_t) sec.sec());
  ltime->second_part= sec.usec();

  return (null_value= 0);
}


bool Item_func_convert_tz::get_date(THD *thd, MYSQL_TIME *ltime,
                                    date_mode_t fuzzydate __attribute__((unused)))
{
  my_time_t my_time_tmp;
  String str;

  if (!from_tz_cached)
  {
    from_tz= my_tz_find(thd, args[1]->val_str_ascii(&str));
    from_tz_cached= args[1]->const_item();
  }

  if (!to_tz_cached)
  {
    to_tz= my_tz_find(thd, args[2]->val_str_ascii(&str));
    to_tz_cached= args[2]->const_item();
  }

  if ((null_value= (from_tz == 0 || to_tz == 0)))
    return true;

  Datetime::Options opt(TIME_NO_ZEROS, thd);
  Datetime *dt= new(ltime) Datetime(thd, args[0], opt);
  if ((null_value= !dt->is_valid_datetime()))
    return true;

  {
    uint not_used;
    my_time_tmp= from_tz->TIME_to_gmt_sec(ltime, &not_used);
    ulong sec_part= ltime->second_part;
    /* my_time_tmp is guaranteed to be in the allowed range */
    if (my_time_tmp)
      to_tz->gmt_sec_to_TIME(ltime, my_time_tmp);
    /* we rely on the fact that no timezone conversion can change sec_part */
    ltime->second_part= sec_part;
  }

  return (null_value= 0);
}


void Item_func_convert_tz::cleanup()
{
  from_tz_cached= to_tz_cached= 0;
  Item_datetimefunc::cleanup();
}


bool Item_date_add_interval::fix_length_and_dec(THD *thd)
{
  enum_field_types arg0_field_type;

  if (!args[0]->type_handler()->is_traditional_scalar_type())
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
             args[0]->type_handler()->name().ptr(),
             "interval", func_name());
    return TRUE;
  }
  /*
    The field type for the result of an Item_datefunc is defined as
    follows:

    - If first arg is a MYSQL_TYPE_DATETIME result is MYSQL_TYPE_DATETIME
    - If first arg is a MYSQL_TYPE_DATE and the interval type uses hours,
      minutes or seconds then type is MYSQL_TYPE_DATETIME
      otherwise it's MYSQL_TYPE_DATE
    - if first arg is a MYSQL_TYPE_TIME and the interval type isn't using
      anything larger than days, then the result is MYSQL_TYPE_TIME,
      otherwise - MYSQL_TYPE_DATETIME.
    - Otherwise the result is MYSQL_TYPE_STRING
      (This is because you can't know if the string contains a DATE,
      MYSQL_TIME or DATETIME argument)
  */
  arg0_field_type= args[0]->field_type();

  if (arg0_field_type == MYSQL_TYPE_DATETIME ||
      arg0_field_type == MYSQL_TYPE_TIMESTAMP)
  {
    set_func_handler(&func_handler_date_add_interval_datetime);
  }
  else if (arg0_field_type == MYSQL_TYPE_DATE)
  {
    if (int_type <= INTERVAL_DAY || int_type == INTERVAL_YEAR_MONTH)
      set_func_handler(&func_handler_date_add_interval_date);
    else
      set_func_handler(&func_handler_date_add_interval_datetime);
  }
  else if (arg0_field_type == MYSQL_TYPE_TIME)
  {
    if (int_type >= INTERVAL_DAY && int_type != INTERVAL_YEAR_MONTH)
      set_func_handler(&func_handler_date_add_interval_time);
    else
      set_func_handler(&func_handler_date_add_interval_datetime_arg0_time);
  }
  else
  {
    set_func_handler(&func_handler_date_add_interval_string);
  }
  set_maybe_null();
  return m_func_handler->fix_length_and_dec(this);
}


bool Func_handler_date_add_interval_datetime_arg0_time::
       get_date(THD *thd, Item_handled_func *item,
                MYSQL_TIME *to, date_mode_t fuzzy) const
{
  // time_expr + INTERVAL {YEAR|QUARTER|MONTH|WEEK|YEAR_MONTH}
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_DATETIME_FUNCTION_OVERFLOW,
                      ER_THD(thd, ER_DATETIME_FUNCTION_OVERFLOW), "time");
  return (item->null_value= true);
}


bool Item_date_add_interval::eq(const Item *item, bool binary_cmp) const
{
  if (!Item_func::eq(item, binary_cmp))
    return 0;
  Item_date_add_interval *other= (Item_date_add_interval*) item;
  return ((int_type == other->int_type) &&
          (date_sub_interval == other->date_sub_interval));
}

/*
   'interval_names' reflects the order of the enumeration interval_type.
   See item_timefunc.h
 */

static const char *interval_names[]=
{
  "year", "quarter", "month", "week", "day",  
  "hour", "minute", "second", "microsecond",
  "year_month", "day_hour", "day_minute", 
  "day_second", "hour_minute", "hour_second",
  "minute_second", "day_microsecond",
  "hour_microsecond", "minute_microsecond",
  "second_microsecond"
};

void Item_date_add_interval::print(String *str, enum_query_type query_type)
{
  args[0]->print_parenthesised(str, query_type, INTERVAL_PRECEDENCE);
  static LEX_CSTRING minus_interval= { STRING_WITH_LEN(" - interval ") };
  static LEX_CSTRING plus_interval=  { STRING_WITH_LEN(" + interval ") };
  LEX_CSTRING *tmp= date_sub_interval ? &minus_interval : &plus_interval;
  str->append(tmp);
  args[1]->print(str, query_type);
  str->append(' ');
  str->append(interval_names[int_type], strlen(interval_names[int_type]));
}

void Item_extract::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("extract("));
  str->append(interval_names[int_type], strlen(interval_names[int_type]));
  str->append(STRING_WITH_LEN(" from "));
  args[0]->print(str, query_type);
  str->append(')');
}


bool Item_extract::check_arguments() const
{
  if (!args[0]->type_handler()->can_return_extract_source(int_type))
  {
    char tmp[64];
    my_snprintf(tmp, sizeof(tmp), "extract(%s)", interval_names[int_type]);
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             args[0]->type_handler()->name().ptr(), tmp);
    return true;
  }
  return false;
}


bool Item_extract::fix_length_and_dec(THD *thd)
{
  set_maybe_null(); // If wrong date
  uint32 daylen= args[0]->cmp_type() == TIME_RESULT ? 2 :
                 TIME_MAX_INTERVAL_DAY_CHAR_LENGTH;
  switch (int_type) {
  case INTERVAL_YEAR:             set_date_length(4); break; // YYYY
  case INTERVAL_YEAR_MONTH:       set_date_length(6); break; // YYYYMM
  case INTERVAL_QUARTER:          set_date_length(2); break; // 1..4
  case INTERVAL_MONTH:            set_date_length(2); break; // MM
  case INTERVAL_WEEK:             set_date_length(2); break; // 0..52
  case INTERVAL_DAY:              set_day_length(daylen); break; // DD
  case INTERVAL_DAY_HOUR:         set_day_length(daylen+2); break; // DDhh
  case INTERVAL_DAY_MINUTE:       set_day_length(daylen+4); break; // DDhhmm
  case INTERVAL_DAY_SECOND:       set_day_length(daylen+6); break; // DDhhmmss
  case INTERVAL_HOUR:             set_time_length(2); break; // hh
  case INTERVAL_HOUR_MINUTE:      set_time_length(4); break; // hhmm
  case INTERVAL_HOUR_SECOND:      set_time_length(6); break; // hhmmss
  case INTERVAL_MINUTE:           set_time_length(2); break; // mm
  case INTERVAL_MINUTE_SECOND:    set_time_length(4); break; // mmss
  case INTERVAL_SECOND:           set_time_length(2); break; // ss
  case INTERVAL_MICROSECOND:      set_time_length(6); break; // ffffff
  case INTERVAL_DAY_MICROSECOND:  set_time_length(daylen+12); break; // DDhhmmssffffff
  case INTERVAL_HOUR_MICROSECOND: set_time_length(12); break; // hhmmssffffff
  case INTERVAL_MINUTE_MICROSECOND: set_time_length(10); break; // mmssffffff
  case INTERVAL_SECOND_MICROSECOND: set_time_length(8); break; // ssffffff
  case INTERVAL_LAST: DBUG_ASSERT(0); break; /* purecov: deadcode */
  }
  return FALSE;
}


uint Extract_source::week(THD *thd) const
{
  DBUG_ASSERT(is_valid_extract_source());
  uint year;
  ulong week_format= current_thd->variables.default_week_format;
  return calc_week(this, week_mode(week_format), &year);
}


longlong Item_extract::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Extract_source dt(thd, args[0], m_date_mode);
  if ((null_value= !dt.is_valid_extract_source()))
    return 0;
  switch (int_type) {
  case INTERVAL_YEAR:                return dt.year();
  case INTERVAL_YEAR_MONTH:          return dt.year_month();
  case INTERVAL_QUARTER:             return dt.quarter();
  case INTERVAL_MONTH:               return dt.month();
  case INTERVAL_WEEK:                return dt.week(thd);
  case INTERVAL_DAY:                 return dt.day();
  case INTERVAL_DAY_HOUR:            return dt.day_hour();
  case INTERVAL_DAY_MINUTE:          return dt.day_minute();
  case INTERVAL_DAY_SECOND:          return dt.day_second();
  case INTERVAL_HOUR:                return dt.hour();
  case INTERVAL_HOUR_MINUTE:         return dt.hour_minute();
  case INTERVAL_HOUR_SECOND:         return dt.hour_second();
  case INTERVAL_MINUTE:              return dt.minute();
  case INTERVAL_MINUTE_SECOND:       return dt.minute_second();
  case INTERVAL_SECOND:              return dt.second();
  case INTERVAL_MICROSECOND:         return dt.microsecond();
  case INTERVAL_DAY_MICROSECOND:     return dt.day_microsecond();
  case INTERVAL_HOUR_MICROSECOND:    return dt.hour_microsecond();
  case INTERVAL_MINUTE_MICROSECOND:  return dt.minute_microsecond();
  case INTERVAL_SECOND_MICROSECOND:  return dt.second_microsecond();
  case INTERVAL_LAST: DBUG_ASSERT(0); break;  /* purecov: deadcode */
  }
  return 0;                                        // Impossible
}

bool Item_extract::eq(const Item *item, bool binary_cmp) const
{
  if (this == item)
    return 1;
  if (item->type() != FUNC_ITEM ||
      functype() != ((Item_func*)item)->functype())
    return 0;

  Item_extract* ie= (Item_extract*)item;
  if (ie->int_type != int_type)
    return 0;

  if (!args[0]->eq(ie->args[0], binary_cmp))
      return 0;
  return 1;
}


bool Item_char_typecast::eq(const Item *item, bool binary_cmp) const
{
  if (this == item)
    return 1;
  if (item->type() != FUNC_ITEM ||
      functype() != ((Item_func*)item)->functype())
    return 0;

  Item_char_typecast *cast= (Item_char_typecast*)item;
  if (cast_length != cast->cast_length ||
      cast_cs     != cast->cast_cs)
    return 0;

  if (!args[0]->eq(cast->args[0], binary_cmp))
      return 0;
  return 1;
}

void Item_func::print_cast_temporal(String *str, enum_query_type query_type)
{
  char buf[32];
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as "));
  const Name name= type_handler()->name();
  str->append(name.ptr(), name.length());
  if (decimals && decimals != NOT_FIXED_DEC)
  {
    str->append('(');
    size_t length= (size_t) (longlong10_to_str(decimals, buf, -10) - buf);
    str->append(buf, length);
    str->append(')');
  }
  str->append(')');
}


void Item_char_typecast::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as char"));
  if (cast_length != ~0U)
  {
    char buf[20];
    size_t length= (size_t) (longlong10_to_str(cast_length, buf, 10) - buf);
    str->append('(');
    str->append(buf, length);
    str->append(')');
  }
  if (cast_cs)
  {
    str->append(STRING_WITH_LEN(" charset "));
    str->append(cast_cs->cs_name);
  }
  str->append(')');
}


void Item_char_typecast::check_truncation_with_warn(String *src, size_t dstlen)
{
  if (dstlen < src->length())
  {
    THD *thd= current_thd;
    char char_type[40];
    ErrConvString err(src);
    bool save_abort_on_warning= thd->abort_on_warning;
    thd->abort_on_warning&= !m_suppress_warning_to_error_escalation;
    my_snprintf(char_type, sizeof(char_type), "%s(%lu)",
                cast_cs == &my_charset_bin ? "BINARY" : "CHAR",
                (ulong) cast_length);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_TRUNCATED_WRONG_VALUE,
                        ER_THD(thd, ER_TRUNCATED_WRONG_VALUE), char_type,
                        err.ptr());
    thd->abort_on_warning= save_abort_on_warning;
  }
}


String *Item_char_typecast::reuse(String *src, size_t length)
{
  DBUG_ASSERT(length <= src->length());
  check_truncation_with_warn(src, length);
  tmp_value.set(src->ptr(), length, cast_cs);
  return &tmp_value;
}


/*
  Make a copy, to handle conversion or fix bad bytes.
*/
String *Item_char_typecast::copy(String *str, CHARSET_INFO *strcs)
{
  String_copier_for_item copier(current_thd);
  if (copier.copy_with_warn(cast_cs, &tmp_value, strcs,
                            str->ptr(), str->length(), cast_length))
  {
    null_value= 1; // EOM
    return 0;
  }
  check_truncation_with_warn(str, (uint)(copier.source_end_pos() - str->ptr()));
  return &tmp_value;
}


uint Item_char_typecast::adjusted_length_with_warn(uint length)
{
  if (length <= current_thd->variables.max_allowed_packet)
    return length;

  THD *thd= current_thd;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
                      ER_THD(thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
                      cast_cs == &my_charset_bin ?
                      "cast_as_binary" : func_name(),
                      thd->variables.max_allowed_packet);
  return thd->variables.max_allowed_packet;
}


String *Item_char_typecast::val_str_generic(String *str)
{
  DBUG_ASSERT(fixed());
  String *res;

  if (has_explicit_length())
    cast_length= adjusted_length_with_warn(cast_length);

  if (!(res= args[0]->val_str(str)))
  {
    null_value= 1;
    return 0;
  }

  if (cast_cs == &my_charset_bin &&
      has_explicit_length() &&
      cast_length > res->length())
  {
    // Special case: pad binary value with trailing 0x00
    DBUG_ASSERT(cast_length <= current_thd->variables.max_allowed_packet);
    if (res->alloced_length() < cast_length)
    {
      str_value.alloc(cast_length);
      str_value.copy(*res);
      res= &str_value;
    }
    bzero((char*) res->ptr() + res->length(), cast_length - res->length());
    res->length(cast_length);
    res->set_charset(&my_charset_bin);
  }
  else
  {
    /*
      from_cs is 0 in the case where the result set may vary between calls,
      for example with dynamic columns.
    */
    CHARSET_INFO *cs= from_cs ? from_cs : res->charset();
    if (!charset_conversion)
    {
      // Try to reuse the original string (if well formed).
      Well_formed_prefix prefix(cs, res->ptr(), res->end(), cast_length);
      if (!prefix.well_formed_error_pos())
        res= reuse(res, prefix.length());
      goto end;
    }
    // Character set conversion, or bad bytes were found.
    if (!(res= copy(res, cs)))
      return 0;
  }

end:
  return ((null_value= (res->length() >
                        adjusted_length_with_warn(res->length())))) ? 0 : res;
}


String *Item_char_typecast::val_str_binary_from_native(String *str)
{
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(cast_cs == &my_charset_bin);
  NativeBuffer<STRING_BUFFER_USUAL_SIZE> native;

  if (args[0]->val_native(current_thd, &native))
  {
    null_value= 1;
    return 0;
  }

  if (has_explicit_length())
  {
    cast_length= adjusted_length_with_warn(cast_length);
    if (cast_length > native.length())
    {
      // add trailing 0x00s
      DBUG_ASSERT(cast_length <= current_thd->variables.max_allowed_packet);
      str->alloc(cast_length);
      str->copy(native.ptr(), native.length(), &my_charset_bin);
      bzero((char*) str->end(), cast_length - str->length());
      str->length(cast_length);
    }
    else
      str->copy(native.ptr(), cast_length, &my_charset_bin);
  }
  else
    str->copy(native.ptr(), native.length(), &my_charset_bin);

  return ((null_value= (str->length() >
                        adjusted_length_with_warn(str->length())))) ? 0 : str;
}


class Item_char_typecast_func_handler: public Item_handled_func::Handler_str
{
public:
  const Type_handler *return_type_handler(const Item_handled_func *item) const
  {
    return Type_handler::string_type_handler(item->max_length);
  }
  const Type_handler *
    type_handler_for_create_select(const Item_handled_func *item) const
  {
    return return_type_handler(item)->type_handler_for_tmp_table(item);
  }

  bool fix_length_and_dec(Item_handled_func *item) const
  {
    return false;
  }
  String *val_str(Item_handled_func *item, String *to) const
  {
    DBUG_ASSERT(dynamic_cast<const Item_char_typecast*>(item));
    return static_cast<Item_char_typecast*>(item)->val_str_generic(to);
  }
};


static Item_char_typecast_func_handler item_char_typecast_func_handler;


void Item_char_typecast::fix_length_and_dec_numeric()
{
  fix_length_and_dec_internal(from_cs= cast_cs->mbminlen == 1 ?
                                       cast_cs :
                                       &my_charset_latin1);
  set_func_handler(&item_char_typecast_func_handler);
}


void Item_char_typecast::fix_length_and_dec_generic()
{
  fix_length_and_dec_internal(from_cs= args[0]->dynamic_result() ?
                                       0 :
                                       args[0]->collation.collation);
  set_func_handler(&item_char_typecast_func_handler);
}


void Item_char_typecast::fix_length_and_dec_str()
{
  fix_length_and_dec_generic();
  m_suppress_warning_to_error_escalation= true;
  set_func_handler(&item_char_typecast_func_handler);
}


void
Item_char_typecast::fix_length_and_dec_native_to_binary(uint32 octet_length)
{
  collation.set(&my_charset_bin, DERIVATION_IMPLICIT);
  max_length= has_explicit_length() ? (uint32) cast_length : octet_length;
  if (current_thd->is_strict_mode())
    set_maybe_null();
}


void Item_char_typecast::fix_length_and_dec_internal(CHARSET_INFO *from_cs)
{
  uint32 char_length;
  /* 
     We always force character set conversion if cast_cs
     is a multi-byte character set. It guarantees that the
     result of CAST is a well-formed string.
     For single-byte character sets we allow just to copy
     from the argument. A single-byte character sets string
     is always well-formed. 
     
     There is a special trick to convert form a number to ucs2.
     As numbers have my_charset_bin as their character set,
     it wouldn't do conversion to ucs2 without an additional action.
     To force conversion, we should pretend to be non-binary.
     Let's choose from_cs this way:
     - If the argument in a number and cast_cs is ucs2 (i.e. mbminlen > 1),
       then from_cs is set to latin1, to perform latin1 -> ucs2 conversion.
     - If the argument is a number and cast_cs is ASCII-compatible
       (i.e. mbminlen == 1), then from_cs is set to cast_cs,
       which allows just to take over the args[0]->val_str() result
       and thus avoid unnecessary character set conversion.
     - If the argument is not a number, then from_cs is set to
       the argument's charset.
     - If argument has a dynamic collation (can change from call to call)
       we set from_cs to 0 as a marker that we have to take the collation
       from the result string.

       Note (TODO): we could use repertoire technique here.
  */
  charset_conversion= !from_cs || (cast_cs->mbmaxlen > 1) ||
                      (!my_charset_same(from_cs, cast_cs) &&
                       from_cs != &my_charset_bin &&
                       cast_cs != &my_charset_bin);
  collation.set(cast_cs, DERIVATION_IMPLICIT);
  char_length= ((cast_length != ~0U) ? cast_length :
                args[0]->max_length /
                (cast_cs == &my_charset_bin ? 1 :
                 args[0]->collation.collation->mbmaxlen));
  max_length= char_length * cast_cs->mbmaxlen;
  // Add NULL-ability in strict mode. See Item_str_func::fix_fields()
  if (current_thd->is_strict_mode())
    set_maybe_null();
}


bool Item_time_typecast::get_date(THD *thd, MYSQL_TIME *to, date_mode_t mode)
{
  Time *tm= new(to) Time(thd, args[0], Time::Options_for_cast(mode, thd),
                         MY_MIN(decimals, TIME_SECOND_PART_DIGITS));
  return (null_value= !tm->is_valid_time());
}


Sql_mode_dependency Item_time_typecast::value_depends_on_sql_mode() const
{
  return Item_timefunc::value_depends_on_sql_mode() |
         Sql_mode_dependency(decimals < args[0]->decimals ?
                             MODE_TIME_ROUND_FRACTIONAL : 0, 0);
}


bool Item_date_typecast::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  date_mode_t tmp= (fuzzydate | sql_mode_for_dates(thd)) & ~TIME_TIME_ONLY;
  // Force truncation
  Date *d= new(ltime) Date(thd, args[0], Date::Options(date_conv_mode_t(tmp)));
  return (null_value= !d->is_valid_date());
}


bool Item_datetime_typecast::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  date_mode_t tmp= (fuzzydate | sql_mode_for_dates(thd)) & ~TIME_TIME_ONLY;
  // Force rounding if the current sql_mode says so
  Datetime::Options opt(date_conv_mode_t(tmp), thd);
  Datetime *dt= new(ltime) Datetime(thd, args[0], opt,
                                    MY_MIN(decimals, TIME_SECOND_PART_DIGITS));
  return (null_value= !dt->is_valid_datetime());
}


Sql_mode_dependency Item_datetime_typecast::value_depends_on_sql_mode() const
{
  return Item_datetimefunc::value_depends_on_sql_mode() |
         Sql_mode_dependency(decimals < args[0]->decimals ?
                             MODE_TIME_ROUND_FRACTIONAL : 0, 0);
}


/**
  MAKEDATE(a,b) is a date function that creates a date value 
  from a year and day value.

  NOTES:
    As arguments are integers, we can't know if the year is a 2 digit
    or 4 digit year.  In this case we treat all years < 100 as 2 digit
    years. Ie, this is not safe for dates between 0000-01-01 and
    0099-12-31
*/

bool Item_func_makedate::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  long year, days, daynr=  (long) args[1]->val_int();

  VYear vyear(args[0]);
  if (vyear.is_null() || args[1]->null_value || vyear.truncated() || daynr <= 0)
    goto err;

  if ((year= (long) vyear.year()) < 100)
    year= year_2000_handling(year);
  days= calc_daynr(year,1,1) + daynr - 1;
  if (get_date_from_daynr(days, &ltime->year, &ltime->month, &ltime->day))
    goto err;
  ltime->time_type= MYSQL_TIMESTAMP_DATE;
  ltime->neg= 0;
  ltime->hour= ltime->minute= ltime->second= ltime->second_part= 0;
  return (null_value= 0);

err:
  return (null_value= 1);
}


bool Item_func_add_time::fix_length_and_dec(THD *thd)
{
  enum_field_types arg0_field_type;

  if (!args[0]->type_handler()->is_traditional_scalar_type() ||
      !args[1]->type_handler()->is_traditional_scalar_type())
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
             args[0]->type_handler()->name().ptr(),
             args[1]->type_handler()->name().ptr(), func_name());
    return TRUE;
  }
  /*
    The field type for the result of an Item_func_add_time function is defined
    as follows:

    - If first arg is a MYSQL_TYPE_DATETIME or MYSQL_TYPE_TIMESTAMP 
      result is MYSQL_TYPE_DATETIME
    - If first arg is a MYSQL_TYPE_TIME result is MYSQL_TYPE_TIME
    - Otherwise the result is MYSQL_TYPE_STRING
  */

  arg0_field_type= args[0]->field_type();
  if (arg0_field_type == MYSQL_TYPE_DATE ||
      arg0_field_type == MYSQL_TYPE_DATETIME ||
      arg0_field_type == MYSQL_TYPE_TIMESTAMP)
  {
    set_func_handler(sign > 0 ? &func_handler_add_time_datetime_add :
                                &func_handler_add_time_datetime_sub);
  }
  else if (arg0_field_type == MYSQL_TYPE_TIME)
  {
    set_func_handler(sign > 0 ? &func_handler_add_time_time_add :
                                &func_handler_add_time_time_sub);
  }
  else
  {
    set_func_handler(sign > 0 ? &func_handler_add_time_string_add :
                                &func_handler_add_time_string_sub);
  }

  set_maybe_null();
  return m_func_handler->fix_length_and_dec(this);
}


/**
  TIMEDIFF(t,s) is a time function that calculates the 
  time value between a start and end time.

  t and s: time_or_datetime_expression
  Result: Time value
*/

bool Item_func_timediff::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  int l_sign= 1;
  MYSQL_TIME l_time1,l_time2,l_time3;

  /* the following may be true in, for example, date_add(timediff(...), ... */
  if (fuzzydate & TIME_NO_ZERO_IN_DATE)
    return (null_value= 1);

  if (args[0]->get_time(thd, &l_time1) ||
      args[1]->get_time(thd, &l_time2) ||
      l_time1.time_type != l_time2.time_type)
    return (null_value= 1);

  if (l_time1.neg != l_time2.neg)
    l_sign= -l_sign;

  if (calc_time_diff(&l_time1, &l_time2, l_sign, &l_time3, fuzzydate))
    return (null_value= 1);

  *ltime= l_time3;
  return (null_value= adjust_time_range_with_warn(thd, ltime, decimals));
}


/**
  MAKETIME(h,m,s) is a time function that calculates a time value 
  from the total number of hours, minutes, and seconds.
  Result: Time value
*/

bool Item_func_maketime::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  Longlong_hybrid hour(args[0]->val_int(), args[0]->unsigned_flag);
  longlong minute= args[1]->val_int();
  VSec9 sec(thd, args[2], "seconds", 59);

  DBUG_ASSERT(sec.is_null() || sec.sec() <= 59);
  if (args[0]->null_value || args[1]->null_value || sec.is_null() ||
       minute < 0 || minute > 59 || sec.neg() || sec.truncated())
    return (null_value= 1);

  int warn;
  new(ltime) Time(&warn, hour.neg(), hour.abs(), (uint) minute,
                  sec.to_const_sec9(), thd->temporal_round_mode(), decimals);
  if (warn)
  {
    // use check_time_range() to set ltime to the max value depending on dec
    int unused;
    ltime->hour= TIME_MAX_HOUR + 1;
    check_time_range(ltime, decimals, &unused);
    char buf[28];
    char *ptr= longlong10_to_str(hour.value(), buf, hour.is_unsigned() ? 10 : -10);
    int len = (int)(ptr - buf) + sprintf(ptr, ":%02u:%02u",
                                         (uint) minute, (uint) sec.sec());
    ErrConvString err(buf, len, &my_charset_bin);
    thd->push_warning_truncated_wrong_value("time", err.ptr());
  }

  return (null_value= 0);
}


/**
  MICROSECOND(a) is a function ( extraction) that extracts the microseconds
  from a.

  a: Datetime or time value
  Result: int value
*/

longlong Item_func_microsecond::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  Time tm(thd, args[0], Time::Options_for_cast(thd));
  return ((null_value= !tm.is_valid_time())) ?
         0 : tm.get_mysql_time()->second_part;
}


longlong Item_func_timestamp_diff::val_int()
{
  MYSQL_TIME ltime1, ltime2;
  ulonglong seconds;
  ulong microseconds;
  long months= 0;
  int neg= 1;
  THD *thd= current_thd;
  Datetime::Options opt(TIME_NO_ZEROS, thd);

  null_value= 0;

  if (Datetime(thd, args[0], opt).copy_to_mysql_time(&ltime1) ||
      Datetime(thd, args[1], opt).copy_to_mysql_time(&ltime2))
    goto null_date;

  if (calc_time_diff(&ltime2,&ltime1, 1,
		     &seconds, &microseconds))
    neg= -1;

  if (int_type == INTERVAL_YEAR ||
      int_type == INTERVAL_QUARTER ||
      int_type == INTERVAL_MONTH)
  {
    uint year_beg, year_end, month_beg, month_end, day_beg, day_end;
    uint years= 0;
    uint second_beg, second_end, microsecond_beg, microsecond_end;

    if (neg == -1)
    {
      year_beg= ltime2.year;
      year_end= ltime1.year;
      month_beg= ltime2.month;
      month_end= ltime1.month;
      day_beg= ltime2.day;
      day_end= ltime1.day;
      second_beg= ltime2.hour * 3600 + ltime2.minute * 60 + ltime2.second;
      second_end= ltime1.hour * 3600 + ltime1.minute * 60 + ltime1.second;
      microsecond_beg= ltime2.second_part;
      microsecond_end= ltime1.second_part;
    }
    else
    {
      year_beg= ltime1.year;
      year_end= ltime2.year;
      month_beg= ltime1.month;
      month_end= ltime2.month;
      day_beg= ltime1.day;
      day_end= ltime2.day;
      second_beg= ltime1.hour * 3600 + ltime1.minute * 60 + ltime1.second;
      second_end= ltime2.hour * 3600 + ltime2.minute * 60 + ltime2.second;
      microsecond_beg= ltime1.second_part;
      microsecond_end= ltime2.second_part;
    }

    /* calc years */
    years= year_end - year_beg;
    if (month_end < month_beg || (month_end == month_beg && day_end < day_beg))
      years-= 1;

    /* calc months */
    months= 12*years;
    if (month_end < month_beg || (month_end == month_beg && day_end < day_beg))
      months+= 12 - (month_beg - month_end);
    else
      months+= (month_end - month_beg);

    if (day_end < day_beg)
      months-= 1;
    else if ((day_end == day_beg) &&
	     ((second_end < second_beg) ||
	      (second_end == second_beg && microsecond_end < microsecond_beg)))
      months-= 1;
  }

  switch (int_type) {
  case INTERVAL_YEAR:
    return months/12*neg;
  case INTERVAL_QUARTER:
    return months/3*neg;
  case INTERVAL_MONTH:
    return months*neg;
  case INTERVAL_WEEK:          
    return ((longlong) (seconds / SECONDS_IN_24H / 7L)) * neg;
  case INTERVAL_DAY:		
    return ((longlong) (seconds / SECONDS_IN_24H)) * neg;
  case INTERVAL_HOUR:		
    return ((longlong) (seconds / 3600L)) * neg;
  case INTERVAL_MINUTE:		
    return ((longlong) (seconds / 60L)) * neg;
  case INTERVAL_SECOND:		
    return ((longlong) seconds) * neg;
  case INTERVAL_MICROSECOND:
    /*
      In MySQL difference between any two valid datetime values
      in microseconds fits into longlong.
    */
    return ((longlong) ((ulonglong) seconds * 1000000L + microseconds)) * neg;
  default:
    break;
  }

null_date:
  null_value=1;
  return 0;
}


void Item_func_timestamp_diff::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');

  switch (int_type) {
  case INTERVAL_YEAR:
    str->append(STRING_WITH_LEN("YEAR"));
    break;
  case INTERVAL_QUARTER:
    str->append(STRING_WITH_LEN("QUARTER"));
    break;
  case INTERVAL_MONTH:
    str->append(STRING_WITH_LEN("MONTH"));
    break;
  case INTERVAL_WEEK:          
    str->append(STRING_WITH_LEN("WEEK"));
    break;
  case INTERVAL_DAY:		
    str->append(STRING_WITH_LEN("DAY"));
    break;
  case INTERVAL_HOUR:
    str->append(STRING_WITH_LEN("HOUR"));
    break;
  case INTERVAL_MINUTE:		
    str->append(STRING_WITH_LEN("MINUTE"));
    break;
  case INTERVAL_SECOND:
    str->append(STRING_WITH_LEN("SECOND"));
    break;		
  case INTERVAL_MICROSECOND:
    str->append(STRING_WITH_LEN("MICROSECOND"));
    break;
  default:
    break;
  }

  for (uint i=0 ; i < 2 ; i++)
  {
    str->append(',');
    args[i]->print(str, query_type);
  }
  str->append(')');
}


String *Item_func_get_format::val_str_ascii(String *str)
{
  DBUG_ASSERT(fixed());
  const char *format_name;
  KNOWN_DATE_TIME_FORMAT *format;
  String *val= args[0]->val_str_ascii(str);
  ulong val_len;

  if ((null_value= args[0]->null_value))
    return 0;    

  val_len= val->length();
  for (format= &known_date_time_formats[0];
       (format_name= format->format_name);
       format++)
  {
    uint format_name_len;
    format_name_len= (uint) strlen(format_name);
    if (val_len == format_name_len &&
	!my_charset_latin1.strnncoll(val->ptr(), val_len, 
		                     format_name, val_len))
    {
      const char *format_str= get_date_time_format_str(format, type);
      str->set(format_str, (uint) strlen(format_str), &my_charset_numeric);
      return str;
    }
  }

  null_value= 1;
  return 0;
}


void Item_func_get_format::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');

  switch (type) {
  case MYSQL_TIMESTAMP_DATE:
    str->append(STRING_WITH_LEN("DATE, "));
    break;
  case MYSQL_TIMESTAMP_DATETIME:
    str->append(STRING_WITH_LEN("DATETIME, "));
    break;
  case MYSQL_TIMESTAMP_TIME:
    str->append(STRING_WITH_LEN("TIME, "));
    break;
  default:
    DBUG_ASSERT(0);
  }
  args[0]->print(str, query_type);
  str->append(')');
}


/**
  Get type of datetime value (DATE/TIME/...) which will be produced
  according to format string.

  @param format   format string
  @param length   length of format string

  @note
    We don't process day format's characters('D', 'd', 'e') because day
    may be a member of all date/time types.

  @note
    Format specifiers supported by this function should be in sync with
    specifiers supported by extract_date_time() function.

  @return
    A function handler corresponding the given format
*/

static const Item_handled_func::Handler *
get_date_time_result_type(const char *format, uint length)
{
  const char *time_part_frms= "HISThiklrs";
  const char *date_part_frms= "MVUXYWabcjmvuxyw";
  bool date_part_used= 0, time_part_used= 0, frac_second_used= 0;
  
  const char *val= format;
  const char *end= format + length;

  for (; val != end; val++)
  {
    if (*val == '%' && val+1 != end)
    {
      val++;
      if (*val == 'f')
        frac_second_used= time_part_used= 1;
      else if (!time_part_used && strchr(time_part_frms, *val))
	time_part_used= 1;
      else if (!date_part_used && strchr(date_part_frms, *val))
	date_part_used= 1;
      if (date_part_used && frac_second_used)
      {
        /*
          frac_second_used implies time_part_used, and thus we already
          have all types of date-time components and can end our search.
        */
        return &func_handler_str_to_date_datetime_usec;
      }
    }
  }

  /* We don't have all three types of date-time components */
  if (frac_second_used)
    return &func_handler_str_to_date_time_usec;
  if (time_part_used)
  {
    if (date_part_used)
      return &func_handler_str_to_date_datetime_sec;
    return &func_handler_str_to_date_time_sec;
  }
  return &func_handler_str_to_date_date;
}


bool Item_func_str_to_date::fix_length_and_dec(THD *thd)
{
  if (!args[0]->type_handler()->is_traditional_scalar_type() ||
      !args[1]->type_handler()->is_traditional_scalar_type())
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
             args[0]->type_handler()->name().ptr(),
             args[1]->type_handler()->name().ptr(), func_name());
    return TRUE;
  }
  if (agg_arg_charsets(collation, args, 2, MY_COLL_ALLOW_CONV, 1))
    return TRUE;
  if (collation.collation->mbminlen > 1)
    internal_charset= &my_charset_utf8mb4_general_ci;

  set_maybe_null();
  set_func_handler(&func_handler_str_to_date_datetime_usec);

  if ((const_item= args[1]->const_item()))
  {
    StringBuffer<64> format_str;
    String *format= args[1]->val_str(&format_str, &format_converter,
                                     internal_charset);
    if (!args[1]->null_value)
      set_func_handler(get_date_time_result_type(format->ptr(), format->length()));
  }
  return m_func_handler->fix_length_and_dec(this);
}


bool Item_func_str_to_date::get_date_common(THD *thd, MYSQL_TIME *ltime,
                                            date_mode_t fuzzydate,
                                            timestamp_type tstype)
{
  DATE_TIME_FORMAT date_time_format;
  StringBuffer<64> val_string, format_str;
  String *val, *format;

  val=    args[0]->val_str(&val_string, &subject_converter, internal_charset);
  format= args[1]->val_str(&format_str, &format_converter, internal_charset);
  if (args[0]->null_value || args[1]->null_value)
    return (null_value=1);

  date_time_format.format.str=    (char*) format->ptr();
  date_time_format.format.length= format->length();
  if (extract_date_time(thd, &date_time_format, val->ptr(), val->length(),
			ltime, tstype, 0, "datetime",
                        date_conv_mode_t(fuzzydate) |
                        sql_mode_for_dates(thd)))
    return (null_value=1);
  return (null_value= 0);
}


bool Item_func_last_day::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  Datetime::Options opt(date_conv_mode_t(fuzzydate & ~TIME_TIME_ONLY),
                        time_round_mode_t(fuzzydate));
  Datetime *d= new(ltime) Datetime(thd, args[0], opt);
  if ((null_value= (!d->is_valid_datetime() || ltime->month == 0)))
    return true;
  uint month_idx= ltime->month-1;
  ltime->day= days_in_month[month_idx];
  if ( month_idx == 1 && calc_days_in_year(ltime->year) == 366)
    ltime->day= 29;
  ltime->hour= ltime->minute= ltime->second= 0;
  ltime->second_part= 0;
  ltime->time_type= MYSQL_TIMESTAMP_DATE;
  return (null_value= 0);
}
