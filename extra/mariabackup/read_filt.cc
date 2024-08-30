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
rf_pass_through_init(
	xb_read_filt_ctxt_t*	ctxt,	/*!<in/out: read filter context */
	const xb_fil_cur_t*	cursor)	/*!<in: file cursor */
{
	ctxt->offset = 0;
	ctxt->data_file_size = cursor->statinfo.st_size;
	ctxt->buffer_capacity = cursor->buf_size;
}

/****************************************************************//**
Get the next batch of pages for the pass-through read filter.  */
static
void
rf_pass_through_get_next_batch(
/*===========================*/
	xb_read_filt_ctxt_t*	ctxt,			/*!<in/out: read filter
							context */
	int64_t*		read_batch_start,	/*!<out: starting read
							offset in bytes for the
							next batch of pages */
	int64_t*		read_batch_len)		/*!<out: length in
							bytes of the next batch
							of pages */
{
	*read_batch_start = ctxt->offset;
	*read_batch_len = ctxt->data_file_size - ctxt->offset;

	if (*read_batch_len > (int64_t)ctxt->buffer_capacity) {
		*read_batch_len = ctxt->buffer_capacity;
	}

	ctxt->offset += *read_batch_len;
}

/* The pass-through read filter */
xb_read_filt_t rf_pass_through = {
	&rf_pass_through_init,
	&rf_pass_through_get_next_batch,
};
