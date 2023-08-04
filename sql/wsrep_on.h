/* Copyright 2022 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#ifndef WSREP_ON_H
#define WSREP_ON_H

#ifdef WITH_WSREP

extern bool WSREP_ON_;
extern bool WSREP_PROVIDER_EXISTS_;
extern my_bool wsrep_emulate_bin_log;
extern ulong wsrep_forced_binlog_format;

#define WSREP_ON unlikely(WSREP_ON_)

/* use xxxxxx_NNULL macros when thd pointer is guaranteed to be non-null to
 * avoid compiler warnings (GCC 6 and later) */

#define WSREP_NNULL(thd) \
  (WSREP_PROVIDER_EXISTS_ && thd->variables.wsrep_on)

#define WSREP(thd) \
  (thd && WSREP_NNULL(thd))

#define WSREP_CLIENT_NNULL(thd) \
  (WSREP_NNULL(thd) && thd->wsrep_client_thread)

#define WSREP_CLIENT(thd) \
    (WSREP(thd) && thd->wsrep_client_thread)

#define WSREP_EMULATE_BINLOG_NNULL(thd) \
  (WSREP_NNULL(thd) && wsrep_emulate_bin_log)

#define WSREP_EMULATE_BINLOG(thd) \
  (WSREP(thd) && wsrep_emulate_bin_log)

#else

#define WSREP_ON false
#define WSREP(T)  (0)
#define WSREP_NNULL(T) (0)
#define WSREP_EMULATE_BINLOG(thd) (0)
#define WSREP_EMULATE_BINLOG_NNULL(thd) (0)

#endif
#endif
