/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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
@file include/row0ins.h
Insert into a table

Created 4/20/1996 Heikki Tuuri
*******************************************************/

#ifndef row0ins_h
#define row0ins_h

#include "data0data.h"
#include "que0types.h"
#include "trx0types.h"
#include "row0types.h"
#include <vector>

/***************************************************************//**
Checks if foreign key constraint fails for an index entry. Sets shared locks
which lock either the success or the failure of the constraint. NOTE that
the caller must have a shared latch on dict_foreign_key_check_lock.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_NO_REFERENCED_ROW, or
DB_ROW_IS_REFERENCED */
dberr_t
row_ins_check_foreign_constraint(
/*=============================*/
	ibool		check_ref,/*!< in: TRUE If we want to check that
				the referenced table is ok, FALSE if we
				want to check the foreign key table */
	dict_foreign_t*	foreign,/*!< in: foreign constraint; NOTE that the
				tables mentioned in it must be in the
				dictionary cache if they exist at all */
	dict_table_t*	table,	/*!< in: if check_ref is TRUE, then the foreign
				table, else the referenced table */
	dtuple_t*	entry,	/*!< in: index entry for index */
	que_thr_t*	thr)	/*!< in: query thread */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*********************************************************************//**
Sets a new row to insert for an INS_DIRECT node. This function is only used
if we have constructed the row separately, which is a rare case; this
function is quite slow. */
void
ins_node_set_new_row(
/*=================*/
	ins_node_t*	node,	/*!< in: insert node */
	dtuple_t*	row);	/*!< in: new row (or first row) for the node */
/***************************************************************//**
Tries to insert an entry into a clustered index, ignoring foreign key
constraints. If a record with the same unique key is found, the other
record is necessarily marked deleted by a committed transaction, or a
unique key violation error occurs. The delete marked record is then
updated to an existing record, and we must write an undo log record on
the delete marked record.
@retval DB_SUCCESS on success
@retval DB_LOCK_WAIT on lock wait when !(flags & BTR_NO_LOCKING_FLAG)
@retval DB_FAIL if retry with BTR_MODIFY_TREE is needed
@return error code */
dberr_t
row_ins_clust_index_entry_low(
/*==========================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE,
				depending on whether we wish optimistic or
				pessimistic descent down the index tree */
	dict_index_t*	index,	/*!< in: clustered index */
	ulint		n_uniq,	/*!< in: 0 or index->n_uniq */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	que_thr_t*	thr)	/*!< in: query thread or NULL */
	MY_ATTRIBUTE((warn_unused_result));

/***************************************************************//**
Tries to insert an entry into a secondary index. If a record with exactly the
same fields is found, the other record is necessarily marked deleted.
It is then unmarked. Otherwise, the entry is just inserted to the index.
@retval DB_SUCCESS on success
@retval DB_LOCK_WAIT on lock wait when !(flags & BTR_NO_LOCKING_FLAG)
@retval DB_FAIL if retry with BTR_MODIFY_TREE is needed
@return error code */
dberr_t
row_ins_sec_index_entry_low(
/*========================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE,
				depending on whether we wish optimistic or
				pessimistic descent down the index tree */
	dict_index_t*	index,	/*!< in: secondary index */
	mem_heap_t*	offsets_heap,
				/*!< in/out: memory heap that can be emptied */
	mem_heap_t*	heap,	/*!< in/out: memory heap */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	trx_id_t	trx_id,	/*!< in: PAGE_MAX_TRX_ID during
				row_log_table_apply(), or 0 */
	que_thr_t*	thr)	/*!< in: query thread */
	MY_ATTRIBUTE((warn_unused_result));

/***************************************************************//**
Inserts an entry into a clustered index. Tries first optimistic,
then pessimistic descent down the tree. If the entry matches enough
to a delete marked record, performs the insert by updating or delete
unmarking the delete marked record.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_DUPLICATE_KEY, or some other error code */
dberr_t
row_ins_clust_index_entry(
/*======================*/
	dict_index_t*	index,	/*!< in: clustered index */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	que_thr_t*	thr,	/*!< in: query thread */
	ulint		n_ext)	/*!< in: number of externally stored columns */
	MY_ATTRIBUTE((warn_unused_result));
/***************************************************************//**
Inserts an entry into a secondary index. Tries first optimistic,
then pessimistic descent down the tree. If the entry matches enough
to a delete marked record, performs the insert by updating or delete
unmarking the delete marked record.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_DUPLICATE_KEY, or some other error code */
dberr_t
row_ins_sec_index_entry(
/*====================*/
	dict_index_t*	index,	/*!< in: secondary index */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	que_thr_t*	thr,	/*!< in: query thread */
	bool		check_foreign = true) /*!< in: true if check
				foreign table is needed, false otherwise */
	MY_ATTRIBUTE((warn_unused_result));
/***********************************************************//**
Inserts a row to a table. This is a high-level function used in
SQL execution graphs.
@return query thread to run next or NULL */
que_thr_t*
row_ins_step(
/*=========*/
	que_thr_t*	thr);	/*!< in: query thread */

/* Insert node types */
#define INS_SEARCHED	0	/* INSERT INTO ... SELECT ... */
#define INS_VALUES	1	/* INSERT INTO ... VALUES ... */
#define INS_DIRECT	2	/* this is for internal use in dict0crea:
				insert the row directly */

/* Node execution states */
#define	INS_NODE_SET_IX_LOCK	1	/* we should set an IX lock on table */
#define INS_NODE_ALLOC_ROW_ID	2	/* row id should be allocated */
#define	INS_NODE_INSERT_ENTRIES 3	/* index entries should be built and
					inserted */

/** Insert node structure */
struct ins_node_t
{
	explicit ins_node_t(ulint ins_type, dict_table_t *table) :
		common(QUE_NODE_INSERT, NULL),
		ins_type(ins_type),
		row(NULL), table(table), select(NULL), values_list(NULL),
		state(INS_NODE_SET_IX_LOCK), index(NULL),
		entry_list(), entry(entry_list.end()),
		trx_id(0), entry_sys_heap(mem_heap_create(128))
	{
	}
	que_common_t common;	 /*!< node type: QUE_NODE_INSERT */
	ulint		ins_type;/* INS_VALUES, INS_SEARCHED, or INS_DIRECT */
	dtuple_t*	row;	/*!< row to insert */
	dict_table_t*	table;	/*!< table where to insert */
	sel_node_t*	select;	/*!< select in searched insert */
	que_node_t*	values_list;/* list of expressions to evaluate and
				insert in an INS_VALUES insert */
	ulint		state;	/*!< node execution state */
	dict_index_t*	index;	/*!< NULL, or the next index where the index
				entry should be inserted */
	std::vector<dtuple_t*>
			entry_list;/* list of entries, one for each index */
	std::vector<dtuple_t*>::iterator
			entry;	/*!< NULL, or entry to insert in the index;
				after a successful insert of the entry,
				this should be reset to NULL */
	/** buffer for the system columns */
	byte		sys_buf[DATA_ROW_ID_LEN
				+ DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN];
	trx_id_t	trx_id;	/*!< trx id or the last trx which executed the
				node */
	byte		vers_start_buf[8]; /* Buffers for System Versioning */
	byte		vers_end_buf[8];   /* system fields. */
	mem_heap_t*	entry_sys_heap;
				/* memory heap used as auxiliary storage;
				entry_list and sys fields are stored here;
				if this is NULL, entry list should be created
				and buffers for sys fields in row allocated */
};

/** Create an insert object.
@param ins_type     INS_VALUES, ...
@param table        table where to insert
@param heap         memory heap
@return the created object */
inline ins_node_t *ins_node_create(ulint ins_type, dict_table_t *table,
                                   mem_heap_t *heap)
{
  return new (mem_heap_alloc(heap, sizeof(ins_node_t)))
    ins_node_t(ins_type, table);
}

#endif
