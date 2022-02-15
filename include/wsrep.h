/* Copyright 2014 Codership Oy <http://www.codership.com> & SkySQL Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef WSREP_INCLUDED
#define WSREP_INCLUDED

#include <my_config.h>
#include "log.h"

#ifdef WITH_WSREP
#define IF_WSREP(A,B) A
#define DBUG_ASSERT_IF_WSREP(A) DBUG_ASSERT(A)

extern ulong wsrep_debug; // wsrep_mysqld.cc
extern void WSREP_LOG(void (*fun)(const char* fmt, ...), const char* fmt, ...);

#define WSREP_DEBUG(...)                                                \
    if (wsrep_debug)     WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_INFO(...)  WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_WARN(...)  WSREP_LOG(sql_print_warning,     ##__VA_ARGS__)
#define WSREP_ERROR(...) WSREP_LOG(sql_print_error,       ##__VA_ARGS__)
#define WSREP_UNKNOWN(fmt, ...) WSREP_ERROR("UNKNOWN: " fmt, ##__VA_ARGS__)

#define WSREP_LOG_CONFLICT_THD(thd, role)                               \
  WSREP_INFO("%s: \n "                                                  \
             "  THD: %lu, mode: %s, state: %s, conflict: %s, seqno: %lld\n " \
             "  SQL: %s",                                               \
             role,                                                      \
             thd_get_thread_id(thd),                                    \
             wsrep_thd_client_mode_str(thd),                            \
             wsrep_thd_client_state_str(thd),                           \
             wsrep_thd_transaction_state_str(thd),                      \
             wsrep_thd_trx_seqno(thd),                                  \
             wsrep_thd_query(thd)                                       \
            );

#define WSREP_LOG_CONFLICT(bf_thd, victim_thd, bf_abort)                \
  if (wsrep_debug || wsrep_log_conflicts)                               \
  {                                                                     \
    WSREP_INFO("cluster conflict due to %s for threads:",               \
               (bf_abort) ? "high priority abort" : "certification failure" \
              );                                                        \
    if (bf_thd)     WSREP_LOG_CONFLICT_THD(bf_thd, "Winning thread");   \
    if (victim_thd) WSREP_LOG_CONFLICT_THD(victim_thd, "Victim thread"); \
    WSREP_INFO("context: %s:%d", __FILE__, __LINE__); \
  }


#else /* !WITH_WSREP */

/* These macros are needed to compile MariaDB without WSREP support
 * (e.g. embedded) */

#define IF_WSREP(A,B) B
//#define DBUG_ASSERT_IF_WSREP(A)
#define WSREP_DEBUG(...)
//#define WSREP_INFO(...)
//#define WSREP_WARN(...)
#define WSREP_ERROR(...)
#endif /* WITH_WSREP */

#endif /* WSREP_INCLUDED */
