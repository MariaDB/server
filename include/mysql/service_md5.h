#ifndef MYSQL_SERVICE_MD5_INCLUDED
/* Copyright (c) 2014, Monty Program Ab

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
  my md5 service

  Functions to calculate MD5 hash from a memory buffer
*/

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdlib.h>
#endif

#define MY_MD5_HASH_SIZE 16 /* Hash size in bytes */

extern struct my_md5_service_st {
  void (*my_md5_type)(unsigned char*, const char*, size_t);
  void (*my_md5_multi_type)(unsigned char*, ...);
  size_t (*my_md5_context_size_type)();
  void (*my_md5_init_type)(void *);
  void (*my_md5_input_type)(void *, const unsigned char *, size_t);
  void (*my_md5_result_type)(void *, unsigned char *);
} *my_md5_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define my_md5(A,B,C) my_md5_service->my_md5_type(A,B,C)
#define my_md5_multi my_md5_service->my_md5_multi_type
#define my_md5_context_size() my_md5_service->my_md5_context_size_type()
#define my_md5_init(A) my_md5_service->my_md5_init_type(A)
#define my_md5_input(A,B,C) my_md5_service->my_md5_input_type(A,B,C)
#define my_md5_result(A,B) my_md5_service->my_md5_result_type(A,B)

#else

void my_md5(unsigned char*, const char*, size_t);
void my_md5_multi(unsigned char*, ...);
size_t my_md5_context_size();
void my_md5_init(void *context);
void my_md5_input(void *context, const unsigned char *buf, size_t len);
void my_md5_result(void *context, unsigned char *digest);

#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_MD5_INCLUDED
#endif

