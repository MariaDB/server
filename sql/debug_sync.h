#ifndef DEBUG_SYNC_INCLUDED
#define DEBUG_SYNC_INCLUDED

/* Copyright (c) 2009, 2010, Oracle and/or its affiliates.

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

  Declarations for the Debug Sync Facility. See debug_sync.cc for details.
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface                      /* gcc class implementation */
#endif

class THD;

#if defined(ENABLED_DEBUG_SYNC)

/* Command line option --debug-sync-timeout. See mysqld.cc. */
extern MYSQL_PLUGIN_IMPORT uint opt_debug_sync_timeout;

/* Default WAIT_FOR timeout if command line option is given without argument. */
#define DEBUG_SYNC_DEFAULT_WAIT_TIMEOUT 300

/* Debug Sync prototypes. See debug_sync.cc. */
extern int  debug_sync_init(void);
extern void debug_sync_end(void);
extern void debug_sync_init_thread(THD *thd);
extern void debug_sync_end_thread(THD *thd);
void debug_sync_reset_thread(THD *thd);
extern bool debug_sync_set_action(THD *thd, const char *action_str, size_t len);
extern bool debug_sync_update(THD *thd, char *val_str, size_t len);
extern uchar *debug_sync_value_ptr(THD *thd);
#else
static inline void debug_sync_init_thread(THD *thd) {}
static inline void debug_sync_end_thread(THD *thd) {}
static inline void debug_sync_reset_thread(THD *thd) {}
static inline bool debug_sync_set_action(THD *, const char *, size_t)
{ return false; }
#endif /* defined(ENABLED_DEBUG_SYNC) */
#endif /* DEBUG_SYNC_INCLUDED */
