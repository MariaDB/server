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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

/* Changed page bitmap interface */

#ifndef XB_CHANGED_PAGE_BITMAP_H
#define XB_CHANGED_PAGE_BITMAP_H

#include <ut0rbt.h>
#include <fil0fil.h>

/* The changed page bitmap structure */
typedef ib_rbt_t xb_page_bitmap;

struct xb_page_bitmap_range_struct;

/* The bitmap range iterator over one space id */
typedef struct xb_page_bitmap_range_struct xb_page_bitmap_range;

/****************************************************************//**
Read the disk bitmap and build the changed page bitmap tree for the
LSN interval incremental_lsn to checkpoint_lsn_start.

@return the built bitmap tree */
xb_page_bitmap*
xb_page_bitmap_init(void);
/*=====================*/

/****************************************************************//**
Free the bitmap tree. */
void
xb_page_bitmap_deinit(
/*==================*/
	xb_page_bitmap*	bitmap);	/*!<in/out: bitmap tree */


/****************************************************************//**
Set up a new bitmap range iterator over a given space id changed
pages in a given bitmap.

@return bitmap range iterator */
xb_page_bitmap_range*
xb_page_bitmap_range_init(
/*======================*/
	xb_page_bitmap*	bitmap,		/*!< in: bitmap to iterate over */
	ulint		space_id);	/*!< in: space id */

/****************************************************************//**
Get the next page id that has its bit set or cleared, i.e. equal to
bit_value.

@return page id */
ulint
xb_page_bitmap_range_get_next_bit(
/*==============================*/
	xb_page_bitmap_range*	bitmap_range,	/*!< in/out: bitmap range */
	ibool			bit_value);	/*!< in: bit value */

/****************************************************************//**
Free the bitmap range iterator. */
void
xb_page_bitmap_range_deinit(
/*========================*/
	xb_page_bitmap_range*	bitmap_range);	/*! in/out: bitmap range */

#endif
