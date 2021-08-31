/*****************************************************************************

Copyright (c) 2007, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2021, MariaDB Corporation.

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
@file fts/fts0sql.cc
Full Text Search functionality.

Created 2007-03-27 Sunny Bains
*******************************************************/

#include "que0que.h"
#include "trx0roll.h"
#include "pars0pars.h"
#include "dict0dict.h"
#include "fts0types.h"
#include "fts0priv.h"

/** SQL statements for creating the ancillary FTS tables. */

/** Preamble to all SQL statements. */
static const char* fts_sql_begin=
	"PROCEDURE P() IS\n";

/** Postamble to non-committing SQL statements. */
static const char* fts_sql_end=
	"\n"
	"END;\n";

/******************************************************************//**
Get the table id.
@return number of bytes written */
int
fts_get_table_id(
/*=============*/
	const fts_table_t*
			fts_table,	/*!< in: FTS Auxiliary table */
	char*		table_id)	/*!< out: table id, must be at least
					FTS_AUX_MIN_TABLE_ID_LENGTH bytes
					long */
{
	int		len;

	ut_a(fts_table->table != NULL);

	switch (fts_table->type) {
	case FTS_COMMON_TABLE:
		len = fts_write_object_id(fts_table->table_id, table_id);
		break;

	case FTS_INDEX_TABLE:

		len = fts_write_object_id(fts_table->table_id, table_id);

		table_id[len] = '_';
		++len;
		table_id += len;

		len += fts_write_object_id(fts_table->index_id, table_id);
		break;

	default:
		ut_error;
	}

	ut_a(len >= 16);
	ut_a(len < FTS_AUX_MIN_TABLE_ID_LENGTH);

	return(len);
}

/** Construct the name of an internal FTS table for the given table.
@param[in]	fts_table	metadata on fulltext-indexed table
@param[out]	table_name	a name up to MAX_FULL_NAME_LEN
@param[in]	dict_locked	whether dict_sys.latch is being held */
void fts_get_table_name(const fts_table_t* fts_table, char* table_name,
			bool dict_locked)
{
	if (!dict_locked) {
		dict_sys.freeze(SRW_LOCK_CALL);
	}
	ut_ad(dict_sys.frozen());
	/* Include the separator as well. */
	const size_t dbname_len = fts_table->table->name.dblen() + 1;
	ut_ad(dbname_len > 1);
	memcpy(table_name, fts_table->table->name.m_name, dbname_len);
	if (!dict_locked) {
		dict_sys.unfreeze();
	}
	memcpy(table_name += dbname_len, "FTS_", 4);
	table_name += 4;
	table_name += fts_get_table_id(fts_table, table_name);
	*table_name++ = '_';
	strcpy(table_name, fts_table->suffix);
}

/******************************************************************//**
Parse an SQL string.
@return query graph */
que_t*
fts_parse_sql(
/*==========*/
	fts_table_t*	fts_table,	/*!< in: FTS auxiliarry table info */
	pars_info_t*	info,		/*!< in: info struct, or NULL */
	const char*	sql)		/*!< in: SQL string to evaluate */
{
	char*		str;
	que_t*		graph;
	ibool		dict_locked;

	str = ut_str3cat(fts_sql_begin, sql, fts_sql_end);

	dict_locked = (fts_table && fts_table->table->fts
		       && fts_table->table->fts->dict_locked);

	if (!dict_locked) {
		/* The InnoDB SQL parser is not re-entrant. */
		dict_sys.lock(SRW_LOCK_CALL);
	}

	graph = pars_sql(info, str);
	ut_a(graph);

	if (!dict_locked) {
		dict_sys.unlock();
	}

	ut_free(str);

	return(graph);
}

/******************************************************************//**
Evaluate an SQL query graph.
@return DB_SUCCESS or error code */
dberr_t
fts_eval_sql(
/*=========*/
	trx_t*		trx,		/*!< in: transaction */
	que_t*		graph)		/*!< in: Query graph to evaluate */
{
	que_thr_t*	thr;

	graph->trx = trx;

	ut_a(thr = que_fork_start_command(graph));

	que_run_threads(thr);

	return(trx->error_state);
}

/******************************************************************//**
Construct the column specification part of the SQL string for selecting the
indexed FTS columns for the given table. Adds the necessary bound
ids to the given 'info' and returns the SQL string. Examples:

One indexed column named "text":

 "$sel0",
 info/ids: sel0 -> "text"

Two indexed columns named "subject" and "content":

 "$sel0, $sel1",
 info/ids: sel0 -> "subject", sel1 -> "content",
@return heap-allocated WHERE string */
const char*
fts_get_select_columns_str(
/*=======================*/
	dict_index_t*   index,		/*!< in: index */
	pars_info_t*    info,		/*!< in/out: parser info */
	mem_heap_t*     heap)		/*!< in: memory heap */
{
	ulint		i;
	const char*	str = "";

	for (i = 0; i < index->n_user_defined_cols; i++) {
		char*           sel_str;

		dict_field_t*   field = dict_index_get_nth_field(index, i);

		sel_str = mem_heap_printf(heap, "sel%lu", (ulong) i);

		/* Set copy_name to TRUE since it's dynamic. */
		pars_info_bind_id(info, sel_str, field->name);

		str = mem_heap_printf(
			heap, "%s%s$%s", str, (*str) ? ", " : "", sel_str);
	}

	return(str);
}
