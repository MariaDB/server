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

/* Source file cursor interface */

#ifndef FIL_CUR_H
#define FIL_CUR_H

#include <my_dir.h>
#include "read_filt.h"
#include "srv0start.h"
#include "srv0srv.h"

struct xb_fil_cur_t {
	pfs_os_file_t	file;		/*!< source file handle */
	fil_node_t*	node;		/*!< source tablespace node */
	char		rel_path[FN_REFLEN];
					/*!< normalized file path */
	char		abs_path[FN_REFLEN];
					/*!< absolute file path */
	MY_STAT		statinfo;	/*!< information about the file */
	page_size_t	page_size;	/*!< page size */
	xb_read_filt_t*	read_filter;	/*!< read filter */
	xb_read_filt_ctxt_t	read_filter_ctxt;
					/*!< read filter context */
	byte*		orig_buf;	/*!< read buffer */
	byte*		buf;		/*!< aligned pointer for orig_buf */
	size_t		buf_size;	/*!< buffer size in bytes */
	size_t		buf_read;	/*!< number of read bytes in buffer
					after the last cursor read */
	size_t		buf_npages;	/*!< number of pages in buffer after the
					last cursor read */
	ib_int64_t	buf_offset;	/*!< file offset of the first page in
					buffer */
	ulint		buf_page_no;	/*!< number of the first page in
					buffer */
	uint		thread_n;	/*!< thread number for diagnostics */
	ulint		space_id;	/*!< ID of tablespace */
	ulint		space_size;	/*!< space size in pages */

	/** TODO: remove this default constructor */
	xb_fil_cur_t() : page_size(0), read_filter(0), read_filter_ctxt() {}

	/** @return whether this is not a file-per-table tablespace */
	bool is_system() const
	{
		ut_ad(space_id != SRV_TMP_SPACE_ID);
		return(space_id == TRX_SYS_SPACE
		      || srv_is_undo_tablespace(space_id));
	}
};

typedef enum {
	XB_FIL_CUR_SUCCESS,
	XB_FIL_CUR_SKIP,
	XB_FIL_CUR_ERROR,
	XB_FIL_CUR_EOF
} xb_fil_cur_result_t;

/************************************************************************
Open a source file cursor and initialize the associated read filter.

@return XB_FIL_CUR_SUCCESS on success, XB_FIL_CUR_SKIP if the source file must
be skipped and XB_FIL_CUR_ERROR on error. */
xb_fil_cur_result_t
xb_fil_cur_open(
/*============*/
	xb_fil_cur_t*	cursor,		/*!< out: source file cursor */
	xb_read_filt_t*	read_filter,	/*!< in/out: the read filter */
	fil_node_t*	node,		/*!< in: source tablespace node */
	uint		thread_n,	/*!< thread number for diagnostics */
	ulonglong max_file_size = ULLONG_MAX);

/************************************************************************
Reads and verifies the next block of pages from the source
file. Positions the cursor after the last read non-corrupted page.

@return XB_FIL_CUR_SUCCESS if some have been read successfully, XB_FIL_CUR_EOF
if there are no more pages to read and XB_FIL_CUR_ERROR on error. */
xb_fil_cur_result_t
xb_fil_cur_read(
/*============*/
	xb_fil_cur_t*	cursor);	/*!< in/out: source file cursor */

/************************************************************************
Close the source file cursor opened with xb_fil_cur_open() and its
associated read filter. */
void
xb_fil_cur_close(
/*=============*/
	xb_fil_cur_t *cursor);	/*!< in/out: source file cursor */

/***********************************************************************
Extracts the relative path ("database/table.ibd") of a tablespace from a
specified possibly absolute path.

For user tablespaces both "./database/table.ibd" and
"/remote/dir/database/table.ibd" result in "database/table.ibd".

For system tablepsaces (i.e. When is_system is TRUE) both "/remote/dir/ibdata1"
and "./ibdata1" yield "ibdata1" in the output. */
const char *
xb_get_relative_path(
/*=================*/
	const char*	path,		/*!< in: tablespace path (either
			  		relative or absolute) */
	ibool		is_system);	/*!< in: TRUE for system tablespaces,
					i.e. when only the filename must be
					returned. */

#endif
