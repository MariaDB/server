/******************************************************
XtraBackup: hot backup tool for InnoDB
(c) 2009-2013 Percona LLC and/or its affiliates.
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

/* Page write filter interface */

#ifndef XB_WRITE_FILT_H
#define XB_WRITE_FILT_H

#include "fil_cur.h"
#include "datasink.h"
#include "compact.h"

/* Incremental page filter context */
typedef struct {
	byte		*delta_buf_base;
	byte		*delta_buf;
	ulint		 npages;
} xb_wf_incremental_ctxt_t;

/* Page filter context used as an opaque structure by callers */
typedef struct {
	xb_fil_cur_t	*cursor;
	union {
		xb_wf_incremental_ctxt_t	wf_incremental_ctxt;
		xb_wf_compact_ctxt_t		wf_compact_ctxt;
	} u;
} xb_write_filt_ctxt_t;


typedef struct {
	my_bool	(*init)(xb_write_filt_ctxt_t *ctxt, char *dst_name,
			xb_fil_cur_t *cursor);
	my_bool	(*process)(xb_write_filt_ctxt_t *ctxt, ds_file_t *dstfile);
	my_bool	(*finalize)(xb_write_filt_ctxt_t *, ds_file_t *dstfile);
	void (*deinit)(xb_write_filt_ctxt_t *);
} xb_write_filt_t;

extern xb_write_filt_t wf_write_through;
extern xb_write_filt_t wf_incremental;
extern xb_write_filt_t wf_compact;

#endif /* XB_WRITE_FILT_H */
