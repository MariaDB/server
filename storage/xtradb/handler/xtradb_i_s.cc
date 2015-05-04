/*****************************************************************************

Copyright (c) 2007, 2012, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2010-2012, Percona Inc. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

#include <mysqld_error.h>
#include <sql_acl.h>				// PROCESS_ACL

#include <m_ctype.h>
#include <hash.h>
#include <myisampack.h>
#include <mysys_err.h>
#include <my_sys.h>
#include "i_s.h"
#include <sql_plugin.h>
#include <mysql/innodb_priv.h>

#include <read0i_s.h>
#include <trx0i_s.h>
#include "srv0start.h"	/* for srv_was_started */
#include <btr0sea.h> /* btr_search_sys */
#include <log0recv.h> /* recv_sys */
#include <fil0fil.h>

/* for XTRADB_RSEG table */
#include "trx0trx.h" /* for TRX_QUE_STATE_STR_MAX_LEN */
#include "trx0rseg.h" /* for trx_rseg_struct */
#include "trx0sys.h" /* for trx_sys */

#define PLUGIN_AUTHOR "Percona Inc."

#define OK(expr)		\
	if ((expr) != 0) {	\
		DBUG_RETURN(1);	\
	}

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

#if !defined __STRICT_ANSI__ && defined __GNUC__ && (__GNUC__) > 2 &&	\
	!defined __INTEL_COMPILER && !defined __clang__
#define STRUCT_FLD(name, value)	name: value
#else
#define STRUCT_FLD(name, value)	value
#endif

#define END_OF_ST_FIELD_INFO \
	{STRUCT_FLD(field_name,		NULL), \
	 STRUCT_FLD(field_length,	0), \
	 STRUCT_FLD(field_type,		MYSQL_TYPE_NULL), \
	 STRUCT_FLD(value,		0), \
	 STRUCT_FLD(field_flags,	0), \
	 STRUCT_FLD(old_name,		""), \
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)}


/*******************************************************************//**
Auxiliary function to store ulint value in MYSQL_TYPE_LONGLONG field.
If the value is ULINT_UNDEFINED then the field it set to NULL.
@return	0 on success */
static
int
field_store_ulint(
/*==============*/
	Field*	field,	/*!< in/out: target field for storage */
	ulint	n)	/*!< in: value to store */
{
	int	ret;

	if (n != ULINT_UNDEFINED) {

		ret = field->store(n);
		field->set_notnull();
	} else {

		ret = 0; /* success */
		field->set_null();
	}

	return(ret);
}

/*******************************************************************//**
Auxiliary function to store char* value in MYSQL_TYPE_STRING field.
@return	0 on success */
static
int
field_store_string(
/*===============*/
	Field*		field,	/*!< in/out: target field for storage */
	const char*	str)	/*!< in: NUL-terminated utf-8 string,
				or NULL */
{
	int	ret;

	if (str != NULL) {

		ret = field->store(str, strlen(str),
				   system_charset_info);
		field->set_notnull();
	} else {

		ret = 0; /* success */
		field->set_null();
	}

	return(ret);
}

static
int
i_s_common_deinit(
/*==============*/
	void*	p)	/*!< in/out: table schema object */
{
	DBUG_ENTER("i_s_common_deinit");

	/* Do nothing */

	DBUG_RETURN(0);
}

static ST_FIELD_INFO xtradb_read_view_fields_info[] =
{
#define READ_VIEW_UNDO_NUMBER		0
	{STRUCT_FLD(field_name,		"READ_VIEW_UNDO_NUMBER"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

#define READ_VIEW_LOW_LIMIT_NUMBER	1
	{STRUCT_FLD(field_name,		"READ_VIEW_LOW_LIMIT_TRX_NUMBER"),
	 STRUCT_FLD(field_length,	TRX_ID_MAX_LEN + 1),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_STRING),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	0),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

#define READ_VIEW_UPPER_LIMIT_ID	2
	{STRUCT_FLD(field_name,		"READ_VIEW_UPPER_LIMIT_TRX_ID"),
	 STRUCT_FLD(field_length,	TRX_ID_MAX_LEN + 1),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_STRING),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	0),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

#define READ_VIEW_LOW_LIMIT_ID		3
	{STRUCT_FLD(field_name,		"READ_VIEW_LOW_LIMIT_TRX_ID"),

	 STRUCT_FLD(field_length,	TRX_ID_MAX_LEN + 1),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_STRING),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	0),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	END_OF_ST_FIELD_INFO
};

static int xtradb_read_view_fill_table(THD* thd, TABLE_LIST* tables, Item*)
{
	const char*		table_name;
	Field**	fields;
	TABLE* table;
	char		trx_id[TRX_ID_MAX_LEN + 1];


	DBUG_ENTER("xtradb_read_view_fill_table");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	table_name = tables->schema_table_name;
	table = tables->table;
	fields = table->field;

	RETURN_IF_INNODB_NOT_STARTED(table_name);

	i_s_xtradb_read_view_t read_view;

	if (read_fill_i_s_xtradb_read_view(&read_view) == NULL)
		DBUG_RETURN(0);

	OK(field_store_ulint(fields[READ_VIEW_UNDO_NUMBER], read_view.undo_no));

	ut_snprintf(trx_id, sizeof(trx_id), TRX_ID_FMT, read_view.low_limit_no);
	OK(field_store_string(fields[READ_VIEW_LOW_LIMIT_NUMBER], trx_id));

	ut_snprintf(trx_id, sizeof(trx_id), TRX_ID_FMT, read_view.up_limit_id);
	OK(field_store_string(fields[READ_VIEW_UPPER_LIMIT_ID], trx_id));

	ut_snprintf(trx_id, sizeof(trx_id), TRX_ID_FMT, read_view.low_limit_id);
	OK(field_store_string(fields[READ_VIEW_LOW_LIMIT_ID], trx_id));

	OK(schema_table_store_record(thd, table));

	DBUG_RETURN(0);
}


static int xtradb_read_view_init(void* p)
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("xtradb_read_view_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = xtradb_read_view_fields_info;
	schema->fill_table = xtradb_read_view_fill_table;

	DBUG_RETURN(0);
}

static struct st_mysql_information_schema i_s_info =
{
	MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

UNIV_INTERN struct st_mysql_plugin i_s_xtradb_read_view =
{
	STRUCT_FLD(type, MYSQL_INFORMATION_SCHEMA_PLUGIN),
	STRUCT_FLD(info, &i_s_info),
	STRUCT_FLD(name, "XTRADB_READ_VIEW"),
	STRUCT_FLD(author, PLUGIN_AUTHOR),
	STRUCT_FLD(descr, "InnoDB Read View information"),
	STRUCT_FLD(license, PLUGIN_LICENSE_GPL),
	STRUCT_FLD(init, xtradb_read_view_init),
	STRUCT_FLD(deinit, i_s_common_deinit),
	STRUCT_FLD(version, INNODB_VERSION_SHORT),
	STRUCT_FLD(status_vars, NULL),
	STRUCT_FLD(system_vars, NULL),
	STRUCT_FLD(__reserved1, NULL),
	STRUCT_FLD(flags, 0UL),
};

static ST_FIELD_INFO xtradb_internal_hash_tables_fields_info[] =
{
#define INT_HASH_TABLES_NAME		0
	{STRUCT_FLD(field_name,		"INTERNAL_HASH_TABLE_NAME"),
	 STRUCT_FLD(field_length,	100),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_STRING),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	0),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

#define INT_HASH_TABLES_TOTAL		1
	{STRUCT_FLD(field_name,		"TOTAL_MEMORY"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

#define INT_HASH_TABLES_CONSTANT		2
	{STRUCT_FLD(field_name,		"CONSTANT_MEMORY"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

#define INT_HASH_TABLES_VARIABLE		3
	{STRUCT_FLD(field_name,		"VARIABLE_MEMORY"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	END_OF_ST_FIELD_INFO
};

static int xtradb_internal_hash_tables_fill_table(THD* thd, TABLE_LIST* tables, Item*)
{
	const char*	table_name;
	Field**		fields;
	TABLE*		table;
	ulong		btr_search_sys_constant;
	ulong		btr_search_sys_variable;

	DBUG_ENTER("xtradb_internal_hash_tables_fill_table");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	table_name = tables->schema_table_name;
	table = tables->table;
	fields = table->field;

	RETURN_IF_INNODB_NOT_STARTED(table_name);

	/* Calculate AHI constant and variable memory allocations */

	btr_search_sys_constant = 0;
	btr_search_sys_variable = 0;

	ut_ad(btr_search_sys->hash_tables);

	for (ulint i = 0; i < btr_search_index_num; i++) {
		hash_table_t* ht = btr_search_sys->hash_tables[i];

		ut_ad(ht);
		ut_ad(ht->heap);

		/* Multiple mutexes/heaps are currently never used for adaptive
		hash index tables. */
		ut_ad(!ht->n_sync_obj);
		ut_ad(!ht->heaps);

		btr_search_sys_variable += mem_heap_get_size(ht->heap);
		btr_search_sys_constant += ht->n_cells * sizeof(hash_cell_t);
	}

	OK(field_store_string(fields[INT_HASH_TABLES_NAME],
			      "Adaptive hash index"));
	OK(field_store_ulint(fields[INT_HASH_TABLES_TOTAL],
			     btr_search_sys_variable + btr_search_sys_constant));
	OK(field_store_ulint(fields[INT_HASH_TABLES_CONSTANT],
			     btr_search_sys_constant));
	OK(field_store_ulint(fields[INT_HASH_TABLES_VARIABLE],
			     btr_search_sys_variable));
	OK(schema_table_store_record(thd, table));

	{
	  OK(field_store_string(fields[INT_HASH_TABLES_NAME],
				"Page hash (buffer pool 0 only)"));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_TOTAL],
			       (ulong) (buf_pool_from_array(0)->page_hash->n_cells * sizeof(hash_cell_t))));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_CONSTANT],
			       (ulong) (buf_pool_from_array(0)->page_hash->n_cells * sizeof(hash_cell_t))));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_VARIABLE], 0));
	  OK(schema_table_store_record(thd, table));

	}

	if (dict_sys)
	{
	  OK(field_store_string(fields[INT_HASH_TABLES_NAME],
				"Dictionary Cache"));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_TOTAL],
			       ((dict_sys->table_hash->n_cells
				 + dict_sys->table_id_hash->n_cells
				 ) * sizeof(hash_cell_t)
				+ dict_sys->size)));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_CONSTANT],
			       ((dict_sys->table_hash->n_cells
				 + dict_sys->table_id_hash->n_cells
				 ) * sizeof(hash_cell_t))));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_VARIABLE],
			       dict_sys->size));
	  OK(schema_table_store_record(thd, table));
	}

	{
	  OK(field_store_string(fields[INT_HASH_TABLES_NAME],
				"File system"));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_TOTAL],
			       (ulong) (fil_system_hash_cells()
					* sizeof(hash_cell_t)
					+ fil_system_hash_nodes())));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_CONSTANT],
			       (ulong) (fil_system_hash_cells() * sizeof(hash_cell_t))));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_VARIABLE],
			       (ulong) fil_system_hash_nodes()));
	  OK(schema_table_store_record(thd, table));

	}

	{
	  ulint lock_sys_constant, lock_sys_variable;

	  trx_i_s_get_lock_sys_memory_usage(&lock_sys_constant,
					    &lock_sys_variable);

	  OK(field_store_string(fields[INT_HASH_TABLES_NAME], "Lock System"));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_TOTAL],
			       lock_sys_constant + lock_sys_variable));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_CONSTANT],
			       lock_sys_constant));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_VARIABLE],
			       lock_sys_variable));
	  OK(schema_table_store_record(thd, table));
	}

	if (recv_sys)
	{
	  ulint recv_sys_subtotal = ((recv_sys && recv_sys->addr_hash)
				     ? mem_heap_get_size(recv_sys->heap) : 0);

	  OK(field_store_string(fields[INT_HASH_TABLES_NAME], "Recovery System"));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_TOTAL],
			       ((recv_sys->addr_hash) ? (recv_sys->addr_hash->n_cells * sizeof(hash_cell_t)) : 0) + recv_sys_subtotal));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_CONSTANT],
			       ((recv_sys->addr_hash) ? (recv_sys->addr_hash->n_cells * sizeof(hash_cell_t)) : 0)));
	  OK(field_store_ulint(fields[INT_HASH_TABLES_VARIABLE],
			       recv_sys_subtotal));
	  OK(schema_table_store_record(thd, table));
	}

	DBUG_RETURN(0);
}

static int xtradb_internal_hash_tables_init(void* p)
{
	ST_SCHEMA_TABLE*	schema;

	DBUG_ENTER("xtradb_internal_hash_tables_init");

	schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = xtradb_internal_hash_tables_fields_info;
	schema->fill_table = xtradb_internal_hash_tables_fill_table;

	DBUG_RETURN(0);
}

UNIV_INTERN struct st_mysql_plugin i_s_xtradb_internal_hash_tables =
{
	STRUCT_FLD(type, MYSQL_INFORMATION_SCHEMA_PLUGIN),
	STRUCT_FLD(info, &i_s_info),
	STRUCT_FLD(name, "XTRADB_INTERNAL_HASH_TABLES"),
	STRUCT_FLD(author, PLUGIN_AUTHOR),
	STRUCT_FLD(descr, "InnoDB internal hash tables information"),
	STRUCT_FLD(license, PLUGIN_LICENSE_GPL),
	STRUCT_FLD(init, xtradb_internal_hash_tables_init),
	STRUCT_FLD(deinit, i_s_common_deinit),
	STRUCT_FLD(version, INNODB_VERSION_SHORT),
	STRUCT_FLD(status_vars, NULL),
	STRUCT_FLD(system_vars, NULL),
	STRUCT_FLD(__reserved1, NULL),
	STRUCT_FLD(flags, 0UL),
};


/***********************************************************************
*/
static ST_FIELD_INFO	i_s_xtradb_rseg_fields_info[] =
{
	{STRUCT_FLD(field_name,		"rseg_id"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	{STRUCT_FLD(field_name,		"space_id"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	{STRUCT_FLD(field_name,		"zip_size"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	{STRUCT_FLD(field_name,		"page_no"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	{STRUCT_FLD(field_name,		"max_size"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	{STRUCT_FLD(field_name,		"curr_size"),
	 STRUCT_FLD(field_length,	MY_INT64_NUM_DECIMAL_DIGITS),
	 STRUCT_FLD(field_type,		MYSQL_TYPE_LONGLONG),
	 STRUCT_FLD(value,		0),
	 STRUCT_FLD(field_flags,	MY_I_S_UNSIGNED),
	 STRUCT_FLD(old_name,		""),
	 STRUCT_FLD(open_method,	SKIP_OPEN_TABLE)},

	END_OF_ST_FIELD_INFO
};

static
int
i_s_xtradb_rseg_fill(
/*=================*/
	THD*		thd,	/* in: thread */
	TABLE_LIST*	tables,	/* in/out: tables to fill */
	Item*		)	/* in: condition (ignored) */
{
	TABLE*	table	= (TABLE *) tables->table;
	int	status	= 0;
	trx_rseg_t*	rseg;

	DBUG_ENTER("i_s_xtradb_rseg_fill");

	/* deny access to non-superusers */
	if (check_global_access(thd, PROCESS_ACL)) {

		DBUG_RETURN(0);
	}

	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name);

	for(int i=0; i < TRX_SYS_N_RSEGS; i++)
	{
	  rseg = trx_sys->rseg_array[i];
	  if (!rseg)
	    continue;

	  table->field[0]->store(rseg->id);
	  table->field[1]->store(rseg->space);
	  table->field[2]->store(rseg->zip_size);
	  table->field[3]->store(rseg->page_no);
	  table->field[4]->store(rseg->max_size);
	  table->field[5]->store(rseg->curr_size);

	  if (schema_table_store_record(thd, table)) {
	    status = 1;
	    break;
	  }
	}

	DBUG_RETURN(status);
}

static
int
i_s_xtradb_rseg_init(
/*=================*/
			/* out: 0 on success */
	void*	p)	/* in/out: table schema object */
{
	DBUG_ENTER("i_s_xtradb_rseg_init");
	ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*) p;

	schema->fields_info = i_s_xtradb_rseg_fields_info;
	schema->fill_table = i_s_xtradb_rseg_fill;

	DBUG_RETURN(0);
}

UNIV_INTERN struct st_mysql_plugin	i_s_xtradb_rseg =
{
	STRUCT_FLD(type, MYSQL_INFORMATION_SCHEMA_PLUGIN),
	STRUCT_FLD(info, &i_s_info),
	STRUCT_FLD(name, "XTRADB_RSEG"),
	STRUCT_FLD(author, PLUGIN_AUTHOR),
	STRUCT_FLD(descr, "InnoDB rollback segment information"),
	STRUCT_FLD(license, PLUGIN_LICENSE_GPL),
	STRUCT_FLD(init, i_s_xtradb_rseg_init),
	STRUCT_FLD(deinit, i_s_common_deinit),
	STRUCT_FLD(version, INNODB_VERSION_SHORT),
	STRUCT_FLD(status_vars, NULL),
	STRUCT_FLD(system_vars, NULL),
	STRUCT_FLD(__reserved1, NULL),
	STRUCT_FLD(flags, 0UL),
};
