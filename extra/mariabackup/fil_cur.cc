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

/* Source file cursor implementation */

#include <my_base.h>

#include <fil0fil.h>
#include <fsp0fsp.h>
#include <srv0start.h>
#include <trx0sys.h>

#include "fil_cur.h"
#include "fil0crypt.h"
#include "fil0pagecompress.h"
#include "common.h"
#include "read_filt.h"
#include "xtrabackup.h"
#include "xb0xb.h"
#include "backup_debug.h"

/* Size of read buffer in pages (640 pages = 10M for 16K sized pages) */
#define XB_FIL_CUR_PAGES 640

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
	ibool		is_system)	/*!< in: TRUE for system tablespaces,
					i.e. when only the filename must be
					returned. */
{
	const char *next;
	const char *cur;
	const char *prev;

	prev = NULL;
	cur = path;

	while ((next = strchr(cur, OS_PATH_SEPARATOR)) != NULL) {

		prev = cur;
		cur = next + 1;
	}

	if (is_system) {

		return(cur);
	} else {

		return((prev == NULL) ? cur : prev);
	}

}

/**********************************************************************//**
Closes a file. */
static
void
xb_fil_node_close_file(
/*===================*/
	fil_node_t*	node)	/*!< in: file node */
{
	ibool	ret;

	mutex_enter(&fil_system->mutex);

	ut_ad(node);
	ut_a(node->n_pending == 0);
	ut_a(node->n_pending_flushes == 0);
	ut_a(!node->being_extended);

	if (!node->is_open()) {

		mutex_exit(&fil_system->mutex);

		return;
	}

	ret = os_file_close(node->handle);
	ut_a(ret);

	node->handle = OS_FILE_CLOSED;

	ut_a(fil_system->n_open > 0);
	fil_system->n_open--;

	if (node->space->purpose == FIL_TYPE_TABLESPACE &&
	    fil_is_user_tablespace_id(node->space->id)) {

		ut_a(UT_LIST_GET_LEN(fil_system->LRU) > 0);

		/* The node is in the LRU list, remove it */
		UT_LIST_REMOVE(fil_system->LRU, node);
	}

	mutex_exit(&fil_system->mutex);
}

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
	ulonglong max_file_size)
{
	bool	success;
	int err;
	/* Initialize these first so xb_fil_cur_close() handles them correctly
	in case of error */
	cursor->orig_buf = NULL;
	cursor->node = NULL;

	cursor->space_id = node->space->id;

	strncpy(cursor->abs_path, node->name, (sizeof cursor->abs_path) - 1);
	cursor->abs_path[(sizeof cursor->abs_path) - 1] = '\0';

	/* Get the relative path for the destination tablespace name, i.e. the
	one that can be appended to the backup root directory. Non-system
	tablespaces may have absolute paths for DATA DIRECTORY.
	We want to make "local" copies for the backup. */
	strncpy(cursor->rel_path,
		xb_get_relative_path(cursor->abs_path, cursor->is_system()),
		(sizeof cursor->rel_path) - 1);
	cursor->rel_path[(sizeof cursor->rel_path) - 1] = '\0';

	/* In the backup mode we should already have a tablespace handle created
	by fil_ibd_load() unless it is a system
	tablespace. Otherwise we open the file here. */
	if (!node->is_open()) {
		ut_ad(cursor->is_system()
		      || srv_operation == SRV_OPERATION_RESTORE_DELTA
		      || xb_close_files);

		node->handle = os_file_create_simple_no_error_handling(
			0, node->name,
			OS_FILE_OPEN,
			OS_FILE_READ_ALLOW_DELETE, true, &success);
		if (!success) {
			/* The following call prints an error message */
			os_file_get_last_error(TRUE);

			msg(thread_n, "mariabackup: error: cannot open "
			    "tablespace %s", cursor->abs_path);

			return(XB_FIL_CUR_SKIP);
		}
		mutex_enter(&fil_system->mutex);

		fil_system->n_open++;

		if (node->space->purpose == FIL_TYPE_TABLESPACE &&
		    fil_is_user_tablespace_id(node->space->id)) {

			/* Put the node to the LRU list */
			UT_LIST_ADD_FIRST(fil_system->LRU, node);
		}

		mutex_exit(&fil_system->mutex);
	}

	ut_ad(node->is_open());

	cursor->node = node;
	cursor->file = node->handle;
#ifdef _WIN32
    HANDLE hDup;
    DuplicateHandle(GetCurrentProcess(),cursor->file.m_file,
		GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
	int filenr = _open_osfhandle((intptr_t)hDup, 0);
	if (filenr < 0) {
		err = EINVAL;
	}
	else {
		err = _fstat64(filenr, &cursor->statinfo);
		close(filenr);
	}
#else
	err = fstat(cursor->file.m_file, &cursor->statinfo);
#endif
	if (max_file_size < (ulonglong)cursor->statinfo.st_size) {
		cursor->statinfo.st_size = (ulonglong)max_file_size;
	}
	if (err) {
		msg(thread_n, "mariabackup: error: cannot fstat %s",
		    cursor->abs_path);

		xb_fil_cur_close(cursor);

		return(XB_FIL_CUR_SKIP);
	}

	if (srv_file_flush_method == SRV_O_DIRECT
	    || srv_file_flush_method == SRV_O_DIRECT_NO_FSYNC) {

		os_file_set_nocache(cursor->file, node->name, "OPEN");
	}

	posix_fadvise(cursor->file, 0, 0, POSIX_FADV_SEQUENTIAL);

	const page_size_t page_size(node->space->flags);
	cursor->page_size = page_size;

	/* Allocate read buffer */
	cursor->buf_size = XB_FIL_CUR_PAGES * page_size.physical();
	cursor->orig_buf = static_cast<byte *>
		(malloc(cursor->buf_size + UNIV_PAGE_SIZE));
	cursor->buf = static_cast<byte *>
		(ut_align(cursor->orig_buf, UNIV_PAGE_SIZE));

	cursor->buf_read = 0;
	cursor->buf_npages = 0;
	cursor->buf_offset = 0;
	cursor->buf_page_no = 0;
	cursor->thread_n = thread_n;

	if (!node->space->crypt_data
	    && os_file_read(IORequestRead,
			    node->handle, cursor->buf, 0,
			    page_size.physical()) == DB_SUCCESS) {
		mutex_enter(&fil_system->mutex);
		if (!node->space->crypt_data) {
			node->space->crypt_data
				= fil_space_read_crypt_data(page_size,
							    cursor->buf);
		}
		mutex_exit(&fil_system->mutex);
	}

	cursor->space_size = (ulint)(cursor->statinfo.st_size
				     / page_size.physical());

	cursor->read_filter = read_filter;
	cursor->read_filter->init(&cursor->read_filter_ctxt, cursor,
				  node->space->id);

	return(XB_FIL_CUR_SUCCESS);
}

static bool page_is_corrupted(const byte *page, ulint page_no,
			      const xb_fil_cur_t *cursor,
			      const fil_space_t *space)
{
	byte tmp_frame[UNIV_PAGE_SIZE_MAX];
	byte tmp_page[UNIV_PAGE_SIZE_MAX];
	const ulint page_size = cursor->page_size.physical();
	ulint page_type = mach_read_from_2(page + FIL_PAGE_TYPE);

	/* We ignore the doublewrite buffer pages.*/
	if (cursor->space_id == TRX_SYS_SPACE
	    && page_no >= FSP_EXTENT_SIZE
	    && page_no < FSP_EXTENT_SIZE * 3) {
		return false;
	}

	/* Validate page number. */
	if (mach_read_from_4(page + FIL_PAGE_OFFSET) != page_no
	    && cursor->space_id != TRX_SYS_SPACE) {
		/* On pages that are not all zero, the
		page number must match.

		There may be a mismatch on tablespace ID,
		because files may be renamed during backup.
		We disable the page number check
		on the system tablespace, because it may consist
		of multiple files, and here we count the pages
		from the start of each file.)

		The first 38 and last 8 bytes are never encrypted. */
		const ulint* p = reinterpret_cast<const ulint*>(page);
		const ulint* const end = reinterpret_cast<const ulint*>(
			page + page_size);
		do {
			if (*p++) {
				return true;
			}
		} while (p != end);

		/* Whole zero page is valid. */
		return false;
	}

	/* Validate encrypted pages. The first page is never encrypted.
	In the system tablespace, the first page would be written with
	FIL_PAGE_FILE_FLUSH_LSN at shutdown, and if the LSN exceeds
	4,294,967,295, the mach_read_from_4() below would wrongly
	interpret the page as encrypted. We prevent that by checking
	page_no first. */
	if (page_no
	    && mach_read_from_4(page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION)
	    && (opt_encrypted_backup
		|| (space->crypt_data
		    && space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED))) {

		if (!fil_space_verify_crypt_checksum(page, cursor->page_size))
			return true;

		/* Compressed encrypted need to be decrypted
		and decompressed for verification. */
		if (page_type != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED
		    && !opt_extended_validation)
			return false;

		memcpy(tmp_page, page, page_size);

		if (!space->crypt_data
		    || space->crypt_data->type == CRYPT_SCHEME_UNENCRYPTED
		    || !fil_space_decrypt(space, tmp_frame, tmp_page)) {
			return true;
		}

		if (page_type != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
			return buf_page_is_corrupted(true, tmp_page,
						     cursor->page_size, space);
		}
	}

	if (page_type == FIL_PAGE_PAGE_COMPRESSED) {
		memcpy(tmp_page, page, page_size);
	}

	if (page_type == FIL_PAGE_PAGE_COMPRESSED
	    || page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		ulint decomp = fil_page_decompress(tmp_frame, tmp_page);
		page_type = mach_read_from_2(tmp_page + FIL_PAGE_TYPE);

		return (!decomp
			|| (decomp != srv_page_size
			    && cursor->page_size.is_compressed())
			|| page_type == FIL_PAGE_PAGE_COMPRESSED
			|| page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED
			|| buf_page_is_corrupted(true, tmp_page,
						 cursor->page_size, space));
	}

	return buf_page_is_corrupted(true, page, cursor->page_size, space);
}

/** Reads and verifies the next block of pages from the source
file. Positions the cursor after the last read non-corrupted page.
@param[in,out] cursor source file cursor
@param[out] corrupted_pages adds corrupted pages if
opt_log_innodb_page_corruption is set
@return XB_FIL_CUR_SUCCESS if some have been read successfully, XB_FIL_CUR_EOF
if there are no more pages to read and XB_FIL_CUR_ERROR on error. */
xb_fil_cur_result_t xb_fil_cur_read(xb_fil_cur_t*	cursor,
                                    CorruptedPages &corrupted_pages)
{
	byte*			page;
	ulint			i;
	ulint			npages;
	ulint			retry_count;
	xb_fil_cur_result_t	ret;
	ib_int64_t		offset;
	ib_int64_t		to_read;
	const ulint		page_size = cursor->page_size.physical();
	xb_ad(!cursor->is_system() || page_size == UNIV_PAGE_SIZE);

	cursor->read_filter->get_next_batch(&cursor->read_filter_ctxt,
					    &offset, &to_read);

	if (to_read == 0LL) {
		return(XB_FIL_CUR_EOF);
	}

	if (to_read > (ib_int64_t) cursor->buf_size) {
		to_read = (ib_int64_t) cursor->buf_size;
	}

	xb_a(to_read > 0 && to_read <= 0xFFFFFFFFLL);

	if ((to_read & ~(page_size - 1))
	    && offset + to_read == cursor->statinfo.st_size) {

		if (to_read < (ib_int64_t) page_size) {
			msg(cursor->thread_n, "Warning: junk at the end of "
			    "%s, offset = %llu, to_read = %llu",cursor->abs_path, (ulonglong) offset, (ulonglong) to_read);
			return(XB_FIL_CUR_EOF);
		}

		to_read = (ib_int64_t) (((ulint) to_read) &
					~(page_size - 1));
	}

	xb_a((to_read & (page_size - 1)) == 0);

	npages = (ulint) (to_read / page_size);

	retry_count = 10;
	ret = XB_FIL_CUR_SUCCESS;

	fil_space_t *space = fil_space_acquire_for_io(cursor->space_id);

	if (!space) {
		return XB_FIL_CUR_ERROR;
	}

read_retry:
	xtrabackup_io_throttling();

	cursor->buf_read = 0;
	cursor->buf_npages = 0;
	cursor->buf_offset = offset;
	cursor->buf_page_no = (ulint)(offset / page_size);

	if (os_file_read(IORequestRead, cursor->file, cursor->buf, offset,
			  (ulint) to_read) != DB_SUCCESS) {
		ret = XB_FIL_CUR_ERROR;
		goto func_exit;
	}
	/* check pages for corruption and re-read if necessary. i.e. in case of
	partially written pages */
	for (page = cursor->buf, i = 0; i < npages;
	     page += page_size, i++) {
		ulint page_no = cursor->buf_page_no + i;

		if (page_is_corrupted(page, page_no, cursor, space)){
			retry_count--;

			if (retry_count == 0) {
				const char *ignore_corruption_warn = opt_log_innodb_page_corruption ?
					" WARNING!!! The corruption is ignored due to"
					" log-innodb-page-corruption option, the backup can contain"
					" corrupted data." : "";
				msg(cursor->thread_n,
				    "Error: failed to read page after "
				    "10 retries. File %s seems to be "
				    "corrupted.%s", cursor->abs_path, ignore_corruption_warn);
				buf_page_print(page, cursor->page_size);
				if (opt_log_innodb_page_corruption) {
					corrupted_pages.add_page(cursor->node->name, cursor->node->space->id,
						page_no);
					retry_count = 1;
				}
				else {
					ret = XB_FIL_CUR_ERROR;
					break;
				}
			}
			else {
				msg(cursor->thread_n, "Database page corruption detected at page "
				    ULINTPF ", retrying...",
				    page_no);
				os_thread_sleep(100000);
				goto read_retry;
			}
		}
		DBUG_EXECUTE_FOR_KEY("add_corrupted_page_for", cursor->node->space->name,
			{
				ulint corrupted_page_no = strtoul(dbug_val, NULL, 10);
				if (page_no == corrupted_page_no)
					corrupted_pages.add_page(cursor->node->name, cursor->node->space->id,
						corrupted_page_no);
			});
		cursor->buf_read += page_size;
		cursor->buf_npages++;
	}

	posix_fadvise(cursor->file, offset, to_read, POSIX_FADV_DONTNEED);
func_exit:
	fil_space_release_for_io(space);
	return(ret);
}

/************************************************************************
Close the source file cursor opened with xb_fil_cur_open() and its
associated read filter. */
void
xb_fil_cur_close(
/*=============*/
	xb_fil_cur_t *cursor)	/*!< in/out: source file cursor */
{
	if (cursor->read_filter) {
		cursor->read_filter->deinit(&cursor->read_filter_ctxt);
	}

	free(cursor->orig_buf);

	if (cursor->node != NULL) {
		xb_fil_node_close_file(cursor->node);
		cursor->file = OS_FILE_CLOSED;
	}
}
