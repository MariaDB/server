/******************************************************
MariaBackup: hot backup tool for InnoDB
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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

/* Page write filters implementation */

#include <my_global.h>
#include <my_base.h>
#include "common.h"
#include "write_filt.h"
#include "fil_cur.h"
#include "xtrabackup.h"

/************************************************************************
Write-through page write filter. */
static my_bool wf_wt_init(xb_write_filt_ctxt_t *ctxt, char *dst_name,
			  xb_fil_cur_t *cursor, CorruptedPages *corrupted_pages);
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
				   xb_fil_cur_t *cursor, CorruptedPages *corrupted_pages);
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
		    xb_fil_cur_t *cursor, CorruptedPages *corrupted_pages)
{
	char				meta_name[FN_REFLEN];
	xb_wf_incremental_ctxt_t	*cp =
		&(ctxt->wf_incremental_ctxt);

	ctxt->cursor = cursor;

	/* allocate buffer for incremental backup (4096 pages) */
	cp->delta_buf_size = (cursor->page_size / 4) * cursor->page_size;
	cp->delta_buf = (unsigned char *)my_large_malloc(&cp->delta_buf_size, MYF(0));

	if (!cp->delta_buf) {
		msg(cursor->thread_n,"Can't allocate %zu bytes",
			(size_t) cp->delta_buf_size);
		return (FALSE);
	}

	/* write delta meta info */
	snprintf(meta_name, sizeof(meta_name), "%s%s", dst_name,
		 XB_DELTA_INFO_SUFFIX);
	const xb_delta_info_t	info(cursor->page_size, cursor->zip_size,
				     cursor->space_id);
	if (!xb_write_delta_metadata(meta_name, &info)) {
		msg(cursor->thread_n,"Error: "
		    "failed to write meta info for %s",
		    cursor->rel_path);
		return(FALSE);
	}

	/* change the target file name, since we are only going to write
	delta pages */
	strcat(dst_name, ".delta");

	mach_write_to_4(cp->delta_buf, 0x78747261UL); /*"xtra"*/

	cp->npages = 1;
	cp->corrupted_pages = corrupted_pages;

	return(TRUE);
}

/************************************************************************
Run the next batch of pages through incremental page write filter.

@return TRUE on success, FALSE on error. */
static my_bool
wf_incremental_process(xb_write_filt_ctxt_t *ctxt, ds_file_t *dstfile)
{
	unsigned				i;
	xb_fil_cur_t			*cursor = ctxt->cursor;
	byte				*page;
	const ulint			page_size = cursor->page_size;
	xb_wf_incremental_ctxt_t	*cp = &(ctxt->wf_incremental_ctxt);

	for (i = 0, page = cursor->buf; i < cursor->buf_npages;
	     i++, page += page_size) {

		if ((!cp->corrupted_pages ||
		     !cp->corrupted_pages->contains({cursor->node->space->id,
			 cursor->buf_page_no + i})) &&
				incremental_lsn >= mach_read_from_8(page + FIL_PAGE_LSN))
			continue;

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
	const ulint			page_size = cursor->page_size;
	xb_wf_incremental_ctxt_t	*cp = &(ctxt->wf_incremental_ctxt);

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
	xb_wf_incremental_ctxt_t	*cp = &(ctxt->wf_incremental_ctxt);
	my_large_free(cp->delta_buf, cp->delta_buf_size);
}

/************************************************************************
Initialize the write-through page write filter.

@return TRUE on success, FALSE on error. */
static my_bool
wf_wt_init(xb_write_filt_ctxt_t *ctxt, char *dst_name __attribute__((unused)),
	   xb_fil_cur_t *cursor, CorruptedPages *)
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
