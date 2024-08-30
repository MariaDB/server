/* Copyright (c) 2012, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef MYSQL_MEMORY_H
#define MYSQL_MEMORY_H

/**
  @file mysql/psi/mysql_memory.h
  Instrumentation helpers for memory allocation.
*/

#include "mysql/psi/psi.h"

#ifdef HAVE_PSI_MEMORY_INTERFACE
#define PSI_CALL_memory_alloc(A1,A2,A3) PSI_MEMORY_CALL(memory_alloc)(A1,A2,A3)
#define PSI_CALL_memory_free(A1,A2,A3) PSI_MEMORY_CALL(memory_free)(A1,A2,A3)
#define PSI_CALL_memory_realloc(A1,A2,A3,A4) PSI_MEMORY_CALL(memory_realloc)(A1,A2,A3,A4)
#define PSI_CALL_register_memory(A1,A2,A3) PSI_MEMORY_CALL(register_memory)(A1,A2,A3)
#else
#define PSI_CALL_memory_alloc(A1,A2,A3) 0
#define PSI_CALL_memory_free(A1,A2,A3) do { } while(0)
#define PSI_CALL_memory_realloc(A1,A2,A3,A4) 0
#define PSI_CALL_register_memory(A1,A2,A3) do { } while(0)
#endif

#ifndef PSI_MEMORY_CALL
#define PSI_MEMORY_CALL(M) PSI_DYNAMIC_CALL(M)
#endif

/**
  @defgroup Memory_instrumentation Memory Instrumentation
  @ingroup Instrumentation_interface
  @{
*/

/**
  @def mysql_memory_register(P1, P2, P3)
  Memory registration.
*/
#define mysql_memory_register(P1, P2, P3) \
  inline_mysql_memory_register(P1, P2, P3)

static inline void inline_mysql_memory_register(
#ifdef HAVE_PSI_MEMORY_INTERFACE
  const char *category,
  PSI_memory_info *info,
  int count)
#else
  const char *category __attribute__((unused)),
  void *info __attribute__((unused)),
  int count __attribute__((unused)))
#endif
{
  PSI_CALL_register_memory(category, info, count);
}

/** @} (end of group Memory_instrumentation) */

#endif

