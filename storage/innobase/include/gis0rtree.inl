/*****************************************************************************

Copyright (c) 2014, Oracle and/or its affiliates. All Rights Reserved.
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

/******************************************************************//**
@file include gis0rtree.h
R-tree Inline code

Created 2013/03/27 Jimmy Yang and Allen Lai
***********************************************************************/

/**************************************************************//**
Sets the child node mbr in a node pointer. */
UNIV_INLINE
void
rtr_page_cal_mbr(
/*=============*/
	const dict_index_t*	index,	/*!< in: index */
	const buf_block_t*	block,	/*!< in: buffer block */
	rtr_mbr_t*		rtr_mbr,/*!< out: MBR encapsulates the page */
	mem_heap_t*		heap)	/*!< in: heap for the memory
					allocation */
{
	page_t*		page;
	rec_t*		rec;
	const byte*	field;
	ulint		len;
	rec_offs*	offsets = NULL;
	double		bmin, bmax;
	double*		amin;
	double*		amax;
	ulint		inc = 0;
	double*		mbr;

	rtr_mbr->xmin = DBL_MAX;
	rtr_mbr->ymin = DBL_MAX;
	rtr_mbr->xmax = -DBL_MAX;
	rtr_mbr->ymax = -DBL_MAX;

	mbr = reinterpret_cast<double*>(rtr_mbr);

	page = buf_block_get_frame(block);

	rec = page_rec_get_next(page_get_infimum_rec(page));
	if (UNIV_UNLIKELY(!rec)) {
		return;
	}
	offsets = rec_get_offsets(rec, index, offsets, page_is_leaf(page)
				  ? index->n_fields : 0,
				  ULINT_UNDEFINED, &heap);

	do {
		/* The mbr address is in the first field. */
		field = rec_get_nth_field(rec, offsets, 0, &len);

		ut_ad(len == DATA_MBR_LEN);
		inc = 0;
		for (unsigned i = 0; i < SPDIMS; i++) {
			bmin = mach_double_read(field + inc);
			bmax = mach_double_read(field + inc + sizeof(double));

			amin = mbr + i * SPDIMS;
			amax = mbr + i * SPDIMS + 1;

			if (*amin > bmin)
				*amin = bmin;
			if (*amax < bmax)
				*amax = bmax;

			inc += 2 * sizeof(double);
		}

		rec = page_rec_get_next(rec);

		if (rec == NULL) {
			break;
		}
	} while (!page_rec_is_supremum(rec));
}

/**************************************************************//**
push a nonleaf index node to the search path */
UNIV_INLINE
void
rtr_non_leaf_stack_push(
/*====================*/
	rtr_node_path_t*	path,		/*!< in/out: search path */
	uint32_t		pageno,		/*!< in: pageno to insert */
	node_seq_t		seq_no,		/*!< in: Node sequence num */
	ulint			level,		/*!< in: index page level */
	uint32_t		child_no,	/*!< in: child page no */
	btr_pcur_t*		cursor,		/*!< in: position cursor */
	double			mbr_inc)	/*!< in: MBR needs to be
						enlarged */
{
	node_visit_t	insert_val;

	insert_val.page_no = pageno;
	insert_val.seq_no = seq_no;
	insert_val.level = level;
	insert_val.child_no = child_no;
	insert_val.cursor = cursor;
	insert_val.mbr_inc = mbr_inc;

	path->push_back(insert_val);

#ifdef RTR_SEARCH_DIAGNOSTIC
	fprintf(stderr, "INNODB_RTR: Push page %d, level %d, seq %d"
			" to search stack \n",
		static_cast<int>(pageno), static_cast<int>(level),
		static_cast<int>(seq_no));
#endif /* RTR_SEARCH_DIAGNOSTIC */
}

/*********************************************************************//**
Sets pointer to the data and length in a field. */
UNIV_INLINE
void
rtr_write_mbr(
/*==========*/
	byte*			data,	/*!< out: data */
	const rtr_mbr_t*	mbr)	/*!< in: data */
{
	const double* my_mbr = reinterpret_cast<const double*>(mbr);

	for (unsigned i = 0; i < SPDIMS * 2; i++) {
		mach_double_write(data + i * sizeof(double), my_mbr[i]);
	}
}

/*********************************************************************//**
Sets pointer to the data and length in a field. */
UNIV_INLINE
void
rtr_read_mbr(
/*==========*/
	const byte*	data,	/*!< in: data */
	rtr_mbr_t*	mbr)	/*!< out: MBR */
{
	for (unsigned i = 0; i < SPDIMS * 2; i++) {
		(reinterpret_cast<double*>(mbr))[i] = mach_double_read(
							data
							+ i * sizeof(double));
	}
}

/*********************************************************//**
Returns the R-Tree node stored in the parent search path
@return pointer to R-Tree cursor component in the parent path,
NULL if parent path is empty or index is larger than num of items contained */
UNIV_INLINE
node_visit_t*
rtr_get_parent_node(
/*================*/
	btr_cur_t*	btr_cur,	/*!< in: persistent cursor */
	ulint		level,		/*!< in: index level of buffer page */
	ulint		is_insert)	/*!< in: whether it is insert */
{
	ulint			num;
	ulint			tree_height = btr_cur->tree_height;
	node_visit_t*		found_node = NULL;

	if (level >= tree_height) {
		return(NULL);
	}

	mysql_mutex_lock(&btr_cur->rtr_info->rtr_path_mutex);

	num = btr_cur->rtr_info->parent_path->size();

	if (!num) {
		mysql_mutex_unlock(&btr_cur->rtr_info->rtr_path_mutex);
		return(NULL);
	}

	if (is_insert) {
		ulint	idx = tree_height - level - 1;
		ut_ad(idx < num);

		found_node = &(*btr_cur->rtr_info->parent_path)[idx];
	} else {
		node_visit_t*	node;

		while (num > 0) {
			node = &(*btr_cur->rtr_info->parent_path)[num - 1];

			if (node->level == level) {
				found_node = node;
				break;
			}
			num--;
		}
	}

	mysql_mutex_unlock(&btr_cur->rtr_info->rtr_path_mutex);

	return(found_node);
}

/*********************************************************//**
Returns the R-Tree cursor stored in the parent search path
@return pointer to R-Tree cursor component */
UNIV_INLINE
btr_pcur_t*
rtr_get_parent_cursor(
/*==================*/
	btr_cur_t*	btr_cur,	/*!< in: persistent cursor */
	ulint		level,		/*!< in: index level of buffer page */
	ulint		is_insert)	/*!< in: whether insert operation */
{
	node_visit_t*   found_node = rtr_get_parent_node(
					btr_cur, level, is_insert);

	return((found_node) ? found_node->cursor : NULL);
}

/********************************************************************//**
Reinitialize a R-Tree search info in btr_cur_t */
UNIV_INLINE
void
rtr_info_reinit_in_cursor(
/************************/
	btr_cur_t*	cursor,		/*!< in/out: tree cursor */
	dict_index_t*	index,		/*!< in: index struct */
	bool		need_prdt)	/*!< in: Whether predicate lock is
					needed */
{
	que_thr_t* thr = cursor->rtr_info->thr;
	ut_ad(thr);
	rtr_clean_rtr_info(cursor->rtr_info, false);
	rtr_init_rtr_info(cursor->rtr_info, need_prdt, cursor, index, true);
	cursor->rtr_info->thr = thr;
}
