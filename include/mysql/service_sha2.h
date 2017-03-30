#ifndef MYSQL_SERVICE_SHA2_INCLUDED
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
  my sha2 service

  Functions to calculate SHA2 hash from a memory buffer
*/

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdlib.h>
#endif

extern struct my_sha2_service_st {
  void (*my_sha224_type)(unsigned char*, const char*, size_t);
  void (*my_sha224_multi_type)(unsigned char*, ...);
  size_t (*my_sha224_context_size_type)();
  void (*my_sha224_init_type)(void *);
  void (*my_sha224_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha224_result_type)(void *, unsigned char *);

  void (*my_sha256_type)(unsigned char*, const char*, size_t);
  void (*my_sha256_multi_type)(unsigned char*, ...);
  size_t (*my_sha256_context_size_type)();
  void (*my_sha256_init_type)(void *);
  void (*my_sha256_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha256_result_type)(void *, unsigned char *);

  void (*my_sha384_type)(unsigned char*, const char*, size_t);
  void (*my_sha384_multi_type)(unsigned char*, ...);
  size_t (*my_sha384_context_size_type)();
  void (*my_sha384_init_type)(void *);
  void (*my_sha384_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha384_result_type)(void *, unsigned char *);

  void (*my_sha512_type)(unsigned char*, const char*, size_t);
  void (*my_sha512_multi_type)(unsigned char*, ...);
  size_t (*my_sha512_context_size_type)();
  void (*my_sha512_init_type)(void *);
  void (*my_sha512_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha512_result_type)(void *, unsigned char *);
} *my_sha2_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define my_sha224(A,B,C) my_sha2_service->my_sha224_type(A,B,C)
#define my_sha224_multi my_sha2_service->my_sha224_multi_type
#define my_sha224_context_size() my_sha2_service->my_sha224_context_size_type()
#define my_sha224_init(A) my_sha2_service->my_sha224_init_type(A)
#define my_sha224_input(A,B,C) my_sha2_service->my_sha224_input_type(A,B,C)
#define my_sha224_result(A,B) my_sha2_service->my_sha224_result_type(A,B)

#define my_sha256(A,B,C) my_sha2_service->my_sha256_type(A,B,C)
#define my_sha256_multi my_sha2_service->my_sha256_multi_type
#define my_sha256_context_size() my_sha2_service->my_sha256_context_size_type()
#define my_sha256_init(A) my_sha2_service->my_sha256_init_type(A)
#define my_sha256_input(A,B,C) my_sha2_service->my_sha256_input_type(A,B,C)
#define my_sha256_result(A,B) my_sha2_service->my_sha256_result_type(A,B)

#define my_sha384(A,B,C) my_sha2_service->my_sha384_type(A,B,C)
#define my_sha384_multi my_sha2_service->my_sha384_multi_type
#define my_sha384_context_size() my_sha2_service->my_sha384_context_size_type()
#define my_sha384_init(A) my_sha2_service->my_sha384_init_type(A)
#define my_sha384_input(A,B,C) my_sha2_service->my_sha384_input_type(A,B,C)
#define my_sha384_result(A,B) my_sha2_service->my_sha384_result_type(A,B)

#define my_sha512(A,B,C) my_sha2_service->my_sha512_type(A,B,C)
#define my_sha512_multi my_sha2_service->my_sha512_multi_type
#define my_sha512_context_size() my_sha2_service->my_sha512_context_size_type()
#define my_sha512_init(A) my_sha2_service->my_sha512_init_type(A)
#define my_sha512_input(A,B,C) my_sha2_service->my_sha512_input_type(A,B,C)
#define my_sha512_result(A,B) my_sha2_service->my_sha512_result_type(A,B)

#else

void my_sha224(unsigned char*, const char*, size_t);
void my_sha224_multi(unsigned char*, ...);
size_t my_sha224_context_size();
void my_sha224_init(void *context);
void my_sha224_input(void *context, const unsigned char *buf, size_t len);
void my_sha224_result(void *context, unsigned char *digest);

void my_sha256(unsigned char*, const char*, size_t);
void my_sha256_multi(unsigned char*, ...);
size_t my_sha256_context_size();
void my_sha256_init(void *context);
void my_sha256_input(void *context, const unsigned char *buf, size_t len);
void my_sha256_result(void *context, unsigned char *digest);

void my_sha384(unsigned char*, const char*, size_t);
void my_sha384_multi(unsigned char*, ...);
size_t my_sha384_context_size();
void my_sha384_init(void *context);
void my_sha384_input(void *context, const unsigned char *buf, size_t len);
void my_sha384_result(void *context, unsigned char *digest);

void my_sha512(unsigned char*, const char*, size_t);
void my_sha512_multi(unsigned char*, ...);
size_t my_sha512_context_size();
void my_sha512_init(void *context);
void my_sha512_input(void *context, const unsigned char *buf, size_t len);
void my_sha512_result(void *context, unsigned char *digest);

#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_SHA2_INCLUDED
#endif

