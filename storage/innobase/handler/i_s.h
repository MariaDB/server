/*****************************************************************************

Copyright (c) 2007, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file handler/i_s.h
InnoDB INFORMATION SCHEMA tables interface to MySQL.

Created July 18, 2007 Vasil Dimov
Modified Dec 29, 2014 Jan Lindström
*******************************************************/

#ifndef i_s_h
#define i_s_h
#include "dict0types.h"

const char plugin_author[] = "Oracle Corporation";
const char maria_plugin_author[] = "MariaDB Corporation";

extern struct st_maria_plugin	i_s_innodb_trx;
extern struct st_maria_plugin	i_s_innodb_locks;
extern struct st_maria_plugin	i_s_innodb_lock_waits;
extern struct st_maria_plugin	i_s_innodb_cmp;
extern struct st_maria_plugin	i_s_innodb_cmp_reset;
extern struct st_maria_plugin	i_s_innodb_cmp_per_index;
extern struct st_maria_plugin	i_s_innodb_cmp_per_index_reset;
extern struct st_maria_plugin	i_s_innodb_cmpmem;
extern struct st_maria_plugin	i_s_innodb_cmpmem_reset;
extern struct st_maria_plugin   i_s_innodb_metrics;
extern struct st_maria_plugin	i_s_innodb_ft_default_stopword;
extern struct st_maria_plugin	i_s_innodb_ft_deleted;
extern struct st_maria_plugin	i_s_innodb_ft_being_deleted;
extern struct st_maria_plugin	i_s_innodb_ft_index_cache;
extern struct st_maria_plugin	i_s_innodb_ft_index_table;
extern struct st_maria_plugin	i_s_innodb_ft_config;
extern struct st_maria_plugin	i_s_innodb_buffer_page;
extern struct st_maria_plugin	i_s_innodb_buffer_page_lru;
extern struct st_maria_plugin	i_s_innodb_buffer_stats;
extern struct st_maria_plugin	i_s_innodb_sys_tables;
extern struct st_maria_plugin	i_s_innodb_sys_tablestats;
extern struct st_maria_plugin	i_s_innodb_sys_indexes;
extern struct st_maria_plugin	i_s_innodb_sys_columns;
extern struct st_maria_plugin	i_s_innodb_sys_fields;
extern struct st_maria_plugin	i_s_innodb_sys_foreign;
extern struct st_maria_plugin	i_s_innodb_sys_foreign_cols;
extern struct st_maria_plugin	i_s_innodb_sys_tablespaces;
extern struct st_maria_plugin	i_s_innodb_sys_virtual;
extern struct st_maria_plugin	i_s_innodb_tablespaces_encryption;

/** The latest successfully looked up innodb_fts_aux_table */
extern table_id_t innodb_ft_aux_table_id;

/** maximum number of buffer page info we would cache. */
#define MAX_BUF_INFO_CACHED		10000

#define OK(expr)		\
	if ((expr) != 0) {	\
		DBUG_RETURN(1);	\
	}

#define BREAK_IF(expr) if ((expr)) break

#define RETURN_IF_INNODB_NOT_STARTED(plugin_name)			\
do {									\
	if (!srv_was_started) {						\
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,	\
				    ER_CANT_FIND_SYSTEM_REC,		\
				    "InnoDB: SELECTing from "		\
				    "INFORMATION_SCHEMA.%s but "	\
				    "the InnoDB storage engine "	\
				    "is not installed", plugin_name);	\
		DBUG_RETURN(0);						\
	}								\
} while (0)

#endif /* i_s_h */
