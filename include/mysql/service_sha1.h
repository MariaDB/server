#ifndef MYSQL_SERVICE_SHA1_INCLUDED
/* Copyright (c) 2013, 2014, Monty Program Ab

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
  my sha1 service

  Functions to calculate SHA1 hash from a memory buffer
*/

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdlib.h>
#endif

#define MY_SHA1_HASH_SIZE 20 /* Hash size in bytes */

extern struct my_sha1_service_st {
  void (*my_sha1_type)(unsigned char*, const char*, size_t);
  void (*my_sha1_multi_type)(unsigned char*, ...);
  size_t (*my_sha1_context_size_type)();
  void (*my_sha1_init_type)(void *);
  void (*my_sha1_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha1_result_type)(void *, unsigned char *);
} *my_sha1_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define my_sha1(A,B,C) my_sha1_service->my_sha1_type(A,B,C)
#define my_sha1_multi my_sha1_service->my_sha1_multi_type
#define my_sha1_context_size() my_sha1_service->my_sha1_context_size_type()
#define my_sha1_init(A) my_sha1_service->my_sha1_init_type(A)
#define my_sha1_input(A,B,C) my_sha1_service->my_sha1_input_type(A,B,C)
#define my_sha1_result(A,B) my_sha1_service->my_sha1_result_type(A,B)

#else

void my_sha1(unsigned char*, const char*, size_t);
void my_sha1_multi(unsigned char*, ...);
size_t my_sha1_context_size();
void my_sha1_init(void *context);
void my_sha1_input(void *context, const unsigned char *buf, size_t len);
void my_sha1_result(void *context, unsigned char *digest);

#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_SHA1_INCLUDED
#endif

