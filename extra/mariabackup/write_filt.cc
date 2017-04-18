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

/* Page write filters implementation */

#include <my_base.h>
#include "common.h"
#include "write_filt.h"
#include "fil_cur.h"
#include "xtrabackup.h"

/************************************************************************
Write-through page write filter. */
static my_bool wf_wt_init(xb_write_filt_ctxt_t *ctxt, char *dst_name,
			  xb_fil_cur_t *cursor);
static my_bool wf_wt_process(xb_write_filt_ctxt_t *ctxt, ds_file_t *dstfile);

xb_write_filt_t wf_write_through = {
	&wf_wt_init,
	&wf_wt_process,
	NULL,
	NULL
};

/************************************************************************
Incremental page write filter. */
static my_bool wf_incremental_init(xb_write_filt_ctxt_t *ctxt, char *dst_name,
				   xb_fil_cur_t *cursor);
static my_bool wf_incremental_process(xb_write_filt_ctxt_t *ctxt,
				      ds_file_t *dstfile);
static my_bool wf_incremental_finalize(xb_write_filt_ctxt_t *ctxt,
				       ds_file_t *dstfile);
static void wf_incremental_deinit(xb_write_filt_ctxt_t *ctxt);

xb_write_filt_t wf_incremental = {
	&wf_incremental_init,
	&wf_incremental_process,
	&wf_incremental_finalize,
	&wf_incremental_deinit
};

/************************************************************************
Initialize incremental page write filter.

@return TRUE on success, FALSE on error. */
static my_bool
wf_incremental_init(xb_write_filt_ctxt_t *ctxt, char *dst_name,
		    xb_fil_cur_t *cursor)
{
	char				meta_name[FN_REFLEN];
	xb_delta_info_t			info;
	ulint				buf_size;
	xb_wf_incremental_ctxt_t	*cp =
		&(ctxt->u.wf_incremental_ctxt);

	ctxt->cursor = cursor;

	/* allocate buffer for incremental backup (4096 pages) */
	buf_size = (UNIV_PAGE_SIZE_MAX / 4 + 1) * UNIV_PAGE_SIZE_MAX;
	cp->delta_buf_base = static_cast<byte *>(ut_malloc(buf_size));
	memset(cp->delta_buf_base, 0, buf_size);
	cp->delta_buf = static_cast<byte *>
		(ut_align(cp->delta_buf_base, UNIV_PAGE_SIZE_MAX));

	/* write delta meta info */
	snprintf(meta_name, sizeof(meta_name), "%s%s", dst_name,
		 XB_DELTA_INFO_SUFFIX);
	info.page_size = cursor->page_size;
	info.zip_size = cursor->zip_size;
	info.space_id = cursor->space_id;
	if (!xb_write_delta_metadata(meta_name, &info)) {
		msg("[%02u] xtrabackup: Error: "
		    "failed to write meta info for %s\n",
		    cursor->thread_n, cursor->rel_path);
		return(FALSE);
	}

	/* change the target file name, since we are only going to write
	delta pages */
	strcat(dst_name, ".delta");

	mach_write_to_4(cp->delta_buf, 0x78747261UL); /*"xtra"*/
	cp->npages = 1;

	return(TRUE);
}

/************************************************************************
Run the next batch of pages through incremental page write filter.

@return TRUE on success, FALSE on error. */
static my_bool
wf_incremental_process(xb_write_filt_ctxt_t *ctxt, ds_file_t *dstfile)
{
	ulint				i;
	xb_fil_cur_t			*cursor = ctxt->cursor;
	ulint				page_size = cursor->page_size;
	byte				*page;
	xb_wf_incremental_ctxt_t	*cp = &(ctxt->u.wf_incremental_ctxt);

	for (i = 0, page = cursor->buf; i < cursor->buf_npages;
	     i++, page += page_size) {

		if (incremental_lsn >= mach_read_from_8(page + FIL_PAGE_LSN)) {

			continue;
		}

		/* updated page */
		if (cp->npages == page_size / 4) {
			/* flush buffer */
			if (ds_write(dstfile, cp->delta_buf,
				     cp->npages * page_size)) {
				return(FALSE);
			}

			/* clear buffer */
			memset(cp->delta_buf, 0, page_size / 4 * page_size);
			/*"xtra"*/
			mach_write_to_4(cp->delta_buf, 0x78747261UL);
			cp->npages = 1;
		}

		mach_write_to_4(cp->delta_buf + cp->npages * 4,
				cursor->buf_page_no + i);
		memcpy(cp->delta_buf + cp->npages * page_size, page,
		       page_size);

		cp->npages++;
	}

	return(TRUE);
}

/************************************************************************
Flush the incremental page write filter's buffer.

@return TRUE on success, FALSE on error. */
static my_bool
wf_incremental_finalize(xb_write_filt_ctxt_t *ctxt, ds_file_t *dstfile)
{
	xb_fil_cur_t			*cursor = ctxt->cursor;
	ulint				page_size = cursor->page_size;
	xb_wf_incremental_ctxt_t	*cp = &(ctxt->u.wf_incremental_ctxt);

	if (cp->npages != page_size / 4) {
		mach_write_to_4(cp->delta_buf + cp->npages * 4, 0xFFFFFFFFUL);
	}

	/* Mark the final block */
	mach_write_to_4(cp->delta_buf, 0x58545241UL); /*"XTRA"*/

	/* flush buffer */
	if (ds_write(dstfile, cp->delta_buf, cp->npages * page_size)) {
		return(FALSE);
	}

	return(TRUE);
}

/************************************************************************
Free the incremental page write filter's buffer. */
static void
wf_incremental_deinit(xb_write_filt_ctxt_t *ctxt)
{
	xb_wf_incremental_ctxt_t	*cp = &(ctxt->u.wf_incremental_ctxt);

	if (cp->delta_buf_base != NULL) {
		ut_free(cp->delta_buf_base);
	}
}

/************************************************************************
Initialize the write-through page write filter.

@return TRUE on success, FALSE on error. */
static my_bool
wf_wt_init(xb_write_filt_ctxt_t *ctxt, char *dst_name __attribute__((unused)),
	   xb_fil_cur_t *cursor)
{
	ctxt->cursor = cursor;

	return(TRUE);
}

/************************************************************************
Write the next batch of pages to the destination datasink.

@return TRUE on success, FALSE on error. */
static my_bool
wf_wt_process(xb_write_filt_ctxt_t *ctxt, ds_file_t *dstfile)
{
	xb_fil_cur_t			*cursor = ctxt->cursor;

	if (ds_write(dstfile, cursor->buf, cursor->buf_read)) {
		return(FALSE);
	}

	return(TRUE);
}
