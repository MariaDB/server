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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef MYSQL_SERVICE_SHA2_INCLUDED
#define MYSQL_SERVICE_SHA2_INCLUDED

/**
  @file
  my sha2 service

  Functions to calculate SHA2 hash from a memory buffer
*/

/**
  @defgroup plugin_api_service_sha2 SHA2 service
  @ingroup plugin_api_services
  my sha2 service

  Functions to calculate SHA2 hash from a memory buffer
  @{
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

#define my_sha224(digest, buf, len) my_sha2_service->my_sha224_type(digest, buf, len)
#define my_sha224_multi(digest, ...) my_sha2_service->my_sha224_multi_type(digest, __VA_ARGS__)
#define my_sha224_context_size() my_sha2_service->my_sha224_context_size_type()
#define my_sha224_init(context) my_sha2_service->my_sha224_init_type(context)
#define my_sha224_input(context, buf, len) my_sha2_service->my_sha224_input_type(context, buf, len)
#define my_sha224_result(context, digest) my_sha2_service->my_sha224_result_type(context, digest)

#define my_sha256(digest, buf, len) my_sha2_service->my_sha256_type(digest, buf, len)
#define my_sha256_multi(digest, ...) my_sha2_service->my_sha256_multi_type(digest, __VA_ARGS__)
#define my_sha256_context_size() my_sha2_service->my_sha256_context_size_type()
#define my_sha256_init(context) my_sha2_service->my_sha256_init_type(context)
#define my_sha256_input(context, buf, len) my_sha2_service->my_sha256_input_type(context, buf, len)
#define my_sha256_result(context, digest) my_sha2_service->my_sha256_result_type(context, digest)

#define my_sha384(digest, buf, len) my_sha2_service->my_sha384_type(digest, buf, len)
#define my_sha384_multi(digest, ...) my_sha2_service->my_sha384_multi_type(digest, __VA_ARGS__)
#define my_sha384_context_size() my_sha2_service->my_sha384_context_size_type()
#define my_sha384_init(context) my_sha2_service->my_sha384_init_type(context)
#define my_sha384_input(context, buf, len) my_sha2_service->my_sha384_input_type(context, buf, len)
#define my_sha384_result(context, digest) my_sha2_service->my_sha384_result_type(context, digest)

#define my_sha512(digest, buf, len) my_sha2_service->my_sha512_type(digest, buf, len)
#define my_sha512_multi(digest, ...) my_sha2_service->my_sha512_multi_type(digest, __VA_ARGS__)
#define my_sha512_context_size() my_sha2_service->my_sha512_context_size_type()
#define my_sha512_init(context) my_sha2_service->my_sha512_init_type(context)
#define my_sha512_input(context, buf, len) my_sha2_service->my_sha512_input_type(context, buf, len)
#define my_sha512_result(context, digest) my_sha2_service->my_sha512_result_type(context, digest)

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

/** @} */

#endif

