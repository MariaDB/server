/*****************************************************************************

Copyright (c) 2012, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file row/row0import.cc
Import a tablespace to a running instance.

Created 2012-02-08 by Sunny Bains.
*******************************************************/

#include "row0import.h"
#include "btr0pcur.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "btr0sea.h"
#endif
#include "que0que.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "pars0pars.h"
#include "row0row.h"
#include "row0sel.h"
#include "row0mysql.h"
#include "srv0start.h"
#include "row0quiesce.h"
#include "fil0pagecompress.h"
#include "trx0undo.h"
#include "lock0lock.h"
#include "lzo/lzo1x.h"
#include "snappy-c.h"

#include "scope.h"

#include <vector>

#ifdef HAVE_MY_AES_H
#include <my_aes.h>
#endif

using st_::span;

/** The size of the buffer to use for IO.
@param n physical page size
@return number of pages */
#define IO_BUFFER_SIZE(n)	((1024 * 1024) / (n))

/** For gathering stats on records during phase I */
struct row_stats_t {
	ulint		m_n_deleted;		/*!< Number of deleted records
						found in the index */

	ulint		m_n_purged;		/*!< Number of records purged
						optimisatically */

	ulint		m_n_rows;		/*!< Number of rows */

	ulint		m_n_purge_failed;	/*!< Number of deleted rows
						that could not be purged */
};

/** Index information required by IMPORT. */
struct row_index_t {
	index_id_t	m_id;			/*!< Index id of the table
						in the exporting server */
	byte*		m_name;			/*!< Index name */

	uint32_t	m_space;		/*!< Space where it is placed */

	uint32_t	m_page_no;		/*!< Root page number */

	ulint		m_type;			/*!< Index type */

	ulint		m_trx_id_offset;	/*!< Relevant only for clustered
						indexes, offset of transaction
						id system column */

	ulint		m_n_user_defined_cols;	/*!< User defined columns */

	ulint		m_n_uniq;		/*!< Number of columns that can
						uniquely identify the row */

	ulint		m_n_nullable;		/*!< Number of nullable
						columns */

	ulint		m_n_fields;		/*!< Total number of fields */

	dict_field_t*	m_fields;		/*!< Index fields */

	const dict_index_t*
			m_srv_index;		/*!< Index instance in the
						importing server */

	row_stats_t	m_stats;		/*!< Statistics gathered during
						the import phase */

};

/** Meta data required by IMPORT. */
struct row_import {
	row_import() UNIV_NOTHROW
		:
		m_table(NULL),
		m_version(0),
		m_hostname(NULL),
		m_table_name(NULL),
		m_autoinc(0),
		m_zip_size(0),
		m_flags(0),
		m_n_cols(0),
		m_cols(NULL),
		m_col_names(NULL),
		m_n_indexes(0),
		m_indexes(NULL),
		m_missing(true) { }

	~row_import() UNIV_NOTHROW;

	/** Find the index entry in in the indexes array.
	@param name index name
	@return instance if found else 0. */
	row_index_t* get_index(const char* name) const UNIV_NOTHROW;

	/** Get the number of rows in the index.
	@param name index name
	@return number of rows (doesn't include delete marked rows). */
	ulint	get_n_rows(const char* name) const UNIV_NOTHROW;

	/** Find the ordinal value of the column name in the cfg table columns.
	@param name of column to look for.
	@return ULINT_UNDEFINED if not found. */
	ulint find_col(const char* name) const UNIV_NOTHROW;

	/** Get the number of rows for which purge failed during the
	convert phase.
	@param name index name
	@return number of rows for which purge failed. */
	ulint get_n_purge_failed(const char* name) const UNIV_NOTHROW;

	/** Check if the index is clean. ie. no delete-marked records
	@param name index name
	@return true if index needs to be purged. */
	bool requires_purge(const char* name) const UNIV_NOTHROW
	{
		return(get_n_purge_failed(name) > 0);
	}

	/** Set the index root <space, pageno> using the index name */
	void set_root_by_name() UNIV_NOTHROW;

	/** Set the index root <space, pageno> using a heuristic
	@return DB_SUCCESS or error code */
	dberr_t set_root_by_heuristic() UNIV_NOTHROW;

	/** Check if the index schema that was read from the .cfg file
	matches the in memory index definition.
	Note: It will update row_import_t::m_srv_index to map the meta-data
	read from the .cfg file to the server index instance.
	@return DB_SUCCESS or error code. */
	dberr_t match_index_columns(
		THD*			thd,
		const dict_index_t*	index) UNIV_NOTHROW;

	/** Check if the table schema that was read from the .cfg file
	matches the in memory table definition.
	@param thd MySQL session variable
	@return DB_SUCCESS or error code. */
	dberr_t match_table_columns(
		THD*			thd) UNIV_NOTHROW;

	/** Check if the table (and index) schema that was read from the
	.cfg file matches the in memory table definition.
	@param thd MySQL session variable
	@return DB_SUCCESS or error code. */
	dberr_t match_schema(
		THD*			thd) UNIV_NOTHROW;

	dberr_t match_flags(THD *thd) const ;


	dict_table_t*	m_table;		/*!< Table instance */

	ulint		m_version;		/*!< Version of config file */

	byte*		m_hostname;		/*!< Hostname where the
						tablespace was exported */
	byte*		m_table_name;		/*!< Exporting instance table
						name */

	ib_uint64_t	m_autoinc;		/*!< Next autoinc value */

	ulint		m_zip_size;		/*!< ROW_FORMAT=COMPRESSED
						page size, or 0 */

	ulint		m_flags;		/*!< Table flags */

	ulint		m_n_cols;		/*!< Number of columns in the
						meta-data file */

	dict_col_t*	m_cols;			/*!< Column data */

	byte**		m_col_names;		/*!< Column names, we store the
						column naems separately becuase
						there is no field to store the
						value in dict_col_t */

	ulint		m_n_indexes;		/*!< Number of indexes,
						including clustered index */

	row_index_t*	m_indexes;		/*!< Index meta data */

	bool		m_missing;		/*!< true if a .cfg file was
						found and was readable */
};

struct fil_iterator_t {
	pfs_os_file_t	file;			/*!< File handle */
	const char*	filepath;		/*!< File path name */
	os_offset_t	start;			/*!< From where to start */
	os_offset_t	end;			/*!< Where to stop */
	os_offset_t	file_size;		/*!< File size in bytes */
	ulint		n_io_buffers;		/*!< Number of pages to use
						for IO */
	byte*		io_buffer;		/*!< Buffer to use for IO */
	fil_space_crypt_t *crypt_data;		/*!< Crypt data (if encrypted) */
	byte*           crypt_io_buffer;        /*!< IO buffer when encrypted */
};

/** Use the page cursor to iterate over records in a block. */
class RecIterator {
public:
	/** Default constructor */
	RecIterator() UNIV_NOTHROW
	{
		memset(&m_cur, 0x0, sizeof(m_cur));
		/* Make page_cur_delete_rec() happy. */
		m_mtr.start();
		m_mtr.set_log_mode(MTR_LOG_NO_REDO);
	}

	/** Position the cursor on the first user record. */
	void	open(buf_block_t* block) UNIV_NOTHROW
	{
		page_cur_set_before_first(block, &m_cur);

		if (!end()) {
			next();
		}
	}

	/** Move to the next record. */
	void	next() UNIV_NOTHROW
	{
		page_cur_move_to_next(&m_cur);
	}

	/**
	@return the current record */
	rec_t*	current() UNIV_NOTHROW
	{
		ut_ad(!end());
		return(page_cur_get_rec(&m_cur));
	}

	buf_block_t* current_block() const { return m_cur.block; }

	/**
	@return true if cursor is at the end */
	bool	end() UNIV_NOTHROW
	{
		return(page_cur_is_after_last(&m_cur) == TRUE);
	}

	/** Remove the current record
	@return true on success */
	bool remove(
		const dict_index_t*	index,
		rec_offs*		offsets) UNIV_NOTHROW
	{
		ut_ad(page_is_leaf(m_cur.block->page.frame));
		/* We can't end up with an empty page unless it is root. */
		if (page_get_n_recs(m_cur.block->page.frame) <= 1) {
			return(false);
		}

		if (!rec_offs_any_extern(offsets)
		    && m_cur.block->page.id().page_no() != index->page
		    && ((page_get_data_size(m_cur.block->page.frame)
			 - rec_offs_size(offsets)
			 < BTR_CUR_PAGE_COMPRESS_LIMIT(index))
			|| !page_has_siblings(m_cur.block->page.frame)
			|| (page_get_n_recs(m_cur.block->page.frame) < 2))) {
			return false;
		}

#ifdef UNIV_ZIP_DEBUG
		page_zip_des_t* page_zip = buf_block_get_page_zip(m_cur.block);
		ut_a(!page_zip || page_zip_validate(
			     page_zip, m_cur.block->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */

		page_cur_delete_rec(&m_cur, index, offsets, &m_mtr);

#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(
			     page_zip, m_cur.block->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */

		return true;
	}

private:
	page_cur_t	m_cur;
public:
	mtr_t		m_mtr;
};

/** Class that purges delete marked records from indexes, both secondary
and cluster. It does a pessimistic delete. This should only be done if we
couldn't purge the delete marked reocrds during Phase I. */
class IndexPurge {
public:
	/** Constructor
	@param trx the user transaction covering the import tablespace
	@param index to be imported
	@param space_id space id of the tablespace */
	IndexPurge(
		trx_t*		trx,
		dict_index_t*	index) UNIV_NOTHROW
		:
		m_trx(trx),
		m_index(index),
		m_n_rows(0)
	{
		ib::info() << "Phase II - Purge records from index "
			<< index->name;
	}

	/** Descructor */
	~IndexPurge() UNIV_NOTHROW { }

	/** Purge delete marked records.
	@return DB_SUCCESS or error code. */
	dberr_t	garbage_collect() UNIV_NOTHROW;

	/** The number of records that are not delete marked.
	@return total records in the index after purge */
	ulint	get_n_rows() const UNIV_NOTHROW
	{
		return(m_n_rows);
	}

private:
	/** Begin import, position the cursor on the first record. */
	void	open() UNIV_NOTHROW;

	/** Close the persistent curosr and commit the mini-transaction. */
	void	close() UNIV_NOTHROW;

	/** Position the cursor on the next record.
	@return DB_SUCCESS or error code */
	dberr_t	next() UNIV_NOTHROW;

	/** Store the persistent cursor position and reopen the
	B-tree cursor in BTR_MODIFY_TREE mode, because the
	tree structure may be changed during a pessimistic delete. */
	void	purge_pessimistic_delete() UNIV_NOTHROW;

	/** Purge delete-marked records.
	@param offsets current row offsets. */
	void	purge() UNIV_NOTHROW;

protected:
	// Disable copying
	IndexPurge();
	IndexPurge(const IndexPurge&);
	IndexPurge &operator=(const IndexPurge&);

private:
	trx_t*			m_trx;		/*!< User transaction */
	mtr_t			m_mtr;		/*!< Mini-transaction */
	btr_pcur_t		m_pcur;		/*!< Persistent cursor */
	dict_index_t*		m_index;	/*!< Index to be processed */
	ulint			m_n_rows;	/*!< Records in index */
};

/** Functor that is called for each physical page that is read from the
tablespace file.  */
class AbstractCallback
{
public:
	/** Constructor
	@param trx covering transaction */
	AbstractCallback(trx_t* trx, uint32_t space_id)
		:
		m_zip_size(0),
		m_trx(trx),
		m_space(space_id),
		m_xdes(),
		m_xdes_page_no(UINT32_MAX),
		m_space_flags(UINT32_MAX) UNIV_NOTHROW { }

	/** Free any extent descriptor instance */
	virtual ~AbstractCallback()
	{
		UT_DELETE_ARRAY(m_xdes);
	}

	/** Determine the page size to use for traversing the tablespace
	@param file_size size of the tablespace file in bytes
	@param block contents of the first page in the tablespace file.
	@retval DB_SUCCESS or error code. */
	virtual dberr_t init(
		os_offset_t		file_size,
		const buf_block_t*	block) UNIV_NOTHROW;

	/** @return true if compressed table. */
	bool is_compressed_table() const UNIV_NOTHROW
	{
		return get_zip_size();
	}

	/** @return the tablespace flags */
	uint32_t get_space_flags() const { return m_space_flags; }

	/**
	Set the name of the physical file and the file handle that is used
	to open it for the file that is being iterated over.
	@param filename the physical name of the tablespace file
	@param file OS file handle */
	void set_file(const char* filename, pfs_os_file_t file) UNIV_NOTHROW
	{
		m_file = file;
		m_filepath = filename;
	}

	ulint get_zip_size() const { return m_zip_size; }
	ulint physical_size() const
	{
		return m_zip_size ? m_zip_size : srv_page_size;
	}

	const char* filename() const { return m_filepath; }

	/**
	Called for every page in the tablespace. If the page was not
	updated then its state must be set to BUF_PAGE_NOT_USED. For
	compressed tables the page descriptor memory will be at offset:
		block->page.frame + srv_page_size;
	@param block block read from file, note it is not from the buffer pool
	@retval DB_SUCCESS or error code. */
	virtual dberr_t operator()(buf_block_t* block) UNIV_NOTHROW = 0;

	/** @return the tablespace identifier */
	uint32_t get_space_id() const { return m_space; }

	bool is_interrupted() const { return trx_is_interrupted(m_trx); }

	/**
	Get the data page depending on the table type, compressed or not.
	@param block - block read from disk
	@retval the buffer frame */
	static byte* get_frame(const buf_block_t* block)
	{
		return block->page.zip.data
			? block->page.zip.data : block->page.frame;
	}

	/** Invoke the functionality for the callback */
	virtual dberr_t run(const fil_iterator_t& iter,
			    buf_block_t* block) UNIV_NOTHROW = 0;

protected:
	/** Get the physical offset of the extent descriptor within the page.
	@param page_no page number of the extent descriptor
	@param page contents of the page containing the extent descriptor.
	@return the start of the xdes array in a page */
	const xdes_t* xdes(
		ulint		page_no,
		const page_t*	page) const UNIV_NOTHROW
	{
		ulint	offset;

		offset = xdes_calc_descriptor_index(get_zip_size(), page_no);

		return(page + XDES_ARR_OFFSET + XDES_SIZE * offset);
	}

	/** Set the current page directory (xdes). If the extent descriptor is
	marked as free then free the current extent descriptor and set it to
	0. This implies that all pages that are covered by this extent
	descriptor are also freed.

	@param page_no offset of page within the file
	@param page page contents
	@return DB_SUCCESS or error code. */
	dberr_t	set_current_xdes(
		uint32_t	page_no,
		const page_t*	page) UNIV_NOTHROW
	{
		m_xdes_page_no = page_no;

		UT_DELETE_ARRAY(m_xdes);
		m_xdes = NULL;

		if (mach_read_from_4(XDES_ARR_OFFSET + XDES_STATE + page)
		    != XDES_FREE) {
			const ulint physical_size = m_zip_size
				? m_zip_size : srv_page_size;

			m_xdes = UT_NEW_ARRAY_NOKEY(xdes_t, physical_size);

			/* Trigger OOM */
			DBUG_EXECUTE_IF(
				"ib_import_OOM_13",
				UT_DELETE_ARRAY(m_xdes);
				m_xdes = NULL;
			);

			if (m_xdes == NULL) {
				return(DB_OUT_OF_MEMORY);
			}

			memcpy(m_xdes, page, physical_size);
		}

		return(DB_SUCCESS);
	}

	/** Check if the page is marked as free in the extent descriptor.
	@param page_no page number to check in the extent descriptor.
	@return true if the page is marked as free */
	bool is_free(uint32_t page_no) const UNIV_NOTHROW
	{
		ut_a(xdes_calc_descriptor_page(get_zip_size(), page_no)
		     == m_xdes_page_no);

		if (m_xdes != 0) {
			const xdes_t*	xdesc = xdes(page_no, m_xdes);
			ulint		pos = page_no % FSP_EXTENT_SIZE;

			return xdes_is_free(xdesc, pos);
		}

		/* If the current xdes was free, the page must be free. */
		return(true);
	}

protected:
	/** The ROW_FORMAT=COMPRESSED page size, or 0. */
	ulint			m_zip_size;

	/** File handle to the tablespace */
	pfs_os_file_t		m_file;

	/** Physical file path. */
	const char*		m_filepath;

	/** Covering transaction. */
	trx_t*			m_trx;

	/** Space id of the file being iterated over. */
	uint32_t		m_space;

	/** Current extent descriptor page */
	xdes_t*			m_xdes;

	/** Physical page offset in the file of the extent descriptor */
	uint32_t		m_xdes_page_no;

	/** Flags value read from the header page */
	uint32_t		m_space_flags;
};

/** Determine the page size to use for traversing the tablespace
@param file_size size of the tablespace file in bytes
@param block contents of the first page in the tablespace file.
@retval DB_SUCCESS or error code. */
dberr_t
AbstractCallback::init(
	os_offset_t		file_size,
	const buf_block_t*	block) UNIV_NOTHROW
{
	const page_t*		page = block->page.frame;

	m_space_flags = fsp_header_get_flags(page);
	if (!fil_space_t::is_valid_flags(m_space_flags, true)) {
		uint32_t cflags = fsp_flags_convert_from_101(m_space_flags);
		if (cflags == UINT32_MAX) {
			return(DB_CORRUPTION);
		}
		m_space_flags = cflags;
	}

	/* Clear the DATA_DIR flag, which is basically garbage. */
	m_space_flags &= ~(1U << FSP_FLAGS_POS_RESERVED);
	m_zip_size = fil_space_t::zip_size(m_space_flags);
	const ulint logical_size = fil_space_t::logical_size(m_space_flags);
	const ulint physical_size = fil_space_t::physical_size(m_space_flags);

	if (logical_size != srv_page_size) {

		ib::error() << "Page size " << logical_size
			<< " of ibd file is not the same as the server page"
			" size " << srv_page_size;

		return(DB_CORRUPTION);

	} else if (file_size & (physical_size - 1)) {

		ib::error() << "File size " << file_size << " is not a"
			" multiple of the page size "
			<< physical_size;

		return(DB_CORRUPTION);
	}

	if (m_space == UINT32_MAX) {
		m_space = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_ID
					   + page);
	}

	return set_current_xdes(0, page);
}

/**
TODO: This can be made parallel trivially by chunking up the file
and creating a callback per thread.. Main benefit will be to use
multiple CPUs for checksums and compressed tables. We have to do
compressed tables block by block right now. Secondly we need to
decompress/compress and copy too much of data. These are
CPU intensive.

Iterate over all the pages in the tablespace.
@param iter - Tablespace iterator
@param block - block to use for IO
@param callback - Callback to inspect and update page contents
@retval DB_SUCCESS or error code */
static dberr_t fil_iterate(
	const fil_iterator_t&	iter,
	buf_block_t*		block,
	AbstractCallback&	callback);

/**
Try and determine the index root pages by checking if the next/prev
pointers are both FIL_NULL. We need to ensure that skip deleted pages. */
struct FetchIndexRootPages : public AbstractCallback {

	/** Index information gathered from the .ibd file. */
	struct Index {

		Index(index_id_t id, uint32_t page_no)
			:
			m_id(id),
			m_page_no(page_no) { }

		index_id_t	m_id;		/*!< Index id */
		uint32_t	m_page_no;	/*!< Root page number */
	};

	/** Constructor
	@param trx covering (user) transaction
	@param table table definition in server .*/
	FetchIndexRootPages(const dict_table_t* table, trx_t* trx)
		:
		AbstractCallback(trx, UINT32_MAX),
		m_table(table), m_index(0, 0) UNIV_NOTHROW { }

	/** Destructor */
	~FetchIndexRootPages() UNIV_NOTHROW override { }

	/** Fetch the clustered index root page in the tablespace
	@param iter	Tablespace iterator
	@param block	Block to use for IO
	@retval DB_SUCCESS or error code */
	dberr_t run(const fil_iterator_t& iter,
		    buf_block_t* block) UNIV_NOTHROW override;

	/** Called for each block as it is read from the file.
	@param block block to convert, it is not from the buffer pool.
	@retval DB_SUCCESS or error code. */
	dberr_t operator()(buf_block_t* block) UNIV_NOTHROW override;

	/** Update the import configuration that will be used to import
	the tablespace. */
	dberr_t build_row_import(row_import* cfg) const UNIV_NOTHROW;

	/** Table definition in server. */
	const dict_table_t*	m_table;

	/** Index information */
	Index			m_index;
};

/** Called for each block as it is read from the file. Check index pages to
determine the exact row format. We can't get that from the tablespace
header flags alone.

@param block block to convert, it is not from the buffer pool.
@retval DB_SUCCESS or error code. */
dberr_t FetchIndexRootPages::operator()(buf_block_t* block) UNIV_NOTHROW
{
	if (is_interrupted()) return DB_INTERRUPTED;

	const page_t*	page = get_frame(block);

	m_index.m_id = btr_page_get_index_id(page);
	m_index.m_page_no = block->page.id().page_no();

	/* Check that the tablespace flags match the table flags. */
	const uint32_t expected = dict_tf_to_fsp_flags(m_table->flags);
	if (!fsp_flags_match(expected, m_space_flags)) {
		ib_errf(m_trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_SCHEMA_MISMATCH,
			"Expected FSP_SPACE_FLAGS=0x%x, .ibd "
			"file contains 0x%x.",
			unsigned(expected),
			unsigned(m_space_flags));
		return(DB_CORRUPTION);
	}

	if (!page_is_comp(block->page.frame) !=
	    !dict_table_is_comp(m_table)) {
		ib_errf(m_trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_SCHEMA_MISMATCH,
			"ROW_FORMAT mismatch");
		return DB_CORRUPTION;
	}

	return DB_SUCCESS;
}

/**
Update the import configuration that will be used to import the tablespace.
@return error code or DB_SUCCESS */
dberr_t
FetchIndexRootPages::build_row_import(row_import* cfg) const UNIV_NOTHROW
{
	ut_a(cfg->m_table == m_table);
	cfg->m_zip_size = m_zip_size;
	cfg->m_n_indexes = 1;

	if (cfg->m_n_indexes == 0) {

		ib::error() << "No B+Tree found in tablespace";

		return(DB_CORRUPTION);
	}

	cfg->m_indexes = UT_NEW_ARRAY_NOKEY(row_index_t, cfg->m_n_indexes);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_11",
		UT_DELETE_ARRAY(cfg->m_indexes);
		cfg->m_indexes = NULL;
	);

	if (cfg->m_indexes == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	memset(cfg->m_indexes, 0x0, sizeof(*cfg->m_indexes) * cfg->m_n_indexes);

	row_index_t*	cfg_index = cfg->m_indexes;

	char	name[BUFSIZ];

	snprintf(name, sizeof(name), "index" IB_ID_FMT, m_index.m_id);

	ulint	len = strlen(name) + 1;

	cfg_index->m_name = UT_NEW_ARRAY_NOKEY(byte, len);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_12",
		UT_DELETE_ARRAY(cfg_index->m_name);
		cfg_index->m_name = NULL;
	);

	if (cfg_index->m_name == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	memcpy(cfg_index->m_name, name, len);

	cfg_index->m_id = m_index.m_id;

	cfg_index->m_space = m_space;

	cfg_index->m_page_no = m_index.m_page_no;

	return(DB_SUCCESS);
}

/* Functor that is called for each physical page that is read from the
tablespace file.

  1. Check each page for corruption.

  2. Update the space id and LSN on every page
     * For the header page
       - Validate the flags
       - Update the LSN

  3. On Btree pages
     * Set the index id
     * Update the max trx id
     * In a cluster index, update the system columns
     * In a cluster index, update the BLOB ptr, set the space id
     * Purge delete marked records, but only if they can be easily
       removed from the page
     * Keep a counter of number of rows, ie. non-delete-marked rows
     * Keep a counter of number of delete marked rows
     * Keep a counter of number of purge failure
     * If a page is stamped with an index id that isn't in the .cfg file
       we assume it is deleted and the page can be ignored.

   4. Set the page state to dirty so that it will be written to disk.
*/
class PageConverter : public AbstractCallback {
public:
	/** Constructor
	@param cfg config of table being imported.
	@param space_id tablespace identifier
	@param trx transaction covering the import */
	PageConverter(row_import* cfg, uint32_t space_id, trx_t* trx)
		:
		AbstractCallback(trx, space_id),
		m_cfg(cfg),
		m_index(cfg->m_indexes),
		m_rec_iter(),
		m_offsets_(), m_offsets(m_offsets_),
		m_heap(0),
		m_cluster_index(dict_table_get_first_index(cfg->m_table))
	{
		rec_offs_init(m_offsets_);
	}

	~PageConverter() UNIV_NOTHROW override
	{
		if (m_heap != 0) {
			mem_heap_free(m_heap);
		}
	}

	dberr_t run(const fil_iterator_t& iter,
		    buf_block_t* block) UNIV_NOTHROW override
	{
		return fil_iterate(iter, block, *this);
	}

	/** Called for each block as it is read from the file.
	@param block block to convert, it is not from the buffer pool.
	@retval DB_SUCCESS or error code. */
	dberr_t operator()(buf_block_t* block) UNIV_NOTHROW override;

private:
	/** Update the page, set the space id, max trx id and index id.
	@param block block read from file
	@param page_type type of the page
	@retval DB_SUCCESS or error code */
	dberr_t update_page(buf_block_t* block, uint16_t& page_type)
		UNIV_NOTHROW;

	/** Update the space, index id, trx id.
	@param block block to convert
	@return DB_SUCCESS or error code */
	dberr_t	update_index_page(buf_block_t*	block) UNIV_NOTHROW;

	/** Update the BLOB refrences and write UNDO log entries for
	rows that can't be purged optimistically.
	@param block block to update
	@retval DB_SUCCESS or error code */
	dberr_t	update_records(buf_block_t* block) UNIV_NOTHROW;

	/** Validate the space flags and update tablespace header page.
	@param block block read from file, not from the buffer pool.
	@retval DB_SUCCESS or error code */
	dberr_t	update_header(buf_block_t* block) UNIV_NOTHROW;

	/** Adjust the BLOB reference for a single column that is externally stored
	@param rec record to update
	@param offsets column offsets for the record
	@param i column ordinal value
	@return DB_SUCCESS or error code */
	dberr_t	adjust_cluster_index_blob_column(
		rec_t*		rec,
		const rec_offs*	offsets,
		ulint		i) UNIV_NOTHROW;

	/** Adjusts the BLOB reference in the clustered index row for all
	externally stored columns.
	@param rec record to update
	@param offsets column offsets for the record
	@return DB_SUCCESS or error code */
	dberr_t	adjust_cluster_index_blob_columns(
		rec_t*		rec,
		const rec_offs*	offsets) UNIV_NOTHROW;

	/** In the clustered index, adjist the BLOB pointers as needed.
	Also update the BLOB reference, write the new space id.
	@param rec record to update
	@param offsets column offsets for the record
	@return DB_SUCCESS or error code */
	dberr_t	adjust_cluster_index_blob_ref(
		rec_t*		rec,
		const rec_offs*	offsets) UNIV_NOTHROW;

	/** Purge delete-marked records, only if it is possible to do
	so without re-organising the B+tree.
	@retval true if purged */
	bool purge() UNIV_NOTHROW;

	/** Adjust the BLOB references and sys fields for the current record.
	@param rec record to update
	@param offsets column offsets for the record
	@return DB_SUCCESS or error code. */
	dberr_t	adjust_cluster_record(
		rec_t*			rec,
		const rec_offs*		offsets) UNIV_NOTHROW;

	/** Find an index with the matching id.
	@return row_index_t* instance or 0 */
	row_index_t* find_index(index_id_t id) UNIV_NOTHROW
	{
		row_index_t*	index = &m_cfg->m_indexes[0];

		for (ulint i = 0; i < m_cfg->m_n_indexes; ++i, ++index) {
			if (id == index->m_id) {
				return(index);
			}
		}

		return(0);

	}
private:
	/** Config for table that is being imported. */
	row_import*		m_cfg;

	/** Current index whose pages are being imported */
	row_index_t*		m_index;

	/** Iterator over records in a block */
	RecIterator		m_rec_iter;

	/** Record offset */
	rec_offs		m_offsets_[REC_OFFS_NORMAL_SIZE];

	/** Pointer to m_offsets_ */
	rec_offs*		m_offsets;

	/** Memory heap for the record offsets */
	mem_heap_t*		m_heap;

	/** Cluster index instance */
	dict_index_t*		m_cluster_index;
};

/**
row_import destructor. */
row_import::~row_import() UNIV_NOTHROW
{
	for (ulint i = 0; m_indexes != 0 && i < m_n_indexes; ++i) {
		UT_DELETE_ARRAY(m_indexes[i].m_name);

		if (m_indexes[i].m_fields == NULL) {
			continue;
		}

		dict_field_t*	fields = m_indexes[i].m_fields;
		ulint		n_fields = m_indexes[i].m_n_fields;

		for (ulint j = 0; j < n_fields; ++j) {
			UT_DELETE_ARRAY(const_cast<char*>(fields[j].name()));
		}

		UT_DELETE_ARRAY(fields);
	}

	for (ulint i = 0; m_col_names != 0 && i < m_n_cols; ++i) {
		UT_DELETE_ARRAY(m_col_names[i]);
	}

	UT_DELETE_ARRAY(m_cols);
	UT_DELETE_ARRAY(m_indexes);
	UT_DELETE_ARRAY(m_col_names);
	UT_DELETE_ARRAY(m_table_name);
	UT_DELETE_ARRAY(m_hostname);
}

/** Find the index entry in in the indexes array.
@param name index name
@return instance if found else 0. */
row_index_t*
row_import::get_index(
	const char*	name) const UNIV_NOTHROW
{
	for (ulint i = 0; i < m_n_indexes; ++i) {
		const char*	index_name;
		row_index_t*	index = &m_indexes[i];

		index_name = reinterpret_cast<const char*>(index->m_name);

		if (strcmp(index_name, name) == 0) {

			return(index);
		}
	}

	return(0);
}

/** Get the number of rows in the index.
@param name index name
@return number of rows (doesn't include delete marked rows). */
ulint
row_import::get_n_rows(
	const char*	name) const UNIV_NOTHROW
{
	const row_index_t*	index = get_index(name);

	ut_a(name != 0);

	return(index->m_stats.m_n_rows);
}

/** Get the number of rows for which purge failed uding the convert phase.
@param name index name
@return number of rows for which purge failed. */
ulint
row_import::get_n_purge_failed(
	const char*	name) const UNIV_NOTHROW
{
	const row_index_t*	index = get_index(name);

	ut_a(name != 0);

	return(index->m_stats.m_n_purge_failed);
}

/** Find the ordinal value of the column name in the cfg table columns.
@param name of column to look for.
@return ULINT_UNDEFINED if not found. */
ulint
row_import::find_col(
	const char*	name) const UNIV_NOTHROW
{
	for (ulint i = 0; i < m_n_cols; ++i) {
		const char*	col_name;

		col_name = reinterpret_cast<const char*>(m_col_names[i]);

		if (strcmp(col_name, name) == 0) {
			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/**
Check if the index schema that was read from the .cfg file matches the
in memory index definition.
@return DB_SUCCESS or error code. */
dberr_t
row_import::match_index_columns(
	THD*			thd,
	const dict_index_t*	index) UNIV_NOTHROW
{
	row_index_t*		cfg_index;
	dberr_t			err = DB_SUCCESS;

	cfg_index = get_index(index->name);

	if (cfg_index == 0) {
		ib_errf(thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_SCHEMA_MISMATCH,
			"Index %s not found in tablespace meta-data file.",
			index->name());

		return(DB_ERROR);
	}

	if (cfg_index->m_n_fields != index->n_fields) {

		ib_errf(thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_SCHEMA_MISMATCH,
			"Index field count %u doesn't match"
			" tablespace metadata file value " ULINTPF,
			index->n_fields, cfg_index->m_n_fields);

		return(DB_ERROR);
	}

	cfg_index->m_srv_index = index;

	const dict_field_t*	field = index->fields;
	const dict_field_t*	cfg_field = cfg_index->m_fields;

	for (ulint i = 0; i < index->n_fields; ++i, ++field, ++cfg_field) {

		if (field->name() && cfg_field->name()
		     && strcmp(field->name(), cfg_field->name()) != 0) {
			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_TABLE_SCHEMA_MISMATCH,
				"Index field name %s doesn't match"
				" tablespace metadata field name %s"
				" for field position " ULINTPF,
				field->name(), cfg_field->name(), i);

			err = DB_ERROR;
		}

		if (cfg_field->prefix_len != field->prefix_len) {
			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_TABLE_SCHEMA_MISMATCH,
				"Index %s field %s prefix len %u"
				" doesn't match metadata file value %u",
				index->name(), field->name(),
				field->prefix_len, cfg_field->prefix_len);

			err = DB_ERROR;
		}

		if (cfg_field->fixed_len != field->fixed_len) {
			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_TABLE_SCHEMA_MISMATCH,
				"Index %s field %s fixed len %u"
				" doesn't match metadata file value %u",
				index->name(), field->name(),
				field->fixed_len,
				cfg_field->fixed_len);

			err = DB_ERROR;
		}
	}

	return(err);
}

/** Check if the table schema that was read from the .cfg file matches the
in memory table definition.
@param thd MySQL session variable
@return DB_SUCCESS or error code. */
dberr_t
row_import::match_table_columns(
	THD*			thd) UNIV_NOTHROW
{
	dberr_t			err = DB_SUCCESS;
	const dict_col_t*	col = m_table->cols;

	for (ulint i = 0; i < m_table->n_cols; ++i, ++col) {

		const char*	col_name;
		ulint		cfg_col_index;

		col_name = dict_table_get_col_name(
			m_table, dict_col_get_no(col));

		cfg_col_index = find_col(col_name);

		if (cfg_col_index == ULINT_UNDEFINED) {

			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				 ER_TABLE_SCHEMA_MISMATCH,
				 "Column %s not found in tablespace.",
				 col_name);

			err = DB_ERROR;
		} else if (cfg_col_index != col->ind) {

			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_TABLE_SCHEMA_MISMATCH,
				"Column %s ordinal value mismatch, it's at %u"
				" in the table and " ULINTPF
				" in the tablespace meta-data file",
				col_name, col->ind, cfg_col_index);

			err = DB_ERROR;
		} else {
			const dict_col_t*	cfg_col;

			cfg_col = &m_cols[cfg_col_index];
			ut_a(cfg_col->ind == cfg_col_index);

			if (cfg_col->prtype != col->prtype) {
				ib_errf(thd,
					IB_LOG_LEVEL_ERROR,
					ER_TABLE_SCHEMA_MISMATCH,
					"Column %s precise type mismatch,"
					" it's 0X%X in the table and 0X%X"
					" in the tablespace meta file",
					col_name, col->prtype, cfg_col->prtype);
				err = DB_ERROR;
			}

			if (cfg_col->mtype != col->mtype) {
				ib_errf(thd,
					IB_LOG_LEVEL_ERROR,
					ER_TABLE_SCHEMA_MISMATCH,
					"Column %s main type mismatch,"
					" it's 0X%X in the table and 0X%X"
					" in the tablespace meta file",
					col_name, col->mtype, cfg_col->mtype);
				err = DB_ERROR;
			}

			if (cfg_col->len != col->len) {
				ib_errf(thd,
					IB_LOG_LEVEL_ERROR,
					ER_TABLE_SCHEMA_MISMATCH,
					"Column %s length mismatch,"
					" it's %u in the table and %u"
					" in the tablespace meta file",
					col_name, col->len, cfg_col->len);
				err = DB_ERROR;
			}

			if (cfg_col->mbminlen != col->mbminlen
			    || cfg_col->mbmaxlen != col->mbmaxlen) {
				ib_errf(thd,
					IB_LOG_LEVEL_ERROR,
					ER_TABLE_SCHEMA_MISMATCH,
					"Column %s multi-byte len mismatch,"
					" it's %u-%u in the table and %u-%u"
					" in the tablespace meta file",
					col_name, col->mbminlen, col->mbmaxlen,
					cfg_col->mbminlen, cfg_col->mbmaxlen);
				err = DB_ERROR;
			}

			if (cfg_col->ind != col->ind) {
				ib_errf(thd,
					IB_LOG_LEVEL_ERROR,
					ER_TABLE_SCHEMA_MISMATCH,
					"Column %s position mismatch,"
					" it's %u in the table and %u"
					" in the tablespace meta file",
					col_name, col->ind, cfg_col->ind);
				err = DB_ERROR;
			}

			if (cfg_col->ord_part != col->ord_part) {
				ib_errf(thd,
					IB_LOG_LEVEL_ERROR,
					ER_TABLE_SCHEMA_MISMATCH,
					"Column %s ordering mismatch,"
					" it's %u in the table and %u"
					" in the tablespace meta file",
					col_name, col->ord_part,
					cfg_col->ord_part);
				err = DB_ERROR;
			}

			if (cfg_col->max_prefix != col->max_prefix) {
				ib_errf(thd,
					IB_LOG_LEVEL_ERROR,
					ER_TABLE_SCHEMA_MISMATCH,
					"Column %s max prefix mismatch"
					" it's %u in the table and %u"
					" in the tablespace meta file",
					col_name, col->max_prefix,
					cfg_col->max_prefix);
				err = DB_ERROR;
			}
		}
	}

	return(err);
}

dberr_t row_import::match_flags(THD *thd) const
{
  ulint mismatch= (m_table->flags ^ m_flags) & ~DICT_TF_MASK_DATA_DIR;
  if (!mismatch)
    return DB_SUCCESS;

  const char *msg;
  if (mismatch & DICT_TF_MASK_ZIP_SSIZE)
  {
    if ((m_table->flags & DICT_TF_MASK_ZIP_SSIZE) &&
        (m_flags & DICT_TF_MASK_ZIP_SSIZE))
    {
      switch (m_flags & DICT_TF_MASK_ZIP_SSIZE) {
      case 0U << DICT_TF_POS_ZIP_SSIZE:
        goto uncompressed;
      case 1U << DICT_TF_POS_ZIP_SSIZE:
        msg= "ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=1";
        break;
      case 2U << DICT_TF_POS_ZIP_SSIZE:
        msg= "ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=2";
        break;
      case 3U << DICT_TF_POS_ZIP_SSIZE:
        msg= "ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4";
        break;
      case 4U << DICT_TF_POS_ZIP_SSIZE:
        msg= "ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8";
        break;
      case 5U << DICT_TF_POS_ZIP_SSIZE:
        msg= "ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=16";
        break;
      default:
        msg= "strange KEY_BLOCK_SIZE";
      }
    }
    else if (m_flags & DICT_TF_MASK_ZIP_SSIZE)
      msg= "ROW_FORMAT=COMPRESSED";
    else
      goto uncompressed;
  }
  else
  {
  uncompressed:
    msg= (m_flags & DICT_TF_MASK_ATOMIC_BLOBS) ? "ROW_FORMAT=DYNAMIC"
         : (m_flags & DICT_TF_MASK_COMPACT)    ? "ROW_FORMAT=COMPACT"
                                               : "ROW_FORMAT=REDUNDANT";
  }

  ib_errf(thd, IB_LOG_LEVEL_ERROR, ER_TABLE_SCHEMA_MISMATCH,
          "Table flags don't match, server table has 0x%x and the meta-data "
          "file has 0x%zx; .cfg file uses %s",
          m_table->flags, m_flags, msg);

  return DB_ERROR;
}

/** Check if the table (and index) schema that was read from the .cfg file
matches the in memory table definition.
@param thd MySQL session variable
@return DB_SUCCESS or error code. */
dberr_t
row_import::match_schema(
	THD*		thd) UNIV_NOTHROW
{
	/* Do some simple checks. */

	if (UT_LIST_GET_LEN(m_table->indexes) != m_n_indexes) {

		/* If the number of indexes don't match then it is better
		to abort the IMPORT. It is easy for the user to create a
		table matching the IMPORT definition. */

		ib_errf(thd, IB_LOG_LEVEL_ERROR, ER_TABLE_SCHEMA_MISMATCH,
			"Number of indexes don't match, table has " ULINTPF
			" indexes but the tablespace meta-data file has "
			ULINTPF " indexes",
			UT_LIST_GET_LEN(m_table->indexes), m_n_indexes);

		return(DB_ERROR);
	}

	dberr_t	err = match_table_columns(thd);

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* Check if the index definitions match. */

	const dict_index_t* index;

	for (index = UT_LIST_GET_FIRST(m_table->indexes);
	     index != 0;
	     index = UT_LIST_GET_NEXT(indexes, index)) {

		dberr_t	index_err;

		index_err = match_index_columns(thd, index);

		if (index_err != DB_SUCCESS) {
			err = index_err;
		}
	}

	return(err);
}

/**
Set the index root <space, pageno>, using index name. */
void
row_import::set_root_by_name() UNIV_NOTHROW
{
	row_index_t*	cfg_index = m_indexes;

	for (ulint i = 0; i < m_n_indexes; ++i, ++cfg_index) {
		dict_index_t*	index;

		const char*	index_name;

		index_name = reinterpret_cast<const char*>(cfg_index->m_name);

		index = dict_table_get_index_on_name(m_table, index_name);

		/* We've already checked that it exists. */
		ut_a(index != 0);

		index->page = cfg_index->m_page_no;
	}
}

/**
Set the index root <space, pageno>, using a heuristic.
@return DB_SUCCESS or error code */
dberr_t
row_import::set_root_by_heuristic() UNIV_NOTHROW
{
	row_index_t*	cfg_index = m_indexes;

	ut_a(m_n_indexes > 0);

	// TODO: For now use brute force, based on ordinality

	if (UT_LIST_GET_LEN(m_table->indexes) != m_n_indexes) {

		ib::warn() << "Table " << m_table->name << " should have "
			<< UT_LIST_GET_LEN(m_table->indexes) << " indexes but"
			" the tablespace has " << m_n_indexes << " indexes";
	}

	ulint	i = 0;
	dberr_t	err = DB_SUCCESS;

	for (dict_index_t* index = UT_LIST_GET_FIRST(m_table->indexes);
	     index != 0;
	     index = UT_LIST_GET_NEXT(indexes, index)) {

		if (index->type & DICT_FTS) {
			index->type |= DICT_CORRUPT;
			ib::warn() << "Skipping FTS index: " << index->name;
		} else if (i < m_n_indexes) {

			UT_DELETE_ARRAY(cfg_index[i].m_name);

			ulint	len = strlen(index->name) + 1;

			cfg_index[i].m_name = UT_NEW_ARRAY_NOKEY(byte, len);

			/* Trigger OOM */
			DBUG_EXECUTE_IF(
				"ib_import_OOM_14",
				UT_DELETE_ARRAY(cfg_index[i].m_name);
				cfg_index[i].m_name = NULL;
			);

			if (cfg_index[i].m_name == NULL) {
				err = DB_OUT_OF_MEMORY;
				break;
			}

			memcpy(cfg_index[i].m_name, index->name, len);

			cfg_index[i].m_srv_index = index;

			index->page = cfg_index[i++].m_page_no;
		}
	}

	return(err);
}

/**
Purge delete marked records.
@return DB_SUCCESS or error code. */
dberr_t
IndexPurge::garbage_collect() UNIV_NOTHROW
{
	dberr_t	err;
	ibool	comp = dict_table_is_comp(m_index->table);

	/* Open the persistent cursor and start the mini-transaction. */

	open();

	while ((err = next()) == DB_SUCCESS) {

		rec_t*	rec = btr_pcur_get_rec(&m_pcur);
		ibool	deleted = rec_get_deleted_flag(rec, comp);

		if (!deleted) {
			++m_n_rows;
		} else {
			purge();
		}
	}

	/* Close the persistent cursor and commit the mini-transaction. */

	close();

	return(err == DB_END_OF_INDEX ? DB_SUCCESS : err);
}

/**
Begin import, position the cursor on the first record. */
void
IndexPurge::open() UNIV_NOTHROW
{
	mtr_start(&m_mtr);

	mtr_set_log_mode(&m_mtr, MTR_LOG_NO_REDO);

	btr_pcur_open_at_index_side(
		true, m_index, BTR_MODIFY_LEAF, &m_pcur, true, 0, &m_mtr);
	btr_pcur_move_to_next_user_rec(&m_pcur, &m_mtr);
	if (rec_is_metadata(btr_pcur_get_rec(&m_pcur), *m_index)) {
		ut_ad(btr_pcur_is_on_user_rec(&m_pcur));
		/* Skip the metadata pseudo-record. */
	} else {
		btr_pcur_move_to_prev_on_page(&m_pcur);
	}
}

/**
Close the persistent curosr and commit the mini-transaction. */
void
IndexPurge::close() UNIV_NOTHROW
{
	btr_pcur_close(&m_pcur);
	mtr_commit(&m_mtr);
}

/**
Position the cursor on the next record.
@return DB_SUCCESS or error code */
dberr_t
IndexPurge::next() UNIV_NOTHROW
{
	btr_pcur_move_to_next_on_page(&m_pcur);

	/* When switching pages, commit the mini-transaction
	in order to release the latch on the old page. */

	if (!btr_pcur_is_after_last_on_page(&m_pcur)) {
		return(DB_SUCCESS);
	} else if (trx_is_interrupted(m_trx)) {
		/* Check after every page because the check
		is expensive. */
		return(DB_INTERRUPTED);
	}

	btr_pcur_store_position(&m_pcur, &m_mtr);

	mtr_commit(&m_mtr);

	mtr_start(&m_mtr);

	mtr_set_log_mode(&m_mtr, MTR_LOG_NO_REDO);

	m_pcur.restore_position(BTR_MODIFY_LEAF, &m_mtr);
	/* The following is based on btr_pcur_move_to_next_user_rec(). */
	m_pcur.old_stored = false;
	ut_ad(m_pcur.latch_mode == BTR_MODIFY_LEAF);
	do {
		if (btr_pcur_is_after_last_on_page(&m_pcur)) {
			if (btr_pcur_is_after_last_in_tree(&m_pcur)) {
				return DB_END_OF_INDEX;
			}

			buf_block_t* block = btr_pcur_get_block(&m_pcur);
			uint32_t next_page = btr_page_get_next(
				block->page.frame);

			/* MDEV-13542 FIXME: Make these checks part of
			btr_pcur_move_to_next_page(), and introduce a
			return status that will be checked in all callers! */
			switch (next_page) {
			default:
				if (next_page != block->page.id().page_no()) {
					break;
				}
				/* MDEV-20931 FIXME: Check that
				next_page is within the tablespace
				bounds! Also check that it is not a
				change buffer bitmap page. */
				/* fall through */
			case 0:
			case 1:
			case FIL_NULL:
				return DB_CORRUPTION;
			}

			dict_index_t* index = m_pcur.btr_cur.index;
			buf_block_t* next_block = btr_block_get(
				*index, next_page, BTR_MODIFY_LEAF, false,
				&m_mtr);

			if (UNIV_UNLIKELY(!next_block
					  || !fil_page_index_page_check(
						  next_block->page.frame)
					  || !!dict_index_is_spatial(index)
					  != (fil_page_get_type(
						      next_block->page.frame)
					      == FIL_PAGE_RTREE)
					  || page_is_comp(next_block->page.frame)
					  != page_is_comp(block->page.frame)
					  || btr_page_get_prev(
						  next_block->page.frame)
					  != block->page.id().page_no())) {
				return DB_CORRUPTION;
			}

			btr_leaf_page_release(block, BTR_MODIFY_LEAF, &m_mtr);

			page_cur_set_before_first(next_block,
						  &m_pcur.btr_cur.page_cur);

			ut_d(page_check_dir(next_block->page.frame));
		} else {
			btr_pcur_move_to_next_on_page(&m_pcur);
		}
	} while (!btr_pcur_is_on_user_rec(&m_pcur));

	return DB_SUCCESS;
}

/**
Store the persistent cursor position and reopen the
B-tree cursor in BTR_MODIFY_TREE mode, because the
tree structure may be changed during a pessimistic delete. */
void
IndexPurge::purge_pessimistic_delete() UNIV_NOTHROW
{
	dberr_t	err;

	m_pcur.restore_position(BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE, &m_mtr);

	ut_ad(rec_get_deleted_flag(
			btr_pcur_get_rec(&m_pcur),
			dict_table_is_comp(m_index->table)));

	btr_cur_pessimistic_delete(
		&err, FALSE, btr_pcur_get_btr_cur(&m_pcur), 0, false, &m_mtr);

	ut_a(err == DB_SUCCESS);

	/* Reopen the B-tree cursor in BTR_MODIFY_LEAF mode */
	mtr_commit(&m_mtr);
}

/**
Purge delete-marked records. */
void
IndexPurge::purge() UNIV_NOTHROW
{
	btr_pcur_store_position(&m_pcur, &m_mtr);

	purge_pessimistic_delete();

	mtr_start(&m_mtr);

	mtr_set_log_mode(&m_mtr, MTR_LOG_NO_REDO);

	m_pcur.restore_position(BTR_MODIFY_LEAF, &m_mtr);
}

/** Adjust the BLOB reference for a single column that is externally stored
@param rec record to update
@param offsets column offsets for the record
@param i column ordinal value
@return DB_SUCCESS or error code */
inline
dberr_t
PageConverter::adjust_cluster_index_blob_column(
	rec_t*		rec,
	const rec_offs*	offsets,
	ulint		i) UNIV_NOTHROW
{
	ulint		len;
	byte*		field;

	field = rec_get_nth_field(rec, offsets, i, &len);

	DBUG_EXECUTE_IF("ib_import_trigger_corruption_2",
			len = BTR_EXTERN_FIELD_REF_SIZE - 1;);

	if (len < BTR_EXTERN_FIELD_REF_SIZE) {

		ib_errf(m_trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_INNODB_INDEX_CORRUPT,
			"Externally stored column(" ULINTPF
			") has a reference length of " ULINTPF
			" in the cluster index %s",
			i, len, m_cluster_index->name());

		return(DB_CORRUPTION);
	}

	field += len - (BTR_EXTERN_FIELD_REF_SIZE - BTR_EXTERN_SPACE_ID);

	mach_write_to_4(field, get_space_id());

	if (UNIV_LIKELY_NULL(m_rec_iter.current_block()->page.zip.data)) {
		page_zip_write_blob_ptr(
			m_rec_iter.current_block(), rec, m_cluster_index,
			offsets, i, &m_rec_iter.m_mtr);
	}

	return(DB_SUCCESS);
}

/** Adjusts the BLOB reference in the clustered index row for all externally
stored columns.
@param rec record to update
@param offsets column offsets for the record
@return DB_SUCCESS or error code */
inline
dberr_t
PageConverter::adjust_cluster_index_blob_columns(
	rec_t*		rec,
	const rec_offs*	offsets) UNIV_NOTHROW
{
	ut_ad(rec_offs_any_extern(offsets));

	/* Adjust the space_id in the BLOB pointers. */

	for (ulint i = 0; i < rec_offs_n_fields(offsets); ++i) {

		/* Only if the column is stored "externally". */

		if (rec_offs_nth_extern(offsets, i)) {
			dberr_t	err;

			err = adjust_cluster_index_blob_column(rec, offsets, i);

			if (err != DB_SUCCESS) {
				return(err);
			}
		}
	}

	return(DB_SUCCESS);
}

/** In the clustered index, adjust BLOB pointers as needed. Also update the
BLOB reference, write the new space id.
@param rec record to update
@param offsets column offsets for the record
@return DB_SUCCESS or error code */
inline
dberr_t
PageConverter::adjust_cluster_index_blob_ref(
	rec_t*		rec,
	const rec_offs*	offsets) UNIV_NOTHROW
{
	if (rec_offs_any_extern(offsets)) {
		dberr_t	err;

		err = adjust_cluster_index_blob_columns(rec, offsets);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	return(DB_SUCCESS);
}

/** Purge delete-marked records, only if it is possible to do so without
re-organising the B+tree.
@return true if purge succeeded */
inline bool PageConverter::purge() UNIV_NOTHROW
{
	const dict_index_t*	index = m_index->m_srv_index;

	/* We can't have a page that is empty and not root. */
	if (m_rec_iter.remove(index, m_offsets)) {

		++m_index->m_stats.m_n_purged;

		return(true);
	} else {
		++m_index->m_stats.m_n_purge_failed;
	}

	return(false);
}

/** Adjust the BLOB references and sys fields for the current record.
@param rec record to update
@param offsets column offsets for the record
@return DB_SUCCESS or error code. */
inline
dberr_t
PageConverter::adjust_cluster_record(
	rec_t*			rec,
	const rec_offs*		offsets) UNIV_NOTHROW
{
	dberr_t	err;

	if ((err = adjust_cluster_index_blob_ref(rec, offsets)) == DB_SUCCESS) {

		/* Reset DB_TRX_ID and DB_ROLL_PTR.  Normally, these fields
		are only written in conjunction with other changes to the
		record. */
		ulint	trx_id_pos = m_cluster_index->n_uniq
			? m_cluster_index->n_uniq : 1;
		if (UNIV_LIKELY_NULL(m_rec_iter.current_block()
				     ->page.zip.data)) {
			page_zip_write_trx_id_and_roll_ptr(
				m_rec_iter.current_block(),
				rec, m_offsets, trx_id_pos,
				0, roll_ptr_t(1) << ROLL_PTR_INSERT_FLAG_POS,
				&m_rec_iter.m_mtr);
		} else {
			ulint	len;
			byte*	ptr = rec_get_nth_field(
				rec, m_offsets, trx_id_pos, &len);
			ut_ad(len == DATA_TRX_ID_LEN);
			memcpy(ptr, reset_trx_id, sizeof reset_trx_id);
		}
	}

	return(err);
}

/** Update the BLOB refrences and write UNDO log entries for
rows that can't be purged optimistically.
@param block block to update
@retval DB_SUCCESS or error code */
inline
dberr_t
PageConverter::update_records(
	buf_block_t*	block) UNIV_NOTHROW
{
	ibool	comp = dict_table_is_comp(m_cfg->m_table);
	bool	clust_index = m_index->m_srv_index == m_cluster_index;

	/* This will also position the cursor on the first user record. */

	m_rec_iter.open(block);

	while (!m_rec_iter.end()) {
		rec_t*	rec = m_rec_iter.current();
		ibool	deleted = rec_get_deleted_flag(rec, comp);

		/* For the clustered index we have to adjust the BLOB
		reference and the system fields irrespective of the
		delete marked flag. The adjustment of delete marked
		cluster records is required for purge to work later. */

		if (deleted || clust_index) {
			m_offsets = rec_get_offsets(
				rec, m_index->m_srv_index, m_offsets,
				m_index->m_srv_index->n_core_fields,
				ULINT_UNDEFINED, &m_heap);
		}

		if (clust_index) {

			dberr_t err = adjust_cluster_record(rec, m_offsets);

			if (err != DB_SUCCESS) {
				return(err);
			}
		}

		/* If it is a delete marked record then try an
		optimistic delete. */

		if (deleted) {
			/* A successful purge will move the cursor to the
			next record. */

			if (!purge()) {
				m_rec_iter.next();
			}

			++m_index->m_stats.m_n_deleted;
		} else {
			++m_index->m_stats.m_n_rows;
			m_rec_iter.next();
		}
	}

	return(DB_SUCCESS);
}

/** Update the space, index id, trx id.
@return DB_SUCCESS or error code */
inline
dberr_t
PageConverter::update_index_page(
	buf_block_t*	block) UNIV_NOTHROW
{
	const page_id_t page_id(block->page.id());

	if (is_free(page_id.page_no())) {
		return(DB_SUCCESS);
	}

	buf_frame_t* page = block->page.frame;
	const index_id_t id = btr_page_get_index_id(page);

	if (id != m_index->m_id) {
		row_index_t* index = find_index(id);

		if (UNIV_UNLIKELY(!index)) {
			if (!m_cfg->m_missing) {
				ib::warn() << "Unknown index id " << id
					   << " on page " << page_id.page_no();
			}
			return DB_SUCCESS;
		}

		m_index = index;
	}

	/* If the .cfg file is missing and there is an index mismatch
	then ignore the error. */
	if (m_cfg->m_missing && !m_index->m_srv_index) {
		return(DB_SUCCESS);
	}

	if (m_index && page_id.page_no() == m_index->m_page_no) {
		byte *b = FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF + FSEG_HDR_SPACE
			+ page;
		mach_write_to_4(b, page_id.space());

		memcpy(FIL_PAGE_DATA + PAGE_BTR_SEG_TOP + FSEG_HDR_SPACE
		       + page, b, 4);
		if (UNIV_LIKELY_NULL(block->page.zip.data)) {
			memcpy(&block->page.zip.data[FIL_PAGE_DATA
						     + PAGE_BTR_SEG_TOP
						     + FSEG_HDR_SPACE], b, 4);
			memcpy(&block->page.zip.data[FIL_PAGE_DATA
						     + PAGE_BTR_SEG_LEAF
						     + FSEG_HDR_SPACE], b, 4);
		}
	}

#ifdef UNIV_ZIP_DEBUG
	ut_a(!block->page.zip.data || page_zip_validate(&block->page.zip, page,
							m_index->m_srv_index));
#endif /* UNIV_ZIP_DEBUG */

	/* This has to be written to uncompressed index header. Set it to
	the current index id. */
	mach_write_to_8(page + (PAGE_HEADER + PAGE_INDEX_ID),
			m_index->m_srv_index->id);
	if (UNIV_LIKELY_NULL(block->page.zip.data)) {
		memcpy(&block->page.zip.data[PAGE_HEADER + PAGE_INDEX_ID],
		       &block->page.frame[PAGE_HEADER + PAGE_INDEX_ID], 8);
	}

	if (m_index->m_srv_index->is_clust()) {
		if (page_id.page_no() != m_index->m_srv_index->page) {
			goto clear_page_max_trx_id;
		}
	} else if (page_is_leaf(page)) {
		/* Set PAGE_MAX_TRX_ID on secondary index leaf pages. */
		mach_write_to_8(&block->page.frame
				[PAGE_HEADER + PAGE_MAX_TRX_ID], m_trx->id);
		if (UNIV_LIKELY_NULL(block->page.zip.data)) {
			memcpy_aligned<8>(&block->page.zip.data
					  [PAGE_HEADER + PAGE_MAX_TRX_ID],
					  &block->page.frame
					  [PAGE_HEADER + PAGE_MAX_TRX_ID], 8);
		}
	} else {
clear_page_max_trx_id:
		/* Clear PAGE_MAX_TRX_ID so that it can be
		used for other purposes in the future. IMPORT
		in MySQL 5.6, 5.7 and MariaDB 10.0 and 10.1
		would set the field to the transaction ID even
		on clustered index pages. */
		memset_aligned<8>(&block->page.frame
				  [PAGE_HEADER + PAGE_MAX_TRX_ID],
				  0, 8);
		if (UNIV_LIKELY_NULL(block->page.zip.data)) {
			memset_aligned<8>(&block->page.zip.data
					  [PAGE_HEADER + PAGE_MAX_TRX_ID],
					  0, 8);
		}
	}

	if (page_is_empty(page)) {

		/* Only a root page can be empty. */
		if (page_has_siblings(page)) {
			// TODO: We should relax this and skip secondary
			// indexes. Mark them as corrupt because they can
			// always be rebuilt.
			return(DB_CORRUPTION);
		}

		return(DB_SUCCESS);
	}

	return page_is_leaf(block->page.frame)
		? update_records(block)
		: DB_SUCCESS;
}

/** Validate the space flags and update tablespace header page.
@param block block read from file, not from the buffer pool.
@retval DB_SUCCESS or error code */
inline dberr_t PageConverter::update_header(buf_block_t* block) UNIV_NOTHROW
{
  byte *frame= get_frame(block);
  if (memcmp_aligned<2>(FIL_PAGE_SPACE_ID + frame,
                        FSP_HEADER_OFFSET + FSP_SPACE_ID + frame, 4))
    ib::warn() << "Space id check in the header failed: ignored";
  else if (!mach_read_from_4(FIL_PAGE_SPACE_ID + frame))
    return DB_CORRUPTION;

  memset(frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);

  /* Write space_id to the tablespace header, page 0. */
  mach_write_to_4(FIL_PAGE_SPACE_ID + frame, get_space_id());
  memcpy_aligned<2>(FSP_HEADER_OFFSET + FSP_SPACE_ID + frame,
                    FIL_PAGE_SPACE_ID + frame, 4);
  /* Write back the adjusted flags. */
  mach_write_to_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + frame, m_space_flags);

  return DB_SUCCESS;
}

/** Update the page, set the space id, max trx id and index id.
@param block block read from file
@retval DB_SUCCESS or error code */
inline
dberr_t
PageConverter::update_page(buf_block_t* block, uint16_t& page_type)
	UNIV_NOTHROW
{
	dberr_t		err = DB_SUCCESS;

	ut_ad(!block->page.zip.data == !is_compressed_table());

	switch (page_type = fil_page_get_type(get_frame(block))) {
	case FIL_PAGE_TYPE_FSP_HDR:
		ut_a(block->page.id().page_no() == 0);
		/* Work directly on the uncompressed page headers. */
		return(update_header(block));

	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		/* We need to decompress the contents
		before we can do anything. */

		if (is_compressed_table() && !buf_zip_decompress(block, TRUE)) {
			return(DB_CORRUPTION);
		}

		/* fall through */
	case FIL_PAGE_TYPE_INSTANT:
		/* This is on every page in the tablespace. */
		mach_write_to_4(
			get_frame(block)
			+ FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, get_space_id());

		/* Only update the Btree nodes. */
		return(update_index_page(block));

	case FIL_PAGE_TYPE_SYS:
		/* This is page 0 in the system tablespace. */
		return(DB_CORRUPTION);

	case FIL_PAGE_TYPE_XDES:
		err = set_current_xdes(
			block->page.id().page_no(), get_frame(block));
		/* fall through */
	case FIL_PAGE_INODE:
	case FIL_PAGE_TYPE_TRX_SYS:
	case FIL_PAGE_IBUF_FREE_LIST:
	case FIL_PAGE_TYPE_ALLOCATED:
	case FIL_PAGE_IBUF_BITMAP:
	case FIL_PAGE_TYPE_BLOB:
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:

		/* Work directly on the uncompressed page headers. */
		/* This is on every page in the tablespace. */
		mach_write_to_4(
			get_frame(block)
			+ FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, get_space_id());

		return(err);
	}

	ib::warn() << "Unknown page type (" << page_type << ")";

	return(DB_CORRUPTION);
}

/** Called for every page in the tablespace. If the page was not
updated then its state must be set to BUF_PAGE_NOT_USED.
@param block block read from file, note it is not from the buffer pool
@retval DB_SUCCESS or error code. */
dberr_t PageConverter::operator()(buf_block_t* block) UNIV_NOTHROW
{
	/* If we already had an old page with matching number
	in the buffer pool, evict it now, because
	we no longer evict the pages on DISCARD TABLESPACE. */
	buf_page_get_gen(block->page.id(), get_zip_size(),
			 RW_NO_LATCH, NULL, BUF_EVICT_IF_IN_POOL, NULL);

	uint16_t page_type;

	if (dberr_t err = update_page(block, page_type)) {
		return err;
	}

	const bool full_crc32 = fil_space_t::full_crc32(get_space_flags());
	byte* frame = get_frame(block);
	memset_aligned<8>(frame + FIL_PAGE_LSN, 0, 8);

	if (!block->page.zip.data) {
		buf_flush_init_for_writing(
			NULL, block->page.frame, NULL, full_crc32);
	} else if (fil_page_type_is_index(page_type)) {
		buf_flush_init_for_writing(
			NULL, block->page.zip.data, &block->page.zip,
			full_crc32);
	} else {
		/* Calculate and update the checksum of non-index
		pages for ROW_FORMAT=COMPRESSED tables. */
		buf_flush_update_zip_checksum(
			block->page.zip.data, block->zip_size());
	}

	return DB_SUCCESS;
}

/*****************************************************************//**
Clean up after import tablespace. */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_cleanup(
/*===============*/
	row_prebuilt_t*	prebuilt,	/*!< in/out: prebuilt from handler */
	dberr_t		err)		/*!< in: error code */
{
	if (err != DB_SUCCESS) {
		dict_table_t* table = prebuilt->table;
		table->file_unreadable = true;
		if (table->space) {
			fil_close_tablespace(table->space_id);
			table->space = NULL;
		}

		prebuilt->trx->error_info = NULL;

		ib::info() << "Discarding tablespace of table "
			   << table->name << ": " << err;

		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index;
		     index = UT_LIST_GET_NEXT(indexes, index)) {
			index->page = FIL_NULL;
		}
	}

	DBUG_EXECUTE_IF("ib_import_before_commit_crash", DBUG_SUICIDE(););

	prebuilt->trx->commit();

	if (prebuilt->trx->dict_operation_lock_mode) {
		row_mysql_unlock_data_dictionary(prebuilt->trx);
	}

	prebuilt->trx->op_info = "";

	DBUG_EXECUTE_IF("ib_import_before_checkpoint_crash", DBUG_SUICIDE(););

	return(err);
}

/*****************************************************************//**
Report error during tablespace import. */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_error(
/*=============*/
	row_prebuilt_t*	prebuilt,	/*!< in/out: prebuilt from handler */
	dberr_t		err)		/*!< in: error code */
{
	if (!trx_is_interrupted(prebuilt->trx)) {
		char	table_name[MAX_FULL_NAME_LEN + 1];

		innobase_format_name(
			table_name, sizeof(table_name),
			prebuilt->table->name.m_name);

		ib_senderrf(
			prebuilt->trx->mysql_thd, IB_LOG_LEVEL_WARN,
			ER_INNODB_IMPORT_ERROR,
			table_name, (ulong) err, ut_strerr(err));
	}

	return row_import_cleanup(prebuilt, err);
}

/*****************************************************************//**
Adjust the root page index node and leaf node segment headers, update
with the new space id. For all the table's secondary indexes.
@return error code */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_adjust_root_pages_of_secondary_indexes(
/*==============================================*/
	trx_t*			trx,		/*!< in: transaction used for
						the import */
	dict_table_t*		table,		/*!< in: table the indexes
						belong to */
	const row_import&	cfg)		/*!< Import context */
{
	dict_index_t*		index;
	ulint			n_rows_in_table;
	dberr_t			err = DB_SUCCESS;

	/* Skip the clustered index. */
	index = dict_table_get_first_index(table);

	n_rows_in_table = cfg.get_n_rows(index->name);

	DBUG_EXECUTE_IF("ib_import_sec_rec_count_mismatch_failure",
			n_rows_in_table++;);

	/* Adjust the root pages of the secondary indexes only. */
	while ((index = dict_table_get_next_index(index)) != NULL) {
		ut_a(!dict_index_is_clust(index));

		if (!(index->type & DICT_CORRUPT)
		    && index->page != FIL_NULL) {

			/* Update the Btree segment headers for index node and
			leaf nodes in the root page. Set the new space id. */

			err = btr_root_adjust_on_import(index);
		} else {
			ib::warn() << "Skip adjustment of root pages for"
				" index " << index->name << ".";

			err = DB_CORRUPTION;
		}

		if (err != DB_SUCCESS) {

			if (index->type & DICT_CLUSTERED) {
				break;
			}

			ib_errf(trx->mysql_thd,
				IB_LOG_LEVEL_WARN,
				ER_INNODB_INDEX_CORRUPT,
				"Index %s not found or corrupt,"
				" you should recreate this index.",
				index->name());

			/* Do not bail out, so that the data
			can be recovered. */

			err = DB_SUCCESS;
			index->type |= DICT_CORRUPT;
			continue;
		}

		/* If we failed to purge any records in the index then
		do it the hard way.

		TODO: We can do this in the first pass by generating UNDO log
		records for the failed rows. */

		if (!cfg.requires_purge(index->name)) {
			continue;
		}

		IndexPurge   purge(trx, index);

		trx->op_info = "secondary: purge delete marked records";

		err = purge.garbage_collect();

		trx->op_info = "";

		if (err != DB_SUCCESS) {
			break;
		} else if (purge.get_n_rows() != n_rows_in_table) {

			ib_errf(trx->mysql_thd,
				IB_LOG_LEVEL_WARN,
				ER_INNODB_INDEX_CORRUPT,
				"Index '%s' contains " ULINTPF " entries, "
				"should be " ULINTPF ", you should recreate "
				"this index.", index->name(),
				purge.get_n_rows(), n_rows_in_table);

			index->type |= DICT_CORRUPT;

			/* Do not bail out, so that the data
			can be recovered. */

			err = DB_SUCCESS;
                }
	}

	return(err);
}

/*****************************************************************//**
Ensure that dict_sys.row_id exceeds SELECT MAX(DB_ROW_ID). */
MY_ATTRIBUTE((nonnull)) static
void
row_import_set_sys_max_row_id(
/*==========================*/
	row_prebuilt_t*		prebuilt,	/*!< in/out: prebuilt from
						handler */
	const dict_table_t*	table)		/*!< in: table to import */
{
	const rec_t*		rec;
	mtr_t			mtr;
	btr_pcur_t		pcur;
	row_id_t		row_id	= 0;
	dict_index_t*		index;

	index = dict_table_get_first_index(table);
	ut_ad(index->is_primary());
	ut_ad(dict_index_is_auto_gen_clust(index));

	mtr_start(&mtr);

	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	btr_pcur_open_at_index_side(
		false,		// High end
		index,
		BTR_SEARCH_LEAF,
		&pcur,
		true,		// Init cursor
		0,		// Leaf level
		&mtr);

	btr_pcur_move_to_prev_on_page(&pcur);
	rec = btr_pcur_get_rec(&pcur);

	/* Check for empty table. */
	if (page_rec_is_infimum(rec)) {
		/* The table is empty. */
	} else if (rec_is_metadata(rec, *index)) {
		/* The clustered index contains the metadata record only,
		that is, the table is empty. */
	} else {
		row_id = mach_read_from_6(rec);
	}

	mtr_commit(&mtr);

	if (row_id) {
		/* Update the system row id if the imported index row id is
		greater than the max system row id. */
		dict_sys.update_row_id(row_id);
	}
}

/*****************************************************************//**
Read the a string from the meta data file.
@return DB_SUCCESS or error code. */
static
dberr_t
row_import_cfg_read_string(
/*=======================*/
	FILE*		file,		/*!< in/out: File to read from */
	byte*		ptr,		/*!< out: string to read */
	ulint		max_len)	/*!< in: maximum length of the output
					buffer in bytes */
{
	DBUG_EXECUTE_IF("ib_import_string_read_error",
			errno = EINVAL; return(DB_IO_ERROR););

	ulint		len = 0;

	while (!feof(file)) {
		int	ch = fgetc(file);

		if (ch == EOF) {
			break;
		} else if (ch != 0) {
			if (len < max_len) {
				ptr[len++] = static_cast<byte>(ch);
			} else {
				break;
			}
		/* max_len includes the NUL byte */
		} else if (len != max_len - 1) {
			break;
		} else {
			ptr[len] = 0;
			return(DB_SUCCESS);
		}
	}

	errno = EINVAL;

	return(DB_IO_ERROR);
}

/*********************************************************************//**
Write the meta data (index user fields) config file.
@return DB_SUCCESS or error code. */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_cfg_read_index_fields(
/*=============================*/
	FILE*			file,	/*!< in: file to write to */
	THD*			thd,	/*!< in/out: session */
	row_index_t*		index)	/*!< Index being read in */
{
	byte			row[sizeof(ib_uint32_t) * 3];
	ulint			n_fields = index->m_n_fields;

	index->m_fields = UT_NEW_ARRAY_NOKEY(dict_field_t, n_fields);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_4",
		UT_DELETE_ARRAY(index->m_fields);
		index->m_fields = NULL;
	);

	if (index->m_fields == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	dict_field_t*	field = index->m_fields;

	for (ulint i = 0; i < n_fields; ++i, ++field) {
		byte*		ptr = row;

		/* Trigger EOF */
		DBUG_EXECUTE_IF("ib_import_io_read_error_1",
				(void) fseek(file, 0L, SEEK_END););

		if (fread(row, 1, sizeof(row), file) != sizeof(row)) {

			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
				(ulong) errno, strerror(errno),
				"while reading index fields.");

			return(DB_IO_ERROR);
		}

		new (field) dict_field_t();

		field->prefix_len = mach_read_from_4(ptr) & ((1U << 12) - 1);
		ptr += sizeof(ib_uint32_t);

		field->fixed_len = mach_read_from_4(ptr) & ((1U << 10) - 1);
		ptr += sizeof(ib_uint32_t);

		/* Include the NUL byte in the length. */
		ulint	len = mach_read_from_4(ptr);

		byte*	name = UT_NEW_ARRAY_NOKEY(byte, len);

		/* Trigger OOM */
		DBUG_EXECUTE_IF(
			"ib_import_OOM_5",
			UT_DELETE_ARRAY(name);
			name = NULL;
		);

		if (name == NULL) {
			return(DB_OUT_OF_MEMORY);
		}

		field->name = reinterpret_cast<const char*>(name);

		dberr_t	err = row_import_cfg_read_string(file, name, len);

		if (err != DB_SUCCESS) {

			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
				(ulong) errno, strerror(errno),
				"while parsing table name.");

			return(err);
		}
	}

	return(DB_SUCCESS);
}

/*****************************************************************//**
Read the index names and root page numbers of the indexes and set the values.
Row format [root_page_no, len of str, str ... ]
@return DB_SUCCESS or error code. */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_read_index_data(
/*=======================*/
	FILE*		file,		/*!< in: File to read from */
	THD*		thd,		/*!< in: session */
	row_import*	cfg)		/*!< in/out: meta-data read */
{
	byte*		ptr;
	row_index_t*	cfg_index;
	byte		row[sizeof(index_id_t) + sizeof(ib_uint32_t) * 9];

	/* FIXME: What is the max value? */
	ut_a(cfg->m_n_indexes > 0);
	ut_a(cfg->m_n_indexes < 1024);

	cfg->m_indexes = UT_NEW_ARRAY_NOKEY(row_index_t, cfg->m_n_indexes);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_6",
		UT_DELETE_ARRAY(cfg->m_indexes);
		cfg->m_indexes = NULL;
	);

	if (cfg->m_indexes == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	memset(cfg->m_indexes, 0x0, sizeof(*cfg->m_indexes) * cfg->m_n_indexes);

	cfg_index = cfg->m_indexes;

	for (ulint i = 0; i < cfg->m_n_indexes; ++i, ++cfg_index) {
		/* Trigger EOF */
		DBUG_EXECUTE_IF("ib_import_io_read_error_2",
				(void) fseek(file, 0L, SEEK_END););

		/* Read the index data. */
		size_t	n_bytes = fread(row, 1, sizeof(row), file);

		/* Trigger EOF */
		DBUG_EXECUTE_IF("ib_import_io_read_error",
				(void) fseek(file, 0L, SEEK_END););

		if (n_bytes != sizeof(row)) {
			char	msg[BUFSIZ];

			snprintf(msg, sizeof(msg),
				 "while reading index meta-data, expected "
				 "to read " ULINTPF
				 " bytes but read only " ULINTPF " bytes",
				 sizeof(row), n_bytes);

			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
				(ulong) errno, strerror(errno), msg);

			ib::error() << "IO Error: " << msg;

			return(DB_IO_ERROR);
		}

		ptr = row;

		cfg_index->m_id = mach_read_from_8(ptr);
		ptr += sizeof(index_id_t);

		cfg_index->m_space = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		cfg_index->m_page_no = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		cfg_index->m_type = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		cfg_index->m_trx_id_offset = mach_read_from_4(ptr);
		if (cfg_index->m_trx_id_offset != mach_read_from_4(ptr)) {
			ut_ad(0);
			/* Overflow. Pretend that the clustered index
			has a variable-length PRIMARY KEY. */
			cfg_index->m_trx_id_offset = 0;
		}
		ptr += sizeof(ib_uint32_t);

		cfg_index->m_n_user_defined_cols = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		cfg_index->m_n_uniq = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		cfg_index->m_n_nullable = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		cfg_index->m_n_fields = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		/* The NUL byte is included in the name length. */
		ulint	len = mach_read_from_4(ptr);

		if (len > OS_FILE_MAX_PATH) {
			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_INNODB_INDEX_CORRUPT,
				"Index name length (" ULINTPF ") is too long, "
				"the meta-data is corrupt", len);

			return(DB_CORRUPTION);
		}

		cfg_index->m_name = UT_NEW_ARRAY_NOKEY(byte, len);

		/* Trigger OOM */
		DBUG_EXECUTE_IF(
			"ib_import_OOM_7",
			UT_DELETE_ARRAY(cfg_index->m_name);
			cfg_index->m_name = NULL;
		);

		if (cfg_index->m_name == NULL) {
			return(DB_OUT_OF_MEMORY);
		}

		dberr_t	err;

		err = row_import_cfg_read_string(file, cfg_index->m_name, len);

		if (err != DB_SUCCESS) {

			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
				(ulong) errno, strerror(errno),
				"while parsing index name.");

			return(err);
		}

		err = row_import_cfg_read_index_fields(file, thd, cfg_index);

		if (err != DB_SUCCESS) {
			return(err);
		}

	}

	return(DB_SUCCESS);
}

/*****************************************************************//**
Set the index root page number for v1 format.
@return DB_SUCCESS or error code. */
static
dberr_t
row_import_read_indexes(
/*====================*/
	FILE*		file,		/*!< in: File to read from */
	THD*		thd,		/*!< in: session */
	row_import*	cfg)		/*!< in/out: meta-data read */
{
	byte		row[sizeof(ib_uint32_t)];

	/* Trigger EOF */
	DBUG_EXECUTE_IF("ib_import_io_read_error_3",
			(void) fseek(file, 0L, SEEK_END););

	/* Read the number of indexes. */
	if (fread(row, 1, sizeof(row), file) != sizeof(row)) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while reading number of indexes.");

		return(DB_IO_ERROR);
	}

	cfg->m_n_indexes = mach_read_from_4(row);

	if (cfg->m_n_indexes == 0) {
		ib_errf(thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			"Number of indexes in meta-data file is 0");

		return(DB_CORRUPTION);

	} else if (cfg->m_n_indexes > 1024) {
		// FIXME: What is the upper limit? */
		ib_errf(thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			"Number of indexes in meta-data file is too high: "
			ULINTPF, cfg->m_n_indexes);
		cfg->m_n_indexes = 0;

		return(DB_CORRUPTION);
	}

	return(row_import_read_index_data(file, thd, cfg));
}

/*********************************************************************//**
Read the meta data (table columns) config file. Deserialise the contents of
dict_col_t structure, along with the column name. */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_read_columns(
/*====================*/
	FILE*			file,	/*!< in: file to write to */
	THD*			thd,	/*!< in/out: session */
	row_import*		cfg)	/*!< in/out: meta-data read */
{
	dict_col_t*		col;
	byte			row[sizeof(ib_uint32_t) * 8];

	/* FIXME: What should the upper limit be? */
	ut_a(cfg->m_n_cols > 0);
	ut_a(cfg->m_n_cols < 1024);

	cfg->m_cols = UT_NEW_ARRAY_NOKEY(dict_col_t, cfg->m_n_cols);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_8",
		UT_DELETE_ARRAY(cfg->m_cols);
		cfg->m_cols = NULL;
	);

	if (cfg->m_cols == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	cfg->m_col_names = UT_NEW_ARRAY_NOKEY(byte*, cfg->m_n_cols);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_9",
		UT_DELETE_ARRAY(cfg->m_col_names);
		cfg->m_col_names = NULL;
	);

	if (cfg->m_col_names == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	memset(cfg->m_cols, 0x0, sizeof(cfg->m_cols) * cfg->m_n_cols);
	memset(cfg->m_col_names, 0x0, sizeof(cfg->m_col_names) * cfg->m_n_cols);

	col = cfg->m_cols;

	for (ulint i = 0; i < cfg->m_n_cols; ++i, ++col) {
		byte*		ptr = row;

		/* Trigger EOF */
		DBUG_EXECUTE_IF("ib_import_io_read_error_4",
				(void) fseek(file, 0L, SEEK_END););

		if (fread(row, 1,  sizeof(row), file) != sizeof(row)) {
			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
				(ulong) errno, strerror(errno),
				"while reading table column meta-data.");

			return(DB_IO_ERROR);
		}

		col->prtype = mach_read_from_4(ptr);
		ptr += sizeof(ib_uint32_t);

		col->mtype = static_cast<byte>(mach_read_from_4(ptr));
		ptr += sizeof(ib_uint32_t);

		col->len = static_cast<uint16_t>(mach_read_from_4(ptr));
		ptr += sizeof(ib_uint32_t);

		uint32_t mbminmaxlen = mach_read_from_4(ptr);
		col->mbmaxlen = (mbminmaxlen / 5) & 7;
		col->mbminlen = (mbminmaxlen % 5) & 7;
		ptr += sizeof(ib_uint32_t);

		col->ind = mach_read_from_4(ptr) & dict_index_t::MAX_N_FIELDS;
		ptr += sizeof(ib_uint32_t);

		col->ord_part = mach_read_from_4(ptr) & 1;
		ptr += sizeof(ib_uint32_t);

		col->max_prefix = mach_read_from_4(ptr) & ((1U << 12) - 1);
		ptr += sizeof(ib_uint32_t);

		/* Read in the column name as [len, byte array]. The len
		includes the NUL byte. */

		ulint		len = mach_read_from_4(ptr);

		/* FIXME: What is the maximum column name length? */
		if (len == 0 || len > 128) {
			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_IO_READ_ERROR,
				"Column name length " ULINTPF ", is invalid",
				len);

			return(DB_CORRUPTION);
		}

		cfg->m_col_names[i] = UT_NEW_ARRAY_NOKEY(byte, len);

		/* Trigger OOM */
		DBUG_EXECUTE_IF(
			"ib_import_OOM_10",
			UT_DELETE_ARRAY(cfg->m_col_names[i]);
			cfg->m_col_names[i] = NULL;
		);

		if (cfg->m_col_names[i] == NULL) {
			return(DB_OUT_OF_MEMORY);
		}

		dberr_t	err;

		err = row_import_cfg_read_string(
			file, cfg->m_col_names[i], len);

		if (err != DB_SUCCESS) {

			ib_senderrf(
				thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
				(ulong) errno, strerror(errno),
				"while parsing table column name.");

			return(err);
		}
	}

	return(DB_SUCCESS);
}

/*****************************************************************//**
Read the contents of the <tablespace>.cfg file.
@return DB_SUCCESS or error code. */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_read_v1(
/*===============*/
	FILE*		file,		/*!< in: File to read from */
	THD*		thd,		/*!< in: session */
	row_import*	cfg)		/*!< out: meta data */
{
	byte		value[sizeof(ib_uint32_t)];

	/* Trigger EOF */
	DBUG_EXECUTE_IF("ib_import_io_read_error_5",
			(void) fseek(file, 0L, SEEK_END););

	/* Read the hostname where the tablespace was exported. */
	if (fread(value, 1, sizeof(value), file) != sizeof(value)) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while reading meta-data export hostname length.");

		return(DB_IO_ERROR);
	}

	ulint	len = mach_read_from_4(value);

	/* NUL byte is part of name length. */
	cfg->m_hostname = UT_NEW_ARRAY_NOKEY(byte, len);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_1",
		UT_DELETE_ARRAY(cfg->m_hostname);
		cfg->m_hostname = NULL;
	);

	if (cfg->m_hostname == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	dberr_t	err = row_import_cfg_read_string(file, cfg->m_hostname, len);

	if (err != DB_SUCCESS) {

		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while parsing export hostname.");

		return(err);
	}

	/* Trigger EOF */
	DBUG_EXECUTE_IF("ib_import_io_read_error_6",
			(void) fseek(file, 0L, SEEK_END););

	/* Read the table name of tablespace that was exported. */
	if (fread(value, 1, sizeof(value), file) != sizeof(value)) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while reading meta-data table name length.");

		return(DB_IO_ERROR);
	}

	len = mach_read_from_4(value);

	/* NUL byte is part of name length. */
	cfg->m_table_name = UT_NEW_ARRAY_NOKEY(byte, len);

	/* Trigger OOM */
	DBUG_EXECUTE_IF(
		"ib_import_OOM_2",
		UT_DELETE_ARRAY(cfg->m_table_name);
		cfg->m_table_name = NULL;
	);

	if (cfg->m_table_name == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	err = row_import_cfg_read_string(file, cfg->m_table_name, len);

	if (err != DB_SUCCESS) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while parsing table name.");

		return(err);
	}

	ib::info() << "Importing tablespace for table '" << cfg->m_table_name
		<< "' that was exported from host '" << cfg->m_hostname << "'";

	byte		row[sizeof(ib_uint32_t) * 3];

	/* Trigger EOF */
	DBUG_EXECUTE_IF("ib_import_io_read_error_7",
			(void) fseek(file, 0L, SEEK_END););

	/* Read the autoinc value. */
	if (fread(row, 1, sizeof(ib_uint64_t), file) != sizeof(ib_uint64_t)) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while reading autoinc value.");

		return(DB_IO_ERROR);
	}

	cfg->m_autoinc = mach_read_from_8(row);

	/* Trigger EOF */
	DBUG_EXECUTE_IF("ib_import_io_read_error_8",
			(void) fseek(file, 0L, SEEK_END););

	/* Read the tablespace page size. */
	if (fread(row, 1, sizeof(row), file) != sizeof(row)) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while reading meta-data header.");

		return(DB_IO_ERROR);
	}

	byte*		ptr = row;

	const ulint	logical_page_size = mach_read_from_4(ptr);
	ptr += sizeof(ib_uint32_t);

	if (logical_page_size != srv_page_size) {

		ib_errf(thd, IB_LOG_LEVEL_ERROR, ER_TABLE_SCHEMA_MISMATCH,
			"Tablespace to be imported has a different"
			" page size than this server. Server page size"
			" is %lu, whereas tablespace page size"
			" is " ULINTPF,
			srv_page_size,
			logical_page_size);

		return(DB_ERROR);
	}

	cfg->m_flags = mach_read_from_4(ptr);
	ptr += sizeof(ib_uint32_t);

	cfg->m_zip_size = dict_tf_get_zip_size(cfg->m_flags);
	cfg->m_n_cols = mach_read_from_4(ptr);

	if (!dict_tf_is_valid(cfg->m_flags)) {
		ib_errf(thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_SCHEMA_MISMATCH,
			"Invalid table flags: " ULINTPF, cfg->m_flags);

		return(DB_CORRUPTION);
	}

	err = row_import_read_columns(file, thd, cfg);

	if (err == DB_SUCCESS) {
		err = row_import_read_indexes(file, thd, cfg);
	}

	return(err);
}

/**
Read the contents of the <tablespace>.cfg file.
@return DB_SUCCESS or error code. */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_read_meta_data(
/*======================*/
	FILE*		file,		/*!< in: File to read from */
	THD*		thd,		/*!< in: session */
	row_import&	cfg)		/*!< out: contents of the .cfg file */
{
	byte		row[sizeof(ib_uint32_t)];

	/* Trigger EOF */
	DBUG_EXECUTE_IF("ib_import_io_read_error_9",
			(void) fseek(file, 0L, SEEK_END););

	if (fread(&row, 1, sizeof(row), file) != sizeof(row)) {
		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno),
			"while reading meta-data version.");

		return(DB_IO_ERROR);
	}

	cfg.m_version = mach_read_from_4(row);

	/* Check the version number. */
	switch (cfg.m_version) {
	case IB_EXPORT_CFG_VERSION_V1:

		return(row_import_read_v1(file, thd, &cfg));
	default:
		ib_errf(thd, IB_LOG_LEVEL_ERROR, ER_IO_READ_ERROR,
			"Unsupported meta-data version number (" ULINTPF "), "
			"file ignored", cfg.m_version);
	}

	return(DB_ERROR);
}

#define BTR_BLOB_HDR_PART_LEN 0 /*!< BLOB part len on this page */
#define BTR_BLOB_HDR_NEXT_PAGE_NO 4 /*!< next BLOB part page no,
                                    FIL_NULL if none */
#define BTR_BLOB_HDR_SIZE 8 /*!< Size of a BLOB part header, in bytes */

/* decrypt and decompress page if needed */
static dberr_t decrypt_decompress(fil_space_crypt_t *space_crypt,
                                  uint32_t space_flags, span<byte> page,
                                  uint32_t space_id, byte *page_compress_buf)
{
  auto *data= page.data();

  if (space_crypt && space_crypt->should_encrypt())
  {
    if (!buf_page_verify_crypt_checksum(data, space_flags))
      return DB_CORRUPTION;

    if (dberr_t err= fil_space_decrypt(space_id, space_flags, space_crypt,
                                       data, page.size(), data))
      return err;
  }

  bool page_compressed= false;

  if (fil_space_t::full_crc32(space_flags) &&
      fil_space_t::is_compressed(space_flags))
    page_compressed= buf_page_is_compressed(data, space_flags);
  else
  {
    switch (fil_page_get_type(data)) {
    case FIL_PAGE_PAGE_COMPRESSED:
    case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
      page_compressed= true;
    }
  }

  if (page_compressed)
  {
    auto compress_length=
      fil_page_decompress(page_compress_buf, data, space_flags);
    ut_ad(compress_length != srv_page_size);

    if (compress_length == 0)
      return DB_CORRUPTION;
  }

  return DB_SUCCESS;
}

static size_t get_buf_size()
{
  return srv_page_size + (
           provider_service_lzo->is_loaded ? LZO1X_1_15_MEM_COMPRESS :
           provider_service_snappy->is_loaded ? snappy_max_compressed_length(srv_page_size) :
           0
         );
}

/* find, parse instant metadata, performing variaous checks,
and apply it to dict_table_t
@return DB_SUCCESS or some error */
static dberr_t handle_instant_metadata(dict_table_t *table,
                                       const row_import &cfg)
{
  dict_get_and_save_data_dir_path(table);

  char *filepath;
  if (DICT_TF_HAS_DATA_DIR(table->flags))
  {
    ut_a(table->data_dir_path);
    filepath= fil_make_filepath(table->data_dir_path, table->name, IBD, true);
  }
  else
    filepath= fil_make_filepath(nullptr, table->name, IBD, false);

  if (!filepath)
    return DB_OUT_OF_MEMORY;

  SCOPE_EXIT([filepath]() { ut_free(filepath); });

  bool success;
  auto file= os_file_create_simple_no_error_handling(
      innodb_data_file_key, filepath, OS_FILE_OPEN, OS_FILE_READ_WRITE, false,
      &success);
  if (!success)
    return DB_IO_ERROR;

  if (os_file_get_size(file) < srv_page_size * 4)
    return DB_CORRUPTION;

  SCOPE_EXIT([&file]() { os_file_close(file); });

  std::unique_ptr<byte[], decltype(&aligned_free)> first_page(
      static_cast<byte *>(aligned_malloc(srv_page_size, srv_page_size)),
      &aligned_free);

  if (dberr_t err= os_file_read_no_error_handling(IORequestReadPartial,
                                                  file, first_page.get(), 0,
                                                  srv_page_size, nullptr))
    return err;

  auto space_flags= fsp_header_get_flags(first_page.get());

  if (!fil_space_t::is_valid_flags(space_flags, true))
  {
    auto cflags= fsp_flags_convert_from_101(space_flags);
    if (cflags == UINT32_MAX)
    {
      ib::error() << "Invalid FSP_SPACE_FLAGS=" << ib::hex(space_flags);
      return DB_CORRUPTION;
    }
    space_flags= static_cast<decltype(space_flags)>(cflags);
  }

  if (!cfg.m_missing)
  {
    if (dberr_t err= cfg.match_flags(current_thd))
      return err;
  }

  const unsigned zip_size= fil_space_t::zip_size(space_flags);
  const unsigned physical_size= zip_size ? zip_size : unsigned(srv_page_size);
  ut_ad(physical_size <= UNIV_PAGE_SIZE_MAX);
  const uint32_t space_id= page_get_space_id(first_page.get());

  auto *space_crypt= fil_space_read_crypt_data(zip_size, first_page.get());
  SCOPE_EXIT([&space_crypt]() {
    if (space_crypt)
      fil_space_destroy_crypt_data(&space_crypt);
  });

  std::unique_ptr<byte[], decltype(&aligned_free)> page(
      static_cast<byte *>(
          aligned_malloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX)),
      &aligned_free);

  if (dberr_t err= os_file_read_no_error_handling(
          IORequestReadPartial, file, page.get(), 3 * physical_size,
          physical_size, nullptr))
    return err;

  std::unique_ptr<byte[]> page_compress_buf(new byte[get_buf_size()]);

  if (dberr_t err= decrypt_decompress(space_crypt, space_flags,
                                      {page.get(), static_cast<size_t>
                                       (physical_size)},
                                      space_id, page_compress_buf.get()))
    return err;

  if (table->supports_instant())
  {
    dict_index_t *index= dict_table_get_first_index(table);

    auto tmp1= table->space_id;
    table->space_id= page_get_space_id(page.get());
    SCOPE_EXIT([tmp1, table]() { table->space_id= tmp1; });

    auto tmp2= index->page;
    index->page= page_get_page_no(page.get());
    SCOPE_EXIT([tmp2, index]() { index->page= tmp2; });

    if (!page_is_comp(page.get()) != !dict_table_is_comp(table))
    {
      ib_errf(current_thd, IB_LOG_LEVEL_ERROR, ER_TABLE_SCHEMA_MISMATCH,
              "ROW_FORMAT mismatch");
      return DB_CORRUPTION;
    }

    if (btr_cur_instant_root_init(index, page.get()))
      return DB_ERROR;

    ut_ad(index->n_core_null_bytes != dict_index_t::NO_CORE_NULL_BYTES);

    if (fil_page_get_type(page.get()) == FIL_PAGE_INDEX)
    {
      ut_ad(!index->is_instant());
      return DB_SUCCESS;
    }

    mem_heap_t *heap= NULL;
    SCOPE_EXIT([&heap]() {
      if (heap)
        mem_heap_free(heap);
    });

    while (btr_page_get_level(page.get()) != 0)
    {
      const rec_t *rec= page_rec_get_next(page_get_infimum_rec(page.get()));

      /* Relax the assertion in rec_init_offsets(). */
      ut_ad(!index->in_instant_init);
      ut_d(index->in_instant_init= true);
      rec_offs *offsets=
          rec_get_offsets(rec, index, nullptr, 0, ULINT_UNDEFINED, &heap);
      ut_d(index->in_instant_init= false);

      uint64_t child_page_no= btr_node_ptr_get_child_page_no(rec, offsets);

      if (dberr_t err=
          os_file_read_no_error_handling(IORequestReadPartial, file,
                                         page.get(),
                                         child_page_no * physical_size,
                                         physical_size, nullptr))
        return err;

      if (dberr_t err= decrypt_decompress(space_crypt, space_flags,
                                          {page.get(), static_cast<size_t>
                                           (physical_size)}, space_id,
                                          page_compress_buf.get()))
        return err;
    }

    const auto *rec= page_rec_get_next(page_get_infimum_rec(page.get()));
    const auto comp= dict_table_is_comp(index->table);
    const auto info_bits= rec_get_info_bits(rec, comp);

    if (page_rec_is_supremum(rec) || !(info_bits & REC_INFO_MIN_REC_FLAG))
    {
      ib::error() << "Table " << index->table->name
                  << " is missing instant ALTER metadata";
      index->table->corrupted= true;
      return DB_CORRUPTION;
    }

    if ((info_bits & ~REC_INFO_DELETED_FLAG) != REC_INFO_MIN_REC_FLAG ||
        (comp && rec_get_status(rec) != REC_STATUS_INSTANT))
    {
    incompatible:
      ib::error() << "Table " << index->table->name
                  << " contains unrecognizable instant ALTER metadata";
      index->table->corrupted= true;
      return DB_CORRUPTION;
    }

    if (info_bits & REC_INFO_DELETED_FLAG)
    {
      ulint trx_id_offset= index->trx_id_offset;
      ut_ad(index->n_uniq);

      if (trx_id_offset)
      {
      }
      else if (index->table->not_redundant())
      {

        for (uint i= index->n_uniq; i--;)
          trx_id_offset+= index->fields[i].fixed_len;
      }
      else if (rec_get_1byte_offs_flag(rec))
      {
        trx_id_offset= rec_1_get_field_end_info(rec, index->n_uniq - 1);
        ut_ad(!(trx_id_offset & REC_1BYTE_SQL_NULL_MASK));
        trx_id_offset&= ~REC_1BYTE_SQL_NULL_MASK;
      }
      else
      {
        trx_id_offset= rec_2_get_field_end_info(rec, index->n_uniq - 1);
        ut_ad(!(trx_id_offset & REC_2BYTE_SQL_NULL_MASK));
        trx_id_offset&= ~REC_2BYTE_SQL_NULL_MASK;
      }

      const byte *ptr=
          rec + trx_id_offset + (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

      if (mach_read_from_4(ptr + BTR_EXTERN_LEN))
        goto incompatible;

      uint len= mach_read_from_4(ptr + BTR_EXTERN_LEN + 4);
      if (!len || mach_read_from_4(ptr + BTR_EXTERN_OFFSET) != FIL_PAGE_DATA)
        goto incompatible;

      std::unique_ptr<byte[], decltype(&aligned_free)>
        second_page(static_cast<byte*>(aligned_malloc(physical_size,
                                                      physical_size)),
                    &aligned_free);

      if (dberr_t err=
          os_file_read_no_error_handling(IORequestReadPartial, file,
                                         second_page.get(), physical_size *
                                         mach_read_from_4(ptr +
                                                          BTR_EXTERN_PAGE_NO),
                                         srv_page_size, nullptr))
        return err;

      if (dberr_t err= decrypt_decompress(space_crypt, space_flags,
                                          {second_page.get(),
                                           static_cast<size_t>(physical_size)},
                                          space_id, page_compress_buf.get()))
        return err;

      if (fil_page_get_type(second_page.get()) != FIL_PAGE_TYPE_BLOB ||
          mach_read_from_4(
              &second_page[FIL_PAGE_DATA + BTR_BLOB_HDR_NEXT_PAGE_NO]) !=
              FIL_NULL ||
          mach_read_from_4(
              &second_page[FIL_PAGE_DATA + BTR_BLOB_HDR_PART_LEN]) != len)
        goto incompatible;

      /* The unused part of the BLOB page should be zero-filled. */
      for (const byte *
               b= second_page.get() + (FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE) +
                  len,
              *const end= second_page.get() + srv_page_size - BTR_EXTERN_LEN;
           b < end;)
      {
        if (*b++)
          goto incompatible;
      }

      if (index->table->deserialise_columns(
              &second_page[FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE], len))
        goto incompatible;
    }

    rec_offs *offsets= rec_get_offsets(
        rec, index, nullptr, index->n_core_fields, ULINT_UNDEFINED, &heap);
    if (rec_offs_any_default(offsets))
    {
    inconsistent:
      goto incompatible;
    }

    /* In fact, because we only ever append fields to the metadata
    record, it is also OK to perform READ UNCOMMITTED and
    then ignore any extra fields, provided that
    trx_sys.is_registered(DB_TRX_ID). */
    if (rec_offs_n_fields(offsets) >
            ulint(index->n_fields) + !!index->table->instant &&
        !trx_sys.is_registered(current_trx(),
                               row_get_rec_trx_id(rec, index, offsets)))
      goto inconsistent;

    for (unsigned i= index->n_core_fields; i < index->n_fields; i++)
    {
      dict_col_t *col= index->fields[i].col;
      const unsigned o= i + !!index->table->instant;
      ulint len;
      const byte *data= rec_get_nth_field(rec, offsets, o, &len);
      ut_ad(!col->is_added());
      ut_ad(!col->def_val.data);
      col->def_val.len= len;
      switch (len) {
      case UNIV_SQL_NULL:
        continue;
      case 0:
        col->def_val.data= field_ref_zero;
        continue;
      }
      ut_ad(len != UNIV_SQL_DEFAULT);
      if (!rec_offs_nth_extern(offsets, o))
        col->def_val.data= mem_heap_dup(index->table->heap, data, len);
      else if (len < BTR_EXTERN_FIELD_REF_SIZE ||
               !memcmp(data + len - BTR_EXTERN_FIELD_REF_SIZE, field_ref_zero,
                       BTR_EXTERN_FIELD_REF_SIZE))
      {
        col->def_val.len= UNIV_SQL_DEFAULT;
        goto inconsistent;
      }
      else
      {
        col->def_val.data= btr_copy_externally_stored_field(
            &col->def_val.len, data, srv_page_size, len, index->table->heap);
      }
    }
  }

  return DB_SUCCESS;
}

/**
Read the contents of the <tablename>.cfg file.
@return DB_SUCCESS or error code. */
static	MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_import_read_cfg(
/*================*/
	dict_table_t*	table,	/*!< in: table */
	THD*		thd,	/*!< in: session */
	row_import&	cfg)	/*!< out: contents of the .cfg file */
{
	dberr_t		err;
	char		name[OS_FILE_MAX_PATH];

	cfg.m_table = table;

	srv_get_meta_data_filename(table, name, sizeof(name));

	FILE*	file = fopen(name, "rb");

	if (file == NULL) {
		char	msg[BUFSIZ];

		snprintf(msg, sizeof(msg),
			 "Error opening '%s', will attempt to import"
			 " without schema verification", name);

		ib_senderrf(
			thd, IB_LOG_LEVEL_WARN, ER_IO_READ_ERROR,
			(ulong) errno, strerror(errno), msg);

		cfg.m_missing = true;

		err = DB_FAIL;
	} else {

		cfg.m_missing = false;

		err = row_import_read_meta_data(file, thd, cfg);
		fclose(file);
	}

	return(err);
}

/** Update the root page numbers and tablespace ID of a table.
@param[in,out]	trx	dictionary transaction
@param[in,out]	table	persistent table
@param[in]	reset	whether to reset the fields to FIL_NULL
@return DB_SUCCESS or error code */
dberr_t
row_import_update_index_root(trx_t* trx, dict_table_t* table, bool reset)
{
	const dict_index_t*	index;
	que_t*			graph = 0;
	dberr_t			err = DB_SUCCESS;

	ut_ad(reset || table->space->id == table->space_id);

	static const char	sql[] = {
		"PROCEDURE UPDATE_INDEX_ROOT() IS\n"
		"BEGIN\n"
		"UPDATE SYS_INDEXES\n"
		"SET SPACE = :space,\n"
		"    PAGE_NO = :page,\n"
		"    TYPE = :type\n"
		"WHERE TABLE_ID = :table_id AND ID = :index_id;\n"
		"END;\n"};

	table->def_trx_id = trx->id;

	for (index = dict_table_get_first_index(table);
	     index != 0;
	     index = dict_table_get_next_index(index)) {

		pars_info_t*	info;
		ib_uint32_t	page;
		ib_uint32_t	space;
		ib_uint32_t	type;
		index_id_t	index_id;
		table_id_t	table_id;

		info = (graph != 0) ? graph->info : pars_info_create();

		mach_write_to_4(
			reinterpret_cast<byte*>(&type),
			index->type);

		mach_write_to_4(
			reinterpret_cast<byte*>(&page),
			reset ? FIL_NULL : index->page);

		mach_write_to_4(
			reinterpret_cast<byte*>(&space),
			reset ? FIL_NULL : index->table->space_id);

		mach_write_to_8(
			reinterpret_cast<byte*>(&index_id),
			index->id);

		mach_write_to_8(
			reinterpret_cast<byte*>(&table_id),
			table->id);

		/* If we set the corrupt bit during the IMPORT phase then
		we need to update the system tables. */
		pars_info_bind_int4_literal(info, "type", &type);
		pars_info_bind_int4_literal(info, "space", &space);
		pars_info_bind_int4_literal(info, "page", &page);
		pars_info_bind_ull_literal(info, "index_id", &index_id);
		pars_info_bind_ull_literal(info, "table_id", &table_id);

		if (graph == 0) {
			graph = pars_sql(info, sql);
			ut_a(graph);
			graph->trx = trx;
		}

		que_thr_t*	thr;

		ut_a(thr = que_fork_start_command(graph));

		que_run_threads(thr);

		DBUG_EXECUTE_IF("ib_import_internal_error",
				trx->error_state = DB_ERROR;);

		err = trx->error_state;

		if (err != DB_SUCCESS) {
			ib_errf(trx->mysql_thd, IB_LOG_LEVEL_ERROR,
				ER_INTERNAL_ERROR,
				"While updating the <space, root page"
				" number> of index %s - %s",
				index->name(), ut_strerr(err));

			break;
		}
	}

	que_graph_free(graph);

	return(err);
}

/** Callback arg for row_import_set_discarded. */
struct discard_t {
	ib_uint32_t	flags2;			/*!< Value read from column */
	bool		state;			/*!< New state of the flag */
	ulint		n_recs;			/*!< Number of recs processed */
};

/******************************************************************//**
Fetch callback that sets or unsets the DISCARDED tablespace flag in
SYS_TABLES. The flags is stored in MIX_LEN column.
@return FALSE if all OK */
static
ibool
row_import_set_discarded(
/*=====================*/
	void*		row,			/*!< in: sel_node_t* */
	void*		user_arg)		/*!< in: bool set/unset flag */
{
	sel_node_t*	node = static_cast<sel_node_t*>(row);
	discard_t*	discard = static_cast<discard_t*>(user_arg);
	dfield_t*	dfield = que_node_get_val(node->select_list);
	dtype_t*	type = dfield_get_type(dfield);
	ulint		len = dfield_get_len(dfield);

	ut_a(dtype_get_mtype(type) == DATA_INT);
	ut_a(len == sizeof(ib_uint32_t));

	ulint	flags2 = mach_read_from_4(
		static_cast<byte*>(dfield_get_data(dfield)));

	if (discard->state) {
		flags2 |= DICT_TF2_DISCARDED;
	} else {
		flags2 &= ~DICT_TF2_DISCARDED;
	}

	mach_write_to_4(reinterpret_cast<byte*>(&discard->flags2), flags2);

	++discard->n_recs;

	/* There should be at most one matching record. */
	ut_a(discard->n_recs == 1);

	return(FALSE);
}

/** Update the DICT_TF2_DISCARDED flag in SYS_TABLES.MIX_LEN.
@param[in,out]	trx		dictionary transaction
@param[in]	table_id	table identifier
@param[in]	discarded	whether to set or clear the flag
@return DB_SUCCESS or error code */
dberr_t row_import_update_discarded_flag(trx_t* trx, table_id_t table_id,
					 bool discarded)
{
	pars_info_t*		info;
	discard_t		discard;

	static const char	sql[] =
		"PROCEDURE UPDATE_DISCARDED_FLAG() IS\n"
		"DECLARE FUNCTION my_func;\n"
		"DECLARE CURSOR c IS\n"
		" SELECT MIX_LEN"
		" FROM SYS_TABLES"
		" WHERE ID = :table_id FOR UPDATE;"
		"\n"
		"BEGIN\n"
		"OPEN c;\n"
		"WHILE 1 = 1 LOOP\n"
		"  FETCH c INTO my_func();\n"
		"  IF c % NOTFOUND THEN\n"
		"    EXIT;\n"
		"  END IF;\n"
		"END LOOP;\n"
		"UPDATE SYS_TABLES"
		" SET MIX_LEN = :flags2"
		" WHERE ID = :table_id;\n"
		"CLOSE c;\n"
		"END;\n";

	discard.n_recs = 0;
	discard.state = discarded;
	discard.flags2 = ULINT32_UNDEFINED;

	info = pars_info_create();

	pars_info_add_ull_literal(info, "table_id", table_id);
	pars_info_bind_int4_literal(info, "flags2", &discard.flags2);

	pars_info_bind_function(
		info, "my_func", row_import_set_discarded, &discard);

	dberr_t	err = que_eval_sql(info, sql, trx);

	ut_a(discard.n_recs == 1);
	ut_a(discard.flags2 != ULINT32_UNDEFINED);

	return(err);
}

/** InnoDB writes page by page when there is page compressed
tablespace involved. It does help to save the disk space when
punch hole is enabled
@param iter     Tablespace iterator
@param full_crc32    whether the file is in the full_crc32 format
@param offset   offset of the file to be written
@param writeptr buffer to be written
@param n_bytes  number of bytes to be written
@param try_punch_only   Try the range punch only because the
                        current range is full of empty pages
@return DB_SUCCESS */
static
dberr_t fil_import_compress_fwrite(const fil_iterator_t &iter,
                                   bool full_crc32,
                                   os_offset_t offset,
                                   const byte *writeptr,
                                   ulint n_bytes,
                                   bool try_punch_only= false)
{
  if (dberr_t err= os_file_punch_hole(iter.file, offset, n_bytes))
    return err;

  if (try_punch_only)
    return DB_SUCCESS;

  for (ulint j= 0; j < n_bytes; j+= srv_page_size)
  {
    /* Read the original data length from block and
    safer to read FIL_PAGE_COMPRESSED_SIZE because it
    is not encrypted*/
    ulint n_write_bytes= srv_page_size;
    if (j || offset)
    {
      n_write_bytes= mach_read_from_2(writeptr + j + FIL_PAGE_DATA);
      const unsigned ptype= mach_read_from_2(writeptr + j + FIL_PAGE_TYPE);
      /* Ignore the empty page */
      if (ptype == 0 && n_write_bytes == 0)
        continue;
      if (full_crc32)
        n_write_bytes= buf_page_full_crc32_size(writeptr + j,
                                                nullptr, nullptr);
      else
      {
        n_write_bytes+= ptype == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED
          ? FIL_PAGE_DATA + FIL_PAGE_ENCRYPT_COMP_METADATA_LEN
          : FIL_PAGE_DATA + FIL_PAGE_COMP_METADATA_LEN;
      }
    }

    if (dberr_t err= os_file_write(IORequestWrite, iter.filepath, iter.file,
                                   writeptr + j, offset + j, n_write_bytes))
      return err;
  }

  return DB_SUCCESS;
}

dberr_t FetchIndexRootPages::run(const fil_iterator_t& iter,
                                 buf_block_t* block) UNIV_NOTHROW
{
  const unsigned zip_size= fil_space_t::zip_size(m_space_flags);
  const unsigned size= zip_size ? zip_size : unsigned(srv_page_size);
  byte* page_compress_buf= static_cast<byte*>(malloc(get_buf_size()));
  const bool full_crc32 = fil_space_t::full_crc32(m_space_flags);
  bool skip_checksum_check = false;
  ut_ad(!srv_read_only_mode);

  if (!page_compress_buf)
    return DB_OUT_OF_MEMORY;

  const bool encrypted= iter.crypt_data != NULL &&
    iter.crypt_data->should_encrypt();
  byte* const readptr= iter.io_buffer;
  block->page.frame= readptr;

  if (block->page.zip.data)
    block->page.zip.data= readptr;

  bool page_compressed= false;

  dberr_t err= os_file_read_no_error_handling(
    IORequestReadPartial, iter.file, readptr, 3 * size, size, 0);
  if (err != DB_SUCCESS)
  {
    ib::error() << iter.filepath << ": os_file_read() failed";
    goto func_exit;
  }

  if (page_get_page_no(readptr) != 3)
  {
page_corrupted:
    ib::warn() << filename() << ": Page 3 at offset "
               << 3 * size << " looks corrupted.";
    err= DB_CORRUPTION;
    goto func_exit;
  }

  block->page.id_.set_page_no(3);
  if (full_crc32 && fil_space_t::is_compressed(m_space_flags))
    page_compressed= buf_page_is_compressed(readptr, m_space_flags);
  else
  {
    switch (fil_page_get_type(readptr)) {
    case FIL_PAGE_PAGE_COMPRESSED:
    case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
      if (block->page.zip.data)
        goto page_corrupted;
      page_compressed= true;
    }
  }

  if (encrypted)
  {
    if (!buf_page_verify_crypt_checksum(readptr, m_space_flags))
      goto page_corrupted;

    if ((err= fil_space_decrypt(get_space_id(), m_space_flags, iter.crypt_data,
                                readptr, size, readptr)))
      goto func_exit;
  }

  /* For full_crc32 format, skip checksum check
  after decryption. */
  skip_checksum_check= full_crc32 && encrypted;

  if (page_compressed)
  {
    ulint compress_length= fil_page_decompress(page_compress_buf,
                                               readptr,
                                               m_space_flags);
    ut_ad(compress_length != srv_page_size);
    if (compress_length == 0)
      goto page_corrupted;
  }
  else if (!skip_checksum_check
           && buf_page_is_corrupted(false, readptr, m_space_flags))
    goto page_corrupted;

  err= this->operator()(block);
func_exit:
  free(page_compress_buf);
  return err;
}

static dberr_t fil_iterate(
	const fil_iterator_t&	iter,
	buf_block_t*		block,
	AbstractCallback&	callback)
{
	os_offset_t		offset;
	const ulint		size = callback.physical_size();
	ulint			n_bytes = iter.n_io_buffers * size;

	byte* page_compress_buf= static_cast<byte*>(malloc(get_buf_size()));
	ut_ad(!srv_read_only_mode);

	if (!page_compress_buf) {
		return DB_OUT_OF_MEMORY;
	}

	uint32_t actual_space_id = 0;
	const bool full_crc32 = fil_space_t::full_crc32(
		callback.get_space_flags());

	/* TODO: For ROW_FORMAT=COMPRESSED tables we do a lot of useless
	copying for non-index pages. Unfortunately, it is
	required by buf_zip_decompress() */
	dberr_t		err = DB_SUCCESS;
	bool		page_compressed = false;
	bool		punch_hole = !my_test_if_thinly_provisioned(iter.file);

	for (offset = iter.start; offset < iter.end; offset += n_bytes) {
		if (callback.is_interrupted()) {
			err = DB_INTERRUPTED;
			goto func_exit;
		}

		byte*		io_buffer = iter.io_buffer;
		block->page.frame = io_buffer;

		if (block->page.zip.data) {
			/* Zip IO is done in the compressed page buffer. */
			io_buffer = block->page.zip.data;
		}

		/* We have to read the exact number of bytes. Otherwise the
		InnoDB IO functions croak on failed reads. */

		n_bytes = ulint(ut_min(os_offset_t(n_bytes),
				       iter.end - offset));

		ut_ad(n_bytes > 0);
		ut_ad(!(n_bytes % size));

		const bool encrypted = iter.crypt_data != NULL
			&& iter.crypt_data->should_encrypt();
		/* Use additional crypt io buffer if tablespace is encrypted */
		byte* const readptr = encrypted
			? iter.crypt_io_buffer : io_buffer;
		byte* const writeptr = readptr;

		err = os_file_read_no_error_handling(
			IORequestReadPartial,
			iter.file, readptr, offset, n_bytes, 0);
		if (err != DB_SUCCESS) {
			ib::error() << iter.filepath
				    << ": os_file_read() failed";
			goto func_exit;
		}

		bool		updated = false;
		os_offset_t	page_off = offset;
		ulint		n_pages_read = n_bytes / size;
		/* This block is not attached to buf_pool */
		block->page.id_.set_page_no(uint32_t(page_off / size));

		for (ulint i = 0; i < n_pages_read;
		     ++block->page.id_,
		     ++i, page_off += size, block->page.frame += size) {
			byte*	src = readptr + i * size;
			const ulint page_no = page_get_page_no(src);
			if (!page_no && block->page.id().page_no()) {
				if (!buf_is_zeroes(span<const byte>(src,
								    size))) {
					goto page_corrupted;
				}
				/* Proceed to the next page,
				because this one is all zero. */
				continue;
			}

			if (page_no != block->page.id().page_no()) {
page_corrupted:
				ib::warn() << callback.filename()
					   << ": Page " << (offset / size)
					   << " at offset " << offset
					   << " looks corrupted.";
				err = DB_CORRUPTION;
				goto func_exit;
			}

			if (block->page.id().page_no() == 0) {
				actual_space_id = mach_read_from_4(
					src + FIL_PAGE_SPACE_ID);
			}

			const uint16_t type = fil_page_get_type(src);
			page_compressed =
				(full_crc32
				 && fil_space_t::is_compressed(
					callback.get_space_flags())
				 && buf_page_is_compressed(
					src, callback.get_space_flags()))
				|| type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED
				|| type == FIL_PAGE_PAGE_COMPRESSED;

			if (page_compressed && block->page.zip.data) {
				goto page_corrupted;
			}

			bool decrypted = false;
			byte* dst = io_buffer + i * size;
			bool frame_changed = false;
			uint key_version = buf_page_get_key_version(
				src, callback.get_space_flags());

			if (!encrypted) {
			} else if (!key_version) {
				if (block->page.id().page_no() == 0
				    && block->page.zip.data) {
					block->page.zip.data = src;
					frame_changed = true;
				} else if (!page_compressed
					   && !block->page.zip.data) {
					block->page.frame = src;
					frame_changed = true;
				} else {
					ut_ad(dst != src);
					memcpy(dst, src, size);
				}
			} else {
				if (!buf_page_verify_crypt_checksum(
					src, callback.get_space_flags())) {
					goto page_corrupted;
				}

				if ((err = fil_space_decrypt(
					actual_space_id,
					callback.get_space_flags(),
					iter.crypt_data, dst,
					callback.physical_size(),
					src))) {
					goto func_exit;
				}

				decrypted = true;
				updated = true;
			}

			/* For full_crc32 format, skip checksum check
			after decryption. */
			bool skip_checksum_check = full_crc32 && encrypted;

			/* If the original page is page_compressed, we need
			to decompress it before adjusting further. */
			if (page_compressed) {
				ulint compress_length = fil_page_decompress(
					page_compress_buf, dst,
					callback.get_space_flags());
				ut_ad(compress_length != srv_page_size);
				if (compress_length == 0) {
					goto page_corrupted;
				}
				updated = true;
			} else if (!skip_checksum_check
				   && buf_page_is_corrupted(
					   false,
					   encrypted && !frame_changed
					   ? dst : src,
					   callback.get_space_flags())) {
				goto page_corrupted;
			}

			if ((err = callback(block)) != DB_SUCCESS) {
				goto func_exit;
			} else if (!updated) {
				updated = !!block->page.frame;
			}

			/* If tablespace is encrypted we use additional
			temporary scratch area where pages are read
			for decrypting readptr == crypt_io_buffer != io_buffer.

			Destination for decryption is a buffer pool block
			block->page.frame == dst == io_buffer that is updated.
			Pages that did not require decryption even when
			tablespace is marked as encrypted are not copied
			instead block->page.frame is set to src == readptr.

			For encryption we again use temporary scratch area
			writeptr != io_buffer == dst
			that is then written to the tablespace

			(1) For normal tables io_buffer == dst == writeptr
			(2) For only page compressed tables
			io_buffer == dst == writeptr
			(3) For encrypted (and page compressed)
			readptr != io_buffer == dst != writeptr
			*/

			ut_ad(!encrypted && !page_compressed ?
			      src == dst && dst == writeptr + (i * size):1);
			ut_ad(page_compressed && !encrypted ?
			      src == dst && dst == writeptr + (i * size):1);
			ut_ad(encrypted ?
			      src != dst && dst != writeptr + (i * size):1);

			/* When tablespace is encrypted or compressed its
			first page (i.e. page 0) is not encrypted or
			compressed and there is no need to copy frame. */
			if (encrypted && block->page.id().page_no() != 0) {
				byte *local_frame = callback.get_frame(block);
				ut_ad((writeptr + (i * size)) != local_frame);
				memcpy((writeptr + (i * size)), local_frame, size);
			}

			if (frame_changed) {
				if (block->page.zip.data) {
					block->page.zip.data = dst;
				} else {
					block->page.frame = dst;
				}
			}

			src =  io_buffer + (i * size);

			if (page_compressed) {
				updated = true;
				if (ulint len = fil_page_compress(
					    src,
					    page_compress_buf,
					    callback.get_space_flags(),
					    512,/* FIXME: proper block size */
					    encrypted)) {
					/* FIXME: remove memcpy() */
					memcpy(src, page_compress_buf, len);
					memset(src + len, 0,
					       srv_page_size - len);
				}
			}

			/* Encrypt the page if encryption was used. */
			if (encrypted && decrypted) {
				byte *dest = writeptr + i * size;

				byte* tmp = fil_encrypt_buf(
					iter.crypt_data,
					block->page.id().space(),
					block->page.id().page_no(),
					src, block->zip_size(), dest,
					full_crc32);

				if (tmp == src) {
					/* TODO: remove unnecessary memcpy's */
					ut_ad(dest != src);
					memcpy(dest, src, size);
				}

				updated = true;
			}

			/* Write checksum for the compressed full crc32 page.*/
			if (full_crc32 && page_compressed) {
				ut_ad(updated);
				byte* dest = writeptr + i * size;
				ut_d(bool comp = false);
				ut_d(bool corrupt = false);
				ulint size = buf_page_full_crc32_size(
					dest,
#ifdef UNIV_DEBUG
					&comp, &corrupt
#else
					NULL, NULL
#endif
				);
				ut_ad(!comp == (size == srv_page_size));
				ut_ad(!corrupt);
				mach_write_to_4(dest + (size - 4),
						my_crc32c(0, dest, size - 4));
			}
		}

		if (page_compressed && punch_hole) {
			err = fil_import_compress_fwrite(
				iter, full_crc32, offset, writeptr, n_bytes,
				!updated);

			if (err != DB_SUCCESS) {
				punch_hole = false;
				if (updated) {
					goto normal_write;
				}
			}
		} else if (updated) {
normal_write:
			/* A page was updated in the set, write it back. */
			err = os_file_write(IORequestWrite,
					    iter.filepath, iter.file,
					    writeptr, offset, n_bytes);

			if (err != DB_SUCCESS) {
				goto func_exit;
			}
		}
	}

func_exit:
	free(page_compress_buf);
	return err;
}

/********************************************************************//**
Iterate over all the pages in the tablespace.
@param table - the table definiton in the server
@param n_io_buffers - number of blocks to read and write together
@param callback - functor that will do the page updates
@return	DB_SUCCESS or error code */
static
dberr_t
fil_tablespace_iterate(
/*===================*/
	dict_table_t*		table,
	ulint			n_io_buffers,
	AbstractCallback&	callback)
{
	dberr_t		err;
	pfs_os_file_t	file;
	char*		filepath;

	ut_a(n_io_buffers > 0);
	ut_ad(!srv_read_only_mode);

	DBUG_EXECUTE_IF("ib_import_trigger_corruption_1",
			return(DB_CORRUPTION););

	/* Make sure the data_dir_path is set. */
	dict_get_and_save_data_dir_path(table);

	ut_ad(!DICT_TF_HAS_DATA_DIR(table->flags) || table->data_dir_path);

	const char *data_dir_path = DICT_TF_HAS_DATA_DIR(table->flags)
		? table->data_dir_path : nullptr;

	filepath = fil_make_filepath(data_dir_path,
				     {table->name.m_name,
				      strlen(table->name.m_name)},
				     IBD, data_dir_path != nullptr);
	if (!filepath) {
		return(DB_OUT_OF_MEMORY);
	} else {
		bool	success;

		file = os_file_create_simple_no_error_handling(
			innodb_data_file_key, filepath,
			OS_FILE_OPEN, OS_FILE_READ_WRITE, false, &success);

		if (!success) {
			/* The following call prints an error message */
			os_file_get_last_error(true);
			ib::error() << "Trying to import a tablespace,"
				" but could not open the tablespace file "
				    << filepath;
			ut_free(filepath);
			return DB_TABLESPACE_NOT_FOUND;
		} else {
			err = DB_SUCCESS;
		}
	}

	callback.set_file(filepath, file);

	os_offset_t	file_size = os_file_get_size(file);
	ut_a(file_size != (os_offset_t) -1);

	/* Allocate a page to read in the tablespace header, so that we
	can determine the page size and zip_size (if it is compressed).
	We allocate an extra page in case it is a compressed table. */

	byte*	page = static_cast<byte*>(aligned_malloc(2 * srv_page_size,
							 srv_page_size));

	buf_block_t* block = reinterpret_cast<buf_block_t*>
		(ut_zalloc_nokey(sizeof *block));
	block->page.frame = page;
	block->page.init(buf_page_t::UNFIXED + 1, page_id_t{~0ULL});

	/* Read the first page and determine the page and zip size. */

	err = os_file_read_no_error_handling(IORequestReadPartial,
					     file, page, 0, srv_page_size, 0);

	if (err == DB_SUCCESS) {
		err = callback.init(file_size, block);
	}

	if (err == DB_SUCCESS) {
		block->page.id_ = page_id_t(callback.get_space_id(), 0);
		if (ulint zip_size = callback.get_zip_size()) {
			page_zip_set_size(&block->page.zip, zip_size);
			/* ROW_FORMAT=COMPRESSED is not optimised for block IO
			for now. We do the IMPORT page by page. */
			n_io_buffers = 1;
		}

		fil_iterator_t	iter;

		/* read (optional) crypt data */
		iter.crypt_data = fil_space_read_crypt_data(
			callback.get_zip_size(), page);

		/* If tablespace is encrypted, it needs extra buffers */
		if (iter.crypt_data && n_io_buffers > 1) {
			/* decrease io buffers so that memory
			consumption will not double */
			n_io_buffers /= 2;
		}

		iter.file = file;
		iter.start = 0;
		iter.end = file_size;
		iter.filepath = filepath;
		iter.file_size = file_size;
		iter.n_io_buffers = n_io_buffers;

		/* Add an extra page for compressed page scratch area. */
		iter.io_buffer = static_cast<byte*>(
			aligned_malloc((1 + iter.n_io_buffers)
				       << srv_page_size_shift, srv_page_size));

		iter.crypt_io_buffer = iter.crypt_data
			? static_cast<byte*>(
				aligned_malloc((1 + iter.n_io_buffers)
					       << srv_page_size_shift,
					       srv_page_size))
			: NULL;

		if (block->page.zip.ssize) {
			ut_ad(iter.n_io_buffers == 1);
			block->page.frame = iter.io_buffer;
			block->page.zip.data = block->page.frame
				+ srv_page_size;
		}

		err = callback.run(iter, block);

		if (iter.crypt_data) {
			fil_space_destroy_crypt_data(&iter.crypt_data);
		}

		aligned_free(iter.crypt_io_buffer);
		aligned_free(iter.io_buffer);
	}

	if (err == DB_SUCCESS) {
		ib::info() << "Sync to disk";

		if (!os_file_flush(file)) {
			ib::info() << "os_file_flush() failed!";
			err = DB_IO_ERROR;
		} else {
			ib::info() << "Sync to disk - done!";
		}
	}

	os_file_close(file);

	aligned_free(page);
	ut_free(filepath);
	ut_free(block);

	return(err);
}

/*****************************************************************//**
Imports a tablespace. The space id in the .ibd file must match the space id
of the table in the data dictionary.
@return error code or DB_SUCCESS */
dberr_t
row_import_for_mysql(
/*=================*/
	dict_table_t*	table,		/*!< in/out: table */
	row_prebuilt_t*	prebuilt)	/*!< in: prebuilt struct in MySQL */
{
	dberr_t		err;
	ib_uint64_t	autoinc = 0;
	char*		filepath = NULL;
	trx_t*		trx = prebuilt->trx;

	/* The caller assured that this is not read_only_mode and that no
	temorary tablespace is being imported. */
	ut_ad(!srv_read_only_mode);
	ut_ad(!table->is_temporary());

	ut_ad(table->space_id);
	ut_ad(table->space_id < SRV_SPACE_ID_UPPER_BOUND);
	ut_ad(trx);
	ut_ad(trx->state == TRX_STATE_ACTIVE);
	ut_ad(!table->is_readable());

	ibuf_delete_for_discarded_space(table->space_id);

	/* Assign an undo segment for the transaction, so that the
	transaction will be recovered after a crash. */

	/* TODO: Do not write any undo log for the IMPORT cleanup. */
	{
		mtr_t mtr;
		mtr.start();
		trx_undo_assign(trx, &err, &mtr);
		mtr.commit();
	}

	DBUG_EXECUTE_IF("ib_import_undo_assign_failure",
			err = DB_TOO_MANY_CONCURRENT_TRXS;);

	if (err == DB_SUCCESS && !trx->has_logged_persistent()) {
		err = DB_TOO_MANY_CONCURRENT_TRXS;
	}
	if (err != DB_SUCCESS) {
		return row_import_cleanup(prebuilt, err);
	}

	trx->op_info = "read meta-data file";

	row_import	cfg;
	THD* thd = trx->mysql_thd;

	err = row_import_read_cfg(table, thd, cfg);

	/* Check if the table column definitions match the contents
	of the config file. */

	if (err == DB_SUCCESS) {

		if (dberr_t err = handle_instant_metadata(table, cfg)) {
			return row_import_error(prebuilt, err);
		}

		/* We have a schema file, try and match it with our
		data dictionary. */

		err = cfg.match_schema(thd);

		/* Update index->page and SYS_INDEXES.PAGE_NO to match the
		B-tree root page numbers in the tablespace. Use the index
		name from the .cfg file to find match. */

		if (err == DB_SUCCESS) {
			cfg.set_root_by_name();
			autoinc = cfg.m_autoinc;
		}

		DBUG_EXECUTE_IF("ib_import_set_index_root_failure",
				err = DB_TOO_MANY_CONCURRENT_TRXS;);

	} else if (cfg.m_missing) {
		/* We don't have a schema file, we will have to discover
		the index root pages from the .ibd file and skip the schema
		matching step. */

		ut_a(err == DB_FAIL);

		cfg.m_zip_size = 0;

		if (UT_LIST_GET_LEN(table->indexes) > 1) {
			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_INTERNAL_ERROR,
				"Drop all secondary indexes before importing "
				"table %s when .cfg file is missing.",
				table->name.m_name);
			err = DB_ERROR;
			return row_import_error(prebuilt, err);
		}

		FetchIndexRootPages	fetchIndexRootPages(table, trx);

		err = fil_tablespace_iterate(
			table, IO_BUFFER_SIZE(srv_page_size),
			fetchIndexRootPages);

		if (err == DB_SUCCESS) {

			err = fetchIndexRootPages.build_row_import(&cfg);

			/* Update index->page and SYS_INDEXES.PAGE_NO
			to match the B-tree root page numbers in the
			tablespace. */

			if (err == DB_SUCCESS) {
				err = cfg.set_root_by_heuristic();

				if (err == DB_SUCCESS) {
					err = handle_instant_metadata(table,
								      cfg);
				}
			}
		}
	}

	if (err != DB_SUCCESS) {
		return row_import_error(prebuilt, err);
	}

	trx->op_info = "importing tablespace";

	ib::info() << "Phase I - Update all pages";

	/* Iterate over all the pages and do the sanity checking and
	the conversion required to import the tablespace. */

	PageConverter	converter(&cfg, table->space_id, trx);

	/* Set the IO buffer size in pages. */

	err = fil_tablespace_iterate(
		table, IO_BUFFER_SIZE(cfg.m_zip_size ? cfg.m_zip_size
				      : srv_page_size), converter);

	DBUG_EXECUTE_IF("ib_import_reset_space_and_lsn_failure",
			err = DB_TOO_MANY_CONCURRENT_TRXS;);
#ifdef BTR_CUR_HASH_ADAPT
	/* On DISCARD TABLESPACE, we did not drop any adaptive hash
	index entries. If we replaced the discarded tablespace with a
	smaller one here, there could still be some adaptive hash
	index entries that point to cached garbage pages in the buffer
	pool, because PageConverter::operator() only evicted those
	pages that were replaced by the imported pages. We must
	detach any remaining adaptive hash index entries, because the
	adaptive hash index must be a subset of the table contents;
	false positives are not tolerated. */
	for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes); index;
	     index = UT_LIST_GET_NEXT(indexes, index)) {
		index = index->clone_if_needed();
	}
#endif /* BTR_CUR_HASH_ADAPT */

	if (err != DB_SUCCESS) {
		char	table_name[MAX_FULL_NAME_LEN + 1];

		innobase_format_name(
			table_name, sizeof(table_name),
			table->name.m_name);

		if (err != DB_DECRYPTION_FAILED) {

			ib_errf(thd, IB_LOG_LEVEL_ERROR,
				ER_INTERNAL_ERROR,
			"Cannot reset LSNs in table %s : %s",
				table_name, ut_strerr(err));
		}

		return row_import_cleanup(prebuilt, err);
	}

	/* If the table is stored in a remote tablespace, we need to
	determine that filepath from the link file and system tables.
	Find the space ID in SYS_TABLES since this is an ALTER TABLE. */
	dict_get_and_save_data_dir_path(table);

	ut_ad(!DICT_TF_HAS_DATA_DIR(table->flags) || table->data_dir_path);
	const char *data_dir_path = DICT_TF_HAS_DATA_DIR(table->flags)
		? table->data_dir_path : nullptr;
	fil_space_t::name_type name{
		table->name.m_name, strlen(table->name.m_name)};

	filepath = fil_make_filepath(data_dir_path, name, IBD,
				     data_dir_path != nullptr);

	DBUG_EXECUTE_IF(
		"ib_import_OOM_15",
		ut_free(filepath);
		filepath = NULL;
	);

	if (filepath == NULL) {
		return row_import_cleanup(prebuilt, DB_OUT_OF_MEMORY);
	}

	/* Open the tablespace so that we can access via the buffer pool.
	The tablespace is initially opened as a temporary one, because
	we will not be writing any redo log for it before we have invoked
	fil_space_t::set_imported() to declare it a persistent tablespace. */

	table->space = fil_ibd_open(
		2, FIL_TYPE_IMPORT, table->space_id,
		dict_tf_to_fsp_flags(table->flags), name, filepath, &err);

	ut_ad((table->space == NULL) == (err != DB_SUCCESS));
	DBUG_EXECUTE_IF("ib_import_open_tablespace_failure",
			err = DB_TABLESPACE_NOT_FOUND; table->space = NULL;);

	if (!table->space) {
		ib_senderrf(thd, IB_LOG_LEVEL_ERROR,
			ER_GET_ERRMSG,
			err, ut_strerr(err), filepath);
	}

	ut_free(filepath);

	if (err == DB_SUCCESS) {
		err = ibuf_check_bitmap_on_import(trx, table->space);
	}

	DBUG_EXECUTE_IF("ib_import_check_bitmap_failure", err = DB_CORRUPTION;);

	if (err != DB_SUCCESS) {
		return row_import_cleanup(prebuilt, err);
	}

	/* The first index must always be the clustered index. */

	dict_index_t*	index = dict_table_get_first_index(table);

	if (!dict_index_is_clust(index)) {
		return row_import_error(prebuilt, DB_CORRUPTION);
	}

	/* Update the Btree segment headers for index node and
	leaf nodes in the root page. Set the new space id. */

	err = btr_root_adjust_on_import(index);

	DBUG_EXECUTE_IF("ib_import_cluster_root_adjust_failure",
			err = DB_CORRUPTION;);

	if (err != DB_SUCCESS) {
		return row_import_error(prebuilt, err);
	} else if (cfg.requires_purge(index->name)) {

		/* Purge any delete-marked records that couldn't be
		purged during the page conversion phase from the
		cluster index. */

		IndexPurge	purge(trx, index);

		trx->op_info = "cluster: purging delete marked records";

		err = purge.garbage_collect();

		trx->op_info = "";
	}

	DBUG_EXECUTE_IF("ib_import_cluster_failure", err = DB_CORRUPTION;);

	if (err != DB_SUCCESS) {
		return row_import_error(prebuilt, err);
	}

	/* For secondary indexes, purge any records that couldn't be purged
	during the page conversion phase. */

	err = row_import_adjust_root_pages_of_secondary_indexes(
		trx, table, cfg);

	DBUG_EXECUTE_IF("ib_import_sec_root_adjust_failure",
			err = DB_CORRUPTION;);

	if (err != DB_SUCCESS) {
		return row_import_error(prebuilt, err);
	}

	/* Ensure that the next available DB_ROW_ID is not smaller than
	any DB_ROW_ID stored in the table. */

	if (prebuilt->clust_index_was_generated) {
		row_import_set_sys_max_row_id(prebuilt, table);
	}

	ib::info() << "Phase III - Flush changes to disk";

	/* Ensure that all pages dirtied during the IMPORT make it to disk.
	The only dirty pages generated should be from the pessimistic purge
	of delete marked records that couldn't be purged in Phase I. */
	while (buf_flush_list_space(prebuilt->table->space));

	for (ulint count = 0; prebuilt->table->space->referenced(); count++) {
		/* Issue a warning every 10.24 seconds, starting after
		2.56 seconds */
		if ((count & 511) == 128) {
			ib::warn() << "Waiting for flush to complete on "
				   << prebuilt->table->name;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	ib::info() << "Phase IV - Flush complete";
	prebuilt->table->space->set_imported();

	/* The dictionary latches will be released in in row_import_cleanup()
	after the transaction commit, for both success and error. */

	row_mysql_lock_data_dictionary(trx);

	/* Update the root pages of the table's indexes. */
	err = row_import_update_index_root(trx, table, false);

	if (err != DB_SUCCESS) {
		return row_import_error(prebuilt, err);
	}

	err = row_import_update_discarded_flag(trx, table->id, false);

	if (err != DB_SUCCESS) {
		return row_import_error(prebuilt, err);
	}

	table->file_unreadable = false;
	table->flags2 &= ~DICT_TF2_DISCARDED & ((1U << DICT_TF2_BITS) - 1);

	/* Set autoinc value read from .cfg file, if one was specified.
	Otherwise, keep the PAGE_ROOT_AUTO_INC as is. */
	if (autoinc) {
		ib::info() << table->name << " autoinc value set to "
			<< autoinc;

		table->autoinc = autoinc--;
		btr_write_autoinc(dict_table_get_first_index(table), autoinc);
	}

	return row_import_cleanup(prebuilt, err);
}
