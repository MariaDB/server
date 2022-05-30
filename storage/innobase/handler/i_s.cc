/*****************************************************************************

Copyright (c) 2007, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2022, MariaDB Corporation.

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
@file handler/i_s.cc
InnoDB INFORMATION SCHEMA tables interface to MySQL.

Created July 18, 2007 Vasil Dimov
*******************************************************/

#include "univ.i"
#include <mysql_version.h>
#include <field.h>

#include <sql_acl.h>
#include <sql_show.h>
#include <sql_time.h>

#include "i_s.h"
#include "btr0pcur.h"
#include "btr0types.h"
#include "dict0dict.h"
#include "dict0load.h"
#include "buf0buddy.h"
#include "buf0buf.h"
#include "ibuf0ibuf.h"
#include "dict0mem.h"
#include "dict0types.h"
#include "srv0start.h"
#include "trx0i_s.h"
#include "trx0trx.h"
#include "srv0mon.h"
#include "fut0fut.h"
#include "pars0pars.h"
#include "fts0types.h"
#include "fts0opt.h"
#include "fts0priv.h"
#include "btr0btr.h"
#include "page0zip.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "dict0crea.h"
#include "fts0vlc.h"
#include "scope.h"

/** The latest successfully looked up innodb_fts_aux_table */
table_id_t innodb_ft_aux_table_id;

/** structure associates a name string with a file page type and/or buffer
page state. */
struct buf_page_desc_t{
	const char*	type_str;	/*!< String explain the page
					type/state */
	ulint		type_value;	/*!< Page type or page state */
};

/** We also define I_S_PAGE_TYPE_INDEX as the Index Page's position
in i_s_page_type[] array */
#define I_S_PAGE_TYPE_INDEX		1

/** Any unassigned FIL_PAGE_TYPE will be treated as unknown. */
#define	I_S_PAGE_TYPE_UNKNOWN		FIL_PAGE_TYPE_UNKNOWN

/** R-tree index page */
#define	I_S_PAGE_TYPE_RTREE		(FIL_PAGE_TYPE_LAST + 1)

/** Change buffer B-tree page */
#define	I_S_PAGE_TYPE_IBUF		(FIL_PAGE_TYPE_LAST + 2)

#define I_S_PAGE_TYPE_LAST		I_S_PAGE_TYPE_IBUF

#define I_S_PAGE_TYPE_BITS		4

/** Name string for File Page Types */
static buf_page_desc_t	i_s_page_type[] = {
	{"ALLOCATED", FIL_PAGE_TYPE_ALLOCATED},
	{"INDEX", FIL_PAGE_INDEX},
	{"UNDO_LOG", FIL_PAGE_UNDO_LOG},
	{"INODE", FIL_PAGE_INODE},
	{"IBUF_FREE_LIST", FIL_PAGE_IBUF_FREE_LIST},
	{"IBUF_BITMAP", FIL_PAGE_IBUF_BITMAP},
	{"SYSTEM", FIL_PAGE_TYPE_SYS},
	{"TRX_SYSTEM", FIL_PAGE_TYPE_TRX_SYS},
	{"FILE_SPACE_HEADER", FIL_PAGE_TYPE_FSP_HDR},
	{"EXTENT_DESCRIPTOR", FIL_PAGE_TYPE_XDES},
	{"BLOB", FIL_PAGE_TYPE_BLOB},
	{"COMPRESSED_BLOB", FIL_PAGE_TYPE_ZBLOB},
	{"COMPRESSED_BLOB2", FIL_PAGE_TYPE_ZBLOB2},
	{"UNKNOWN", I_S_PAGE_TYPE_UNKNOWN},
	{"RTREE_INDEX", I_S_PAGE_TYPE_RTREE},
	{"IBUF_INDEX", I_S_PAGE_TYPE_IBUF},
	{"PAGE COMPRESSED", FIL_PAGE_PAGE_COMPRESSED},
	{"PAGE COMPRESSED AND ENCRYPTED", FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED},
};

/** This structure defines information we will fetch from pages
currently cached in the buffer pool. It will be used to populate
table INFORMATION_SCHEMA.INNODB_BUFFER_PAGE */
struct buf_page_info_t{
	ulint		block_id;	/*!< Buffer Pool block ID */
	/** page identifier */
	page_id_t	id;
	uint32_t	access_time;	/*!< Time of first access */
	uint32_t	state;		/*!< buf_page_t::state() */
#ifdef BTR_CUR_HASH_ADAPT
	unsigned	hashed:1;	/*!< Whether hash index has been
					built on this page */
#endif /* BTR_CUR_HASH_ADAPT */
	unsigned	is_old:1;	/*!< TRUE if the block is in the old
					blocks in buf_pool.LRU_old */
	unsigned	freed_page_clock:31; /*!< the value of
					buf_pool.freed_page_clock */
	unsigned	zip_ssize:PAGE_ZIP_SSIZE_BITS;
					/*!< Compressed page size */
	unsigned	compressed_only:1; /*!< ROW_FORMAT=COMPRESSED only */
	unsigned	page_type:I_S_PAGE_TYPE_BITS;	/*!< Page type */
	unsigned	num_recs:UNIV_PAGE_SIZE_SHIFT_MAX-2;
					/*!< Number of records on Page */
	unsigned	data_size:UNIV_PAGE_SIZE_SHIFT_MAX;
					/*!< Sum of the sizes of the records */
	lsn_t		newest_mod;	/*!< Log sequence number of
					the youngest modification */
	lsn_t		oldest_mod;	/*!< Log sequence number of
					the oldest modification */
	index_id_t	index_id;	/*!< Index ID if a index page */
};

/*
Use the following types mapping:

C type	ST_FIELD_INFO::field_type
---------------------------------
long			MYSQL_TYPE_LONGLONG
(field_length=MY_INT64_NUM_DECIMAL_DIGITS)

long unsigned		MYSQL_TYPE_LONGLONG
(field_length=MY_INT64_NUM_DECIMAL_DIGITS, field_flags=MY_I_S_UNSIGNED)

char*			MYSQL_TYPE_STRING
(field_length=n)

float			MYSQL_TYPE_FLOAT
(field_length=0 is ignored)

void*			MYSQL_TYPE_LONGLONG
(field_length=MY_INT64_NUM_DECIMAL_DIGITS, field_flags=MY_I_S_UNSIGNED)

boolean (if else)	MYSQL_TYPE_LONG
(field_length=1)

time_t			MYSQL_TYPE_DATETIME
(field_length=0 ignored)
---------------------------------
*/

/*******************************************************************//**
Common function to fill any of the dynamic tables:
INFORMATION_SCHEMA.innodb_trx
INFORMATION_SCHEMA.innodb_locks
INFORMATION_SCHEMA.innodb_lock_waits
@return 0 on success */
static
int
trx_i_s_common_fill_table(
/*======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		);	/*!< in: condition (not used) */

/*******************************************************************//**
Unbind a dynamic INFORMATION_SCHEMA table.
@return 0 on success */
static
int
i_s_common_deinit(
/*==============*/
	void*	p);	/*!< in/out: table schema object */
/*******************************************************************//**
Auxiliary function to store time_t value in MYSQL_TYPE_DATETIME
field.
@return 0 on success */
static
int
field_store_time_t(
/*===============*/
	Field*	field,	/*!< in/out: target field for storage */
	time_t	time)	/*!< in: value to store */
{
	MYSQL_TIME	my_time;
	struct tm	tm_time;

	if (time) {
#if 0
		/* use this if you are sure that `variables' and `time_zone'
		are always initialized */
		thd->variables.time_zone->gmt_sec_to_TIME(
			&my_time, (my_time_t) time);
#else
		localtime_r(&time, &tm_time);
		localtime_to_TIME(&my_time, &tm_time);
		my_time.time_type = MYSQL_TIMESTAMP_DATETIME;
#endif
	} else {
		memset(&my_time, 0, sizeof(my_time));
	}

	/* JAN: TODO: MySQL 5.7
	return(field->store_time(&my_time, MYSQL_TIMESTAMP_DATETIME));
	*/
	return(field->store_time(&my_time));
}

/*******************************************************************//**
Auxiliary function to store char* value in MYSQL_TYPE_STRING field.
@return 0 on success */
static
int
field_store_string(
/*===============*/
	Field*		field,	/*!< in/out: target field for storage */
	const char*	str)	/*!< in: NUL-terminated utf-8 string,
				or NULL */
{
	if (!str) {
		field->set_null();
		return 0;
	}

	field->set_notnull();
	return field->store(str, uint(strlen(str)), system_charset_info);
}

#ifdef BTR_CUR_HASH_ADAPT
# define I_S_AHI 1 /* Include the IS_HASHED column */
#else
# define I_S_AHI 0 /* Omit the IS_HASHED column */
#endif

static const LEX_CSTRING isolation_level_values[] =
{
	{ STRING_WITH_LEN("READ UNCOMMITTED") },
	{ STRING_WITH_LEN("READ COMMITTED") },
	{ STRING_WITH_LEN("REPEATABLE READ") },
	{ STRING_WITH_LEN("SERIALIZABLE") }
};

static TypelibBuffer<4> isolation_level_values_typelib(isolation_level_values);

namespace Show {

/* Fields of the dynamic table INFORMATION_SCHEMA.innodb_trx */
static ST_FIELD_INFO innodb_trx_fields_info[]=
{
#define IDX_TRX_ID		0
  Column("trx_id", ULonglong(), NOT_NULL),

#define IDX_TRX_STATE		1
  Column("trx_state", Varchar(13), NOT_NULL),

#define IDX_TRX_STARTED		2
  Column("trx_started", Datetime(0), NOT_NULL),

#define IDX_TRX_REQUESTED_LOCK_ID	3
  Column("trx_requested_lock_id",
         Varchar(TRX_I_S_LOCK_ID_MAX_LEN + 1), NULLABLE),

#define IDX_TRX_WAIT_STARTED	4
 Column("trx_wait_started", Datetime(0), NULLABLE),

#define IDX_TRX_WEIGHT		5
 Column("trx_weight", ULonglong(), NOT_NULL),

#define IDX_TRX_MYSQL_THREAD_ID	6
  Column("trx_mysql_thread_id", ULonglong(), NOT_NULL),

#define IDX_TRX_QUERY		7
  Column("trx_query", Varchar(TRX_I_S_TRX_QUERY_MAX_LEN), NULLABLE),

#define IDX_TRX_OPERATION_STATE	8
  Column("trx_operation_state", Varchar(64), NULLABLE),

#define IDX_TRX_TABLES_IN_USE	9
  Column("trx_tables_in_use", ULonglong(), NOT_NULL),

#define IDX_TRX_TABLES_LOCKED	10
  Column("trx_tables_locked", ULonglong(), NOT_NULL),

#define IDX_TRX_LOCK_STRUCTS	11
  Column("trx_lock_structs", ULonglong(), NOT_NULL),

#define IDX_TRX_LOCK_MEMORY_BYTES	12
  Column("trx_lock_memory_bytes", ULonglong(), NOT_NULL),

#define IDX_TRX_ROWS_LOCKED	13
  Column("trx_rows_locked", ULonglong(), NOT_NULL),

#define IDX_TRX_ROWS_MODIFIED	14
  Column("trx_rows_modified", ULonglong(), NOT_NULL),

#define IDX_TRX_CONNCURRENCY_TICKETS	15
  Column("trx_concurrency_tickets", ULonglong(), NOT_NULL),

#define IDX_TRX_ISOLATION_LEVEL	16
  Column("trx_isolation_level",
         Enum(&isolation_level_values_typelib), NOT_NULL),

#define IDX_TRX_UNIQUE_CHECKS	17
  Column("trx_unique_checks", SLong(1), NOT_NULL),

#define IDX_TRX_FOREIGN_KEY_CHECKS	18
  Column("trx_foreign_key_checks", SLong(1), NOT_NULL),

#define IDX_TRX_LAST_FOREIGN_KEY_ERROR	19
  Column("trx_last_foreign_key_error",
         Varchar(TRX_I_S_TRX_FK_ERROR_MAX_LEN),NULLABLE),

#define IDX_TRX_READ_ONLY		20
  Column("trx_is_read_only", SLong(1), NOT_NULL),

#define IDX_TRX_AUTOCOMMIT_NON_LOCKING	21
  Column("trx_autocommit_non_locking", SLong(1), NOT_NULL),

  CEnd()
};

} // namespace Show

/*******************************************************************//**
Read data from cache buffer and fill the INFORMATION_SCHEMA.innodb_trx
table with it.
@return 0 on success */
static
int
fill_innodb_trx_from_cache(
/*=======================*/
	trx_i_s_cache_t*	cache,	/*!< in: cache to read from */
	THD*			thd,	/*!< in: used to call
					schema_table_store_record() */
	TABLE*			table)	/*!< in/out: fill this table */
{
	Field**	fields;
	ulint	rows_num;
	char	lock_id[TRX_I_S_LOCK_ID_MAX_LEN + 1];
	ulint	i;

	DBUG_ENTER("fill_innodb_trx_from_cache");

	fields = table->field;

	rows_num = trx_i_s_cache_get_rows_used(cache,
					       I_S_INNODB_TRX);

	for (i = 0; i < rows_num; i++) {

		i_s_trx_row_t*	row;

		row = (i_s_trx_row_t*)
			trx_i_s_cache_get_nth_row(
				cache, I_S_INNODB_TRX, i);

		/* trx_id */
		OK(fields[IDX_TRX_ID]->store(row->trx_id, true));

		/* trx_state */
		OK(field_store_string(fields[IDX_TRX_STATE],
				      row->trx_state));

		/* trx_started */
		OK(field_store_time_t(fields[IDX_TRX_STARTED],
				      (time_t) row->trx_started));

		/* trx_requested_lock_id */
		/* trx_wait_started */
		if (row->trx_wait_started != 0) {

			OK(field_store_string(
				   fields[IDX_TRX_REQUESTED_LOCK_ID],
				   trx_i_s_create_lock_id(
					   row->requested_lock_row,
					   lock_id, sizeof(lock_id))));
			/* field_store_string() sets it no notnull */

			OK(field_store_time_t(
				   fields[IDX_TRX_WAIT_STARTED],
				   (time_t) row->trx_wait_started));
			fields[IDX_TRX_WAIT_STARTED]->set_notnull();
		} else {

			fields[IDX_TRX_REQUESTED_LOCK_ID]->set_null();
			fields[IDX_TRX_WAIT_STARTED]->set_null();
		}

		/* trx_weight */
		OK(fields[IDX_TRX_WEIGHT]->store(row->trx_weight, true));

		/* trx_mysql_thread_id */
		OK(fields[IDX_TRX_MYSQL_THREAD_ID]->store(
			   row->trx_mysql_thread_id, true));

		/* trx_query */
		if (row->trx_query) {
			/* store will do appropriate character set
			conversion check */
			fields[IDX_TRX_QUERY]->store(
				row->trx_query,
				static_cast<uint>(strlen(row->trx_query)),
				row->trx_query_cs);
			fields[IDX_TRX_QUERY]->set_notnull();
		} else {
			fields[IDX_TRX_QUERY]->set_null();
		}

		/* trx_operation_state */
		OK(field_store_string(fields[IDX_TRX_OPERATION_STATE],
				      row->trx_operation_state));

		/* trx_tables_in_use */
		OK(fields[IDX_TRX_TABLES_IN_USE]->store(
			   row->trx_tables_in_use, true));

		/* trx_tables_locked */
		OK(fields[IDX_TRX_TABLES_LOCKED]->store(
			   row->trx_tables_locked, true));

		/* trx_lock_structs */
		OK(fields[IDX_TRX_LOCK_STRUCTS]->store(
			   row->trx_lock_structs, true));

		/* trx_lock_memory_bytes */
		OK(fields[IDX_TRX_LOCK_MEMORY_BYTES]->store(
			   row->trx_lock_memory_bytes, true));

		/* trx_rows_locked */
		OK(fields[IDX_TRX_ROWS_LOCKED]->store(
			   row->trx_rows_locked, true));

		/* trx_rows_modified */
		OK(fields[IDX_TRX_ROWS_MODIFIED]->store(
			   row->trx_rows_modified, true));

		/* trx_concurrency_tickets */
		OK(fields[IDX_TRX_CONNCURRENCY_TICKETS]->store(0, true));

		/* trx_isolation_level */
		OK(fields[IDX_TRX_ISOLATION_LEVEL]->store(
			   1 + row->trx_isolation_level, true));

		/* trx_unique_checks */
		OK(fields[IDX_TRX_UNIQUE_CHECKS]->store(
			   row->trx_unique_checks, true));

		/* trx_foreign_key_checks */
		OK(fields[IDX_TRX_FOREIGN_KEY_CHECKS]->store(
			   row->trx_foreign_key_checks, true));

		/* trx_last_foreign_key_error */
		OK(field_store_string(fields[IDX_TRX_LAST_FOREIGN_KEY_ERROR],
				      row->trx_foreign_key_error));

		/* trx_is_read_only*/
		OK(fields[IDX_TRX_READ_ONLY]->store(
			   row->trx_is_read_only, true));

		/* trx_is_autocommit_non_locking */
		OK(fields[IDX_TRX_AUTOCOMMIT_NON_LOCKING]->store(
			   row->trx_is_autocommit_non_locking, true));

		OK(schema_table_store_record(thd, table));
	}

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_trx
@return 0 on success */
static
int
innodb_trx_init(
/*============*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_trx_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_trx_fields_info;
	schema->fill_table = trx_i_s_common_fill_table;

	DBUG_RETURN(0);
}

static struct st_mysql_information_schema	i_s_info =
{
	MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

struct st_maria_plugin	i_s_innodb_trx =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_TRX",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB transactions",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_trx_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

static const LEX_CSTRING lock_mode_values[] =
{
	{ STRING_WITH_LEN("S") },
	{ STRING_WITH_LEN("S,GAP") },
	{ STRING_WITH_LEN("X") },
	{ STRING_WITH_LEN("X,GAP") },
	{ STRING_WITH_LEN("IS") },
	{ STRING_WITH_LEN("IS,GAP") },
	{ STRING_WITH_LEN("IX") },
	{ STRING_WITH_LEN("IX,GAP") },
	{ STRING_WITH_LEN("AUTO_INC") }
};

static TypelibBuffer<9> lock_mode_values_typelib(lock_mode_values);

static const LEX_CSTRING lock_type_values[] =
{
	{ STRING_WITH_LEN("RECORD") },
	{ STRING_WITH_LEN("TABLE") }
};

static TypelibBuffer<2> lock_type_values_typelib(lock_type_values);

namespace Show {
/* Fields of the dynamic table INFORMATION_SCHEMA.innodb_locks */
static ST_FIELD_INFO innodb_locks_fields_info[]=
{
#define IDX_LOCK_ID		0
  Column("lock_id",     Varchar(TRX_I_S_LOCK_ID_MAX_LEN + 1),  NOT_NULL),

#define IDX_LOCK_TRX_ID		1
  Column("lock_trx_id", ULonglong(), NOT_NULL),

#define IDX_LOCK_MODE		2
  Column("lock_mode",   Enum(&lock_mode_values_typelib), NOT_NULL),

#define IDX_LOCK_TYPE		3
  Column("lock_type",   Enum(&lock_type_values_typelib), NOT_NULL),

#define IDX_LOCK_TABLE		4
  Column("lock_table",  Varchar(1024), NOT_NULL),

#define IDX_LOCK_INDEX		5
  Column("lock_index",  Varchar(1024), NULLABLE),

#define IDX_LOCK_SPACE		6
  Column("lock_space",  ULong(),   NULLABLE),

#define IDX_LOCK_PAGE		7
  Column("lock_page",   ULong(),   NULLABLE),

#define IDX_LOCK_REC		8
  Column("lock_rec",    ULong(),   NULLABLE),

#define IDX_LOCK_DATA		9
  Column("lock_data",   Varchar(TRX_I_S_LOCK_DATA_MAX_LEN), NULLABLE),
  CEnd()
};
} // namespace Show

/*******************************************************************//**
Read data from cache buffer and fill the INFORMATION_SCHEMA.innodb_locks
table with it.
@return 0 on success */
static
int
fill_innodb_locks_from_cache(
/*=========================*/
	trx_i_s_cache_t*	cache,	/*!< in: cache to read from */
	THD*			thd,	/*!< in: MySQL client connection */
	TABLE*			table)	/*!< in/out: fill this table */
{
	Field**	fields;
	ulint	rows_num;
	char	lock_id[TRX_I_S_LOCK_ID_MAX_LEN + 1];
	ulint	i;

	DBUG_ENTER("fill_innodb_locks_from_cache");

	fields = table->field;

	rows_num = trx_i_s_cache_get_rows_used(cache,
					       I_S_INNODB_LOCKS);

	for (i = 0; i < rows_num; i++) {

		i_s_locks_row_t*	row;
		char			buf[MAX_FULL_NAME_LEN + 1];
		const char*		bufend;

		row = (i_s_locks_row_t*)
			trx_i_s_cache_get_nth_row(
				cache, I_S_INNODB_LOCKS, i);

		/* lock_id */
		trx_i_s_create_lock_id(row, lock_id, sizeof(lock_id));
		OK(field_store_string(fields[IDX_LOCK_ID],
				      lock_id));

		/* lock_trx_id */
		OK(fields[IDX_LOCK_TRX_ID]->store(row->lock_trx_id, true));

		/* lock_mode */
		OK(fields[IDX_LOCK_MODE]->store(row->lock_mode, true));

		/* lock_type */
		OK(fields[IDX_LOCK_TYPE]->store(
			   row->lock_index ? 1 : 2, true));

		/* lock_table */
		bufend = innobase_convert_name(buf, sizeof(buf),
					       row->lock_table,
					       strlen(row->lock_table),
					       thd);
		OK(fields[IDX_LOCK_TABLE]->store(
			buf, uint(bufend - buf), system_charset_info));

		if (row->lock_index) {
			/* record lock */
			OK(field_store_string(fields[IDX_LOCK_INDEX],
					      row->lock_index));
			OK(fields[IDX_LOCK_SPACE]->store(
				   row->lock_page.space(), true));
			fields[IDX_LOCK_SPACE]->set_notnull();
			OK(fields[IDX_LOCK_PAGE]->store(
				   row->lock_page.page_no(), true));
			fields[IDX_LOCK_PAGE]->set_notnull();
			OK(fields[IDX_LOCK_REC]->store(
				   row->lock_rec, true));
			fields[IDX_LOCK_REC]->set_notnull();
			OK(field_store_string(fields[IDX_LOCK_DATA],
					      row->lock_data));
		} else {
			fields[IDX_LOCK_INDEX]->set_null();
			fields[IDX_LOCK_SPACE]->set_null();
			fields[IDX_LOCK_REC]->set_null();
			fields[IDX_LOCK_DATA]->set_null();
		}

		OK(schema_table_store_record(thd, table));
	}

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_locks
@return 0 on success */
static
int
innodb_locks_init(
/*==============*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_locks_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_locks_fields_info;
	schema->fill_table = trx_i_s_common_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_locks =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_LOCKS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB conflicting locks",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_locks_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};


namespace Show {
/* Fields of the dynamic table INFORMATION_SCHEMA.innodb_lock_waits */
static ST_FIELD_INFO innodb_lock_waits_fields_info[]=
{
#define IDX_REQUESTING_TRX_ID	0
  Column("requesting_trx_id", ULonglong(), NOT_NULL),

#define IDX_REQUESTED_LOCK_ID	1
  Column("requested_lock_id", Varchar(TRX_I_S_LOCK_ID_MAX_LEN + 1), NOT_NULL),

#define IDX_BLOCKING_TRX_ID	2
  Column("blocking_trx_id",   ULonglong(), NOT_NULL),

#define IDX_BLOCKING_LOCK_ID	3
  Column("blocking_lock_id",  Varchar(TRX_I_S_LOCK_ID_MAX_LEN + 1), NOT_NULL),
  CEnd()
};
} // namespace Show

/*******************************************************************//**
Read data from cache buffer and fill the
INFORMATION_SCHEMA.innodb_lock_waits table with it.
@return 0 on success */
static
int
fill_innodb_lock_waits_from_cache(
/*==============================*/
	trx_i_s_cache_t*	cache,	/*!< in: cache to read from */
	THD*			thd,	/*!< in: used to call
					schema_table_store_record() */
	TABLE*			table)	/*!< in/out: fill this table */
{
	Field**	fields;
	ulint	rows_num;
	char	requested_lock_id[TRX_I_S_LOCK_ID_MAX_LEN + 1];
	char	blocking_lock_id[TRX_I_S_LOCK_ID_MAX_LEN + 1];
	ulint	i;

	DBUG_ENTER("fill_innodb_lock_waits_from_cache");

	fields = table->field;

	rows_num = trx_i_s_cache_get_rows_used(cache,
					       I_S_INNODB_LOCK_WAITS);

	for (i = 0; i < rows_num; i++) {

		i_s_lock_waits_row_t*	row;

		row = (i_s_lock_waits_row_t*)
			trx_i_s_cache_get_nth_row(
				cache, I_S_INNODB_LOCK_WAITS, i);

		/* requesting_trx_id */
		OK(fields[IDX_REQUESTING_TRX_ID]->store(
				      row->requested_lock_row->lock_trx_id, true));

		/* requested_lock_id */
		OK(field_store_string(
			   fields[IDX_REQUESTED_LOCK_ID],
			   trx_i_s_create_lock_id(
				   row->requested_lock_row,
				   requested_lock_id,
				   sizeof(requested_lock_id))));

		/* blocking_trx_id */
		OK(fields[IDX_BLOCKING_TRX_ID]->store(
				      row->blocking_lock_row->lock_trx_id, true));

		/* blocking_lock_id */
		OK(field_store_string(
			   fields[IDX_BLOCKING_LOCK_ID],
			   trx_i_s_create_lock_id(
				   row->blocking_lock_row,
				   blocking_lock_id,
				   sizeof(blocking_lock_id))));

		OK(schema_table_store_record(thd, table));
	}

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_lock_waits
@return 0 on success */
static
int
innodb_lock_waits_init(
/*===================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_lock_waits_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_lock_waits_fields_info;
	schema->fill_table = trx_i_s_common_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_lock_waits =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_LOCK_WAITS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB which lock is blocking which",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_lock_waits_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

/*******************************************************************//**
Common function to fill any of the dynamic tables:
INFORMATION_SCHEMA.innodb_trx
INFORMATION_SCHEMA.innodb_locks
INFORMATION_SCHEMA.innodb_lock_waits
@return 0 on success */
static
int
trx_i_s_common_fill_table(
/*======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	LEX_CSTRING		table_name;
	int			ret;
	trx_i_s_cache_t*	cache;

	DBUG_ENTER("trx_i_s_common_fill_table");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	/* minimize the number of places where global variables are
	referenced */
	cache = trx_i_s_cache;

	/* which table we have to fill? */
	table_name = tables->schema_table_name;
	/* or table_name = tables->schema_table->table_name; */

	RETURN_IF_INNODB_NOT_STARTED(table_name.str);

	/* update the cache */
	trx_i_s_cache_start_write(cache);
	trx_i_s_possibly_fetch_data_into_cache(cache);
	trx_i_s_cache_end_write(cache);

	if (trx_i_s_cache_is_truncated(cache)) {

		ib::warn() << "Data in " << table_name.str << " truncated due to"
			" memory limit of " << TRX_I_S_MEM_LIMIT << " bytes";
	}

	ret = 0;

	trx_i_s_cache_start_read(cache);

	if (innobase_strcasecmp(table_name.str, "innodb_trx") == 0) {

		if (fill_innodb_trx_from_cache(
			cache, thd, tables->table) != 0) {

			ret = 1;
		}

	} else if (innobase_strcasecmp(table_name.str, "innodb_locks") == 0) {

		if (fill_innodb_locks_from_cache(
			cache, thd, tables->table) != 0) {

			ret = 1;
		}

	} else if (innobase_strcasecmp(table_name.str, "innodb_lock_waits") == 0) {

		if (fill_innodb_lock_waits_from_cache(
			cache, thd, tables->table) != 0) {

			ret = 1;
		}

	} else {
		ib::error() << "trx_i_s_common_fill_table() was"
			" called to fill unknown table: " << table_name.str << "."
			" This function only knows how to fill"
			" innodb_trx, innodb_locks and"
			" innodb_lock_waits tables.";

		ret = 1;
	}

	trx_i_s_cache_end_read(cache);

#if 0
	DBUG_RETURN(ret);
#else
	/* if this function returns something else than 0 then a
	deadlock occurs between the mysqld server and mysql client,
	see http://bugs.mysql.com/29900 ; when that bug is resolved
	we can enable the DBUG_RETURN(ret) above */
	ret++;  // silence a gcc46 warning
	DBUG_RETURN(0);
#endif
}

namespace Show {
/* Fields of the dynamic table information_schema.innodb_cmp. */
static ST_FIELD_INFO i_s_cmp_fields_info[] =
{
  Column("page_size",      SLong(5),NOT_NULL, "Compressed Page Size"),
  Column("compress_ops",   SLong(), NOT_NULL, "Total Number of Compressions"),
  Column("compress_ops_ok",SLong(), NOT_NULL, "Total Number of "
                                              "Successful Compressions"),
  Column("compress_time",  SLong(), NOT_NULL, "Total Duration of "
                                              "Compressions, in Seconds"),
  Column("uncompress_ops", SLong(), NOT_NULL, "Total Number of Decompressions"),
  Column("uncompress_time",SLong(), NOT_NULL, "Total Duration of "
                                              "Decompressions, in Seconds"),
  CEnd(),
};
} // namespace Show


/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmp or
innodb_cmp_reset.
@return 0 on success, 1 on failure */
static
int
i_s_cmp_fill_low(
/*=============*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		,	/*!< in: condition (ignored) */
	ibool		reset)	/*!< in: TRUE=reset cumulated counts */
{
	TABLE*	table	= (TABLE*) tables->table;
	int	status	= 0;

	DBUG_ENTER("i_s_cmp_fill_low");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	for (uint i = 0; i < PAGE_ZIP_SSIZE_MAX; i++) {
		page_zip_stat_t*	zip_stat = &page_zip_stat[i];

		table->field[0]->store(UNIV_ZIP_SIZE_MIN << i);

		/* The cumulated counts are not protected by any
		mutex.  Thus, some operation in page0zip.cc could
		increment a counter between the time we read it and
		clear it.  We could introduce mutex protection, but it
		could cause a measureable performance hit in
		page0zip.cc. */
		table->field[1]->store(zip_stat->compressed, true);
		table->field[2]->store(zip_stat->compressed_ok, true);
		table->field[3]->store(zip_stat->compressed_usec / 1000000,
				       true);
		table->field[4]->store(zip_stat->decompressed, true);
		table->field[5]->store(zip_stat->decompressed_usec / 1000000,
				       true);

		if (reset) {
			new (zip_stat) page_zip_stat_t();
		}

		if (schema_table_store_record(thd, table)) {
			status = 1;
			break;
		}
	}

	DBUG_RETURN(status);
}

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmp.
@return 0 on success, 1 on failure */
static
int
i_s_cmp_fill(
/*=========*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		cond)	/*!< in: condition (ignored) */
{
	return(i_s_cmp_fill_low(thd, tables, cond, FALSE));
}

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmp_reset.
@return 0 on success, 1 on failure */
static
int
i_s_cmp_reset_fill(
/*===============*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		cond)	/*!< in: condition (ignored) */
{
	return(i_s_cmp_fill_low(thd, tables, cond, TRUE));
}

/*******************************************************************//**
Bind the dynamic table information_schema.innodb_cmp.
@return 0 on success */
static
int
i_s_cmp_init(
/*=========*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_cmp_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_cmp_fields_info;
	schema->fill_table = i_s_cmp_fill;

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table information_schema.innodb_cmp_reset.
@return 0 on success */
static
int
i_s_cmp_reset_init(
/*===============*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_cmp_reset_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_cmp_fields_info;
	schema->fill_table = i_s_cmp_reset_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_cmp =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_CMP",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"Statistics for the InnoDB compression",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_cmp_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

struct st_maria_plugin	i_s_innodb_cmp_reset =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_CMP_RESET",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"Statistics for the InnoDB compression;"
		   " reset cumulated counts",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_cmp_reset_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};


namespace Show {
/* Fields of the dynamic tables
information_schema.innodb_cmp_per_index and
information_schema.innodb_cmp_per_index_reset. */
static ST_FIELD_INFO i_s_cmp_per_index_fields_info[]=
{
#define IDX_DATABASE_NAME	0
  Column("database_name",   Varchar(NAME_CHAR_LEN), NOT_NULL),

#define IDX_TABLE_NAME		1 /* FIXME: this is in my_charset_filename! */
  Column("table_name",      Varchar(NAME_CHAR_LEN), NOT_NULL),

#define IDX_INDEX_NAME		2
  Column("index_name",      Varchar(NAME_CHAR_LEN), NOT_NULL),

#define IDX_COMPRESS_OPS	3
  Column("compress_ops",    SLong(),      NOT_NULL),

#define IDX_COMPRESS_OPS_OK	4
  Column("compress_ops_ok", SLong(),      NOT_NULL),

#define IDX_COMPRESS_TIME	5
  Column("compress_time",   SLong(),      NOT_NULL),

#define IDX_UNCOMPRESS_OPS	6
  Column("uncompress_ops",  SLong(),      NOT_NULL),

#define IDX_UNCOMPRESS_TIME	7
  Column("uncompress_time", SLong(),      NOT_NULL),

  CEnd()
};

} // namespace Show

/*******************************************************************//**
Fill the dynamic table
information_schema.innodb_cmp_per_index or
information_schema.innodb_cmp_per_index_reset.
@return 0 on success, 1 on failure */
static
int
i_s_cmp_per_index_fill_low(
/*=======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		,	/*!< in: condition (ignored) */
	ibool		reset)	/*!< in: TRUE=reset cumulated counts */
{
	TABLE*	table = tables->table;
	Field**	fields = table->field;
	int	status = 0;

	DBUG_ENTER("i_s_cmp_per_index_fill_low");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* Create a snapshot of the stats so we do not bump into lock
	order violations with dict_sys.latch below. */
	mysql_mutex_lock(&page_zip_stat_per_index_mutex);
	page_zip_stat_per_index_t		snap (page_zip_stat_per_index);
	mysql_mutex_unlock(&page_zip_stat_per_index_mutex);

	dict_sys.freeze(SRW_LOCK_CALL);

	page_zip_stat_per_index_t::iterator	iter;
	ulint					i;

	for (iter = snap.begin(), i = 0; iter != snap.end(); iter++, i++) {

		dict_index_t*	index = dict_index_find_on_id_low(iter->first);

		if (index != NULL) {
			char	db_utf8[MAX_DB_UTF8_LEN];
			char	table_utf8[MAX_TABLE_UTF8_LEN];

			dict_fs2utf8(index->table->name.m_name,
				     db_utf8, sizeof(db_utf8),
				     table_utf8, sizeof(table_utf8));

			status = field_store_string(fields[IDX_DATABASE_NAME],
						    db_utf8)
				|| field_store_string(fields[IDX_TABLE_NAME],
						      table_utf8)
				|| field_store_string(fields[IDX_INDEX_NAME],
						      index->name);
		} else {
			/* index not found */
			char name[MY_INT64_NUM_DECIMAL_DIGITS
				  + sizeof "index_id: "];
			fields[IDX_DATABASE_NAME]->set_null();
			fields[IDX_TABLE_NAME]->set_null();
			fields[IDX_INDEX_NAME]->set_notnull();
			status = fields[IDX_INDEX_NAME]->store(
				name,
				uint(snprintf(name, sizeof name,
					      "index_id: " IB_ID_FMT,
					      iter->first)),
				system_charset_info);
		}

		if (status
		    || fields[IDX_COMPRESS_OPS]->store(
			    iter->second.compressed, true)
		    || fields[IDX_COMPRESS_OPS_OK]->store(
			    iter->second.compressed_ok, true)
		    || fields[IDX_COMPRESS_TIME]->store(
			    iter->second.compressed_usec / 1000000, true)
		    || fields[IDX_UNCOMPRESS_OPS]->store(
			    iter->second.decompressed, true)
		    || fields[IDX_UNCOMPRESS_TIME]->store(
			    iter->second.decompressed_usec / 1000000, true)
		    || schema_table_store_record(thd, table)) {
			status = 1;
			break;
		}
		/* Release and reacquire the dict_sys.latch to allow other
		threads to proceed. This could eventually result in the
		contents of INFORMATION_SCHEMA.innodb_cmp_per_index being
		inconsistent, but it is an acceptable compromise. */
		if (i == 1000) {
			dict_sys.unfreeze();
			i = 0;
			dict_sys.freeze(SRW_LOCK_CALL);
		}
	}

	dict_sys.unfreeze();

	if (reset) {
		page_zip_reset_stat_per_index();
	}

	DBUG_RETURN(status);
}

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmp_per_index.
@return 0 on success, 1 on failure */
static
int
i_s_cmp_per_index_fill(
/*===================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		cond)	/*!< in: condition (ignored) */
{
	return(i_s_cmp_per_index_fill_low(thd, tables, cond, FALSE));
}

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmp_per_index_reset.
@return 0 on success, 1 on failure */
static
int
i_s_cmp_per_index_reset_fill(
/*=========================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		cond)	/*!< in: condition (ignored) */
{
	return(i_s_cmp_per_index_fill_low(thd, tables, cond, TRUE));
}

/*******************************************************************//**
Bind the dynamic table information_schema.innodb_cmp_per_index.
@return 0 on success */
static
int
i_s_cmp_per_index_init(
/*===================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_cmp_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_cmp_per_index_fields_info;
	schema->fill_table = i_s_cmp_per_index_fill;

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table information_schema.innodb_cmp_per_index_reset.
@return 0 on success */
static
int
i_s_cmp_per_index_reset_init(
/*=========================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_cmp_reset_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_cmp_per_index_fields_info;
	schema->fill_table = i_s_cmp_per_index_reset_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_cmp_per_index =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_CMP_PER_INDEX",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"Statistics for the InnoDB compression (per index)",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_cmp_per_index_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

struct st_maria_plugin	i_s_innodb_cmp_per_index_reset =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_CMP_PER_INDEX_RESET",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"Statistics for the InnoDB compression (per index);"
		   " reset cumulated counts",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_cmp_per_index_reset_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};


namespace Show {
/* Fields of the dynamic table information_schema.innodb_cmpmem. */
static ST_FIELD_INFO i_s_cmpmem_fields_info[] =
{
  Column("page_size",           SLong(5), NOT_NULL, "Buddy Block Size"),
  Column("buffer_pool_instance", SLong(), NOT_NULL, "Buffer Pool Id"),
  Column("pages_used",           SLong(), NOT_NULL, "Currently in Use"),
  Column("pages_free",           SLong(), NOT_NULL, "Currently Available"),
  Column("relocation_ops",   SLonglong(), NOT_NULL, "Total Number of Relocations"),
  Column("relocation_time",      SLong(), NOT_NULL, "Total Duration of Relocations,"
                                                    " in Seconds"),
  CEnd()
};
} // namespace Show

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmpmem or
innodb_cmpmem_reset.
@return 0 on success, 1 on failure */
static
int
i_s_cmpmem_fill_low(
/*================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		,	/*!< in: condition (ignored) */
	ibool		reset)	/*!< in: TRUE=reset cumulated counts */
{
	TABLE*	table	= (TABLE*) tables->table;

	DBUG_ENTER("i_s_cmpmem_fill_low");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	ulint			zip_free_len_local[BUF_BUDDY_SIZES_MAX + 1];
	buf_buddy_stat_t	buddy_stat_local[BUF_BUDDY_SIZES_MAX + 1];

	/* Save buddy stats for buffer pool in local variables. */
	mysql_mutex_lock(&buf_pool.mutex);

	for (uint x = 0; x <= BUF_BUDDY_SIZES; x++) {
		zip_free_len_local[x] = (x < BUF_BUDDY_SIZES) ?
			UT_LIST_GET_LEN(buf_pool.zip_free[x]) : 0;

		buddy_stat_local[x] = buf_pool.buddy_stat[x];

		if (reset) {
			/* This is protected by buf_pool.mutex. */
			buf_pool.buddy_stat[x].relocated = 0;
			buf_pool.buddy_stat[x].relocated_usec = 0;
		}
	}

	mysql_mutex_unlock(&buf_pool.mutex);

	for (uint x = 0; x <= BUF_BUDDY_SIZES; x++) {
		buf_buddy_stat_t* buddy_stat = &buddy_stat_local[x];

		Field **field = table->field;

		(*field++)->store(BUF_BUDDY_LOW << x);
		(*field++)->store(0, true);
		(*field++)->store(buddy_stat->used, true);
		(*field++)->store(zip_free_len_local[x], true);
		(*field++)->store(buddy_stat->relocated, true);
		(*field)->store(buddy_stat->relocated_usec / 1000000, true);

		if (schema_table_store_record(thd, table)) {
			DBUG_RETURN(1);
		}
	}

	DBUG_RETURN(0);
}

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmpmem.
@return 0 on success, 1 on failure */
static
int
i_s_cmpmem_fill(
/*============*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		cond)	/*!< in: condition (ignored) */
{
	return(i_s_cmpmem_fill_low(thd, tables, cond, FALSE));
}

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_cmpmem_reset.
@return 0 on success, 1 on failure */
static
int
i_s_cmpmem_reset_fill(
/*==================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		cond)	/*!< in: condition (ignored) */
{
	return(i_s_cmpmem_fill_low(thd, tables, cond, TRUE));
}

/*******************************************************************//**
Bind the dynamic table information_schema.innodb_cmpmem.
@return 0 on success */
static
int
i_s_cmpmem_init(
/*============*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_cmpmem_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_cmpmem_fields_info;
	schema->fill_table = i_s_cmpmem_fill;

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table information_schema.innodb_cmpmem_reset.
@return 0 on success */
static
int
i_s_cmpmem_reset_init(
/*==================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_cmpmem_reset_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_cmpmem_fields_info;
	schema->fill_table = i_s_cmpmem_reset_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_cmpmem =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_CMPMEM",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"Statistics for the InnoDB compressed buffer pool",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_cmpmem_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

struct st_maria_plugin	i_s_innodb_cmpmem_reset =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_CMPMEM_RESET",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"Statistics for the InnoDB compressed buffer pool;"
		   " reset cumulated counts",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_cmpmem_reset_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};


static const LEX_CSTRING metric_type_values[] =
{
	{ STRING_WITH_LEN("value") },
	{ STRING_WITH_LEN("status_counter") },
	{ STRING_WITH_LEN("set_owner") },
	{ STRING_WITH_LEN("set_member") },
	{ STRING_WITH_LEN("counter") }
};

static TypelibBuffer<5> metric_type_values_typelib(metric_type_values);

namespace Show {
/* Fields of the dynamic table INFORMATION_SCHEMA.innodb_metrics */
static ST_FIELD_INFO innodb_metrics_fields_info[]=
{
#define	METRIC_NAME		0
  Column("NAME",            Varchar(NAME_LEN + 1),       NOT_NULL),

#define	METRIC_SUBSYS		1
  Column("SUBSYSTEM",       Varchar(NAME_LEN + 1),       NOT_NULL),

#define	METRIC_VALUE_START	2
  Column("COUNT",           SLonglong(),                 NOT_NULL),

#define	METRIC_MAX_VALUE_START	3
  Column("MAX_COUNT",       SLonglong(),                 NULLABLE),

#define	METRIC_MIN_VALUE_START	4
  Column("MIN_COUNT",       SLonglong(),                 NULLABLE),

#define	METRIC_AVG_VALUE_START	5
  Column("AVG_COUNT",       Float(MAX_FLOAT_STR_LENGTH), NULLABLE),

#define	METRIC_VALUE_RESET	6
  Column("COUNT_RESET",     SLonglong(),                 NOT_NULL),

#define	METRIC_MAX_VALUE_RESET	7
  Column("MAX_COUNT_RESET", SLonglong(),                 NULLABLE),

#define	METRIC_MIN_VALUE_RESET	8
  Column("MIN_COUNT_RESET", SLonglong(),                 NULLABLE),

#define	METRIC_AVG_VALUE_RESET	9
  Column("AVG_COUNT_RESET", Float(MAX_FLOAT_STR_LENGTH), NULLABLE),

#define	METRIC_START_TIME	10
  Column("TIME_ENABLED",    Datetime(0),                 NULLABLE),

#define	METRIC_STOP_TIME	11
  Column("TIME_DISABLED",   Datetime(0),                 NULLABLE),

#define	METRIC_TIME_ELAPSED	12
  Column("TIME_ELAPSED",    SLonglong(),                 NULLABLE),

#define	METRIC_RESET_TIME	13
  Column("TIME_RESET",      Datetime(0),                 NULLABLE),

#define	METRIC_STATUS		14
  Column("ENABLED", SLong(1), NOT_NULL),

#define	METRIC_TYPE		15
  Column("TYPE",    Enum(&metric_type_values_typelib), NOT_NULL),

#define	METRIC_DESC		16
  Column("COMMENT",         Varchar(NAME_LEN + 1),       NOT_NULL),
  CEnd()
};
} // namespace Show

/**********************************************************************//**
Fill the information schema metrics table.
@return 0 on success */
static
int
i_s_metrics_fill(
/*=============*/
	THD*		thd,		/*!< in: thread */
	TABLE*		table_to_fill)	/*!< in/out: fill this table */
{
	int		count;
	Field**		fields;
	double		time_diff = 0;
	monitor_info_t*	monitor_info;
	mon_type_t	min_val;
	mon_type_t	max_val;

	DBUG_ENTER("i_s_metrics_fill");
	fields = table_to_fill->field;

	for (count = 0; count < NUM_MONITOR; count++) {
		monitor_info = srv_mon_get_info((monitor_id_t) count);

		/* A good place to sanity check the Monitor ID */
		ut_a(count == monitor_info->monitor_id);

		/* If the item refers to a Module, nothing to fill,
		continue. */
		if ((monitor_info->monitor_type & MONITOR_MODULE)
		    || (monitor_info->monitor_type & MONITOR_HIDDEN)) {
			continue;
		}

		/* If this is an existing "status variable", and
		its corresponding counter is still on, we need
		to calculate the result from its corresponding
		counter. */
		if (monitor_info->monitor_type & MONITOR_EXISTING
		    && MONITOR_IS_ON(count)) {
			srv_mon_process_existing_counter((monitor_id_t) count,
							 MONITOR_GET_VALUE);
		}

		/* Fill in counter's basic information */
		OK(field_store_string(fields[METRIC_NAME],
				      monitor_info->monitor_name));

		OK(field_store_string(fields[METRIC_SUBSYS],
				      monitor_info->monitor_module));

		OK(field_store_string(fields[METRIC_DESC],
				      monitor_info->monitor_desc));

		/* Fill in counter values */
		OK(fields[METRIC_VALUE_RESET]->store(
			MONITOR_VALUE(count), FALSE));

		OK(fields[METRIC_VALUE_START]->store(
			MONITOR_VALUE_SINCE_START(count), FALSE));

		/* If the max value is MAX_RESERVED, counter max
		value has not been updated. Set the column value
		to NULL. */
		if (MONITOR_MAX_VALUE(count) == MAX_RESERVED
		    || MONITOR_MAX_MIN_NOT_INIT(count)) {
			fields[METRIC_MAX_VALUE_RESET]->set_null();
		} else {
			OK(fields[METRIC_MAX_VALUE_RESET]->store(
				MONITOR_MAX_VALUE(count), FALSE));
			fields[METRIC_MAX_VALUE_RESET]->set_notnull();
		}

		/* If the min value is MAX_RESERVED, counter min
		value has not been updated. Set the column value
		to NULL. */
		if (MONITOR_MIN_VALUE(count) == MIN_RESERVED
		    || MONITOR_MAX_MIN_NOT_INIT(count)) {
			fields[METRIC_MIN_VALUE_RESET]->set_null();
		} else {
			OK(fields[METRIC_MIN_VALUE_RESET]->store(
				MONITOR_MIN_VALUE(count), FALSE));
			fields[METRIC_MIN_VALUE_RESET]->set_notnull();
		}

		/* Calculate the max value since counter started */
		max_val = srv_mon_calc_max_since_start((monitor_id_t) count);

		if (max_val == MAX_RESERVED
		    || MONITOR_MAX_MIN_NOT_INIT(count)) {
			fields[METRIC_MAX_VALUE_START]->set_null();
		} else {
			OK(fields[METRIC_MAX_VALUE_START]->store(
				max_val, FALSE));
			fields[METRIC_MAX_VALUE_START]->set_notnull();
		}

		/* Calculate the min value since counter started */
		min_val = srv_mon_calc_min_since_start((monitor_id_t) count);

		if (min_val == MIN_RESERVED
		    || MONITOR_MAX_MIN_NOT_INIT(count)) {
			fields[METRIC_MIN_VALUE_START]->set_null();
		} else {
			OK(fields[METRIC_MIN_VALUE_START]->store(
				min_val, FALSE));

			fields[METRIC_MIN_VALUE_START]->set_notnull();
		}

		/* If monitor has been enabled (no matter it is disabled
		or not now), fill METRIC_START_TIME and METRIC_TIME_ELAPSED
		field */
		if (MONITOR_FIELD(count, mon_start_time)) {
			OK(field_store_time_t(fields[METRIC_START_TIME],
				(time_t)MONITOR_FIELD(count, mon_start_time)));
			fields[METRIC_START_TIME]->set_notnull();

			/* If monitor is enabled, the TIME_ELAPSED is the
			time difference between current and time when monitor
			is enabled. Otherwise, it is the time difference
			between time when monitor is enabled and time
			when it is disabled */
			if (MONITOR_IS_ON(count)) {
				time_diff = difftime(time(NULL),
					MONITOR_FIELD(count, mon_start_time));
			} else {
				time_diff =  difftime(
					MONITOR_FIELD(count, mon_stop_time),
					MONITOR_FIELD(count, mon_start_time));
			}

			OK(fields[METRIC_TIME_ELAPSED]->store(
				time_diff));
			fields[METRIC_TIME_ELAPSED]->set_notnull();
		} else {
			fields[METRIC_START_TIME]->set_null();
			fields[METRIC_TIME_ELAPSED]->set_null();
			time_diff = 0;
		}

		/* Unless MONITOR_NO_AVERAGE is set, we must
		to calculate the average value. If this is a monitor set
		owner marked by MONITOR_SET_OWNER, divide
		the value by another counter (number of calls) designated
		by monitor_info->monitor_related_id.
		Otherwise average the counter value by the time between the
		time that the counter is enabled and time it is disabled
		or time it is sampled. */
		if ((monitor_info->monitor_type
		     & (MONITOR_NO_AVERAGE | MONITOR_SET_OWNER))
		    == MONITOR_SET_OWNER
		    && monitor_info->monitor_related_id) {
			mon_type_t	value_start
				 = MONITOR_VALUE_SINCE_START(
					monitor_info->monitor_related_id);

			if (value_start) {
				OK(fields[METRIC_AVG_VALUE_START]->store(
					MONITOR_VALUE_SINCE_START(count)
					/ value_start, FALSE));

				fields[METRIC_AVG_VALUE_START]->set_notnull();
			} else {
				fields[METRIC_AVG_VALUE_START]->set_null();
			}

			if (mon_type_t related_value =
			    MONITOR_VALUE(monitor_info->monitor_related_id)) {
				OK(fields[METRIC_AVG_VALUE_RESET]
				   ->store(MONITOR_VALUE(count)
					   / related_value, false));
				fields[METRIC_AVG_VALUE_RESET]->set_notnull();
			} else {
				fields[METRIC_AVG_VALUE_RESET]->set_null();
			}
		} else if (!(monitor_info->monitor_type
			     & (MONITOR_NO_AVERAGE
				| MONITOR_DISPLAY_CURRENT))) {
			if (time_diff != 0) {
				OK(fields[METRIC_AVG_VALUE_START]->store(
					(double) MONITOR_VALUE_SINCE_START(
						count) / time_diff));
				fields[METRIC_AVG_VALUE_START]->set_notnull();
			} else {
				fields[METRIC_AVG_VALUE_START]->set_null();
			}

			if (MONITOR_FIELD(count, mon_reset_time)) {
				/* calculate the time difference since last
				reset */
				if (MONITOR_IS_ON(count)) {
					time_diff = difftime(
						time(NULL), MONITOR_FIELD(
							count, mon_reset_time));
				} else {
					time_diff =  difftime(
					MONITOR_FIELD(count, mon_stop_time),
					MONITOR_FIELD(count, mon_reset_time));
				}
			} else {
				time_diff = 0;
			}

			if (time_diff != 0) {
				OK(fields[METRIC_AVG_VALUE_RESET]->store(
					static_cast<double>(
						MONITOR_VALUE(count))
					/ time_diff));
				fields[METRIC_AVG_VALUE_RESET]->set_notnull();
			} else {
				fields[METRIC_AVG_VALUE_RESET]->set_null();
			}
		} else {
			fields[METRIC_AVG_VALUE_START]->set_null();
			fields[METRIC_AVG_VALUE_RESET]->set_null();
		}

		if (MONITOR_IS_ON(count)) {
			/* If monitor is on, the stop time will set to NULL */
			fields[METRIC_STOP_TIME]->set_null();

			/* Display latest Monitor Reset Time only if Monitor
			counter is on. */
			if (MONITOR_FIELD(count, mon_reset_time)) {
				OK(field_store_time_t(
					fields[METRIC_RESET_TIME],
					(time_t)MONITOR_FIELD(
						count, mon_reset_time)));
				fields[METRIC_RESET_TIME]->set_notnull();
			} else {
				fields[METRIC_RESET_TIME]->set_null();
			}

			OK(fields[METRIC_STATUS]->store(1, true));
		} else {
			if (MONITOR_FIELD(count, mon_stop_time)) {
				OK(field_store_time_t(fields[METRIC_STOP_TIME],
				(time_t)MONITOR_FIELD(count, mon_stop_time)));
				fields[METRIC_STOP_TIME]->set_notnull();
			} else {
				fields[METRIC_STOP_TIME]->set_null();
			}

			fields[METRIC_RESET_TIME]->set_null();

			OK(fields[METRIC_STATUS]->store(0, true));
		}

		uint metric_type;

		if (monitor_info->monitor_type & MONITOR_DISPLAY_CURRENT) {
			metric_type = 1; /* "value" */
		} else if (monitor_info->monitor_type & MONITOR_EXISTING) {
			metric_type = 2; /* "status_counter" */
		} else if (monitor_info->monitor_type & MONITOR_SET_OWNER) {
			metric_type = 3; /* "set_owner" */
		} else if (monitor_info->monitor_type & MONITOR_SET_MEMBER) {
			metric_type = 4; /* "set_member" */
		} else {
			metric_type = 5; /* "counter" */
		}

		OK(fields[METRIC_TYPE]->store(metric_type, true));

		OK(schema_table_store_record(thd, table_to_fill));
	}

	DBUG_RETURN(0);
}

/*******************************************************************//**
Function to fill information schema metrics tables.
@return 0 on success */
static
int
i_s_metrics_fill_table(
/*===================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	DBUG_ENTER("i_s_metrics_fill_table");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	i_s_metrics_fill(thd, tables->table);

	DBUG_RETURN(0);
}
/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_metrics
@return 0 on success */
static
int
innodb_metrics_init(
/*================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_metrics_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_metrics_fields_info;
	schema->fill_table = i_s_metrics_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_metrics =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_METRICS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB Metrics Info",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_metrics_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/* Fields of the dynamic table INFORMATION_SCHEMA.innodb_ft_default_stopword */
static ST_FIELD_INFO i_s_stopword_fields_info[]=
{
#define STOPWORD_VALUE	0
  Column("value", Varchar(TRX_ID_MAX_LEN + 1), NOT_NULL),
  CEnd()
};
} // namespace Show

/*******************************************************************//**
Fill the dynamic table information_schema.innodb_ft_default_stopword.
@return 0 on success, 1 on failure */
static
int
i_s_stopword_fill(
/*==============*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	Field**	fields;
	ulint	i = 0;
	TABLE*	table = (TABLE*) tables->table;

	DBUG_ENTER("i_s_stopword_fill");

	fields = table->field;

	/* Fill with server default stopword list in array
	fts_default_stopword */
	while (fts_default_stopword[i]) {
		OK(field_store_string(fields[STOPWORD_VALUE],
				      fts_default_stopword[i]));

		OK(schema_table_store_record(thd, table));
		i++;
	}

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table information_schema.innodb_ft_default_stopword.
@return 0 on success */
static
int
i_s_stopword_init(
/*==============*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_stopword_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_stopword_fields_info;
	schema->fill_table = i_s_stopword_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_ft_default_stopword =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_FT_DEFAULT_STOPWORD",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"Default stopword list for InnoDB Full Text Search",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_stopword_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_FT_DELETED
INFORMATION_SCHEMA.INNODB_FT_BEING_DELETED */
static ST_FIELD_INFO i_s_fts_doc_fields_info[]=
{
#define	I_S_FTS_DOC_ID			0
  Column("DOC_ID", ULonglong(), NOT_NULL),
  CEnd()
};
} // namespace Show

/*******************************************************************//**
Fill the dynamic table INFORMATION_SCHEMA.INNODB_FT_DELETED or
INFORMATION_SCHEMA.INNODB_FT_BEING_DELETED
@return 0 on success, 1 on failure */
static
int
i_s_fts_deleted_generic_fill(
/*=========================*/
	THD*		thd,		/*!< in: thread */
	TABLE_LIST*	tables,		/*!< in/out: tables to fill */
	ibool		being_deleted)	/*!< in: BEING_DELTED table */
{
	Field**			fields;
	TABLE*			table = (TABLE*) tables->table;
	trx_t*			trx;
	fts_table_t		fts_table;
	fts_doc_ids_t*		deleted;
	dict_table_t*		user_table;

	DBUG_ENTER("i_s_fts_deleted_generic_fill");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	MDL_ticket* mdl_ticket = nullptr;
	user_table = dict_table_open_on_id(
		innodb_ft_aux_table_id, false, DICT_TABLE_OP_NORMAL,
		thd, &mdl_ticket);

	if (!user_table) {
		DBUG_RETURN(0);
	} else if (!dict_table_has_fts_index(user_table)
		   || !user_table->is_readable()) {
		dict_table_close(user_table, false, thd, mdl_ticket);
		DBUG_RETURN(0);
	}

	deleted = fts_doc_ids_create();

	trx = trx_create();
	trx->op_info = "Select for FTS DELETE TABLE";

	FTS_INIT_FTS_TABLE(&fts_table,
			   (being_deleted) ? "BEING_DELETED" : "DELETED",
			   FTS_COMMON_TABLE, user_table);

	fts_table_fetch_doc_ids(trx, &fts_table, deleted);

	dict_table_close(user_table, false, thd, mdl_ticket);

	trx->free();

	fields = table->field;

	int	ret = 0;

	for (ulint j = 0; j < ib_vector_size(deleted->doc_ids); ++j) {
		doc_id_t	doc_id;

		doc_id = *(doc_id_t*) ib_vector_get_const(deleted->doc_ids, j);

		BREAK_IF(ret = fields[I_S_FTS_DOC_ID]->store(doc_id, true));

		BREAK_IF(ret = schema_table_store_record(thd, table));
	}

	fts_doc_ids_free(deleted);

	DBUG_RETURN(ret);
}

/*******************************************************************//**
Fill the dynamic table INFORMATION_SCHEMA.INNODB_FT_DELETED
@return 0 on success, 1 on failure */
static
int
i_s_fts_deleted_fill(
/*=================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (ignored) */
{
	DBUG_ENTER("i_s_fts_deleted_fill");

	DBUG_RETURN(i_s_fts_deleted_generic_fill(thd, tables, FALSE));
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_FT_DELETED
@return 0 on success */
static
int
i_s_fts_deleted_init(
/*=================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_fts_deleted_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_fts_doc_fields_info;
	schema->fill_table = i_s_fts_deleted_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_ft_deleted =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_FT_DELETED",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"INNODB AUXILIARY FTS DELETED TABLE",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_fts_deleted_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

/*******************************************************************//**
Fill the dynamic table INFORMATION_SCHEMA.INNODB_FT_BEING_DELETED
@return 0 on success, 1 on failure */
static
int
i_s_fts_being_deleted_fill(
/*=======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (ignored) */
{
	DBUG_ENTER("i_s_fts_being_deleted_fill");

	DBUG_RETURN(i_s_fts_deleted_generic_fill(thd, tables, TRUE));
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_FT_BEING_DELETED
@return 0 on success */
static
int
i_s_fts_being_deleted_init(
/*=======================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_fts_deleted_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_fts_doc_fields_info;
	schema->fill_table = i_s_fts_being_deleted_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_ft_being_deleted =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_FT_BEING_DELETED",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"INNODB AUXILIARY FTS BEING DELETED TABLE",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_fts_being_deleted_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};


namespace Show {
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_FT_INDEX_CACHED and
INFORMATION_SCHEMA.INNODB_FT_INDEX_TABLE */
static ST_FIELD_INFO i_s_fts_index_fields_info[]=
{
#define	I_S_FTS_WORD			0
  Column("WORD",         Varchar(FTS_MAX_WORD_LEN + 1), NOT_NULL),

#define	I_S_FTS_FIRST_DOC_ID		1
  Column("FIRST_DOC_ID", ULonglong(),                   NOT_NULL),

#define	I_S_FTS_LAST_DOC_ID		2
  Column("LAST_DOC_ID",  ULonglong(),                   NOT_NULL),

#define	I_S_FTS_DOC_COUNT		3
  Column("DOC_COUNT",    ULonglong(),                   NOT_NULL),

#define	I_S_FTS_ILIST_DOC_ID		4
  Column("DOC_ID",       ULonglong(),                   NOT_NULL),

#define	I_S_FTS_ILIST_DOC_POS		5
  Column("POSITION",     ULonglong(),                   NOT_NULL),
  CEnd()
};
} // namespace Show

/*******************************************************************//**
Go through the Doc Node and its ilist, fill the dynamic table
INFORMATION_SCHEMA.INNODB_FT_INDEX_CACHED for one FTS index on the table.
@return 0 on success, 1 on failure */
static
int
i_s_fts_index_cache_fill_one_index(
/*===============================*/
	fts_index_cache_t*	index_cache,	/*!< in: FTS index cache */
	THD*			thd,		/*!< in: thread */
	fts_string_t*		conv_str,	/*!< in/out: buffer */
	TABLE_LIST*		tables)		/*!< in/out: tables to fill */
{
	TABLE*			table = (TABLE*) tables->table;
	Field**			fields;
	CHARSET_INFO*		index_charset;
	const ib_rbt_node_t*	rbt_node;
	uint			dummy_errors;
	char*			word_str;

	DBUG_ENTER("i_s_fts_index_cache_fill_one_index");

	fields = table->field;

	index_charset = index_cache->charset;
	conv_str->f_n_char = 0;

	int	ret = 0;

	/* Go through each word in the index cache */
	for (rbt_node = rbt_first(index_cache->words);
	     rbt_node;
	     rbt_node = rbt_next(index_cache->words, rbt_node)) {
		fts_tokenizer_word_t* word;

		word = rbt_value(fts_tokenizer_word_t, rbt_node);

		/* Convert word from index charset to system_charset_info */
		if (index_charset->cset != system_charset_info->cset) {
			conv_str->f_n_char = my_convert(
				reinterpret_cast<char*>(conv_str->f_str),
				static_cast<uint32>(conv_str->f_len),
				system_charset_info,
				reinterpret_cast<char*>(word->text.f_str),
				static_cast<uint32>(word->text.f_len),
				index_charset, &dummy_errors);
			ut_ad(conv_str->f_n_char <= conv_str->f_len);
			conv_str->f_str[conv_str->f_n_char] = 0;
			word_str = reinterpret_cast<char*>(conv_str->f_str);
		} else {
			word_str = reinterpret_cast<char*>(word->text.f_str);
		}

		/* Decrypt the ilist, and display Dod ID and word position */
		for (ulint i = 0; i < ib_vector_size(word->nodes); i++) {
			fts_node_t*	node;
			const byte*	ptr;
			ulint		decoded = 0;
			doc_id_t	doc_id = 0;

			node = static_cast<fts_node_t*> (ib_vector_get(
				word->nodes, i));

			ptr = node->ilist;

			while (decoded < node->ilist_size) {

				doc_id += fts_decode_vlc(&ptr);

				/* Get position info */
				while (*ptr) {

					OK(field_store_string(
						   fields[I_S_FTS_WORD],
						   word_str));

					OK(fields[I_S_FTS_FIRST_DOC_ID]->store(
						   node->first_doc_id,
						   true));

					OK(fields[I_S_FTS_LAST_DOC_ID]->store(
						   node->last_doc_id,
						   true));

					OK(fields[I_S_FTS_DOC_COUNT]->store(
						   node->doc_count, true));

					OK(fields[I_S_FTS_ILIST_DOC_ID]->store(
						   doc_id, true));

					OK(fields[I_S_FTS_ILIST_DOC_POS]->store(
						   fts_decode_vlc(&ptr), true));

					OK(schema_table_store_record(
						   thd, table));
				}

				++ptr;

				decoded = ptr - (byte*) node->ilist;
			}
		}
	}

	DBUG_RETURN(ret);
}
/*******************************************************************//**
Fill the dynamic table INFORMATION_SCHEMA.INNODB_FT_INDEX_CACHED
@return 0 on success, 1 on failure */
static
int
i_s_fts_index_cache_fill(
/*=====================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (ignored) */
{
	dict_table_t*		user_table;
	fts_cache_t*		cache;

	DBUG_ENTER("i_s_fts_index_cache_fill");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	MDL_ticket* mdl_ticket = nullptr;
	user_table = dict_table_open_on_id(
		innodb_ft_aux_table_id, false, DICT_TABLE_OP_NORMAL,
		thd, &mdl_ticket);

	if (!user_table) {
		DBUG_RETURN(0);
	}

	if (!user_table->fts || !user_table->fts->cache) {
		dict_table_close(user_table, false, thd, mdl_ticket);
		DBUG_RETURN(0);
	}

	cache = user_table->fts->cache;

	int			ret = 0;
	fts_string_t		conv_str;
	byte			word[HA_FT_MAXBYTELEN + 1];
	conv_str.f_len = sizeof word;
	conv_str.f_str = word;

	mysql_mutex_lock(&cache->lock);

	for (ulint i = 0; i < ib_vector_size(cache->indexes); i++) {
		fts_index_cache_t*      index_cache;

		index_cache = static_cast<fts_index_cache_t*> (
			ib_vector_get(cache->indexes, i));

		BREAK_IF(ret = i_s_fts_index_cache_fill_one_index(
				 index_cache, thd, &conv_str, tables));
	}

	mysql_mutex_unlock(&cache->lock);
	dict_table_close(user_table, false, thd, mdl_ticket);

	DBUG_RETURN(ret);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_FT_INDEX_CACHE
@return 0 on success */
static
int
i_s_fts_index_cache_init(
/*=====================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_fts_index_cache_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_fts_index_fields_info;
	schema->fill_table = i_s_fts_index_cache_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_ft_index_cache =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_FT_INDEX_CACHE",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"INNODB AUXILIARY FTS INDEX CACHED",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_fts_index_cache_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

/*******************************************************************//**
Go through a FTS index auxiliary table, fetch its rows and fill
FTS word cache structure.
@return DB_SUCCESS on success, otherwise error code */
static
dberr_t
i_s_fts_index_table_fill_selected(
/*==============================*/
	dict_index_t*		index,		/*!< in: FTS index */
	ib_vector_t*		words,		/*!< in/out: vector to hold
						fetched words */
	ulint			selected,	/*!< in: selected FTS index */
	fts_string_t*		word)		/*!< in: word to select */
{
	pars_info_t*		info;
	fts_table_t		fts_table;
	trx_t*			trx;
	que_t*			graph;
	dberr_t			error;
	fts_fetch_t		fetch;
	char			table_name[MAX_FULL_NAME_LEN];

	info = pars_info_create();

	fetch.read_arg = words;
	fetch.read_record = fts_optimize_index_fetch_node;
	fetch.total_memory = 0;

	DBUG_EXECUTE_IF("fts_instrument_result_cache_limit",
	        fts_result_cache_limit = 8192;
	);

	trx = trx_create();

	trx->op_info = "fetching FTS index nodes";

	pars_info_bind_function(info, "my_func", fetch.read_record, &fetch);
	pars_info_bind_varchar_literal(info, "word", word->f_str, word->f_len);

	FTS_INIT_INDEX_TABLE(&fts_table, fts_get_suffix(selected),
			     FTS_INDEX_TABLE, index);
	fts_get_table_name(&fts_table, table_name);
	pars_info_bind_id(info, "table_name", table_name);

	graph = fts_parse_sql(
		&fts_table, info,
		"DECLARE FUNCTION my_func;\n"
		"DECLARE CURSOR c IS"
		" SELECT word, doc_count, first_doc_id, last_doc_id,"
		" ilist\n"
		" FROM $table_name WHERE word >= :word;\n"
		"BEGIN\n"
		"\n"
		"OPEN c;\n"
		"WHILE 1 = 1 LOOP\n"
		"  FETCH c INTO my_func();\n"
		"  IF c % NOTFOUND THEN\n"
		"    EXIT;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"CLOSE c;");

	for (;;) {
		error = fts_eval_sql(trx, graph);

		if (UNIV_LIKELY(error == DB_SUCCESS)) {
			fts_sql_commit(trx);

			break;
		} else {
			fts_sql_rollback(trx);

			if (error == DB_LOCK_WAIT_TIMEOUT) {
				ib::warn() << "Lock wait timeout reading"
					" FTS index. Retrying!";

				trx->error_state = DB_SUCCESS;
			} else {
				ib::error() << "Error occurred while reading"
					" FTS index: " << error;
				break;
			}
		}
	}

	que_graph_free(graph);

	trx->free();

	if (fetch.total_memory >= fts_result_cache_limit) {
		error = DB_FTS_EXCEED_RESULT_CACHE_LIMIT;
	}

	return(error);
}

/*******************************************************************//**
Free words. */
static
void
i_s_fts_index_table_free_one_fetch(
/*===============================*/
	ib_vector_t*		words)		/*!< in: words fetched */
{
	for (ulint i = 0; i < ib_vector_size(words); i++) {
		fts_word_t*	word;

		word = static_cast<fts_word_t*>(ib_vector_get(words, i));

		for (ulint j = 0; j < ib_vector_size(word->nodes); j++) {
			fts_node_t*     node;

			node = static_cast<fts_node_t*> (ib_vector_get(
				word->nodes, j));
			ut_free(node->ilist);
		}

		fts_word_free(word);
	}

	ib_vector_reset(words);
}

/*******************************************************************//**
Go through words, fill INFORMATION_SCHEMA.INNODB_FT_INDEX_TABLE.
@return	0 on success, 1 on failure */
static
int
i_s_fts_index_table_fill_one_fetch(
/*===============================*/
	CHARSET_INFO*		index_charset,	/*!< in: FTS index charset */
	THD*			thd,		/*!< in: thread */
	TABLE_LIST*		tables,		/*!< in/out: tables to fill */
	ib_vector_t*		words,		/*!< in: words fetched */
	fts_string_t*		conv_str,	/*!< in: string for conversion*/
	bool			has_more)	/*!< in: has more to fetch */
{
	TABLE*			table = (TABLE*) tables->table;
	Field**			fields;
	uint			dummy_errors;
	char*			word_str;
	ulint			words_size;
	int			ret = 0;

	DBUG_ENTER("i_s_fts_index_table_fill_one_fetch");

	fields = table->field;

	words_size = ib_vector_size(words);
	if (has_more) {
		/* the last word is not fetched completely. */
		ut_ad(words_size > 1);
		words_size -= 1;
	}

	/* Go through each word in the index cache */
	for (ulint i = 0; i < words_size; i++) {
		fts_word_t*	word;

		word = static_cast<fts_word_t*>(ib_vector_get(words, i));

		word->text.f_str[word->text.f_len] = 0;

		/* Convert word from index charset to system_charset_info */
		if (index_charset->cset != system_charset_info->cset) {
			conv_str->f_n_char = my_convert(
				reinterpret_cast<char*>(conv_str->f_str),
				static_cast<uint32>(conv_str->f_len),
				system_charset_info,
				reinterpret_cast<char*>(word->text.f_str),
				static_cast<uint32>(word->text.f_len),
				index_charset, &dummy_errors);
			ut_ad(conv_str->f_n_char <= conv_str->f_len);
			conv_str->f_str[conv_str->f_n_char] = 0;
			word_str = reinterpret_cast<char*>(conv_str->f_str);
		} else {
			word_str = reinterpret_cast<char*>(word->text.f_str);
		}

		/* Decrypt the ilist, and display Dod ID and word position */
		for (ulint i = 0; i < ib_vector_size(word->nodes); i++) {
			fts_node_t*	node;
			const byte*	ptr;
			ulint		decoded = 0;
			doc_id_t	doc_id = 0;

			node = static_cast<fts_node_t*> (ib_vector_get(
				word->nodes, i));

			ptr = node->ilist;

			while (decoded < node->ilist_size) {
				doc_id += fts_decode_vlc(&ptr);

				/* Get position info */
				while (*ptr) {

					OK(field_store_string(
						   fields[I_S_FTS_WORD],
						   word_str));

					OK(fields[I_S_FTS_FIRST_DOC_ID]->store(
						longlong(node->first_doc_id), true));

					OK(fields[I_S_FTS_LAST_DOC_ID]->store(
						longlong(node->last_doc_id), true));

					OK(fields[I_S_FTS_DOC_COUNT]->store(
						   node->doc_count, true));

					OK(fields[I_S_FTS_ILIST_DOC_ID]->store(
						longlong(doc_id), true));

					OK(fields[I_S_FTS_ILIST_DOC_POS]->store(
						   fts_decode_vlc(&ptr), true));

					OK(schema_table_store_record(
						   thd, table));
				}

				++ptr;

				decoded = ptr - (byte*) node->ilist;
			}
		}
	}

	DBUG_RETURN(ret);
}

/*******************************************************************//**
Go through a FTS index and its auxiliary tables, fetch rows in each table
and fill INFORMATION_SCHEMA.INNODB_FT_INDEX_TABLE.
@return 0 on success, 1 on failure */
static
int
i_s_fts_index_table_fill_one_index(
/*===============================*/
	dict_index_t*		index,		/*!< in: FTS index */
	THD*			thd,		/*!< in: thread */
	fts_string_t*		conv_str,	/*!< in/out: buffer */
	TABLE_LIST*		tables)		/*!< in/out: tables to fill */
{
	ib_vector_t*		words;
	mem_heap_t*		heap;
	CHARSET_INFO*		index_charset;
	dberr_t			error;
	int			ret = 0;

	DBUG_ENTER("i_s_fts_index_table_fill_one_index");
	DBUG_ASSERT(!dict_index_is_online_ddl(index));

	heap = mem_heap_create(1024);

	words = ib_vector_create(ib_heap_allocator_create(heap),
				 sizeof(fts_word_t), 256);

	index_charset = fts_index_get_charset(index);

	/* Iterate through each auxiliary table as described in
	fts_index_selector */
	for (ulint selected = 0; selected < FTS_NUM_AUX_INDEX; selected++) {
		fts_string_t	word;
		bool		has_more = false;

		word.f_str = NULL;
		word.f_len = 0;
		word.f_n_char = 0;

		do {
			/* Fetch from index */
			error = i_s_fts_index_table_fill_selected(
				index, words, selected, &word);

			if (error == DB_SUCCESS) {
				has_more = false;
			} else if (error == DB_FTS_EXCEED_RESULT_CACHE_LIMIT) {
				has_more = true;
			} else {
				i_s_fts_index_table_free_one_fetch(words);
				ret = 1;
				goto func_exit;
			}

			if (has_more) {
				fts_word_t*	last_word;

				/* Prepare start point for next fetch */
				last_word = static_cast<fts_word_t*>(ib_vector_last(words));
				ut_ad(last_word != NULL);
				fts_string_dup(&word, &last_word->text, heap);
			}

			/* Fill into tables */
			ret = i_s_fts_index_table_fill_one_fetch(
				index_charset, thd, tables, words, conv_str,
				has_more);
			i_s_fts_index_table_free_one_fetch(words);

			if (ret != 0) {
				goto func_exit;
			}
		} while (has_more);
	}

func_exit:
	mem_heap_free(heap);

	DBUG_RETURN(ret);
}
/*******************************************************************//**
Fill the dynamic table INFORMATION_SCHEMA.INNODB_FT_INDEX_TABLE
@return 0 on success, 1 on failure */
static
int
i_s_fts_index_table_fill(
/*=====================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (ignored) */
{
	dict_table_t*		user_table;
	dict_index_t*		index;

	DBUG_ENTER("i_s_fts_index_table_fill");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	MDL_ticket* mdl_ticket = nullptr;
	user_table = dict_table_open_on_id(
		innodb_ft_aux_table_id, false, DICT_TABLE_OP_NORMAL,
		thd, &mdl_ticket);

	if (!user_table) {
		DBUG_RETURN(0);
	}

	int		ret = 0;
	fts_string_t	conv_str;
	conv_str.f_len = system_charset_info->mbmaxlen
		* FTS_MAX_WORD_LEN_IN_CHAR;
	conv_str.f_str = static_cast<byte*>(ut_malloc_nokey(conv_str.f_len));

	for (index = dict_table_get_first_index(user_table);
	     index; index = dict_table_get_next_index(index)) {
		if (index->type & DICT_FTS) {
			BREAK_IF(ret = i_s_fts_index_table_fill_one_index(
					 index, thd, &conv_str, tables));
		}
	}

	dict_table_close(user_table, false, thd, mdl_ticket);

	ut_free(conv_str.f_str);

	DBUG_RETURN(ret);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_FT_INDEX_TABLE
@return 0 on success */
static
int
i_s_fts_index_table_init(
/*=====================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_fts_index_table_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_fts_index_fields_info;
	schema->fill_table = i_s_fts_index_table_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_ft_index_table =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_FT_INDEX_TABLE",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"INNODB AUXILIARY FTS INDEX TABLE",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_fts_index_table_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};


namespace Show {
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_FT_CONFIG */
static ST_FIELD_INFO i_s_fts_config_fields_info[]=
{
#define	FTS_CONFIG_KEY			0
  Column("KEY",   Varchar(NAME_LEN + 1),  NOT_NULL),

#define	FTS_CONFIG_VALUE		1
  Column("VALUE", Varchar(NAME_LEN + 1),  NOT_NULL),

  CEnd()
};
} // namespace Show

static const char* fts_config_key[] = {
	FTS_OPTIMIZE_LIMIT_IN_SECS,
	FTS_SYNCED_DOC_ID,
	FTS_STOPWORD_TABLE_NAME,
	FTS_USE_STOPWORD,
        NULL
};

/*******************************************************************//**
Fill the dynamic table INFORMATION_SCHEMA.INNODB_FT_CONFIG
@return 0 on success, 1 on failure */
static
int
i_s_fts_config_fill(
/*================*/
	THD*		thd,		/*!< in: thread */
	TABLE_LIST*	tables,		/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (ignored) */
{
	Field**			fields;
	TABLE*			table = (TABLE*) tables->table;
	trx_t*			trx;
	fts_table_t		fts_table;
	dict_table_t*		user_table;
	ulint			i = 0;
	dict_index_t*		index = NULL;
	unsigned char		str[FTS_MAX_CONFIG_VALUE_LEN + 1];

	DBUG_ENTER("i_s_fts_config_fill");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	MDL_ticket* mdl_ticket = nullptr;
	user_table = dict_table_open_on_id(
		innodb_ft_aux_table_id, false, DICT_TABLE_OP_NORMAL,
		thd, &mdl_ticket);

	if (!user_table) {
		DBUG_RETURN(0);
	}

	if (!dict_table_has_fts_index(user_table)) {
		dict_table_close(user_table, false, thd, mdl_ticket);
		DBUG_RETURN(0);
	}

	fields = table->field;

	trx = trx_create();
	trx->op_info = "Select for FTS CONFIG TABLE";

	FTS_INIT_FTS_TABLE(&fts_table, "CONFIG", FTS_COMMON_TABLE, user_table);

	if (!ib_vector_is_empty(user_table->fts->indexes)) {
		index = (dict_index_t*) ib_vector_getp_const(
				user_table->fts->indexes, 0);
		DBUG_ASSERT(!dict_index_is_online_ddl(index));
	}

	int	ret = 0;

	while (fts_config_key[i]) {
		fts_string_t	value;
		char*		key_name;
		ulint		allocated = FALSE;

		value.f_len = FTS_MAX_CONFIG_VALUE_LEN;

		value.f_str = str;

		if (index
		    && strcmp(fts_config_key[i], FTS_TOTAL_WORD_COUNT) == 0) {
			key_name = fts_config_create_index_param_name(
				fts_config_key[i], index);
			allocated = TRUE;
		} else {
			key_name = (char*) fts_config_key[i];
		}

		fts_config_get_value(trx, &fts_table, key_name, &value);

		if (allocated) {
			ut_free(key_name);
		}

		BREAK_IF(ret = field_store_string(
				 fields[FTS_CONFIG_KEY], fts_config_key[i]));

		BREAK_IF(ret = field_store_string(
				 fields[FTS_CONFIG_VALUE],
				 reinterpret_cast<const char*>(value.f_str)));

		BREAK_IF(ret = schema_table_store_record(thd, table));

		i++;
	}

	fts_sql_commit(trx);

	dict_table_close(user_table, false, thd, mdl_ticket);

	trx->free();

	DBUG_RETURN(ret);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_FT_CONFIG
@return 0 on success */
static
int
i_s_fts_config_init(
/*=================*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_fts_config_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::i_s_fts_config_fields_info;
	schema->fill_table = i_s_fts_config_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_ft_config =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_FT_CONFIG",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"INNODB AUXILIARY FTS CONFIG TABLE",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_fts_config_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/* Fields of the dynamic table INNODB_BUFFER_POOL_STATS. */
static ST_FIELD_INFO i_s_innodb_buffer_stats_fields_info[]=
{
#define IDX_BUF_STATS_POOL_ID		0
  Column("POOL_ID", ULong(), NOT_NULL),

#define IDX_BUF_STATS_POOL_SIZE		1
  Column("POOL_SIZE", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_FREE_BUFFERS	2
  Column("FREE_BUFFERS", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_LRU_LEN		3
  Column("DATABASE_PAGES", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_OLD_LRU_LEN	4
  Column("OLD_DATABASE_PAGES", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_FLUSH_LIST_LEN	5
  Column("MODIFIED_DATABASE_PAGES", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_PENDING_ZIP	6
  Column("PENDING_DECOMPRESS", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_PENDING_READ	7
  Column("PENDING_READS",ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_FLUSH_LRU		8
  Column("PENDING_FLUSH_LRU",ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_FLUSH_LIST	9
  Column("PENDING_FLUSH_LIST", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_PAGE_YOUNG	10
  Column("PAGES_MADE_YOUNG",ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_PAGE_NOT_YOUNG	11
  Column("PAGES_NOT_MADE_YOUNG",ULonglong(), NOT_NULL),

#define	IDX_BUF_STATS_PAGE_YOUNG_RATE	12
  Column("PAGES_MADE_YOUNG_RATE", Float(MAX_FLOAT_STR_LENGTH), NOT_NULL),

#define	IDX_BUF_STATS_PAGE_NOT_YOUNG_RATE 13
  Column("PAGES_MADE_NOT_YOUNG_RATE", Float(MAX_FLOAT_STR_LENGTH), NOT_NULL),

#define IDX_BUF_STATS_PAGE_READ		14
  Column("NUMBER_PAGES_READ",ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_PAGE_CREATED	15
  Column("NUMBER_PAGES_CREATED",ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_PAGE_WRITTEN	16
  Column("NUMBER_PAGES_WRITTEN",ULonglong(), NOT_NULL),

#define	IDX_BUF_STATS_PAGE_READ_RATE	17
  Column("PAGES_READ_RATE", Float(MAX_FLOAT_STR_LENGTH), NOT_NULL),

#define	IDX_BUF_STATS_PAGE_CREATE_RATE	18
  Column("PAGES_CREATE_RATE", Float(MAX_FLOAT_STR_LENGTH), NOT_NULL),

#define	IDX_BUF_STATS_PAGE_WRITTEN_RATE	19
  Column("PAGES_WRITTEN_RATE",Float(MAX_FLOAT_STR_LENGTH), NOT_NULL),

#define IDX_BUF_STATS_GET		20
  Column("NUMBER_PAGES_GET", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_HIT_RATE		21
  Column("HIT_RATE", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_MADE_YOUNG_PCT	22
  Column("YOUNG_MAKE_PER_THOUSAND_GETS", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_NOT_MADE_YOUNG_PCT 23
  Column("NOT_YOUNG_MAKE_PER_THOUSAND_GETS", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_READ_AHEAD	24
  Column("NUMBER_PAGES_READ_AHEAD", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_READ_AHEAD_EVICTED 25
  Column("NUMBER_READ_AHEAD_EVICTED", ULonglong(), NOT_NULL),

#define	IDX_BUF_STATS_READ_AHEAD_RATE	26
  Column("READ_AHEAD_RATE", Float(MAX_FLOAT_STR_LENGTH), NOT_NULL),

#define	IDX_BUF_STATS_READ_AHEAD_EVICT_RATE 27
  Column("READ_AHEAD_EVICTED_RATE",Float(MAX_FLOAT_STR_LENGTH), NOT_NULL),

#define IDX_BUF_STATS_LRU_IO_SUM	28
  Column("LRU_IO_TOTAL", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_LRU_IO_CUR	29
  Column("LRU_IO_CURRENT", ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_UNZIP_SUM		30
  Column("UNCOMPRESS_TOTAL",ULonglong(), NOT_NULL),

#define IDX_BUF_STATS_UNZIP_CUR		31
  Column("UNCOMPRESS_CURRENT", ULonglong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/** Fill INFORMATION_SCHEMA.INNODB_BUFFER_POOL_STATS
@param[in,out]	thd	connection
@param[in,out]	tables	tables to fill
@return 0 on success, 1 on failure */
static int i_s_innodb_stats_fill(THD *thd, TABLE_LIST * tables, Item *)
{
	TABLE*		table;
	Field**		fields;
	buf_pool_info_t	info;

	DBUG_ENTER("i_s_innodb_stats_fill");

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* Only allow the PROCESS privilege holder to access the stats */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	buf_stats_get_pool_info(&info);

	table = tables->table;

	fields = table->field;

	OK(fields[IDX_BUF_STATS_POOL_ID]->store(0, true));

	OK(fields[IDX_BUF_STATS_POOL_SIZE]->store(info.pool_size, true));

	OK(fields[IDX_BUF_STATS_LRU_LEN]->store(info.lru_len, true));

	OK(fields[IDX_BUF_STATS_OLD_LRU_LEN]->store(info.old_lru_len, true));

	OK(fields[IDX_BUF_STATS_FREE_BUFFERS]->store(
		   info.free_list_len, true));

	OK(fields[IDX_BUF_STATS_FLUSH_LIST_LEN]->store(
		   info.flush_list_len, true));

	OK(fields[IDX_BUF_STATS_PENDING_ZIP]->store(info.n_pend_unzip, true));

	OK(fields[IDX_BUF_STATS_PENDING_READ]->store(info.n_pend_reads, true));

	OK(fields[IDX_BUF_STATS_FLUSH_LRU]->store(
		   info.n_pending_flush_lru, true));

	OK(fields[IDX_BUF_STATS_FLUSH_LIST]->store(
		   info.n_pending_flush_list, true));

	OK(fields[IDX_BUF_STATS_PAGE_YOUNG]->store(
		   info.n_pages_made_young, true));

	OK(fields[IDX_BUF_STATS_PAGE_NOT_YOUNG]->store(
		   info.n_pages_not_made_young, true));

	OK(fields[IDX_BUF_STATS_PAGE_YOUNG_RATE]->store(
		   info.page_made_young_rate));

	OK(fields[IDX_BUF_STATS_PAGE_NOT_YOUNG_RATE]->store(
		   info.page_not_made_young_rate));

	OK(fields[IDX_BUF_STATS_PAGE_READ]->store(info.n_pages_read, true));

	OK(fields[IDX_BUF_STATS_PAGE_CREATED]->store(
		   info.n_pages_created, true));

	OK(fields[IDX_BUF_STATS_PAGE_WRITTEN]->store(
		   info.n_pages_written, true));

	OK(fields[IDX_BUF_STATS_GET]->store(info.n_page_gets, true));

	OK(fields[IDX_BUF_STATS_PAGE_READ_RATE]->store(
		   info.pages_read_rate));

	OK(fields[IDX_BUF_STATS_PAGE_CREATE_RATE]->store(
		   info.pages_created_rate));

	OK(fields[IDX_BUF_STATS_PAGE_WRITTEN_RATE]->store(
		   info.pages_written_rate));

	if (info.n_page_get_delta) {
		if (info.page_read_delta <= info.n_page_get_delta) {
			OK(fields[IDX_BUF_STATS_HIT_RATE]->store(
				static_cast<double>(
					1000 - (1000 * info.page_read_delta
					/ info.n_page_get_delta))));
		} else {
			OK(fields[IDX_BUF_STATS_HIT_RATE]->store(0));
		}

		OK(fields[IDX_BUF_STATS_MADE_YOUNG_PCT]->store(
			   1000 * info.young_making_delta
			   / info.n_page_get_delta, true));

		OK(fields[IDX_BUF_STATS_NOT_MADE_YOUNG_PCT]->store(
			   1000 * info.not_young_making_delta
			   / info.n_page_get_delta, true));
	} else {
		OK(fields[IDX_BUF_STATS_HIT_RATE]->store(0, true));
		OK(fields[IDX_BUF_STATS_MADE_YOUNG_PCT]->store(0, true));
		OK(fields[IDX_BUF_STATS_NOT_MADE_YOUNG_PCT]->store(0, true));
	}

	OK(fields[IDX_BUF_STATS_READ_AHEAD]->store(
		   info.n_ra_pages_read, true));

	OK(fields[IDX_BUF_STATS_READ_AHEAD_EVICTED]->store(
		   info.n_ra_pages_evicted, true));

	OK(fields[IDX_BUF_STATS_READ_AHEAD_RATE]->store(
		   info.pages_readahead_rate));

	OK(fields[IDX_BUF_STATS_READ_AHEAD_EVICT_RATE]->store(
		   info.pages_evicted_rate));

	OK(fields[IDX_BUF_STATS_LRU_IO_SUM]->store(info.io_sum, true));

	OK(fields[IDX_BUF_STATS_LRU_IO_CUR]->store(info.io_cur, true));

	OK(fields[IDX_BUF_STATS_UNZIP_SUM]->store(info.unzip_sum, true));

	OK(fields[IDX_BUF_STATS_UNZIP_CUR]->store(info.unzip_cur, true));

	DBUG_RETURN(schema_table_store_record(thd, table));
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_BUFFER_POOL_STATS.
@return 0 on success, 1 on failure */
static
int
i_s_innodb_buffer_pool_stats_init(
/*==============================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("i_s_innodb_buffer_pool_stats_init");

	schema = reinterpret_cast<ST_SCHEMA_TABLE*>(p);

	schema->fields_info = Show::i_s_innodb_buffer_stats_fields_info;
	schema->fill_table = i_s_innodb_stats_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_buffer_stats =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_BUFFER_POOL_STATS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB Buffer Pool Statistics Information ",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_innodb_buffer_pool_stats_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

/** These must correspond to the first values of buf_page_state */
static const LEX_CSTRING page_state_values[] =
{
  { STRING_WITH_LEN("NOT_USED") },
  { STRING_WITH_LEN("MEMORY") },
  { STRING_WITH_LEN("REMOVE_HASH") },
  { STRING_WITH_LEN("FILE_PAGE") },
};

static const TypelibBuffer<4> page_state_values_typelib(page_state_values);

static const LEX_CSTRING io_values[] =
{
	{ STRING_WITH_LEN("IO_NONE") },
	{ STRING_WITH_LEN("IO_READ") },
	{ STRING_WITH_LEN("IO_WRITE") }
};


static TypelibBuffer<3> io_values_typelib(io_values);

namespace Show {
/* Fields of the dynamic table INNODB_BUFFER_POOL_PAGE. */
static ST_FIELD_INFO i_s_innodb_buffer_page_fields_info[]=
{
#define IDX_BUFFER_POOL_ID		0
  Column("POOL_ID", ULong(), NOT_NULL),

#define IDX_BUFFER_BLOCK_ID		1
  Column("BLOCK_ID", ULonglong(), NOT_NULL),

#define IDX_BUFFER_PAGE_SPACE		2
  Column("SPACE", ULong(), NOT_NULL),

#define IDX_BUFFER_PAGE_NUM		3
  Column("PAGE_NUMBER", ULong(), NOT_NULL),

#define IDX_BUFFER_PAGE_TYPE		4
  Column("PAGE_TYPE", Varchar(64), NULLABLE),

#define IDX_BUFFER_PAGE_FLUSH_TYPE	5
  Column("FLUSH_TYPE", ULong(), NOT_NULL),

#define IDX_BUFFER_PAGE_FIX_COUNT	6
  Column("FIX_COUNT", ULong(), NOT_NULL),

#ifdef BTR_CUR_HASH_ADAPT
#define IDX_BUFFER_PAGE_HASHED		7
  Column("IS_HASHED", SLong(1), NOT_NULL),
#endif /* BTR_CUR_HASH_ADAPT */
#define IDX_BUFFER_PAGE_NEWEST_MOD	7 + I_S_AHI
  Column("NEWEST_MODIFICATION", ULonglong(), NOT_NULL),

#define IDX_BUFFER_PAGE_OLDEST_MOD	8 + I_S_AHI
  Column("OLDEST_MODIFICATION", ULonglong(), NOT_NULL),

#define IDX_BUFFER_PAGE_ACCESS_TIME	9 + I_S_AHI
  Column("ACCESS_TIME", ULonglong(), NOT_NULL),

#define IDX_BUFFER_PAGE_TABLE_NAME	10 + I_S_AHI
  Column("TABLE_NAME", Varchar(1024), NULLABLE),

#define IDX_BUFFER_PAGE_INDEX_NAME	11 + I_S_AHI
  Column("INDEX_NAME", Varchar(NAME_CHAR_LEN), NULLABLE),

#define IDX_BUFFER_PAGE_NUM_RECS	12 + I_S_AHI
  Column("NUMBER_RECORDS", ULonglong(), NOT_NULL),

#define IDX_BUFFER_PAGE_DATA_SIZE	13 + I_S_AHI
  Column("DATA_SIZE", ULonglong(), NOT_NULL),

#define IDX_BUFFER_PAGE_ZIP_SIZE	14 + I_S_AHI
  Column("COMPRESSED_SIZE", ULonglong(), NOT_NULL),

#define IDX_BUFFER_PAGE_STATE		15 + I_S_AHI
  Column("PAGE_STATE", Enum(&page_state_values_typelib), NOT_NULL),

#define IDX_BUFFER_PAGE_IO_FIX		16 + I_S_AHI
  Column("IO_FIX", Enum(&io_values_typelib), NOT_NULL),

#define IDX_BUFFER_PAGE_IS_OLD		17 + I_S_AHI
  Column("IS_OLD", SLong(1), NOT_NULL),

#define IDX_BUFFER_PAGE_FREE_CLOCK	18 + I_S_AHI
  Column("FREE_PAGE_CLOCK", ULonglong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/*******************************************************************//**
Fill Information Schema table INNODB_BUFFER_PAGE with information
cached in the buf_page_info_t array
@return 0 on success, 1 on failure */
static
int
i_s_innodb_buffer_page_fill(
/*========================*/
	THD*			thd,		/*!< in: thread */
	TABLE_LIST*		tables,		/*!< in/out: tables to fill */
	const buf_page_info_t*	info_array,	/*!< in: array cached page
						info */
	ulint			num_page)	/*!< in: number of page info
						cached */
{
	TABLE*			table;
	Field**			fields;

	compile_time_assert(I_S_PAGE_TYPE_LAST < 1 << I_S_PAGE_TYPE_BITS);

	DBUG_ENTER("i_s_innodb_buffer_page_fill");

	table = tables->table;

	fields = table->field;

	/* Iterate through the cached array and fill the I_S table rows */
	for (ulint i = 0; i < num_page; i++) {
		const buf_page_info_t*	page_info;
		char			table_name[MAX_FULL_NAME_LEN + 1];
		const char*		table_name_end = NULL;

		page_info = info_array + i;

		OK(fields[IDX_BUFFER_POOL_ID]->store(0, true));

		OK(fields[IDX_BUFFER_BLOCK_ID]->store(
			   page_info->block_id, true));

		OK(fields[IDX_BUFFER_PAGE_SPACE]->store(
			   page_info->id.space(), true));

		OK(fields[IDX_BUFFER_PAGE_NUM]->store(
			   page_info->id.page_no(), true));

		OK(field_store_string(
			   fields[IDX_BUFFER_PAGE_TYPE],
			   i_s_page_type[page_info->page_type].type_str));

		OK(fields[IDX_BUFFER_PAGE_FLUSH_TYPE]->store(0, true));

		OK(fields[IDX_BUFFER_PAGE_FIX_COUNT]->store(
			   ~buf_page_t::LRU_MASK & page_info->state, true));

#ifdef BTR_CUR_HASH_ADAPT
		OK(fields[IDX_BUFFER_PAGE_HASHED]->store(
			   page_info->hashed, true));
#endif /* BTR_CUR_HASH_ADAPT */

		OK(fields[IDX_BUFFER_PAGE_NEWEST_MOD]->store(
			   page_info->newest_mod, true));

		OK(fields[IDX_BUFFER_PAGE_OLDEST_MOD]->store(
			   page_info->oldest_mod, true));

		OK(fields[IDX_BUFFER_PAGE_ACCESS_TIME]->store(
			   page_info->access_time, true));

		fields[IDX_BUFFER_PAGE_TABLE_NAME]->set_null();

		fields[IDX_BUFFER_PAGE_INDEX_NAME]->set_null();

		/* If this is an index page, fetch the index name
		and table name */
		if (page_info->page_type == I_S_PAGE_TYPE_INDEX) {
			bool ret = false;

			dict_sys.freeze(SRW_LOCK_CALL);

			const dict_index_t* index =
				dict_index_get_if_in_cache_low(
					page_info->index_id);

			if (index) {
				table_name_end = innobase_convert_name(
					table_name, sizeof(table_name),
					index->table->name.m_name,
					strlen(index->table->name.m_name),
					thd);

				ret = fields[IDX_BUFFER_PAGE_TABLE_NAME]
					->store(table_name,
						static_cast<uint>(
							table_name_end
							- table_name),
						system_charset_info)
					|| fields[IDX_BUFFER_PAGE_INDEX_NAME]
					->store(index->name,
						uint(strlen(index->name)),
						system_charset_info);
			}

			dict_sys.unfreeze();

			OK(ret);

			if (index) {
				fields[IDX_BUFFER_PAGE_TABLE_NAME]
					->set_notnull();
				fields[IDX_BUFFER_PAGE_INDEX_NAME]
					->set_notnull();
			}
		}

		OK(fields[IDX_BUFFER_PAGE_NUM_RECS]->store(
			   page_info->num_recs, true));

		OK(fields[IDX_BUFFER_PAGE_DATA_SIZE]->store(
			   page_info->data_size, true));

		OK(fields[IDX_BUFFER_PAGE_ZIP_SIZE]->store(
			   page_info->zip_ssize
			   ? (UNIV_ZIP_SIZE_MIN >> 1) << page_info->zip_ssize
			   : 0, true));

		static_assert(buf_page_t::NOT_USED == 0, "compatibility");
		static_assert(buf_page_t::MEMORY == 1, "compatibility");
		static_assert(buf_page_t::REMOVE_HASH == 2, "compatibility");

		OK(fields[IDX_BUFFER_PAGE_STATE]->store(
			   std::min<uint32_t>(3, page_info->state) + 1, true));

		static_assert(buf_page_t::UNFIXED == 1U << 29, "comp.");
		static_assert(buf_page_t::READ_FIX == 4U << 29, "comp.");
		static_assert(buf_page_t::WRITE_FIX == 5U << 29, "comp.");

		unsigned io_fix = page_info->state >> 29;
		if (io_fix < 4) {
			io_fix = 1;
		} else if (io_fix > 5) {
			io_fix = 3;
		} else {
			io_fix -= 2;
		}

		OK(fields[IDX_BUFFER_PAGE_IO_FIX]->store(io_fix, true));

		OK(fields[IDX_BUFFER_PAGE_IS_OLD]->store(
			   page_info->is_old, true));

		OK(fields[IDX_BUFFER_PAGE_FREE_CLOCK]->store(
			   page_info->freed_page_clock, true));

		OK(schema_table_store_record(thd, table));
	}

	DBUG_RETURN(0);
}

/*******************************************************************//**
Set appropriate page type to a buf_page_info_t structure */
static
void
i_s_innodb_set_page_type(
/*=====================*/
	buf_page_info_t*page_info,	/*!< in/out: structure to fill with
					scanned info */
	const byte*	frame)		/*!< in: buffer frame */
{
	uint16_t page_type = fil_page_get_type(frame);

	if (fil_page_type_is_index(page_type)) {
		const page_t*	page = (const page_t*) frame;

		page_info->index_id = btr_page_get_index_id(page);

		/* FIL_PAGE_INDEX and FIL_PAGE_RTREE are a bit special,
		their values are defined as 17855 and 17854, so we cannot
		use them to index into i_s_page_type[] array, its array index
		in the i_s_page_type[] array is I_S_PAGE_TYPE_INDEX
		(1) for index pages or I_S_PAGE_TYPE_IBUF for
		change buffer index pages */
		if (page_type == FIL_PAGE_RTREE) {
			page_info->page_type = I_S_PAGE_TYPE_RTREE;
		} else if (page_info->index_id
			   == static_cast<index_id_t>(DICT_IBUF_ID_MIN
						      + IBUF_SPACE_ID)) {
			page_info->page_type = I_S_PAGE_TYPE_IBUF;
		} else {
			ut_ad(page_type == FIL_PAGE_INDEX
			      || page_type == FIL_PAGE_TYPE_INSTANT);
			page_info->page_type = I_S_PAGE_TYPE_INDEX;
		}

		page_info->data_size = uint16_t(page_header_get_field(
			page, PAGE_HEAP_TOP) - (page_is_comp(page)
						? PAGE_NEW_SUPREMUM_END
						: PAGE_OLD_SUPREMUM_END)
			- page_header_get_field(page, PAGE_GARBAGE));

		page_info->num_recs = page_get_n_recs(page) & ((1U << 14) - 1);
	} else if (page_type > FIL_PAGE_TYPE_LAST) {
		/* Encountered an unknown page type */
		page_info->page_type = I_S_PAGE_TYPE_UNKNOWN;
	} else {
		/* Make sure we get the right index into the
		i_s_page_type[] array */
		ut_a(page_type == i_s_page_type[page_type].type_value);

		page_info->page_type = page_type & 0xf;
	}
}
/*******************************************************************//**
Scans pages in the buffer cache, and collect their general information
into the buf_page_info_t array which is zero-filled. So any fields
that are not initialized in the function will default to 0 */
static
void
i_s_innodb_buffer_page_get_info(
/*============================*/
	const buf_page_t*bpage,		/*!< in: buffer pool page to scan */
	ulint		pos,		/*!< in: buffer block position in
					buffer pool or in the LRU list */
	buf_page_info_t*page_info)	/*!< in: zero filled info structure;
					out: structure filled with scanned
					info */
{
	page_info->block_id = pos;

	static_assert(buf_page_t::NOT_USED == 0, "compatibility");
	static_assert(buf_page_t::MEMORY == 1, "compatibility");
	static_assert(buf_page_t::REMOVE_HASH == 2, "compatibility");
	static_assert(buf_page_t::UNFIXED == 1U << 29, "compatibility");
	static_assert(buf_page_t::READ_FIX == 4U << 29, "compatibility");
	static_assert(buf_page_t::WRITE_FIX == 5U << 29, "compatibility");

	page_info->state = bpage->state();

	if (page_info->state < buf_page_t::FREED) {
		page_info->page_type = I_S_PAGE_TYPE_UNKNOWN;
		page_info->compressed_only = false;
	} else {
		const byte*	frame;

		page_info->id = bpage->id();

		page_info->oldest_mod = bpage->oldest_modification();

		page_info->access_time = bpage->access_time;

		page_info->zip_ssize = bpage->zip.ssize;

		page_info->is_old = bpage->old;

		page_info->freed_page_clock = bpage->freed_page_clock;

		if (page_info->state >= buf_page_t::READ_FIX
		    && page_info->state < buf_page_t::WRITE_FIX) {
			page_info->page_type = I_S_PAGE_TYPE_UNKNOWN;
			page_info->newest_mod = 0;
			return;
		}

		page_info->compressed_only = !bpage->frame,
		frame = bpage->frame;
		if (UNIV_LIKELY(frame != nullptr)) {
#ifdef BTR_CUR_HASH_ADAPT
			/* Note: this may be a false positive, that
			is, block->index will not always be set to
			NULL when the last adaptive hash index
			reference is dropped. */
			page_info->hashed =
				reinterpret_cast<const buf_block_t*>(bpage)
				->index != nullptr;
#endif /* BTR_CUR_HASH_ADAPT */
		} else {
			ut_ad(page_info->zip_ssize);
			frame = bpage->zip.data;
		}

		page_info->newest_mod = mach_read_from_8(FIL_PAGE_LSN + frame);
		i_s_innodb_set_page_type(page_info, frame);
	}
}

/*******************************************************************//**
This is the function that goes through each block of the buffer pool
and fetch information to information schema tables: INNODB_BUFFER_PAGE.
@param[in,out]	thd	connection
@param[in,out]	tables	tables to fill
@return 0 on success, 1 on failure */
static int i_s_innodb_buffer_page_fill(THD *thd, TABLE_LIST *tables, Item *)
{
	int			status	= 0;
	mem_heap_t*		heap;

	DBUG_ENTER("i_s_innodb_buffer_page_fill");

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS privilege */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	heap = mem_heap_create(10000);

	for (ulint n = 0;
	     n < ut_min(buf_pool.n_chunks, buf_pool.n_chunks_new); n++) {
		const buf_block_t*	block;
		ulint			n_blocks;
		buf_page_info_t*	info_buffer;
		ulint			num_page;
		ulint			mem_size;
		ulint			chunk_size;
		ulint			num_to_process = 0;
		ulint			block_id = 0;

		/* Get buffer block of the nth chunk */
		block = buf_pool.chunks[n].blocks;
		chunk_size = buf_pool.chunks[n].size;
		num_page = 0;

		while (chunk_size > 0) {
			/* we cache maximum MAX_BUF_INFO_CACHED number of
			buffer page info */
			num_to_process = ut_min(chunk_size,
				(ulint)MAX_BUF_INFO_CACHED);

			mem_size = num_to_process * sizeof(buf_page_info_t);

			/* For each chunk, we'll pre-allocate information
			structures to cache the page information read from
			the buffer pool. Doing so before obtain any mutex */
			info_buffer = (buf_page_info_t*) mem_heap_zalloc(
				heap, mem_size);

			/* Obtain appropriate mutexes. Since this is diagnostic
			buffer pool info printout, we are not required to
			preserve the overall consistency, so we can
			release mutex periodically */
			mysql_mutex_lock(&buf_pool.mutex);

			/* GO through each block in the chunk */
			for (n_blocks = num_to_process; n_blocks--; block++) {
				i_s_innodb_buffer_page_get_info(
					&block->page, block_id,
					info_buffer + num_page);
				block_id++;
				num_page++;
			}

			mysql_mutex_unlock(&buf_pool.mutex);

			/* Fill in information schema table with information
			just collected from the buffer chunk scan */
			status = i_s_innodb_buffer_page_fill(
				thd, tables, info_buffer,
				num_page);

			/* If something goes wrong, break and return */
			if (status) {
				break;
			}

			mem_heap_empty(heap);
			chunk_size -= num_to_process;
			num_page = 0;
		}
	}

	mem_heap_free(heap);

	DBUG_RETURN(status);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_BUFFER_PAGE.
@return 0 on success, 1 on failure */
static
int
i_s_innodb_buffer_page_init(
/*========================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("i_s_innodb_buffer_page_init");

	schema = reinterpret_cast<ST_SCHEMA_TABLE*>(p);

	schema->fields_info = Show::i_s_innodb_buffer_page_fields_info;
	schema->fill_table = i_s_innodb_buffer_page_fill;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_buffer_page =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_BUFFER_PAGE",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB Buffer Page Information",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_innodb_buffer_page_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
static ST_FIELD_INFO i_s_innodb_buf_page_lru_fields_info[] =
{
#define IDX_BUF_LRU_POOL_ID		0
  Column("POOL_ID", ULong(), NOT_NULL),

#define IDX_BUF_LRU_POS			1
  Column("LRU_POSITION", ULonglong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_SPACE		2
  Column("SPACE", ULong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_NUM		3
  Column("PAGE_NUMBER", ULong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_TYPE		4
  Column("PAGE_TYPE", Varchar(64), NULLABLE),

#define IDX_BUF_LRU_PAGE_FLUSH_TYPE	5
  Column("FLUSH_TYPE", ULong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_FIX_COUNT	6
  Column("FIX_COUNT", ULong(), NOT_NULL),

#ifdef BTR_CUR_HASH_ADAPT
#define IDX_BUF_LRU_PAGE_HASHED		7
  Column("IS_HASHED", SLong(1), NOT_NULL),
#endif /* BTR_CUR_HASH_ADAPT */
#define IDX_BUF_LRU_PAGE_NEWEST_MOD	7 + I_S_AHI
  Column("NEWEST_MODIFICATION",ULonglong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_OLDEST_MOD	8 + I_S_AHI
  Column("OLDEST_MODIFICATION",ULonglong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_ACCESS_TIME	9 + I_S_AHI
  Column("ACCESS_TIME",ULonglong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_TABLE_NAME	10 + I_S_AHI
  Column("TABLE_NAME", Varchar(1024), NULLABLE),

#define IDX_BUF_LRU_PAGE_INDEX_NAME	11 + I_S_AHI
  Column("INDEX_NAME", Varchar(NAME_CHAR_LEN), NULLABLE),

#define IDX_BUF_LRU_PAGE_NUM_RECS	12 + I_S_AHI
  Column("NUMBER_RECORDS", ULonglong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_DATA_SIZE	13 + I_S_AHI
  Column("DATA_SIZE", ULonglong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_ZIP_SIZE	14 + I_S_AHI
  Column("COMPRESSED_SIZE",ULonglong(), NOT_NULL),

#define IDX_BUF_LRU_PAGE_STATE		15 + I_S_AHI
  Column("COMPRESSED", SLong(1), NOT_NULL),

#define IDX_BUF_LRU_PAGE_IO_FIX		16 + I_S_AHI
  Column("IO_FIX", Enum(&io_values_typelib), NOT_NULL),

#define IDX_BUF_LRU_PAGE_IS_OLD		17 + I_S_AHI
  Column("IS_OLD", SLong(1), NULLABLE),

#define IDX_BUF_LRU_PAGE_FREE_CLOCK	18 + I_S_AHI
  Column("FREE_PAGE_CLOCK", ULonglong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/*******************************************************************//**
Fill Information Schema table INNODB_BUFFER_PAGE_LRU with information
cached in the buf_page_info_t array
@return 0 on success, 1 on failure */
static
int
i_s_innodb_buf_page_lru_fill(
/*=========================*/
	THD*			thd,		/*!< in: thread */
	TABLE_LIST*		tables,		/*!< in/out: tables to fill */
	const buf_page_info_t*	info_array,	/*!< in: array cached page
						info */
	ulint			num_page)	/*!< in: number of page info
						 cached */
{
	DBUG_ENTER("i_s_innodb_buf_page_lru_fill");

	TABLE*	table	= tables->table;
	Field**	fields	= table->field;

	/* Iterate through the cached array and fill the I_S table rows */
	for (ulint i = 0; i < num_page; i++) {
		const buf_page_info_t*	page_info;
		char			table_name[MAX_FULL_NAME_LEN + 1];
		const char*		table_name_end = NULL;

		page_info = info_array + i;

		OK(fields[IDX_BUF_LRU_POOL_ID]->store(0, true));

		OK(fields[IDX_BUF_LRU_POS]->store(
			   page_info->block_id, true));

		OK(fields[IDX_BUF_LRU_PAGE_SPACE]->store(
			   page_info->id.space(), true));

		OK(fields[IDX_BUF_LRU_PAGE_NUM]->store(
			   page_info->id.page_no(), true));

		OK(field_store_string(
			   fields[IDX_BUF_LRU_PAGE_TYPE],
			   i_s_page_type[page_info->page_type].type_str));

		OK(fields[IDX_BUF_LRU_PAGE_FLUSH_TYPE]->store(0, true));

		OK(fields[IDX_BUF_LRU_PAGE_FIX_COUNT]->store(
			   ~buf_page_t::LRU_MASK & page_info->state, true));

#ifdef BTR_CUR_HASH_ADAPT
		OK(fields[IDX_BUF_LRU_PAGE_HASHED]->store(
			   page_info->hashed, true));
#endif /* BTR_CUR_HASH_ADAPT */

		OK(fields[IDX_BUF_LRU_PAGE_NEWEST_MOD]->store(
			   page_info->newest_mod, true));

		OK(fields[IDX_BUF_LRU_PAGE_OLDEST_MOD]->store(
			   page_info->oldest_mod, true));

		OK(fields[IDX_BUF_LRU_PAGE_ACCESS_TIME]->store(
			   page_info->access_time, true));

		fields[IDX_BUF_LRU_PAGE_TABLE_NAME]->set_null();

		fields[IDX_BUF_LRU_PAGE_INDEX_NAME]->set_null();

		/* If this is an index page, fetch the index name
		and table name */
		if (page_info->page_type == I_S_PAGE_TYPE_INDEX) {
			bool ret = false;

			dict_sys.freeze(SRW_LOCK_CALL);

			const dict_index_t* index =
				dict_index_get_if_in_cache_low(
					page_info->index_id);

			if (index) {
				table_name_end = innobase_convert_name(
					table_name, sizeof(table_name),
					index->table->name.m_name,
					strlen(index->table->name.m_name),
					thd);

				ret = fields[IDX_BUF_LRU_PAGE_TABLE_NAME]
					->store(table_name,
						static_cast<uint>(
							table_name_end
							- table_name),
						system_charset_info)
					|| fields[IDX_BUF_LRU_PAGE_INDEX_NAME]
					->store(index->name,
						uint(strlen(index->name)),
						system_charset_info);
			}

			dict_sys.unfreeze();

			OK(ret);

			if (index) {
				fields[IDX_BUF_LRU_PAGE_TABLE_NAME]
					->set_notnull();
				fields[IDX_BUF_LRU_PAGE_INDEX_NAME]
					->set_notnull();
			}
		}

		OK(fields[IDX_BUF_LRU_PAGE_NUM_RECS]->store(
			   page_info->num_recs, true));

		OK(fields[IDX_BUF_LRU_PAGE_DATA_SIZE]->store(
			   page_info->data_size, true));

		OK(fields[IDX_BUF_LRU_PAGE_ZIP_SIZE]->store(
			   page_info->zip_ssize
			   ? 512 << page_info->zip_ssize : 0, true));

		OK(fields[IDX_BUF_LRU_PAGE_STATE]->store(
			   page_info->compressed_only, true));

		static_assert(buf_page_t::UNFIXED == 1U << 29, "comp.");
		static_assert(buf_page_t::READ_FIX == 4U << 29, "comp.");
		static_assert(buf_page_t::WRITE_FIX == 5U << 29, "comp.");

		unsigned io_fix = page_info->state >> 29;
		if (io_fix < 4) {
			io_fix = 1;
		} else if (io_fix > 5) {
			io_fix = 3;
		} else {
			io_fix -= 2;
		}

		OK(fields[IDX_BUF_LRU_PAGE_IO_FIX]->store(io_fix, true));

		OK(fields[IDX_BUF_LRU_PAGE_IS_OLD]->store(
			   page_info->is_old, true));

		OK(fields[IDX_BUF_LRU_PAGE_FREE_CLOCK]->store(
			   page_info->freed_page_clock, true));

		OK(schema_table_store_record(thd, table));
	}

	DBUG_RETURN(0);
}

/** Fill the table INFORMATION_SCHEMA.INNODB_BUFFER_PAGE_LRU.
@param[in]	thd		thread
@param[in,out]	tables		tables to fill
@return 0 on success, 1 on failure */
static int i_s_innodb_fill_buffer_lru(THD *thd, TABLE_LIST *tables, Item *)
{
	int			status = 0;
	buf_page_info_t*	info_buffer;
	ulint			lru_pos = 0;
	const buf_page_t*	bpage;
	ulint			lru_len;

	DBUG_ENTER("i_s_innodb_fill_buffer_lru");

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to any users that do not hold PROCESS_ACL */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	/* Aquire the mutex before allocating info_buffer, since
	UT_LIST_GET_LEN(buf_pool.LRU) could change */
	mysql_mutex_lock(&buf_pool.mutex);

	lru_len = UT_LIST_GET_LEN(buf_pool.LRU);

	/* Print error message if malloc fail */
	info_buffer = (buf_page_info_t*) my_malloc(PSI_INSTRUMENT_ME,
		lru_len * sizeof *info_buffer, MYF(MY_WME | MY_ZEROFILL));

	if (!info_buffer) {
		status = 1;
		goto exit;
	}

	/* Walk through Pool's LRU list and print the buffer page
	information */
	bpage = UT_LIST_GET_LAST(buf_pool.LRU);

	while (bpage != NULL) {
		/* Use the same function that collect buffer info for
		INNODB_BUFFER_PAGE to get buffer page info */
		i_s_innodb_buffer_page_get_info(bpage, lru_pos,
						(info_buffer + lru_pos));

		bpage = UT_LIST_GET_PREV(LRU, bpage);

		lru_pos++;
	}

	ut_ad(lru_pos == lru_len);
	ut_ad(lru_pos == UT_LIST_GET_LEN(buf_pool.LRU));

exit:
	mysql_mutex_unlock(&buf_pool.mutex);

	if (info_buffer) {
		status = i_s_innodb_buf_page_lru_fill(
			thd, tables, info_buffer, lru_len);

		my_free(info_buffer);
	}

	DBUG_RETURN(status);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_BUFFER_PAGE_LRU.
@return 0 on success, 1 on failure */
static
int
i_s_innodb_buffer_page_lru_init(
/*============================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("i_s_innodb_buffer_page_lru_init");

	schema = reinterpret_cast<ST_SCHEMA_TABLE*>(p);

	schema->fields_info = Show::i_s_innodb_buf_page_lru_fields_info;
	schema->fill_table = i_s_innodb_fill_buffer_lru;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_buffer_page_lru =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_BUFFER_PAGE_LRU",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB Buffer Page in LRU",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	i_s_innodb_buffer_page_lru_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

/*******************************************************************//**
Unbind a dynamic INFORMATION_SCHEMA table.
@return 0 */
static int i_s_common_deinit(void*)
{
	DBUG_ENTER("i_s_common_deinit");

	/* Do nothing */

	DBUG_RETURN(0);
}

static const LEX_CSTRING row_format_values[] =
{
  { STRING_WITH_LEN("Redundant") },
  { STRING_WITH_LEN("Compact") },
  { STRING_WITH_LEN("Compressed") },
  { STRING_WITH_LEN("Dynamic") }
};

static TypelibBuffer<4> row_format_values_typelib(row_format_values);

static const LEX_CSTRING space_type_values[] =
{
	{ STRING_WITH_LEN("Single") },
	{ STRING_WITH_LEN("System") }
};

static TypelibBuffer<2> space_type_values_typelib(space_type_values);

namespace Show {
/**  SYS_TABLES  ***************************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.SYS_TABLES */
static ST_FIELD_INFO innodb_sys_tables_fields_info[]=
{
#define SYS_TABLES_ID			0
  Column("TABLE_ID", ULonglong(), NOT_NULL),

#define SYS_TABLES_NAME			1
  Column("NAME", Varchar(MAX_FULL_NAME_LEN + 1), NOT_NULL),

#define SYS_TABLES_FLAG			2
  Column("FLAG", SLong(), NOT_NULL),

#define SYS_TABLES_NUM_COLUMN		3
  Column("N_COLS", ULong(), NOT_NULL),

#define SYS_TABLES_SPACE		4
  Column("SPACE", ULong(), NOT_NULL),

#define SYS_TABLES_ROW_FORMAT		5
  Column("ROW_FORMAT", Enum(&row_format_values_typelib), NULLABLE),

#define SYS_TABLES_ZIP_PAGE_SIZE	6
  Column("ZIP_PAGE_SIZE", ULong(), NOT_NULL),

#define SYS_TABLES_SPACE_TYPE		7
  Column("SPACE_TYPE", Enum(&space_type_values_typelib), NULLABLE),

  CEnd()
};
} // namespace Show

/**********************************************************************//**
Populate information_schema.innodb_sys_tables table with information
from SYS_TABLES.
@return 0 on success */
static
int
i_s_dict_fill_sys_tables(
/*=====================*/
	THD*		thd,		/*!< in: thread */
	dict_table_t*	table,		/*!< in: table */
	TABLE*		table_to_fill)	/*!< in/out: fill this table */
{
	Field**			fields;
	ulint			compact = DICT_TF_GET_COMPACT(table->flags);
	ulint			atomic_blobs = DICT_TF_HAS_ATOMIC_BLOBS(
								table->flags);
	const ulint zip_size = dict_tf_get_zip_size(table->flags);
	const char*		row_format;

	if (!compact) {
		row_format = "Redundant";
	} else if (!atomic_blobs) {
		row_format = "Compact";
	} else if (DICT_TF_GET_ZIP_SSIZE(table->flags)) {
		row_format = "Compressed";
	} else {
		row_format = "Dynamic";
	}

	DBUG_ENTER("i_s_dict_fill_sys_tables");

	fields = table_to_fill->field;

	OK(fields[SYS_TABLES_ID]->store(longlong(table->id), TRUE));

	OK(field_store_string(fields[SYS_TABLES_NAME], table->name.m_name));

	OK(fields[SYS_TABLES_FLAG]->store(table->flags));

	OK(fields[SYS_TABLES_NUM_COLUMN]->store(table->n_cols));

	OK(fields[SYS_TABLES_SPACE]->store(table->space_id, true));

	OK(field_store_string(fields[SYS_TABLES_ROW_FORMAT], row_format));

	OK(fields[SYS_TABLES_ZIP_PAGE_SIZE]->store(zip_size, true));

	OK(field_store_string(fields[SYS_TABLES_SPACE_TYPE],
			      table->space_id ? "Single" : "System"));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}

/** Convert one SYS_TABLES record to dict_table_t.
@param pcur      persistent cursor position on SYS_TABLES record
@param mtr       mini-transaction (nullptr=use the dict_sys cache)
@param rec       record to read from (nullptr=use the dict_sys cache)
@param table     the converted dict_table_t
@return error message
@retval nullptr on success */
static const char *i_s_sys_tables_rec(const btr_pcur_t &pcur, mtr_t *mtr,
                                      const rec_t *rec, dict_table_t **table)
{
  static_assert(DICT_FLD__SYS_TABLES__NAME == 0, "compatibility");
  size_t len;
  if (rec_get_1byte_offs_flag(pcur.old_rec))
  {
    len= rec_1_get_field_end_info(pcur.old_rec, 0);
    if (len & REC_1BYTE_SQL_NULL_MASK)
      return "corrupted SYS_TABLES.NAME";
  }
  else
  {
    len= rec_2_get_field_end_info(pcur.old_rec, 0);
    static_assert(REC_2BYTE_EXTERN_MASK == 16384, "compatibility");
    if (len >= REC_2BYTE_EXTERN_MASK)
      return "corrupted SYS_TABLES.NAME";
  }

  if (rec)
    return dict_load_table_low(mtr, rec, table);

  *table= dict_sys.load_table
    (span<const char>{reinterpret_cast<const char*>(pcur.old_rec), len});
  return *table ? nullptr : "Table not found in cache";
}

/*******************************************************************//**
Function to go through each record in SYS_TABLES table, and fill the
information_schema.innodb_sys_tables table with related table information
@return 0 on success */
static
int
i_s_sys_tables_fill_table(
/*======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	btr_pcur_t	pcur;
	mtr_t		mtr;

	DBUG_ENTER("i_s_sys_tables_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	mtr.start();
	dict_sys.lock(SRW_LOCK_CALL);

	for (const rec_t *rec = dict_startscan_system(&pcur, &mtr,
						      dict_sys.sys_tables);
	     rec; rec = dict_getnext_system(&pcur, &mtr)) {
		if (rec_get_deleted_flag(rec, 0)) {
			continue;
		}

		const char*	err_msg;
		dict_table_t*	table_rec;

		/* Create and populate a dict_table_t structure with
		information from SYS_TABLES row */
		err_msg = i_s_sys_tables_rec(pcur, &mtr, rec, &table_rec);
		mtr.commit();
		dict_sys.unlock();

		if (!err_msg) {
			i_s_dict_fill_sys_tables(thd, table_rec,
						 tables->table);
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		if (table_rec) {
			dict_mem_table_free(table_rec);
		}

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
	}

	mtr.commit();
	dict_sys.unlock();

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_tables
@return 0 on success */
static
int
innodb_sys_tables_init(
/*===================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_tables_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_tables_fields_info;
	schema->fill_table = i_s_sys_tables_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_tables =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_TABLES",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_TABLES",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_tables_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  SYS_TABLESTATS  ***********************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.SYS_TABLESTATS */
static ST_FIELD_INFO innodb_sys_tablestats_fields_info[]=
{
#define SYS_TABLESTATS_ID		0
  Column("TABLE_ID", ULonglong(), NOT_NULL),

#define SYS_TABLESTATS_NAME		1
  Column("NAME", Varchar(NAME_CHAR_LEN), NOT_NULL),

#define SYS_TABLESTATS_INIT		2
  Column("STATS_INITIALIZED", SLong(1), NOT_NULL),

#define SYS_TABLESTATS_NROW		3
  Column("NUM_ROWS", ULonglong(), NOT_NULL),

#define SYS_TABLESTATS_CLUST_SIZE	4
  Column("CLUST_INDEX_SIZE", ULonglong(), NOT_NULL),

#define SYS_TABLESTATS_INDEX_SIZE	5
  Column("OTHER_INDEX_SIZE", ULonglong(), NOT_NULL),

#define SYS_TABLESTATS_MODIFIED		6
  Column("MODIFIED_COUNTER", ULonglong(), NOT_NULL),

#define SYS_TABLESTATS_AUTONINC		7
  Column("AUTOINC", ULonglong(), NOT_NULL),

#define SYS_TABLESTATS_TABLE_REF_COUNT	8
  Column("REF_COUNT", SLong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/** Populate information_schema.innodb_sys_tablestats table with information
from SYS_TABLES.
@param[in]	thd		thread ID
@param[in,out]	table		table
@param[in]	ref_count	table reference count
@param[in,out]	table_to_fill	fill this table
@return 0 on success */
static
int
i_s_dict_fill_sys_tablestats(
	THD*		thd,
	dict_table_t*	table,
	ulint		ref_count,
	TABLE*		table_to_fill)
{
	Field**		fields;

	DBUG_ENTER("i_s_dict_fill_sys_tablestats");

	fields = table_to_fill->field;

	OK(fields[SYS_TABLESTATS_ID]->store(longlong(table->id), TRUE));

	OK(field_store_string(fields[SYS_TABLESTATS_NAME],
			      table->name.m_name));

	{
		table->stats_mutex_lock();
		auto _ = make_scope_exit([table]() {
			table->stats_mutex_unlock(); });

		OK(fields[SYS_TABLESTATS_INIT]->store(table->stat_initialized,
						      true));

		if (table->stat_initialized) {
			OK(fields[SYS_TABLESTATS_NROW]->store(
				   table->stat_n_rows, true));

			OK(fields[SYS_TABLESTATS_CLUST_SIZE]->store(
				   table->stat_clustered_index_size, true));

			OK(fields[SYS_TABLESTATS_INDEX_SIZE]->store(
				   table->stat_sum_of_other_index_sizes,
				   true));

			OK(fields[SYS_TABLESTATS_MODIFIED]->store(
				   table->stat_modified_counter, true));
		} else {
			OK(fields[SYS_TABLESTATS_NROW]->store(0, true));

			OK(fields[SYS_TABLESTATS_CLUST_SIZE]->store(0, true));

			OK(fields[SYS_TABLESTATS_INDEX_SIZE]->store(0, true));

			OK(fields[SYS_TABLESTATS_MODIFIED]->store(0, true));
		}
	}

	OK(fields[SYS_TABLESTATS_AUTONINC]->store(table->autoinc, true));

	OK(fields[SYS_TABLESTATS_TABLE_REF_COUNT]->store(ref_count, true));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}

/*******************************************************************//**
Function to go through each record in SYS_TABLES table, and fill the
information_schema.innodb_sys_tablestats table with table statistics
related information
@return 0 on success */
static
int
i_s_sys_tables_fill_table_stats(
/*============================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	btr_pcur_t	pcur;
	const rec_t*	rec;
	mtr_t		mtr;

	DBUG_ENTER("i_s_sys_tables_fill_table_stats");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	mtr.start();
	dict_sys.lock(SRW_LOCK_CALL);

	rec = dict_startscan_system(&pcur, &mtr, dict_sys.sys_tables);

	while (rec) {
		const char*	err_msg;
		dict_table_t*	table_rec= 0;

		mtr.commit();
		/* Fetch the dict_table_t structure corresponding to
		this SYS_TABLES record */
		err_msg = i_s_sys_tables_rec(pcur, nullptr, nullptr,
                                             &table_rec);

		if (UNIV_LIKELY(!err_msg)) {
			bool evictable = dict_sys.prevent_eviction(table_rec);
			ulint ref_count = table_rec->get_ref_count();
			dict_sys.unlock();
			i_s_dict_fill_sys_tablestats(thd, table_rec, ref_count,
						     tables->table);
			if (!evictable) {
				table_rec = nullptr;
			}
		} else {
			ut_ad(!table_rec);
			dict_sys.unlock();
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
		if (table_rec) {
			dict_sys.allow_eviction(table_rec);
		}

		rec = dict_getnext_system(&pcur, &mtr);
	}

	mtr.commit();
	dict_sys.unlock();

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_tablestats
@return 0 on success */
static
int
innodb_sys_tablestats_init(
/*=======================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_tablestats_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_tablestats_fields_info;
	schema->fill_table = i_s_sys_tables_fill_table_stats;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_tablestats =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_TABLESTATS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_TABLESTATS",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_tablestats_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  SYS_INDEXES  **************************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.SYS_INDEXES */
static ST_FIELD_INFO innodb_sysindex_fields_info[]=
{
#define SYS_INDEX_ID		0
  Column("INDEX_ID", ULonglong(), NOT_NULL),

#define SYS_INDEX_NAME		1
  Column("NAME", Varchar(NAME_CHAR_LEN), NOT_NULL),

#define SYS_INDEX_TABLE_ID	2
  Column("TABLE_ID", ULonglong(), NOT_NULL),

#define SYS_INDEX_TYPE		3
  Column("TYPE", SLong(), NOT_NULL),

#define SYS_INDEX_NUM_FIELDS	4
  Column("N_FIELDS", SLong(), NOT_NULL),

#define SYS_INDEX_PAGE_NO	5
  Column("PAGE_NO", SLong(), NOT_NULL),

#define SYS_INDEX_SPACE		6
  Column("SPACE", SLong(), NOT_NULL),

#define SYS_INDEX_MERGE_THRESHOLD 7
  Column("MERGE_THRESHOLD", SLong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/**********************************************************************//**
Function to populate the information_schema.innodb_sys_indexes table with
collected index information
@return 0 on success */
static
int
i_s_dict_fill_sys_indexes(
/*======================*/
	THD*		thd,		/*!< in: thread */
	table_id_t	table_id,	/*!< in: table id */
	ulint		space_id,	/*!< in: tablespace id */
	dict_index_t*	index,		/*!< in: populated dict_index_t
					struct with index info */
	TABLE*		table_to_fill)	/*!< in/out: fill this table */
{
	Field**		fields;

	DBUG_ENTER("i_s_dict_fill_sys_indexes");

	fields = table_to_fill->field;

	if (*index->name == *TEMP_INDEX_PREFIX_STR) {
		/* Since TEMP_INDEX_PREFIX_STR is not valid UTF-8, we
		need to convert it to something else. */
		*const_cast<char*>(index->name()) = '?';
	}

	OK(fields[SYS_INDEX_NAME]->store(index->name,
					 uint(strlen(index->name)),
					 system_charset_info));

	OK(fields[SYS_INDEX_ID]->store(longlong(index->id), true));

	OK(fields[SYS_INDEX_TABLE_ID]->store(longlong(table_id), true));

	OK(fields[SYS_INDEX_TYPE]->store(index->type, true));

	OK(fields[SYS_INDEX_NUM_FIELDS]->store(index->n_fields));

	/* FIL_NULL is ULINT32_UNDEFINED */
	if (index->page == FIL_NULL) {
		fields[SYS_INDEX_PAGE_NO]->set_null();
	} else {
		OK(fields[SYS_INDEX_PAGE_NO]->store(index->page, true));
	}

	if (space_id == ULINT_UNDEFINED) {
		fields[SYS_INDEX_SPACE]->set_null();
	} else {
		OK(fields[SYS_INDEX_SPACE]->store(space_id, true));
	}

	OK(fields[SYS_INDEX_MERGE_THRESHOLD]->store(index->merge_threshold,
						    true));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}
/*******************************************************************//**
Function to go through each record in SYS_INDEXES table, and fill the
information_schema.innodb_sys_indexes table with related index information
@return 0 on success */
static
int
i_s_sys_indexes_fill_table(
/*=======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	btr_pcur_t		pcur;
	const rec_t*		rec;
	mem_heap_t*		heap;
	mtr_t			mtr;

	DBUG_ENTER("i_s_sys_indexes_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	heap = mem_heap_create(1000);
	dict_sys.lock(SRW_LOCK_CALL);
	mtr_start(&mtr);

	/* Start scan the SYS_INDEXES table */
	rec = dict_startscan_system(&pcur, &mtr, dict_sys.sys_indexes);

	/* Process each record in the table */
	while (rec) {
		const char*	err_msg;
		table_id_t	table_id;
		ulint		space_id;
		dict_index_t	index_rec;

		/* Populate a dict_index_t structure with information from
		a SYS_INDEXES row */
		err_msg = dict_process_sys_indexes_rec(heap, rec, &index_rec,
						       &table_id);
		const byte* field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__SPACE, &space_id);
		space_id = space_id == 4 ? mach_read_from_4(field)
			: ULINT_UNDEFINED;
		mtr.commit();
		dict_sys.unlock();

		if (!err_msg) {
			if (int err = i_s_dict_fill_sys_indexes(
				    thd, table_id, space_id, &index_rec,
				    tables->table)) {
				mem_heap_free(heap);
				DBUG_RETURN(err);
			}
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		mem_heap_empty(heap);

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
		rec = dict_getnext_system(&pcur, &mtr);
	}

	mtr.commit();
	dict_sys.unlock();
	mem_heap_free(heap);

	DBUG_RETURN(0);
}
/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_indexes
@return 0 on success */
static
int
innodb_sys_indexes_init(
/*====================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_indexes_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sysindex_fields_info;
	schema->fill_table = i_s_sys_indexes_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_indexes =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_INDEXES",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_INDEXES",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_indexes_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  SYS_COLUMNS  **************************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_SYS_COLUMNS */
static ST_FIELD_INFO innodb_sys_columns_fields_info[]=
{
#define SYS_COLUMN_TABLE_ID		0
  Column("TABLE_ID", ULonglong(), NOT_NULL),

#define SYS_COLUMN_NAME		1
  Column("NAME", Varchar(NAME_CHAR_LEN), NOT_NULL),

#define SYS_COLUMN_POSITION	2
  Column("POS", ULonglong(), NOT_NULL),

#define SYS_COLUMN_MTYPE		3
  Column("MTYPE", SLong(), NOT_NULL),

#define SYS_COLUMN__PRTYPE	4
  Column("PRTYPE", SLong(), NOT_NULL),

#define SYS_COLUMN_COLUMN_LEN	5
  Column("LEN", SLong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/**********************************************************************//**
Function to populate the information_schema.innodb_sys_columns with
related column information
@return 0 on success */
static
int
i_s_dict_fill_sys_columns(
/*======================*/
	THD*		thd,		/*!< in: thread */
	table_id_t	table_id,	/*!< in: table ID */
	const char*	col_name,	/*!< in: column name */
	dict_col_t*	column,		/*!< in: dict_col_t struct holding
					more column information */
	ulint		nth_v_col,	/*!< in: virtual column, its
					sequence number (nth virtual col) */
	TABLE*		table_to_fill)	/*!< in/out: fill this table */
{
	Field**		fields;

	DBUG_ENTER("i_s_dict_fill_sys_columns");

	fields = table_to_fill->field;

	OK(fields[SYS_COLUMN_TABLE_ID]->store((longlong) table_id, TRUE));

	OK(field_store_string(fields[SYS_COLUMN_NAME], col_name));

	if (column->is_virtual()) {
		ulint	pos = dict_create_v_col_pos(nth_v_col, column->ind);
		OK(fields[SYS_COLUMN_POSITION]->store(pos, true));
	} else {
		OK(fields[SYS_COLUMN_POSITION]->store(column->ind, true));
	}

	OK(fields[SYS_COLUMN_MTYPE]->store(column->mtype));

	OK(fields[SYS_COLUMN__PRTYPE]->store(column->prtype));

	OK(fields[SYS_COLUMN_COLUMN_LEN]->store(column->len));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}
/*******************************************************************//**
Function to fill information_schema.innodb_sys_columns with information
collected by scanning SYS_COLUMNS table.
@return 0 on success */
static
int
i_s_sys_columns_fill_table(
/*=======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	btr_pcur_t	pcur;
	const rec_t*	rec;
	const char*	col_name;
	mem_heap_t*	heap;
	mtr_t		mtr;

	DBUG_ENTER("i_s_sys_columns_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	heap = mem_heap_create(1000);
	mtr.start();
	dict_sys.lock(SRW_LOCK_CALL);

	rec = dict_startscan_system(&pcur, &mtr, dict_sys.sys_columns);

	while (rec) {
		const char*	err_msg;
		dict_col_t	column_rec;
		table_id_t	table_id;
		ulint		nth_v_col;

		/* populate a dict_col_t structure with information from
		a SYS_COLUMNS row */
		err_msg = dict_process_sys_columns_rec(heap, rec, &column_rec,
						       &table_id, &col_name,
						       &nth_v_col);

		mtr.commit();
		dict_sys.unlock();

		if (!err_msg) {
			i_s_dict_fill_sys_columns(thd, table_id, col_name,
						 &column_rec, nth_v_col,
						 tables->table);
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		mem_heap_empty(heap);

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
		rec = dict_getnext_system(&pcur, &mtr);
	}

	mtr.commit();
	dict_sys.unlock();
	mem_heap_free(heap);

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_columns
@return 0 on success */
static
int
innodb_sys_columns_init(
/*====================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_columns_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_columns_fields_info;
	schema->fill_table = i_s_sys_columns_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_columns =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_COLUMNS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_COLUMNS",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_columns_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  SYS_VIRTUAL **************************************************/
/** Fields of the dynamic table INFORMATION_SCHEMA.INNODB_SYS_VIRTUAL */
static ST_FIELD_INFO innodb_sys_virtual_fields_info[]=
{
#define SYS_VIRTUAL_TABLE_ID		0
  Column("TABLE_ID", ULonglong(), NOT_NULL),

#define SYS_VIRTUAL_POS			1
  Column("POS", ULong(), NOT_NULL),

#define SYS_VIRTUAL_BASE_POS		2
  Column("BASE_POS", ULong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/** Function to populate the information_schema.innodb_sys_virtual with
related information
param[in]	thd		thread
param[in]	table_id	table ID
param[in]	pos		virtual column position
param[in]	base_pos	base column position
param[in,out]	table_to_fill	fill this table
@return 0 on success */
static
int
i_s_dict_fill_sys_virtual(
	THD*		thd,
	table_id_t	table_id,
	ulint		pos,
	ulint		base_pos,
	TABLE*		table_to_fill)
{
	Field**		fields;

	DBUG_ENTER("i_s_dict_fill_sys_virtual");

	fields = table_to_fill->field;

	OK(fields[SYS_VIRTUAL_TABLE_ID]->store(table_id, true));

	OK(fields[SYS_VIRTUAL_POS]->store(pos, true));

	OK(fields[SYS_VIRTUAL_BASE_POS]->store(base_pos, true));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}

/** Function to fill information_schema.innodb_sys_virtual with information
collected by scanning SYS_VIRTUAL table.
param[in]	thd		thread
param[in,out]	tables		tables to fill
param[in]	item		condition (not used)
@return 0 on success */
static
int
i_s_sys_virtual_fill_table(
	THD*		thd,
	TABLE_LIST*	tables,
	Item*		)
{
	btr_pcur_t	pcur;
	const rec_t*	rec;
	ulint		pos;
	ulint		base_pos;
	mtr_t		mtr;

	DBUG_ENTER("i_s_sys_virtual_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL) || !dict_sys.sys_virtual) {
		DBUG_RETURN(0);
	}

	mtr.start();
	dict_sys.lock(SRW_LOCK_CALL);

	rec = dict_startscan_system(&pcur, &mtr, dict_sys.sys_virtual);

	while (rec) {
		const char*	err_msg;
		table_id_t	table_id;

		/* populate a dict_col_t structure with information from
		a SYS_VIRTUAL row */
		err_msg = dict_process_sys_virtual_rec(rec,
						       &table_id, &pos,
						       &base_pos);

		mtr.commit();
		dict_sys.unlock();

		if (!err_msg) {
			i_s_dict_fill_sys_virtual(thd, table_id, pos, base_pos,
						  tables->table);
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
		rec = dict_getnext_system(&pcur, &mtr);
	}

	mtr.commit();
	dict_sys.unlock();

	DBUG_RETURN(0);
}

/** Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_virtual
param[in,out]	p	table schema object
@return 0 on success */
static
int
innodb_sys_virtual_init(
	void*	p)
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_virtual_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_virtual_fields_info;
	schema->fill_table = i_s_sys_virtual_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_virtual =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_VIRTUAL",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_VIRTUAL",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_virtual_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

	/* Maria extension */
	INNODB_VERSION_STR,
	MariaDB_PLUGIN_MATURITY_STABLE,
};


namespace Show {
/**  SYS_FIELDS  ***************************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_SYS_FIELDS */
static ST_FIELD_INFO innodb_sys_fields_fields_info[]=
{
#define SYS_FIELD_INDEX_ID	0
  Column("INDEX_ID", ULonglong(), NOT_NULL),

#define SYS_FIELD_NAME		1
  Column("NAME", Varchar(NAME_CHAR_LEN), NOT_NULL),

#define SYS_FIELD_POS		2
  Column("POS", ULong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/**********************************************************************//**
Function to fill information_schema.innodb_sys_fields with information
collected by scanning SYS_FIELDS table.
@return 0 on success */
static
int
i_s_dict_fill_sys_fields(
/*=====================*/
	THD*		thd,		/*!< in: thread */
	index_id_t	index_id,	/*!< in: index id for the field */
	dict_field_t*	field,		/*!< in: table */
	ulint		pos,		/*!< in: Field position */
	TABLE*		table_to_fill)	/*!< in/out: fill this table */
{
	Field**		fields;

	DBUG_ENTER("i_s_dict_fill_sys_fields");

	fields = table_to_fill->field;

	OK(fields[SYS_FIELD_INDEX_ID]->store(index_id, true));

	OK(field_store_string(fields[SYS_FIELD_NAME], field->name));

	OK(fields[SYS_FIELD_POS]->store(pos, true));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}
/*******************************************************************//**
Function to go through each record in SYS_FIELDS table, and fill the
information_schema.innodb_sys_fields table with related index field
information
@return 0 on success */
static
int
i_s_sys_fields_fill_table(
/*======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	btr_pcur_t	pcur;
	const rec_t*	rec;
	mem_heap_t*	heap;
	index_id_t	last_id;
	mtr_t		mtr;

	DBUG_ENTER("i_s_sys_fields_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	heap = mem_heap_create(1000);
	mtr.start();

	/* will save last index id so that we know whether we move to
	the next index. This is used to calculate prefix length */
	last_id = 0;

	dict_sys.lock(SRW_LOCK_CALL);
	rec = dict_startscan_system(&pcur, &mtr, dict_sys.sys_fields);

	while (rec) {
		ulint		pos;
		const char*	err_msg;
		index_id_t	index_id;
		dict_field_t	field_rec;

		/* Populate a dict_field_t structure with information from
		a SYS_FIELDS row */
		err_msg = dict_process_sys_fields_rec(heap, rec, &field_rec,
						      &pos, &index_id, last_id);

		mtr.commit();
		dict_sys.unlock();

		if (!err_msg) {
			i_s_dict_fill_sys_fields(thd, index_id, &field_rec,
						 pos, tables->table);
			last_id = index_id;
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		mem_heap_empty(heap);

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
		rec = dict_getnext_system(&pcur, &mtr);
	}

	mtr.commit();
	dict_sys.unlock();
	mem_heap_free(heap);

	DBUG_RETURN(0);
}
/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_fields
@return 0 on success */
static
int
innodb_sys_fields_init(
/*===================*/
	void*   p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_field_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_fields_fields_info;
	schema->fill_table = i_s_sys_fields_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_fields =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_FIELDS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_FIELDS",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_fields_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  SYS_FOREIGN        ********************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_SYS_FOREIGN */
static ST_FIELD_INFO innodb_sys_foreign_fields_info[]=
{
#define SYS_FOREIGN_ID		0
  Column("ID", Varchar(NAME_LEN + 1), NOT_NULL),

#define SYS_FOREIGN_FOR_NAME	1
  Column("FOR_NAME", Varchar(NAME_LEN + 1), NOT_NULL),

#define SYS_FOREIGN_REF_NAME	2
  Column("REF_NAME", Varchar(NAME_LEN + 1), NOT_NULL),

#define SYS_FOREIGN_NUM_COL	3
  Column("N_COLS", ULong(), NOT_NULL),

#define SYS_FOREIGN_TYPE	4
  Column("TYPE", ULong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/**********************************************************************//**
Function to fill information_schema.innodb_sys_foreign with information
collected by scanning SYS_FOREIGN table.
@return 0 on success */
static
int
i_s_dict_fill_sys_foreign(
/*======================*/
	THD*		thd,		/*!< in: thread */
	dict_foreign_t*	foreign,	/*!< in: table */
	TABLE*		table_to_fill)	/*!< in/out: fill this table */
{
	Field**		fields;

	DBUG_ENTER("i_s_dict_fill_sys_foreign");

	fields = table_to_fill->field;

	OK(field_store_string(fields[SYS_FOREIGN_ID], foreign->id));

	OK(field_store_string(fields[SYS_FOREIGN_FOR_NAME],
			      foreign->foreign_table_name));

	OK(field_store_string(fields[SYS_FOREIGN_REF_NAME],
			      foreign->referenced_table_name));

	OK(fields[SYS_FOREIGN_NUM_COL]->store(foreign->n_fields));

	OK(fields[SYS_FOREIGN_TYPE]->store(foreign->type));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}

/*******************************************************************//**
Function to populate INFORMATION_SCHEMA.innodb_sys_foreign table. Loop
through each record in SYS_FOREIGN, and extract the foreign key
information.
@return 0 on success */
static
int
i_s_sys_foreign_fill_table(
/*=======================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	btr_pcur_t	pcur;
	const rec_t*	rec;
	mem_heap_t*	heap;
	mtr_t		mtr;

	DBUG_ENTER("i_s_sys_foreign_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL) || !dict_sys.sys_foreign) {
		DBUG_RETURN(0);
	}

	heap = mem_heap_create(1000);
	mtr.start();
	dict_sys.lock(SRW_LOCK_CALL);

	rec = dict_startscan_system(&pcur, &mtr, dict_sys.sys_foreign);

	while (rec) {
		const char*	err_msg;
		dict_foreign_t	foreign_rec;

		/* Populate a dict_foreign_t structure with information from
		a SYS_FOREIGN row */
		err_msg = dict_process_sys_foreign_rec(heap, rec, &foreign_rec);

		mtr.commit();
		dict_sys.unlock();

		if (!err_msg) {
			i_s_dict_fill_sys_foreign(thd, &foreign_rec,
						 tables->table);
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		mem_heap_empty(heap);

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
		rec = dict_getnext_system(&pcur, &mtr);
	}

	mtr.commit();
	dict_sys.unlock();
	mem_heap_free(heap);

	DBUG_RETURN(0);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_foreign
@return 0 on success */
static
int
innodb_sys_foreign_init(
/*====================*/
	void*   p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_foreign_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_foreign_fields_info;
	schema->fill_table = i_s_sys_foreign_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_foreign =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_FOREIGN",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_FOREIGN",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_foreign_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  SYS_FOREIGN_COLS   ********************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_SYS_FOREIGN_COLS */
static ST_FIELD_INFO innodb_sys_foreign_cols_fields_info[]=
{
#define SYS_FOREIGN_COL_ID		0
  Column("ID", Varchar(NAME_LEN + 1), NOT_NULL),

#define SYS_FOREIGN_COL_FOR_NAME	1
  Column("FOR_COL_NAME", Varchar(NAME_CHAR_LEN), NOT_NULL),

#define SYS_FOREIGN_COL_REF_NAME	2
  Column("REF_COL_NAME", Varchar(NAME_CHAR_LEN), NOT_NULL),

#define SYS_FOREIGN_COL_POS		3
  Column("POS", ULong(), NOT_NULL),

  CEnd()
};
} // namespace Show

/**********************************************************************//**
Function to fill information_schema.innodb_sys_foreign_cols with information
collected by scanning SYS_FOREIGN_COLS table.
@return 0 on success */
static
int
i_s_dict_fill_sys_foreign_cols(
/*==========================*/
	THD*		thd,		/*!< in: thread */
	const char*	name,		/*!< in: foreign key constraint name */
	const char*	for_col_name,	/*!< in: referencing column name*/
	const char*	ref_col_name,	/*!< in: referenced column
					name */
	ulint		pos,		/*!< in: column position */
	TABLE*		table_to_fill)	/*!< in/out: fill this table */
{
	Field**		fields;

	DBUG_ENTER("i_s_dict_fill_sys_foreign_cols");

	fields = table_to_fill->field;

	OK(field_store_string(fields[SYS_FOREIGN_COL_ID], name));

	OK(field_store_string(fields[SYS_FOREIGN_COL_FOR_NAME], for_col_name));

	OK(field_store_string(fields[SYS_FOREIGN_COL_REF_NAME], ref_col_name));

	OK(fields[SYS_FOREIGN_COL_POS]->store(pos, true));

	OK(schema_table_store_record(thd, table_to_fill));

	DBUG_RETURN(0);
}
/*******************************************************************//**
Function to populate INFORMATION_SCHEMA.innodb_sys_foreign_cols table. Loop
through each record in SYS_FOREIGN_COLS, and extract the foreign key column
information and fill the INFORMATION_SCHEMA.innodb_sys_foreign_cols table.
@return 0 on success */
static
int
i_s_sys_foreign_cols_fill_table(
/*============================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	btr_pcur_t	pcur;
	const rec_t*	rec;
	mem_heap_t*	heap;
	mtr_t		mtr;

	DBUG_ENTER("i_s_sys_foreign_cols_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)
	    || !dict_sys.sys_foreign_cols) {
		DBUG_RETURN(0);
	}

	heap = mem_heap_create(1000);
	mtr.start();
	dict_sys.lock(SRW_LOCK_CALL);

	rec = dict_startscan_system(&pcur, &mtr, dict_sys.sys_foreign_cols);

	while (rec) {
		const char*	err_msg;
		const char*	name;
		const char*	for_col_name;
		const char*	ref_col_name;
		ulint		pos;

		/* Extract necessary information from a SYS_FOREIGN_COLS row */
		err_msg = dict_process_sys_foreign_col_rec(
			heap, rec, &name, &for_col_name, &ref_col_name, &pos);

		mtr.commit();
		dict_sys.unlock();

		if (!err_msg) {
			i_s_dict_fill_sys_foreign_cols(
				thd, name, for_col_name, ref_col_name, pos,
				tables->table);
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_CANT_FIND_SYSTEM_REC, "%s",
					    err_msg);
		}

		mem_heap_empty(heap);

		/* Get the next record */
		mtr.start();
		dict_sys.lock(SRW_LOCK_CALL);
		rec = dict_getnext_system(&pcur, &mtr);
	}

	mtr.commit();
	dict_sys.unlock();
	mem_heap_free(heap);

	DBUG_RETURN(0);
}
/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.innodb_sys_foreign_cols
@return 0 on success */
static
int
innodb_sys_foreign_cols_init(
/*========================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_foreign_cols_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_foreign_cols_fields_info;
	schema->fill_table = i_s_sys_foreign_cols_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_foreign_cols =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_FOREIGN_COLS",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB SYS_FOREIGN_COLS",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_foreign_cols_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  SYS_TABLESPACES    ********************************************/
/* Fields of the dynamic table INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES */
static ST_FIELD_INFO innodb_sys_tablespaces_fields_info[]=
{
#define SYS_TABLESPACES_SPACE		0
  Column("SPACE", ULong(), NOT_NULL),

#define SYS_TABLESPACES_NAME		1
  Column("NAME", Varchar(MAX_FULL_NAME_LEN + 1), NOT_NULL),

#define SYS_TABLESPACES_FLAGS		2
  Column("FLAG", ULong(), NOT_NULL),

#define SYS_TABLESPACES_ROW_FORMAT	3
  Column("ROW_FORMAT", Varchar(22), NULLABLE),

#define SYS_TABLESPACES_PAGE_SIZE	4
  Column("PAGE_SIZE", ULong(), NOT_NULL),

#define SYS_TABLESPACES_FILENAME	5
  Column("FILENAME", Varchar(FN_REFLEN), NOT_NULL),

#define SYS_TABLESPACES_FS_BLOCK_SIZE	6
  Column("FS_BLOCK_SIZE", ULong(),NOT_NULL),

#define SYS_TABLESPACES_FILE_SIZE	7
  Column("FILE_SIZE", ULonglong(), NOT_NULL),

#define SYS_TABLESPACES_ALLOC_SIZE	8
  Column("ALLOCATED_SIZE", ULonglong(), NOT_NULL),

  CEnd()
};
} // namespace Show

extern size_t os_file_get_fs_block_size(const char *path);

/** Produce one row of INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES.
@param thd  connection
@param s    tablespace
@param t    output table
@return 0 on success */
static int i_s_sys_tablespaces_fill(THD *thd, const fil_space_t &s, TABLE *t)
{
  DBUG_ENTER("i_s_sys_tablespaces_fill");
  const char *row_format;

  if (s.full_crc32() || is_system_tablespace(s.id))
    row_format= nullptr;
  else if (FSP_FLAGS_GET_ZIP_SSIZE(s.flags))
    row_format= "Compressed";
  else if (FSP_FLAGS_HAS_ATOMIC_BLOBS(s.flags))
    row_format= "Dynamic";
  else
    row_format= "Compact or Redundant";

  Field **fields= t->field;

  OK(fields[SYS_TABLESPACES_SPACE]->store(s.id, true));
  {
    Field *f= fields[SYS_TABLESPACES_NAME];
    const auto name= s.name();
    if (name.data())
    {
      OK(f->store(name.data(), name.size(), system_charset_info));
      f->set_notnull();
    }
    else
      f->set_notnull();
  }

  fields[SYS_TABLESPACES_NAME]->set_null();
  OK(fields[SYS_TABLESPACES_FLAGS]->store(s.flags, true));
  OK(field_store_string(fields[SYS_TABLESPACES_ROW_FORMAT], row_format));
  const char *filepath= s.chain.start->name;
  OK(field_store_string(fields[SYS_TABLESPACES_FILENAME], filepath));

  OK(fields[SYS_TABLESPACES_PAGE_SIZE]->store(s.physical_size(), true));
  size_t fs_block_size;
  os_file_size_t file= os_file_get_size(filepath);
  if (file.m_total_size == os_offset_t(~0))
  {
    file.m_total_size= 0;
    file.m_alloc_size= 0;
    fs_block_size= 0;
  }
  else
    fs_block_size= os_file_get_fs_block_size(filepath);

  OK(fields[SYS_TABLESPACES_FS_BLOCK_SIZE]->store(fs_block_size, true));
  OK(fields[SYS_TABLESPACES_FILE_SIZE]->store(file.m_total_size, true));
  OK(fields[SYS_TABLESPACES_ALLOC_SIZE]->store(file.m_alloc_size, true));

  OK(schema_table_store_record(thd, t));

  DBUG_RETURN(0);
}

/** Populate INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES.
@param thd    connection
@param tables table to fill
@return 0 on success */
static int i_s_sys_tablespaces_fill_table(THD *thd, TABLE_LIST *tables, Item*)
{
  DBUG_ENTER("i_s_sys_tablespaces_fill_table");
  RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

  if (check_global_access(thd, PROCESS_ACL))
    DBUG_RETURN(0);

  int err= 0;

  mysql_mutex_lock(&fil_system.mutex);
  fil_system.freeze_space_list++;

  for (fil_space_t &space : fil_system.space_list)
  {
    if (space.purpose == FIL_TYPE_TABLESPACE && !space.is_stopping() &&
        space.chain.start)
    {
      space.reacquire();
      mysql_mutex_unlock(&fil_system.mutex);
      err= i_s_sys_tablespaces_fill(thd, space, tables->table);
      mysql_mutex_lock(&fil_system.mutex);
      space.release();
      if (err)
        break;
    }
  }

  fil_system.freeze_space_list--;
  mysql_mutex_unlock(&fil_system.mutex);
  DBUG_RETURN(err);
}

/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES
@return 0 on success */
static
int
innodb_sys_tablespaces_init(
/*========================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_sys_tablespaces_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_sys_tablespaces_fields_info;
	schema->fill_table = i_s_sys_tablespaces_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_sys_tablespaces =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_SYS_TABLESPACES",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	plugin_author,

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB tablespaces",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_GPL,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_sys_tablespaces_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

        /* Maria extension */
	INNODB_VERSION_STR,
        MariaDB_PLUGIN_MATURITY_STABLE,
};

namespace Show {
/**  TABLESPACES_ENCRYPTION    ********************************************/
/* Fields of the table INFORMATION_SCHEMA.INNODB_TABLESPACES_ENCRYPTION */
static ST_FIELD_INFO innodb_tablespaces_encryption_fields_info[]=
{
#define TABLESPACES_ENCRYPTION_SPACE	0
  Column("SPACE", ULong(), NOT_NULL),

#define TABLESPACES_ENCRYPTION_NAME		1
  Column("NAME", Varchar(MAX_FULL_NAME_LEN + 1), NULLABLE),

#define TABLESPACES_ENCRYPTION_ENCRYPTION_SCHEME	2
  Column("ENCRYPTION_SCHEME", ULong(), NOT_NULL),

#define TABLESPACES_ENCRYPTION_KEYSERVER_REQUESTS	3
  Column("KEYSERVER_REQUESTS", ULong(), NOT_NULL),

#define TABLESPACES_ENCRYPTION_MIN_KEY_VERSION	4
  Column("MIN_KEY_VERSION", ULong(), NOT_NULL),

#define TABLESPACES_ENCRYPTION_CURRENT_KEY_VERSION	5
  Column("CURRENT_KEY_VERSION", ULong(), NOT_NULL),

#define TABLESPACES_ENCRYPTION_KEY_ROTATION_PAGE_NUMBER	6
  Column("KEY_ROTATION_PAGE_NUMBER", ULonglong(), NULLABLE),

#define TABLESPACES_ENCRYPTION_KEY_ROTATION_MAX_PAGE_NUMBER 7
  Column("KEY_ROTATION_MAX_PAGE_NUMBER", ULonglong(), NULLABLE),

#define TABLESPACES_ENCRYPTION_CURRENT_KEY_ID	8
  Column("CURRENT_KEY_ID", ULong(), NOT_NULL),

#define TABLESPACES_ENCRYPTION_ROTATING_OR_FLUSHING 9
  Column("ROTATING_OR_FLUSHING", SLong(1), NOT_NULL),

  CEnd()
};
} // namespace Show

/**********************************************************************//**
Function to fill INFORMATION_SCHEMA.INNODB_TABLESPACES_ENCRYPTION.
@param[in]	thd		thread handle
@param[in]	space		Tablespace
@param[in]	table_to_fill	I_S table to fill
@return 0 on success */
static
int
i_s_dict_fill_tablespaces_encryption(
	THD*		thd,
	fil_space_t*	space,
	TABLE*		table_to_fill)
{
	Field**	fields;
	struct fil_space_crypt_status_t status;

	DBUG_ENTER("i_s_dict_fill_tablespaces_encryption");

	fields = table_to_fill->field;

	fil_space_crypt_get_status(space, &status);

	/* If tablespace id does not match, we did not find
	encryption information for this tablespace. */
	if (!space->crypt_data || space->id != status.space) {
		goto skip;
	}

	OK(fields[TABLESPACES_ENCRYPTION_SPACE]->store(space->id, true));

	{
		const auto name = space->name();
		if (name.data()) {
			OK(fields[TABLESPACES_ENCRYPTION_NAME]->store(
				   name.data(), name.size(),
				   system_charset_info));
			fields[TABLESPACES_ENCRYPTION_NAME]->set_notnull();
		} else {
			fields[TABLESPACES_ENCRYPTION_NAME]->set_null();
		}
	}

	OK(fields[TABLESPACES_ENCRYPTION_ENCRYPTION_SCHEME]->store(
		   status.scheme, true));
	OK(fields[TABLESPACES_ENCRYPTION_KEYSERVER_REQUESTS]->store(
		   status.keyserver_requests, true));
	OK(fields[TABLESPACES_ENCRYPTION_MIN_KEY_VERSION]->store(
		   status.min_key_version, true));
	OK(fields[TABLESPACES_ENCRYPTION_CURRENT_KEY_VERSION]->store(
		   status.current_key_version, true));
	OK(fields[TABLESPACES_ENCRYPTION_CURRENT_KEY_ID]->store(
		   status.key_id, true));
	OK(fields[TABLESPACES_ENCRYPTION_ROTATING_OR_FLUSHING]->store(
			   status.rotating || status.flushing, true));

	if (status.rotating) {
		fields[TABLESPACES_ENCRYPTION_KEY_ROTATION_PAGE_NUMBER]->set_notnull();
		OK(fields[TABLESPACES_ENCRYPTION_KEY_ROTATION_PAGE_NUMBER]->store(
			   status.rotate_next_page_number, true));
		fields[TABLESPACES_ENCRYPTION_KEY_ROTATION_MAX_PAGE_NUMBER]->set_notnull();
		OK(fields[TABLESPACES_ENCRYPTION_KEY_ROTATION_MAX_PAGE_NUMBER]->store(
			   status.rotate_max_page_number, true));
	} else {
		fields[TABLESPACES_ENCRYPTION_KEY_ROTATION_PAGE_NUMBER]
			->set_null();
		fields[TABLESPACES_ENCRYPTION_KEY_ROTATION_MAX_PAGE_NUMBER]
			->set_null();
	}

	OK(schema_table_store_record(thd, table_to_fill));

skip:
	DBUG_RETURN(0);
}
/*******************************************************************//**
Function to populate INFORMATION_SCHEMA.INNODB_TABLESPACES_ENCRYPTION table.
Loop through each record in TABLESPACES_ENCRYPTION, and extract the column
information and fill the INFORMATION_SCHEMA.INNODB_TABLESPACES_ENCRYPTION table.
@return 0 on success */
static
int
i_s_tablespaces_encryption_fill_table(
/*===========================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	DBUG_ENTER("i_s_tablespaces_encryption_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	int err = 0;
	mysql_mutex_lock(&fil_system.mutex);
	fil_system.freeze_space_list++;

	for (fil_space_t& space : fil_system.space_list) {
		if (space.purpose == FIL_TYPE_TABLESPACE
		    && !space.is_stopping()) {
			space.reacquire();
			mysql_mutex_unlock(&fil_system.mutex);
			err = i_s_dict_fill_tablespaces_encryption(
				thd, &space, tables->table);
			mysql_mutex_lock(&fil_system.mutex);
			space.release();
			if (err) {
				break;
			}
		}
	}

	fil_system.freeze_space_list--;
	mysql_mutex_unlock(&fil_system.mutex);
	DBUG_RETURN(err);
}
/*******************************************************************//**
Bind the dynamic table INFORMATION_SCHEMA.INNODB_TABLESPACES_ENCRYPTION
@return 0 on success */
static
int
innodb_tablespaces_encryption_init(
/*========================*/
	void*	p)	/*!< in/out: table schema object */
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("innodb_tablespaces_encryption_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = Show::innodb_tablespaces_encryption_fields_info;
	schema->fill_table = i_s_tablespaces_encryption_fill_table;

	DBUG_RETURN(0);
}

struct st_maria_plugin	i_s_innodb_tablespaces_encryption =
{
	/* the plugin type (a MYSQL_XXX_PLUGIN value) */
	/* int */
	MYSQL_INFORMATION_SCHEMA_PLUGIN,

	/* pointer to type-specific plugin descriptor */
	/* void* */
	&i_s_info,

	/* plugin name */
	/* const char* */
	"INNODB_TABLESPACES_ENCRYPTION",

	/* plugin author (for SHOW PLUGINS) */
	/* const char* */
	"Google Inc",

	/* general descriptive text (for SHOW PLUGINS) */
	/* const char* */
	"InnoDB TABLESPACES_ENCRYPTION",

	/* the plugin license (PLUGIN_LICENSE_XXX) */
	/* int */
	PLUGIN_LICENSE_BSD,

	/* the function to invoke when plugin is loaded */
	/* int (*)(void*); */
	innodb_tablespaces_encryption_init,

	/* the function to invoke when plugin is unloaded */
	/* int (*)(void*); */
	i_s_common_deinit,

	/* plugin version (for SHOW PLUGINS) */
	/* unsigned int */
	INNODB_VERSION_SHORT,

	/* struct st_mysql_show_var* */
	NULL,

	/* struct st_mysql_sys_var** */
	NULL,

	/* Maria extension */
	INNODB_VERSION_STR,
	MariaDB_PLUGIN_MATURITY_STABLE
};
