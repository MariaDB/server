/*****************************************************************************

Copyright (c) 2000, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
@file include/row0mysql.h
Interface between Innobase row operations and MySQL.
Contains also create table and other data dictionary operations.

Created 9/17/2000 Heikki Tuuri
*******************************************************/

#ifndef row0mysql_h
#define row0mysql_h

#include "que0types.h"
#include "trx0types.h"
#include "row0types.h"
#include "btr0types.h"
#include "lock0types.h"
#include "fil0fil.h"
#include "fts0fts.h"
#include "gis0type.h"

struct row_prebuilt_t;
class ha_innobase;

/*******************************************************************//**
Frees the blob heap in prebuilt when no longer needed. */
void
row_mysql_prebuilt_free_blob_heap(
/*==============================*/
	row_prebuilt_t*	prebuilt);	/*!< in: prebuilt struct of a
					ha_innobase:: table handle */
/*******************************************************************//**
Stores a >= 5.0.3 format true VARCHAR length to dest, in the MySQL row
format.
@return pointer to the data, we skip the 1 or 2 bytes at the start
that are used to store the len */
byte*
row_mysql_store_true_var_len(
/*=========================*/
	byte*	dest,	/*!< in: where to store */
	ulint	len,	/*!< in: length, must fit in two bytes */
	ulint	lenlen);/*!< in: storage length of len: either 1 or 2 bytes */
/*******************************************************************//**
Reads a >= 5.0.3 format true VARCHAR length, in the MySQL row format, and
returns a pointer to the data.
@return pointer to the data, we skip the 1 or 2 bytes at the start
that are used to store the len */
const byte*
row_mysql_read_true_varchar(
/*========================*/
	ulint*		len,	/*!< out: variable-length field length */
	const byte*	field,	/*!< in: field in the MySQL format */
	ulint		lenlen);/*!< in: storage length of len: either 1
				or 2 bytes */
/*******************************************************************//**
Stores a reference to a BLOB in the MySQL format. */
void
row_mysql_store_blob_ref(
/*=====================*/
	byte*		dest,	/*!< in: where to store */
	ulint		col_len,/*!< in: dest buffer size: determines into
				how many bytes the BLOB length is stored,
				the space for the length may vary from 1
				to 4 bytes */
	const void*	data,	/*!< in: BLOB data; if the value to store
				is SQL NULL this should be NULL pointer */
	ulint		len);	/*!< in: BLOB length; if the value to store
				is SQL NULL this should be 0; remember
				also to set the NULL bit in the MySQL record
				header! */
/*******************************************************************//**
Reads a reference to a BLOB in the MySQL format.
@return pointer to BLOB data */
const byte*
row_mysql_read_blob_ref(
/*====================*/
	ulint*		len,		/*!< out: BLOB length */
	const byte*	ref,		/*!< in: BLOB reference in the
					MySQL format */
	ulint		col_len);	/*!< in: BLOB reference length
					(not BLOB length) */
/*******************************************************************//**
Converts InnoDB geometry data format to MySQL data format. */
void
row_mysql_store_geometry(
/*=====================*/
	byte*		dest,		/*!< in/out: where to store */
	ulint		dest_len,	/*!< in: dest buffer size: determines into
					how many bytes the geometry length is stored,
					the space for the length may vary from 1
					to 4 bytes */
	const byte*	src,		/*!< in: geometry data; if the value to store
					is SQL NULL this should be NULL pointer */
	ulint		src_len);	/*!< in: geometry length; if the value to store
					is SQL NULL this should be 0; remember
					also to set the NULL bit in the MySQL record
					header! */
/**************************************************************//**
Pad a column with spaces. */
void
row_mysql_pad_col(
/*==============*/
	ulint	mbminlen,	/*!< in: minimum size of a character,
				in bytes */
	byte*	pad,		/*!< out: padded buffer */
	ulint	len);		/*!< in: number of bytes to pad */

/**************************************************************//**
Stores a non-SQL-NULL field given in the MySQL format in the InnoDB format.
The counterpart of this function is row_sel_field_store_in_mysql_format() in
row0sel.cc.
@return up to which byte we used buf in the conversion */
byte*
row_mysql_store_col_in_innobase_format(
/*===================================*/
	dfield_t*	dfield,		/*!< in/out: dfield where dtype
					information must be already set when
					this function is called! */
	byte*		buf,		/*!< in/out: buffer for a converted
					integer value; this must be at least
					col_len long then! NOTE that dfield
					may also get a pointer to 'buf',
					therefore do not discard this as long
					as dfield is used! */
	ibool		row_format_col,	/*!< TRUE if the mysql_data is from
					a MySQL row, FALSE if from a MySQL
					key value;
					in MySQL, a true VARCHAR storage
					format differs in a row and in a
					key value: in a key value the length
					is always stored in 2 bytes! */
	const byte*	mysql_data,	/*!< in: MySQL column value, not
					SQL NULL; NOTE that dfield may also
					get a pointer to mysql_data,
					therefore do not discard this as long
					as dfield is used! */
	ulint		col_len,	/*!< in: MySQL column length; NOTE that
					this is the storage length of the
					column in the MySQL format row, not
					necessarily the length of the actual
					payload data; if the column is a true
					VARCHAR then this is irrelevant */
	ulint		comp);		/*!< in: nonzero=compact format */
/****************************************************************//**
Handles user errors and lock waits detected by the database engine.
@return true if it was a lock wait and we should continue running the
query thread */
bool
row_mysql_handle_errors(
/*====================*/
	dberr_t*	new_err,/*!< out: possible new error encountered in
				rollback, or the old error which was
				during the function entry */
	trx_t*		trx,	/*!< in: transaction */
	que_thr_t*	thr,	/*!< in: query thread, or NULL */
	trx_savept_t*	savept)	/*!< in: savepoint, or NULL */
	MY_ATTRIBUTE((nonnull(1,2)));
/********************************************************************//**
Create a prebuilt struct for a MySQL table handle.
@return own: a prebuilt struct */
row_prebuilt_t*
row_create_prebuilt(
/*================*/
	dict_table_t*	table,		/*!< in: Innobase table handle */
	ulint		mysql_row_len);	/*!< in: length in bytes of a row in
					the MySQL format */
/** Free a prebuilt struct for a TABLE handle. */
void row_prebuilt_free(row_prebuilt_t *prebuilt);
/*********************************************************************//**
Updates the transaction pointers in query graphs stored in the prebuilt
struct. */
void
row_update_prebuilt_trx(
/*====================*/
	row_prebuilt_t*	prebuilt,	/*!< in/out: prebuilt struct
					in MySQL handle */
	trx_t*		trx);		/*!< in: transaction handle */

/*********************************************************************//**
Sets an AUTO_INC type lock on the table mentioned in prebuilt. The
AUTO_INC lock gives exclusive access to the auto-inc counter of the
table. The lock is reserved only for the duration of an SQL statement.
It is not compatible with another AUTO_INC or exclusive lock on the
table.
@return error code or DB_SUCCESS */
dberr_t
row_lock_table_autoinc_for_mysql(
/*=============================*/
	row_prebuilt_t*	prebuilt)	/*!< in: prebuilt struct in the MySQL
					table handle */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Lock a table.
@param[in,out]	prebuilt	table handle
@return error code or DB_SUCCESS */
dberr_t
row_lock_table(row_prebuilt_t* prebuilt);

/** System Versioning: row_insert_for_mysql() modes */
enum ins_mode_t {
	/* plain row (without versioning) */
	ROW_INS_NORMAL = 0,
	/* row_start = TRX_ID, row_end = MAX */
	ROW_INS_VERSIONED,
	/* row_end = TRX_ID */
	ROW_INS_HISTORICAL
};

/** Does an insert for MySQL.
@param[in]	mysql_rec	row in the MySQL format
@param[in,out]	prebuilt	prebuilt struct in MySQL handle
@param[in]	ins_mode	what row type we're inserting
@return error code or DB_SUCCESS*/
dberr_t
row_insert_for_mysql(
	const byte*		mysql_rec,
	row_prebuilt_t*		prebuilt,
	ins_mode_t		ins_mode)
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Builds a dummy query graph used in selects. */
void
row_prebuild_sel_graph(
/*===================*/
	row_prebuilt_t*	prebuilt);	/*!< in: prebuilt struct in MySQL
					handle */
/*********************************************************************//**
Gets pointer to a prebuilt update vector used in updates. If the update
graph has not yet been built in the prebuilt struct, then this function
first builds it.
@return prebuilt update vector */
upd_t*
row_get_prebuilt_update_vector(
/*===========================*/
	row_prebuilt_t*	prebuilt);	/*!< in: prebuilt struct in MySQL
					handle */
/** Does an update or delete of a row for MySQL.
@param[in,out]	prebuilt	prebuilt struct in MySQL handle
@return error code or DB_SUCCESS */
dberr_t
row_update_for_mysql(
	row_prebuilt_t*		prebuilt)
	MY_ATTRIBUTE((warn_unused_result));

/** This can only be used when the current transaction is at
READ COMMITTED or READ UNCOMMITTED isolation level.
Before calling this function row_search_for_mysql() must have
initialized prebuilt->new_rec_locks to store the information which new
record locks really were set. This function removes a newly set
clustered index record lock under prebuilt->pcur or
prebuilt->clust_pcur.  Thus, this implements a 'mini-rollback' that
releases the latest clustered index record lock we set.
@param[in,out]	prebuilt		prebuilt struct in MySQL handle
@param[in]	has_latches_on_recs	TRUE if called so that we have the
					latches on the records under pcur
					and clust_pcur, and we do not need
					to reposition the cursors. */
void
row_unlock_for_mysql(
	row_prebuilt_t*	prebuilt,
	ibool		has_latches_on_recs);

/*********************************************************************//**
Creates an query graph node of 'update' type to be used in the MySQL
interface.
@return own: update node */
upd_node_t*
row_create_update_node_for_mysql(
/*=============================*/
	dict_table_t*	table,	/*!< in: table to update */
	mem_heap_t*	heap);	/*!< in: mem heap from which allocated */

/**********************************************************************//**
Does a cascaded delete or set null in a foreign key operation.
@return error code or DB_SUCCESS */
dberr_t
row_update_cascade_for_mysql(
/*=========================*/
        que_thr_t*      thr,    /*!< in: query thread */
        upd_node_t*     node,   /*!< in: update node used in the cascade
                                or set null operation */
        dict_table_t*   table)  /*!< in: table where we do the operation */
        MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Lock the data dictionary cache exclusively. */
#define row_mysql_lock_data_dictionary(trx)			\
	do {							\
		ut_ad(!trx->dict_operation_lock_mode);		\
		dict_sys.lock(SRW_LOCK_CALL);			\
		trx->dict_operation_lock_mode = true;		\
	} while (0)

/** Unlock the data dictionary. */
#define row_mysql_unlock_data_dictionary(trx)			\
	do {							\
		ut_ad(!lock_trx_has_sys_table_locks(trx));	\
		ut_ad(trx->dict_operation_lock_mode);		\
		trx->dict_operation_lock_mode = false;		\
		dict_sys.unlock();				\
	} while (0)

/*********************************************************************//**
Creates a table for MySQL. On failure the transaction will be rolled back
and the 'table' object will be freed.
@return error code or DB_SUCCESS */
dberr_t
row_create_table_for_mysql(
/*=======================*/
	dict_table_t*	table,	/*!< in, own: table definition
				(will be freed, or on DB_SUCCESS
				added to the data dictionary cache) */
	trx_t*		trx)	/*!< in/out: transaction */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Create an index when creating a table.
On failure, the caller must drop the table!
@return error number or DB_SUCCESS */
dberr_t
row_create_index_for_mysql(
/*=======================*/
	dict_index_t*	index,		/*!< in, own: index definition
					(will be freed) */
	trx_t*		trx,		/*!< in: transaction handle */
	const ulint*	field_lengths,	/*!< in: if not NULL, must contain
					dict_index_get_n_fields(index)
					actual field lengths for the
					index columns, which are
					then checked for not being too
					large. */
	fil_encryption_t mode,	/*!< in: encryption mode */
	uint32_t	key_id)	/*!< in: encryption key_id */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Discards the tablespace of a table which stored in an .ibd file. Discarding
means that this function deletes the .ibd file and assigns a new table id for
the table. Also the file_unreadable flag is set.
@return error code or DB_SUCCESS */
dberr_t row_discard_tablespace_for_mysql(dict_table_t *table, trx_t *trx)
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*****************************************************************//**
Imports a tablespace. The space id in the .ibd file must match the space id
of the table in the data dictionary.
@return error code or DB_SUCCESS */
dberr_t
row_import_tablespace_for_mysql(
/*============================*/
	dict_table_t*	table,		/*!< in/out: table */
	row_prebuilt_t*	prebuilt)	/*!< in: prebuilt struct in MySQL */
        MY_ATTRIBUTE((nonnull, warn_unused_result));

/*********************************************************************//**
Renames a table for MySQL.
@return error code or DB_SUCCESS */
dberr_t
row_rename_table_for_mysql(
/*=======================*/
	const char*	old_name,	/*!< in: old table name */
	const char*	new_name,	/*!< in: new table name */
	trx_t*		trx,		/*!< in/out: transaction */
	bool		use_fk)		/*!< in: whether to parse and enforce
					FOREIGN KEY constraints */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*********************************************************************//**
Scans an index for either COOUNT(*) or CHECK TABLE.
If CHECK TABLE; Checks that the index contains entries in an ascending order,
unique constraint is not broken, and calculates the number of index entries
in the read view of the current transaction.
@return DB_SUCCESS or other error */
dberr_t
row_scan_index_for_mysql(
/*=====================*/
	row_prebuilt_t*		prebuilt,	/*!< in: prebuilt struct
						in MySQL handle */
	const dict_index_t*	index,		/*!< in: index */
	ulint*			n_rows)		/*!< out: number of entries
						seen in the consistent read */
	MY_ATTRIBUTE((warn_unused_result));

/* A struct describing a place for an individual column in the MySQL
row format which is presented to the table handler in ha_innobase.
This template struct is used to speed up row transformations between
Innobase and MySQL. */

struct mysql_row_templ_t {
	ulint	col_no;			/*!< column number of the column */
	ulint	rec_field_no;		/*!< field number of the column in an
					Innobase record in the current index;
					not defined if template_type is
					ROW_MYSQL_WHOLE_ROW */
	ibool	rec_field_is_prefix;	/* is this field in a prefix index? */
	ulint	rec_prefix_field_no;	/* record field, even if just a
					prefix; same as rec_field_no when not a
					prefix, otherwise rec_field_no is
					ULINT_UNDEFINED but this is the true
					field number*/
	ulint	clust_rec_field_no;	/*!< field number of the column in an
					Innobase record in the clustered index;
					not defined if template_type is
					ROW_MYSQL_WHOLE_ROW */
	ulint	icp_rec_field_no;	/*!< field number of the column in an
					Innobase record in the current index;
					not defined unless
					index condition pushdown is used */
	ulint	mysql_col_offset;	/*!< offset of the column in the MySQL
					row format */
	ulint	mysql_col_len;		/*!< length of the column in the MySQL
					row format */
	ulint	mysql_null_byte_offset;	/*!< MySQL NULL bit byte offset in a
					MySQL record */
	ulint	mysql_null_bit_mask;	/*!< bit mask to get the NULL bit,
					zero if column cannot be NULL */
	ulint	type;			/*!< column type in Innobase mtype
					numbers DATA_CHAR... */
	ulint	mysql_type;		/*!< MySQL type code; this is always
					< 256 */
	ulint	mysql_length_bytes;	/*!< if mysql_type
					== DATA_MYSQL_TRUE_VARCHAR, this tells
					whether we should use 1 or 2 bytes to
					store the MySQL true VARCHAR data
					length at the start of row in the MySQL
					format (NOTE that the MySQL key value
					format always uses 2 bytes for the data
					len) */
	ulint	charset;		/*!< MySQL charset-collation code
					of the column, or zero */
	ulint	mbminlen;		/*!< minimum length of a char, in bytes,
					or zero if not a char type */
	ulint	mbmaxlen;		/*!< maximum length of a char, in bytes,
					or zero if not a char type */
	ulint	is_unsigned;		/*!< if a column type is an integer
					type and this field is != 0, then
					it is an unsigned integer type */
	ulint	is_virtual;		/*!< if a column is a virtual column */
};

#define MYSQL_FETCH_CACHE_SIZE		8
/* After fetching this many rows, we start caching them in fetch_cache */
#define MYSQL_FETCH_CACHE_THRESHOLD	4

#define ROW_PREBUILT_ALLOCATED	78540783
#define ROW_PREBUILT_FREED	26423527

/** A struct for (sometimes lazily) prebuilt structures in an Innobase table
handle used within MySQL; these are used to save CPU time. */

struct row_prebuilt_t {
	ulint		magic_n;	/*!< this magic number is set to
					ROW_PREBUILT_ALLOCATED when created,
					or ROW_PREBUILT_FREED when the
					struct has been freed */
	dict_table_t*	table;		/*!< Innobase table handle */
	dict_index_t*	index;		/*!< current index for a search, if
					any */
	trx_t*		trx;		/*!< current transaction handle */
	unsigned	sql_stat_start:1;/*!< TRUE when we start processing of
					an SQL statement: we may have to set
					an intention lock on the table,
					create a consistent read view etc. */
	unsigned	clust_index_was_generated:1;
					/*!< if the user did not define a
					primary key in MySQL, then Innobase
					automatically generated a clustered
					index where the ordering column is
					the row id: in this case this flag
					is set to TRUE */
	unsigned	index_usable:1;	/*!< caches the value of
					row_merge_is_index_usable(trx,index) */
	unsigned	read_just_key:1;/*!< set to 1 when MySQL calls
					ha_innobase::extra with the
					argument HA_EXTRA_KEYREAD; it is enough
					to read just columns defined in
					the index (i.e., no read of the
					clustered index record necessary) */
	unsigned	used_in_HANDLER:1;/*!< TRUE if we have been using this
					handle in a MySQL HANDLER low level
					index cursor command: then we must
					store the pcur position even in a
					unique search from a clustered index,
					because HANDLER allows NEXT and PREV
					in such a situation */
	unsigned	template_type:2;/*!< ROW_MYSQL_WHOLE_ROW,
					ROW_MYSQL_REC_FIELDS,
					ROW_MYSQL_DUMMY_TEMPLATE, or
					ROW_MYSQL_NO_TEMPLATE */
	unsigned	n_template:10;	/*!< number of elements in the
					template */
	unsigned	null_bitmap_len:10;/*!< number of bytes in the SQL NULL
					bitmap at the start of a row in the
					MySQL format */
	unsigned	need_to_access_clustered:1; /*!< if we are fetching
					columns through a secondary index
					and at least one column is not in
					the secondary index, then this is
					set to TRUE; note that sometimes this
					is set but we later optimize out the
					clustered index lookup */
	unsigned	templ_contains_blob:1;/*!< TRUE if the template contains
					a column with DATA_LARGE_MTYPE(
					get_innobase_type_from_mysql_type())
					is TRUE;
					not to be confused with InnoDB
					externally stored columns
					(VARCHAR can be off-page too) */
	unsigned	versioned_write:1;/*!< whether this is
					a versioned write */
	mysql_row_templ_t* mysql_template;/*!< template used to transform
					rows fast between MySQL and Innobase
					formats; memory for this template
					is not allocated from 'heap' */
	mem_heap_t*	heap;		/*!< memory heap from which
					these auxiliary structures are
					allocated when needed */
	ins_node_t*	ins_node;	/*!< Innobase SQL insert node
					used to perform inserts
					to the table */
	byte*		ins_upd_rec_buff;/*!< buffer for storing data converted
					to the Innobase format from the MySQL
					format */
	const byte*	default_rec;	/*!< the default values of all columns
					(a "default row") in MySQL format */
	ulint		hint_need_to_fetch_extra_cols;
					/*!< normally this is set to 0; if this
					is set to ROW_RETRIEVE_PRIMARY_KEY,
					then we should at least retrieve all
					columns in the primary key; if this
					is set to ROW_RETRIEVE_ALL_COLS, then
					we must retrieve all columns in the
					key (if read_just_key == 1), or all
					columns in the table */
	upd_node_t*	upd_node;	/*!< Innobase SQL update node used
					to perform updates and deletes */
	trx_id_t	trx_id;		/*!< The table->def_trx_id when
					ins_graph was built */
	que_fork_t*	ins_graph;	/*!< Innobase SQL query graph used
					in inserts. Will be rebuilt on
					trx_id or n_indexes mismatch. */
	que_fork_t*	upd_graph;	/*!< Innobase SQL query graph used
					in updates or deletes */
	btr_pcur_t*	pcur;		/*!< persistent cursor used in selects
					and updates */
	btr_pcur_t*	clust_pcur;	/*!< persistent cursor used in
					some selects and updates */
	que_fork_t*	sel_graph;	/*!< dummy query graph used in
					selects */
	dtuple_t*	search_tuple;	/*!< prebuilt dtuple used in selects */
	byte		row_id[DATA_ROW_ID_LEN];
					/*!< if the clustered index was
					generated, the row id of the
					last row fetched is stored
					here */
	doc_id_t	fts_doc_id;	/* if the table has an FTS index on
					it then we fetch the doc_id.
					FTS-FIXME: Currently we fetch it always
					but in the future we must only fetch
					it when FTS columns are being
					updated */
	dtuple_t*	clust_ref;	/*!< prebuilt dtuple used in
					sel/upd/del */
	lock_mode	select_lock_type;/*!< LOCK_NONE, LOCK_S, or LOCK_X */
	bool		skip_locked;	/*!< TL_{READ,WRITE}_SKIP_LOCKED */
	lock_mode	stored_select_lock_type;/*!< this field is used to
					remember the original select_lock_type
					that was decided in ha_innodb.cc,
					::store_lock(), ::external_lock(),
					etc. */
	ulint		row_read_type;	/*!< ROW_READ_WITH_LOCKS if row locks
					should be the obtained for records
					under an UPDATE or DELETE cursor.
					At READ UNCOMMITTED or
					READ COMMITTED isolation level,
					this can be set to
					ROW_READ_TRY_SEMI_CONSISTENT, so that
					if the row under an UPDATE or DELETE
					cursor was locked by another
					transaction, InnoDB will resort
					to reading the last committed value
					('semi-consistent read').  Then,
					this field will be set to
					ROW_READ_DID_SEMI_CONSISTENT to
					indicate that.	If the row does not
					match the WHERE condition, MySQL will
					invoke handler::unlock_row() to
					clear the flag back to
					ROW_READ_TRY_SEMI_CONSISTENT and
					to simply skip the row.	 If
					the row matches, the next call to
					row_search_for_mysql() will lock
					the row.
					This eliminates lock waits in some
					cases; note that this breaks
					serializability. */
	ulint		new_rec_locks;	/*!< normally 0; if
					the session is using READ
					COMMITTED or READ UNCOMMITTED
					isolation level, set in
					row_search_for_mysql() if we set a new
					record lock on the secondary
					or clustered index; this is
					used in row_unlock_for_mysql()
					when releasing the lock under
					the cursor if we determine
					after retrieving the row that
					it does not need to be locked
					('mini-rollback') */
	ulint		mysql_prefix_len;/*!< byte offset of the end of
					the last requested column */
	ulint		mysql_row_len;	/*!< length in bytes of a row in the
					MySQL format */
	ulint		n_rows_fetched;	/*!< number of rows fetched after
					positioning the current cursor */
	ulint		fetch_direction;/*!< ROW_SEL_NEXT or ROW_SEL_PREV */
	byte*		fetch_cache[MYSQL_FETCH_CACHE_SIZE];
					/*!< a cache for fetched rows if we
					fetch many rows from the same cursor:
					it saves CPU time to fetch them in a
					batch; we reserve mysql_row_len
					bytes for each such row; these
					pointers point 4 bytes past the
					allocated mem buf start, because
					there is a 4 byte magic number at the
					start and at the end */
	bool		keep_other_fields_on_keyread; /*!< when using fetch
					cache with HA_EXTRA_KEYREAD, don't
					overwrite other fields in mysql row
					row buffer.*/
	ulint		fetch_cache_first;/*!< position of the first not yet
					fetched row in fetch_cache */
	ulint		n_fetch_cached;	/*!< number of not yet fetched rows
					in fetch_cache */
	mem_heap_t*	blob_heap;	/*!< in SELECTS BLOB fields are copied
					to this heap */
	mem_heap_t*	old_vers_heap;	/*!< memory heap where a previous
					version is built in consistent read */
	bool		in_fts_query;	/*!< Whether we are in a FTS query */
	bool		fts_doc_id_in_read_set; /*!< true if table has externally
					defined FTS_DOC_ID coulmn. */
	/*----------------------*/
	ulonglong	autoinc_last_value;
					/*!< last value of AUTO-INC interval */
	ulonglong	autoinc_increment;/*!< The increment step of the auto
					increment column. Value must be
					greater than or equal to 1. Required to
					calculate the next value */
	ulonglong	autoinc_offset; /*!< The offset passed to
					get_auto_increment() by MySQL. Required
					to calculate the next value */
	dberr_t		autoinc_error;	/*!< The actual error code encountered
					while trying to init or read the
					autoinc value from the table. We
					store it here so that we can return
					it to MySQL */
	/*----------------------*/

	/** Argument of handler_rowid_filter_check(),
	or NULL if no PRIMARY KEY filter is pushed */
	ha_innobase*	pk_filter;

	/** Argument to handler_index_cond_check(),
	or NULL if no index condition pushdown (ICP) is used. */
	ha_innobase*	idx_cond;
	ulint		idx_cond_n_cols;/*!< Number of fields in idx_cond_cols.
					0 if and only if idx_cond == NULL. */
	/*----------------------*/

	/*----------------------*/
	rtr_info_t*	rtr_info;	/*!< R-tree Search Info */
	/*----------------------*/

	ulint		magic_n2;	/*!< this should be the same as
					magic_n */

	byte*		srch_key_val1;  /*!< buffer used in converting
					search key values from MySQL format
					to InnoDB format.*/
	byte*		srch_key_val2;  /*!< buffer used in converting
					search key values from MySQL format
					to InnoDB format.*/
	uint		srch_key_val_len; /*!< Size of search key */
	/** The MySQL table object */
	TABLE*		m_mysql_table;

	/** Get template by dict_table_t::cols[] number */
	const mysql_row_templ_t* get_template_by_col(ulint col) const
	{
		ut_ad(col < n_template);
		ut_ad(mysql_template);
		for (ulint i = col; i < n_template; ++i) {
			const mysql_row_templ_t* templ = &mysql_template[i];
			if (!templ->is_virtual && templ->col_no == col) {
				return templ;
			}
		}
		return NULL;
	}
};

/** Callback for row_mysql_sys_index_iterate() */
struct SysIndexCallback {
	virtual ~SysIndexCallback() { }

	/** Callback method
	@param mtr current mini transaction
	@param pcur persistent cursor. */
	virtual void operator()(mtr_t* mtr, btr_pcur_t* pcur) throw() = 0;
};


/** Storage for calculating virtual columns */

class String;
struct VCOL_STORAGE
{
	TABLE *maria_table;
	byte *innobase_record;
	byte *maria_record;
	String *blob_value_storage;
	VCOL_STORAGE(): maria_table(NULL), innobase_record(NULL),
		maria_record(NULL),  blob_value_storage(NULL) {}
};

/**
   Allocate a heap and record for calculating virtual fields
   Used mainly for virtual fields in indexes

@param[in]	thd		MariaDB THD
@param[in]	index		Index in use
@param[out]	heap		Heap that holds temporary row
@param[in,out]	mysql_table	MariaDB table
@param[out]	rec		Pointer to allocated MariaDB record
@param[out]	storage		Internal storage for blobs etc

@return		FALSE ok
@return		TRUE  malloc failure
*/

bool innobase_allocate_row_for_vcol(
				    THD *	  thd,
				    dict_index_t* index,
				    mem_heap_t**  heap,
				    TABLE**	  table,
				    VCOL_STORAGE* storage);

/** Free memory allocated by innobase_allocate_row_for_vcol() */
void innobase_free_row_for_vcol(VCOL_STORAGE *storage);

class ib_vcol_row
{
  VCOL_STORAGE storage;
public:
  mem_heap_t *heap;

  ib_vcol_row(mem_heap_t *heap) : heap(heap) {}

  byte *record(THD *thd, dict_index_t *index, TABLE **table)
  {
    if (!storage.innobase_record)
    {
      bool ok = innobase_allocate_row_for_vcol(thd, index, &heap, table,
                                               &storage);
      if (!ok)
        return NULL;
    }
    return storage.innobase_record;
  };

  ~ib_vcol_row()
  {
    if (heap)
    {
      if (storage.innobase_record)
        innobase_free_row_for_vcol(&storage);
      mem_heap_free(heap);
    }
  }
};

/** Report virtual value computation failure in ib::error
@param[in]    row    the data row
*/
ATTRIBUTE_COLD
void innobase_report_computed_value_failed(dtuple_t *row);

/** Get the computed value by supplying the base column values.
@param[in,out]	row		the data row
@param[in]	col		virtual column
@param[in]	index		index on the virtual column
@param[in,out]	local_heap	heap memory for processing large data etc.
@param[in,out]	heap		memory heap that copies the actual index row
@param[in]	ifield		index field
@param[in]	thd		connection handle
@param[in,out]	mysql_table	MariaDB table handle
@param[in,out]	mysql_rec	MariaDB record buffer
@param[in]	old_table	during ALTER TABLE, this is the old table
				or NULL.
@param[in]	update	update vector for the parent row
@param[in]	foreign		foreign key information
@return the field filled with computed value */
dfield_t*
innobase_get_computed_value(
	dtuple_t*		row,
	const dict_v_col_t*	col,
	const dict_index_t*	index,
	mem_heap_t**		local_heap,
	mem_heap_t*		heap,
	const dict_field_t*	ifield,
	THD*			thd,
	TABLE*			mysql_table,
	byte*			mysql_rec,
	const dict_table_t*	old_table,
	const upd_t*		update);

/** Get the computed value by supplying the base column values.
@param[in,out]	table		the table whose virtual column
				template to be built */
TABLE* innobase_init_vc_templ(dict_table_t* table);

/** Change dbname and table name in table->vc_templ.
@param[in,out]	table	the table whose virtual column template
dbname and tbname to be renamed. */
void
innobase_rename_vc_templ(
	dict_table_t*	table);

#define ROW_PREBUILT_FETCH_MAGIC_N	465765687

#define ROW_MYSQL_WHOLE_ROW	0
#define ROW_MYSQL_REC_FIELDS	1
#define ROW_MYSQL_NO_TEMPLATE	2
#define ROW_MYSQL_DUMMY_TEMPLATE 3	/* dummy template used in
					row_scan_and_check_index */

/* Values for hint_need_to_fetch_extra_cols */
#define ROW_RETRIEVE_PRIMARY_KEY	1
#define ROW_RETRIEVE_ALL_COLS		2

/* Values for row_read_type */
#define ROW_READ_WITH_LOCKS		0
#define ROW_READ_TRY_SEMI_CONSISTENT	1
#define ROW_READ_DID_SEMI_CONSISTENT	2

#endif /* row0mysql.h */
