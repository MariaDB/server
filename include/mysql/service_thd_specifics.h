#ifndef MYSQL_SERVICE_THD_SPECIFICS_INCLUDED
/* Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.

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

  THD specific for plugin(s)

  This API provides pthread_getspecific like functionality to plugin authors.
  This is a functional alternative to the declarative MYSQL_THDVAR

  A plugin should at init call thd_key_create that create a key that
  will have storage in each THD. The key should be used by all threads
  and can be used concurrently from all threads.

  A plugin should at deinit call thd_key_delete.

  Alternatively, a plugin can use thd_key_create_from_var(K,V) to create
  a key that corresponds to a named MYSQL_THDVAR variable.

  This API is also safe when using pool-of-threads in which case
  pthread_getspecific is not, because the actual OS thread may change.

  @note

  Normally one should prefer MYSQL_THDVAR declarative API.

  The benefits are:

  - It supports typed variables (int, char*, enum, etc), not only void*.
  - The memory allocated for MYSQL_THDVAR is free'd automatically
    (if PLUGIN_VAR_MEMALLOC is specified).
  - Continuous loading and unloading of the same plugin does not allocate
    memory for same variables over and over again.

  An example of using MYSQL_THDVAR for a thd local storage:

    MYSQL_THDVAR_STR(my_tls,
            PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT,
            "thd local storage example", 0, 0, 0);
*/

#ifdef __cplusplus
extern "C" {
#endif

typedef int MYSQL_THD_KEY_T;

extern struct thd_specifics_service_st {
  int (*thd_key_create_func)(MYSQL_THD_KEY_T *key);
  void (*thd_key_delete_func)(MYSQL_THD_KEY_T *key);
  void *(*thd_getspecific_func)(MYSQL_THD thd, MYSQL_THD_KEY_T key);
  int  (*thd_setspecific_func)(MYSQL_THD thd, MYSQL_THD_KEY_T key, void *value);
} *thd_specifics_service;

#define thd_key_create_from_var(K, V) do { *(K)= MYSQL_SYSVAR_NAME(V).offset; } while(0)

#ifdef MYSQL_DYNAMIC_PLUGIN

#define thd_key_create(K) (thd_specifics_service->thd_key_create_func(K))
#define thd_key_delete(K) (thd_specifics_service->thd_key_delete_func(K))
#define thd_getspecific(T, K) (thd_specifics_service->thd_getspecific_func(T, K))
#define thd_setspecific(T, K, V) (thd_specifics_service->thd_setspecific_func(T, K, V))

#else

/**
 * create THD specific storage
 * @return 0 on success
 *    else errno is returned
 */
int thd_key_create(MYSQL_THD_KEY_T *key);

/**
 * delete THD specific storage
 */
void thd_key_delete(MYSQL_THD_KEY_T *key);

/**
 * get/set thd specific storage
 *  - first time this is called from a thread it will return 0
 *  - this call is thread-safe in that different threads may call this
 *    simultaneously if operating on different THDs.
 *  - this call acquires no mutexes and is implemented as an array lookup
 */
void* thd_getspecific(MYSQL_THD thd, MYSQL_THD_KEY_T key);
int thd_setspecific(MYSQL_THD thd, MYSQL_THD_KEY_T key, void *value);

#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_THD_SPECIFICS_INCLUDED
#endif

