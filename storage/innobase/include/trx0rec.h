/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file include/trx0rec.h
Transaction undo log record

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#ifndef trx0rec_h
#define trx0rec_h

#include "trx0types.h"
#include "row0types.h"
#include "mtr0mtr.h"
#include "rem0types.h"
#include "page0types.h"
#include "row0log.h"
#include "que0types.h"

/***********************************************************************//**
Copies the undo record to the heap.
@return own: copy of undo log record */
UNIV_INLINE
trx_undo_rec_t*
trx_undo_rec_copy(
/*==============*/
	const trx_undo_rec_t*	undo_rec,	/*!< in: undo log record */
	mem_heap_t*		heap);		/*!< in: heap where copied */
/**********************************************************************//**
Reads the undo log record type.
@return record type */
UNIV_INLINE
ulint
trx_undo_rec_get_type(
/*==================*/
	const trx_undo_rec_t*	undo_rec);	/*!< in: undo log record */
/**********************************************************************//**
Reads the undo log record number.
@return undo no */
UNIV_INLINE
undo_no_t
trx_undo_rec_get_undo_no(
/*=====================*/
	const trx_undo_rec_t*	undo_rec);	/*!< in: undo log record */

/**********************************************************************//**
Returns the start of the undo record data area. */
#define trx_undo_rec_get_ptr(undo_rec, undo_no)		\
	((undo_rec) + trx_undo_rec_get_offset(undo_no))

/**********************************************************************//**
Reads from an undo log record the general parameters.
@return remaining part of undo log record after reading these values */
byte*
trx_undo_rec_get_pars(
/*==================*/
	trx_undo_rec_t*	undo_rec,	/*!< in: undo log record */
	ulint*		type,		/*!< out: undo record type:
					TRX_UNDO_INSERT_REC, ... */
	ulint*		cmpl_info,	/*!< out: compiler info, relevant only
					for update type records */
	bool*		updated_extern,	/*!< out: true if we updated an
					externally stored fild */
	undo_no_t*	undo_no,	/*!< out: undo log record number */
	table_id_t*	table_id)	/*!< out: table id */
	MY_ATTRIBUTE((nonnull));

/*******************************************************************//**
Builds a row reference from an undo log record.
@return pointer to remaining part of undo record */
byte*
trx_undo_rec_get_row_ref(
/*=====================*/
	byte*		ptr,	/*!< in: remaining part of a copy of an undo log
				record, at the start of the row reference;
				NOTE that this copy of the undo log record must
				be preserved as long as the row reference is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	const dtuple_t**ref,	/*!< out, own: row reference */
	mem_heap_t*	heap);	/*!< in: memory heap from which the memory
				needed is allocated */
/**********************************************************************//**
Reads from an undo log update record the system field values of the old
version.
@return remaining part of undo log record after reading these values */
byte*
trx_undo_update_rec_get_sys_cols(
/*=============================*/
	const byte*	ptr,		/*!< in: remaining part of undo
					log record after reading
					general parameters */
	trx_id_t*	trx_id,		/*!< out: trx id */
	roll_ptr_t*	roll_ptr,	/*!< out: roll ptr */
	byte*		info_bits);	/*!< out: info bits state */
/*******************************************************************//**
Builds an update vector based on a remaining part of an undo log record.
@return remaining part of the record, NULL if an error detected, which
means that the record is corrupted */
byte*
trx_undo_update_rec_get_update(
/*===========================*/
	const byte*	ptr,	/*!< in: remaining part in update undo log
				record, after reading the row reference
				NOTE that this copy of the undo log record must
				be preserved as long as the update vector is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	ulint		type,	/*!< in: TRX_UNDO_UPD_EXIST_REC,
				TRX_UNDO_UPD_DEL_REC, or
				TRX_UNDO_DEL_MARK_REC; in the last case,
				only trx id and roll ptr fields are added to
				the update vector */
	trx_id_t	trx_id,	/*!< in: transaction id from this undorecord */
	roll_ptr_t	roll_ptr,/*!< in: roll pointer from this undo record */
	byte		info_bits,/*!< in: info bits from this undo record */
	mem_heap_t*	heap,	/*!< in: memory heap from which the memory
				needed is allocated */
	upd_t**		upd);	/*!< out, own: update vector */
/*******************************************************************//**
Builds a partial row from an update undo log record, for purge.
It contains the columns which occur as ordering in any index of the table.
Any missing columns are indicated by col->mtype == DATA_MISSING.
@return pointer to remaining part of undo record */
byte*
trx_undo_rec_get_partial_row(
/*=========================*/
	const byte*	ptr,	/*!< in: remaining part in update undo log
				record of a suitable type, at the start of
				the stored index columns;
				NOTE that this copy of the undo log record must
				be preserved as long as the partial row is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	const upd_t*	update,	/*!< in: updated columns */
	dtuple_t**	row,	/*!< out, own: partial row */
	ibool		ignore_prefix, /*!< in: flag to indicate if we
				expect blob prefixes in undo. Used
				only in the assertion. */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory
				needed is allocated */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Report a RENAME TABLE operation.
@param[in,out]	trx	transaction
@param[in]	table	table that is being renamed
@return	DB_SUCCESS or error code */
dberr_t trx_undo_report_rename(trx_t* trx, const dict_table_t* table)
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************************//**
Writes information to an undo log about an insert, update, or a delete marking
of a clustered index record. This information is used in a rollback of the
transaction and in consistent reads that must look to the history of this
transaction.
@return DB_SUCCESS or error code */
dberr_t
trx_undo_report_row_operation(
/*==========================*/
	que_thr_t*	thr,		/*!< in: query thread */
	dict_index_t*	index,		/*!< in: clustered index */
	const dtuple_t*	clust_entry,	/*!< in: in the case of an insert,
					index entry to insert into the
					clustered index; in updates,
					may contain a clustered index
					record tuple that also contains
					virtual columns of the table;
					otherwise, NULL */
	const upd_t*	update,		/*!< in: in the case of an update,
					the update vector, otherwise NULL */
	ulint		cmpl_info,	/*!< in: compiler info on secondary
					index updates */
	const rec_t*	rec,		/*!< in: case of an update or delete
					marking, the record in the clustered
					index; NULL if insert */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec) */
	roll_ptr_t*	roll_ptr)	/*!< out: DB_ROLL_PTR to the
					undo log record */
	MY_ATTRIBUTE((nonnull(1,2), warn_unused_result));

/** status bit used for trx_undo_prev_version_build() */

/** TRX_UNDO_PREV_IN_PURGE tells trx_undo_prev_version_build() that it
is being called purge view and we would like to get the purge record
even it is in the purge view (in normal case, it will return without
fetching the purge record */
#define		TRX_UNDO_PREV_IN_PURGE		0x1

/** This tells trx_undo_prev_version_build() to fetch the old value in
the undo log (which is the after image for an update) */
#define		TRX_UNDO_GET_OLD_V_VALUE	0x2

/** Build a previous version of a clustered index record. The caller
must hold a latch on the index page of the clustered index record.
@param	index_rec	clustered index record in the index tree
@param	index_mtr	mtr which contains the latch to index_rec page
			and purge_view
@param	rec		version of a clustered index record
@param	index		clustered index
@param	offsets		rec_get_offsets(rec, index)
@param	heap		memory heap from which the memory needed is
			allocated
@param	old_vers	previous version or NULL if rec is the
			first inserted version, or if history data
			has been deleted (an error), or if the purge
			could have removed the version
			though it has not yet done so
@param	v_heap		memory heap used to create vrow
			dtuple if it is not yet created. This heap
                        diffs from "heap" above in that it could be
                        prebuilt->old_vers_heap for selection
@param	vrow		virtual column info, if any
@param	v_status	status determine if it is going into this
			function by purge thread or not.
			And if we read "after image" of undo log
@retval true if previous version was built, or if it was an insert
or the table has been rebuilt
@retval false if the previous version is earlier than purge_view,
or being purged, which means that it may have been removed */
bool
trx_undo_prev_version_build(
	const rec_t	*index_rec,
	mtr_t		*index_mtr,
	const rec_t 	*rec,
	dict_index_t	*index,
	rec_offs	*offsets,
	mem_heap_t	*heap,
	rec_t		**old_vers,
	mem_heap_t	*v_heap,
	dtuple_t	**vrow,
	ulint		v_status);

/** Read from an undo log record a non-virtual column value.
@param[in,out]	ptr		pointer to remaining part of the undo record
@param[in,out]	field		stored field
@param[in,out]	len		length of the field, or UNIV_SQL_NULL
@param[in,out]	orig_len	original length of the locally stored part
of an externally stored column, or 0
@return remaining part of undo log record after reading these values */
byte *trx_undo_rec_get_col_val(const byte *ptr, const byte **field,
                               uint32_t *len, uint32_t *orig_len);

/** Read virtual column value from undo log
@param[in]	table		the table
@param[in]	ptr		undo log pointer
@param[in,out]	row		the dtuple to fill
@param[in]	in_purge	whether this is called by purge */
void
trx_undo_read_v_cols(
	const dict_table_t*	table,
	const byte*		ptr,
	dtuple_t*		row,
	bool			in_purge);

/** Read virtual column index from undo log if the undo log contains such
info, and verify the column is still indexed, and output its position
@param[in]	table		the table
@param[in]	ptr		undo log pointer
@param[in]	first_v_col	if this is the first virtual column, which
				has the version marker
@param[in,out]	is_undo_log	his function is used to parse both undo log,
				and online log for virtual columns. So
				check to see if this is undo log
@param[out]	field_no	the column number, or FIL_NULL if not indexed
@return remaining part of undo log record after reading these values */
const byte*
trx_undo_read_v_idx(
	const dict_table_t*	table,
	const byte*		ptr,
	bool			first_v_col,
	bool*			is_undo_log,
	uint32_t*		field_no);

/* Types of an undo log record: these have to be smaller than 16, as the
compilation info multiplied by 16 is ORed to this value in an undo log
record */

/** Undo log records for DDL operations

Note: special rollback and purge triggers exist for SYS_INDEXES records:
@see dict_drop_index_tree() */
enum trx_undo_ddl_type
{
  /** RENAME TABLE (logging the old table name).

  Because SYS_TABLES has PRIMARY KEY(NAME), the row-level undo log records
  for SYS_TABLES cannot be distinguished from DROP TABLE, CREATE TABLE. */
  TRX_UNDO_RENAME_TABLE= 9,
  /** insert a metadata pseudo-record for instant ALTER TABLE */
  TRX_UNDO_INSERT_METADATA= 10
};

/* DML operations */
#define	TRX_UNDO_INSERT_REC	11	/* fresh insert into clustered index */
#define	TRX_UNDO_UPD_EXIST_REC	12	/* update of a non-delete-marked
					record */
#define	TRX_UNDO_UPD_DEL_REC	13	/* update of a delete marked record to
					a not delete marked record; also the
					fields of the record can change */
#define	TRX_UNDO_DEL_MARK_REC	14	/* delete marking of a record; fields
					do not change */
/** Bulk insert operation. It is written only when the table is
under exclusive lock and the clustered index root page latch is being held,
and the clustered index is empty. Rollback will empty the table and
free the leaf segment of all indexes, re-create the new
leaf segment and re-initialize the root page alone. */
#define	TRX_UNDO_EMPTY		15

#define	TRX_UNDO_CMPL_INFO_MULT	16U	/* compilation info is multiplied by
					this and ORed to the type above */
#define	TRX_UNDO_UPD_EXTERN	128U	/* This bit can be ORed to type_cmpl
					to denote that we updated external
					storage fields: used by purge to
					free the external storage */

/** The search tuple corresponding to TRX_UNDO_INSERT_METADATA */
extern const dtuple_t trx_undo_metadata;

/** Read the table id from an undo log record.
@param[in]      rec        Undo log record
@return table id stored as a part of undo log record */
inline table_id_t trx_undo_rec_get_table_id(const trx_undo_rec_t *rec)
{
  rec+= 3;
  mach_read_next_much_compressed(&rec);
  return mach_read_next_much_compressed(&rec);
}

#include "trx0rec.inl"

#endif /* trx0rec_h */
