/* Copyright (c) 2010, 2012, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2017, MariaDB Corporation.

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

#ifndef MYSQL_TABLE_H
#define MYSQL_TABLE_H

/**
  @file mysql/psi/mysql_table.h
  Instrumentation helpers for table io.
*/

#include "mysql/psi/psi.h"

/**
  @defgroup Table_instrumentation Table Instrumentation
  @ingroup Instrumentation_interface
  @{
*/

#ifdef HAVE_PSI_TABLE_INTERFACE
#define MYSQL_UNBIND_TABLE(handler) (handler)->unbind_psi()
#define MYSQL_REBIND_TABLE(handler) (handler)->rebind_psi()

#define PSI_CALL_unbind_table           PSI_TABLE_CALL(unbind_table)
#define PSI_CALL_rebind_table           PSI_TABLE_CALL(rebind_table)
#define PSI_CALL_open_table             PSI_TABLE_CALL(open_table)
#define PSI_CALL_close_table            PSI_TABLE_CALL(close_table)
#define PSI_CALL_get_table_share        PSI_TABLE_CALL(get_table_share)
#define PSI_CALL_release_table_share    PSI_TABLE_CALL(release_table_share)
#define PSI_CALL_drop_table_share       PSI_TABLE_CALL(drop_table_share)
#else
#define MYSQL_UNBIND_TABLE(handler)                     do { } while(0)
#define MYSQL_REBIND_TABLE(handler)                     do { } while(0)

#define PSI_CALL_unbind_table(A1)                       do { } while(0)
#define PSI_CALL_rebind_table(A1,A2,A3)                 NULL
#define PSI_CALL_close_table(A1)                        do { } while(0)
#define PSI_CALL_open_table(A1,A2)                      NULL
#define PSI_CALL_get_table_share(A1,A2)                 NULL
#define PSI_CALL_release_table_share(A1)                do { } while(0)
#define PSI_CALL_drop_table_share(A1,A2,A3,A4,A5)       do { } while(0)
#endif

/**
  @def MYSQL_TABLE_WAIT_VARIABLES
  Instrumentation helper for table waits.
  This instrumentation declares local variables.
  Do not use a ';' after this macro
  @param LOCKER the locker
  @param STATE the locker state
  @sa MYSQL_START_TABLE_IO_WAIT.
  @sa MYSQL_END_TABLE_IO_WAIT.
  @sa MYSQL_START_TABLE_LOCK_WAIT.
  @sa MYSQL_END_TABLE_LOCK_WAIT.
*/
#ifdef HAVE_PSI_TABLE_INTERFACE
  #define MYSQL_TABLE_WAIT_VARIABLES(LOCKER, STATE) \
    struct PSI_table_locker* LOCKER; \
    PSI_table_locker_state STATE;
#else
  #define MYSQL_TABLE_WAIT_VARIABLES(LOCKER, STATE)
#endif

/**
  @def MYSQL_TABLE_IO_WAIT
  Instrumentation helper for table io_waits.
  This instrumentation marks the start of a wait event.
  @param PSI the instrumented table
  @param OP the table operation to be performed
  @param INDEX the table index used if any, or MAY_KEY.
  @param FLAGS per table operation flags.
  @sa MYSQL_END_TABLE_WAIT.
*/
#ifdef HAVE_PSI_TABLE_INTERFACE
  #define MYSQL_TABLE_IO_WAIT(PSI, OP, INDEX, FLAGS, PAYLOAD) \
    {                                                         \
      if (psi_likely(PSI != NULL))                            \
      {                                                       \
        PSI_table_locker *locker;                             \
        PSI_table_locker_state state;                         \
        locker= PSI_TABLE_CALL(start_table_io_wait)           \
          (& state, PSI, OP, INDEX, __FILE__, __LINE__);      \
        PAYLOAD                                               \
        if (locker != NULL)                                   \
          PSI_TABLE_CALL(end_table_io_wait)(locker);          \
      }                                                       \
      else                                                    \
      {                                                       \
        PAYLOAD                                               \
      }                                                       \
    }
#else
  #define MYSQL_TABLE_IO_WAIT(PSI, OP, INDEX, FLAGS, PAYLOAD) \
    PAYLOAD
#endif

/**
  @def MYSQL_TABLE_LOCK_WAIT
  Instrumentation helper for table io_waits.
  This instrumentation marks the start of a wait event.
  @param PSI the instrumented table
  @param OP the table operation to be performed
  @param INDEX the table index used if any, or MAY_KEY.
  @param FLAGS per table operation flags.
  @sa MYSQL_END_TABLE_WAIT.
*/
#ifdef HAVE_PSI_TABLE_INTERFACE
  #define MYSQL_TABLE_LOCK_WAIT(PSI, OP, FLAGS, PAYLOAD) \
    {                                                    \
      if (psi_likely(PSI != NULL))                       \
      {                                                  \
        PSI_table_locker *locker;                        \
        PSI_table_locker_state state;                    \
        locker= PSI_TABLE_CALL(start_table_lock_wait)    \
          (& state, PSI, OP, FLAGS, __FILE__, __LINE__); \
        PAYLOAD                                          \
        if (locker != NULL)                              \
          PSI_TABLE_CALL(end_table_lock_wait)(locker);   \
      }                                                  \
      else                                               \
      {                                                  \
        PAYLOAD                                          \
      }                                                  \
    }
#else
  #define MYSQL_TABLE_LOCK_WAIT(PSI, OP, FLAGS, PAYLOAD) \
    PAYLOAD
#endif

/**
  @def MYSQL_START_TABLE_LOCK_WAIT
  Instrumentation helper for table lock waits.
  This instrumentation marks the start of a wait event.
  @param LOCKER the locker
  @param STATE the locker state
  @param PSI the instrumented table
  @param OP the table operation to be performed
  @param FLAGS per table operation flags.
  @sa MYSQL_END_TABLE_LOCK_WAIT.
*/
#ifdef HAVE_PSI_TABLE_INTERFACE
  #define MYSQL_START_TABLE_LOCK_WAIT(LOCKER, STATE, PSI, OP, FLAGS) \
    LOCKER= inline_mysql_start_table_lock_wait(STATE, PSI, \
                                               OP, FLAGS, __FILE__, __LINE__)
#else
  #define MYSQL_START_TABLE_LOCK_WAIT(LOCKER, STATE, PSI, OP, FLAGS) \
    do {} while (0)
#endif

/**
  @def MYSQL_END_TABLE_LOCK_WAIT
  Instrumentation helper for table lock waits.
  This instrumentation marks the end of a wait event.
  @param LOCKER the locker
  @sa MYSQL_START_TABLE_LOCK_WAIT.
*/
#ifdef HAVE_PSI_TABLE_INTERFACE
  #define MYSQL_END_TABLE_LOCK_WAIT(LOCKER) \
    inline_mysql_end_table_lock_wait(LOCKER)
#else
  #define MYSQL_END_TABLE_LOCK_WAIT(LOCKER) \
    do {} while (0)
#endif

#ifdef HAVE_PSI_TABLE_INTERFACE
/**
  Instrumentation calls for MYSQL_START_TABLE_LOCK_WAIT.
  @sa MYSQL_END_TABLE_LOCK_WAIT.
*/
static inline struct PSI_table_locker *
inline_mysql_start_table_lock_wait(PSI_table_locker_state *state,
                                   struct PSI_table *psi,
                                   enum PSI_table_lock_operation op,
                                   ulong flags, const char *src_file, uint src_line)
{
  if (psi_likely(psi != NULL))
  {
    struct PSI_table_locker *locker;
    locker= PSI_TABLE_CALL(start_table_lock_wait)
      (state, psi, op, flags, src_file, src_line);
    return locker;
  }
  return NULL;
}

/**
  Instrumentation calls for MYSQL_END_TABLE_LOCK_WAIT.
  @sa MYSQL_START_TABLE_LOCK_WAIT.
*/
static inline void
inline_mysql_end_table_lock_wait(struct PSI_table_locker *locker)
{
  if (psi_likely(locker != NULL))
    PSI_TABLE_CALL(end_table_lock_wait)(locker);
}
#endif

/** @} (end of group Table_instrumentation) */

#endif

