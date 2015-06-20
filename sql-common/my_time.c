/*
   Copyright (c) 2004, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2013, Monty Program Ab.

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

#include <my_time.h>
#include <m_string.h>
#include <m_ctype.h>
/* Windows version of localtime_r() is declared in my_ptrhead.h */
#include <my_pthread.h>
#include <mysqld_error.h>

ulonglong log_10_int[20]=
{
  1, 10, 100, 1000, 10000UL, 100000UL, 1000000UL, 10000000UL,
  100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
  1000000000000ULL, 10000000000000ULL, 100000000000000ULL,
  1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL,
  1000000000000000000ULL, 10000000000000000000ULL
};


/* Position for YYYY-DD-MM HH-MM-DD.FFFFFF AM in default format */

static uchar internal_format_positions[]=
{0, 1, 2, 3, 4, 5, 6, (uchar) 255};

static char time_separator=':';

static ulong const days_at_timestart=719528;	/* daynr at 1970.01.01 */
uchar days_in_month[]= {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0};

/*
  Offset of system time zone from UTC in seconds used to speed up 
  work of my_system_gmt_sec() function.
*/
static long my_time_zone=0;


/* Calc days in one year. works with 0 <= year <= 99 */

uint calc_days_in_year(uint year)
{
  return ((year & 3) == 0 && (year%100 || (year%400 == 0 && year)) ?
          366 : 365);
}

/**
  @brief Check datetime value for validity according to flags.

  @param[in]  ltime          Date to check.
  @param[in]  not_zero_date  ltime is not the zero date
  @param[in]  flags          flags to check
                             (see str_to_datetime() flags in my_time.h)
  @param[out] was_cut        set to 2 if value was invalid according to flags.
                             (Feb 29 in non-leap etc.)  This remains unchanged
                             if value is not invalid.

  @details Here we assume that year and month is ok!
    If month is 0 we allow any date. (This only happens if we allow zero
    date parts in str_to_datetime())
    Disallow dates with zero year and non-zero month and/or day.

  @return
    0  OK
    1  error
*/

my_bool check_date(const MYSQL_TIME *ltime, my_bool not_zero_date,
                   ulonglong flags, int *was_cut)
{
  if (ltime->time_type == MYSQL_TIMESTAMP_TIME)
    return FALSE;
  if (not_zero_date)
  {
    if (((flags & TIME_NO_ZERO_IN_DATE) &&
         (ltime->month == 0 || ltime->day == 0)) || ltime->neg ||
        (!(flags & TIME_INVALID_DATES) &&
         ltime->month && ltime->day > days_in_month[ltime->month-1] &&
         (ltime->month != 2 || calc_days_in_year(ltime->year) != 366 ||
          ltime->day != 29)))
    {
      *was_cut= 2;
      return TRUE;
    }
  }
  else if (flags & TIME_NO_ZERO_DATE)
  {
    /*
      We don't set *was_cut here to signal that the problem was a zero date
      and not an invalid date
    */
    return TRUE;
  }
  return FALSE;
}

static int get_number(uint *val, uint *number_of_fields, const char **str,
                      const char *end)
{
  const char *s = *str;

  if (s >= end)
    return 0;

  if (!my_isdigit(&my_charset_latin1, *s))
    return 1;
  *val= *s++ - '0';

  for (; s < end && my_isdigit(&my_charset_latin1, *s); s++)
    *val= *val * 10 + *s - '0';
  *str = s;
  (*number_of_fields)++;
  return 0;
}

static int get_digits(uint *val, uint *number_of_fields, const char **str,
                      const char *end, uint length)
{
  return get_number(val, number_of_fields, str, MY_MIN(end, *str + length));
}

static int get_punct(const char **str, const char *end)
{
  if (*str >= end)
    return 0;
  if (my_ispunct(&my_charset_latin1, **str))
  {
    (*str)++;
    return 0;
  }
  return 1;
}

static int get_date_time_separator(uint *number_of_fields, ulonglong flags,
                                   const char **str, const char *end)
{
  const char *s= *str;
  if (s >= end)
    return 0;

  if (*s == 'T')
  {
    (*str)++;
    return 0;
  }

  /*
    now, this is tricky, for backward compatibility reasons.
    cast("11:11:11.12.12.12" as datetime) should give 2011-11-11 12:12:12
    but
    cast("11:11:11.12.12.12" as time) should give 11:11:11.12
    that is, a punctuation character can be accepted as a date/time separator
    only if TIME_DATETIME_ONLY (see str_to_time) is not set.
  */
  if (my_ispunct(&my_charset_latin1, *s))
  {
    if (flags & TIME_DATETIME_ONLY)
    {
      /* see above, returning 1 is not enough, we need hard abort here */
      *number_of_fields= 0;
      return 1;
    }

    (*str)++;
    return 0;
  }

  if (!my_isspace(&my_charset_latin1, *s))
    return 1;

  do
  {
    s++;
  } while (my_isspace(&my_charset_latin1, *s));
  *str= s;
  return 0;
}

static int get_maybe_T(const char **str, const char *end)
{
  if (*str < end && **str == 'T')
    (*str)++;
  return 0;
}

static uint skip_digits(const char **str, const char *end)
{
  const char *start= *str, *s= *str;
  while (s < end && my_isdigit(&my_charset_latin1, *s))
    s++;
  *str= s;
  return s - start;
}


/**
  Check datetime, date, or normalized time (i.e. time without days) range.
  @param ltime   Datetime value.
  @returns
  @retval   FALSE on success
  @retval   TRUE  on error
*/
my_bool check_datetime_range(const MYSQL_TIME *ltime)
{
  /*
    In case of MYSQL_TIMESTAMP_TIME hour value can be up to TIME_MAX_HOUR.
    In case of MYSQL_TIMESTAMP_DATETIME it cannot be bigger than 23.
  */
  return
    ltime->year > 9999 || ltime->month > 12  || ltime->day > 31 || 
    ltime->minute > 59 || ltime->second > 59 ||
    ltime->second_part > TIME_MAX_SECOND_PART ||
    (ltime->hour >
     (ltime->time_type == MYSQL_TIMESTAMP_TIME ? TIME_MAX_HOUR : 23));
}


static void get_microseconds(ulong *val, MYSQL_TIME_STATUS *status,
                             uint *number_of_fields,
                             const char **str, const char *end)
{
  const char *start= *str;
  uint tmp= 0; /* For the case '10:10:10.' */
  if (get_digits(&tmp, number_of_fields, str, end, 6))
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
  if ((status->precision= (*str - start)) < 6)
    *val= tmp * log_10_int[6 - (*str - start)];
  else
    *val= tmp;
  if (skip_digits(str, end))
    status->warnings|= MYSQL_TIME_NOTE_TRUNCATED;
}


/*
  Convert a timestamp string to a MYSQL_TIME value.

  SYNOPSIS
    str_to_datetime()
    str                 String to parse
    length              Length of string
    l_time              Date is stored here
    flags               Bitmap of following items
                        TIME_FUZZY_DATE
                        TIME_DATETIME_ONLY Set if we only allow full datetimes.
                        TIME_NO_ZERO_IN_DATE	Don't allow partial dates
                        TIME_NO_ZERO_DATE	Don't allow 0000-00-00 date
                        TIME_INVALID_DATES	Allow 2000-02-31
    status              Conversion status


  DESCRIPTION
    At least the following formats are recogniced (based on number of digits)
    YYMMDD, YYYYMMDD, YYMMDDHHMMSS, YYYYMMDDHHMMSS
    YY-MM-DD, YYYY-MM-DD, YY-MM-DD HH.MM.SS
    YYYYMMDDTHHMMSS  where T is a the character T (ISO8601)
    Also dates where all parts are zero are allowed

    The second part may have an optional .###### fraction part.

    status->warnings is set to:
    0                            Value OK
    MYSQL_TIME_WARN_TRUNCATED    If value was cut during conversion
    MYSQL_TIME_WARN_OUT_OF_RANGE check_date(date,flags) considers date invalid

    l_time->time_type is set as follows:
    MYSQL_TIMESTAMP_NONE        String wasn't a timestamp, like
                                [DD [HH:[MM:[SS]]]].fraction.
                                l_time is not changed.
    MYSQL_TIMESTAMP_DATE        DATE string (YY MM and DD parts ok)
    MYSQL_TIMESTAMP_DATETIME    Full timestamp
    MYSQL_TIMESTAMP_ERROR       Timestamp with wrong values.
                                All elements in l_time is set to 0
  RETURN VALUES
    0 - Ok
    1 - Error
*/

#define MAX_DATE_PARTS 8

my_bool
str_to_datetime(const char *str, uint length, MYSQL_TIME *l_time,
                ulonglong flags, MYSQL_TIME_STATUS *status)
{
  const char *end=str+length, *pos;
  uint number_of_fields= 0, digits, year_length, not_zero_date;
  DBUG_ENTER("str_to_datetime");
  bzero(l_time, sizeof(*l_time));

  if (flags & TIME_TIME_ONLY)
  {
    my_bool ret= str_to_time(str, length, l_time, flags, status);
    DBUG_RETURN(ret);
  }

  my_time_status_init(status);

  /* Skip space at start */
  for (; str != end && my_isspace(&my_charset_latin1, *str) ; str++)
    ;
  if (str == end || ! my_isdigit(&my_charset_latin1, *str))
  {
    status->warnings= MYSQL_TIME_WARN_TRUNCATED;
    l_time->time_type= MYSQL_TIMESTAMP_NONE;
    DBUG_RETURN(1);
  }

  /*
    Calculate number of digits in first part.
    If length= 8 or >= 14 then year is of format YYYY.
    (YYYY-MM-DD,  YYYYMMDD, YYYYYMMDDHHMMSS)
  */
  pos= str;
  digits= skip_digits(&pos, end);

  if (pos < end && *pos == 'T') /* YYYYYMMDDHHMMSSThhmmss is supported too */
  {
    pos++;
    digits+= skip_digits(&pos, end);
  }
  if (pos < end && *pos == '.' && digits >= 12) /* YYYYYMMDDHHMMSShhmmss.uuuuuu is supported too */
  {
    pos++;
    skip_digits(&pos, end); // ignore the return value
  }

  if (pos == end)
  {
    /*
      Found date in internal format
      (only numbers like [YY]YYMMDD[T][hhmmss[.uuuuuu]])
    */
    year_length= (digits == 4 || digits == 8 || digits >= 14) ? 4 : 2;
    if (get_digits(&l_time->year, &number_of_fields, &str, end, year_length) 
        || get_digits(&l_time->month, &number_of_fields, &str, end, 2)
        || get_digits(&l_time->day, &number_of_fields, &str, end, 2)
        || get_maybe_T(&str, end)
        || get_digits(&l_time->hour, &number_of_fields, &str, end, 2)
        || get_digits(&l_time->minute, &number_of_fields, &str, end, 2)
        || get_digits(&l_time->second, &number_of_fields, &str, end, 2))
     status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
  }
  else
  {
    const char *start= str;
    if (get_number(&l_time->year, &number_of_fields, &str, end))
      status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
    year_length= str - start;

    if (!status->warnings &&
        (get_punct(&str, end)
         || get_number(&l_time->month, &number_of_fields, &str, end)
         || get_punct(&str, end)
         || get_number(&l_time->day, &number_of_fields, &str, end)
         || get_date_time_separator(&number_of_fields, flags, &str, end)
         || get_number(&l_time->hour, &number_of_fields, &str, end)
         || get_punct(&str, end)
         || get_number(&l_time->minute, &number_of_fields, &str, end)
         || get_punct(&str, end)
         || get_number(&l_time->second, &number_of_fields, &str, end)))
      status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
  }

  /* we're ok if date part is correct. even if the rest is truncated */
  if (number_of_fields < 3)
  {
    l_time->time_type= MYSQL_TIMESTAMP_NONE;
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
    DBUG_RETURN(TRUE);
  }

  if (!status->warnings && str < end && *str == '.')
  {
    str++;
    get_microseconds(&l_time->second_part, status,
                     &number_of_fields, &str, end);
  }

  not_zero_date = l_time->year || l_time->month || l_time->day ||
                  l_time->hour || l_time->minute || l_time->second ||
                  l_time->second_part;

  if (year_length == 2 && not_zero_date)
    l_time->year+= (l_time->year < YY_PART_YEAR ? 2000 : 1900);

  if (l_time->year > 9999 || l_time->month > 12 || l_time->day > 31 ||
      l_time->hour > 23 || l_time->minute > 59 || l_time->second > 59)
  {
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
    goto err;
  }

  if (check_date(l_time, not_zero_date, flags, &status->warnings))
    goto err;

  l_time->time_type= (number_of_fields <= 3 ?
                      MYSQL_TIMESTAMP_DATE : MYSQL_TIMESTAMP_DATETIME);

  for (; str != end ; str++)
  {
    if (!my_isspace(&my_charset_latin1,*str))
    {
      status->warnings= MYSQL_TIME_WARN_TRUNCATED;
      break;
    }
  }

  DBUG_RETURN(FALSE);

err:
  bzero((char*) l_time, sizeof(*l_time));
  l_time->time_type= MYSQL_TIMESTAMP_ERROR;
  DBUG_RETURN(TRUE);
}


/*
 Convert a time string to a MYSQL_TIME struct.

  SYNOPSIS
   str_to_time()
   str                  A string in full TIMESTAMP format or
                        [-] DAYS [H]H:MM:SS, [H]H:MM:SS, [M]M:SS, [H]HMMSS,
                        [M]MSS or [S]S
                        There may be an optional [.second_part] after seconds
   length               Length of str
   l_time               Store result here
   status               Conversion status


   NOTES

     Because of the extra days argument, this function can only
     work with times where the time arguments are in the above order.

     status->warnings is set as follows:
     MYSQL_TIME_WARN_TRUNCATED if the input string was cut during conversion,
     and/or
     MYSQL_TIME_WARN_OUT_OF_RANGE flag is set if the value is out of range.

   RETURN
     FALSE on success
     TRUE  on error
*/

my_bool str_to_time(const char *str, uint length, MYSQL_TIME *l_time,
                    ulonglong fuzzydate, MYSQL_TIME_STATUS *status)
{
  ulong date[5];
  ulonglong value;
  const char *end=str+length, *end_of_days;
  my_bool found_days,found_hours, neg= 0;
  uint UNINIT_VAR(state);

  my_time_status_init(status);
  for (; str != end && my_isspace(&my_charset_latin1,*str) ; str++)
    length--;
  if (str != end && *str == '-')
  {
    neg=1;
    str++;
    length--;
  }
  if (str == end)
  {
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
    goto err;
  }

  /* Check first if this is a full TIMESTAMP */
  if (length >= 12)
  {                                             /* Probably full timestamp */
    (void) str_to_datetime(str, length, l_time,
                           (fuzzydate & ~TIME_TIME_ONLY) | TIME_DATETIME_ONLY,
                            status);
    if (l_time->time_type >= MYSQL_TIMESTAMP_ERROR)
      return l_time->time_type == MYSQL_TIMESTAMP_ERROR;
    my_time_status_init(status);
  }

  l_time->neg= neg;
  /* Not a timestamp. Try to get this as a DAYS_TO_SECOND string */
  for (value=0; str != end && my_isdigit(&my_charset_latin1,*str) ; str++)
    value=value*10L + (long) (*str - '0');

  /* Skip all space after 'days' */
  end_of_days= str;
  for (; str != end && my_isspace(&my_charset_latin1, str[0]) ; str++)
    ;

  found_days=found_hours=0;
  if ((uint) (end-str) > 1 && str != end_of_days &&
      my_isdigit(&my_charset_latin1, *str))
  {                                             /* Found days part */
    date[0]= (ulong) value;
    state= 1;                                   /* Assume next is hours */
    found_days= 1;
  }
  else if ((end-str) > 1 &&  *str == time_separator &&
           my_isdigit(&my_charset_latin1, str[1]))
  {
    date[0]= 0;                                 /* Assume we found hours */
    date[1]= (ulong) value;
    state=2;
    found_hours=1;
    str++;                                      /* skip ':' */
  }
  else
  {
    /* String given as one number; assume HHMMSS format */
    date[0]= 0;
    date[1]= (ulong) (value/10000);
    date[2]= (ulong) (value/100 % 100);
    date[3]= (ulong) (value % 100);
    state=4;
    goto fractional;
  }

  /* Read hours, minutes and seconds */
  for (;;)
  {
    for (value=0; str != end && my_isdigit(&my_charset_latin1,*str) ; str++)
      value=value*10L + (long) (*str - '0');
    date[state++]= (ulong) value;
    if (state == 4 || (end-str) < 2 || *str != time_separator ||
        !my_isdigit(&my_charset_latin1,str[1]))
      break;
    str++;                                      /* Skip time_separator (':') */
  }

  if (state != 4)
  {                                             /* Not HH:MM:SS */
    /* Fix the date to assume that seconds was given */
    if (!found_hours && !found_days)
    {
      bmove_upp((uchar*) (date+4), (uchar*) (date+state),
                sizeof(long)*(state-1));
      bzero((uchar*) date, sizeof(long)*(4-state));
    }
    else
      bzero((uchar*) (date+state), sizeof(long)*(4-state));
  }

fractional:
  /* Get fractional second part */
  if (!status->warnings && str < end && *str == '.')
  {
    uint number_of_fields= 0;
    str++;
    get_microseconds(&date[4], status, &number_of_fields, &str, end);
  }
  else
    date[4]= 0;

  /* Check for exponent part: E<gigit> | E<sign><digit> */
  /* (may occur as result of %g formatting of time value) */
  if ((end - str) > 1 &&
      (*str == 'e' || *str == 'E') &&
      (my_isdigit(&my_charset_latin1, str[1]) ||
       ((str[1] == '-' || str[1] == '+') &&
        (end - str) > 2 &&
        my_isdigit(&my_charset_latin1, str[2]))))
  {
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
    goto err;
  }

  if (internal_format_positions[7] != 255)
  {
    /* Read a possible AM/PM */
    while (str != end && my_isspace(&my_charset_latin1, *str))
      str++;
    if (str+2 <= end && (str[1] == 'M' || str[1] == 'm'))
    {
      if (str[0] == 'p' || str[0] == 'P')
      {
        str+= 2;
        date[1]= date[1]%12 + 12;
      }
      else if (str[0] == 'a' || str[0] == 'A')
        str+=2;
    }
  }

  /* Integer overflow checks */
  if (date[0] > UINT_MAX || date[1] > UINT_MAX ||
      date[2] > UINT_MAX || date[3] > UINT_MAX ||
      date[4] > UINT_MAX)
  {
    status->warnings|= MYSQL_TIME_WARN_OUT_OF_RANGE;
    goto err;
  }  

  l_time->year=         0;                      /* For protocol::store_time */
  l_time->month=        0;
  l_time->day=          0;
  l_time->hour=         date[1] + date[0] * 24; /* Mix days and hours */
  l_time->minute=       date[2];
  l_time->second=       date[3];
  l_time->second_part=  date[4];
  l_time->time_type= MYSQL_TIMESTAMP_TIME;

  /* Check if the value is valid and fits into MYSQL_TIME range */
  if (check_time_range(l_time, 6, &status->warnings))
    return TRUE;

  /* Check if there is garbage at end of the MYSQL_TIME specification */
  if (str != end)
  {
    do
    {
      if (!my_isspace(&my_charset_latin1,*str))
      {
        status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
        break;
      }
    } while (++str != end);
  }
  return FALSE;

err:
  bzero((char*) l_time, sizeof(*l_time));
  l_time->time_type= MYSQL_TIMESTAMP_ERROR;
  return TRUE;
}


/*
  Check 'time' value to lie in the MYSQL_TIME range

  SYNOPSIS:
    check_time_range()
    time     pointer to MYSQL_TIME value
    uint     dec
    warning  set MYSQL_TIME_WARN_OUT_OF_RANGE flag if the value is out of range

  DESCRIPTION
  If the time value lies outside of the range [-838:59:59, 838:59:59],
  set it to the closest endpoint of the range and set
  MYSQL_TIME_WARN_OUT_OF_RANGE flag in the 'warning' variable.

  RETURN
    0        time value is valid, but was possibly truncated
    1        time value is invalid
*/

int check_time_range(struct st_mysql_time *my_time, uint dec, int *warning) 
{
  longlong hour;
  static ulong max_sec_part[TIME_SECOND_PART_DIGITS+1]= {000000, 900000, 990000,
                                             999000, 999900, 999990, 999999};

  if (my_time->minute >= 60 || my_time->second >= 60)
  {
    *warning|= MYSQL_TIME_WARN_TRUNCATED;
    return 1;
  }

  hour= my_time->hour + (24*my_time->day);

  if (dec == AUTO_SEC_PART_DIGITS)
    dec= TIME_SECOND_PART_DIGITS;

  if (hour <= TIME_MAX_HOUR &&
      (hour != TIME_MAX_HOUR || my_time->minute != TIME_MAX_MINUTE ||
       my_time->second != TIME_MAX_SECOND ||
       my_time->second_part <= max_sec_part[dec]))
    return 0;

  my_time->day= 0;
  my_time->hour= TIME_MAX_HOUR;
  my_time->minute= TIME_MAX_MINUTE;
  my_time->second= TIME_MAX_SECOND;
  my_time->second_part= max_sec_part[dec];
  *warning|= MYSQL_TIME_WARN_OUT_OF_RANGE;
  return 0;
}


/*
  Prepare offset of system time zone from UTC for my_system_gmt_sec() func.

  SYNOPSIS
    my_init_time()
*/
void my_init_time(void)
{
  time_t seconds;
  struct tm *l_time,tm_tmp;
  MYSQL_TIME my_time;
  uint not_used;

  seconds= (time_t) time((time_t*) 0);
  localtime_r(&seconds,&tm_tmp);
  l_time= &tm_tmp;
  my_time_zone=		3600;		/* Comp. for -3600 in my_gmt_sec */
  my_time.year=		(uint) l_time->tm_year+1900;
  my_time.month=	(uint) l_time->tm_mon+1;
  my_time.day=		(uint) l_time->tm_mday;
  my_time.hour=		(uint) l_time->tm_hour;
  my_time.minute=	(uint) l_time->tm_min;
  my_time.second=	(uint) l_time->tm_sec;
  my_time.neg=          0;
  my_time.second_part=  0;
  my_time.time_type=    MYSQL_TIMESTAMP_DATETIME;

  my_system_gmt_sec(&my_time, &my_time_zone, &not_used); /* Init my_time_zone */
}


/*
  Handle 2 digit year conversions

  SYNOPSIS
  year_2000_handling()
  year     2 digit year

  RETURN
    Year between 1970-2069
*/

uint year_2000_handling(uint year)
{
  if ((year=year+1900) < 1900+YY_PART_YEAR)
    year+=100;
  return year;
}


/*
  Calculate nr of day since year 0 in new date-system (from 1615)

  SYNOPSIS
    calc_daynr()
    year		 Year (exact 4 digit year, no year conversions)
    month		 Month
    day			 Day

  NOTES: 0000-00-00 is a valid date, and will return 0

  RETURN
    Days since 0000-00-00
*/

long calc_daynr(uint year,uint month,uint day)
{
  long delsum;
  int temp;
  int y= year;                                  /* may be < 0 temporarily */
  DBUG_ENTER("calc_daynr");

  if (y == 0 && month == 0)
    DBUG_RETURN(0);				/* Skip errors */
  /* Cast to int to be able to handle month == 0 */
  delsum= (long) (365 * y + 31 *((int) month - 1) + (int) day);
  if (month <= 2)
      y--;
  else
    delsum-= (long) ((int) month * 4 + 23) / 10;
  temp=(int) ((y/100+1)*3)/4;
  DBUG_PRINT("exit",("year: %d  month: %d  day: %d -> daynr: %ld",
		     y+(month <= 2),month,day,delsum+y/4-temp));
  DBUG_ASSERT(delsum+(int) y/4-temp >= 0);
  DBUG_RETURN(delsum+(int) y/4-temp);
} /* calc_daynr */


/*
  Convert time in MYSQL_TIME representation in system time zone to its
  my_time_t form (number of seconds in UTC since begginning of Unix Epoch).

  SYNOPSIS
    my_system_gmt_sec()
      t               - time value to be converted
      my_timezone     - pointer to long where offset of system time zone
                        from UTC will be stored for caching
      error_code      - 0, if the conversion was successful;
                        ER_WARN_DATA_OUT_OF_RANGE, if t contains datetime value
                           which is out of TIMESTAMP range;
                        ER_WARN_INVALID_TIMESTAMP, if t represents value which
                           doesn't exists (falls into the spring time-gap).

  NOTES
    The idea is to cache the time zone offset from UTC (including daylight 
    saving time) for the next call to make things faster. But currently we 
    just calculate this offset during startup (by calling my_init_time() 
    function) and use it all the time.
    Time value provided should be legal time value (e.g. '2003-01-01 25:00:00'
    is not allowed).

  RETURN VALUE
    Time in UTC seconds since Unix Epoch representation.
*/
my_time_t
my_system_gmt_sec(const MYSQL_TIME *t_src, long *my_timezone, uint *error_code)
{
  uint loop;
  time_t tmp= 0;
  int shift= 0;
  MYSQL_TIME tmp_time;
  MYSQL_TIME *t= &tmp_time;
  struct tm *l_time,tm_tmp;
  long diff, current_timezone;

  /*
    Use temp variable to avoid trashing input data, which could happen in
    case of shift required for boundary dates processing.
  */
  memcpy(&tmp_time, t_src, sizeof(MYSQL_TIME));

  if (!validate_timestamp_range(t))
  {
    *error_code= ER_WARN_DATA_OUT_OF_RANGE;
    return 0;
  }
  *error_code= 0;

  /*
    Calculate the gmt time based on current time and timezone
    The -1 on the end is to ensure that if have a date that exists twice
    (like 2002-10-27 02:00:0 MET), we will find the initial date.

    By doing -3600 we will have to call localtime_r() several times, but
    I couldn't come up with a better way to get a repeatable result :(

    We can't use mktime() as it's buggy on many platforms and not thread safe.

    Note: this code assumes that our time_t estimation is not too far away
    from real value (we assume that localtime_r(tmp) will return something
    within 24 hrs from t) which is probably true for all current time zones.

    Note2: For the dates, which have time_t representation close to
    MAX_INT32 (efficient time_t limit for supported platforms), we should
    do a small trick to avoid overflow. That is, convert the date, which is
    two days earlier, and then add these days to the final value.

    The same trick is done for the values close to 0 in time_t
    representation for platfroms with unsigned time_t (QNX).

    To be more verbose, here is a sample (extracted from the code below):
    (calc_daynr(2038, 1, 19) - (long) days_at_timestart)*86400L + 4*3600L
    would return -2147480896 because of the long type overflow. In result
    we would get 1901 year in localtime_r(), which is an obvious error.

    Alike problem raises with the dates close to Epoch. E.g.
    (calc_daynr(1969, 12, 31) - (long) days_at_timestart)*86400L + 23*3600L
    will give -3600.

    On some platforms, (E.g. on QNX) time_t is unsigned and localtime(-3600)
    wil give us a date around 2106 year. Which is no good.

    Theoreticaly, there could be problems with the latter conversion:
    there are at least two timezones, which had time switches near 1 Jan
    of 1970 (because of political reasons). These are America/Hermosillo and
    America/Mazatlan time zones. They changed their offset on
    1970-01-01 08:00:00 UTC from UTC-8 to UTC-7. For these zones
    the code below will give incorrect results for dates close to
    1970-01-01, in the case OS takes into account these historical switches.
    Luckily, it seems that we support only one platform with unsigned
    time_t. It's QNX. And QNX does not support historical timezone data at all.
    E.g. there are no /usr/share/zoneinfo/ files or any other mean to supply
    historical information for localtime_r() etc. That is, the problem is not
    relevant to QNX.

    We are safe with shifts close to MAX_INT32, as there are no known
    time switches on Jan 2038 yet :)
  */
  if ((t->year == TIMESTAMP_MAX_YEAR) && (t->month == 1) && (t->day > 4))
  {
    /*
      Below we will pass (uint) (t->day - shift) to calc_daynr.
      As we don't want to get an overflow here, we will shift
      only safe dates. That's why we have (t->day > 4) above.
    */
    t->day-= 2;
    shift= 2;
  }
#ifdef TIME_T_UNSIGNED
  else
  {
    /*
      We can get 0 in time_t representaion only on 1969, 31 of Dec or on
      1970, 1 of Jan. For both dates we use shift, which is added
      to t->day in order to step out a bit from the border.
      This is required for platforms, where time_t is unsigned.
      As far as I know, among the platforms we support it's only QNX.
      Note: the order of below if-statements is significant.
    */

    if ((t->year == TIMESTAMP_MIN_YEAR + 1) && (t->month == 1)
        && (t->day <= 10))
    {
      t->day+= 2;
      shift= -2;
    }

    if ((t->year == TIMESTAMP_MIN_YEAR) && (t->month == 12)
        && (t->day == 31))
    {
      t->year++;
      t->month= 1;
      t->day= 2;
      shift= -2;
    }
  }
#endif

  tmp= (time_t) (((calc_daynr((uint) t->year, (uint) t->month, (uint) t->day) -
                   (long) days_at_timestart) * SECONDS_IN_24H +
                   (long) t->hour*3600L +
                  (long) (t->minute*60 + t->second)) + (time_t) my_time_zone -
                 3600);

  current_timezone= my_time_zone;
  localtime_r(&tmp,&tm_tmp);
  l_time=&tm_tmp;
  for (loop=0;
       loop < 2 &&
	 (t->hour != (uint) l_time->tm_hour ||
	  t->minute != (uint) l_time->tm_min ||
          t->second != (uint) l_time->tm_sec);
       loop++)
  {					/* One check should be enough ? */
    /* Get difference in days */
    int days= t->day - l_time->tm_mday;
    if (days < -1)
      days= 1;					/* Month has wrapped */
    else if (days > 1)
      days= -1;
    diff=(3600L*(long) (days*24+((int) t->hour - (int) l_time->tm_hour)) +
          (long) (60*((int) t->minute - (int) l_time->tm_min)) +
          (long) ((int) t->second - (int) l_time->tm_sec));
    current_timezone+= diff+3600;		/* Compensate for -3600 above */
    tmp+= (time_t) diff;
    localtime_r(&tmp,&tm_tmp);
    l_time=&tm_tmp;
  }
  /*
    Fix that if we are in the non existing daylight saving time hour
    we move the start of the next real hour.

    This code doesn't handle such exotical thing as time-gaps whose length
    is more than one hour or non-integer (latter can theoretically happen
    if one of seconds will be removed due leap correction, or because of
    general time correction like it happened for Africa/Monrovia time zone
    in year 1972).
  */
  if (loop == 2 && t->hour != (uint) l_time->tm_hour)
  {
    int days= t->day - l_time->tm_mday;
    if (days < -1)
      days=1;					/* Month has wrapped */
    else if (days > 1)
      days= -1;
    diff=(3600L*(long) (days*24+((int) t->hour - (int) l_time->tm_hour))+
	  (long) (60*((int) t->minute - (int) l_time->tm_min)) +
          (long) ((int) t->second - (int) l_time->tm_sec));
    if (diff == 3600)
      tmp+=3600 - t->minute*60 - t->second;	/* Move to next hour */
    else if (diff == -3600)
      tmp-=t->minute*60 + t->second;		/* Move to previous hour */

    *error_code= ER_WARN_INVALID_TIMESTAMP;
  }
  *my_timezone= current_timezone;


  /* shift back, if we were dealing with boundary dates */
  tmp+= shift * SECONDS_IN_24H;

  /*
    This is possible for dates, which slightly exceed boundaries.
    Conversion will pass ok for them, but we don't allow them.
    First check will pass for platforms with signed time_t.
    instruction above (tmp+= shift*86400L) could exceed
    MAX_INT32 (== TIMESTAMP_MAX_VALUE) and overflow will happen.
    So, tmp < TIMESTAMP_MIN_VALUE will be triggered. On platfroms
    with unsigned time_t tmp+= shift*86400L might result in a number,
    larger then TIMESTAMP_MAX_VALUE, so another check will work.
  */
  if (!IS_TIME_T_VALID_FOR_TIMESTAMP(tmp))
  {
    tmp= 0;
    *error_code= ER_WARN_DATA_OUT_OF_RANGE;
  }

  return (my_time_t) tmp;
} /* my_system_gmt_sec */


/* Set MYSQL_TIME structure to 0000-00-00 00:00:00.000000 */

void set_zero_time(MYSQL_TIME *tm, enum enum_mysql_timestamp_type time_type)
{
  bzero((void*) tm, sizeof(*tm));
  tm->time_type= time_type;
}


/*
  Helper function for datetime formatting.
  Format number as string, left-padded with 0.

  The reason to use own formatting rather than sprintf() is performance - in a
  datetime benchmark it helped to reduced the datetime formatting overhead 
  from ~30% down to ~4%.
*/

static char* fmt_number(uint val, char *out, uint digits)
{
  uint i;
  for(i= 0; i < digits; i++)
  {
    out[digits-i-1]= '0' + val%10;
    val/=10;
  }
  return out + digits;
}


/*
  Functions to convert time/date/datetime value to a string,
  using default format.
  This functions don't check that given MYSQL_TIME structure members are
  in valid range. If they are not, return value won't reflect any
  valid date either.

  RETURN
    number of characters written to 'to'
*/

int my_time_to_str(const MYSQL_TIME *l_time, char *to, uint digits)
{
  uint day= (l_time->year || l_time->month) ? 0 : l_time->day;
  uint hour=  day * 24 + l_time->hour;
  char*pos= to;

  if (digits == AUTO_SEC_PART_DIGITS)
    digits= l_time->second_part ? TIME_SECOND_PART_DIGITS : 0;

  DBUG_ASSERT(digits <= TIME_SECOND_PART_DIGITS);

  if(l_time->neg)
    *pos++= '-';

  if(hour > 99)
    /* Need more than 2 digits for hours in string representation. */
    pos= longlong10_to_str((longlong)hour, pos, 10);
  else
    pos= fmt_number(hour, pos, 2);

  *pos++= ':';
  pos= fmt_number(l_time->minute, pos, 2);
  *pos++= ':';
  pos= fmt_number(l_time->second, pos, 2);

  if (digits)
  {
    *pos++= '.';
    pos= fmt_number((uint)sec_part_shift(l_time->second_part, digits), 
       pos, digits);
  }

  *pos= 0;
  return (int) (pos-to);
}


int my_date_to_str(const MYSQL_TIME *l_time, char *to)
{
  char *pos=to;
  pos= fmt_number(l_time->year, pos, 4);
  *pos++='-';
  pos= fmt_number(l_time->month, pos, 2);
  *pos++='-';
  pos= fmt_number(l_time->day, pos, 2);
  *pos= 0;
  return (int)(pos - to);
}


int my_datetime_to_str(const MYSQL_TIME *l_time, char *to, uint digits)
{
  char *pos= to;

  if (digits == AUTO_SEC_PART_DIGITS)
    digits= l_time->second_part ? TIME_SECOND_PART_DIGITS : 0;

  DBUG_ASSERT(digits <= TIME_SECOND_PART_DIGITS);

  pos= fmt_number(l_time->year, pos, 4);
  *pos++='-';
  pos= fmt_number(l_time->month, pos, 2);
  *pos++='-';
  pos= fmt_number(l_time->day, pos, 2);
  *pos++=' ';
  pos= fmt_number(l_time->hour, pos, 2);
  *pos++= ':';
  pos= fmt_number(l_time->minute, pos, 2);
  *pos++= ':';
  pos= fmt_number(l_time->second, pos, 2);

  if (digits)
  {
    *pos++='.';
    pos= fmt_number((uint) sec_part_shift(l_time->second_part, digits), pos, 
      digits);
  }

  *pos= 0;
  return (int)(pos - to);
}


/*
  Convert struct DATE/TIME/DATETIME value to string using built-in
  MySQL time conversion formats.

  SYNOPSIS
    my_TIME_to_string()

  RETURN
    length of string

  NOTE
    The string must have at least MAX_DATE_STRING_REP_LENGTH bytes reserved.
*/

int my_TIME_to_str(const MYSQL_TIME *l_time, char *to, uint digits)
{
  switch (l_time->time_type) {
  case MYSQL_TIMESTAMP_DATETIME:
    return my_datetime_to_str(l_time, to, digits);
  case MYSQL_TIMESTAMP_DATE:
    return my_date_to_str(l_time, to);
  case MYSQL_TIMESTAMP_TIME:
    return my_time_to_str(l_time, to, digits);
  case MYSQL_TIMESTAMP_NONE:
  case MYSQL_TIMESTAMP_ERROR:
    to[0]='\0';
    return 0;
  default:
    DBUG_ASSERT(0);
    return 0;
  }
}


/**
  Print a timestamp with an optional fractional part: XXXXX[.YYYYY]

  @param      tm  The timestamp value to print.
  @param  OUT to  The string pointer to print at. 
  @param      dec Precision, in the range 0..6.
  @return         The length of the result string.
*/
int my_timeval_to_str(const struct timeval *tm, char *to, uint dec)
{
  char *pos= longlong10_to_str((longlong) tm->tv_sec, to, 10);
  if (dec)
  {
    *pos++= '.';
    pos= fmt_number((uint) sec_part_shift(tm->tv_usec, dec), pos, dec);
  }
  *pos= '\0';
  return (int) (pos - to);
}


/*
  Convert datetime value specified as number to broken-down TIME
  representation and form value of DATETIME type as side-effect.

  SYNOPSIS
    number_to_datetime()
      nr         - datetime value as number
      time_res   - pointer for structure for broken-down representation
      flags      - flags to use in validating date, as in str_to_datetime()
      was_cut    0      Value ok
                 1      If value was cut during conversion
                 2      check_date(date,flags) considers date invalid

  DESCRIPTION
    Convert a datetime value of formats YYMMDD, YYYYMMDD, YYMMDDHHMSS,
    YYYYMMDDHHMMSS to broken-down MYSQL_TIME representation. Return value in
    YYYYMMDDHHMMSS format as side-effect.

    This function also checks if datetime value fits in DATETIME range.

  RETURN VALUE
    -1              Timestamp with wrong values
    anything else   DATETIME as integer in YYYYMMDDHHMMSS format
    Datetime value in YYYYMMDDHHMMSS format.
*/

longlong number_to_datetime(longlong nr, ulong sec_part, MYSQL_TIME *time_res,
                            ulonglong flags, int *was_cut)
{
  long part1,part2;

  *was_cut= 0;
  time_res->time_type=MYSQL_TIMESTAMP_DATE;

  if (nr == 0 || nr >= 10000101000000LL)
  {
    time_res->time_type=MYSQL_TIMESTAMP_DATETIME;
    goto ok;
  }
  if (nr < 101)
    goto err;
  if (nr <= (YY_PART_YEAR-1)*10000L+1231L)
  {
    nr= (nr+20000000L)*1000000L;                 /* YYMMDD, year: 2000-2069 */
    goto ok;
  }
  if (nr < (YY_PART_YEAR)*10000L+101L)
    goto err;
  if (nr <= 991231L)
  {
    nr= (nr+19000000L)*1000000L;                 /* YYMMDD, year: 1970-1999 */
    goto ok;
  }
  if (nr < 10000101L)
    goto err;
  if (nr <= 99991231L)
  {
    nr= nr*1000000L;
    goto ok;
  }
  if (nr < 101000000L)
    goto err;

  time_res->time_type=MYSQL_TIMESTAMP_DATETIME;

  if (nr <= (YY_PART_YEAR-1)*10000000000LL+1231235959LL)
  {
    nr= nr+20000000000000LL;                   /* YYMMDDHHMMSS, 2000-2069 */
    goto ok;
  }
  if (nr <  YY_PART_YEAR*10000000000LL+ 101000000LL)
    goto err;
  if (nr <= 991231235959LL)
    nr= nr+19000000000000LL;		/* YYMMDDHHMMSS, 1970-1999 */

 ok:
  part1=(long) (nr/1000000LL);
  part2=(long) (nr - (longlong) part1*1000000LL);
  time_res->year=  (int) (part1/10000L);  part1%=10000L;
  time_res->month= (int) part1 / 100;
  time_res->day=   (int) part1 % 100;
  time_res->hour=  (int) (part2/10000L);  part2%=10000L;
  time_res->minute=(int) part2 / 100;
  time_res->second=(int) part2 % 100;
  time_res->second_part= sec_part;
  time_res->neg= 0;

  if (time_res->year <= 9999 && time_res->month <= 12 &&
      time_res->day <= 31 && time_res->hour <= 23 &&
      time_res->minute <= 59 && time_res->second <= 59 &&
      sec_part <= TIME_MAX_SECOND_PART &&
      !check_date(time_res, nr || sec_part, flags, was_cut))
  {
    if (time_res->time_type == MYSQL_TIMESTAMP_DATE && sec_part != 0)
       *was_cut= MYSQL_TIME_NOTE_TRUNCATED;
    return nr;
  }

  /* Don't want to have was_cut get set if NO_ZERO_DATE was violated. */
  if (nr || !(flags & TIME_NO_ZERO_DATE))
    *was_cut= 1;
  return -1;

 err:
  {
    /* reset everything except time_type */
    enum enum_mysql_timestamp_type save= time_res->time_type;
    bzero((char*) time_res, sizeof(*time_res));
    time_res->time_type= save;                     /* Restore range */
    *was_cut= 1;                                /* Found invalid date */
  }
  return -1;
}

/*
  Convert a pair of integers to a MYSQL_TIME struct.

  @param[in]  nr             a number to convert
  @param[out] ltime          Date to check.
  @param[out] was_cut        MYSQL_TIME_WARN_OUT_OF_RANGE if the value was
                             modified to fit in the valid range. Otherwise 0.

  @details
    Takes a number in the [-]HHHMMSS.uuuuuu,
    YYMMDDHHMMSS.uuuuuu, or in the YYYYMMDDHHMMSS.uuuuuu formats.
 
  @return
    0        time value is valid, but was possibly truncated
    -1       time value is invalid
*/
int number_to_time(my_bool neg, ulonglong nr, ulong sec_part,
                   MYSQL_TIME *ltime, int *was_cut)
{
  if (nr > 9999999 && nr < 99991231235959ULL && neg == 0)
    return number_to_datetime(nr, sec_part, ltime,
                              TIME_INVALID_DATES, was_cut) < 0 ? -1 : 0;

  *was_cut= 0;
  ltime->year= ltime->month= ltime->day= 0;
  ltime->time_type= MYSQL_TIMESTAMP_TIME;

  ltime->neg= neg;

  if (nr > TIME_MAX_VALUE)
  {
    nr= TIME_MAX_VALUE;
    sec_part= TIME_MAX_SECOND_PART;
    *was_cut= MYSQL_TIME_WARN_OUT_OF_RANGE;
  }
  ltime->hour  = (uint)(nr/100/100);
  ltime->minute= nr/100%100;
  ltime->second= nr%100;
  ltime->second_part= sec_part;

  if (ltime->minute < 60 && ltime->second < 60 && sec_part <= TIME_MAX_SECOND_PART)
    return 0;

  *was_cut= MYSQL_TIME_WARN_TRUNCATED;
  return -1;
}


/* Convert time value to integer in YYYYMMDDHHMMSS format */

ulonglong TIME_to_ulonglong_datetime(const MYSQL_TIME *my_time)
{
  return ((ulonglong) (my_time->year * 10000UL +
                       my_time->month * 100UL +
                       my_time->day) * 1000000ULL +
          (ulonglong) (my_time->hour * 10000UL +
                       my_time->minute * 100UL +
                       my_time->second));
}


/* Convert MYSQL_TIME value to integer in YYYYMMDD format */

ulonglong TIME_to_ulonglong_date(const MYSQL_TIME *my_time)
{
  return (ulonglong) (my_time->year * 10000UL + my_time->month * 100UL +
                      my_time->day);
}


/*
  Convert MYSQL_TIME value to integer in HHMMSS format.
  This function doesn't take into account time->day member:
  it's assumed that days have been converted to hours already.
*/

ulonglong TIME_to_ulonglong_time(const MYSQL_TIME *my_time)
{
  return (ulonglong) (my_time->hour * 10000UL +
                      my_time->minute * 100UL +
                      my_time->second);
}


/*
  Convert struct MYSQL_TIME (date and time split into year/month/day/hour/...
  to a number in format YYYYMMDDHHMMSS (DATETIME),
  YYYYMMDD (DATE)  or HHMMSS (TIME).

  SYNOPSIS
    TIME_to_ulonglong()

  DESCRIPTION
    The function is used when we need to convert value of time item
    to a number if it's used in numeric context, i. e.:
    SELECT NOW()+1, CURDATE()+0, CURTIME()+0;
    SELECT ?+1;

  NOTE
    This function doesn't check that given MYSQL_TIME structure members are
    in valid range. If they are not, return value won't reflect any
    valid date either.
*/

ulonglong TIME_to_ulonglong(const MYSQL_TIME *my_time)
{
  switch (my_time->time_type) {
  case MYSQL_TIMESTAMP_DATETIME:
    return TIME_to_ulonglong_datetime(my_time);
  case MYSQL_TIMESTAMP_DATE:
    return TIME_to_ulonglong_date(my_time);
  case MYSQL_TIMESTAMP_TIME:
    return TIME_to_ulonglong_time(my_time);
  case MYSQL_TIMESTAMP_NONE:
  case MYSQL_TIMESTAMP_ERROR:
    return 0;
  default:
    DBUG_ASSERT(0);
  }
  return 0;
}

double TIME_to_double(const MYSQL_TIME *my_time)
{
  double d= (double)TIME_to_ulonglong(my_time);

  if (my_time->time_type == MYSQL_TIMESTAMP_DATE)
    return d;

  d+= my_time->second_part/(double)TIME_SECOND_PART_FACTOR;
  return my_time->neg ? -d : d;
}

longlong pack_time(const MYSQL_TIME *my_time)
{
  return  ((((((my_time->year     * 13ULL +
               my_time->month)    * 32ULL +
               my_time->day)      * 24ULL +
               my_time->hour)     * 60ULL +
               my_time->minute)   * 60ULL +
               my_time->second)   * 1000000ULL +
               my_time->second_part) * (my_time->neg ? -1 : 1);
}

#define get_one(WHERE, FACTOR) WHERE= (ulong)(packed % FACTOR); packed/= FACTOR

MYSQL_TIME *unpack_time(longlong packed, MYSQL_TIME *my_time)
{
  if ((my_time->neg= packed < 0))
    packed= -packed;
  get_one(my_time->second_part, 1000000ULL);
  get_one(my_time->second,           60ULL);
  get_one(my_time->minute,           60ULL);
  get_one(my_time->hour,             24ULL);
  get_one(my_time->day,              32ULL);
  get_one(my_time->month,            13ULL);
  my_time->year= (uint)packed;
  my_time->time_type= MYSQL_TIMESTAMP_DATETIME;
  return my_time;
}
