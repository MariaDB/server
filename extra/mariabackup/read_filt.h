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

/* Data file read filter interface */

#ifndef XB_READ_FILT_H
#define XB_READ_FILT_H

#include <cstdint>
#include <cstddef>

struct xb_fil_cur_t;

/* The read filter context */
struct xb_read_filt_ctxt_t {
	int64_t		offset;		/*!< current file offset */
	int64_t		data_file_size;	/*!< data file size */
	size_t		buffer_capacity;/*!< read buffer capacity */
};

/* The read filter */
struct xb_read_filt_t {
	void (*init)(xb_read_filt_ctxt_t* ctxt,
		     const xb_fil_cur_t* cursor);
	void (*get_next_batch)(xb_read_filt_ctxt_t* ctxt,
			       int64_t* read_batch_start,
			       int64_t* read_batch_len);
};

extern xb_read_filt_t rf_pass_through;

#endif
