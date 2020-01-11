/*****************************************************************************

Copyright (c) 2007, 2013, Oracle and/or its affiliates. All Rights Reserved.
Copyrigth (c) 2014, 2019, MariaDB Corporation.

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

#define st_mysql_plugin st_maria_plugin

extern struct st_maria_plugin	i_s_innodb_trx;
extern struct st_mysql_plugin	i_s_innodb_trx;
extern struct st_mysql_plugin	i_s_innodb_locks;
extern struct st_mysql_plugin	i_s_innodb_lock_waits;
extern struct st_mysql_plugin	i_s_innodb_cmp;
extern struct st_mysql_plugin	i_s_innodb_cmp_reset;
extern struct st_mysql_plugin	i_s_innodb_cmp_per_index;
extern struct st_mysql_plugin	i_s_innodb_cmp_per_index_reset;
extern struct st_mysql_plugin	i_s_innodb_cmpmem;
extern struct st_mysql_plugin	i_s_innodb_cmpmem_reset;
extern struct st_mysql_plugin   i_s_innodb_metrics;
extern struct st_mysql_plugin	i_s_innodb_ft_default_stopword;
extern struct st_mysql_plugin	i_s_innodb_ft_deleted;
extern struct st_mysql_plugin	i_s_innodb_ft_being_deleted;
extern struct st_mysql_plugin	i_s_innodb_ft_index_cache;
extern struct st_mysql_plugin	i_s_innodb_ft_index_table;
extern struct st_mysql_plugin	i_s_innodb_ft_config;
extern struct st_mysql_plugin	i_s_innodb_buffer_page;
extern struct st_mysql_plugin	i_s_innodb_buffer_page_lru;
extern struct st_mysql_plugin	i_s_innodb_buffer_stats;
extern struct st_mysql_plugin	i_s_innodb_sys_tables;
extern struct st_mysql_plugin	i_s_innodb_sys_tablestats;
extern struct st_mysql_plugin	i_s_innodb_sys_indexes;
extern struct st_mysql_plugin	i_s_innodb_sys_columns;
extern struct st_mysql_plugin	i_s_innodb_sys_fields;
extern struct st_mysql_plugin	i_s_innodb_sys_foreign;
extern struct st_mysql_plugin	i_s_innodb_sys_foreign_cols;
extern struct st_mysql_plugin	i_s_innodb_sys_tablespaces;
extern struct st_mysql_plugin	i_s_innodb_sys_datafiles;
extern struct st_mysql_plugin	i_s_innodb_changed_pages;
extern struct st_mysql_plugin	i_s_innodb_mutexes;
extern struct st_maria_plugin	i_s_innodb_tablespaces_encryption;
extern struct st_maria_plugin	i_s_innodb_tablespaces_scrubbing;
extern struct st_mysql_plugin	i_s_innodb_sys_semaphore_waits;
extern struct st_mysql_plugin	i_s_innodb_changed_page_bitmaps;

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

#if !defined __STRICT_ANSI__ && defined __GNUC__ && (__GNUC__) > 2 && !defined __INTEL_COMPILER && !defined __clang__
#ifdef HAVE_C99_INITIALIZERS
#define STRUCT_FLD(name, value)	.name = value
#else
#define STRUCT_FLD(name, value)	name: value
#endif /* HAVE_C99_INITIALIZERS */
#else
#define STRUCT_FLD(name, value)	value
#endif

/* Don't use a static const variable here, as some C++ compilers (notably
HPUX aCC: HP ANSI C++ B3910B A.03.65) can't handle it. */
#define END_OF_ST_FIELD_INFO \
	{STRUCT_FLD(field_name,		NULL), \
	 STRUCT_FLD(field_length,	0), \
	 STRUCT_FLD(field_type,		MYSQL_TYPE_NULL), \
	 STRUCT_FLD(value,		0), \
	 STRUCT_FLD(field_flags,	0), \
	 STRUCT_FLD(old_name,		""), \
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)}

/** Fields on INFORMATION_SCHEMA.SYS_SEMAMPHORE_WAITS table */
#define SYS_SEMAPHORE_WAITS_THREAD_ID	0
#define SYS_SEMAPHORE_WAITS_OBJECT_NAME 1
#define SYS_SEMAPHORE_WAITS_FILE	2
#define SYS_SEMAPHORE_WAITS_LINE	3
#define SYS_SEMAPHORE_WAITS_WAIT_TIME	4
#define SYS_SEMAPHORE_WAITS_WAIT_OBJECT	5
#define SYS_SEMAPHORE_WAITS_WAIT_TYPE	6
#define SYS_SEMAPHORE_WAITS_HOLDER_THREAD_ID 7
#define SYS_SEMAPHORE_WAITS_HOLDER_FILE 8
#define SYS_SEMAPHORE_WAITS_HOLDER_LINE 9
#define SYS_SEMAPHORE_WAITS_CREATED_FILE 10
#define SYS_SEMAPHORE_WAITS_CREATED_LINE 11
#define SYS_SEMAPHORE_WAITS_WRITER_THREAD 12
#define SYS_SEMAPHORE_WAITS_RESERVATION_MODE 13
#define SYS_SEMAPHORE_WAITS_READERS	14
#define SYS_SEMAPHORE_WAITS_WAITERS_FLAG 15
#define SYS_SEMAPHORE_WAITS_LOCK_WORD	16
#define SYS_SEMAPHORE_WAITS_LAST_READER_FILE 17
#define SYS_SEMAPHORE_WAITS_LAST_READER_LINE 18
#define SYS_SEMAPHORE_WAITS_LAST_WRITER_FILE 19
#define SYS_SEMAPHORE_WAITS_LAST_WRITER_LINE 20
#define SYS_SEMAPHORE_WAITS_OS_WAIT_COUNT 21

/*******************************************************************//**
Auxiliary function to store ulint value in MYSQL_TYPE_LONGLONG field.
If the value is ULINT_UNDEFINED then the field it set to NULL.
@return	0 on success */
int
field_store_ulint(
/*==============*/
	Field*	field,	/*!< in/out: target field for storage */
	ulint	n);	/*!< in: value to store */

/*******************************************************************//**
Auxiliary function to store char* value in MYSQL_TYPE_STRING field.
@return	0 on success */
int
field_store_string(
/*===============*/
	Field*		field,	/*!< in/out: target field for storage */
	const char*	str);	/*!< in: NUL-terminated utf-8 string,
				or NULL */
#endif /* i_s_h */
