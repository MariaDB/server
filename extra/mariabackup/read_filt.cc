/******************************************************
XtraBackup: hot backup tool for InnoDB
(c) 2009-2012 Percona Inc.
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

/* Data file read filter implementation */

#include "read_filt.h"
#include "common.h"
#include "fil_cur.h"
#include "xtrabackup.h"

/****************************************************************//**
Perform read filter context initialization that is common to all read
filters.  */
static
void
common_init(
/*========*/
	xb_read_filt_ctxt_t*	ctxt,	/*!<in/out: read filter context */
	const xb_fil_cur_t*	cursor)	/*!<in: file cursor */
{
	ctxt->offset = 0;
	ctxt->data_file_size = cursor->statinfo.st_size;
	ctxt->buffer_capacity = cursor->buf_size;
	ctxt->page_size = cursor->page_size;
}

/****************************************************************//**
Initialize the pass-through read filter. */
static
void
rf_pass_through_init(
/*=================*/
	xb_read_filt_ctxt_t*	ctxt,	/*!<in/out: read filter context */
	const xb_fil_cur_t*	cursor,	/*!<in: file cursor */
	ulint			space_id __attribute__((unused)))
					/*!<in: space id we are reading */
{
	common_init(ctxt, cursor);
}

/****************************************************************//**
Get the next batch of pages for the pass-through read filter.  */
static
void
rf_pass_through_get_next_batch(
/*===========================*/
	xb_read_filt_ctxt_t*	ctxt,			/*!<in/out: read filter
							context */
	ib_int64_t*		read_batch_start,	/*!<out: starting read
							offset in bytes for the
							next batch of pages */
	ib_int64_t*		read_batch_len)		/*!<out: length in
							bytes of the next batch
							of pages */
{
	*read_batch_start = ctxt->offset;
	*read_batch_len = ctxt->data_file_size - ctxt->offset;

	if (*read_batch_len > (ib_int64_t)ctxt->buffer_capacity) {
		*read_batch_len = ctxt->buffer_capacity;
	}

	ctxt->offset += *read_batch_len;
}

/****************************************************************//**
Deinitialize the pass-through read filter.  */
static
void
rf_pass_through_deinit(
/*===================*/
	xb_read_filt_ctxt_t*	ctxt __attribute__((unused)))
					/*!<in: read filter context */
{
}

/****************************************************************//**
Initialize the changed page bitmap-based read filter.  Assumes that
the bitmap is already set up in changed_page_bitmap.  */
static
void
rf_bitmap_init(
/*===========*/
	xb_read_filt_ctxt_t*	ctxt,		/*!<in/out: read filter
						context */
	const xb_fil_cur_t*	cursor,		/*!<in: read cursor */
	ulint			space_id)	/*!<in: space id  */
{
	common_init(ctxt, cursor);
	ctxt->bitmap_range = xb_page_bitmap_range_init(changed_page_bitmap,
						       space_id);
	ctxt->filter_batch_end = 0;
}

/****************************************************************//**
Get the next batch of pages for the bitmap read filter.  */
static
void
rf_bitmap_get_next_batch(
/*=====================*/
	xb_read_filt_ctxt_t*	ctxt,			/*!<in/out: read filter
							context */
	ib_int64_t*		read_batch_start,	/*!<out: starting read
							offset in bytes for the
							next batch of pages */
	ib_int64_t*		read_batch_len)		/*!<out: length in
							bytes of the next batch
							of pages */
{
	ulint	start_page_id;
	const ulint	page_size	= ctxt->page_size.physical();

	start_page_id = (ulint)(ctxt->offset / page_size);

	xb_a (ctxt->offset % page_size == 0);

	if (start_page_id == ctxt->filter_batch_end) {

		/* Used up all the previous bitmap range, get some more */
		ulint next_page_id;

		/* Find the next changed page using the bitmap */
		next_page_id = xb_page_bitmap_range_get_next_bit
			(ctxt->bitmap_range, TRUE);

		if (next_page_id == ULINT_UNDEFINED) {
			*read_batch_len = 0;
			return;
		}

		ctxt->offset = next_page_id * page_size;

		/* Find the end of the current changed page block by searching
		for the next cleared bitmap bit */
		ctxt->filter_batch_end
			= xb_page_bitmap_range_get_next_bit(ctxt->bitmap_range,
							    FALSE);
		xb_a(next_page_id < ctxt->filter_batch_end);
	}

	*read_batch_start = ctxt->offset;
	if (ctxt->filter_batch_end == ULINT_UNDEFINED) {
		/* No more cleared bits in the bitmap, need to copy all the
		remaining pages.  */
		*read_batch_len = ctxt->data_file_size - ctxt->offset;
	} else {
		*read_batch_len = ctxt->filter_batch_end * page_size
			- ctxt->offset;
	}

	/* If the page block is larger than the buffer capacity, limit it to
	buffer capacity.  The subsequent invocations will continue returning
	the current block in buffer-sized pieces until ctxt->filter_batch_end
	is reached, trigerring the next bitmap query.  */
	if (*read_batch_len > (ib_int64_t)ctxt->buffer_capacity) {
		*read_batch_len = ctxt->buffer_capacity;
	}

	ctxt->offset += *read_batch_len;
	xb_a (ctxt->offset % page_size == 0);
	xb_a (*read_batch_start % page_size == 0);
	xb_a (*read_batch_len % page_size == 0);
}

/****************************************************************//**
Deinitialize the changed page bitmap-based read filter.  */
static
void
rf_bitmap_deinit(
/*=============*/
	xb_read_filt_ctxt_t*	ctxt)	/*!<in/out: read filter context */
{
	xb_page_bitmap_range_deinit(ctxt->bitmap_range);
}

/* The pass-through read filter */
xb_read_filt_t rf_pass_through = {
	&rf_pass_through_init,
	&rf_pass_through_get_next_batch,
	&rf_pass_through_deinit
};

/* The changed page bitmap-based read filter */
xb_read_filt_t rf_bitmap = {
	&rf_bitmap_init,
	&rf_bitmap_get_next_batch,
	&rf_bitmap_deinit
};
