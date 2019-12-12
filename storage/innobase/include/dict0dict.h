/*****************************************************************************

Copyright (c) 1996, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2019, MariaDB Corporation.

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
@file include/dict0dict.h
Data dictionary system

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#ifndef dict0dict_h
#define dict0dict_h

#include "data0data.h"
#include "dict0mem.h"
#include "fsp0fsp.h"
#include <deque>

extern bool innodb_table_stats_not_found;
extern bool innodb_index_stats_not_found;

/** the first table or index ID for other than hard-coded system tables */
constexpr uint8_t DICT_HDR_FIRST_ID= 10;

/********************************************************************//**
Get the database name length in a table name.
@return database name length */
ulint
dict_get_db_name_len(
/*=================*/
	const char*	name)	/*!< in: table name in the form
				dbname '/' tablename */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Open a table from its database and table name, this is currently used by
foreign constraint parser to get the referenced table.
@return complete table name with database and table name, allocated from
heap memory passed in */
char*
dict_get_referenced_table(
/*======================*/
	const char*	name,		/*!< in: foreign key table name */
	const char*	database_name,	/*!< in: table db name */
	ulint		database_name_len,/*!< in: db name length */
	const char*	table_name,	/*!< in: table name */
	ulint		table_name_len,	/*!< in: table name length */
	dict_table_t**	table,		/*!< out: table object or NULL */
	mem_heap_t*	heap);		/*!< in: heap memory */
/*********************************************************************//**
Frees a foreign key struct. */
void
dict_foreign_free(
/*==============*/
	dict_foreign_t*	foreign);	/*!< in, own: foreign key struct */
/*********************************************************************//**
Finds the highest [number] for foreign key constraints of the table. Looks
only at the >= 4.0.18-format id's, which are of the form
databasename/tablename_ibfk_[number].
@return highest number, 0 if table has no new format foreign key constraints */
ulint
dict_table_get_highest_foreign_id(
/*==============================*/
	dict_table_t*	table);		/*!< in: table in the dictionary
					memory cache */
/********************************************************************//**
Return the end of table name where we have removed dbname and '/'.
@return table name */
const char*
dict_remove_db_name(
/*================*/
	const char*	name)	/*!< in: table name in the form
				dbname '/' tablename */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Operation to perform when opening a table */
enum dict_table_op_t {
	/** Expect the tablespace to exist. */
	DICT_TABLE_OP_NORMAL = 0,
	/** Drop any orphan indexes after an aborted online index creation */
	DICT_TABLE_OP_DROP_ORPHAN,
	/** Silently load the tablespace if it does not exist,
	and do not load the definitions of incomplete indexes. */
	DICT_TABLE_OP_LOAD_TABLESPACE,
	/** Open the table only if it's in table cache. */
	DICT_TABLE_OP_OPEN_ONLY_IF_CACHED
};

/**********************************************************************//**
Returns a table object based on table id.
@return table, NULL if does not exist */
dict_table_t*
dict_table_open_on_id(
/*==================*/
	table_id_t	table_id,	/*!< in: table id */
	ibool		dict_locked,	/*!< in: TRUE=data dictionary locked */
	dict_table_op_t	table_op)	/*!< in: operation to perform */
	MY_ATTRIBUTE((warn_unused_result));

/**********************************************************************//**
Returns a table object based on table id.
@return	table, NULL if does not exist */
dict_table_t* dict_table_open_on_index_id(index_id_t index_id)
	__attribute__((warn_unused_result));
/********************************************************************//**
Decrements the count of open handles to a table. */
void
dict_table_close(
/*=============*/
	dict_table_t*	table,		/*!< in/out: table */
	ibool		dict_locked,	/*!< in: TRUE=data dictionary locked */
	ibool		try_drop)	/*!< in: TRUE=try to drop any orphan
					indexes after an aborted online
					index creation */
	MY_ATTRIBUTE((nonnull));
/*********************************************************************//**
Closes the only open handle to a table and drops a table while assuring
that dict_sys.mutex is held the whole time.  This assures that the table
is not evicted after the close when the count of open handles goes to zero.
Because dict_sys.mutex is held, we do not need to call prevent_eviction(). */
void
dict_table_close_and_drop(
/*======================*/
	trx_t*		trx,		/*!< in: data dictionary transaction */
	dict_table_t*	table);		/*!< in/out: table */

/*********************************************************************//**
Gets the minimum number of bytes per character.
@return minimum multi-byte char size, in bytes */
UNIV_INLINE
ulint
dict_col_get_mbminlen(
/*==================*/
	const dict_col_t*	col)	/*!< in: column */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Gets the maximum number of bytes per character.
@return maximum multi-byte char size, in bytes */
UNIV_INLINE
ulint
dict_col_get_mbmaxlen(
/*==================*/
	const dict_col_t*	col)	/*!< in: column */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Gets the column data type. */
UNIV_INLINE
void
dict_col_copy_type(
/*===============*/
	const dict_col_t*	col,	/*!< in: column */
	dtype_t*		type);	/*!< out: data type */

/**********************************************************************//**
Determine bytes of column prefix to be stored in the undo log. Please
note that if !dict_table_has_atomic_blobs(table), no prefix
needs to be stored in the undo log.
@return bytes of column prefix to be stored in the undo log */
UNIV_INLINE
ulint
dict_max_field_len_store_undo(
/*==========================*/
	dict_table_t*		table,	/*!< in: table */
	const dict_col_t*	col)	/*!< in: column which index prefix
					is based on */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Determine maximum bytes of a virtual column need to be stored
in the undo log.
@param[in]	table		dict_table_t for the table
@param[in]	col_no		virtual column number
@return maximum bytes of virtual column to be stored in the undo log */
UNIV_INLINE
ulint
dict_max_v_field_len_store_undo(
	dict_table_t*		table,
	ulint			col_no);

#ifdef UNIV_DEBUG
/*********************************************************************//**
Assert that a column and a data type match.
@return TRUE */
UNIV_INLINE
ibool
dict_col_type_assert_equal(
/*=======================*/
	const dict_col_t*	col,	/*!< in: column */
	const dtype_t*		type)	/*!< in: data type */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
#endif /* UNIV_DEBUG */

/***********************************************************************//**
Returns the minimum size of the column.
@return minimum size */
UNIV_INLINE
ulint
dict_col_get_min_size(
/*==================*/
	const dict_col_t*	col)	/*!< in: column */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************************//**
Returns the maximum size of the column.
@return maximum size */
UNIV_INLINE
ulint
dict_col_get_max_size(
/*==================*/
	const dict_col_t*	col)	/*!< in: column */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************************//**
Returns the size of a fixed size column, 0 if not a fixed size column.
@return fixed size, or 0 */
UNIV_INLINE
ulint
dict_col_get_fixed_size(
/*====================*/
	const dict_col_t*	col,	/*!< in: column */
	ulint			comp)	/*!< in: nonzero=ROW_FORMAT=COMPACT  */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************************//**
Returns the ROW_FORMAT=REDUNDANT stored SQL NULL size of a column.
For fixed length types it is the fixed length of the type, otherwise 0.
@return SQL null storage size in ROW_FORMAT=REDUNDANT */
UNIV_INLINE
ulint
dict_col_get_sql_null_size(
/*=======================*/
	const dict_col_t*	col,	/*!< in: column */
	ulint			comp)	/*!< in: nonzero=ROW_FORMAT=COMPACT  */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Gets the column number.
@return col->ind, table column position (starting from 0) */
UNIV_INLINE
ulint
dict_col_get_no(
/*============*/
	const dict_col_t*	col)	/*!< in: column */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Gets the column position in the clustered index. */
UNIV_INLINE
ulint
dict_col_get_clust_pos(
/*===================*/
	const dict_col_t*	col,		/*!< in: table column */
	const dict_index_t*	clust_index)	/*!< in: clustered index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Gets the column position in the given index.
@param[in]	col	table column
@param[in]	index	index to be searched for column
@return position of column in the given index. */
UNIV_INLINE
ulint
dict_col_get_index_pos(
	const dict_col_t*	col,
	const dict_index_t*	index)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/****************************************************************//**
If the given column name is reserved for InnoDB system columns, return
TRUE.
@return TRUE if name is reserved */
ibool
dict_col_name_is_reserved(
/*======================*/
	const char*	name)	/*!< in: column name */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Unconditionally set the AUTO_INCREMENT counter.
@param[in,out]	table	table or partition
@param[in]	value	next available AUTO_INCREMENT value */
MY_ATTRIBUTE((nonnull))
UNIV_INLINE
void
dict_table_autoinc_initialize(dict_table_t* table, ib_uint64_t value)
{
	ut_ad(mutex_own(&table->autoinc_mutex));
	table->autoinc = value;
}

/**
@param[in]	table	table or partition
@return the next AUTO_INCREMENT counter value
@retval	0	if AUTO_INCREMENT is not yet initialized */
MY_ATTRIBUTE((nonnull, warn_unused_result))
UNIV_INLINE
ib_uint64_t
dict_table_autoinc_read(const dict_table_t* table)
{
	ut_ad(mutex_own(&table->autoinc_mutex));
	return(table->autoinc);
}

/** Update the AUTO_INCREMENT sequence if the value supplied is greater
than the current value.
@param[in,out]	table	table or partition
@param[in]	value	AUTO_INCREMENT value that was assigned to a row
@return	whether the AUTO_INCREMENT sequence was updated */
MY_ATTRIBUTE((nonnull))
UNIV_INLINE
bool
dict_table_autoinc_update_if_greater(dict_table_t* table, ib_uint64_t value)
{
	ut_ad(mutex_own(&table->autoinc_mutex));

	if (value > table->autoinc) {

		table->autoinc = value;
		return(true);
	}

	return(false);
}

/**********************************************************************//**
Adds system columns to a table object. */
void
dict_table_add_system_columns(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table */
	mem_heap_t*	heap)	/*!< in: temporary heap */
	MY_ATTRIBUTE((nonnull));
/**********************************************************************//**
Renames a table object.
@return TRUE if success */
dberr_t
dict_table_rename_in_cache(
/*=======================*/
	dict_table_t*	table,		/*!< in/out: table */
	const char*	new_name,	/*!< in: new name */
	bool		rename_also_foreigns,
					/*!< in: in ALTER TABLE we want
					to preserve the original table name
					in constraints which reference it */
	bool		replace_new_file = false)
					/*!< in: whether to replace the
					file with the new name
					(as part of rolling back TRUNCATE) */
	MY_ATTRIBUTE((nonnull));

/** Removes an index from the dictionary cache.
@param[in,out]	table	table whose index to remove
@param[in,out]	index	index to remove, this object is destroyed and must not
be accessed by the caller afterwards */
void
dict_index_remove_from_cache(
	dict_table_t*	table,
	dict_index_t*	index);

/**********************************************************************//**
Change the id of a table object in the dictionary cache. This is used in
DISCARD TABLESPACE. */
void
dict_table_change_id_in_cache(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table object already in cache */
	table_id_t	new_id)	/*!< in: new id to set */
	MY_ATTRIBUTE((nonnull));
/**********************************************************************//**
Removes a foreign constraint struct from the dictionary cache. */
void
dict_foreign_remove_from_cache(
/*===========================*/
	dict_foreign_t*	foreign)	/*!< in, own: foreign constraint */
	MY_ATTRIBUTE((nonnull));
/**********************************************************************//**
Adds a foreign key constraint object to the dictionary cache. May free
the object if there already is an object with the same identifier in.
At least one of foreign table or referenced table must already be in
the dictionary cache!
@return DB_SUCCESS or error code */
dberr_t
dict_foreign_add_to_cache(
/*======================*/
	dict_foreign_t*		foreign,
				/*!< in, own: foreign key constraint */
	const char**		col_names,
				/*!< in: column names, or NULL to use
				foreign->foreign_table->col_names */
	bool			check_charsets,
				/*!< in: whether to check charset
				compatibility */
	dict_err_ignore_t	ignore_err)
				/*!< in: error to be ignored */
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));
/*********************************************************************//**
Checks if a table is referenced by foreign keys.
@return TRUE if table is referenced by a foreign key */
ibool
dict_table_is_referenced_by_foreign_key(
/*====================================*/
	const dict_table_t*	table)	/*!< in: InnoDB table */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/**********************************************************************//**
Replace the index passed in with another equivalent index in the
foreign key lists of the table.
@return whether all replacements were found */
bool
dict_foreign_replace_index(
/*=======================*/
	dict_table_t*		table,  /*!< in/out: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const dict_index_t*	index)	/*!< in: index to be replaced */
	MY_ATTRIBUTE((nonnull(1,3), warn_unused_result));
/** Scans a table create SQL string and adds to the data dictionary
the foreign key constraints declared in the string. This function
should be called after the indexes for a table have been created.
Each foreign key constraint must be accompanied with indexes in
bot participating tables. The indexes are allowed to contain more
fields than mentioned in the constraint.

@param[in]	trx		transaction
@param[in]	sql_string	table create statement where
				foreign keys are declared like:
				FOREIGN KEY (a, b) REFERENCES table2(c, d),
				table2 can be written also with the database
				name before it: test.table2; the default
				database id the database of parameter name
@param[in]	sql_length	length of sql_string
@param[in]	name		table full name in normalized form
@param[in]	reject_fks	if TRUE, fail with error code
				DB_CANNOT_ADD_CONSTRAINT if any
				foreign keys are found.
@return error code or DB_SUCCESS */
dberr_t
dict_create_foreign_constraints(
	trx_t*			trx,
	const char*		sql_string,
	size_t			sql_length,
	const char*		name,
	ibool			reject_fks)
	MY_ATTRIBUTE((warn_unused_result));
/**********************************************************************//**
Parses the CONSTRAINT id's to be dropped in an ALTER TABLE statement.
@return DB_SUCCESS or DB_CANNOT_DROP_CONSTRAINT if syntax error or the
constraint id does not match */
dberr_t
dict_foreign_parse_drop_constraints(
/*================================*/
	mem_heap_t*	heap,			/*!< in: heap from which we can
						allocate memory */
	trx_t*		trx,			/*!< in: transaction */
	dict_table_t*	table,			/*!< in: table */
	ulint*		n,			/*!< out: number of constraints
						to drop */
	const char***	constraints_to_drop)	/*!< out: id's of the
						constraints to drop */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/**********************************************************************//**
Returns a table object and increments its open handle count.
NOTE! This is a high-level function to be used mainly from outside the
'dict' directory. Inside this directory dict_table_get_low
is usually the appropriate function.
@param[in] table_name Table name
@param[in] dict_locked TRUE=data dictionary locked
@param[in] try_drop TRUE=try to drop any orphan indexes after
				an aborted online index creation
@param[in] ignore_err error to be ignored when loading the table
@return table, NULL if does not exist */
dict_table_t*
dict_table_open_on_name(
	const char*		table_name,
	ibool			dict_locked,
	ibool			try_drop,
	dict_err_ignore_t	ignore_err)
	MY_ATTRIBUTE((warn_unused_result));

/** Outcome of dict_foreign_find_index() or dict_foreign_qualify_index() */
enum fkerr_t
{
  /** A backing index was found for a FOREIGN KEY constraint */
  FK_SUCCESS = 0,
  /** There is no index that covers the columns in the constraint. */
  FK_INDEX_NOT_FOUND,
  /** The index is for a prefix index, not a full column. */
  FK_IS_PREFIX_INDEX,
  /** A condition of SET NULL conflicts with a NOT NULL column. */
  FK_COL_NOT_NULL,
  /** The column types do not match */
  FK_COLS_NOT_EQUAL
};

/*********************************************************************//**
Tries to find an index whose first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return matching index, NULL if not found */
dict_index_t*
dict_foreign_find_index(
/*====================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error = NULL,	/*!< out: error code */
	ulint*			err_col_no = NULL,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index = NULL)
					/*!< out: index where error
					happened */

	MY_ATTRIBUTE((nonnull(1,3), warn_unused_result));

/** Returns a virtual column's name.
@param[in]	table		table object
@param[in]	col_nr		virtual column number(nth virtual column)
@return column name. */
const char*
dict_table_get_v_col_name(
	const dict_table_t*	table,
	ulint			col_nr);

/** Check if the table has a given column.
@param[in]	table		table object
@param[in]	col_name	column name
@param[in]	col_nr		column number guessed, 0 as default
@return column number if the table has the specified column,
otherwise table->n_def */
ulint
dict_table_has_column(
	const dict_table_t*	table,
	const char*		col_name,
	ulint			col_nr = 0);

/**********************************************************************//**
Outputs info on foreign keys of a table. */
std::string
dict_print_info_on_foreign_keys(
/*============================*/
	ibool		create_table_format, /*!< in: if TRUE then print in
				a format suitable to be inserted into
				a CREATE TABLE, otherwise in the format
				of SHOW TABLE STATUS */
	trx_t*		trx,	/*!< in: transaction */
	dict_table_t*	table);	/*!< in: table */

/**********************************************************************//**
Outputs info on a foreign key of a table in a format suitable for
CREATE TABLE. */
std::string
dict_print_info_on_foreign_key_in_create_format(
/*============================================*/
	trx_t*		trx,		/*!< in: transaction */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	ibool		add_newline);	/*!< in: whether to add a newline */

/*********************************************************************//**
Tries to find an index whose first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return matching index, NULL if not found */
bool
dict_foreign_qualify_index(
/*====================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	index,	/*!< in: index to check */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error,	/*!< out: error code */
	ulint*			err_col_no,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index)
					/*!< out: index where error
					happened */
	MY_ATTRIBUTE((nonnull(1,3), warn_unused_result));
#ifdef UNIV_DEBUG
/********************************************************************//**
Gets the first index on the table (the clustered index).
@return index, NULL if none exists */
UNIV_INLINE
dict_index_t*
dict_table_get_first_index(
/*=======================*/
	const dict_table_t*	table)	/*!< in: table */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Gets the last index on the table.
@return index, NULL if none exists */
UNIV_INLINE
dict_index_t*
dict_table_get_last_index(
/*=======================*/
	const dict_table_t*	table)	/*!< in: table */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Gets the next index on the table.
@return index, NULL if none left */
UNIV_INLINE
dict_index_t*
dict_table_get_next_index(
/*======================*/
	const dict_index_t*	index)	/*!< in: index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
#else /* UNIV_DEBUG */
# define dict_table_get_first_index(table) UT_LIST_GET_FIRST((table)->indexes)
# define dict_table_get_last_index(table) UT_LIST_GET_LAST((table)->indexes)
# define dict_table_get_next_index(index) UT_LIST_GET_NEXT(indexes, index)
#endif /* UNIV_DEBUG */

/* Skip corrupted index */
#define dict_table_skip_corrupt_index(index)			\
	while (index && index->is_corrupted()) {		\
		index = dict_table_get_next_index(index);	\
	}

/* Get the next non-corrupt index */
#define dict_table_next_uncorrupted_index(index)		\
do {								\
	index = dict_table_get_next_index(index);		\
	dict_table_skip_corrupt_index(index);			\
} while (0)

#define dict_index_is_clust(index) (index)->is_clust()
#define dict_index_is_auto_gen_clust(index) (index)->is_gen_clust()
#define dict_index_is_unique(index) (index)->is_unique()
#define dict_index_is_spatial(index) (index)->is_spatial()
#define dict_index_is_ibuf(index) (index)->is_ibuf()
#define dict_index_is_sec_or_ibuf(index) !(index)->is_primary()
#define dict_index_has_virtual(index) (index)->has_virtual()

/** Get all the FTS indexes on a table.
@param[in]	table	table
@param[out]	indexes	all FTS indexes on this table
@return number of FTS indexes */
ulint
dict_table_get_all_fts_indexes(
	const dict_table_t*	table,
	ib_vector_t*		indexes);

/********************************************************************//**
Gets the number of user-defined non-virtual columns in a table in the
dictionary cache.
@return number of user-defined (e.g., not ROW_ID) non-virtual
columns of a table */
UNIV_INLINE
ulint
dict_table_get_n_user_cols(
/*=======================*/
	const dict_table_t*	table)	/*!< in: table */
	MY_ATTRIBUTE((warn_unused_result));
/********************************************************************//**
Gets the number of all non-virtual columns (also system) in a table
in the dictionary cache.
@return number of columns of a table */
UNIV_INLINE
ulint
dict_table_get_n_cols(
/*==================*/
	const dict_table_t*	table)	/*!< in: table */
	MY_ATTRIBUTE((warn_unused_result));

/** Gets the number of virtual columns in a table in the dictionary cache.
@param[in]	table	the table to check
@return number of virtual columns of a table */
UNIV_INLINE
ulint
dict_table_get_n_v_cols(
	const dict_table_t*	table);

/** Check if a table has indexed virtual columns
@param[in]	table	the table to check
@return true is the table has indexed virtual columns */
UNIV_INLINE
bool
dict_table_has_indexed_v_cols(
	const dict_table_t*	table);

/********************************************************************//**
Gets the approximately estimated number of rows in the table.
@return estimated number of rows */
UNIV_INLINE
ib_uint64_t
dict_table_get_n_rows(
/*==================*/
	const dict_table_t*	table)	/*!< in: table */
	MY_ATTRIBUTE((warn_unused_result));
/********************************************************************//**
Increment the number of rows in the table by one.
Notice that this operation is not protected by any latch, the number is
approximate. */
UNIV_INLINE
void
dict_table_n_rows_inc(
/*==================*/
	dict_table_t*	table)	/*!< in/out: table */
	MY_ATTRIBUTE((nonnull));
/********************************************************************//**
Decrement the number of rows in the table by one.
Notice that this operation is not protected by any latch, the number is
approximate. */
UNIV_INLINE
void
dict_table_n_rows_dec(
/*==================*/
	dict_table_t*	table)	/*!< in/out: table */
	MY_ATTRIBUTE((nonnull));

/** Get nth virtual column
@param[in]	table	target table
@param[in]	col_nr	column number in MySQL Table definition
@return dict_v_col_t ptr */
dict_v_col_t*
dict_table_get_nth_v_col_mysql(
	const dict_table_t*	table,
	ulint			col_nr);

#ifdef UNIV_DEBUG
/********************************************************************//**
Gets the nth column of a table.
@return pointer to column object */
UNIV_INLINE
dict_col_t*
dict_table_get_nth_col(
/*===================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			pos)	/*!< in: position of column */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Gets the nth virtual column of a table.
@param[in]	table	table
@param[in]	pos	position of virtual column
@return pointer to virtual column object */
UNIV_INLINE
dict_v_col_t*
dict_table_get_nth_v_col(
        const dict_table_t*	table,
        ulint			pos);
/********************************************************************//**
Gets the given system column of a table.
@return pointer to column object */
UNIV_INLINE
dict_col_t*
dict_table_get_sys_col(
/*===================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			sys)	/*!< in: DATA_ROW_ID, ... */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
#else /* UNIV_DEBUG */
#define dict_table_get_nth_col(table, pos)	(&(table)->cols[pos])
#define dict_table_get_sys_col(table, sys)	\
	&(table)->cols[(table)->n_cols + (sys) - DATA_N_SYS_COLS]
/* Get nth virtual columns */
#define dict_table_get_nth_v_col(table, pos)	(&(table)->v_cols[pos])
#endif /* UNIV_DEBUG */
/** Wrapper function.
@see dict_col_t::name()
@param[in]	table	table
@param[in]	col_nr	column number in table
@return	column name */
inline
const char*
dict_table_get_col_name(const dict_table_t* table, ulint col_nr)
{
	return(dict_table_get_nth_col(table, col_nr)->name(*table));
}

/********************************************************************//**
Gets the given system column number of a table.
@return column number */
UNIV_INLINE
ulint
dict_table_get_sys_col_no(
/*======================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			sys)	/*!< in: DATA_ROW_ID, ... */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/********************************************************************//**
Returns the minimum data size of an index record.
@return minimum data size in bytes */
UNIV_INLINE
ulint
dict_index_get_min_size(
/*====================*/
	const dict_index_t*	index)	/*!< in: index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#define dict_table_is_comp(table) (table)->not_redundant()

/** Determine if a table uses atomic BLOBs (no locally stored prefix).
@param[in]	table	InnoDB table
@return whether BLOBs are atomic */
inline
bool
dict_table_has_atomic_blobs(const dict_table_t* table)
{
	return(DICT_TF_HAS_ATOMIC_BLOBS(table->flags));
}

/** @return potential max length stored inline for externally stored fields */
inline size_t dict_table_t::get_overflow_field_local_len() const
{
	if (dict_table_has_atomic_blobs(this)) {
		/* ROW_FORMAT=DYNAMIC or ROW_FORMAT=COMPRESSED: do not
		store any BLOB prefix locally */
		return BTR_EXTERN_FIELD_REF_SIZE;
	}
	/* up to MySQL 5.1: store a 768-byte prefix locally */
	return BTR_EXTERN_FIELD_REF_SIZE + DICT_ANTELOPE_MAX_INDEX_COL_LEN;
}

/** Set the various values in a dict_table_t::flags pointer.
@param[in,out]	flags,		Pointer to a 4 byte Table Flags
@param[in]	format,		File Format
@param[in]	zip_ssize	Zip Shift Size
@param[in]	use_data_dir	Table uses DATA DIRECTORY
@param[in]	page_compressed Table uses page compression
@param[in]	page_compression_level Page compression level */
UNIV_INLINE
void
dict_tf_set(
	ulint*		flags,
	rec_format_t	format,
	ulint		zip_ssize,
	bool		use_data_dir,
	bool		page_compressed,
	ulint		page_compression_level);

/** Convert a 32 bit integer table flags to the 32 bit FSP Flags.
Fsp Flags are written into the tablespace header at the offset
FSP_SPACE_FLAGS and are also stored in the fil_space_t::flags field.
The following chart shows the translation of the low order bit.
Other bits are the same.
========================= Low order bit ==========================
                    | REDUNDANT | COMPACT | COMPRESSED | DYNAMIC
dict_table_t::flags |     0     |    1    |     1      |    1
fil_space_t::flags  |     0     |    0    |     1      |    1
==================================================================
@param[in]	table_flags	dict_table_t::flags
@return tablespace flags (fil_space_t::flags) */
UNIV_INLINE
ulint
dict_tf_to_fsp_flags(ulint table_flags)
	MY_ATTRIBUTE((const));


/** Extract the ROW_FORMAT=COMPRESSED page size from table flags.
@param[in]	flags	flags
@return ROW_FORMAT=COMPRESSED page size
@retval	0 if not compressed */
inline ulint dict_tf_get_zip_size(ulint flags)
{
	flags &= DICT_TF_MASK_ZIP_SSIZE;
	return flags
		? (UNIV_ZIP_SIZE_MIN >> 1)
		<< (FSP_FLAGS_GET_ZIP_SSIZE(flags >> DICT_TF_POS_ZIP_SSIZE
					    << FSP_FLAGS_POS_ZIP_SSIZE))
		: 0;
}

/** Determine the extent size (in pages) for the given table
@param[in]	table	the table whose extent size is being
			calculated.
@return extent size in pages (256, 128 or 64) */
inline ulint dict_table_extent_size(const dict_table_t* table)
{
	if (ulint zip_size = table->space->zip_size()) {
		return (1ULL << 20) / zip_size;
	}

	return FSP_EXTENT_SIZE;
}

/********************************************************************//**
Checks if a column is in the ordering columns of the clustered index of a
table. Column prefixes are treated like whole columns.
@return TRUE if the column, or its prefix, is in the clustered key */
ibool
dict_table_col_in_clustered_key(
/*============================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n)	/*!< in: column number */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*******************************************************************//**
Check if the table has an FTS index.
@return TRUE if table has an FTS index */
UNIV_INLINE
ibool
dict_table_has_fts_index(
/*=====================*/
	dict_table_t*   table)		/*!< in: table */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Copies types of virtual columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create().
@param[in,out]	tuple	data tuple
@param[in]	table	table
*/
void
dict_table_copy_v_types(
	dtuple_t*		tuple,
	const dict_table_t*	table);

/*******************************************************************//**
Copies types of columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create(). */
void
dict_table_copy_types(
/*==================*/
	dtuple_t*		tuple,	/*!< in/out: data tuple */
	const dict_table_t*	table)	/*!< in: table */
	MY_ATTRIBUTE((nonnull));
/**********************************************************************//**
Looks for an index with the given id. NOTE that we do not reserve
the dictionary mutex: this function is for emergency purposes like
printing info of a corrupt database page!
@return index or NULL if not found from cache */
dict_index_t*
dict_index_find_on_id_low(
/*======================*/
	index_id_t	id)	/*!< in: index id */
	MY_ATTRIBUTE((warn_unused_result));
/**********************************************************************//**
Make room in the table cache by evicting an unused table. The unused table
should not be part of FK relationship and currently not used in any user
transaction. There is no guarantee that it will remove a table.
@return number of tables evicted. */
ulint
dict_make_room_in_cache(
/*====================*/
	ulint		max_tables,	/*!< in: max tables allowed in cache */
	ulint		pct_check);	/*!< in: max percent to check */

/** Adds an index to the dictionary cache, with possible indexing newly
added column.
@param[in,out]	index	index; NOTE! The index memory
			object is freed in this function!
@param[in]	page_no	root page number of the index
@param[in]	add_v	virtual columns being added along with ADD INDEX
@return DB_SUCCESS, or DB_CORRUPTION */
dberr_t
dict_index_add_to_cache(
	dict_index_t*&		index,
	ulint			page_no,
	const dict_add_v_col_t* add_v = NULL)
	MY_ATTRIBUTE((warn_unused_result));
/********************************************************************//**
Gets the number of fields in the internal representation of an index,
including fields added by the dictionary system.
@return number of fields */
UNIV_INLINE
ulint
dict_index_get_n_fields(
/*====================*/
	const dict_index_t*	index)	/*!< in: an internal
					representation of index (in
					the dictionary cache) */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/********************************************************************//**
Gets the number of fields in the internal representation of an index
that uniquely determine the position of an index entry in the index, if
we do not take multiversioning into account: in the B-tree use the value
returned by dict_index_get_n_unique_in_tree.
@return number of fields */
UNIV_INLINE
ulint
dict_index_get_n_unique(
/*====================*/
	const dict_index_t*	index)	/*!< in: an internal representation
					of index (in the dictionary cache) */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Gets the number of fields in the internal representation of an index
which uniquely determine the position of an index entry in the index, if
we also take multiversioning into account.
@return number of fields */
UNIV_INLINE
ulint
dict_index_get_n_unique_in_tree(
/*============================*/
	const dict_index_t*	index)	/*!< in: an internal representation
					of index (in the dictionary cache) */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** The number of fields in the nonleaf page of spatial index, except
the page no field. */
#define DICT_INDEX_SPATIAL_NODEPTR_SIZE	1
/**
Gets the number of fields on nonleaf page level in the internal representation
of an index which uniquely determine the position of an index entry in the
index, if we also take multiversioning into account. Note, it doesn't
include page no field.
@param[in]	index	index
@return number of fields */
UNIV_INLINE
ulint
dict_index_get_n_unique_in_tree_nonleaf(
	const dict_index_t*	index)
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Gets the number of user-defined ordering fields in the index. In the internal
representation we add the row id to the ordering fields to make all indexes
unique, but this function returns the number of fields the user defined
in the index as ordering fields.
@return number of fields */
UNIV_INLINE
ulint
dict_index_get_n_ordering_defined_by_user(
/*======================================*/
	const dict_index_t*	index)	/*!< in: an internal representation
					of index (in the dictionary cache) */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
#ifdef UNIV_DEBUG
/********************************************************************//**
Gets the nth field of an index.
@return pointer to field object */
UNIV_INLINE
dict_field_t*
dict_index_get_nth_field(
/*=====================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			pos)	/*!< in: position of field */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
#else /* UNIV_DEBUG */
# define dict_index_get_nth_field(index, pos) ((index)->fields + (pos))
#endif /* UNIV_DEBUG */
/********************************************************************//**
Gets pointer to the nth column in an index.
@return column */
UNIV_INLINE
const dict_col_t*
dict_index_get_nth_col(
/*===================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			pos)	/*!< in: position of the field */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Gets the column number of the nth field in an index.
@return column number */
UNIV_INLINE
ulint
dict_index_get_nth_col_no(
/*======================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			pos)	/*!< in: position of the field */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Looks for column n in an index.
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
UNIV_INLINE
ulint
dict_index_get_nth_col_pos(
/*=======================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			n,	/*!< in: column number */
	ulint*			prefix_col_pos) /*!< out: col num if prefix */
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));

/** Looks for column n in an index.
@param[in]	index		index
@param[in]	n		column number
@param[in]	inc_prefix	true=consider column prefixes too
@param[in]	is_virtual	true==virtual column
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_col_or_prefix_pos(
	const dict_index_t*	index,		/*!< in: index */
	ulint			n,		/*!< in: column number */
	bool			inc_prefix,	/*!< in: TRUE=consider
						column prefixes too */
	bool			is_virtual,	/*!< in: is a virtual column
						*/
	ulint*			prefix_col_pos) /*!< out: col num if prefix
						*/
	__attribute__((warn_unused_result));
/********************************************************************//**
Looks for a matching field in an index. The column has to be the same. The
column in index must be complete, or must contain a prefix longer than the
column in index2. That is, we must be able to construct the prefix in index2
from the prefix in index.
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_field_pos(
/*=========================*/
	const dict_index_t*	index,	/*!< in: index from which to search */
	const dict_index_t*	index2,	/*!< in: index */
	ulint			n)	/*!< in: field number in index2 */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Looks for column n position in the clustered index.
@return position in internal representation of the clustered index */
ulint
dict_table_get_nth_col_pos(
/*=======================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n,	/*!< in: column number */
	ulint*			prefix_col_pos) /*!< out: col num if prefix */
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));
/*******************************************************************//**
Adds a column to index. */
void
dict_index_add_col(
/*===============*/
	dict_index_t*		index,		/*!< in/out: index */
	const dict_table_t*	table,		/*!< in: table */
	dict_col_t*		col,		/*!< in: column */
	ulint			prefix_len)	/*!< in: column prefix length */
	MY_ATTRIBUTE((nonnull));

/*******************************************************************//**
Copies types of fields contained in index to tuple. */
void
dict_index_copy_types(
/*==================*/
	dtuple_t*		tuple,		/*!< in/out: data tuple */
	const dict_index_t*	index,		/*!< in: index */
	ulint			n_fields)	/*!< in: number of
						field types to copy */
	MY_ATTRIBUTE((nonnull));
/*********************************************************************//**
Gets the field column.
@return field->col, pointer to the table column */
UNIV_INLINE
const dict_col_t*
dict_field_get_col(
/*===============*/
	const dict_field_t*	field)	/*!< in: index field */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
Assumes that dict_sys.mutex is already being held.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache_low(
/*===========================*/
	index_id_t	index_id)	/*!< in: index id */
	MY_ATTRIBUTE((warn_unused_result));
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache(
/*=======================*/
	index_id_t	index_id)	/*!< in: index id */
	MY_ATTRIBUTE((warn_unused_result));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#ifdef UNIV_DEBUG
/**********************************************************************//**
Checks that a tuple has n_fields_cmp value in a sensible range, so that
no comparison can occur with the page number field in a node pointer.
@return TRUE if ok */
ibool
dict_index_check_search_tuple(
/*==========================*/
	const dict_index_t*	index,	/*!< in: index tree */
	const dtuple_t*		tuple)	/*!< in: tuple used in a search */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Whether and when to allow temporary index names */
enum check_name {
	/** Require all indexes to be complete. */
	CHECK_ALL_COMPLETE,
	/** Allow aborted online index creation. */
	CHECK_ABORTED_OK,
	/** Allow partial indexes to exist. */
	CHECK_PARTIAL_OK
};
/**********************************************************************//**
Check for duplicate index entries in a table [using the index name] */
void
dict_table_check_for_dup_indexes(
/*=============================*/
	const dict_table_t*	table,	/*!< in: Check for dup indexes
					in this table */
	enum check_name		check)	/*!< in: whether and when to allow
					temporary index names */
	MY_ATTRIBUTE((nonnull));
#endif /* UNIV_DEBUG */
/**********************************************************************//**
Builds a node pointer out of a physical record and a page number.
@return own: node pointer */
dtuple_t*
dict_index_build_node_ptr(
/*======================*/
	const dict_index_t*	index,	/*!< in: index */
	const rec_t*		rec,	/*!< in: record for which to build node
					pointer */
	ulint			page_no,/*!< in: page number to put in node
					pointer */
	mem_heap_t*		heap,	/*!< in: memory heap where pointer
					created */
	ulint			level)	/*!< in: level of rec in tree:
					0 means leaf level */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Convert a physical record into a search tuple.
@param[in]	rec		index record (not necessarily in an index page)
@param[in]	index		index
@param[in]	leaf		whether rec is in a leaf page
@param[in]	n_fields	number of data fields
@param[in,out]	heap		memory heap for allocation
@return own: data tuple */
dtuple_t*
dict_index_build_data_tuple(
	const rec_t*		rec,
	const dict_index_t*	index,
	bool			leaf,
	ulint			n_fields,
	mem_heap_t*		heap)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*********************************************************************//**
Gets the page number of the root of the index tree.
@return page number */
UNIV_INLINE
ulint
dict_index_get_page(
/*================*/
	const dict_index_t*	tree)	/*!< in: index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Gets the read-write lock of the index tree.
@return read-write lock */
UNIV_INLINE
rw_lock_t*
dict_index_get_lock(
/*================*/
	dict_index_t*	index)	/*!< in: index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Returns free space reserved for future updates of records. This is
relevant only in the case of many consecutive inserts, as updates
which make the records bigger might fragment the index.
@return number of free bytes on page, reserved for updates */
UNIV_INLINE
ulint
dict_index_get_space_reserve(void);
/*==============================*/

/* Online index creation @{ */
/********************************************************************//**
Gets the status of online index creation.
@return the status */
UNIV_INLINE
enum online_index_status
dict_index_get_online_status(
/*=========================*/
	const dict_index_t*	index)	/*!< in: secondary index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/********************************************************************//**
Sets the status of online index creation. */
UNIV_INLINE
void
dict_index_set_online_status(
/*=========================*/
	dict_index_t*			index,	/*!< in/out: index */
	enum online_index_status	status)	/*!< in: status */
	MY_ATTRIBUTE((nonnull));
/********************************************************************//**
Determines if a secondary index is being or has been created online,
or if the table is being rebuilt online, allowing concurrent modifications
to the table.
@retval true if the index is being or has been built online, or
if this is a clustered index and the table is being or has been rebuilt online
@retval false if the index has been created or the table has been
rebuilt completely */
UNIV_INLINE
bool
dict_index_is_online_ddl(
/*=====================*/
	const dict_index_t*	index)	/*!< in: index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Calculates the minimum record length in an index. */
ulint
dict_index_calc_min_rec_len(
/*========================*/
	const dict_index_t*	index)	/*!< in: index */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#define dict_mutex_enter_for_mysql() mutex_enter(&dict_sys.mutex)
#define dict_mutex_exit_for_mysql() mutex_exit(&dict_sys.mutex)

/********************************************************************//**
Checks if the database name in two table names is the same.
@return TRUE if same db name */
ibool
dict_tables_have_same_db(
/*=====================*/
	const char*	name1,	/*!< in: table name in the form
				dbname '/' tablename */
	const char*	name2)	/*!< in: table name in the form
				dbname '/' tablename */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Get an index by name.
@param[in]	table		the table where to look for the index
@param[in]	name		the index name to look for
@return index, NULL if does not exist */
dict_index_t*
dict_table_get_index_on_name(dict_table_t* table, const char* name)
		MY_ATTRIBUTE((warn_unused_result));

/** Get an index by name.
@param[in]	table		the table where to look for the index
@param[in]	name		the index name to look for
@return index, NULL if does not exist */
inline
const dict_index_t*
dict_table_get_index_on_name(const dict_table_t* table, const char* name)
{
	return dict_table_get_index_on_name(const_cast<dict_table_t*>(table),
					    name);
}

/***************************************************************
Check whether a column exists in an FTS index. */
UNIV_INLINE
ulint
dict_table_is_fts_column(
/*=====================*/
				/* out: ULINT_UNDEFINED if no match else
				the offset within the vector */
	ib_vector_t*	indexes,/* in: vector containing only FTS indexes */
	ulint		col_no,	/* in: col number to search for */
	bool		is_virtual)/*!< in: whether it is a virtual column */
	MY_ATTRIBUTE((warn_unused_result));

/** Looks for an index with the given id given a table instance.
@param[in]	table	table instance
@param[in]	id	index id
@return index or NULL */
dict_index_t*
dict_table_find_index_on_id(
	const dict_table_t*	table,
	index_id_t		id)
	MY_ATTRIBUTE((nonnull(1)));

/** Maximum number of columns in a foreign key constraint. Please Note MySQL
has a much lower limit on the number of columns allowed in a foreign key
constraint */
#define MAX_NUM_FK_COLUMNS		500

/* Buffers for storing detailed information about the latest foreign key
and unique key errors */
extern FILE*		dict_foreign_err_file;
extern ib_mutex_t	dict_foreign_err_mutex; /* mutex protecting the
						foreign key error messages */

/** InnoDB data dictionary cache */
class dict_sys_t
{
public:
	DictSysMutex	mutex;		/*!< mutex protecting the data
					dictionary; protects also the
					disk-based dictionary system tables;
					this mutex serializes CREATE TABLE
					and DROP TABLE, as well as reading
					the dictionary data for a table from
					system tables */
	/** @brief the data dictionary rw-latch protecting dict_sys

	Table create, drop, etc. reserve this in X-mode; implicit or
	backround operations purge, rollback, foreign key checks reserve this
	in S-mode; not all internal InnoDB operations are covered by MDL.

	This latch also prevents lock waits when accessing the InnoDB
	data dictionary tables. @see trx_t::dict_operation_lock_mode */
	rw_lock_t	latch;
	row_id_t	row_id;		/*!< the next row id to assign;
					NOTE that at a checkpoint this
					must be written to the dict system
					header and flushed to a file; in
					recovery this must be derived from
					the log records */
	hash_table_t*	table_hash;	/*!< hash table of the tables, based
					on name */
	/** hash table of persistent table IDs */
	hash_table_t*	table_id_hash;
	dict_table_t*	sys_tables;	/*!< SYS_TABLES table */
	dict_table_t*	sys_columns;	/*!< SYS_COLUMNS table */
	dict_table_t*	sys_indexes;	/*!< SYS_INDEXES table */
	dict_table_t*	sys_fields;	/*!< SYS_FIELDS table */
	dict_table_t*	sys_virtual;	/*!< SYS_VIRTUAL table */

	/*=============================*/
	UT_LIST_BASE_NODE_T(dict_table_t)
			table_LRU;	/*!< List of tables that can be evicted
					from the cache */
	UT_LIST_BASE_NODE_T(dict_table_t)
			table_non_LRU;	/*!< List of tables that can't be
					evicted from the cache */
private:
	bool m_initialised;
	/** the sequence of temporary table IDs */
	std::atomic<table_id_t> temp_table_id;
	/** hash table of temporary table IDs */
	hash_table_t*	temp_id_hash;
public:
	/** @return a new temporary table ID */
	table_id_t get_temporary_table_id() {
		return temp_table_id.fetch_add(1, std::memory_order_relaxed);
	}

	/** Look up a temporary table.
	@param id	temporary table ID
	@return	temporary table
	@retval	NULL	if the table does not exist
	(should only happen during the rollback of CREATE...SELECT) */
	dict_table_t* get_temporary_table(table_id_t id)
	{
		ut_ad(mutex_own(&mutex));
		dict_table_t* table;
		ulint fold = ut_fold_ull(id);
		HASH_SEARCH(id_hash, temp_id_hash, fold, dict_table_t*, table,
			    ut_ad(table->cached), table->id == id);
		if (UNIV_LIKELY(table != NULL)) {
			DBUG_ASSERT(table->is_temporary());
			DBUG_ASSERT(table->id >= DICT_HDR_FIRST_ID);
			table->acquire();
		}
		return table;
	}

	/** Look up a persistent table.
	@param id	table ID
	@return	table
	@retval	NULL	if not cached */
	dict_table_t* get_table(table_id_t id)
	{
		ut_ad(mutex_own(&mutex));
		dict_table_t* table;
		ulint fold = ut_fold_ull(id);
		HASH_SEARCH(id_hash, table_id_hash, fold, dict_table_t*, table,
			    ut_ad(table->cached), table->id == id);
		DBUG_ASSERT(!table || !table->is_temporary());
		return table;
	}

  /**
    Constructor.  Further initialisation happens in create().
  */

  dict_sys_t() : m_initialised(false), temp_table_id(DICT_HDR_FIRST_ID) {}

  bool is_initialised() const { return m_initialised; }

  /** Initialise the data dictionary cache. */
  void create();

  /** Close the data dictionary cache on shutdown. */
  void close();

  /** Resize the hash tables based on the current buffer pool size. */
  void resize();

  /** Add a table definition to the data dictionary cache */
  inline void add(dict_table_t* table);
  /** Remove a table definition from the data dictionary cache.
  @param[in,out]	table	cached table definition to be evicted
  @param[in]	lru	whether this is part of least-recently-used evictiono
  @param[in]	keep	whether to keep (not free) the object */
  void remove(dict_table_t* table, bool lru = false, bool keep = false);

#ifdef UNIV_DEBUG
  /** Find a table */
  template <bool in_lru> bool find(dict_table_t* table)
  {
    ut_ad(table);
    ut_ad(table->can_be_evicted == in_lru);
    ut_ad(mutex_own(&mutex));
    for (const dict_table_t* t = UT_LIST_GET_FIRST(in_lru
					     ? table_LRU : table_non_LRU);
	 t; t = UT_LIST_GET_NEXT(table_LRU, t))
    {
      if (t == table) return true;
      ut_ad(t->can_be_evicted == in_lru);
    }
    return false;
  }
  /** Find a table */
  bool find(dict_table_t* table)
  {
    return table->can_be_evicted ? find<true>(table) : find<false>(table);
  }
#endif

  /** Move a table to the non-LRU list from the LRU list. */
  void prevent_eviction(dict_table_t* table)
  {
    ut_ad(find(table));
    if (table->can_be_evicted)
    {
      table->can_be_evicted = FALSE;
      UT_LIST_REMOVE(table_LRU, table);
      UT_LIST_ADD_LAST(table_non_LRU, table);
    }
  }
  /** Acquire a reference to a cached table. */
  inline void acquire(dict_table_t* table);

#ifdef UNIV_DEBUG
  /** Assert that the data dictionary is locked */
  void assert_locked()
  {
    ut_ad(mutex_own(&mutex));
    ut_ad(rw_lock_own(&latch, RW_LOCK_X));
  }
#endif
  /** Lock the data dictionary cache. */
  void lock(const char* file, unsigned line)
  {
    rw_lock_x_lock_func(&latch, 0, file, line);
    mutex_enter_loc(&mutex, file, line);
  }

  /** Unlock the data dictionary cache. */
  void unlock()
  {
    mutex_exit(&mutex);
    rw_lock_x_unlock(&latch);
  }
};

/** the data dictionary cache */
extern dict_sys_t	dict_sys;

#define dict_table_prevent_eviction(table) dict_sys.prevent_eviction(table)
#define dict_sys_lock() dict_sys.lock(__FILE__, __LINE__)
#define dict_sys_unlock() dict_sys.unlock()

/** dummy index for ROW_FORMAT=REDUNDANT supremum and infimum records */
extern dict_index_t*	dict_ind_redundant;

/** Initialize dict_ind_redundant. */
void
dict_ind_init();

/** Free dict_ind_redundant. */
void
dict_ind_free();

/* Auxiliary structs for checking a table definition @{ */

/* This struct is used to specify the name and type that a column must
have when checking a table's schema. */
struct dict_col_meta_t {
	const char*	name;		/* column name */
	ulint		mtype;		/* required column main type */
	ulint		prtype_mask;	/* required column precise type mask;
					if this is non-zero then all the
					bits it has set must also be set
					in the column's prtype */
	ulint		len;		/* required column length */
};

/* This struct is used for checking whether a given table exists and
whether it has a predefined schema (number of columns and column names
and types) */
struct dict_table_schema_t {
	const char*		table_name;	/* the name of the table whose
						structure we are checking */
	ulint			n_cols;		/* the number of columns the
						table must have */
	dict_col_meta_t*	columns;	/* metadata for the columns;
						this array has n_cols
						elements */
	ulint			n_foreign;	/* number of foreign keys this
						table has, pointing to other
						tables (where this table is
						FK child) */
	ulint			n_referenced;	/* number of foreign keys other
						tables have, pointing to this
						table (where this table is
						parent) */
};
/* @} */

/*********************************************************************//**
Checks whether a table exists and whether it has the given structure.
The table must have the same number of columns with the same names and
types. The order of the columns does not matter.
The caller must own the dictionary mutex.
dict_table_schema_check() @{
@return DB_SUCCESS if the table exists and contains the necessary columns */
dberr_t
dict_table_schema_check(
/*====================*/
	dict_table_schema_t*	req_schema,	/*!< in/out: required table
						schema */
	char*			errstr,		/*!< out: human readable error
						message if != DB_SUCCESS and
						!= DB_TABLE_NOT_FOUND is
						returned */
	size_t			errstr_sz)	/*!< in: errstr size */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/* @} */

/*********************************************************************//**
Converts a database and table name from filesystem encoding
(e.g. d@i1b/a@q1b@1Kc, same format as used in dict_table_t::name) in two
strings in UTF8 encoding (e.g. dцb and aюbØc). The output buffers must be
at least MAX_DB_UTF8_LEN and MAX_TABLE_UTF8_LEN bytes. */
void
dict_fs2utf8(
/*=========*/
	const char*	db_and_table,	/*!< in: database and table names,
					e.g. d@i1b/a@q1b@1Kc */
	char*		db_utf8,	/*!< out: database name, e.g. dцb */
	size_t		db_utf8_size,	/*!< in: dbname_utf8 size */
	char*		table_utf8,	/*!< out: table name, e.g. aюbØc */
	size_t		table_utf8_size)/*!< in: table_utf8 size */
	MY_ATTRIBUTE((nonnull));

/**********************************************************************//**
Check whether the table is corrupted.
@return nonzero for corrupted table, zero for valid tables */
UNIV_INLINE
ulint
dict_table_is_corrupted(
/*====================*/
	const dict_table_t*	table)	/*!< in: table */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/**********************************************************************//**
Flags an index and table corrupted both in the data dictionary cache
and in the system table SYS_INDEXES. */
void
dict_set_corrupted(
/*===============*/
	dict_index_t*	index,	/*!< in/out: index */
	trx_t*		trx,	/*!< in/out: transaction */
	const char*	ctx)	/*!< in: context */
	ATTRIBUTE_COLD __attribute__((nonnull));

/** Flags an index corrupted in the data dictionary cache only. This
is used mostly to mark a corrupted index when index's own dictionary
is corrupted, and we force to load such index for repair purpose
@param[in,out]	index	index that is corrupted */
void
dict_set_corrupted_index_cache_only(
	dict_index_t*	index);

/**********************************************************************//**
Flags a table with specified space_id corrupted in the table dictionary
cache.
@return TRUE if successful */
bool dict_set_corrupted_by_space(const fil_space_t* space);

/** Flag a table encrypted in the data dictionary cache. */
void dict_set_encrypted_by_space(const fil_space_t* space);

/** Sets merge_threshold in the SYS_INDEXES
@param[in,out]	index		index
@param[in]	merge_threshold	value to set */
void
dict_index_set_merge_threshold(
	dict_index_t*	index,
	ulint		merge_threshold);

#ifdef UNIV_DEBUG
/** Sets merge_threshold for all indexes in dictionary cache for debug.
@param[in]	merge_threshold_all	value to set for all indexes */
void
dict_set_merge_threshold_all_debug(
	uint	merge_threshold_all);
#endif /* UNIV_DEBUG */

/** Validate the table flags.
@param[in]	flags	Table flags
@return true if valid. */
UNIV_INLINE
bool
dict_tf_is_valid(
	ulint	flags);

/** Validate both table flags and table flags2 and make sure they
are compatible.
@param[in]	flags	Table flags
@param[in]	flags2	Table flags2
@return true if valid. */
UNIV_INLINE
bool
dict_tf2_is_valid(
	ulint	flags,
	ulint	flags2);

/*********************************************************************//**
This function should be called whenever a page is successfully
compressed. Updates the compression padding information. */
void
dict_index_zip_success(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
	MY_ATTRIBUTE((nonnull));
/*********************************************************************//**
This function should be called whenever a page compression attempt
fails. Updates the compression padding information. */
void
dict_index_zip_failure(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
	MY_ATTRIBUTE((nonnull));
/*********************************************************************//**
Return the optimal page size, for which page will likely compress.
@return page size beyond which page may not compress*/
ulint
dict_index_zip_pad_optimal_page_size(
/*=================================*/
	dict_index_t*	index)	/*!< in: index for which page size
				is requested */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*************************************************************//**
Convert table flag to row format string.
@return row format name */
const char*
dict_tf_to_row_format_string(
/*=========================*/
	ulint	table_flag);		/*!< in: row format setting */

/** encode number of columns and number of virtual columns in one
4 bytes value. We could do this because the number of columns in
InnoDB is limited to 1017
@param[in]	n_col	number of non-virtual column
@param[in]	n_v_col	number of virtual column
@return encoded value */
UNIV_INLINE
ulint
dict_table_encode_n_col(
	ulint	n_col,
	ulint	n_v_col);

/** Decode number of virtual and non-virtual columns in one 4 bytes value.
@param[in]	encoded	encoded value
@param[in,out]	n_col	number of non-virtual column
@param[in,out]	n_v_col	number of virtual column */
UNIV_INLINE
void
dict_table_decode_n_col(
	ulint	encoded,
	ulint*	n_col,
	ulint*	n_v_col);

/** Calculate the used memory occupied by the data dictionary
table and index objects.
@return number of bytes occupied. */
UNIV_INTERN
ulint
dict_sys_get_size();

/** Look for any dictionary objects that are found in the given tablespace.
@param[in]	space_id	Tablespace ID to search for.
@return true if tablespace is empty. */
bool
dict_space_is_empty(
	ulint	space_id);

/** Find the space_id for the given name in sys_tablespaces.
@param[in]	name	Tablespace name to search for.
@return the tablespace ID. */
ulint
dict_space_get_id(
	const char*	name);

/** Free the virtual column template
@param[in,out]	vc_templ	virtual column template */
UNIV_INLINE
void
dict_free_vc_templ(
	dict_vcol_templ_t*	vc_templ);

/** Check whether the table have virtual index.
@param[in]	table	InnoDB table
@return true if the table have virtual index, false otherwise. */
UNIV_INLINE
bool
dict_table_have_virtual_index(
	dict_table_t*	table);

#include "dict0dict.ic"

#endif
