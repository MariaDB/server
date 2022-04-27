/*****************************************************************************

Copyright (c) 2005, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2022, MariaDB Corporation.

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
@file include/row0merge.h
Index build routines using a merge sort

Created 13/06/2005 Jan Lindstrom
*******************************************************/

#pragma once

#include "que0types.h"
#include "trx0types.h"
#include "mtr0mtr.h"
#include "rem0types.h"
#include "rem0rec.h"
#include "btr0types.h"
#include "row0mysql.h"
#include "lock0types.h"
#include "srv0srv.h"

class ut_stage_alter_t;

/* Reserve free space from every block for key_version */
#define ROW_MERGE_RESERVE_SIZE 4

/* Cluster index read task is mandatory */
#define COST_READ_CLUSTERED_INDEX            1.0

/* Basic fixed cost to build all type of index */
#define COST_BUILD_INDEX_STATIC              0.5
/* Dynamic cost to build all type of index, dynamic cost will be re-distributed based on page count ratio of each index */
#define COST_BUILD_INDEX_DYNAMIC             0.5

/* Sum of below two must be 1.0 */
#define PCT_COST_MERGESORT_INDEX                 0.4
#define PCT_COST_INSERT_INDEX                    0.6

// Forward declaration
struct ib_sequence_t;

/** @brief Block size for I/O operations in merge sort.

The minimum is srv_page_size, or page_get_free_space_of_empty()
rounded to a power of 2.

When not creating a PRIMARY KEY that contains column prefixes, this
can be set as small as srv_page_size / 2. */
typedef byte	row_merge_block_t;

/** @brief Secondary buffer for I/O operations of merge records.

This buffer is used for writing or reading a record that spans two
row_merge_block_t.  Thus, it must be able to hold one merge record,
whose maximum size is the same as the minimum size of
row_merge_block_t. */
typedef byte	mrec_buf_t[UNIV_PAGE_SIZE_MAX];

/** @brief Merge record in row_merge_block_t.

The format is the same as a record in ROW_FORMAT=COMPACT with the
exception that the REC_N_NEW_EXTRA_BYTES are omitted. */
typedef byte	mrec_t;

/** Merge record in row_merge_buf_t */
struct mtuple_t {
	dfield_t*	fields;		/*!< data fields */
};

/** Buffer for sorting in main memory. */
struct row_merge_buf_t {
	mem_heap_t*	heap;		/*!< memory heap where allocated */
	dict_index_t*	index;		/*!< the index the tuples belong to */
	ulint		total_size;	/*!< total amount of data bytes */
	ulint		n_tuples;	/*!< number of data tuples */
	ulint		max_tuples;	/*!< maximum number of data tuples */
	mtuple_t*	tuples;		/*!< array of data tuples */
	mtuple_t*	tmp_tuples;	/*!< temporary copy of tuples,
					for sorting */
};

/** Information about temporary files used in merge sort */
struct merge_file_t {
	pfs_os_file_t	fd;		/*!< file descriptor */
	ulint		offset;		/*!< file offset (end of file) */
	ib_uint64_t	n_rec;		/*!< number of records in the file */
};

/** Index field definition */
struct index_field_t {
	ulint		col_no;		/*!< column offset */
	ulint		prefix_len;	/*!< column prefix length, or 0
					if indexing the whole column */
	bool		is_v_col;	/*!< whether this is a virtual column */
	bool		descending;	/*!< whether to use DESC order */
};

/** Definition of an index being created */
struct index_def_t {
	const char*	name;		/*!< index name */
	bool		rebuild;	/*!< whether the table is rebuilt */
	ulint		ind_type;	/*!< 0, DICT_UNIQUE,
					or DICT_CLUSTERED */
	ulint		key_number;	/*!< MySQL key number,
					or ULINT_UNDEFINED if none */
	ulint		n_fields;	/*!< number of fields in index */
	index_field_t*	fields;		/*!< field definitions */
	st_mysql_ftparser*
			parser;		/*!< fulltext parser plugin */
};

/** Structure for reporting duplicate records. */
struct row_merge_dup_t {
	dict_index_t*		index;	/*!< index being sorted */
	struct TABLE*		table;	/*!< MySQL table object */
	const ulint*		col_map;/*!< mapping of column numbers
					in table to the rebuilt table
					(index->table), or NULL if not
					rebuilding table */
	ulint			n_dup;	/*!< number of duplicates */
};

/*************************************************************//**
Report a duplicate key. */
void
row_merge_dup_report(
/*=================*/
	row_merge_dup_t*	dup,	/*!< in/out: for reporting duplicates */
	const dfield_t*		entry)	/*!< in: duplicate index entry */
	MY_ATTRIBUTE((nonnull));

/** Drop indexes that were created before an error occurred.
The data dictionary must have been locked exclusively by the caller,
because the transaction will not be committed.
@param trx              dictionary transaction
@param table            table containing the indexes
@param locked           True if table is locked,
                        false - may need to do lazy drop
@param alter_trx        Alter table transaction */
void
row_merge_drop_indexes(
        trx_t*          trx,
        dict_table_t*   table,
        bool            locked,
        const trx_t*    alter_trx=NULL);

/** During recovery, drop recovered index stubs that were created in
prepare_inplace_alter_table_dict(). */
void row_merge_drop_temp_indexes();

/** Create temporary merge files in the given paramater path, and if
UNIV_PFS_IO defined, register the file descriptor with Performance Schema.
@param[in]	path	location for creating temporary merge files, or NULL
@return File descriptor */
pfs_os_file_t
row_merge_file_create_low(
	const char*	path)
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Destroy a merge file. And de-register the file from Performance Schema
if UNIV_PFS_IO is defined. */
void
row_merge_file_destroy_low(
/*=======================*/
	const pfs_os_file_t&	fd);	/*!< in: merge file descriptor */

/*********************************************************************//**
Rename an index in the dictionary that was created. The data
dictionary must have been locked exclusively by the caller, because
the transaction will not be committed.
@return DB_SUCCESS if all OK */
dberr_t
row_merge_rename_index_to_add(
/*==========================*/
	trx_t*		trx,		/*!< in/out: transaction */
	table_id_t	table_id,	/*!< in: table identifier */
	index_id_t	index_id)	/*!< in: index identifier */
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));

/** Create the index and load in to the dictionary.
@param[in,out]	table		the index is on this table
@param[in]	index_def	the index definition
@param[in]	add_v		new virtual columns added along with add
				index call
@return index, or NULL on error */
dict_index_t*
row_merge_create_index(
	dict_table_t*		table,
	const index_def_t*	index_def,
	const dict_add_v_col_t*	add_v)
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Check if a transaction can use an index.
@return whether the index can be used by the transaction */
bool
row_merge_is_index_usable(
/*======================*/
	const trx_t*		trx,	/*!< in: transaction */
	const dict_index_t*	index)	/*!< in: index to check */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Build indexes on a table by reading a clustered index, creating a temporary
file containing index entries, merge sorting these index entries and inserting
sorted index entries to indexes.
@param[in]	trx		transaction
@param[in]	old_table	table where rows are read from
@param[in]	new_table	table where indexes are created; identical to
old_table unless creating a PRIMARY KEY
@param[in]	online		true if creating indexes online
@param[in]	indexes		indexes to be created
@param[in]	key_numbers	MySQL key numbers
@param[in]	n_indexes	size of indexes[]
@param[in,out]	table		MySQL table, for reporting erroneous key value
if applicable
@param[in]	defaults	default values of added, changed columns, or NULL
@param[in]	col_map		mapping of old column numbers to new ones, or
NULL if old_table == new_table
@param[in]	add_autoinc	number of added AUTO_INCREMENT columns, or
ULINT_UNDEFINED if none is added
@param[in,out]	sequence	autoinc sequence
@param[in]	skip_pk_sort	whether the new PRIMARY KEY will follow
existing order
@param[in,out]	stage		performance schema accounting object, used by
ALTER TABLE. stage->begin_phase_read_pk() will be called at the beginning of
this function and it will be passed to other functions for further accounting.
@param[in]	add_v		new virtual columns added along with indexes
@param[in]	eval_table	mysql table used to evaluate virtual column
				value, see innobase_get_computed_value().
@param[in]	allow_non_null	allow the conversion from null to not-null
@return DB_SUCCESS or error code */
dberr_t
row_merge_build_indexes(
	trx_t*			trx,
	dict_table_t*		old_table,
	dict_table_t*		new_table,
	bool			online,
	dict_index_t**		indexes,
	const ulint*		key_numbers,
	ulint			n_indexes,
	struct TABLE*		table,
	const dtuple_t*		defaults,
	const ulint*		col_map,
	ulint			add_autoinc,
	ib_sequence_t&		sequence,
	bool			skip_pk_sort,
	ut_stage_alter_t*	stage,
	const dict_add_v_col_t*	add_v,
	struct TABLE*		eval_table,
	bool			allow_non_null)
	MY_ATTRIBUTE((warn_unused_result));

/** Write a buffer to a block.
@param buf              sorted buffer
@param block            buffer for writing to file
@param blob_file        blob file handle for doing bulk insert operation */
dberr_t row_merge_buf_write(const row_merge_buf_t *buf,
#ifndef DBUG_OFF
                            const merge_file_t *of, /*!< output file */
#endif
                            row_merge_block_t *block,
                            merge_file_t *blob_file= nullptr);

/********************************************************************//**
Sort a buffer. */
void
row_merge_buf_sort(
/*===============*/
	row_merge_buf_t*	buf,	/*!< in/out: sort buffer */
	row_merge_dup_t*	dup)	/*!< in/out: reporter of duplicates
					(NULL if non-unique index) */
	MY_ATTRIBUTE((nonnull(1)));

/********************************************************************//**
Write a merge block to the file system.
@return whether the request was completed successfully
@retval	false	on error
@retval	true	on success */
bool
row_merge_write(
	const pfs_os_file_t&	fd,	/*!< in: file descriptor */
	ulint		offset,	/*!< in: offset where to write,
				in number of row_merge_block_t elements */
	const void*	buf,	/*!< in: data */
	void*		crypt_buf,		/*!< in: crypt buf or NULL */
	ulint		space)			/*!< in: space id */
	MY_ATTRIBUTE((warn_unused_result));

/********************************************************************//**
Empty a sort buffer.
@return sort buffer */
row_merge_buf_t*
row_merge_buf_empty(
/*================*/
	row_merge_buf_t*	buf)	/*!< in,own: sort buffer */
	MY_ATTRIBUTE((warn_unused_result, nonnull));

/** Create a merge file in the given location.
@param[out]	merge_file	merge file structure
@param[in]	path		location for creating temporary file, or NULL
@return file descriptor, or -1 on failure */
pfs_os_file_t
row_merge_file_create(
	merge_file_t*	merge_file,
	const char*	path)
	MY_ATTRIBUTE((warn_unused_result, nonnull(1)));

/** Merge disk files.
@param[in]	trx	transaction
@param[in]	dup	descriptor of index being created
@param[in,out]	file	file containing index entries
@param[in,out]	block	3 buffers
@param[in,out]	tmpfd	temporary file handle
@param[in]      update_progress true, if we should update progress status
@param[in]      pct_progress total progress percent until now
@param[in]      pct_ocst current progress percent
@param[in]      crypt_block crypt buf or NULL
@param[in]      space    space_id
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. If not NULL, stage->begin_phase_sort() will be called initially
and then stage->inc() will be called for each record processed.
@return DB_SUCCESS or error code */
dberr_t
row_merge_sort(
/*===========*/
	trx_t*			trx,
	const row_merge_dup_t*	dup,
	merge_file_t*		file,
	row_merge_block_t*	block,
	pfs_os_file_t*		tmpfd,
	const bool		update_progress,
	const double	pct_progress,
	const double	pct_cost,
	row_merge_block_t*	crypt_block,
	ulint			space,
	ut_stage_alter_t*	stage = NULL)
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Allocate a sort buffer.
@return own: sort buffer */
row_merge_buf_t*
row_merge_buf_create(
/*=================*/
	dict_index_t*	index)	/*!< in: secondary index */
	MY_ATTRIBUTE((warn_unused_result, nonnull, malloc));

/*********************************************************************//**
Deallocate a sort buffer. */
void
row_merge_buf_free(
/*===============*/
	row_merge_buf_t*	buf)	/*!< in,own: sort buffer to be freed */
	MY_ATTRIBUTE((nonnull));

/*********************************************************************//**
Destroy a merge file. */
void
row_merge_file_destroy(
/*===================*/
	merge_file_t*	merge_file)	/*!< in/out: merge file structure */
	MY_ATTRIBUTE((nonnull));

/** Read a merge block from the file system.
@return whether the request was completed successfully */
bool
row_merge_read(
/*===========*/
	const pfs_os_file_t&	fd,	/*!< in: file descriptor */
	ulint			offset,	/*!< in: offset where to read
					in number of row_merge_block_t
					elements */
	row_merge_block_t*	buf,	/*!< out: data */
	row_merge_block_t*	crypt_buf, /*!< in: crypt buf or NULL */
	ulint			space)	   /*!< in: space id */
	MY_ATTRIBUTE((warn_unused_result));

/********************************************************************//**
Read a merge record.
@return pointer to next record, or NULL on I/O error or end of list */
const byte*
row_merge_read_rec(
/*===============*/
	row_merge_block_t*	block,	/*!< in/out: file buffer */
	mrec_buf_t*		buf,	/*!< in/out: secondary buffer */
	const byte*		b,	/*!< in: pointer to record */
	const dict_index_t*	index,	/*!< in: index of the record */
	const pfs_os_file_t&	fd,	/*!< in: file descriptor */
	ulint*			foffs,	/*!< in/out: file offset */
	const mrec_t**		mrec,	/*!< out: pointer to merge record,
					or NULL on end of list
					(non-NULL on I/O error) */
	rec_offs*		offsets,/*!< out: offsets of mrec */
	row_merge_block_t*	crypt_block, /*!< in: crypt buf or NULL */
	ulint			space)	   /*!< in: space id */
	MY_ATTRIBUTE((warn_unused_result));

/** Buffer for bulk insert */
class row_merge_bulk_t
{
  /** Buffer for each index in the table. main memory
  buffer for sorting the index */
  row_merge_buf_t *m_merge_buf;
  /** Block for IO operation */
  row_merge_block_t *m_block= nullptr;
  /** File to store the buffer and used for merge sort */
  merge_file_t *m_merge_files= nullptr;
  /** Temporary file to be used for merge sort */
  pfs_os_file_t m_tmpfd;
  /** Allocate memory for merge file data structure */
  ut_allocator<row_merge_block_t> m_alloc;
  /** Storage for description for the m_alloc */
  ut_new_pfx_t m_block_pfx;
  /** Temporary file to store the blob */
  merge_file_t m_blob_file;
public:
  /** Constructor.
  Create all merge files, merge buffer for all the table indexes
  expect fts indexes.
  Create a merge block which is used to write IO operation
  @param table  table which undergoes bulk insert operation */
  row_merge_bulk_t(dict_table_t *table);

  /** Destructor.
  Remove all merge files, merge buffer for all table indexes. */
  ~row_merge_bulk_t();

  /** Remove all buffer for the table indexes */
  void remove_all_bulk_buffer();

  /** Clean the merge buffer for the given index number */
  void clean_bulk_buffer(ulint index_no);

  /** Create the temporary file for the given index number
  @retval true if temporary file creation went well */
  bool create_tmp_file(ulint index_no);

  /** Write the merge buffer to the tmp file for the given
  index number.
  @param index_no       buffer to be written for the index */
  dberr_t write_to_tmp_file(ulint index_no);

  /** Add the tuple to the merge buffer for the given index.
  If the buffer ran out of memory then write the buffer into
  the temporary file and do insert the tuple again.
  @param row     tuple to be inserted
  @param ind     index to be buffered
  @param trx     bulk transaction */
  dberr_t bulk_insert_buffered(const dtuple_t &row, const dict_index_t &ind,
                               trx_t *trx);

  /** Do bulk insert operation into the index tree from
  buffer or merge file if exists
  @param index_no  index to be inserted
  @param trx       bulk transaction */
  dberr_t write_to_index(ulint index_no, trx_t *trx);

  /** Do bulk insert for the buffered insert for the table.
  @param table  table which undergoes for bulk insert operation
  @param trx    bulk transaction */
  dberr_t write_to_table(dict_table_t *table, trx_t *trx);

  /** Allocate block for writing the buffer into disk */
  dberr_t alloc_block();

  /** Init temporary files for each index */
  void init_tmp_file();
};
