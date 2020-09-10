/*
   Copyright (c) 2004, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2017, MariaDB Corporation.

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

#include <my_global.h>
#include <my_time.h>
#include <m_string.h>
#include <m_ctype.h>
/* Windows version of localtime_r() is declared in my_ptrhead.h */
#include <my_pthread.h>


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


#ifdef DBUG_ASSERT_EXISTS


static const ulonglong C_KNOWN_FLAGS= C_TIME_NO_ZERO_IN_DATE |
                                      C_TIME_NO_ZERO_DATE    |
                                      C_TIME_INVALID_DATES;

#define C_FLAGS_OK(flags)             (((flags) & ~C_KNOWN_FLAGS) == 0)

#endif


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
  DBUG_ASSERT(C_FLAGS_OK(flags));
  if (ltime->time_type == MYSQL_TIMESTAMP_TIME)
    return FALSE;
  if (not_zero_date)
  {
    if (((flags & C_TIME_NO_ZERO_IN_DATE) &&
         (ltime->month == 0 || ltime->day == 0)) || ltime->neg ||
        (!(flags & C_TIME_INVALID_DATES) &&
         ltime->month && ltime->day > days_in_month[ltime->month-1] &&
         (ltime->month != 2 || calc_days_in_year(ltime->year) != 366 ||
          ltime->day != 29)))
    {
      *was_cut= 2;
      return TRUE;
    }
  }
  else if (flags & C_TIME_NO_ZERO_DATE)
  {
    /*
      We don't set *was_cut here to signal that the problem was a zero date
      and not an invalid date
    */
    *was_cut|= MYSQL_TIME_WARN_ZERO_DATE;
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

static int get_date_time_separator(uint *number_of_fields,
                                   my_bool punct_is_date_time_separator,
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
    only if "punct_is_date_time_separator" is set.
  */
  if (my_ispunct(&my_charset_latin1, *s))
  {
    if (!punct_is_date_time_separator)
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
  } while (s < end && my_isspace(&my_charset_latin1, *s));
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
  return (uint)(s - start);
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
     (uint) (ltime->time_type == MYSQL_TIMESTAMP_TIME ? TIME_MAX_HOUR : 23));
}


static void get_microseconds(ulong *val, MYSQL_TIME_STATUS *status,
                             uint *number_of_fields,
                             const char **str, const char *end)
{
  const char *start= *str;
  uint tmp= 0; /* For the case '10:10:10.' */
  if (get_digits(&tmp, number_of_fields, str, end, 6))
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
  if ((status->precision= (uint)(*str - start)) < 6)
    *val= (ulong) (tmp * log_10_int[6 - (*str - start)]);
  else
    *val= tmp;
  if (str[0] < end && my_isdigit(&my_charset_latin1, str[0][0]))
  {
    /*
      We don't need the exact nanoseconds value.
      Knowing the first digit is enough for rounding.
    */
    status->nanoseconds= 100 * (uint)(str[0][0] - '0');
  }
  if (skip_digits(str, end))
    status->warnings|= MYSQL_TIME_NOTE_TRUNCATED;
}


static int check_time_range_internal(MYSQL_TIME *ltime,
                                     ulong max_hour, ulong err_hour,
                                     uint dec, int *warning);

int check_time_range(MYSQL_TIME *ltime, uint dec, int *warning)
{
  return check_time_range_internal(ltime, TIME_MAX_HOUR, UINT_MAX32,
                                   dec, warning);
}


static my_bool
set_neg(my_bool neg, MYSQL_TIME_STATUS *st, MYSQL_TIME *ltime)
{
  if ((ltime->neg= neg) && ltime->time_type != MYSQL_TIMESTAMP_TIME)
  {
    st->warnings|= MYSQL_TIME_WARN_OUT_OF_RANGE;
    return TRUE;
  }
  return FALSE;
}


 /* Remove trailing spaces and garbage */
static my_bool get_suffix(const char *str, size_t length, size_t *new_length)
{
  /*
    QQ: perhaps 'T' should be considered as a date/time delimiter only
    if it's followed by a digit. Learn ISO 8601 details.
  */
  my_bool garbage= FALSE;
  for ( ; length > 0 ; length--)
  {
    char ch= str[length - 1];
    if (my_isdigit(&my_charset_latin1, ch) ||
        my_ispunct(&my_charset_latin1, ch))
      break;
    if (my_isspace(&my_charset_latin1, ch))
      continue;
    if (ch == 'T')
    {
      /* 'T' has a meaning only after a digit. Otherwise it's a garbage */
      if (length >= 2 && my_isdigit(&my_charset_latin1, str[length - 2]))
        break;
    }
    garbage= TRUE;
  }
  *new_length= length;
  return garbage;
}


static size_t get_prefix(const char *str, size_t length, const char **endptr)
{
  const char *str0= str, *end= str + length;
  for (; str < end && my_isspace(&my_charset_latin1, *str) ; str++)
  { }
  *endptr= str;
  return str - str0;
}


static size_t get_sign(my_bool *neg, const char *str, size_t length,
                       const char **endptr)
{
  const char *str0= str;
  if (length)
  {
    if ((*neg= (*str == '-')) || (*str == '+'))
      str++;
  }
  else
    *neg= FALSE;
  *endptr= str;
  return str - str0;
}


static my_bool find_body(my_bool *neg, const char *str, size_t length,
                         MYSQL_TIME *to, int *warn,
                         const char **new_str, size_t *new_length)
{
  size_t sign_length;
  *warn= 0;
  length-= get_prefix(str, length, &str);
  sign_length= get_sign(neg, str, length, &str);
  length-= sign_length;
  /* There can be a space after a sign again: '- 10:20:30' or '- 1 10:20:30' */
  length-= get_prefix(str, length, &str);
  if (get_suffix(str, length, &length))
    *warn|= MYSQL_TIME_WARN_TRUNCATED;
  *new_str= str;
  *new_length= length;
  if (!length || !my_isdigit(&my_charset_latin1, *str))
  {
    *warn|= MYSQL_TIME_WARN_EDOM;
    set_zero_time(to, MYSQL_TIMESTAMP_ERROR);
    return TRUE;
  }
  return FALSE;
}


typedef struct
{
  uint count_punct;
  uint count_colon;
  uint count_iso_date_time_separator;
} MYSQL_TIME_USED_CHAR_STATISTICS;


static void
mysql_time_used_char_statistics_init(MYSQL_TIME_USED_CHAR_STATISTICS *to,
                                     const char *str, const char *end)
{
  const char *s;
  bzero((void *) to, sizeof(MYSQL_TIME_USED_CHAR_STATISTICS));
  for (s= str; s < end; s++)
  {
    if (my_ispunct(&my_charset_latin1, *s))
      to->count_punct++;
    if (*s == ':')
      to->count_colon++;
    if (*s == 'T')
      to->count_iso_date_time_separator++;
  }
}


static my_bool
is_datetime_body_candidate(const char *str, size_t length,
                           my_bool allow_dates_delimited,
                           my_bool allow_dates_numeric)
{
  static uint min_date_length= 5; /* '1-1-1' -> '0001-01-01' */
  uint pos, count_punct= 0;
  uint date_time_separator_length= MY_TEST(!allow_dates_delimited);
  if (length >= 12)
    return TRUE;
  /*
    The shortest possible DATE is '1-1-1', which is 5 characters.
    To make a full datetime it should be at least followed by a space or a 'T'.
    To make a date it should be just not less that 5 characters.
  */
  if (length < min_date_length + date_time_separator_length &&
      !allow_dates_numeric)
    return FALSE;
  for (pos= 0; pos < length; pos++)
  {
    if (str[pos] == 'T') /* Date/time separator */
      return TRUE;
    if (str[pos] == ' ')
    {
      /*
        We found a space. If can be a DATE/TIME separator:
          TIME('1-1-1 1:1:1.0) -> '0001-01-01 01:01:01.0'

        But it can be also a DAY/TIME separator:
          TIME('1 11')      -> 35:00:00   = 1 day 11 hours
          TIME('1 111')     -> 135:00:00  = 1 day 111 hours
          TIME('11 11')     -> 275:00:00  = 11 days 11 hours
          TIME('111 11')    -> 838:59:59  = 111 days 11 hours with overflow
          TIME('1111 11')   -> 838:59:59  = 1111 days 11 hours with overflow
      */
      return count_punct > 0; /* Can be a DATE if already had separators*/
    }
    if (my_ispunct(&my_charset_latin1, str[pos]))
    {
      if (allow_dates_delimited && str[pos] != ':')
        return TRUE;
      count_punct++;
    }
  }
  return allow_dates_numeric && count_punct == 0;
}


static my_bool
str_to_DDhhmmssff_internal(my_bool neg, const char *str, size_t length,
                           MYSQL_TIME *l_time,
                           ulong max_hour, ulong err_hour,
                           MYSQL_TIME_STATUS *status,
                           const char **endptr);


/*
  Convert a timestamp string to a MYSQL_TIME value.

  SYNOPSIS
    str_to_datetime_or_date_body()
    str                 String to parse
    length              Length of string
    l_time              Date is stored here
    flags               Bitmap of following items
                        TIME_DATETIME_ONLY Set if we only allow full datetimes.
                        TIME_NO_ZERO_IN_DATE	Don't allow partial dates
                        TIME_NO_ZERO_DATE	Don't allow 0000-00-00 date
                        TIME_INVALID_DATES	Allow 2000-02-31
    punct_is_date_time_separator
                        Allow punctuation as a date/time separator,
                        or return a hard error.
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

static my_bool
str_to_datetime_or_date_body(const char *str, size_t length, MYSQL_TIME *l_time,
                             ulonglong flags,
                             my_bool punct_is_date_time_separator,
                             MYSQL_TIME_STATUS *status,
                             uint *number_of_fields,
                             const char **endptr)
{
  const char *end=str+length, *pos;
  uint digits, year_length, not_zero_date;
  int warn= 0;
  DBUG_ENTER("str_to_datetime_or_date_body");
  DBUG_ASSERT(C_FLAGS_OK(flags));
  bzero(l_time, sizeof(*l_time));
  *number_of_fields= 0;
  *endptr= str;

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
    if (get_digits(&l_time->year, number_of_fields, &str, end, year_length) 
        || get_digits(&l_time->month, number_of_fields, &str, end, 2)
        || get_digits(&l_time->day, number_of_fields, &str, end, 2)
        || get_maybe_T(&str, end)
        || get_digits(&l_time->hour, number_of_fields, &str, end, 2)
        || get_digits(&l_time->minute, number_of_fields, &str, end, 2)
        || get_digits(&l_time->second, number_of_fields, &str, end, 2))
     warn|= MYSQL_TIME_WARN_TRUNCATED;
  }
  else
  {
    const char *start= str;
    if (get_number(&l_time->year, number_of_fields, &str, end))
      warn|= MYSQL_TIME_WARN_TRUNCATED;
    year_length= (uint)(str - start);

    if (!warn &&
        (get_punct(&str, end)
         || get_number(&l_time->month, number_of_fields, &str, end)
         || get_punct(&str, end)
         || get_number(&l_time->day, number_of_fields, &str, end)
         || get_date_time_separator(number_of_fields,
                                    punct_is_date_time_separator, &str, end)
         || get_number(&l_time->hour, number_of_fields, &str, end)
         || get_punct(&str, end)
         || get_number(&l_time->minute, number_of_fields, &str, end)
         || get_punct(&str, end)
         || get_number(&l_time->second, number_of_fields, &str, end)))
      warn|= MYSQL_TIME_WARN_TRUNCATED;
  }
  status->warnings|= warn;

  *endptr= str;
  /* we're ok if date part is correct. even if the rest is truncated */
  if (*number_of_fields < 3)
  {
    l_time->time_type= MYSQL_TIMESTAMP_NONE;
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
    DBUG_RETURN(TRUE);
  }

  if (!warn && str < end && *str == '.')
  {
    str++;
    get_microseconds(&l_time->second_part, status,
                     number_of_fields, &str, end);
    *endptr= str;
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

  l_time->time_type= (*number_of_fields <= 3 ?
                      MYSQL_TIMESTAMP_DATE : MYSQL_TIMESTAMP_DATETIME);

  if (str != end)
    status->warnings= MYSQL_TIME_WARN_TRUNCATED;

  DBUG_RETURN(FALSE);

err:
  set_zero_time(l_time, MYSQL_TIMESTAMP_ERROR);
  DBUG_RETURN(TRUE);
}


/*
 Convert a time string to a MYSQL_TIME struct.

  SYNOPSIS
   str_to_datetime_or_date_or_time_body()
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

static my_bool
str_to_datetime_or_date_or_time_body(const char *str, size_t length,
                                     MYSQL_TIME *l_time,
                                     ulonglong fuzzydate,
                                     MYSQL_TIME_STATUS *status,
                                     ulong time_max_hour,
                                     ulong time_err_hour,
                                     my_bool allow_dates_delimited,
                                     my_bool allow_dates_numeric)
{
  const char *endptr;
  DBUG_ASSERT(C_FLAGS_OK(fuzzydate));

  /* Check first if this is a full TIMESTAMP */
  if (is_datetime_body_candidate(str, length,
                                 allow_dates_delimited,
                                 allow_dates_numeric))
  {                                             /* Probably full timestamp */
    int warn_copy= status->warnings; /* could already be set by find_body() */
    uint number_of_fields;
    (void) str_to_datetime_or_date_body(str, length, l_time, fuzzydate,
                                        FALSE, status,
                                        &number_of_fields, &endptr);
    DBUG_ASSERT(endptr >= str);
    DBUG_ASSERT(endptr <= str + length);
    switch (l_time->time_type) {
    case MYSQL_TIMESTAMP_DATETIME:
      return FALSE;
    case MYSQL_TIMESTAMP_DATE:
      {
       /*
         Successfully parsed as DATE, but it can also be a TIME:
           '24:02:03'               - continue and parse as TIME
           '24:02:03 garbage /////' - continue and parse as TIME
           '24:02:03T'              - return DATE
           '24-02-03'               - return DATE
           '24/02/03'               - return DATE
           '11111'                  - return DATE
       */
        MYSQL_TIME_USED_CHAR_STATISTICS used_chars;
        mysql_time_used_char_statistics_init(&used_chars, str, endptr);
        if (used_chars.count_iso_date_time_separator || !used_chars.count_colon)
          return FALSE;
      }
      break;
    case MYSQL_TIMESTAMP_ERROR:
      {
        MYSQL_TIME_USED_CHAR_STATISTICS used_chars;
        /*
          Check if it parsed as DATETIME but then failed as out of range:
            '2011-02-32 8:46:06.23434' - return error
        */
        if (number_of_fields > 3)
          return TRUE;
        /*
          Check if it parsed as DATE but then failed as out of range:
            '100000:02:03'  - continue and parse as TIME
            '100000:02:03T' - return error
            '100000/02/03'  - return error
            '100000-02-03'  - return error
        */
        mysql_time_used_char_statistics_init(&used_chars, str, endptr);
        if (used_chars.count_iso_date_time_separator || !used_chars.count_colon)
          return TRUE;
      }
      break;
    case MYSQL_TIMESTAMP_NONE:
      {
        if (allow_dates_numeric && endptr >= str + length)
        {
          /*
            For backward compatibility this parses as DATE and fails:
              EXTRACT(DAY FROM '1111') -- return error
              EXTRACT(DAY FROM '1')    -- return error
          */
          MYSQL_TIME_USED_CHAR_STATISTICS used_chars;
          mysql_time_used_char_statistics_init(&used_chars, str, endptr);
          if (!used_chars.count_iso_date_time_separator &&
              !used_chars.count_colon &&
              !used_chars.count_punct)
            return TRUE;
        }
        /*
          - '256 10:30:30'               - continue and parse as TIME
          - '4294967296:59:59.123456456' - continue and parse as TIME
        */
      }
      break;
    case MYSQL_TIMESTAMP_TIME:
      DBUG_ASSERT(0);
      break;
    }
    my_time_status_init(status);
    status->warnings= warn_copy;
  }

  if (!str_to_DDhhmmssff_internal(FALSE, str, length, l_time,
                                  time_max_hour, time_err_hour,
                                  status, &endptr))
    return FALSE;

  set_zero_time(l_time, MYSQL_TIMESTAMP_ERROR);
  return TRUE;
}


/*
  Convert a string with INTERVAL DAY TO SECOND to MYSQL_TIME.
  Input format: [-][DD ]hh:mm:ss.ffffff

  If the input string appears to be a DATETIME, error is returned.
*/
my_bool str_to_DDhhmmssff(const char *str, size_t length, MYSQL_TIME *ltime,
                          ulong max_hour, MYSQL_TIME_STATUS *status)
{
  my_bool neg;
  const char *endptr;

  my_time_status_init(status);
  if (find_body(&neg, str, length, ltime, &status->warnings, &str, &length))
    return TRUE;

  /* Reject anything that might be parsed as a full TIMESTAMP */
  if (is_datetime_body_candidate(str, length, FALSE, FALSE))
  {
    uint number_of_fields;
    (void) str_to_datetime_or_date_body(str, length, ltime, 0, FALSE,
                                        status, &number_of_fields, &endptr);
    if (ltime->time_type > MYSQL_TIMESTAMP_ERROR)
    {
      status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
      ltime->time_type= MYSQL_TIMESTAMP_NONE;
      return TRUE;
    }
    my_time_status_init(status);
  }

  /*
    Scan DDhhmmssff then reject anything that can remind date/datetime.
    For example, in case of '2001-01-01', str_to_DDhhmmssff_internal()
    will scan only '2001'.
  */
  if (str_to_DDhhmmssff_internal(neg, str, length, ltime, max_hour,
                                 UINT_MAX32, status, &endptr) ||
      (endptr < str + length && endptr[0] == '-'))
    return TRUE;
  return FALSE;
}


my_bool
str_to_datetime_or_date_or_time(const char *str, size_t length,
                                MYSQL_TIME *to, ulonglong mode,
                                MYSQL_TIME_STATUS *status,
                                ulong time_max_hour,
                                ulong time_err_hour)
{
  my_bool neg;
  DBUG_ASSERT(C_FLAGS_OK(mode));
  my_time_status_init(status);
  return
    find_body(&neg, str, length, to, &status->warnings, &str, &length) ||
    str_to_datetime_or_date_or_time_body(str, length, to, mode, status,
                                         time_max_hour, time_err_hour,
                                         FALSE, FALSE) ||
    set_neg(neg, status, to);
}


my_bool
str_to_datetime_or_date_or_interval_hhmmssff(const char *str, size_t length,
                                             MYSQL_TIME *to, ulonglong mode,
                                             MYSQL_TIME_STATUS *status,
                                             ulong time_max_hour,
                                             ulong time_err_hour)
{
  my_bool neg;
  DBUG_ASSERT(C_FLAGS_OK(mode));
  my_time_status_init(status);
  return
    find_body(&neg, str, length, to, &status->warnings, &str, &length) ||
    str_to_datetime_or_date_or_time_body(str, length, to, mode, status,
                                         time_max_hour, time_err_hour,
                                         TRUE, FALSE) ||
    set_neg(neg, status, to);
}


my_bool
str_to_datetime_or_date_or_interval_day(const char *str, size_t length,
                                        MYSQL_TIME *to, ulonglong mode,
                                        MYSQL_TIME_STATUS *status,
                                        ulong time_max_hour,
                                        ulong time_err_hour)
{
  my_bool neg;
  DBUG_ASSERT(C_FLAGS_OK(mode));
  my_time_status_init(status);
  /*
    For backward compatibility we allow to parse non-delimited
    values as DATE rather than as TIME:
      EXTRACT(DAY FROM '11111') 
  */
  return
    find_body(&neg, str, length, to, &status->warnings, &str, &length) ||
    str_to_datetime_or_date_or_time_body(str, length, to, mode, status,
                                         time_max_hour, time_err_hour,
                                         TRUE, TRUE) ||
    set_neg(neg, status, to);
}


my_bool
str_to_datetime_or_date(const char *str, size_t length, MYSQL_TIME *l_time,
                        ulonglong flags, MYSQL_TIME_STATUS *status)
{
  my_bool neg;
  uint number_of_fields;
  const char *endptr;
  DBUG_ASSERT(C_FLAGS_OK(flags));
  my_time_status_init(status);
  return
    find_body(&neg, str, length, l_time, &status->warnings, &str, &length) ||
    str_to_datetime_or_date_body(str, length, l_time, flags, TRUE,
                                 status, &number_of_fields, &endptr) ||
    set_neg(neg, status, l_time);
}



/**
  Convert a string to INTERVAL DAY TO SECOND.
  Input format:  [DD ]hh:mm:ss.ffffff

  Datetime or date formats are not understood.

  Optional leading spaces and signs must be scanned by the caller.
  "str" should point to the first digit.

  @param      neg      - set the value to be negative
  @param      str      - the input string
  @param      length   - length of "str"
  @param[OUT] l_time   - write the result here
  @param      max_hour - if the result hour value appears to be greater than
                         max_hour, then cut to result to 'max_hour:59:59.999999'
  @param      err_hour - if the hour appears to be greater than err_hour,
                         return an error (without cut)
  @param      status
  @param      endptr
*/
static my_bool
str_to_DDhhmmssff_internal(my_bool neg, const char *str, size_t length,
                           MYSQL_TIME *l_time,
                           ulong max_hour, ulong err_hour,
                           MYSQL_TIME_STATUS *status, const char **endptr)
{
  ulong date[5];
  ulonglong value;
  const char *end=str + length, *end_of_days;
  my_bool found_days, found_hours;
  uint UNINIT_VAR(state);

  *endptr= str;
  l_time->neg= neg;
  /* Not a timestamp. Try to get this as a DAYS TO SECOND string */
  for (value=0; str != end && my_isdigit(&my_charset_latin1,*str) ; str++)
  {
    value=value*10L + (long) (*str - '0');
    if (value >= 42949672955959ULL) /* i.e. UINT_MAX32 : 59 : 59 */
    {
      status->warnings|= MYSQL_TIME_WARN_OUT_OF_RANGE;
      goto err;
    }
  }

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
    if (value >= UINT_MAX32)
    {
      status->warnings|= MYSQL_TIME_WARN_OUT_OF_RANGE;
      goto err;
    }
    date[1]= (ulong) value;
    state=2;
    found_hours=1;
    str++;                                      /* skip ':' */
  }
  else
  {
    /* String given as one number; assume HHMMSS format */
    date[0]= 0;
    DBUG_ASSERT(value <= ((ulonglong) UINT_MAX32) * 10000ULL);
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
  if (str < end && *str == '.')
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

  if ((ulonglong) date[0] * 24 + date[1] > (ulonglong) UINT_MAX32)
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

  *endptr= str;

  /* Check if the value is valid and fits into MYSQL_TIME range */
  if (check_time_range_internal(l_time, max_hour, err_hour,
                                6, &status->warnings))
    return TRUE;

  /* Check if there is garbage at end of the MYSQL_TIME specification */
  if (str != end)
    status->warnings|= MYSQL_TIME_WARN_TRUNCATED;
  return FALSE;

err:
  *endptr= str;
  return TRUE;
}


/*
  Check 'time' value to lie in the MYSQL_TIME range

  SYNOPSIS:
    check_time_range_internal()
    time     pointer to MYSQL_TIME value
    ulong    max_hour - maximum allowed hour value. if the hour is greater,
                        cut the time value to 'max_hour:59:59.999999'
    ulong    err_hour - if hour is greater than this value, return an error
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

int check_time_range_internal(struct st_mysql_time *my_time,
                              ulong max_hour, ulong err_hour,
                              uint dec, int *warning)
{
  ulonglong hour;
  static ulong max_sec_part[TIME_SECOND_PART_DIGITS+1]= {000000, 900000, 990000,
                                             999000, 999900, 999990, 999999};

  if (my_time->minute >= 60 || my_time->second >= 60 ||
      my_time->hour > err_hour)
  {
    *warning|= MYSQL_TIME_WARN_TRUNCATED;
    return 1;
  }

  hour= my_time->hour + (24*my_time->day);

  if (dec == AUTO_SEC_PART_DIGITS)
    dec= TIME_SECOND_PART_DIGITS;

  if (hour <= max_hour &&
      (hour != max_hour || my_time->minute != TIME_MAX_MINUTE ||
       my_time->second != TIME_MAX_SECOND ||
       my_time->second_part <= max_sec_part[dec]))
    return 0;

  my_time->day= 0;
  my_time->hour= max_hour;
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
  A formatting routine to print a 2 digit zero padded number.
  It prints 2 digits at a time, which gives a performance improvement.
  The idea is taken from "class TwoDigitWriter" in MySQL.

  The old implementation printed one digit at a time, using the division
  and the remainder operators, which appeared to be slow.
  It's cheaper to have a cached array of 2-digit numbers
  in their string representation.

  Benchmark results showed a 10% to 23% time reduce for these queries:
    SELECT BENCHMARK(10*1000*1000,CONCAT(TIME'10:20:30'));
    SELECT BENCHMARK(10*1000*1000,CONCAT(DATE'2001-01-01'));
    SELECT BENCHMARK(10*1000*1000,CONCAT(TIMESTAMP'2001-01-01 10:20:30'));
    SELECT BENCHMARK(10*1000*1000,CONCAT(TIME'10:20:30.123456'));
    SELECT BENCHMARK(10*1000*1000,CONCAT(TIMESTAMP'2001-01-01 10:20:30.123456'));
  (depending on the exact data type and fractional precision).

  The array has extra elements for values 100..255.
  This is done for safety. If the caller passes a value
  outside of the expected range 0..99, the value will be printed as "XX".

  Part2:

  As an additional improvement over "class TwoDigitWriter", we store
  the string representations of the numbers in an array uint16[256]
  instead of char[512]. This allows to copy data using int2store(),
  which copies two bytes at a time on x86 and gives an additional
  7% to 26% time reduce over copying the two bytes separately.

  The total time reduce is 15% to 38% on the above queries.

  The bytes in the following array are swapped:
  e.g.  0x3130 in two_digit_numbers[1] means the following:
  - 0x31 is '1' (the left byte, the right digit)
  - 0x30 is '0' (the right byte, the left digit)
  int2store() puts the lower byte first, so the output string becomes '01'.
*/
static const uint16 two_digit_numbers[256]=
{
  /* 0..99 */
  0x3030,0x3130,0x3230,0x3330,0x3430,0x3530,0x3630,0x3730,0x3830,0x3930,
  0x3031,0x3131,0x3231,0x3331,0x3431,0x3531,0x3631,0x3731,0x3831,0x3931,
  0x3032,0x3132,0x3232,0x3332,0x3432,0x3532,0x3632,0x3732,0x3832,0x3932,
  0x3033,0x3133,0x3233,0x3333,0x3433,0x3533,0x3633,0x3733,0x3833,0x3933,
  0x3034,0x3134,0x3234,0x3334,0x3434,0x3534,0x3634,0x3734,0x3834,0x3934,
  0x3035,0x3135,0x3235,0x3335,0x3435,0x3535,0x3635,0x3735,0x3835,0x3935,
  0x3036,0x3136,0x3236,0x3336,0x3436,0x3536,0x3636,0x3736,0x3836,0x3936,
  0x3037,0x3137,0x3237,0x3337,0x3437,0x3537,0x3637,0x3737,0x3837,0x3937,
  0x3038,0x3138,0x3238,0x3338,0x3438,0x3538,0x3638,0x3738,0x3838,0x3938,
  0x3039,0x3139,0x3239,0x3339,0x3439,0x3539,0x3639,0x3739,0x3839,0x3939,
  /* 100..199 - safety */
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  /* 200..255 - safety */
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
  0x5858,0x5858,0x5858,0x5858,0x5858,0x5858,
};

static inline char* fmt_number2(uint8 val, char *out)
{
  int2store(out, two_digit_numbers[val]);
  return out + 2;
}


/*
  We tried the same trick with a char array of 16384 zerofill 4-digit numbers,
  with 10000 elements with numbers 0000..9999, and a tail filled with "XXXX".

  Benchmark results for a RelWithDebInfo build:

  SELECT BENCHMARK(10*1000*1000,CONCAT(TIMESTAMP'2001-01-01 10:20:30.123456'));
  - 0.379 sec (current)
  - 0.369 sec (array)

  SELECT BENCHMARK(10*1000*1000,CONCAT(DATE'2001-01-01'));
  - 0.225 sec (current)
  - 0.219 sec (array)

  It demonstrated an additional 3% performance imrovement one these queries.
  However, as the array size is too huge, we afraid that it will flush data
  from the CPU memory cache, which under real load may affect negatively.

  Let's keep using the fmt_number4() version with division and remainder
  for now. This can be revised later. We could try some smaller array,
  e.g. for YEARs in the range 1970..2098 (fitting into a 256 element array).
*/
/*
static inline char* fmt_number4(uint16 val, char *out)
{
  const char *src= four_digit_numbers + (val & 0x3FFF) * 4;
  memcpy(out, src, 4);
  return out + 4;
}
*/


/*
  A formatting routine to print a 4 digit zero padded number.
*/
static inline char* fmt_number4(uint16 val, char *out)
{
  out= fmt_number2((uint8) (val / 100), out);
  out= fmt_number2((uint8) (val % 100), out);
  return out;
}


/*
  A formatting routine to print a 6 digit zero padded number.
*/
static inline char* fmt_number6(uint val, char *out)
{
  out= fmt_number2((uint8) (val / 10000), out);
  val%= 10000;
  out= fmt_number2((uint8) (val / 100),   out);
  out= fmt_number2((uint8) (val % 100),   out);
  return out;
}


static char* fmt_usec(uint val, char *out, uint digits)
{
  switch (digits)
  {
  case 1:
    *out++= '0' + (val % 10);
    return out;
  case 2:
    return fmt_number2((uint8) val, out);
  case 3:
    *out++= '0' + (val / 100) % 10;
    return fmt_number2((uint8) (val % 100), out);
  case 4:
    return fmt_number4((uint16) val, out);
  case 5:
    *out++= '0' + (val / 10000) % 10;
    return fmt_number4((uint16) (val % 10000), out);
  case 6:
    return fmt_number6(val, out);
  }
  DBUG_ASSERT(0);
  return out;
}


static int my_mmssff_to_str(const MYSQL_TIME *ltime, char *to, uint fsp)
{
  char *pos= to;
  if (fsp == AUTO_SEC_PART_DIGITS)
    fsp= ltime->second_part ? TIME_SECOND_PART_DIGITS : 0;
  DBUG_ASSERT(fsp <= TIME_SECOND_PART_DIGITS);
  pos= fmt_number2((uint8) ltime->minute, pos);
  *pos++= ':';
  pos= fmt_number2((uint8) ltime->second, pos);
  if (fsp)
  {
    *pos++= '.';
    pos= fmt_usec((uint)sec_part_shift(ltime->second_part, fsp), pos, fsp);
  }
  return (int) (pos - to);
}


int my_interval_DDhhmmssff_to_str(const MYSQL_TIME *ltime, char *to, uint fsp)
{
  uint hour= ltime->day * 24 + ltime->hour;
  char *pos= to;
  DBUG_ASSERT(!ltime->year);
  DBUG_ASSERT(!ltime->month);

  if(ltime->neg)
    *pos++= '-';
  if (hour >= 24)
  {
    pos= longlong10_to_str((longlong) hour / 24, pos, 10);
    *pos++= ' ';
  }
  pos= fmt_number2((uint8) (hour % 24), pos);
  *pos++= ':';
  pos+= my_mmssff_to_str(ltime, pos, fsp);
  *pos= 0;
  return (int) (pos-to);
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

  if(l_time->neg)
    *pos++= '-';

  if(hour > 99)
    /* Need more than 2 digits for hours in string representation. */
    pos= longlong10_to_str((longlong)hour, pos, 10);
  else
    pos= fmt_number2((uint8) hour, pos);

  *pos++= ':';
  pos+= my_mmssff_to_str(l_time, pos, digits);
  *pos= 0;
  return (int) (pos-to);
}


int my_date_to_str(const MYSQL_TIME *l_time, char *to)
{
  char *pos=to;
  pos= fmt_number4((uint16) l_time->year, pos);
  *pos++='-';
  pos= fmt_number2((uint8) l_time->month, pos);
  *pos++='-';
  pos= fmt_number2((uint8) l_time->day, pos);
  *pos= 0;
  return (int)(pos - to);
}


int my_datetime_to_str(const MYSQL_TIME *l_time, char *to, uint digits)
{
  char *pos= to;
  pos= fmt_number4((uint16) l_time->year, pos);
  *pos++='-';
  pos= fmt_number2((uint8) l_time->month, pos);
  *pos++='-';
  pos= fmt_number2((uint8) l_time->day, pos);
  *pos++=' ';
  pos= fmt_number2((uint8) l_time->hour, pos);
  *pos++= ':';
  pos+= my_mmssff_to_str(l_time, pos, digits);
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
    pos= fmt_usec((uint) sec_part_shift(tm->tv_usec, dec), pos, dec);
  }
  *pos= '\0';
  return (int) (pos - to);
}


/*
  Convert datetime value specified as number to broken-down TIME
  representation and form value of DATETIME type as side-effect.

  SYNOPSIS
    number_to_datetime_or_date()
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

longlong number_to_datetime_or_date(longlong nr, ulong sec_part,
                                    MYSQL_TIME *time_res,
                                    ulonglong flags, int *was_cut)
{
  long part1,part2;
  DBUG_ASSERT(C_FLAGS_OK(flags));

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
    {
      /* Date format, but with fractional digits, e.g. 20010203.5 */
      *was_cut= MYSQL_TIME_NOTE_TRUNCATED;
      time_res->second_part= 0;
    }
    return nr;
  }

  /* Don't want to have was_cut get set if NO_ZERO_DATE was violated. */
  if (nr || !(flags & C_TIME_NO_ZERO_DATE))
    *was_cut= MYSQL_TIME_WARN_TRUNCATED;
  return -1;

 err:
  {
    /* reset everything except time_type */
    enum enum_mysql_timestamp_type save= time_res->time_type;
    bzero((char*) time_res, sizeof(*time_res));
    time_res->time_type= save;                     /* Restore range */
    *was_cut= MYSQL_TIME_WARN_TRUNCATED;           /* Found invalid date */
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
int number_to_time_only(my_bool neg, ulonglong nr, ulong sec_part,
                        ulong max_hour, MYSQL_TIME *ltime, int *was_cut)
{
  static const ulonglong TIME_MAX_mmss= TIME_MAX_MINUTE*100 + TIME_MAX_SECOND;
  ulonglong time_max_value= max_hour * 10000ULL + TIME_MAX_mmss;
  *was_cut= 0;
  ltime->year= ltime->month= ltime->day= 0;
  ltime->time_type= MYSQL_TIMESTAMP_TIME;

  ltime->neg= neg;

  if (nr > time_max_value)
  {
    nr= time_max_value;
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
