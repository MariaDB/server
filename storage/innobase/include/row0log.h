/*****************************************************************************

Copyright (c) 2011, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/row0log.h
Modification log for online index creation and online table rebuild

Created 2011-05-26 Marko Makela
*******************************************************/

#pragma once

#include "que0types.h"
#include "mtr0types.h"
#include "row0types.h"
#include "rem0types.h"
#include "dict0dict.h"
#include "trx0types.h"
#include "trx0undo.h"

class ut_stage_alter_t;

extern Atomic_counter<ulint> onlineddl_rowlog_rows;
extern ulint onlineddl_rowlog_pct_used;
extern ulint onlineddl_pct_progress;

/******************************************************//**
Allocate the row log for an index and flag the index
for online creation.
@retval true if success, false if not */
bool
row_log_allocate(
/*=============*/
	const trx_t*	trx,	/*!< in: the ALTER TABLE transaction */
	dict_index_t*	index,	/*!< in/out: index */
	dict_table_t*	table,	/*!< in/out: new table being rebuilt,
				or NULL when creating a secondary index */
	bool		same_pk,/*!< in: whether the definition of the
				PRIMARY KEY has remained the same */
	const dtuple_t*	defaults,
				/*!< in: default values of
				added, changed columns, or NULL */
	const ulint*	col_map,/*!< in: mapping of old column
				numbers to new ones, or NULL if !table */
	const char*	path,	/*!< in: where to create temporary file */
	const TABLE*	old_table,	/*!< in:table definition before alter */
	bool		allow_not_null) /*!< in: allow null to non-null
					conversion */
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));

/******************************************************//**
Free the row log for an index that was being created online. */
void
row_log_free(
/*=========*/
	row_log_t*	log)	/*!< in,own: row log */
	MY_ATTRIBUTE((nonnull));

/******************************************************//**
Free the row log for an index on which online creation was aborted. */
inline void row_log_abort_sec(dict_index_t *index)
{
  ut_ad(index->lock.have_u_or_x());
  ut_ad(!index->is_clust());
  dict_index_set_online_status(index, ONLINE_INDEX_ABORTED);
  row_log_free(index->online_log);
  index->online_log= nullptr;
}

/** Logs an operation to a secondary index that is (or was) being created.
@param	index	index, S or X latched
@param	tuple	index tuple
@param	trx_id	transaction ID for insert, or 0 for delete
@retval false if row_log_apply() failure happens
or true otherwise */
bool row_log_online_op(dict_index_t *index, const dtuple_t *tuple,
                       trx_id_t trx_id) ATTRIBUTE_COLD;

/******************************************************//**
Gets the error status of the online index rebuild log.
@return DB_SUCCESS or error code */
dberr_t
row_log_table_get_error(
/*====================*/
	const dict_index_t*	index)	/*!< in: clustered index of a table
					that is being rebuilt online */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Check whether a virtual column is indexed in the new table being
created during alter table
@param[in]	index	cluster index
@param[in]	v_no	virtual column number
@return true if it is indexed, else false */
bool
row_log_col_is_indexed(
	const dict_index_t*	index,
	ulint			v_no);

/******************************************************//**
Logs a delete operation to a table that is being rebuilt.
This will be merged in row_log_table_apply_delete(). */
void
row_log_table_delete(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec,index) */
	const byte*	sys)	/*!< in: DB_TRX_ID,DB_ROLL_PTR that should
				be logged, or NULL to use those in rec */
	ATTRIBUTE_COLD __attribute__((nonnull(1,2,3)));

/******************************************************//**
Logs an update operation to a table that is being rebuilt.
This will be merged in row_log_table_apply_update(). */
void
row_log_table_update(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec,index) */
	const dtuple_t*	old_pk);/*!< in: row_log_table_get_pk()
				before the update */

/******************************************************//**
Constructs the old PRIMARY KEY and DB_TRX_ID,DB_ROLL_PTR
of a table that is being rebuilt.
@return tuple of PRIMARY KEY,DB_TRX_ID,DB_ROLL_PTR in the rebuilt table,
or NULL if the PRIMARY KEY definition does not change */
const dtuple_t*
row_log_table_get_pk(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec,index),
				or NULL */
	byte*		sys,	/*!< out: DB_TRX_ID,DB_ROLL_PTR for
				row_log_table_delete(), or NULL */
	mem_heap_t**	heap)	/*!< in/out: memory heap where allocated */
	ATTRIBUTE_COLD __attribute__((nonnull(1,2,5), warn_unused_result));

/******************************************************//**
Logs an insert to a table that is being rebuilt.
This will be merged in row_log_table_apply_insert(). */
void
row_log_table_insert(
/*=================*/
	const rec_t*	rec,	/*!< in: clustered index leaf page record,
				page X-latched */
	dict_index_t*	index,	/*!< in/out: clustered index, S-latched
				or X-latched */
	const rec_offs*	offsets);/*!< in: rec_get_offsets(rec,index) */

/** Apply the row_log_table log to a table upon completing rebuild.
@param[in]	thr		query graph
@param[in]	old_table	old table
@param[in,out]	table		MySQL table (for reporting duplicates)
@param[in,out]	stage		performance schema accounting object, used by
ALTER TABLE. stage->begin_phase_log_table() will be called initially and then
stage->inc() will be called for each block of log that is applied.
@param[in]	new_table	Altered table
@return DB_SUCCESS, or error code on failure */
dberr_t
row_log_table_apply(
	que_thr_t*		thr,
	dict_table_t*		old_table,
	struct TABLE*		table,
	ut_stage_alter_t*	stage,
	dict_table_t*		new_table)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************//**
Get the latest transaction ID that has invoked row_log_online_op()
during online creation.
@return latest transaction ID, or 0 if nothing was logged */
trx_id_t
row_log_get_max_trx(
/*================*/
	dict_index_t*	index)	/*!< in: index, must be locked */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Apply the row log to the index upon completing index creation.
@param[in]	trx	transaction (for checking if the operation was
interrupted)
@param[in,out]	index	secondary index
@param[in,out]	table	MySQL table (for reporting duplicates)
@param[in,out]	stage	performance schema accounting object, used by
ALTER TABLE. stage->begin_phase_log_index() will be called initially and then
stage->inc() will be called for each block of log that is applied.
@return DB_SUCCESS, or error code on failure */
dberr_t
row_log_apply(
	const trx_t*		trx,
	dict_index_t*		index,
	struct TABLE*		table,
	ut_stage_alter_t*	stage)
	MY_ATTRIBUTE((warn_unused_result));

/** Get the n_core_fields of online log for the index
@param	 index	index whose n_core_fields of log to be accessed
@return number of n_core_fields */
unsigned row_log_get_n_core_fields(const dict_index_t *index);

/** Get the error code of online log for the index
@param	index	online index
@return error code present in online log */
dberr_t row_log_get_error(const dict_index_t *index);

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Estimate how much work is to be done by the log apply phase
of an ALTER TABLE for this index.
@param[in]	index	index whose log to assess
@return work to be done by log-apply in abstract units
*/
ulint
row_log_estimate_work(
	const dict_index_t*	index);
#endif /* HAVE_PSI_STAGE_INTERFACE */
