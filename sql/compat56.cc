/*
   Copyright (c) 2004, 2012, Oracle and/or its affiliates.
   Copyright (c) 2013, MariaDB Foundation.

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

#include "my_global.h"
#include "compat56.h"
#include "myisampack.h"
#include "my_time.h"

/*** MySQL56 TIME low-level memory and disk representation routines ***/

/*
  In-memory format:

   1  bit sign          (Used for sign, when on disk)
   1  bit unused        (Reserved for wider hour range, e.g. for intervals)
   10 bit hour          (0-836)
   6  bit minute        (0-59)
   6  bit second        (0-59)
  24  bits microseconds (0-999999)

 Total: 48 bits = 6 bytes
   Suhhhhhh.hhhhmmmm.mmssssss.ffffffff.ffffffff.ffffffff
*/


/**
  Convert time value to MySQL56 numeric packed representation.
  
  @param    ltime   The value to convert.
  @return           Numeric packed representation.
*/
longlong TIME_to_longlong_time_packed(const MYSQL_TIME *ltime)
{
  /* If month is 0, we mix day with hours: "1 00:10:10" -> "24:00:10" */
  long hms= (((ltime->month ? 0 : ltime->day * 24) + ltime->hour) << 12) |
            (ltime->minute << 6) | ltime->second;
  longlong tmp= MY_PACKED_TIME_MAKE(hms, ltime->second_part);
  return ltime->neg ? -tmp : tmp;
}



/**
  Convert MySQL56 time packed numeric representation to time.

  @param  OUT ltime  The MYSQL_TIME variable to set.
  @param      tmp    The packed numeric representation.
*/
void TIME_from_longlong_time_packed(MYSQL_TIME *ltime, longlong tmp)
{
  long hms;
  if ((ltime->neg= (tmp < 0)))
    tmp= -tmp;
  hms= (long) MY_PACKED_TIME_GET_INT_PART(tmp);
  ltime->year=   (uint) 0;
  ltime->month=  (uint) 0;
  ltime->day=    (uint) 0;
  ltime->hour=   (uint) (hms >> 12) % (1 << 10); /* 10 bits starting at 12th */
  ltime->minute= (uint) (hms >> 6)  % (1 << 6);  /* 6 bits starting at 6th   */
  ltime->second= (uint)  hms        % (1 << 6);  /* 6 bits starting at 0th   */
  ltime->second_part= MY_PACKED_TIME_GET_FRAC_PART(tmp);
  ltime->time_type= MYSQL_TIMESTAMP_TIME;
}


/**
  Calculate binary size of MySQL56 packed numeric time representation.
  
  @param   dec   Precision.
*/
uint my_time_binary_length(uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  return 3 + (dec + 1) / 2;
}


/*
  On disk we convert from signed representation to unsigned
  representation using TIMEF_OFS, so all values become binary comparable.
*/
#define TIMEF_OFS 0x800000000000LL
#define TIMEF_INT_OFS 0x800000LL


/**
  Convert MySQL56 in-memory numeric time representation to on-disk representation
  
  @param       nr   Value in packed numeric time format.
  @param   OUT ptr  The buffer to put value at.
  @param       dec  Precision.
*/
void my_time_packed_to_binary(longlong nr, uchar *ptr, uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  /* Make sure the stored value was previously properly rounded or truncated */
  DBUG_ASSERT((MY_PACKED_TIME_GET_FRAC_PART(nr) % 
              (int) log_10_int[TIME_SECOND_PART_DIGITS - dec]) == 0);

  switch (dec)
  {
  case 0:
  default:
    mi_int3store(ptr, TIMEF_INT_OFS + MY_PACKED_TIME_GET_INT_PART(nr));
    break;

  case 1:
  case 2:
    mi_int3store(ptr, TIMEF_INT_OFS + MY_PACKED_TIME_GET_INT_PART(nr));
    ptr[3]= (unsigned char) (char) (MY_PACKED_TIME_GET_FRAC_PART(nr) / 10000);
    break;

  case 4:
  case 3:
    mi_int3store(ptr, TIMEF_INT_OFS + MY_PACKED_TIME_GET_INT_PART(nr));
    mi_int2store(ptr + 3, MY_PACKED_TIME_GET_FRAC_PART(nr) / 100);
    break;

  case 5:
  case 6:
    mi_int6store(ptr, nr + TIMEF_OFS);
    break;
  }
}


/**
  Convert MySQL56 on-disk time representation to in-memory packed numeric 
  representation.
  
  @param   ptr  The pointer to read the value at.
  @param   dec  Precision.
  @return       Packed numeric time representation.
*/
longlong my_time_packed_from_binary(const uchar *ptr, uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);

  switch (dec)
  {
  case 0:
  default:
    {
      longlong intpart= mi_uint3korr(ptr) - TIMEF_INT_OFS;
      return MY_PACKED_TIME_MAKE_INT(intpart);
    }
  case 1:
  case 2:
    {
      longlong intpart= mi_uint3korr(ptr) - TIMEF_INT_OFS;
      int frac= (uint) ptr[3];
      if (intpart < 0 && frac)
      {
        /*
          Negative values are stored with reverse fractional part order,
          for binary sort compatibility.

            Disk value  intpart frac   Time value   Memory value
            800000.00    0      0      00:00:00.00  0000000000.000000
            7FFFFF.FF   -1      255   -00:00:00.01  FFFFFFFFFF.FFD8F0
            7FFFFF.9D   -1      99    -00:00:00.99  FFFFFFFFFF.F0E4D0
            7FFFFF.00   -1      0     -00:00:01.00  FFFFFFFFFF.000000
            7FFFFE.FF   -1      255   -00:00:01.01  FFFFFFFFFE.FFD8F0
            7FFFFE.F6   -2      246   -00:00:01.10  FFFFFFFFFE.FE7960

            Formula to convert fractional part from disk format
            (now stored in "frac" variable) to absolute value: "0x100 - frac".
            To reconstruct in-memory value, we shift
            to the next integer value and then substruct fractional part.
        */
        intpart++;    /* Shift to the next integer value */
        frac-= 0x100; /* -(0x100 - frac) */
      }
      return MY_PACKED_TIME_MAKE(intpart, frac * 10000);
    }

  case 3:
  case 4:
    {
      longlong intpart= mi_uint3korr(ptr) - TIMEF_INT_OFS;
      int frac= mi_uint2korr(ptr + 3);
      if (intpart < 0 && frac)
      {
        /*
          Fix reverse fractional part order: "0x10000 - frac".
          See comments for FSP=1 and FSP=2 above.
        */
        intpart++;      /* Shift to the next integer value */
        frac-= 0x10000; /* -(0x10000-frac) */
      }
      return MY_PACKED_TIME_MAKE(intpart, frac * 100);
    }

  case 5:
  case 6:
    return ((longlong) mi_uint6korr(ptr)) - TIMEF_OFS;
  }
}


/*** MySQL56 DATETIME low-level memory and disk representation routines ***/

/*
    1 bit  sign            (used when on disk)
   17 bits year*13+month   (year 0-9999, month 0-12)
    5 bits day             (0-31)
    5 bits hour            (0-23)
    6 bits minute          (0-59)
    6 bits second          (0-59)
   24 bits microseconds    (0-999999)

   Total: 64 bits = 8 bytes

   SYYYYYYY.YYYYYYYY.YYdddddh.hhhhmmmm.mmssssss.ffffffff.ffffffff.ffffffff
*/

/**
  Convert datetime to MySQL56 packed numeric datetime representation.
  @param ltime  The value to convert.
  @return       Packed numeric representation of ltime.
*/
longlong TIME_to_longlong_datetime_packed(const MYSQL_TIME *ltime)
{
  longlong ymd= ((ltime->year * 13 + ltime->month) << 5) | ltime->day;
  longlong hms= (ltime->hour << 12) | (ltime->minute << 6) | ltime->second;
  longlong tmp= MY_PACKED_TIME_MAKE(((ymd << 17) | hms), ltime->second_part);
  DBUG_ASSERT(!check_datetime_range(ltime)); /* Make sure no overflow */
  return ltime->neg ? -tmp : tmp;
}


/**
  Convert MySQL56 packed numeric datetime representation to MYSQL_TIME.
  @param OUT  ltime The datetime variable to convert to.
  @param      tmp   The packed numeric datetime value.
*/
void TIME_from_longlong_datetime_packed(MYSQL_TIME *ltime, longlong tmp)
{
  longlong ymd, hms;
  longlong ymdhms, ym;

  DBUG_ASSERT(tmp != LONGLONG_MIN);

  if ((ltime->neg= (tmp < 0)))
    tmp= -tmp;

  ltime->second_part= MY_PACKED_TIME_GET_FRAC_PART(tmp);
  ymdhms= MY_PACKED_TIME_GET_INT_PART(tmp);

  ymd= ymdhms >> 17;
  ym= ymd >> 5;
  hms= ymdhms % (1 << 17);

  ltime->day= ymd % (1 << 5);
  ltime->month= ym % 13;
  ltime->year= (uint) (ym / 13);

  ltime->second= hms % (1 << 6);
  ltime->minute= (hms >> 6) % (1 << 6);
  ltime->hour= (uint) (hms >> 12);
  
  ltime->time_type= MYSQL_TIMESTAMP_DATETIME;
}


/**
  Calculate binary size of MySQL56 packed datetime representation.
  @param dec  Precision.
*/
uint my_datetime_binary_length(uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  return 5 + (dec + 1) / 2;
}


/*
  On disk we store as unsigned number with DATETIMEF_INT_OFS offset,
  for HA_KETYPE_BINARY compatibility purposes.
*/
#define DATETIMEF_INT_OFS 0x8000000000LL


/**
  Convert MySQL56 on-disk datetime representation
  to in-memory packed numeric representation.

  @param ptr   The pointer to read value at.
  @param dec   Precision.
  @return      In-memory packed numeric datetime representation.
*/
longlong my_datetime_packed_from_binary(const uchar *ptr, uint dec)
{
  longlong intpart= mi_uint5korr(ptr) - DATETIMEF_INT_OFS;
  int frac;
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  switch (dec)
  {
  case 0:
  default:
    return MY_PACKED_TIME_MAKE_INT(intpart);
  case 1:
  case 2:
    frac= ((int) (signed char) ptr[5]) * 10000;
    break;
  case 3:
  case 4:
    frac= mi_sint2korr(ptr + 5) * 100;
    break;
  case 5:
  case 6:
    frac= mi_sint3korr(ptr + 5);
    break;
  }
  return MY_PACKED_TIME_MAKE(intpart, frac);
}


/**
  Store MySQL56 in-memory numeric packed datetime representation to disk.

  @param      nr  In-memory numeric packed datetime representation.
  @param OUT  ptr The pointer to store at.
  @param      dec Precision, 1-6.
*/
void my_datetime_packed_to_binary(longlong nr, uchar *ptr, uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  /* The value being stored must have been properly rounded or truncated */
  DBUG_ASSERT((MY_PACKED_TIME_GET_FRAC_PART(nr) %
              (int) log_10_int[TIME_SECOND_PART_DIGITS - dec]) == 0);

  mi_int5store(ptr, MY_PACKED_TIME_GET_INT_PART(nr) + DATETIMEF_INT_OFS);
  switch (dec)
  {
  case 0:
  default:
    break;
  case 1:
  case 2:
    ptr[5]= (unsigned char) (char) (MY_PACKED_TIME_GET_FRAC_PART(nr) / 10000);
    break;
  case 3:
  case 4:
    mi_int2store(ptr + 5, MY_PACKED_TIME_GET_FRAC_PART(nr) / 100);
    break;
  case 5:
  case 6:
    mi_int3store(ptr + 5, MY_PACKED_TIME_GET_FRAC_PART(nr));
  }
}


/*** MySQL56 TIMESTAMP low-level memory and disk representation routines ***/

/**
  Calculate on-disk size of a timestamp value.

  @param  dec  Precision.
*/
uint my_timestamp_binary_length(uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  return 4 + (dec + 1) / 2;
}


/**
  Convert MySQL56 binary timestamp representation to in-memory representation.

  @param  OUT tm  The variable to convert to.
  @param      ptr The pointer to read the value from.
  @param      dec Precision.
*/
void my_timestamp_from_binary(struct timeval *tm, const uchar *ptr, uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  tm->tv_sec= mi_uint4korr(ptr);
  switch (dec)
  {
    case 0:
    default:
      tm->tv_usec= 0;
      break;
    case 1:
    case 2:
      tm->tv_usec= ((int) ptr[4]) * 10000;
      break;
    case 3:
    case 4:
      tm->tv_usec= mi_sint2korr(ptr + 4) * 100;
      break;
    case 5:
    case 6:
      tm->tv_usec= mi_sint3korr(ptr + 4);
  }
}


/**
  Convert MySQL56 in-memory timestamp representation to on-disk representation.

  @param        tm   The value to convert.
  @param  OUT   ptr  The pointer to store the value to.
  @param        dec  Precision.
*/
void my_timestamp_to_binary(const struct timeval *tm, uchar *ptr, uint dec)
{
  DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  /* Stored value must have been previously properly rounded or truncated */
  DBUG_ASSERT((tm->tv_usec %
               (int) log_10_int[TIME_SECOND_PART_DIGITS - dec]) == 0);
  mi_int4store(ptr, tm->tv_sec);
  switch (dec)
  {
    case 0:
    default:
      break;
    case 1:
    case 2:
      ptr[4]= (unsigned char) (char) (tm->tv_usec / 10000);
      break;
    case 3:
    case 4:
      mi_int2store(ptr + 4, tm->tv_usec / 100);
      break;
      /* Impossible second precision. Fall through */
    case 5:
    case 6:
      mi_int3store(ptr + 4, tm->tv_usec);
  }
}

/****************************************/
