#ifndef MYSQL_SERVICE_MY_SNPRINTF_INCLUDED
/* Copyright (c) 2009, 2012, Oracle and/or its affiliates. All rights reserved.

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
  my_snprintf service

  Portable and limited vsnprintf() implementation.

  This is a portable, limited vsnprintf() implementation, with some
  extra features. "Portable" means that it'll produce identical result
  on all platforms (for example, on Windows and Linux system printf %e
  formats the exponent differently, on different systems %p either
  prints leading 0x or not, %s may accept null pointer or crash on
  it). "Limited" means that it does not support all the C89 features.
  But it supports few extensions, not in any standard.

  my_vsnprintf(to, n, fmt, ap)

  @param[out] to     A buffer to store the result in
  @param[in]  n      Store up to n-1 characters, followed by an end 0
  @param[in]  fmt    printf-like format string
  @param[in]  ap     Arguments

  @return a number of bytes written to a buffer *excluding* terminating '\0'

  @note
  The syntax of a format string is generally the same:
  %[<flag>][<length>][.<precision>][<size modifier>]<format>[<format extension>]
  where everything but the <format> is optional.

  Two one-character <flags> are recognized:
    '0' has the standard zero-padding semantics;
    '-' is parsed, but silently ignored;

  Both <length> and <precision> are the same as in the standard.
  They can be specified as integers, or as '*' to consume an int argument.

  <size modifier> can be 'l', 'll', or 'z'.

  Supported <format>s are 's' (null pointer is accepted, printed as "(null)"),
  'c', 'd', 'i', 'u', 'x', 'X', 'o', 'p' (works as "0x%x"), 'f', and 'g'.

  The '$n' syntax for positional arguments is supported.

  Format extensions:

    Format 'sQ'
      quotes the string with '`' (backtick)s similar to "`%s`",
      but also "escapes" existing '`'s in the string to '``' as in SQL ''''.

    Format 'sB'
      treats the argument as a byte sequence. It reads and prints exactly
      <precision> bytes without terminating on any '\0's in the sequence.
      The default <precision> when it's unspecified is not defined.

    Format 'sT'
      replaces the end of the printed string with "..." if it was truncated.

    Format 'sS'
      is a synonym for 's'. It's an escape that avoid
      consuming the following plain char as one of the above extension suffixes.
      Example: "Data Class: %sSType"

    Format 'iE'
      treats the argument as an errno number. It prints this number, a space,
      then its corresponding error message in double quotes. In other words:
        printf("%iE", n) === printf("%i \"%sT\"", n, strerror(n))
      Format 'dE' has no effect. Therefore, to escape '%iE', use '%dE' instead.

    Unrecognized and multiple suffixes are not parsed;
      for example, both "%sTQ" and "%iQ" will suffix with a literal 'Q'.
*/

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdarg.h>
#include <stdlib.h>
#endif
#include <my_attribute.h>

extern struct my_snprintf_service_st {
  size_t (*my_snprintf_type)(char*, size_t, const char*, ...)
    ATTRIBUTE_FORMAT_FPTR(printf, 3, 4);
  size_t (*my_vsnprintf_type)(char *, size_t, const char*, va_list)
    ATTRIBUTE_FORMAT_FPTR(printf, 3, 0);
} *my_snprintf_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define my_vsnprintf my_snprintf_service->my_vsnprintf_type
#define my_snprintf my_snprintf_service->my_snprintf_type

#else

size_t my_snprintf(char* to, size_t n, const char* fmt, ...)
  ATTRIBUTE_FORMAT(printf, 3, 4);
size_t my_vsnprintf(char *to, size_t n, const char* fmt, va_list ap)
  ATTRIBUTE_FORMAT(printf, 3, 0);

#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_MY_SNPRINTF_INCLUDED
#endif
