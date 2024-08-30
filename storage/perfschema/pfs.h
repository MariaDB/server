/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.

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
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef PFS_H
#define PFS_H

/**
  @file storage/perfschema/pfs.h
  Performance schema instrumentation (declarations).
*/

#define HAVE_PSI_1

#include <my_global.h>
#include "my_thread.h"
#include <mysql/psi/psi.h>

/**
  Entry point to the performance schema implementation.
  This singleton is used to discover the performance schema services.
*/
extern struct PSI_bootstrap PFS_bootstrap;
/** Performance schema Thread Local Storage key.  */
struct PFS_thread;

extern MY_THREAD_LOCAL PFS_thread* THR_PFS;
extern MY_THREAD_LOCAL void* THR_PFS_VG;  // global_variables
extern MY_THREAD_LOCAL void* THR_PFS_SV;  // session_variables
extern MY_THREAD_LOCAL void* THR_PFS_VBT; // variables_by_thread
extern MY_THREAD_LOCAL void* THR_PFS_SG;  // global_status
extern MY_THREAD_LOCAL void* THR_PFS_SS;  // session_status
extern MY_THREAD_LOCAL void* THR_PFS_SBT; // status_by_thread
extern MY_THREAD_LOCAL void* THR_PFS_SBU; // status_by_user
extern MY_THREAD_LOCAL void* THR_PFS_SBA; // status_by_host
extern MY_THREAD_LOCAL void* THR_PFS_SBH; // status_by_account

#define PSI_VOLATILITY_UNKNOWN 0
#define PSI_VOLATILITY_SESSION 1

#define PSI_COUNT_VOLATILITY 2

#endif

