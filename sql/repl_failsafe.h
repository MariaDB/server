#ifndef REPL_FAILSAFE_INCLUDED
#define REPL_FAILSAFE_INCLUDED

/* Copyright (c) 2001, 2011, Oracle and/or its affiliates. All rights reserved.

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

#ifdef HAVE_REPLICATION

#include "mysql.h"
#include <my_sys.h>
#include "slave.h"

extern Atomic_counter<uint32_t> binlog_dump_thread_count;
typedef enum {RPL_AUTH_MASTER=0,RPL_IDLE_SLAVE,RPL_ACTIVE_SLAVE,
	      RPL_LOST_SOLDIER,RPL_TROOP_SOLDIER,
	      RPL_RECOVERY_CAPTAIN,RPL_NULL /* inactive */,
	      RPL_ANY /* wild card used by change_rpl_status */ } RPL_STATUS;
extern ulong rpl_status;

extern mysql_mutex_t LOCK_rpl_status;
extern mysql_cond_t COND_rpl_status;
extern TYPELIB rpl_role_typelib;
extern const char* rpl_role_type[], *rpl_status_type[];

void change_rpl_status(ulong from_status, ulong to_status);
int find_recovery_captain(THD* thd, MYSQL* mysql);

bool show_slave_hosts(THD* thd);

#endif /* HAVE_REPLICATION */
#endif /* REPL_FAILSAFE_INCLUDED */
