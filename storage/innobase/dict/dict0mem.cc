/*****************************************************************************

Copyright (c) 1996, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2021, MariaDB Corporation.

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

/** Create a table memory object.
@param name     table name
@param space    tablespace
@param n_cols   total number of columns (both virtual and non-virtual)
@param n_v_cols number of virtual columns
@param flags    table flags
@param flags2   table flags2
@return own: table object */
dict_table_t*
dict_mem_table_create(
	const char*	name,
	fil_space_t*	space,
	ulint		n_cols,
	ulint		n_v_cols,
	ulint		flags,
	ulint		flags2)
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
#ifdef BTR_CUR_HASH_ADAPT
	UT_LIST_INIT(table->freed_indexes, &dict_index_t::indexes);
#endif /* BTR_CUR_HASH_ADAPT */

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

	table->autoinc_lock = static_cast<ib_lock_t*>(
		mem_heap_alloc(heap, lock_get_size()));

	/* If the table has an FTS index or we are in the process
	of building one, create the table->fts */
	if (dict_table_has_fts_index(table)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID)) {
		table->fts = fts_create(table);
		table->fts->cache = fts_cache_create(table);
	} else {
		table->fts = NULL;
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
	ut_ad(UT_LIST_GET_LEN(table->indexes) == 0);
#ifdef BTR_CUR_HASH_ADAPT
	ut_ad(UT_LIST_GET_LEN(table->freed_indexes) == 0);
#endif /* BTR_CUR_HASH_ADAPT */
	ut_d(table->cached = FALSE);

	if (dict_table_has_fts_index(table)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID)) {
		if (table->fts) {
			fts_optimize_remove_table(table);

			fts_free(table);
		}
	}

	dict_mem_table_free_foreign_vcol_set(table);

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
	strncpy(from, s, sizeof from - 1);
	from[sizeof from - 1] = '\0';

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

			/* New index can be null if InnoDB already dropped
			the foreign index when FOREIGN_KEY_CHECKS is
			disabled */
			foreign->foreign_index = dict_foreign_find_index(
				foreign->foreign_table, NULL,
				foreign->foreign_col_names,
				foreign->n_fields, NULL, true, false,
				NULL, NULL, NULL);

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

		if (!foreign->referenced_index) {
			/* Referenced index could have been dropped
			when foreign_key_checks is disabled. In that case,
			rename the corresponding referenced_col_names and
			find the equivalent referenced index also */
			for (unsigned f = 0; f < foreign->n_fields; f++) {

				const char*& rc =
					foreign->referenced_col_names[f];
				if (strcmp(rc, from)) {
					continue;
				}

				if (to_len <= strlen(rc)) {
					memcpy(const_cast<char*>(rc), to,
					       to_len + 1);
				} else {
					rc = static_cast<char*>(
						mem_heap_dup(
							foreign->heap,
							to, to_len + 1));
				}
			}

			/* New index can be null if InnoDB already dropped
			the referenced index when FOREIGN_KEY_CHECKS is
			disabled */
			foreign->referenced_index = dict_foreign_find_index(
				foreign->referenced_table, NULL,
				foreign->referenced_col_names,
				foreign->n_fields, NULL, true, false,
				NULL, NULL, NULL);
			return;
		}


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
	column->def_val.data = NULL;
	column->def_val.len = UNIV_SQL_DEFAULT;
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

	mysql_mutex_init(0, &index->zip_pad.mutex, NULL);

	if (type & DICT_SPATIAL) {
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
		    || index->has_new_v_col()) {
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

	mysql_mutex_destroy(&index->zip_pad.mutex);

	if (dict_index_is_spatial(index)) {
		rtr_info_active::iterator	it;
		rtr_info_t*			rtr_info;

		for (it = index->rtr_track->rtr_active->begin();
		     it != index->rtr_track->rtr_active->end(); ++it) {
			rtr_info = *it;

			rtr_info->index = NULL;
		}

		mutex_destroy(&index->rtr_track->rtr_active_mutex);
		UT_DELETE(index->rtr_track->rtr_active);
	}

	dict_index_remove_from_v_col_list(index);
	mem_heap_free(index->heap);
}

/** Create a temporary tablename like "#sql-ibNNN".
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

/** Check whether fulltext index gets affected by foreign
key constraint. */
bool dict_foreign_t::affects_fulltext() const
{
  if (foreign_table == referenced_table || !foreign_table->fts)
    return false;

  for (ulint i= 0; i < n_fields; i++)
  {
    const dict_col_t *col= dict_index_get_nth_col(foreign_index, i);
    if (dict_table_is_fts_column(foreign_table->fts->indexes, col->ind,
                                 col->is_virtual()) != ULINT_UNDEFINED)
      return true;
  }

  return false;
}

/** Adjust clustered index metadata for instant ADD COLUMN.
@param[in]	clustered index definition after instant ADD COLUMN */
inline void dict_index_t::instant_add_field(const dict_index_t& instant)
{
	DBUG_ASSERT(is_primary());
	DBUG_ASSERT(instant.is_primary());
	DBUG_ASSERT(!instant.is_instant());
	DBUG_ASSERT(n_def == n_fields);
	DBUG_ASSERT(instant.n_def == instant.n_fields);

	DBUG_ASSERT(type == instant.type);
	DBUG_ASSERT(trx_id_offset == instant.trx_id_offset);
	DBUG_ASSERT(n_user_defined_cols == instant.n_user_defined_cols);
	DBUG_ASSERT(n_uniq == instant.n_uniq);
	DBUG_ASSERT(instant.n_fields > n_fields);
	DBUG_ASSERT(instant.n_def > n_def);
	DBUG_ASSERT(instant.n_nullable >= n_nullable);
	DBUG_ASSERT(instant.n_core_fields >= n_core_fields);
	DBUG_ASSERT(instant.n_core_null_bytes >= n_core_null_bytes);

	n_fields = instant.n_fields;
	n_def = instant.n_def;
	n_nullable = instant.n_nullable;
	fields = static_cast<dict_field_t*>(
		mem_heap_dup(heap, instant.fields, n_fields * sizeof *fields));

	ut_d(unsigned n_null = 0);

	for (unsigned i = 0; i < n_fields; i++) {
		DBUG_ASSERT(fields[i].same(instant.fields[i]));
		const dict_col_t* icol = instant.fields[i].col;
		DBUG_ASSERT(!icol->is_virtual());
		dict_col_t* col = fields[i].col = &table->cols[
			icol - instant.table->cols];
		fields[i].name = col->name(*table);
		ut_d(n_null += col->is_nullable());
	}

	ut_ad(n_null == n_nullable);
}

/** Adjust metadata for instant ADD COLUMN.
@param[in]	table	table definition after instant ADD COLUMN */
void dict_table_t::instant_add_column(const dict_table_t& table)
{
	DBUG_ASSERT(!table.cached);
	DBUG_ASSERT(table.n_def == table.n_cols);
	DBUG_ASSERT(table.n_t_def == table.n_t_cols);
	DBUG_ASSERT(n_def == n_cols);
	DBUG_ASSERT(n_t_def == n_t_cols);
	DBUG_ASSERT(table.n_cols > n_cols);
	ut_ad(mutex_own(&dict_sys->mutex));

	const char* end = table.col_names;
	for (unsigned i = table.n_cols; i--; ) end += strlen(end) + 1;

	col_names = static_cast<char*>(
		mem_heap_dup(heap, table.col_names,
			     ulint(end - table.col_names)));
	const dict_col_t* const old_cols = cols;
	const dict_col_t* const old_cols_end = cols + n_cols;
	cols = static_cast<dict_col_t*>(mem_heap_dup(heap, table.cols,
						     table.n_cols
						     * sizeof *cols));

	/* Preserve the default values of previously instantly
	added columns. */
	for (unsigned i = unsigned(n_cols) - DATA_N_SYS_COLS; i--; ) {
		cols[i].def_val = old_cols[i].def_val;
	}

	/* Copy the new default values to this->heap. */
	for (unsigned i = n_cols; i < table.n_cols; i++) {
		dict_col_t& c = cols[i - DATA_N_SYS_COLS];
		DBUG_ASSERT(c.is_instant());
		if (c.def_val.len == 0) {
			c.def_val.data = field_ref_zero;
		} else if (const void*& d = c.def_val.data) {
			d = mem_heap_dup(heap, d, c.def_val.len);
		} else {
			DBUG_ASSERT(c.def_val.len == UNIV_SQL_NULL);
		}
	}

	const unsigned old_n_cols = n_cols;
	const unsigned n_add = unsigned(table.n_cols - n_cols);

	n_t_def += n_add;
	n_t_cols += n_add;
	n_cols = table.n_cols;
	n_def = n_cols;

	for (unsigned i = n_v_def; i--; ) {
		const dict_v_col_t& v = v_cols[i];
		for (ulint n = v.num_base; n--; ) {
			dict_col_t*& base = v.base_col[n];
			if (!base->is_virtual()) {
				DBUG_ASSERT(base >= old_cols);
				size_t n = size_t(base - old_cols);
				DBUG_ASSERT(n + DATA_N_SYS_COLS < old_n_cols);
				base = &cols[n];
			}
		}
	}

	dict_index_t* index = dict_table_get_first_index(this);

	index->instant_add_field(*dict_table_get_first_index(&table));

	while ((index = dict_table_get_next_index(index)) != NULL) {
		for (unsigned i = 0; i < index->n_fields; i++) {
			dict_field_t& field = index->fields[i];
			if (field.col < old_cols
			    || field.col >= old_cols_end) {
				DBUG_ASSERT(field.col->is_virtual());
			} else {
				/* Secondary indexes may contain user
				columns and DB_ROW_ID (if there is
				GEN_CLUST_INDEX instead of PRIMARY KEY),
				but not DB_TRX_ID,DB_ROLL_PTR. */
				DBUG_ASSERT(field.col >= old_cols);
				size_t n = size_t(field.col - old_cols);
				DBUG_ASSERT(n + DATA_N_SYS_COLS <= old_n_cols);
				if (n + DATA_N_SYS_COLS >= old_n_cols) {
					/* Replace DB_ROW_ID */
					n += n_add;
				}
				field.col = &cols[n];
				DBUG_ASSERT(!field.col->is_virtual());
				field.name = field.col->name(*this);
			}
		}
	}
}

/** Roll back instant_add_column().
@param[in]	old_n_cols	original n_cols
@param[in]	old_cols	original cols
@param[in]	old_col_names	original col_names */
void
dict_table_t::rollback_instant(
	unsigned	old_n_cols,
	dict_col_t*	old_cols,
	const char*	old_col_names)
{
	ut_ad(mutex_own(&dict_sys->mutex));
	dict_index_t* index = indexes.start;
	/* index->is_instant() does not necessarily hold here, because
	the table may have been emptied */
	DBUG_ASSERT(old_n_cols >= DATA_N_SYS_COLS);
	DBUG_ASSERT(n_cols >= old_n_cols);
	DBUG_ASSERT(n_cols == n_def);
	DBUG_ASSERT(index->n_def == index->n_fields);

	const unsigned n_remove = n_cols - old_n_cols;

	for (unsigned i = index->n_fields - n_remove; i < index->n_fields;
	     i++) {
		if (index->fields[i].col->is_nullable()) {
			index->n_nullable--;
		}
	}

	index->n_fields -= n_remove;
	index->n_def = index->n_fields;
	if (index->n_core_fields > index->n_fields) {
		index->n_core_fields = index->n_fields;
		index->n_core_null_bytes
			= UT_BITS_IN_BYTES(unsigned(index->n_nullable));
	}

	const dict_col_t* const new_cols = cols;
	const dict_col_t* const new_cols_end = cols + n_cols;

	cols = old_cols;
	col_names = old_col_names;
	n_cols = old_n_cols;
	n_def = old_n_cols;
	n_t_def -= n_remove;
	n_t_cols -= n_remove;

	for (unsigned i = n_v_def; i--; ) {
		const dict_v_col_t& v = v_cols[i];
		for (ulint n = v.num_base; n--; ) {
			dict_col_t*& base = v.base_col[n];
			if (!base->is_virtual()) {
				base = &cols[base - new_cols];
			}
		}
	}

	do {
		for (unsigned i = 0; i < index->n_fields; i++) {
			dict_field_t& field = index->fields[i];
			if (field.col < new_cols
			    || field.col >= new_cols_end) {
				DBUG_ASSERT(field.col->is_virtual());
			} else {
				DBUG_ASSERT(field.col >= new_cols);
				size_t n = size_t(field.col - new_cols);
				DBUG_ASSERT(n <= n_cols);
				if (n + DATA_N_SYS_COLS >= n_cols) {
					n -= n_remove;
				}
				field.col = &cols[n];
				DBUG_ASSERT(!field.col->is_virtual());
				field.name = field.col->name(*this);
			}
		}
	} while ((index = dict_table_get_next_index(index)) != NULL);
}

/** Trim the instantly added columns when an insert into SYS_COLUMNS
is rolled back during ALTER TABLE or recovery.
@param[in]	n	number of surviving non-system columns */
void dict_table_t::rollback_instant(unsigned n)
{
	ut_ad(mutex_own(&dict_sys->mutex));
	dict_index_t* index = indexes.start;
	DBUG_ASSERT(index->is_instant());
	DBUG_ASSERT(index->n_def == index->n_fields);
	DBUG_ASSERT(n_cols == n_def);
	DBUG_ASSERT(n >= index->n_uniq);
	DBUG_ASSERT(n_cols > n + DATA_N_SYS_COLS);
	const unsigned n_remove = n_cols - n - DATA_N_SYS_COLS;

	char* names = const_cast<char*>(dict_table_get_col_name(this, n));
	const char* sys = names;
	for (unsigned i = n_remove; i--; ) {
		sys += strlen(sys) + 1;
	}
	static const char system[] = "DB_ROW_ID\0DB_TRX_ID\0DB_ROLL_PTR";
	DBUG_ASSERT(!memcmp(sys, system, sizeof system));
	for (unsigned i = index->n_fields - n_remove; i < index->n_fields;
	     i++) {
		if (index->fields[i].col->is_nullable()) {
			index->n_nullable--;
		}
	}
	index->n_fields -= n_remove;
	index->n_def = index->n_fields;
	memmove(names, sys, sizeof system);
	memmove(cols + n, cols + n_cols - DATA_N_SYS_COLS,
		DATA_N_SYS_COLS * sizeof *cols);
	n_cols -= n_remove;
	n_def = n_cols;
	n_t_cols -= n_remove;
	n_t_def -= n_remove;

	for (unsigned i = DATA_N_SYS_COLS; i--; ) {
		cols[n_cols - i].ind--;
	}

	if (dict_index_is_auto_gen_clust(index)) {
		DBUG_ASSERT(index->n_uniq == 1);
		dict_field_t* field = index->fields;
		field->name = sys;
		field->col = dict_table_get_sys_col(this, DATA_ROW_ID);
		field++;
		field->name = sys + sizeof "DB_ROW_ID";
		field->col = dict_table_get_sys_col(this, DATA_TRX_ID);
		field++;
		field->name = sys + sizeof "DB_ROW_ID\0DB_TRX_ID";
		field->col = dict_table_get_sys_col(this, DATA_ROLL_PTR);

		/* Replace the DB_ROW_ID column in secondary indexes. */
		while ((index = dict_table_get_next_index(index)) != NULL) {
			field = &index->fields[index->n_fields - 1];
			DBUG_ASSERT(field->col->mtype == DATA_SYS);
			DBUG_ASSERT(field->col->prtype
				    == DATA_NOT_NULL + DATA_TRX_ID);
			field->col--;
			field->name = sys;
		}

		return;
	}

	dict_field_t* field = &index->fields[index->n_uniq];
	field->name = sys + sizeof "DB_ROW_ID";
	field->col = dict_table_get_sys_col(this, DATA_TRX_ID);
	field++;
	field->name = sys + sizeof "DB_ROW_ID\0DB_TRX_ID";
	field->col = dict_table_get_sys_col(this, DATA_ROLL_PTR);
}


/** Check if record in clustered index is historical row.
@param[in]	rec	clustered row
@param[in]	offsets	offsets
@return true if row is historical */
bool
dict_index_t::vers_history_row(
	const rec_t*		rec,
	const rec_offs*		offsets)
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

	/*
	  Get row_end from clustered index

	  TODO (optimization): row_end can be taken from unique secondary index
	  as well. For that dict_index_t::vers_end member should be added and
	  updated at index init (dict_index_build_internal_non_clust()).

	  Test case:

		create or replace table t1 (x int unique, y int unique,
			foreign key r (y) references t1 (x))
			with system versioning engine innodb;
		insert into t1 values (1, 1);
	 */
	bool error = false;
	mem_heap_t* heap = NULL;
	dict_index_t* clust_index = NULL;
	rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs* offsets = offsets_;
	rec_offs_init(offsets_);

	mtr_t mtr;
	mtr.start();

	rec_t* clust_rec =
	    row_get_clust_rec(BTR_SEARCH_LEAF, rec, this, &clust_index, &mtr);
	if (clust_rec) {
		offsets = rec_get_offsets(clust_rec, clust_index, offsets,
					  clust_index->n_core_fields,
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
