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

#ifdef WITH_WSREP

#define IF_WSREP(A,B) A

#define DBUG_ASSERT_IF_WSREP(A) DBUG_ASSERT(A)

#define WSREP_MYSQL_DB (char *)"mysql"

#define WSREP_TO_ISOLATION_BEGIN(db_, table_, table_list_)              \
  if (WSREP_ON && WSREP(thd) &&                                         \
      wsrep_to_isolation_begin(thd, db_, table_, table_list_))          \
    goto wsrep_error_label;

#define WSREP_TO_ISOLATION_BEGIN_CREATE(db_, table_, table_list_, create_info_)	\
  if (WSREP_ON && WSREP(thd) &&                                                 \
      wsrep_to_isolation_begin(thd, db_, table_,                                \
                               table_list_, nullptr, nullptr, create_info_))    \
    goto wsrep_error_label;

#define WSREP_TO_ISOLATION_BEGIN_ALTER(db_, table_, table_list_, alter_info_, fk_tables_, create_info_)	\
  if (WSREP(thd) &&                                                     \
      wsrep_to_isolation_begin(thd, db_, table_,                        \
                               table_list_, alter_info_,                \
                               fk_tables_, create_info_))

/*
  Checks if lex->no_write_to_binlog is set for statements that use LOCAL or
  NO_WRITE_TO_BINLOG.
*/
#define WSREP_TO_ISOLATION_BEGIN_WRTCHK(db_, table_, table_list_)       \
  if (WSREP(thd) && !thd->lex->no_write_to_binlog &&                    \
    wsrep_to_isolation_begin(thd, db_, table_, table_list_))            \
    goto wsrep_error_label;

#define WSREP_SYNC_WAIT(thd_, before_)                                  \
    { if (WSREP_CLIENT(thd_) &&                                         \
          wsrep_sync_wait(thd_, before_)) goto wsrep_error_label; }

#else /* !WITH_WSREP */

/* These macros are needed to compile MariaDB without WSREP support
 * (e.g. embedded) */

#define IF_WSREP(A,B) B
#define WSREP_DEBUG(...)
#define WSREP_ERROR(...)
#define WSREP_TO_ISOLATION_BEGIN(db_, table_, table_list_) do { } while(0)
#define WSREP_TO_ISOLATION_BEGIN_ALTER(db_, table_, table_list_, alter_info_, fk_tables_, create_info_)
#define WSREP_TO_ISOLATION_BEGIN_CREATE(db_, table_, table_list_, create_info_)
#define WSREP_TO_ISOLATION_BEGIN_WRTCHK(db_, table_, table_list_)
#define WSREP_SYNC_WAIT(thd_, before_)
#endif /* WITH_WSREP */

#endif /* WSREP_INCLUDED */
