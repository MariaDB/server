/*****************************************************************************

Copyright (c) 1996, 2018, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/row0upd.h
Update of a row

Created 12/27/1996 Heikki Tuuri
*******************************************************/

#ifndef row0upd_h
#define row0upd_h

#include "data0data.h"
#include "rem0types.h"
#include "row0types.h"
#include "btr0types.h"
#include "trx0types.h"
#include "btr0pcur.h"
#include "que0types.h"
#include "pars0types.h"

/*********************************************************************//**
Creates an update vector object.
@return own: update vector object */
UNIV_INLINE
upd_t*
upd_create(
/*=======*/
	ulint		n,	/*!< in: number of fields */
	mem_heap_t*	heap);	/*!< in: heap from which memory allocated */
/*********************************************************************//**
Returns the number of fields in the update vector == number of columns
to be updated by an update vector.
@return number of fields */
UNIV_INLINE
ulint
upd_get_n_fields(
/*=============*/
	const upd_t*	update);	/*!< in: update vector */
#ifdef UNIV_DEBUG
/*********************************************************************//**
Returns the nth field of an update vector.
@return update vector field */
UNIV_INLINE
upd_field_t*
upd_get_nth_field(
/*==============*/
	const upd_t*	update,	/*!< in: update vector */
	ulint		n);	/*!< in: field position in update vector */
#else
# define upd_get_nth_field(update, n) ((update)->fields + (n))
#endif

/*********************************************************************//**
Sets an index field number to be updated by an update vector field. */
UNIV_INLINE
void
upd_field_set_field_no(
/*===================*/
	upd_field_t*	upd_field,	/*!< in: update vector field */
	uint16_t	field_no,	/*!< in: field number in a clustered
					index */
	dict_index_t*	index);

/** set field number to a update vector field, marks this field is updated
@param[in,out]	upd_field	update vector field
@param[in]	field_no	virtual column sequence num
@param[in]	index		index */
UNIV_INLINE
void
upd_field_set_v_field_no(
	upd_field_t*	upd_field,
	uint16_t	field_no,
	dict_index_t*	index);
/*********************************************************************//**
Returns a field of an update vector by field_no.
@return update vector field, or NULL */
UNIV_INLINE
const upd_field_t*
upd_get_field_by_field_no(
/*======================*/
	const upd_t*	update,	/*!< in: update vector */
	uint16_t	no,	/*!< in: field_no */
	bool		is_virtual) /*!< in: if it is a virtual column */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Creates an update node for a query graph.
@return own: update node */
upd_node_t*
upd_node_create(
/*============*/
	mem_heap_t*	heap);	/*!< in: mem heap where created */
/***********************************************************//**
Returns TRUE if row update changes size of some field in index or if some
field to be updated is stored externally in rec or update.
@return TRUE if the update changes the size of some field in index or
the field is external in rec or update */
ibool
row_upd_changes_field_size_or_external(
/*===================================*/
	dict_index_t*	index,	/*!< in: index */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	const upd_t*	update);/*!< in: update vector */
/***********************************************************//**
Returns true if row update contains disowned external fields.
@return true if the update contains disowned external fields. */
bool
row_upd_changes_disowned_external(
/*==============================*/
	const upd_t*	update)	/*!< in: update vector */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/***************************************************************//**
Builds an update vector from those fields which in a secondary index entry
differ from a record that has the equal ordering fields. NOTE: we compare
the fields as binary strings!
@return own: update vector of differing fields */
upd_t*
row_upd_build_sec_rec_difference_binary(
/*====================================*/
	const rec_t*	rec,	/*!< in: secondary index record */
	dict_index_t*	index,	/*!< in: index */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	const dtuple_t*	entry,	/*!< in: entry to insert */
	mem_heap_t*	heap)	/*!< in: memory heap from which allocated */
	MY_ATTRIBUTE((warn_unused_result, nonnull));
/** Builds an update vector from those fields, excluding the roll ptr and
trx id fields, which in an index entry differ from a record that has
the equal ordering fields. NOTE: we compare the fields as binary strings!
@param[in]	index		clustered index
@param[in]	entry		clustered index entry to insert
@param[in]	rec		clustered index record
@param[in]	offsets		rec_get_offsets(rec,index), or NULL
@param[in]	no_sys		skip the system columns
				DB_TRX_ID and DB_ROLL_PTR
@param[in]	trx		transaction (for diagnostics),
				or NULL
@param[in]	heap		memory heap from which allocated
@param[in,out]	mysql_table	NULL, or mysql table object when
				user thread invokes dml
@param[out]	error		error number in case of failure
@return own: update vector of differing fields, excluding roll ptr and
trx id */
upd_t*
row_upd_build_difference_binary(
	dict_index_t*	index,
	const dtuple_t*	entry,
	const rec_t*	rec,
	const rec_offs*	offsets,
	bool		no_sys,
	trx_t*		trx,
	mem_heap_t*	heap,
	TABLE*		mysql_table,
	dberr_t*	error)
	MY_ATTRIBUTE((nonnull(1,2,3,7,9), warn_unused_result));
/** Apply an update vector to an index entry.
@param[in,out]	entry	index entry to be updated; the clustered index record
			must be covered by a lock or a page latch to prevent
			deletion (rollback or purge)
@param[in]	index	index of the entry
@param[in]	update	update vector built for the entry
@param[in,out]	heap	memory heap for copying off-page columns */
void
row_upd_index_replace_new_col_vals_index_pos(
	dtuple_t*		entry,
	const dict_index_t*	index,
	const upd_t*		update,
	mem_heap_t*		heap)
	MY_ATTRIBUTE((nonnull));
/** Replace the new column values stored in the update vector,
during trx_undo_prev_version_build().
@param entry   clustered index tuple where the values are replaced
               (the clustered index leaf page latch must be held)
@param index   clustered index
@param update  update vector for the clustered index
@param heap    memory heap for allocating and copying values
@return whether the previous version was built successfully */
bool
row_upd_index_replace_new_col_vals(dtuple_t *entry, const dict_index_t &index,
                                   const upd_t *update, mem_heap_t *heap)
  MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************//**
Replaces the new column values stored in the update vector. */
void
row_upd_replace(
/*============*/
	dtuple_t*		row,	/*!< in/out: row where replaced,
					indexed by col_no;
					the clustered index record must be
					covered by a lock or a page latch to
					prevent deletion (rollback or purge) */
	row_ext_t**		ext,	/*!< out, own: NULL, or externally
					stored column prefixes */
	const dict_index_t*	index,	/*!< in: clustered index */
	const upd_t*		update,	/*!< in: an update vector built for the
					clustered index */
	mem_heap_t*		heap);	/*!< in: memory heap */
/** Replaces the virtual column values stored in a dtuple with that of
a update vector.
@param[in,out]	row	dtuple whose column to be updated
@param[in]	table	table
@param[in]	update	an update vector built for the clustered index
@param[in]	upd_new	update to new or old value
@param[in,out]	undo_row undo row (if needs to be updated)
@param[in]	ptr	remaining part in update undo log */
void
row_upd_replace_vcol(
	dtuple_t*		row,
	const dict_table_t*	table,
	const upd_t*		update,
	bool			upd_new,
	dtuple_t*		undo_row,
	const byte*		ptr);

/***********************************************************//**
Checks if an update vector changes an ordering field of an index record.

This function is fast if the update vector is short or the number of ordering
fields in the index is small. Otherwise, this can be quadratic.
NOTE: we compare the fields as binary strings!
@return TRUE if update vector changes an ordering field in the index record */
ibool
row_upd_changes_ord_field_binary_func(
/*==================================*/
	dict_index_t*	index,	/*!< in: index of the record */
	const upd_t*	update,	/*!< in: update vector for the row; NOTE: the
				field numbers in this MUST be clustered index
				positions! */
#ifdef UNIV_DEBUG
	const que_thr_t*thr,	/*!< in: query thread */
#endif /* UNIV_DEBUG */
	const dtuple_t*	row,	/*!< in: old value of row, or NULL if the
				row and the data values in update are not
				known when this function is called, e.g., at
				compile time */
	const row_ext_t*ext,	/*!< NULL, or prefixes of the externally
				stored columns in the old row */
	ulint		flag)	/*!< in: ROW_BUILD_NORMAL,
				ROW_BUILD_FOR_PURGE or ROW_BUILD_FOR_UNDO */
	MY_ATTRIBUTE((nonnull(1,2), warn_unused_result));
#ifdef UNIV_DEBUG
# define row_upd_changes_ord_field_binary(index,update,thr,row,ext)	\
	row_upd_changes_ord_field_binary_func(index,update,thr,row,ext,0)
#else /* UNIV_DEBUG */
# define row_upd_changes_ord_field_binary(index,update,thr,row,ext)	\
	row_upd_changes_ord_field_binary_func(index,update,row,ext,0)
#endif /* UNIV_DEBUG */
/***********************************************************//**
Checks if an FTS indexed column is affected by an UPDATE.
@return offset within fts_t::indexes if FTS indexed column updated else
ULINT_UNDEFINED */
ulint
row_upd_changes_fts_column(
/*=======================*/
	dict_table_t*	table,		/*!< in: table */
	upd_field_t*	upd_field);	/*!< in: field to check */
/***********************************************************//**
Checks if an FTS Doc ID column is affected by an UPDATE.
@return whether Doc ID column is affected */
bool
row_upd_changes_doc_id(
/*===================*/
	dict_table_t*	table,		/*!< in: table */
	upd_field_t*	upd_field)	/*!< in: field to check */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************//**
Checks if an update vector changes an ordering field of an index record.
This function is fast if the update vector is short or the number of ordering
fields in the index is small. Otherwise, this can be quadratic.
NOTE: we compare the fields as binary strings!
@return TRUE if update vector may change an ordering field in an index
record */
ibool
row_upd_changes_some_index_ord_field_binary(
/*========================================*/
	const dict_table_t*	table,	/*!< in: table */
	const upd_t*		update);/*!< in: update vector for the row */
/***********************************************************//**
Updates a row in a table. This is a high-level function used
in SQL execution graphs.
@return query thread to run next or NULL */
que_thr_t*
row_upd_step(
/*=========*/
	que_thr_t*	thr);	/*!< in: query thread */

/* Update vector field */
struct upd_field_t{
	uint16_t	field_no;	/*!< field number in an index, usually
					the clustered index, but in updating
					a secondary index record in btr0cur.cc
					this is the position in the secondary
					index. If this field is a virtual
					column, then field_no represents
					the nth virtual	column in the table */
	uint16_t	orig_len;	/*!< original length of the locally
					stored part of an externally stored
					column, or 0 */
	que_node_t*	exp;		/*!< expression for calculating a new
					value: it refers to column values and
					constants in the symbol table of the
					query graph */
	dfield_t	new_val;	/*!< new value for the column */
	dfield_t*	old_v_val;	/*!< old value for the virtual column */
};


/* check whether an update field is on virtual column */
#define upd_fld_is_virtual_col(upd_fld)			\
	(((upd_fld)->new_val.type.prtype & DATA_VIRTUAL) == DATA_VIRTUAL)

/* set DATA_VIRTUAL bit on update field to show it is a virtual column */
#define upd_fld_set_virtual_col(upd_fld)			\
	((upd_fld)->new_val.type.prtype |= DATA_VIRTUAL)

/* Update vector structure */
struct upd_t{
	mem_heap_t*	heap;		/*!< heap from which memory allocated */
	byte		info_bits;	/*!< new value of info bits to record;
					default is 0 */
	dtuple_t*	old_vrow;	/*!< pointer to old row, used for
					virtual column update now */
	ulint		n_fields;	/*!< number of update fields */
	upd_field_t*	fields;		/*!< array of update fields */
	byte		vers_sys_value[8]; /*!< buffer for updating system fields */

	/** Append an update field to the end of array
	@param[in]	field	an update field */
	void append(const upd_field_t& field)
	{
		fields[n_fields++] = field;
	}

        void remove_element(ulint i)
        {
          ut_ad(n_fields > 0);
          ut_ad(i < n_fields);
          while (i < n_fields - 1)
          {
            fields[i]= fields[i + 1];
            i++;
          }
          n_fields--;
        }

        bool remove(const ulint field_no)
        {
          for (ulint i= 0; i < n_fields; ++i)
          {
            if (field_no == fields[i].field_no)
            {
              remove_element(i);
              return true;
            }
          }
          return false;
        }

        /** Determine if the given field_no is modified.
	@return true if modified, false otherwise.  */
	bool is_modified(uint16_t field_no) const
	{
		for (ulint i = 0; i < n_fields; ++i) {
			if (field_no == fields[i].field_no) {
				return(true);
			}
		}
		return(false);
	}

	/** Determine if the update affects a system versioned column or row_end. */
	bool affects_versioned() const
	{
		for (ulint i = 0; i < n_fields; i++) {
			dtype_t type = fields[i].new_val.type;
			if (type.is_versioned()) {
				return true;
			}
			// versioned DELETE is UPDATE SET row_end=NOW
			if (type.vers_sys_end()) {
				return true;
			}
		}
		return false;
	}

	/** @return whether this is for a hidden metadata record
	for instant ALTER TABLE */
	bool is_metadata() const { return dtuple_t::is_metadata(info_bits); }
	/** @return whether this is for a hidden metadata record
	for instant ALTER TABLE (not only ADD COLUMN) */
	bool is_alter_metadata() const
	{ return dtuple_t::is_alter_metadata(info_bits); }

#ifdef UNIV_DEBUG
        bool validate() const
        {
                for (ulint i = 0; i < n_fields; ++i) {
                        dfield_t* field = &fields[i].new_val;
                        if (dfield_is_ext(field)) {
				ut_ad(dfield_get_len(field)
				      >= BTR_EXTERN_FIELD_REF_SIZE);
                        }
                }
                return(true);
        }
#endif // UNIV_DEBUG
};

/** Kinds of update operation */
enum delete_mode_t {
	NO_DELETE = 0,		/*!< this operation does not delete */
	PLAIN_DELETE,		/*!< ordinary delete */
	VERSIONED_DELETE	/*!< update old and insert a new row */
};

/* Update node structure which also implements the delete operation
of a row */

struct upd_node_t{
	que_common_t	common;	/*!< node type: QUE_NODE_UPDATE */
	delete_mode_t	is_delete;	/*!< kind of DELETE */
	ibool		searched_update;
				/* TRUE if searched update, FALSE if
				positioned */
	bool		in_mysql_interface;
				/* whether the update node was created
				for the MySQL interface */
	dict_foreign_t*	foreign;/* NULL or pointer to a foreign key
				constraint if this update node is used in
				doing an ON DELETE or ON UPDATE operation */
	upd_node_t*	cascade_node;/* NULL or an update node template which
				is used to implement ON DELETE/UPDATE CASCADE
				or ... SET NULL for foreign keys */
	mem_heap_t*	cascade_heap;
				/*!< NULL or a mem heap where cascade
				node is created.*/
	sel_node_t*	select;	/*!< query graph subtree implementing a base
				table cursor: the rows returned will be
				updated */
	btr_pcur_t*	pcur;	/*!< persistent cursor placed on the clustered
				index record which should be updated or
				deleted; the cursor is stored in the graph
				of 'select' field above, except in the case
				of the MySQL interface */
	dict_table_t*	table;	/*!< table where updated */
	upd_t*		update;	/*!< update vector for the row */
	ulint		update_n_fields;
				/* when this struct is used to implement
				a cascade operation for foreign keys, we store
				here the size of the buffer allocated for use
				as the update vector */
	sym_node_list_t	columns;/* symbol table nodes for the columns
				to retrieve from the table */
	ibool		has_clust_rec_x_lock;
				/* TRUE if the select which retrieves the
				records to update already sets an x-lock on
				the clustered record; note that it must always
				set at least an s-lock */
	ulint		cmpl_info;/* information extracted during query
				compilation; speeds up execution:
				UPD_NODE_NO_ORD_CHANGE and
				UPD_NODE_NO_SIZE_CHANGE, ORed */
	/*----------------------*/
	/* Local storage for this graph node */
	ulint		state;	/*!< node execution state */
	dict_index_t*	index;	/*!< NULL, or the next index whose record should
				be updated */
	dtuple_t*	row;	/*!< NULL, or a copy (also fields copied to
				heap) of the row to update; this must be reset
				to NULL after a successful update */
	dtuple_t*	historical_row;	/*!< historical row used in
				CASCADE UPDATE/SET NULL;
				allocated from historical_heap  */
	mem_heap_t*	historical_heap; /*!< heap for historical row insertion;
				created when row to update is located;
				freed right before row update */
	row_ext_t*	ext;	/*!< NULL, or prefixes of the externally
				stored columns in the old row */
	dtuple_t*	upd_row;/* NULL, or a copy of the updated row */
	row_ext_t*	upd_ext;/* NULL, or prefixes of the externally
				stored columns in upd_row */
	mem_heap_t*	heap;	/*!< memory heap used as auxiliary storage;
				this must be emptied after a successful
				update */
	/*----------------------*/
	sym_node_t*	table_sym;/* table node in symbol table */
	que_node_t*	col_assign_list;
				/* column assignment list */
	ulint		magic_n;

private:
	/** Appends row_start or row_end field to update vector and sets a
	CURRENT_TIMESTAMP/trx->id value to it.
	Supposed to be called only by make_versioned_update() and
	make_versioned_delete().
	@param[in]	trx	transaction
	@param[in]	vers_sys_idx	table->row_start or table->row_end */
  void vers_update_fields(const trx_t *trx, ulint idx);

public:
	/** Also set row_start = CURRENT_TIMESTAMP/trx->id
	@param[in]	trx	transaction */
  void vers_make_update(const trx_t *trx)
  {
    vers_update_fields(trx, table->vers_start);
        }

	/** Only set row_end = CURRENT_TIMESTAMP/trx->id.
	Do not touch other fields at all.
	@param[in]	trx	transaction */
        void vers_make_delete(const trx_t *trx)
        {
		update->n_fields = 0;
		is_delete = VERSIONED_DELETE;
                vers_update_fields(trx, table->vers_end);
        }
};

#define	UPD_NODE_MAGIC_N	1579975

/* Node execution states */
#define UPD_NODE_SET_IX_LOCK	   1	/* execution came to the node from
					a node above and if the field
					has_clust_rec_x_lock is FALSE, we
					should set an intention x-lock on
					the table */
#define UPD_NODE_UPDATE_CLUSTERED  2	/* clustered index record should be
					updated */
#define UPD_NODE_INSERT_CLUSTERED  3	/* clustered index record should be
					inserted, old record is already delete
					marked */
#define UPD_NODE_UPDATE_ALL_SEC	   5	/* an ordering field of the clustered
					index record was changed, or this is
					a delete operation: should update
					all the secondary index records */
#define UPD_NODE_UPDATE_SOME_SEC   6	/* secondary index entries should be
					looked at and updated if an ordering
					field changed */

/* Compilation info flags: these must fit within 3 bits; see trx0rec.h */
#define UPD_NODE_NO_ORD_CHANGE	1	/* no secondary index record will be
					changed in the update and no ordering
					field of the clustered index */
#define UPD_NODE_NO_SIZE_CHANGE	2	/* no record field size will be
					changed in the update */


#include "row0upd.inl"

#endif
