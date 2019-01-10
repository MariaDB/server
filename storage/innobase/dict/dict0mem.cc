/*****************************************************************************

Copyright (c) 1996, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2018, MariaDB Corporation.

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

/******************************************************************//**
@file dict/dict0mem.cc
Data dictionary memory object creation

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include "ha_prototypes.h"
#include <mysql_com.h>

#include "dict0mem.h"
#include "rem0rec.h"
#include "data0type.h"
#include "mach0data.h"
#include "dict0dict.h"
#include "fts0priv.h"
#include "lock0lock.h"
#include "sync0sync.h"
#include "row0row.h"
#include <iostream>

#define	DICT_HEAP_SIZE		100	/*!< initial memory heap size when
					creating a table or index object */

/** System databases */
static const char* innobase_system_databases[] = {
	"mysql/",
	"information_schema/",
	"performance_schema/",
	NullS
};

/** Determine if a table belongs to innobase_system_databases[]
@param[in]	name	database_name/table_name
@return	whether the database_name is in innobase_system_databases[] */
static bool dict_mem_table_is_system(const char *name)
{
	/* table has the following format: database/table
	and some system table are of the form SYS_* */
	if (!strchr(name, '/')) {
		return true;
	}
	size_t table_len = strlen(name);
	const char *system_db;
	int i = 0;
	while ((system_db = innobase_system_databases[i++])
	       && (system_db != NullS)) {
		size_t len = strlen(system_db);
		if (table_len > len && !strncmp(name, system_db, len)) {
			return true;
		}
	}
	return false;
}

/** The start of the table basename suffix for partitioned tables */
const char table_name_t::part_suffix[4]
#ifdef _WIN32
= "#p#";
#else
= "#P#";
#endif

/** Display an identifier.
@param[in,out]	s	output stream
@param[in]	id_name	SQL identifier (other than table name)
@return the output stream */
std::ostream&
operator<<(
	std::ostream&		s,
	const id_name_t&	id_name)
{
	const char	q	= '`';
	const char*	c	= id_name;
	s << q;
	for (; *c != 0; c++) {
		if (*c == q) {
			s << *c;
		}
		s << *c;
	}
	s << q;
	return(s);
}

/** Display a table name.
@param[in,out]	s		output stream
@param[in]	table_name	table name
@return the output stream */
std::ostream&
operator<<(
	std::ostream&		s,
	const table_name_t&	table_name)
{
	return(s << ut_get_name(NULL, table_name.m_name));
}

/**********************************************************************//**
Creates a table memory object.
@return own: table object */
dict_table_t*
dict_mem_table_create(
/*==================*/
	const char*	name,	/*!< in: table name */
	fil_space_t*	space,	/*!< in: tablespace */
	ulint		n_cols,	/*!< in: total number of columns including
				virtual and non-virtual columns */
	ulint		n_v_cols,/*!< in: number of virtual columns */
	ulint		flags,	/*!< in: table flags */
	ulint		flags2)	/*!< in: table flags2 */
{
	dict_table_t*	table;
	mem_heap_t*	heap;

	ut_ad(name);
	ut_ad(!space
	      || space->purpose == FIL_TYPE_TABLESPACE
	      || space->purpose == FIL_TYPE_TEMPORARY
	      || space->purpose == FIL_TYPE_IMPORT);
	ut_a(dict_tf2_is_valid(flags, flags2));
	ut_a(!(flags2 & DICT_TF2_UNUSED_BIT_MASK));

	heap = mem_heap_create(DICT_HEAP_SIZE);

	table = static_cast<dict_table_t*>(
		mem_heap_zalloc(heap, sizeof(*table)));

	lock_table_lock_list_init(&table->locks);

	UT_LIST_INIT(table->indexes, &dict_index_t::indexes);

	table->heap = heap;

	ut_d(table->magic_n = DICT_TABLE_MAGIC_N);

	table->flags = (unsigned int) flags;
	table->flags2 = (unsigned int) flags2;
	table->name.m_name = mem_strdup(name);
	table->is_system_db = dict_mem_table_is_system(table->name.m_name);
	table->space = space;
	table->space_id = space ? space->id : ULINT_UNDEFINED;
	table->n_t_cols = unsigned(n_cols + DATA_N_SYS_COLS);
	table->n_v_cols = (unsigned int) (n_v_cols);
	table->n_cols = unsigned(table->n_t_cols - table->n_v_cols);

	table->cols = static_cast<dict_col_t*>(
		mem_heap_alloc(heap, table->n_cols * sizeof(dict_col_t)));
	table->v_cols = static_cast<dict_v_col_t*>(
		mem_heap_alloc(heap, n_v_cols * sizeof(*table->v_cols)));

	/* true means that the stats latch will be enabled -
	dict_table_stats_lock() will not be noop. */
	dict_table_stats_latch_create(table, true);

	table->autoinc_lock = static_cast<ib_lock_t*>(
		mem_heap_alloc(heap, lock_get_size()));

	/* lazy creation of table autoinc latch */
	dict_table_autoinc_create_lazy(table);

	/* If the table has an FTS index or we are in the process
	of building one, create the table->fts */
	if (dict_table_has_fts_index(table)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID)) {
		table->fts = fts_create(table);
		table->fts->cache = fts_cache_create(table);
	}

	new(&table->foreign_set) dict_foreign_set();
	new(&table->referenced_set) dict_foreign_set();

	return(table);
}

/****************************************************************//**
Free a table memory object. */
void
dict_mem_table_free(
/*================*/
	dict_table_t*	table)		/*!< in: table */
{
	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_d(table->cached = FALSE);

	if (dict_table_has_fts_index(table)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID)) {
		if (table->fts) {
			fts_optimize_remove_table(table);

			fts_free(table);
		}
	}

	dict_table_autoinc_destroy(table);
	dict_mem_table_free_foreign_vcol_set(table);
	dict_table_stats_latch_destroy(table);

	table->foreign_set.~dict_foreign_set();
	table->referenced_set.~dict_foreign_set();

	ut_free(table->name.m_name);
	table->name.m_name = NULL;

	/* Clean up virtual index info structures that are registered
	with virtual columns */
	for (ulint i = 0; i < table->n_v_def; i++) {
		dict_v_col_t*	vcol
			= dict_table_get_nth_v_col(table, i);

		UT_DELETE(vcol->v_indexes);
	}

	if (table->s_cols != NULL) {
		UT_DELETE(table->s_cols);
	}

	mem_heap_free(table->heap);
}

/****************************************************************//**
Append 'name' to 'col_names'.  @see dict_table_t::col_names
@return new column names array */
static
const char*
dict_add_col_name(
/*==============*/
	const char*	col_names,	/*!< in: existing column names, or
					NULL */
	ulint		cols,		/*!< in: number of existing columns */
	const char*	name,		/*!< in: new column name */
	mem_heap_t*	heap)		/*!< in: heap */
{
	ulint	old_len;
	ulint	new_len;
	ulint	total_len;
	char*	res;

	ut_ad(!cols == !col_names);

	/* Find out length of existing array. */
	if (col_names) {
		const char*	s = col_names;
		ulint		i;

		for (i = 0; i < cols; i++) {
			s += strlen(s) + 1;
		}

		old_len = unsigned(s - col_names);
	} else {
		old_len = 0;
	}

	new_len = strlen(name) + 1;
	total_len = old_len + new_len;

	res = static_cast<char*>(mem_heap_alloc(heap, total_len));

	if (old_len > 0) {
		memcpy(res, col_names, old_len);
	}

	memcpy(res + old_len, name, new_len);

	return(res);
}

/**********************************************************************//**
Adds a column definition to a table. */
void
dict_mem_table_add_col(
/*===================*/
	dict_table_t*	table,	/*!< in: table */
	mem_heap_t*	heap,	/*!< in: temporary memory heap, or NULL */
	const char*	name,	/*!< in: column name, or NULL */
	ulint		mtype,	/*!< in: main datatype */
	ulint		prtype,	/*!< in: precise type */
	ulint		len)	/*!< in: precision */
{
	dict_col_t*	col;
	ulint		i;

	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(!heap == !name);

	ut_ad(!(prtype & DATA_VIRTUAL));

	i = table->n_def++;

	table->n_t_def++;

	if (name) {
		if (table->n_def == table->n_cols) {
			heap = table->heap;
		}
		if (i && !table->col_names) {
			/* All preceding column names are empty. */
			char* s = static_cast<char*>(
				mem_heap_zalloc(heap, table->n_def));

			table->col_names = s;
		}

		table->col_names = dict_add_col_name(table->col_names,
						     i, name, heap);
	}

	col = dict_table_get_nth_col(table, i);

	dict_mem_fill_column_struct(col, i, mtype, prtype, len);

	switch (prtype & DATA_VERSIONED) {
	case DATA_VERS_START:
		ut_ad(!table->vers_start);
		table->vers_start = i;
		break;
	case DATA_VERS_END:
		ut_ad(!table->vers_end);
		table->vers_end = i;
	}
}

/** Adds a virtual column definition to a table.
@param[in,out]	table		table
@param[in,out]	heap		temporary memory heap, or NULL. It is
				used to store name when we have not finished
				adding all columns. When all columns are
				added, the whole name will copy to memory from
				table->heap
@param[in]	name		column name
@param[in]	mtype		main datatype
@param[in]	prtype		precise type
@param[in]	len		length
@param[in]	pos		position in a table
@param[in]	num_base	number of base columns
@return the virtual column definition */
dict_v_col_t*
dict_mem_table_add_v_col(
	dict_table_t*	table,
	mem_heap_t*	heap,
	const char*	name,
	ulint		mtype,
	ulint		prtype,
	ulint		len,
	ulint		pos,
	ulint		num_base)
{
	dict_v_col_t*	v_col;
	ulint		i;

	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(!heap == !name);

	ut_ad(prtype & DATA_VIRTUAL);

	i = table->n_v_def++;

	table->n_t_def++;

	if (name != NULL) {
		if (table->n_v_def == table->n_v_cols) {
			heap = table->heap;
		}

		if (i && !table->v_col_names) {
			/* All preceding column names are empty. */
			char* s = static_cast<char*>(
				mem_heap_zalloc(heap, table->n_v_def));

			table->v_col_names = s;
		}

		table->v_col_names = dict_add_col_name(table->v_col_names,
						       i, name, heap);
	}

	v_col = &table->v_cols[i];

	dict_mem_fill_column_struct(&v_col->m_col, pos, mtype, prtype, len);
	v_col->v_pos = i;

	if (num_base != 0) {
		v_col->base_col = static_cast<dict_col_t**>(mem_heap_zalloc(
					table->heap, num_base * sizeof(
						*v_col->base_col)));
	} else {
		v_col->base_col = NULL;
	}

	v_col->num_base = num_base;

	/* Initialize the index list for virtual columns */
	v_col->v_indexes = UT_NEW_NOKEY(dict_v_idx_list());

	return(v_col);
}

/** Adds a stored column definition to a table.
@param[in]	table		table
@param[in]	num_base	number of base columns. */
void
dict_mem_table_add_s_col(
	dict_table_t*	table,
	ulint		num_base)
{
	unsigned	i = unsigned(table->n_def) - 1;
	dict_col_t*	col = dict_table_get_nth_col(table, i);
	dict_s_col_t	s_col;

	ut_ad(col != NULL);

	if (table->s_cols == NULL) {
		table->s_cols = UT_NEW_NOKEY(dict_s_col_list());
	}

	s_col.m_col = col;
	s_col.s_pos = i + table->n_v_def;

	if (num_base != 0) {
		s_col.base_col = static_cast<dict_col_t**>(mem_heap_zalloc(
			table->heap, num_base * sizeof(dict_col_t*)));
	} else {
		s_col.base_col = NULL;
	}

	s_col.num_base = num_base;
	table->s_cols->push_back(s_col);
}

/**********************************************************************//**
Renames a column of a table in the data dictionary cache. */
static MY_ATTRIBUTE((nonnull))
void
dict_mem_table_col_rename_low(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table */
	unsigned	i,	/*!< in: column offset corresponding to s */
	const char*	to,	/*!< in: new column name */
	const char*	s,	/*!< in: pointer to table->col_names */
	bool		is_virtual)
				/*!< in: if this is a virtual column */
{
	char*	t_col_names = const_cast<char*>(
		is_virtual ? table->v_col_names : table->col_names);
	ulint	n_col = is_virtual ? table->n_v_def : table->n_def;

	size_t from_len = strlen(s), to_len = strlen(to);

	ut_ad(i < table->n_def || is_virtual);
	ut_ad(i < table->n_v_def || !is_virtual);

	ut_ad(from_len <= NAME_LEN);
	ut_ad(to_len <= NAME_LEN);

	char from[NAME_LEN + 1];
	strncpy(from, s, NAME_LEN + 1);

	if (from_len == to_len) {
		/* The easy case: simply replace the column name in
		table->col_names. */
		strcpy(const_cast<char*>(s), to);
	} else {
		/* We need to adjust all affected index->field
		pointers, as in dict_index_add_col(). First, copy
		table->col_names. */
		ulint	prefix_len	= ulint(s - t_col_names);

		for (; i < n_col; i++) {
			s += strlen(s) + 1;
		}

		ulint	full_len	= ulint(s - t_col_names);
		char*	col_names;

		if (to_len > from_len) {
			col_names = static_cast<char*>(
				mem_heap_alloc(
					table->heap,
					full_len + to_len - from_len));

			memcpy(col_names, t_col_names, prefix_len);
		} else {
			col_names = const_cast<char*>(t_col_names);
		}

		memcpy(col_names + prefix_len, to, to_len);
		memmove(col_names + prefix_len + to_len,
			t_col_names + (prefix_len + from_len),
			full_len - (prefix_len + from_len));

		/* Replace the field names in every index. */
		for (dict_index_t* index = dict_table_get_first_index(table);
		     index != NULL;
		     index = dict_table_get_next_index(index)) {
			ulint	n_fields = dict_index_get_n_fields(index);

			for (ulint i = 0; i < n_fields; i++) {
				dict_field_t*	field
					= dict_index_get_nth_field(
						index, i);

				ut_ad(!field->name
				      == field->col->is_dropped());
				if (!field->name) {
					/* dropped columns lack a name */
					ut_ad(index->is_instant());
					continue;
				}

				/* if is_virtual and that in field->col does
				not match, continue */
				if ((!is_virtual) !=
				    (!field->col->is_virtual())) {
					continue;
				}

				ulint		name_ofs
					= ulint(field->name - t_col_names);
				if (name_ofs <= prefix_len) {
					field->name = col_names + name_ofs;
				} else {
					ut_a(name_ofs < full_len);
					field->name = col_names
						+ name_ofs + to_len - from_len;
				}
			}
		}

		if (is_virtual) {
			table->v_col_names = col_names;
		} else {
			table->col_names = col_names;
		}
	}

	/* Virtual columns are not allowed for foreign key */
	if (is_virtual) {
		return;
	}

	dict_foreign_t*	foreign;

	/* Replace the field names in every foreign key constraint. */
	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;

		if (foreign->foreign_index == NULL) {
			/* We may go here when we set foreign_key_checks to 0,
			and then try to rename a column and modify the
			corresponding foreign key constraint. The index
			would have been dropped, we have to find an equivalent
			one */
			for (unsigned f = 0; f < foreign->n_fields; f++) {
				if (strcmp(foreign->foreign_col_names[f], from)
				    == 0) {

					char** rc = const_cast<char**>(
						foreign->foreign_col_names
						+ f);

					if (to_len <= strlen(*rc)) {
						memcpy(*rc, to, to_len + 1);
					} else {
						*rc = static_cast<char*>(
							mem_heap_dup(
								foreign->heap,
								to,
								to_len + 1));
					}
				}
			}

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->foreign_table, NULL,
				foreign->foreign_col_names,
				foreign->n_fields, NULL, true, false,
				NULL, NULL, NULL);
			/* There must be an equivalent index in this case. */
			ut_ad(new_index != NULL);

			foreign->foreign_index = new_index;

		} else {

			for (unsigned f = 0; f < foreign->n_fields; f++) {
				/* These can point straight to
				table->col_names, because the foreign key
				constraints will be freed at the same time
				when the table object is freed. */
				foreign->foreign_col_names[f]
					= dict_index_get_nth_field(
						foreign->foreign_index,
						f)->name;
			}
		}
	}

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;

		ut_ad(foreign->referenced_index != NULL);

		for (unsigned f = 0; f < foreign->n_fields; f++) {
			/* foreign->referenced_col_names[] need to be
			copies, because the constraint may become
			orphan when foreign_key_checks=0 and the
			parent table is dropped. */

			const char* col_name = dict_index_get_nth_field(
				foreign->referenced_index, f)->name;

			if (strcmp(foreign->referenced_col_names[f],
				   col_name)) {
				char**	rc = const_cast<char**>(
					foreign->referenced_col_names + f);
				size_t	col_name_len_1 = strlen(col_name) + 1;

				if (col_name_len_1 <= strlen(*rc) + 1) {
					memcpy(*rc, col_name, col_name_len_1);
				} else {
					*rc = static_cast<char*>(
						mem_heap_dup(
							foreign->heap,
							col_name,
							col_name_len_1));
				}
			}
		}
	}
}

/**********************************************************************//**
Renames a column of a table in the data dictionary cache. */
void
dict_mem_table_col_rename(
/*======================*/
	dict_table_t*	table,	/*!< in/out: table */
	ulint		nth_col,/*!< in: column index */
	const char*	from,	/*!< in: old column name */
	const char*	to,	/*!< in: new column name */
	bool		is_virtual)
				/*!< in: if this is a virtual column */
{
	const char*	s = is_virtual ? table->v_col_names : table->col_names;

	ut_ad((!is_virtual && nth_col < table->n_def)
	       || (is_virtual && nth_col < table->n_v_def));

	for (ulint i = 0; i < nth_col; i++) {
		size_t	len = strlen(s);
		ut_ad(len > 0);
		s += len + 1;
	}

	ut_ad(!my_strcasecmp(system_charset_info, from, s));

	dict_mem_table_col_rename_low(table, static_cast<unsigned>(nth_col),
				      to, s, is_virtual);
}

/**********************************************************************//**
This function populates a dict_col_t memory structure with
supplied information. */
void
dict_mem_fill_column_struct(
/*========================*/
	dict_col_t*	column,		/*!< out: column struct to be
					filled */
	ulint		col_pos,	/*!< in: column position */
	ulint		mtype,		/*!< in: main data type */
	ulint		prtype,		/*!< in: precise type */
	ulint		col_len)	/*!< in: column length */
{
	ulint	mbminlen;
	ulint	mbmaxlen;

	column->ind = (unsigned int) col_pos;
	column->ord_part = 0;
	column->max_prefix = 0;
	column->mtype = (unsigned int) mtype;
	column->prtype = (unsigned int) prtype;
	column->len = (unsigned int) col_len;
	dtype_get_mblen(mtype, prtype, &mbminlen, &mbmaxlen);
	column->mbminlen = mbminlen;
	column->mbmaxlen = mbmaxlen;
	column->unsigned_len = 0;
	column->def_val.data = NULL;
	column->def_val.len = UNIV_SQL_DEFAULT;
	ut_ad(!column->is_dropped());
}

/**********************************************************************//**
Creates an index memory object.
@return own: index object */
dict_index_t*
dict_mem_index_create(
/*==================*/
	dict_table_t*	table,		/*!< in: table */
	const char*	index_name,	/*!< in: index name */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields)	/*!< in: number of fields */
{
	dict_index_t*	index;
	mem_heap_t*	heap;

	ut_ad(!table || table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(index_name);

	heap = mem_heap_create(DICT_HEAP_SIZE);

	index = static_cast<dict_index_t*>(
		mem_heap_zalloc(heap, sizeof(*index)));
	index->table = table;

	dict_mem_fill_index_struct(index, heap, index_name, type, n_fields);

	dict_index_zip_pad_mutex_create_lazy(index);

	if (type & DICT_SPATIAL) {
		mutex_create(LATCH_ID_RTR_SSN_MUTEX, &index->rtr_ssn.mutex);
		index->rtr_track = static_cast<rtr_info_track_t*>(
					mem_heap_alloc(
						heap,
						sizeof(*index->rtr_track)));
		mutex_create(LATCH_ID_RTR_ACTIVE_MUTEX,
			     &index->rtr_track->rtr_active_mutex);
		index->rtr_track->rtr_active = UT_NEW_NOKEY(rtr_info_active());
	}

	return(index);
}

/**********************************************************************//**
Creates and initializes a foreign constraint memory object.
@return own: foreign constraint struct */
dict_foreign_t*
dict_mem_foreign_create(void)
/*=========================*/
{
	dict_foreign_t*	foreign;
	mem_heap_t*	heap;
	DBUG_ENTER("dict_mem_foreign_create");

	heap = mem_heap_create(100);

	foreign = static_cast<dict_foreign_t*>(
		mem_heap_zalloc(heap, sizeof(dict_foreign_t)));

	foreign->heap = heap;

	foreign->v_cols = NULL;

	DBUG_PRINT("dict_mem_foreign_create", ("heap: %p", heap));

	DBUG_RETURN(foreign);
}

/**********************************************************************//**
Sets the foreign_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, foreign_table_name_lookup
will point to foreign_table_name.  If 2, then another string is
allocated from foreign->heap and set to lower case. */
void
dict_mem_foreign_table_name_lookup_set(
/*===================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc)	/*!< in: is an alloc needed */
{
	if (innobase_get_lower_case_table_names() == 2) {
		if (do_alloc) {
			ulint	len;

			len = strlen(foreign->foreign_table_name) + 1;

			foreign->foreign_table_name_lookup =
				static_cast<char*>(
					mem_heap_alloc(foreign->heap, len));
		}
		strcpy(foreign->foreign_table_name_lookup,
		       foreign->foreign_table_name);
		innobase_casedn_str(foreign->foreign_table_name_lookup);
	} else {
		foreign->foreign_table_name_lookup
			= foreign->foreign_table_name;
	}
}

/**********************************************************************//**
Sets the referenced_table_name_lookup pointer based on the value of
lower_case_table_names.  If that is 0 or 1, referenced_table_name_lookup
will point to referenced_table_name.  If 2, then another string is
allocated from foreign->heap and set to lower case. */
void
dict_mem_referenced_table_name_lookup_set(
/*======================================*/
	dict_foreign_t*	foreign,	/*!< in/out: foreign struct */
	ibool		do_alloc)	/*!< in: is an alloc needed */
{
	if (innobase_get_lower_case_table_names() == 2) {
		if (do_alloc) {
			ulint	len;

			len = strlen(foreign->referenced_table_name) + 1;

			foreign->referenced_table_name_lookup =
				static_cast<char*>(
					mem_heap_alloc(foreign->heap, len));
		}
		strcpy(foreign->referenced_table_name_lookup,
		       foreign->referenced_table_name);
		innobase_casedn_str(foreign->referenced_table_name_lookup);
	} else {
		foreign->referenced_table_name_lookup
			= foreign->referenced_table_name;
	}
}

/** Fill the virtual column set with virtual column information
present in the given virtual index.
@param[in]	index	virtual index
@param[out]	v_cols	virtual column set. */
static
void
dict_mem_fill_vcol_has_index(
	const dict_index_t*	index,
	dict_vcol_set**		v_cols)
{
	for (ulint i = 0; i < index->table->n_v_cols; i++) {
		dict_v_col_t*	v_col = dict_table_get_nth_v_col(
					index->table, i);
		if (!v_col->m_col.ord_part) {
			continue;
		}

		dict_v_idx_list::iterator it;
		for (it = v_col->v_indexes->begin();
		     it != v_col->v_indexes->end(); ++it) {
			dict_v_idx_t	v_idx = *it;

			if (v_idx.index != index) {
				continue;
			}

			if (*v_cols == NULL) {
				*v_cols = UT_NEW_NOKEY(dict_vcol_set());
			}

			(*v_cols)->insert(v_col);
		}
	}
}

/** Fill the virtual column set with the virtual column of the index
if the index contains given column name.
@param[in]	col_name	column name
@param[in]	table		innodb table object
@param[out]	v_cols		set of virtual column information. */
static
void
dict_mem_fill_vcol_from_v_indexes(
	const char*		col_name,
	const dict_table_t*	table,
	dict_vcol_set**		v_cols)
{
	/* virtual column can't be Primary Key, so start with
	secondary index */
	for (dict_index_t* index = dict_table_get_next_index(
			dict_table_get_first_index(table));
		index;
		index = dict_table_get_next_index(index)) {

		/* Skip if the index have newly added
		virtual column because field name is NULL.
		Later virtual column set will be
		refreshed during loading of table. */
		if (!dict_index_has_virtual(index)
		    || index->has_new_v_col) {
			continue;
		}

		for (ulint i = 0; i < index->n_fields; i++) {
			dict_field_t*	field =
				dict_index_get_nth_field(index, i);

			if (strcmp(field->name, col_name) == 0) {
				dict_mem_fill_vcol_has_index(
					index, v_cols);
			}
		}
	}
}

/** Fill the virtual column set with virtual columns which have base columns
as the given col_name
@param[in]	col_name	column name
@param[in]	table		table object
@param[out]	v_cols		set of virtual columns. */
static
void
dict_mem_fill_vcol_set_for_base_col(
	const char*		col_name,
	const dict_table_t*	table,
	dict_vcol_set**		v_cols)
{
	for (ulint i = 0; i < table->n_v_cols; i++) {
		dict_v_col_t*	v_col = dict_table_get_nth_v_col(table, i);

		if (!v_col->m_col.ord_part) {
			continue;
		}

		for (ulint j = 0; j < v_col->num_base; j++) {
			if (strcmp(col_name, dict_table_get_col_name(
					table,
					v_col->base_col[j]->ind)) == 0) {

				if (*v_cols == NULL) {
					*v_cols = UT_NEW_NOKEY(dict_vcol_set());
				}

				(*v_cols)->insert(v_col);
			}
		}
	}
}

/** Fills the dependent virtual columns in a set.
Reason for being dependent are
1) FK can be present on base column of virtual columns
2) FK can be present on column which is a part of virtual index
@param[in,out]  foreign foreign key information. */
void
dict_mem_foreign_fill_vcol_set(
        dict_foreign_t* foreign)
{
	ulint	type = foreign->type;

	if (type == 0) {
		return;
	}

	for (ulint i = 0; i < foreign->n_fields; i++) {
		/** FK can be present on base columns
		of virtual columns. */
		dict_mem_fill_vcol_set_for_base_col(
			foreign->foreign_col_names[i],
			foreign->foreign_table,
			&foreign->v_cols);

		/** FK can be present on the columns
		which can be a part of virtual index. */
		dict_mem_fill_vcol_from_v_indexes(
			foreign->foreign_col_names[i],
			foreign->foreign_table,
			&foreign->v_cols);
	}
}

/** Fill virtual columns set in each fk constraint present in the table.
@param[in,out]	table	innodb table object. */
void
dict_mem_table_fill_foreign_vcol_set(
	dict_table_t*	table)
{
	dict_foreign_set	fk_set = table->foreign_set;
	dict_foreign_t*		foreign;

	dict_foreign_set::iterator it;
	for (it = fk_set.begin(); it != fk_set.end(); ++it) {
		foreign = *it;

		dict_mem_foreign_fill_vcol_set(foreign);
	}
}

/** Free the vcol_set from all foreign key constraint on the table.
@param[in,out]	table	innodb table object. */
void
dict_mem_table_free_foreign_vcol_set(
	dict_table_t*	table)
{
	dict_foreign_set	fk_set = table->foreign_set;
	dict_foreign_t*		foreign;

	dict_foreign_set::iterator it;
	for (it = fk_set.begin(); it != fk_set.end(); ++it) {

		foreign = *it;

		if (foreign->v_cols != NULL) {
			UT_DELETE(foreign->v_cols);
			foreign->v_cols = NULL;
		}
	}
}

/**********************************************************************//**
Adds a field definition to an index. NOTE: does not take a copy
of the column name if the field is a column. The memory occupied
by the column name may be released only after publishing the index. */
void
dict_mem_index_add_field(
/*=====================*/
	dict_index_t*	index,		/*!< in: index */
	const char*	name,		/*!< in: column name */
	ulint		prefix_len)	/*!< in: 0 or the column prefix length
					in a MySQL index like
					INDEX (textcol(25)) */
{
	dict_field_t*	field;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	index->n_def++;

	field = dict_index_get_nth_field(index, unsigned(index->n_def) - 1);

	field->name = name;
	field->prefix_len = (unsigned int) prefix_len;
}

/**********************************************************************//**
Frees an index memory object. */
void
dict_mem_index_free(
/*================*/
	dict_index_t*	index)	/*!< in: index */
{
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	dict_index_zip_pad_mutex_destroy(index);

	if (dict_index_is_spatial(index)) {
		rtr_info_active::iterator	it;
		rtr_info_t*			rtr_info;

		for (it = index->rtr_track->rtr_active->begin();
		     it != index->rtr_track->rtr_active->end(); ++it) {
			rtr_info = *it;

			rtr_info->index = NULL;
		}

		mutex_destroy(&index->rtr_ssn.mutex);
		mutex_destroy(&index->rtr_track->rtr_active_mutex);
		UT_DELETE(index->rtr_track->rtr_active);
	}

	dict_index_remove_from_v_col_list(index);
	mem_heap_free(index->heap);
}

/** Create a temporary tablename like "#sql-ibtid-inc where
  tid = the Table ID
  inc = a randomly initialized number that is incremented for each file
The table ID is a 64 bit integer, can use up to 20 digits, and is
initialized at bootstrap. The second number is 32 bits, can use up to 10
digits, and is initialized at startup to a randomly distributed number.
It is hoped that the combination of these two numbers will provide a
reasonably unique temporary file name.
@param[in]	heap	A memory heap
@param[in]	dbtab	Table name in the form database/table name
@param[in]	id	Table id
@return A unique temporary tablename suitable for InnoDB use */
char*
dict_mem_create_temporary_tablename(
	mem_heap_t*	heap,
	const char*	dbtab,
	table_id_t	id)
{
	size_t		size;
	char*		name;
	const char*	dbend   = strchr(dbtab, '/');
	ut_ad(dbend);
	size_t		dblen   = size_t(dbend - dbtab) + 1;

	size = dblen + (sizeof(TEMP_FILE_PREFIX) + 3 + 20);
	name = static_cast<char*>(mem_heap_alloc(heap, size));
	memcpy(name, dbtab, dblen);
	snprintf(name + dblen, size - dblen,
		 TEMP_FILE_PREFIX_INNODB UINT64PF, id);

	return(name);
}

/** Validate the search order in the foreign key set.
@param[in]	fk_set	the foreign key set to be validated
@return true if search order is fine in the set, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_foreign_set&	fk_set)
{
	dict_foreign_not_exists	not_exists(fk_set);

	dict_foreign_set::const_iterator it = std::find_if(
		fk_set.begin(), fk_set.end(), not_exists);

	if (it == fk_set.end()) {
		return(true);
	}

	dict_foreign_t*	foreign = *it;
	std::cerr << "Foreign key lookup failed: " << *foreign;
	std::cerr << fk_set;
	ut_ad(0);
	return(false);
}

/** Validate the search order in the foreign key sets of the table
(foreign_set and referenced_set).
@param[in]	table	table whose foreign key sets are to be validated
@return true if foreign key sets are fine, false otherwise. */
bool
dict_foreign_set_validate(
	const dict_table_t&	table)
{
	return(dict_foreign_set_validate(table.foreign_set)
	       && dict_foreign_set_validate(table.referenced_set));
}

std::ostream&
operator<< (std::ostream& out, const dict_foreign_t& foreign)
{
	out << "[dict_foreign_t: id='" << foreign.id << "'";

	if (foreign.foreign_table_name != NULL) {
		out << ",for: '" << foreign.foreign_table_name << "'";
	}

	out << "]";
	return(out);
}

std::ostream&
operator<< (std::ostream& out, const dict_foreign_set& fk_set)
{
	out << "[dict_foreign_set:";
	std::for_each(fk_set.begin(), fk_set.end(), dict_foreign_print(out));
	out << "]" << std::endl;
	return(out);
}

/** Reconstruct the clustered index fields. */
inline void dict_index_t::reconstruct_fields()
{
	DBUG_ASSERT(is_primary());

	n_fields += table->instant->n_dropped;
	n_def += table->instant->n_dropped;

	const unsigned n_first = first_user_field();

	dict_field_t* tfields = static_cast<dict_field_t*>(
		mem_heap_zalloc(heap, n_fields * sizeof *fields));

	memcpy(tfields, fields, n_first * sizeof *fields);

	n_nullable = 0;
	ulint n_core_null = 0;
	const bool comp = dict_table_is_comp(table);
	const auto* field_map_it = table->instant->field_map;
	for (unsigned i = n_first, j = 0; i < n_fields; ) {
		dict_field_t& f = tfields[i++];
		auto c = *field_map_it++;
		if (c.is_dropped()) {
			f.col = &table->instant->dropped[j++];
			DBUG_ASSERT(f.col->is_dropped());
			f.fixed_len = dict_col_get_fixed_size(f.col, comp);
		} else {
			DBUG_ASSERT(!c.is_not_null());
			const auto old = std::find_if(
				fields + n_first, fields + n_fields,
				[c](const dict_field_t& o)
				{ return o.col->ind == c.ind(); });
			ut_ad(old >= &fields[n_first]);
			ut_ad(old < &fields[n_fields]);
			DBUG_ASSERT(!old->prefix_len);
			DBUG_ASSERT(old->col == &table->cols[c.ind()]);
			f = *old;
		}

		f.col->clear_instant();
		if (f.col->is_nullable()) {
			n_nullable++;
			n_core_null += i <= n_core_fields;
		}
		f.col->unsigned_len = c.unsigned_len();
	}

	fields = tfields;
	n_core_null_bytes = UT_BITS_IN_BYTES(n_core_null);
}

/** Reconstruct dropped or reordered columns.
@param[in]	metadata	data from serialise_columns()
@param[in]	len		length of the metadata, in bytes
@return whether parsing the metadata failed */
bool dict_table_t::deserialise_columns(const byte* metadata, ulint len)
{
	DBUG_ASSERT(!instant);

	unsigned num_non_pk_fields = mach_read_from_4(metadata);
	metadata += 4;

	if (num_non_pk_fields >= REC_MAX_N_FIELDS - 3) {
		return true;
	}

	dict_index_t* index = UT_LIST_GET_FIRST(indexes);

	if (num_non_pk_fields < unsigned(index->n_fields)
	    - index->first_user_field()) {
		return true;
	}

	field_map_element_t* field_map = static_cast<field_map_element_t*>(
		mem_heap_alloc(heap,
			       num_non_pk_fields * sizeof *field_map));

	unsigned n_dropped_cols = 0;

	for (unsigned i = 0; i < num_non_pk_fields; i++) {
		auto c = field_map[i] = mach_read_from_2(metadata);
		metadata += 2;

		if (field_map[i].is_dropped()) {
			if (c.ind() > DICT_MAX_FIXED_COL_LEN + 1) {
				return true;
			}
			n_dropped_cols++;
		} else if (c >= n_cols) {
			return true;
		}
	}

	dict_col_t* dropped_cols = static_cast<dict_col_t*>(mem_heap_zalloc(
		heap, n_dropped_cols * sizeof(dict_col_t)));
	instant = new (mem_heap_alloc(heap, sizeof *instant)) dict_instant_t();
	instant->n_dropped = n_dropped_cols;
	instant->dropped = dropped_cols;
	instant->field_map = field_map;

	dict_col_t* col = dropped_cols;
	for (unsigned i = 0; i < num_non_pk_fields; i++) {
		if (field_map[i].is_dropped()) {
			auto fixed_len = field_map[i].ind();
			DBUG_ASSERT(fixed_len <= DICT_MAX_FIXED_COL_LEN + 1);
			(col++)->set_dropped(field_map[i].is_not_null(),
					     fixed_len == 1,
					     fixed_len > 1 ? fixed_len - 1
					     : 0);
		}
	}
	DBUG_ASSERT(col == &dropped_cols[n_dropped_cols]);

	UT_LIST_GET_FIRST(indexes)->reconstruct_fields();
	return false;
}

/** Check if record in clustered index is historical row.
@param[in]	rec	clustered row
@param[in]	offsets	offsets
@return true if row is historical */
bool
dict_index_t::vers_history_row(
	const rec_t*		rec,
	const ulint*		offsets)
{
	ut_ad(is_primary());

	ulint len;
	dict_col_t& col= table->cols[table->vers_end];
	ut_ad(col.vers_sys_end());
	ulint nfield = dict_col_get_clust_pos(&col, this);
	const byte *data = rec_get_nth_field(rec, offsets, nfield, &len);
	if (col.vers_native()) {
		ut_ad(len == sizeof trx_id_max_bytes);
		return 0 != memcmp(data, trx_id_max_bytes, len);
	}
	ut_ad(len == sizeof timestamp_max_bytes);
	return 0 != memcmp(data, timestamp_max_bytes, len);
}

/** Check if record in secondary index is historical row.
@param[in]	rec	record in a secondary index
@param[out]	history_row true if row is historical
@return true on error */
bool
dict_index_t::vers_history_row(
	const rec_t* rec,
	bool &history_row)
{
	ut_ad(!is_primary());

	bool error = false;
	mem_heap_t* heap = NULL;
	dict_index_t* clust_index = NULL;
	ulint offsets_[REC_OFFS_NORMAL_SIZE];
	ulint* offsets = offsets_;
	rec_offs_init(offsets_);

	mtr_t mtr;
	mtr.start();

	rec_t* clust_rec =
	    row_get_clust_rec(BTR_SEARCH_LEAF, rec, this, &clust_index, &mtr);
	if (clust_rec) {
		offsets = rec_get_offsets(clust_rec, clust_index, offsets, true,
					  ULINT_UNDEFINED, &heap);

		history_row = clust_index->vers_history_row(clust_rec, offsets);
        } else {
		ib::error() << "foreign constraints: secondary index is out of "
			       "sync";
		ut_ad(!"secondary index is out of sync");
		error = true;
	}
	mtr.commit();
	if (heap) {
		mem_heap_free(heap);
	}
	return(error);
}
