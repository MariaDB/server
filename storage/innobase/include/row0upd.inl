/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/row0upd.ic
Update of a row

Created 12/27/1996 Heikki Tuuri
*******************************************************/

#include "mtr0log.h"
#include "trx0trx.h"
#include "trx0undo.h"
#include "row0row.h"
#include "lock0lock.h"
#include "page0zip.h"

/*********************************************************************//**
Creates an update vector object.
@return own: update vector object */
UNIV_INLINE
upd_t*
upd_create(
/*=======*/
	ulint		n,	/*!< in: number of fields */
	mem_heap_t*	heap)	/*!< in: heap from which memory allocated */
{
	upd_t*	update;

	update = static_cast<upd_t*>(mem_heap_zalloc(
			heap, sizeof(upd_t) + sizeof(upd_field_t) * n));

	update->n_fields = n;
	update->fields = reinterpret_cast<upd_field_t*>(&update[1]);
	update->heap = heap;

	return(update);
}

/*********************************************************************//**
Returns the number of fields in the update vector == number of columns
to be updated by an update vector.
@return number of fields */
UNIV_INLINE
ulint
upd_get_n_fields(
/*=============*/
	const upd_t*	update)	/*!< in: update vector */
{
	ut_ad(update);

	return(update->n_fields);
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Returns the nth field of an update vector.
@return update vector field */
UNIV_INLINE
upd_field_t*
upd_get_nth_field(
/*==============*/
	const upd_t*	update,	/*!< in: update vector */
	ulint		n)	/*!< in: field position in update vector */
{
	ut_ad(update);
	ut_ad(n < update->n_fields);

	return((upd_field_t*) update->fields + n);
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Sets an index field number to be updated by an update vector field. */
UNIV_INLINE
void
upd_field_set_field_no(
/*===================*/
	upd_field_t*	upd_field,	/*!< in: update vector field */
	uint16_t	field_no,	/*!< in: field number in a clustered
					index */
	dict_index_t*	index)		/*!< in: index */
{
	upd_field->field_no = field_no;
	upd_field->orig_len = 0;
	dict_col_copy_type(dict_index_get_nth_col(index, field_no),
			   dfield_get_type(&upd_field->new_val));
}

/** set field number to a update vector field, marks this field is updated.
@param[in,out]	upd_field	update vector field
@param[in]	field_no	virtual column sequence num
@param[in]	index		index */
UNIV_INLINE
void
upd_field_set_v_field_no(
	upd_field_t*	upd_field,
	uint16_t	field_no,
	dict_index_t*	index)
{
	ut_a(field_no < dict_table_get_n_v_cols(index->table));
	upd_field->field_no = field_no;
	upd_field->orig_len = 0;

	dict_col_copy_type(&dict_table_get_nth_v_col(
				index->table, field_no)->m_col,
			   dfield_get_type(&upd_field->new_val));
}

/*********************************************************************//**
Returns a field of an update vector by field_no.
@return update vector field, or NULL */
UNIV_INLINE
const upd_field_t*
upd_get_field_by_field_no(
/*======================*/
	const upd_t*	update,	/*!< in: update vector */
	uint16_t	no,	/*!< in: field_no */
	bool		is_virtual) /*!< in: if it is virtual column */
{
	ulint	i;
	for (i = 0; i < upd_get_n_fields(update); i++) {
		const upd_field_t*	uf = upd_get_nth_field(update, i);

		/* matches only if the field matches that of is_virtual */
		if ((!is_virtual) != (!upd_fld_is_virtual_col(uf))) {
			continue;
		}

		if (uf->field_no == no) {

			return(uf);
		}
	}

	return(NULL);
}
