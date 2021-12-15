#ifndef COMPAT56_H_INCLUDED
#define COMPAT56_H_INCLUDED
/*
   Copyright (c) 2004, 2012, Oracle and/or its affiliates.
   Copyright (c) 2013  MariaDB Foundation.

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


/** MySQL56 routines and macros **/
#define MY_PACKED_TIME_GET_INT_PART(x)     ((x) >> 24)
#define MY_PACKED_TIME_GET_FRAC_PART(x)    ((x) % (1LL << 24))
#define MY_PACKED_TIME_MAKE(i, f)          ((((longlong) (i)) << 24) + (f))
#define MY_PACKED_TIME_MAKE_INT(i)         ((((longlong) (i)) << 24))

longlong TIME_to_longlong_datetime_packed(const MYSQL_TIME *);
longlong TIME_to_longlong_time_packed(const MYSQL_TIME *);

void TIME_from_longlong_datetime_packed(MYSQL_TIME *ltime, longlong nr);
void TIME_from_longlong_time_packed(MYSQL_TIME *ltime, longlong nr);

void my_datetime_packed_to_binary(longlong nr, uchar *ptr, uint dec);
longlong my_datetime_packed_from_binary(const uchar *ptr, uint dec);
uint my_datetime_binary_length(uint dec);

void my_time_packed_to_binary(longlong nr, uchar *ptr, uint dec);
longlong my_time_packed_from_binary(const uchar *ptr, uint dec);
uint my_time_binary_length(uint dec);

void my_timestamp_to_binary(const struct timeval *tm, uchar *ptr, uint dec);
void my_timestamp_from_binary(struct timeval *tm, const uchar *ptr, uint dec);
uint my_timestamp_binary_length(uint dec);
/** End of MySQL routines and macros **/

#endif /* COMPAT56_H_INCLUDED */
