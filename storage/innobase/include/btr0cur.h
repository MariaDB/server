/*****************************************************************************

Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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
@file include/btr0cur.h
The index tree cursor

Created 10/16/1994 Heikki Tuuri
*******************************************************/

#ifndef btr0cur_h
#define btr0cur_h

#include "dict0dict.h"
#include "page0cur.h"
#include "btr0types.h"
#include "rem0types.h"
#include "gis0type.h"
#include "my_base.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "srw_lock.h"
#endif

/** Mode flags for btr_cur operations; these can be ORed */
enum {
	/** do no undo logging */
	BTR_NO_UNDO_LOG_FLAG = 1,
	/** do no record lock checking */
	BTR_NO_LOCKING_FLAG = 2,
	/** sys fields will be found in the update vector or inserted
	entry */
	BTR_KEEP_SYS_FLAG = 4,

	/** no rollback */
	BTR_NO_ROLLBACK = BTR_NO_UNDO_LOG_FLAG
		| BTR_NO_LOCKING_FLAG | BTR_KEEP_SYS_FLAG,

	/** btr_cur_pessimistic_update() must keep cursor position
	when moving columns to big_rec */
	BTR_KEEP_POS_FLAG = 8,
	/** the caller is creating the index or wants to bypass the
	index->info.online creation log */
	BTR_CREATE_FLAG = 16
};

#include "que0types.h"
#include "row0types.h"

#define btr_cur_get_page_cur(cursor)	(&(cursor)->page_cur)
#define btr_cur_get_block(cursor)	((cursor)->page_cur.block)
#define btr_cur_get_rec(cursor)	((cursor)->page_cur.rec)

/*********************************************************//**
Returns the compressed page on which the tree cursor is positioned.
@return pointer to compressed page, or NULL if the page is not compressed */
UNIV_INLINE
page_zip_des_t*
btr_cur_get_page_zip(
/*=================*/
	btr_cur_t*	cursor);/*!< in: tree cursor */
/*********************************************************//**
Returns the page of a tree cursor.
@return pointer to page */
UNIV_INLINE
page_t*
btr_cur_get_page(
/*=============*/
	btr_cur_t*	cursor);/*!< in: tree cursor */
/*********************************************************//**
Returns the index of a cursor.
@param cursor b-tree cursor
@return index */
#define btr_cur_get_index(cursor) ((cursor)->index())
/*********************************************************//**
Positions a tree cursor at a given record. */
UNIV_INLINE
void
btr_cur_position(
/*=============*/
	dict_index_t*	index,	/*!< in: index */
	rec_t*		rec,	/*!< in: record in tree */
	buf_block_t*	block,	/*!< in: buffer block of rec */
	btr_cur_t*	cursor);/*!< in: cursor */

/** Load the instant ALTER TABLE metadata from the clustered index
when loading a table definition.
@param[in,out]	table	table definition from the data dictionary
@return	error code
@retval	DB_SUCCESS	if no error occurred */
dberr_t
btr_cur_instant_init(dict_table_t* table)
	ATTRIBUTE_COLD __attribute__((nonnull, warn_unused_result));

/** Initialize the n_core_null_bytes on first access to a clustered
index root page.
@param[in]	index	clustered index that is on its first access
@param[in]	page	clustered index root page
@return	whether the page is corrupted */
bool
btr_cur_instant_root_init(dict_index_t* index, const page_t* page)
	ATTRIBUTE_COLD __attribute__((nonnull, warn_unused_result));

MY_ATTRIBUTE((warn_unused_result))
/********************************************************************//**
Searches an index tree and positions a tree cursor on a given non-leaf level.
NOTE: n_fields_cmp in tuple must be set so that it cannot be compared
to node pointer page number fields on the upper levels of the tree!
cursor->up_match and cursor->low_match both will have sensible values.
Cursor is left at the place where an insert of the
search tuple should be performed in the B-tree. InnoDB does an insert
immediately after the cursor. Thus, the cursor may end up on a user record,
or on a page infimum record.
@param level      the tree level of search
@param tuple      data tuple; NOTE: n_fields_cmp in tuple must be set so that
                  it cannot get compared to the node ptr page number field!
@param latch      RW_S_LATCH or RW_X_LATCH
@param cursor     tree cursor; the cursor page is s- or x-latched, but see also
                  above!
@param mtr        mini-transaction
@return DB_SUCCESS on success or error code otherwise */
dberr_t btr_cur_search_to_nth_level(ulint level,
                                    const dtuple_t *tuple,
                                    rw_lock_type_t rw_latch,
                                    btr_cur_t *cursor, mtr_t *mtr);

/*************************************************************//**
Tries to perform an insert to a page in an index tree, next to cursor.
It is assumed that mtr holds an x-latch on the page. The operation does
not succeed if there is too little space on the page. If there is just
one record on the page, the insert will always succeed; this is to
prevent trying to split a page with just one record.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_FAIL, or error number */
dberr_t
btr_cur_optimistic_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags: if not
				zero, the parameters index and thr should be
				specified */
	btr_cur_t*	cursor,	/*!< in: cursor on page after which to insert;
				cursor stays valid */
	rec_offs**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap */
	dtuple_t*	entry,	/*!< in/out: entry to insert */
	rec_t**		rec,	/*!< out: pointer to inserted record if
				succeed */
	big_rec_t**	big_rec,/*!< out: big rec vector whose fields have to
				be stored externally by the caller */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	que_thr_t*	thr,	/*!< in/out: query thread; can be NULL if
				!(~flags
				& (BTR_NO_LOCKING_FLAG
				| BTR_NO_UNDO_LOG_FLAG)) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction;
				if this function returns DB_SUCCESS on
				a leaf page of a secondary index in a
				compressed tablespace, the caller must
				mtr_commit(mtr) before latching
				any further pages */
	MY_ATTRIBUTE((nonnull(2,3,4,5,6,7,10), warn_unused_result));
/*************************************************************//**
Performs an insert on a page of an index tree. It is assumed that mtr
holds an x-latch on the tree and on the cursor page. If the insert is
made on the leaf level, to avoid deadlocks, mtr must also own x-latches
to brothers of page, if those brothers exist.
@return DB_SUCCESS or error number */
dberr_t
btr_cur_pessimistic_insert(
/*=======================*/
	ulint		flags,	/*!< in: undo logging and locking flags: if not
				zero, the parameter thr should be
				specified; if no undo logging is specified,
				then the caller must have reserved enough
				free extents in the file space so that the
				insertion will certainly succeed */
	btr_cur_t*	cursor,	/*!< in: cursor after which to insert;
				cursor stays valid */
	rec_offs**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap
				that can be emptied */
	dtuple_t*	entry,	/*!< in/out: entry to insert */
	rec_t**		rec,	/*!< out: pointer to inserted record if
				succeed */
	big_rec_t**	big_rec,/*!< out: big rec vector whose fields have to
				be stored externally by the caller */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	que_thr_t*	thr,	/*!< in/out: query thread; can be NULL if
				!(~flags
				& (BTR_NO_LOCKING_FLAG
				| BTR_NO_UNDO_LOG_FLAG)) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull(2,3,4,5,6,7,10), warn_unused_result));
/*************************************************************//**
See if there is enough place in the page modification log to log
an update-in-place.

@retval false if out of space
@retval true if enough place */
bool
btr_cur_update_alloc_zip_func(
/*==========================*/
	page_zip_des_t*	page_zip,/*!< in/out: compressed page */
	page_cur_t*	cursor,	/*!< in/out: B-tree page cursor */
#ifdef UNIV_DEBUG
	rec_offs*	offsets,/*!< in/out: offsets of the cursor record */
#endif /* UNIV_DEBUG */
	ulint		length,	/*!< in: size needed */
	bool		create,	/*!< in: true=delete-and-insert,
				false=update-in-place */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
#ifdef UNIV_DEBUG
# define btr_cur_update_alloc_zip(page_zip,cursor,offsets,len,cr,mtr) \
	btr_cur_update_alloc_zip_func(page_zip,cursor,offsets,len,cr,mtr)
#else /* UNIV_DEBUG */
# define btr_cur_update_alloc_zip(page_zip,cursor,offsets,len,cr,mtr) \
	btr_cur_update_alloc_zip_func(page_zip,cursor,len,cr,mtr)
#endif /* UNIV_DEBUG */

/** Apply an update vector to a record. No field size changes are allowed.

This is usually invoked on a clustered index. The only use case for a
secondary index is row_ins_sec_index_entry_by_modify() or its
counterpart in ibuf_insert_to_index_page().
@param[in,out]  rec     index record
@param[in]      index   the index of the record
@param[in]      offsets rec_get_offsets(rec, index)
@param[in]      update  update vector
@param[in,out]  block   index page
@param[in,out]  mtr     mini-transaction */
void btr_cur_upd_rec_in_place(rec_t *rec, const dict_index_t *index,
                              const rec_offs *offsets, const upd_t *update,
                              buf_block_t *block, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));
/*************************************************************//**
Updates a record when the update causes no size changes in its fields.
@return locking or undo log related error code, or
@retval DB_SUCCESS on success
@retval DB_ZIP_OVERFLOW if there is not enough space left
on a ROW_FORMAT=COMPRESSED page */
dberr_t
btr_cur_update_in_place(
/*====================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor on the record to update;
				cursor stays valid and positioned on the
				same record */
	rec_offs*	offsets,/*!< in/out: offsets on cursor->page_cur.rec */
	const upd_t*	update,	/*!< in: update vector */
	ulint		cmpl_info,/*!< in: compiler info on secondary index
				updates */
	que_thr_t*	thr,	/*!< in: query thread */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction; if this
				is a secondary index, the caller must
				mtr_commit(mtr) before latching any
				further pages */
	MY_ATTRIBUTE((warn_unused_result, nonnull));
/*************************************************************//**
Tries to update a record on a page in an index tree. It is assumed that mtr
holds an x-latch on the page. The operation does not succeed if there is too
little space on the page or if the update would result in too empty a page,
so that tree compression is recommended.
@return error code, including
@retval DB_SUCCESS on success
@retval DB_OVERFLOW if the updated record does not fit
@retval DB_UNDERFLOW if the page would become too empty
@retval DB_ZIP_OVERFLOW if there is not enough space left
on the compressed page */
dberr_t
btr_cur_optimistic_update(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor on the record to update;
				cursor stays valid and positioned on the
				same record */
	rec_offs**	offsets,/*!< out: offsets on cursor->page_cur.rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to NULL or memory heap */
	const upd_t*	update,	/*!< in: update vector; this must also
				contain trx id and roll ptr fields */
	ulint		cmpl_info,/*!< in: compiler info on secondary index
				updates */
	que_thr_t*	thr,	/*!< in: query thread */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction; if this
				is a secondary index, the caller must
				mtr_commit(mtr) before latching any
				further pages */
	MY_ATTRIBUTE((warn_unused_result, nonnull));
/*************************************************************//**
Performs an update of a record on a page of a tree. It is assumed
that mtr holds an x-latch on the tree and on the cursor page. If the
update is made on the leaf level, to avoid deadlocks, mtr must also
own x-latches to brothers of page, if those brothers exist.
@return DB_SUCCESS or error code */
dberr_t
btr_cur_pessimistic_update(
/*=======================*/
	ulint		flags,	/*!< in: undo logging, locking, and rollback
				flags */
	btr_cur_t*	cursor,	/*!< in/out: cursor on the record to update;
				cursor may become invalid if *big_rec == NULL
				|| !(flags & BTR_KEEP_POS_FLAG) */
	rec_offs**	offsets,/*!< out: offsets on cursor->page_cur.rec */
	mem_heap_t**	offsets_heap,
				/*!< in/out: pointer to memory heap
				that can be emptied */
	mem_heap_t*	entry_heap,
				/*!< in/out: memory heap for allocating
				big_rec and the index tuple */
	big_rec_t**	big_rec,/*!< out: big rec vector whose fields have to
				be stored externally by the caller */
	upd_t*		update,	/*!< in/out: update vector; this is allowed to
				also contain trx id and roll ptr fields.
				Non-updated columns that are moved offpage will
				be appended to this. */
	ulint		cmpl_info,/*!< in: compiler info on secondary index
				updates */
	que_thr_t*	thr,	/*!< in: query thread */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction; must be committed
				before latching any further pages */
	MY_ATTRIBUTE((warn_unused_result, nonnull));
/***********************************************************//**
Marks a clustered index record deleted. Writes an undo log record to
undo log on this delete marking. Writes in the trx id field the id
of the deleting transaction, and in the roll ptr field pointer to the
undo log record created.
@return DB_SUCCESS, DB_LOCK_WAIT, or error number */
dberr_t
btr_cur_del_mark_set_clust_rec(
/*===========================*/
	buf_block_t*	block,	/*!< in/out: buffer block of the record */
	rec_t*		rec,	/*!< in/out: record */
	dict_index_t*	index,	/*!< in: clustered index of the record */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec) */
	que_thr_t*	thr,	/*!< in: query thread */
	const dtuple_t*	entry,	/*!< in: dtuple for the deleting record */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*************************************************************//**
Tries to compress a page of the tree if it seems useful. It is assumed
that mtr holds an x-latch on the tree and on the cursor page. To avoid
deadlocks, mtr must also own x-latches to brothers of page, if those
brothers exist. NOTE: it is assumed that the caller has reserved enough
free extents so that the compression will always succeed if done!
@return whether compression occurred */
bool
btr_cur_compress_if_useful(
/*=======================*/
	btr_cur_t*	cursor,	/*!< in/out: cursor on the page to compress;
				cursor does not stay valid if !adjust and
				compression occurs */
	bool		adjust,	/*!< in: whether the cursor position should be
				adjusted even when compression occurs */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull));
/*******************************************************//**
Removes the record on which the tree cursor is positioned. It is assumed
that the mtr has an x-latch on the page where the cursor is positioned,
but no latch on the whole tree.
@return error code
@retval DB_FAIL if the page would become too empty */
dberr_t
btr_cur_optimistic_delete(
/*======================*/
	btr_cur_t*	cursor,	/*!< in: cursor on the record to delete;
				cursor stays valid: if deletion succeeds,
				on function exit it points to the successor
				of the deleted record */
	ulint		flags,	/*!< in: BTR_CREATE_FLAG or 0 */
	mtr_t*		mtr)	/*!< in: mtr; if this function returns
				TRUE on a leaf page of a secondary
				index, the mtr must be committed
				before latching any further pages */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*************************************************************//**
Removes the record on which the tree cursor is positioned. Tries
to compress the page if its fillfactor drops below a threshold
or if it is the only page on the level. It is assumed that mtr holds
an x-latch on the tree and on the cursor page. To avoid deadlocks,
mtr must also own x-latches to brothers of page, if those brothers
exist.
@return TRUE if compression occurred */
ibool
btr_cur_pessimistic_delete(
/*=======================*/
	dberr_t*		err,	/*!< out: DB_SUCCESS or DB_OUT_OF_FILE_SPACE;
				the latter may occur because we may have
				to update node pointers on upper levels,
				and in the case of variable length keys
				these may actually grow in size */
	ibool		has_reserved_extents, /*!< in: TRUE if the
				caller has already reserved enough free
				extents so that he knows that the operation
				will succeed */
	btr_cur_t*	cursor,	/*!< in: cursor on the record to delete;
				if compression does not occur, the cursor
				stays valid: it points to successor of
				deleted record on function exit */
	ulint		flags,	/*!< in: BTR_CREATE_FLAG or 0 */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull));
/** Delete the node pointer in a parent page.
@param[in,out]	parent	cursor pointing to parent record
@param[in,out]	mtr	mini-transaction */
dberr_t btr_cur_node_ptr_delete(btr_cur_t* parent, mtr_t* mtr)
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************//**
Parses a redo log record of updating a record in-place.
@return end of log record or NULL */
byte*
btr_cur_parse_update_in_place(
/*==========================*/
	byte*		ptr,	/*!< in: buffer */
	byte*		end_ptr,/*!< in: buffer end */
	page_t*		page,	/*!< in/out: page or NULL */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page, or NULL */
	dict_index_t*	index);	/*!< in: index corresponding to page */
/** Arguments to btr_estimate_n_rows_in_range */
struct btr_pos_t
{
  btr_pos_t(dtuple_t *arg_tuple,
            page_cur_mode_t arg_mode,
            page_id_t arg_page_id)
  :tuple(arg_tuple), mode(arg_mode), page_id(arg_page_id)
  {}

  dtuple_t*       tuple;       /* Range start or end. May be NULL */
  page_cur_mode_t mode;        /* search mode for range */
  page_id_t       page_id;     /* Out: Page where we found the tuple */
};

/** Estimates the number of rows in a given index range. Do search in the
left page, then if there are pages between left and right ones, read a few
pages to the right, if the right page is reached, fetch it and count the exact
number of rows, otherwise count the estimated(see
btr_estimate_n_rows_in_range_on_level() for details) number if rows, and
fetch the right page. If leaves are reached, unlatch non-leaf pages except
the right leaf parent. After the right leaf page is fetched, commit mtr.
@param[in]  index index
@param[in]  range_start range start
@param[in]  range_end   range end
@return estimated number of rows; */
ha_rows btr_estimate_n_rows_in_range(dict_index_t *index,
                                     btr_pos_t *range_start,
                                     btr_pos_t *range_end);

/** Gets the externally stored size of a record, in units of a database page.
@param[in]	rec	record
@param[in]	offsets	array returned by rec_get_offsets()
@return externally stored part, in units of a database page */
ulint
btr_rec_get_externally_stored_len(
	const rec_t*	rec,
	const rec_offs*	offsets);

/*******************************************************************//**
Marks non-updated off-page fields as disowned by this record. The ownership
must be transferred to the updated record which is inserted elsewhere in the
index tree. In purge only the owner of externally stored field is allowed
to free the field. */
void
btr_cur_disown_inherited_fields(
/*============================*/
	buf_block_t*	block,	/*!< in/out: index page */
	rec_t*		rec,	/*!< in/out: record in a clustered index */
	dict_index_t*	index,	/*!< in: index of the page */
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
	const upd_t*	update,	/*!< in: update vector */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull(2,3,4,5,6)));

/** Operation code for btr_store_big_rec_extern_fields(). */
enum blob_op {
	/** Store off-page columns for a freshly inserted record */
	BTR_STORE_INSERT = 0,
	/** Store off-page columns for an insert by update */
	BTR_STORE_INSERT_UPDATE,
	/** Store off-page columns for an update */
	BTR_STORE_UPDATE,
	/** Store off-page columns for a freshly inserted record by bulk */
	BTR_STORE_INSERT_BULK
};

/*******************************************************************//**
Determine if an operation on off-page columns is an update.
@return TRUE if op != BTR_STORE_INSERT */
UNIV_INLINE
ibool
btr_blob_op_is_update(
/*==================*/
	enum blob_op	op)	/*!< in: operation */
	MY_ATTRIBUTE((warn_unused_result));

/*******************************************************************//**
Stores the fields in big_rec_vec to the tablespace and puts pointers to
them in rec.  The extern flags in rec will have to be set beforehand.
The fields are stored on pages allocated from leaf node
file segment of the index tree.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
dberr_t
btr_store_big_rec_extern_fields(
/*============================*/
	btr_pcur_t*	pcur,		/*!< in: a persistent cursor */
	rec_offs*	offsets,	/*!< in/out: rec_get_offsets() on
					pcur. the "external storage" flags
					in offsets will correctly correspond
					to rec when this function returns */
	const big_rec_t*big_rec_vec,	/*!< in: vector containing fields
					to be stored externally */
	mtr_t*		btr_mtr,	/*!< in/out: mtr containing the
					latches to the clustered index. can be
					committed and restarted. */
	enum blob_op	op)		/*! in: operation code */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*******************************************************************//**
Frees the space in an externally stored field to the file space
management if the field in data is owned the externally stored field,
in a rollback we may have the additional condition that the field must
not be inherited. */
void
btr_free_externally_stored_field(
/*=============================*/
	dict_index_t*	index,		/*!< in: index of the data, the index
					tree MUST be X-latched; if the tree
					height is 1, then also the root page
					must be X-latched! (this is relevant
					in the case this function is called
					from purge where 'data' is located on
					an undo log page, not an index
					page) */
	byte*		field_ref,	/*!< in/out: field reference */
	const rec_t*	rec,		/*!< in: record containing field_ref, for
					page_zip_write_blob_ptr(), or NULL */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index),
					or NULL */
	buf_block_t*	block,		/*!< in/out: page of field_ref */
	ulint		i,		/*!< in: field number of field_ref;
					ignored if rec == NULL */
	bool		rollback,	/*!< in: performing rollback? */
	mtr_t*		local_mtr)	/*!< in: mtr containing the latch */
	MY_ATTRIBUTE((nonnull(1,2,5,8)));

/** Copies the prefix of an externally stored field of a record.
The clustered index record must be protected by a lock or a page latch.
@param[out]	buf		the field, or a prefix of it
@param[in]	len		length of buf, in bytes
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	data		'internally' stored part of the field
containing also the reference to the external part; must be protected by
a lock or a page latch
@param[in]	local_len	length of data, in bytes
@return the length of the copied field, or 0 if the column was being
or has been deleted */
ulint
btr_copy_externally_stored_field_prefix(
	byte*			buf,
	ulint			len,
	ulint			zip_size,
	const byte*		data,
	ulint			local_len);

/** Copies an externally stored field of a record to mem heap.
The clustered index record must be protected by a lock or a page latch.
@param[out]	len		length of the whole field
@param[in]	data		'internally' stored part of the field
containing also the reference to the external part; must be protected by
a lock or a page latch
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	local_len	length of data
@param[in,out]	heap		mem heap
@return the whole field copied to heap */
byte*
btr_copy_externally_stored_field(
	ulint*			len,
	const byte*		data,
	ulint			zip_size,
	ulint			local_len,
	mem_heap_t*		heap);

/** Copies an externally stored field of a record to mem heap.
@param[in]	rec		record in a clustered index; must be
protected by a lock or a page latch
@param[in]	offset		array returned by rec_get_offsets()
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	no		field number
@param[out]	len		length of the field
@param[in,out]	heap		mem heap
@return the field copied to heap, or NULL if the field is incomplete */
byte*
btr_rec_copy_externally_stored_field(
	const rec_t*		rec,
	const rec_offs*		offsets,
	ulint			zip_size,
	ulint			no,
	ulint*			len,
	mem_heap_t*		heap);

/*######################################################################*/

/** In the pessimistic delete, if the page data size drops below this
limit, merging it to a neighbor is tried */
#define BTR_CUR_PAGE_COMPRESS_LIMIT(index) \
	((srv_page_size * (ulint)((index)->merge_threshold)) / 100)

/** A slot in the path array. We store here info on a search path down the
tree. Each slot contains data on a single level of the tree. */
struct btr_path_t {
	/* Assume a page like:
	records:             (inf, a, b, c, d, sup)
	index of the record:    0, 1, 2, 3, 4, 5
	*/

	/** Index of the record where the page cursor stopped on this level
	(index in alphabetical order). Value ULINT_UNDEFINED denotes array
	end. In the above example, if the search stopped on record 'c', then
	nth_rec will be 3. */
	ulint	nth_rec;

	/** Number of the records on the page, not counting inf and sup.
	In the above example n_recs will be 4. */
	ulint	n_recs;

	/** Number of the page containing the record. */
	uint32_t page_no;

	/** Level of the page. If later we fetch the page under page_no
	and it is no different level then we know that the tree has been
	reorganized. */
	ulint	page_level;
};

#define BTR_PATH_ARRAY_N_SLOTS	250	/*!< size of path array (in slots) */

/** Values for the flag documenting the used search method */
enum btr_cur_method {
	BTR_CUR_HASH = 1,	/*!< successful shortcut using
				the hash index */
	BTR_CUR_HASH_FAIL,	/*!< failure using hash, success using
				binary search: the misleading hash
				reference is stored in the field
				hash_node, and might be necessary to
				update */
	BTR_CUR_BINARY		/*!< success using the binary search */
};

/** The tree cursor: the definition appears here only for the compiler
to know struct size! */
struct btr_cur_t {
	page_cur_t	page_cur;	/*!< page cursor */
	/*------------------------------*/
	/** The following fields are used in
	search_leaf() to pass information: */
	/* @{ */
	enum btr_cur_method	flag;	/*!< Search method used */
	ulint		tree_height;	/*!< Tree height if the search is done
					for a pessimistic insert or update
					operation */
	ulint		up_match;	/*!< If the search mode was PAGE_CUR_LE,
					the number of matched fields to the
					the first user record to the right of
					the cursor record after search_leaf();
					for the mode PAGE_CUR_GE, the matched
					fields to the first user record AT THE
					CURSOR or to the right of it;
					NOTE that the up_match and low_match
					values may exceed the correct values
					for comparison to the adjacent user
					record if that record is on a
					different leaf page! (See the note in
					row_ins_duplicate_error_in_clust.) */
	ulint		up_bytes;	/*!< number of matched bytes to the
					right at the time cursor positioned;
					only used internally in searches: not
					defined after the search */
	ulint		low_match;	/*!< if search mode was PAGE_CUR_LE,
					the number of matched fields to the
					first user record AT THE CURSOR or
					to the left of it after search_leaf();
					NOT defined for PAGE_CUR_GE or any
					other search modes; see also the NOTE
					in up_match! */
	ulint		low_bytes;	/*!< number of matched bytes to the
					left at the time cursor positioned;
					only used internally in searches: not
					defined after the search */
	ulint		n_fields;	/*!< prefix length used in a hash
					search if hash_node != NULL */
	ulint		n_bytes;	/*!< hash prefix bytes if hash_node !=
					NULL */
	ulint		fold;		/*!< fold value used in the search if
					flag is BTR_CUR_HASH */
	/* @} */
	btr_path_t*	path_arr;	/*!< in estimating the number of
					rows in range, we store in this array
					information of the path through
					the tree */
	rtr_info_t*	rtr_info;	/*!< rtree search info */
  btr_cur_t() { memset((void*) this, 0, sizeof *this); }

  dict_index_t *index() const { return page_cur.index; }
  buf_block_t *block() const { return page_cur.block; }

  /** Open the cursor on the first or last record.
  @param first         true=first record, false=last record
  @param index         B-tree
  @param latch_mode    which latches to acquire
  @param mtr           mini-transaction
  @return error code */
  dberr_t open_leaf(bool first, dict_index_t *index, btr_latch_mode latch_mode,
                    mtr_t *mtr);

  /** Search the leaf page record corresponding to a key.
  @param tuple      key to search for, with correct n_fields_cmp
  @param mode       search mode; PAGE_CUR_LE for unique prefix or for inserting
  @param latch_mode latch mode
  @param mtr        mini-transaction
  @return error code */
  dberr_t search_leaf(const dtuple_t *tuple, page_cur_mode_t mode,
                      btr_latch_mode latch_mode, mtr_t *mtr);

  /** Search the leaf page record corresponding to a key, exclusively latching
  all sibling pages on the way.
  @param tuple      key to search for, with correct n_fields_cmp
  @param mode       search mode; PAGE_CUR_LE for unique prefix or for inserting
  @param mtr        mini-transaction
  @return error code */
  dberr_t pessimistic_search_leaf(const dtuple_t *tuple, page_cur_mode_t mode,
                                  mtr_t *mtr);

  /** Open the cursor at a random leaf page record.
  @param offsets   temporary memory for rec_get_offsets()
  @param heap      memory heap for rec_get_offsets()
  @param mtr       mini-transaction
  @return error code */
  inline dberr_t open_random_leaf(rec_offs *&offsets, mem_heap_t *& heap,
                                  mtr_t &mtr);
};

/** Modify the delete-mark flag of a record.
@tparam         flag    the value of the delete-mark flag
@param[in,out]  block   buffer block
@param[in,out]  rec     record on a physical index page
@param[in,out]  mtr     mini-transaction  */
template<bool flag>
void btr_rec_set_deleted(buf_block_t *block, rec_t *rec, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));

/** If pessimistic delete fails because of lack of file space, there
is still a good change of success a little later.  Try this many
times. */
#define BTR_CUR_RETRY_DELETE_N_TIMES	100
/** If pessimistic delete fails because of lack of file space, there
is still a good change of success a little later.  Sleep this time
between retries. */
static const std::chrono::milliseconds BTR_CUR_RETRY_SLEEP_TIME(50);

/** The reference in a field for which data is stored on a different page.
The reference is at the end of the 'locally' stored part of the field.
'Locally' means storage in the index record.
We store locally a long enough prefix of each column so that we can determine
the ordering parts of each index record without looking into the externally
stored part. */
/*-------------------------------------- @{ */
#define BTR_EXTERN_SPACE_ID		0U	/*!< space id where stored */
#define BTR_EXTERN_PAGE_NO		4U	/*!< page no where stored */
#define BTR_EXTERN_OFFSET		8U	/*!< offset of BLOB header
						on that page */
#define BTR_EXTERN_LEN			12U	/*!< 8 bytes containing the
						length of the externally
						stored part of the BLOB.
						The 2 highest bits are
						reserved to the flags below. */
/*-------------------------------------- @} */
/* #define BTR_EXTERN_FIELD_REF_SIZE	20 // moved to btr0types.h */

/** The most significant bit of BTR_EXTERN_LEN (i.e., the most
significant bit of the byte at smallest address) is set to 1 if this
field does not 'own' the externally stored field; only the owner field
is allowed to free the field in purge! */
#define BTR_EXTERN_OWNER_FLAG		128U
/** If the second most significant bit of BTR_EXTERN_LEN (i.e., the
second most significant bit of the byte at smallest address) is 1 then
it means that the externally stored field was inherited from an
earlier version of the row.  In rollback we are not allowed to free an
inherited external field. */
#define BTR_EXTERN_INHERITED_FLAG	64U

#ifdef BTR_CUR_HASH_ADAPT
/** Number of searches down the B-tree in btr_cur_t::search_leaf(). */
extern ib_counter_t<ulint, ib_counter_element_t>	btr_cur_n_non_sea;
/** Old value of btr_cur_n_non_sea.  Copied by
srv_refresh_innodb_monitor_stats().  Referenced by
srv_printf_innodb_monitor(). */
extern ulint	btr_cur_n_non_sea_old;
/** Number of successful adaptive hash index lookups in
btr_cur_t::search_leaf(). */
extern ib_counter_t<ulint, ib_counter_element_t>	btr_cur_n_sea;
/** Old value of btr_cur_n_sea.  Copied by
srv_refresh_innodb_monitor_stats().  Referenced by
srv_printf_innodb_monitor(). */
extern ulint	btr_cur_n_sea_old;
#endif /* BTR_CUR_HASH_ADAPT */

#ifdef UNIV_DEBUG
/* Flag to limit optimistic insert records */
extern uint	btr_cur_limit_optimistic_insert_debug;
#endif /* UNIV_DEBUG */

#include "btr0cur.inl"

#endif
