#ifndef MYSQL_SERVICE_BASE64_INCLUDED
/* Copyright (c) 2017, MariaDB

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

/**
  @file
  my base64 service

  Functions for base64 en- and decoding
*/

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdlib.h>
#endif

/* Allow multuple chunks 'AAA= AA== AA==', binlog uses this */
#define MY_BASE64_DECODE_ALLOW_MULTIPLE_CHUNKS 1

extern struct base64_service_st {
  int (*base64_needed_encoded_length_ptr)(int length_of_data);
  int (*base64_encode_max_arg_length_ptr)(void);
  int (*base64_needed_decoded_length_ptr)(int length_of_encoded_data);
  int (*base64_decode_max_arg_length_ptr)();
  int (*base64_encode_ptr)(const void *src, size_t src_len, char *dst);
  int (*base64_decode_ptr)(const char *src, size_t src_len,
                           void *dst, const char **end_ptr, int flags);
} *base64_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define base64_needed_encoded_length(A) base64_service->base64_needed_encoded_length_ptr(A)
#define base64_encode_max_arg_length() base64_service->base64_encode_max_arg_length_ptr()
#define base64_needed_decoded_length(A) base64_service->base64_needed_decoded_length_ptr(A)
#define base64_decode_max_arg_length() base64_service->base64_decode_max_arg_length_ptr()
#define base64_encode(A,B,C) base64_service->base64_encode_ptr(A,B,C)
#define base64_decode(A,B,C,D,E) base64_service->base64_decode_ptr(A,B,C,D,E)

#else

/* Calculate how much memory needed for dst of base64_encode() */
int base64_needed_encoded_length(int length_of_data);

/* Maximum length base64_encode_needed_length() can accept with no overflow.  */
int base64_encode_max_arg_length(void);

/* Calculate how much memory needed for dst of base64_decode() */
int base64_needed_decoded_length(int length_of_encoded_data);

/* Maximum length base64_decode_needed_length() can accept with no overflow.  */
int base64_decode_max_arg_length();

/* Encode data as a base64 string */
int base64_encode(const void *src, size_t src_len, char *dst);

/* Decode a base64 string into data */
int base64_decode(const char *src, size_t src_len,
                  void *dst, const char **end_ptr, int flags);

#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_BASE64_INCLUDED
#endif
