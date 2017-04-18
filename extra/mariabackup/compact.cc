/******************************************************
XtraBackup: hot backup tool for InnoDB
(c) 2009-2014 Percona LLC and/or its affiliates.
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

/* Compact backups implementation */

#include <my_base.h>
#include <table.h>

#include <univ.i>
#include <dict0mem.h>
#include <dict0priv.h>
#include <fsp0fsp.h>
#include <handler0alter.h>
#include <ibuf0ibuf.h>
#include <page0page.h>
#include <row0merge.h>
#include "common.h"
#include "write_filt.h"
#include "fil_cur.h"
#include "xtrabackup.h"
#include "ds_buffer.h"
#include "xb0xb.h"

/* Number of the first primary key page in an .ibd file */
#define XB_FIRST_CLUSTERED_INDEX_PAGE_NO 3

/* Suffix for page map files */
#define XB_PAGE_MAP_SUFFIX ".pmap"
#define XB_TMPFILE_SUFFIX ".tmp"

/* Page range */
struct page_range_t {
	ulint	from;			/*!< range start */
	ulint	to;			/*!< range end */
};

/* Cursor in a page map file */
struct page_map_cursor_t {
	File		fd;	/*!< file descriptor */
	IO_CACHE	cache;	/*!< IO_CACHE associated with fd */
};

/* Table descriptor for the index rebuild operation */
struct index_rebuild_table_t {
	char*	name;					/* table name */
	ulint	space_id;				/* space ID */
	UT_LIST_NODE_T(index_rebuild_table_t)	list;	/* list node */
};

/* Thread descriptor for the index rebuild operation */
struct index_rebuild_thread_t {
	ulint		num;    /* thread number */
	pthread_t	id;	/* thread ID */
};

/* Empty page use to replace skipped pages in the data files */
static byte		empty_page[UNIV_PAGE_SIZE_MAX];
static const char	compacted_page_magic[] = "COMPACTP";
static const size_t	compacted_page_magic_size =
	sizeof(compacted_page_magic) - 1;
static const ulint	compacted_page_magic_offset = FIL_PAGE_DATA;

/* Mutex protecting table_list */
static pthread_mutex_t					table_list_mutex;
/* List of tablespaces to process by the index rebuild operation */
static UT_LIST_BASE_NODE_T(index_rebuild_table_t)	table_list;


/************************************************************************
Compact page filter. */
static my_bool wf_compact_init(xb_write_filt_ctxt_t *ctxt, char *dst_name,
			       xb_fil_cur_t *cursor);
static my_bool wf_compact_process(xb_write_filt_ctxt_t *ctxt,
				  ds_file_t *dstfile);
static my_bool wf_compact_finalize(xb_write_filt_ctxt_t *ctxt,
				   ds_file_t *dstfile);
xb_write_filt_t wf_compact = {
	&wf_compact_init,
	&wf_compact_process,
	&wf_compact_finalize,
	NULL
};

/************************************************************************
Initialize the compact page filter.

@return TRUE on success, FALSE on error. */
static my_bool
wf_compact_init(xb_write_filt_ctxt_t *ctxt,
		char *dst_name __attribute__((unused)), xb_fil_cur_t *cursor)
{
	xb_wf_compact_ctxt_t	*cp = &(ctxt->u.wf_compact_ctxt);
	char			 page_map_name[FN_REFLEN];
	MY_STAT			 mystat;

	ctxt->cursor = cursor;
	cp->clustered_index_found = FALSE;
	cp->inside_skipped_range = FALSE;
	cp->free_limit = 0;

	/* Don't compact the system table space */
	cp->skip = cursor->is_system;
	if (cp->skip) {
		return(TRUE);
	}

	snprintf(page_map_name, sizeof(page_map_name), "%s%s", dst_name,
		 XB_PAGE_MAP_SUFFIX);

	cp->ds_buffer = ds_create(xtrabackup_target_dir, DS_TYPE_BUFFER);
	if (cp->ds_buffer == NULL) {
		return(FALSE);
	}

	ds_set_pipe(cp->ds_buffer, ds_meta);

	memset(&mystat, 0, sizeof(mystat));
	mystat.st_mtime = my_time(0);
	cp->buffer = ds_open(cp->ds_buffer, page_map_name, &mystat);
	if (cp->buffer == NULL) {
		msg("xtrabackup: Error: cannot open output stream for %s\n",
		    page_map_name);
		return(FALSE);
	}

	return(TRUE);
}

/************************************************************************
Check if the specified page should be skipped. We currently skip all
non-clustered index pages for compact backups.

@return TRUE if the page should be skipped. */
static my_bool
check_if_skip_page(xb_wf_compact_ctxt_t *cp, xb_fil_cur_t *cursor, ulint offset)
{
	byte		*page;
	ulint		 page_no;
	ulint		 page_type;
	index_id_t	 index_id;


	xb_ad(cursor->is_system == FALSE);

	page = cursor->buf + cursor->page_size * offset;
	page_no = cursor->buf_page_no + offset;
	page_type = fil_page_get_type(page);

	if (UNIV_UNLIKELY(page_no == 0)) {

		cp->free_limit = mach_read_from_4(page + FSP_HEADER_OFFSET +
						  FSP_FREE_LIMIT);
	} else if (UNIV_UNLIKELY(page_no == XB_FIRST_CLUSTERED_INDEX_PAGE_NO)) {

		xb_ad(cp->clustered_index_found == FALSE);

		if (page_type != FIL_PAGE_INDEX) {

			/* Uninitialized clustered index root page, there's
			nothing we can do to compact the space.*/

			msg("[%02u] Uninitialized page type value (%lu) in the "
			    "clustered index root page of tablespace %s. "
			    "Will not be compacted.\n",
			    cursor->thread_n,
			    page_type, cursor->rel_path);

			cp->skip = TRUE;

			return(FALSE);
		}

		cp->clustered_index =
			mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
		cp->clustered_index_found = TRUE;
	} else if (UNIV_UNLIKELY(page_no >= cp->free_limit)) {

		/* Skip unused pages above free limit, if that value is set in
		the FSP header.*/

		return(cp->free_limit > 0);
	} else if (cp->clustered_index_found && page_type == FIL_PAGE_INDEX) {

		index_id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
		if (index_id != cp->clustered_index) {

			ulint	fseg_hdr_space =
				mach_read_from_4(page + PAGE_HEADER +
						 PAGE_BTR_SEG_TOP);
			ulint	fseg_hdr_page_no =
				mach_read_from_4(page + PAGE_HEADER +
						 PAGE_BTR_SEG_TOP + 4);
			ulint fseg_hdr_offset =
				mach_read_from_2(page + PAGE_HEADER +
						 PAGE_BTR_SEG_TOP + 8);

			/* Don't skip root index pages, i.e. the ones where the
			above fields are defined. We need root index pages to be
			able to correctly drop the indexes later, as they
			contain fseg inode pointers. */

			return(fseg_hdr_space == 0 &&
			       fseg_hdr_page_no == 0 &&
			       fseg_hdr_offset == 0);
		}
	}

	return(FALSE);
}

/************************************************************************
Run the next batch of pages through the compact page filter.

@return TRUE on success, FALSE on error. */
static my_bool
wf_compact_process(xb_write_filt_ctxt_t *ctxt, ds_file_t *dstfile)
{
	xb_fil_cur_t		*cursor = ctxt->cursor;
	ulint			 page_size = cursor->page_size;
	byte			*page;
	byte 			*buf_end;
	byte			*write_from;
	xb_wf_compact_ctxt_t	*cp = &(ctxt->u.wf_compact_ctxt);
	ulint 			i;
	ulint			page_no;
	byte			tmp[4];

	if (cp->skip) {
		return(!ds_write(dstfile, cursor->buf, cursor->buf_read));
	}

	write_from = NULL;
	buf_end = cursor->buf + cursor->buf_read;
	for (i = 0, page = cursor->buf; page < buf_end;
	     i++, page += page_size) {

		page_no = cursor->buf_page_no + i;

		if (!check_if_skip_page(cp, cursor, i)) {

			if (write_from == NULL) {
				write_from = page;
			}

			if (cp->inside_skipped_range) {
				cp->inside_skipped_range = FALSE;

				/* Write the last range endpoint to the
				skipped pages map */

				xb_ad(page_no > 0);
				mach_write_to_4(tmp, page_no - 1);
				if (ds_write(cp->buffer, tmp, sizeof(tmp))) {
					return(FALSE);
				}
			}
			continue;
		}

		if (write_from != NULL) {

			/* The first skipped page in this block, write the
			non-skipped ones to the data file */

			if (ds_write(dstfile, write_from, page - write_from)) {
				return(FALSE);
			}

			write_from = NULL;
		}

		if (!cp->inside_skipped_range) {

			/* The first skipped page in range, write the first
			range endpoint to the skipped pages map */

			cp->inside_skipped_range = TRUE;

			mach_write_to_4(tmp, page_no);
			if (ds_write(cp->buffer, tmp, sizeof(tmp))) {
				return(FALSE);
			}
		}
	}

	/* Write the remaining pages in the buffer, if any */
	if (write_from != NULL &&
	    ds_write(dstfile, write_from, buf_end - write_from)) {
		return(FALSE);
	}

	return(TRUE);
}

/************************************************************************
Close the compact filter's page map stream.

@return TRUE on success, FALSE on error. */
static my_bool
wf_compact_finalize(xb_write_filt_ctxt_t *ctxt,
		    ds_file_t *dstfile __attribute__((unused)))
{
	xb_fil_cur_t		*cursor = ctxt->cursor;
	xb_wf_compact_ctxt_t	*cp = &(ctxt->u.wf_compact_ctxt);
	my_bool			rc = TRUE;

	/* Write the last endpoint of the current range, if the last pages of
	the space have been skipped. */
	if (cp->inside_skipped_range) {
		byte	tmp[4];

		mach_write_to_4(tmp, cursor->space_size - 1);
		if (ds_write(cp->buffer, tmp, sizeof(tmp))) {
			return(FALSE);
		}

		cp->inside_skipped_range = FALSE;
	}

	if (cp->buffer) {
		if (ds_close(cp->buffer)) {
			rc = FALSE;
		}
	}
	if (cp->ds_buffer) {
		ds_destroy(cp->ds_buffer);
	}

	return(rc);
}

/************************************************************************
Open a page map file and return a cursor.

@return page map cursor, or NULL if the file doesn't exist. */
static page_map_cursor_t *
page_map_file_open(const char *path)
{
	MY_STAT			 statinfo;
	page_map_cursor_t	*pmap_cur;
	int			 rc;

	if (my_stat(path, &statinfo, MYF(0)) == NULL) {

		return(NULL);
	}

	/* The maximum possible page map file corresponds to a 64 TB tablespace
	and the worst case when every other page was skipped. That is, 2^32/2
	page ranges = 16 GB. */
	xb_a(statinfo.st_size < (off_t) 16 * 1024 * 1024 * 1024);

	/* Must be a series of 8-byte tuples */
	xb_a(statinfo.st_size % 8 == 0);

	pmap_cur = (page_map_cursor_t *) my_malloc(sizeof(page_map_cursor_t),
						   MYF(MY_FAE));

	pmap_cur->fd = my_open(path, O_RDONLY, MYF(MY_WME));
	xb_a(pmap_cur->fd != 0);

	rc = init_io_cache(&pmap_cur->cache, pmap_cur->fd, 0, READ_CACHE,
			   0, 0, MYF(MY_WME));
	xb_a(rc == 0);

	return(pmap_cur);
}

/************************************************************************
Read the next range from a page map file and update the cursor.

@return TRUE on success, FALSE on end-of-file. */
static ibool
page_map_file_next(page_map_cursor_t *pmap_cur, page_range_t *range)
{
	byte buf[8];

	xb_ad(pmap_cur != NULL);

	if (my_b_read(&pmap_cur->cache, buf, sizeof(buf))) {
		return(FALSE);
	}

	range->from = mach_read_from_4(buf);
	range->to = mach_read_from_4(buf + 4);

	return(TRUE);
}

/************************************************************************
Close the page map cursor.*/
static void
page_map_file_close(page_map_cursor_t *pmap_cur)
{
	int	rc;

	xb_ad(pmap_cur != NULL);

	rc = end_io_cache(&pmap_cur->cache);
	xb_a(rc == 0);

	posix_fadvise(pmap_cur->fd, 0, 0, POSIX_FADV_DONTNEED);

	rc = my_close(pmap_cur->fd, MY_WME);
	xb_a(rc == 0);

	my_free(pmap_cur);
}

/****************************************************************************
Expand a single data file according to the skipped pages maps created by
--compact.

@return TRUE on success, FALSE on failure. */
static my_bool
xb_expand_file(fil_node_t *node)
{
	char			 pmapfile_path[FN_REFLEN];
	char			 tmpfile_path[FN_REFLEN];
	xb_fil_cur_t		 cursor;
	xb_fil_cur_result_t	 res;
	ds_ctxt_t		*ds_local;
	ds_ctxt_t		*ds_buffer;
	ds_file_t		*tmpfile;
	my_bool			 success = FALSE;
	ulint			 i;
	byte			*page;
	ulint			 page_expected_no;
	page_map_cursor_t	*pmap_cur;
	ibool			 have_next_range;
	page_range_t		 pmap_range;

	xb_ad(trx_sys_sys_space(node->space->id) == FALSE);

	snprintf(pmapfile_path, sizeof(pmapfile_path), "%s%s",
		 node->name, XB_PAGE_MAP_SUFFIX);

	/* Skip files that don't have a corresponding page map file */

	if (!(pmap_cur = page_map_file_open(pmapfile_path))) {

		msg("Not expanding %s\n", node->name);

		return(FALSE);
	}

	msg("Expanding %s\n", node->name);

	ds_local = ds_create(".", DS_TYPE_LOCAL);
	ds_buffer = ds_create(".", DS_TYPE_BUFFER);

	xb_a(ds_local != NULL && ds_buffer != NULL);

	ds_buffer_set_size(ds_buffer, FSP_EXTENT_SIZE * UNIV_PAGE_SIZE_MAX);

	ds_set_pipe(ds_buffer, ds_local);

	res = xb_fil_cur_open(&cursor, &rf_pass_through, node, 1);
	xb_a(res == XB_FIL_CUR_SUCCESS);

	snprintf(tmpfile_path, sizeof(tmpfile_path), "%s%s",
		 node->name, XB_TMPFILE_SUFFIX);

	tmpfile = ds_open(ds_buffer, tmpfile_path, &cursor.statinfo);
	if (tmpfile == NULL) {

		msg("Could not open temporary file '%s'\n", tmpfile_path);
		goto error;
	}

	have_next_range = page_map_file_next(pmap_cur, &pmap_range);

	page_expected_no = 0;

	/* Initialize and mark the empty page which is used to replace
	skipped pages. */
	memset(empty_page, 0, cursor.page_size);
	memcpy(empty_page + compacted_page_magic_offset,
	       compacted_page_magic, compacted_page_magic_size);
	mach_write_to_4(empty_page + FIL_PAGE_SPACE_OR_CHKSUM,
			BUF_NO_CHECKSUM_MAGIC);
	mach_write_to_4(empty_page + cursor.page_size -
			FIL_PAGE_END_LSN_OLD_CHKSUM,
			BUF_NO_CHECKSUM_MAGIC);


	/* Main copy loop */

	while ((res = xb_fil_cur_read(&cursor)) == XB_FIL_CUR_SUCCESS) {

		for (i = 0, page = cursor.buf; i < cursor.buf_npages;
		     i++, page += cursor.page_size) {

			ulint	page_read_no;

			page_read_no = mach_read_from_4(page + FIL_PAGE_OFFSET);
			xb_a(!page_read_no || page_expected_no <= page_read_no);

			if (have_next_range &&
			    page_expected_no == pmap_range.from) {

				xb_a(pmap_range.from <= pmap_range.to);

				/* Write empty pages instead of skipped ones, if
				necessary. */

				while (page_expected_no <= pmap_range.to) {

					if (ds_write(tmpfile, empty_page,
						     cursor.page_size)) {

						goto write_error;
					}

					page_expected_no++;
				}

				have_next_range =
					page_map_file_next(pmap_cur,
							   &pmap_range);
			}

			/* Write the current page */

			if (ds_write(tmpfile, page, cursor.page_size)) {

				goto write_error;
			}

			page_expected_no++;
		}
	}

	if (res != XB_FIL_CUR_EOF) {

		goto error;
	}

	/* Write empty pages instead of trailing skipped ones, if any */

	if (have_next_range) {

		xb_a(page_expected_no == pmap_range.from);
		xb_a(pmap_range.from <= pmap_range.to);

		while (page_expected_no <= pmap_range.to) {

			if (ds_write(tmpfile, empty_page,
				     cursor.page_size)) {

				goto write_error;
			}

			page_expected_no++;
		}

		xb_a(!page_map_file_next(pmap_cur, &pmap_range));
	}

	/* Replace the original .ibd file with the expanded file */
	if (my_rename(tmpfile_path, node->name, MYF(MY_WME))) {

		msg("Failed to rename '%s' to '%s'\n",
		    tmpfile_path, node->name);
		goto error;
	}

	my_delete(pmapfile_path, MYF(MY_WME));

	if (!ds_close(tmpfile)) {
		success = TRUE;
	}
	tmpfile = NULL;

	goto end;

write_error:
	msg("Write to '%s' failed\n", tmpfile_path);

error:
	if (tmpfile != NULL) {

		ds_close(tmpfile);
		my_delete(tmpfile_path, MYF(MY_WME));
	}

end:
	ds_destroy(ds_buffer);
	ds_destroy(ds_local);

	xb_fil_cur_close(&cursor);

	page_map_file_close(pmap_cur);

	return(success);
}

/******************************************************************************
Expand the data files according to the skipped pages maps created by --compact.
@return TRUE on success, FALSE on failure. */
my_bool
xb_expand_datafiles(void)
/*=====================*/
{
	ulint			 nfiles;
	datafiles_iter_t	*it = NULL;
	fil_node_t		*node;
	fil_space_t		*space;

	msg("Starting to expand compacted .ibd files.\n");

	/* Initialize the tablespace cache */
	if (xb_data_files_init() != DB_SUCCESS) {
		return(FALSE);
	}

	nfiles = UT_LIST_GET_LEN(fil_system->space_list);
	xb_a(nfiles > 0);

	it = datafiles_iter_new(fil_system);
	if (it == NULL) {
		msg("xtrabackup: error: datafiles_iter_new() failed.\n");
		goto error;
	}

	while ((node = datafiles_iter_next(it)) != NULL) {

		space = node->space;

		/* System tablespace cannot be compacted */
		if (!fil_is_user_tablespace_id(space->id)) {

			continue;
		}

		if (!xb_expand_file(node)) {

			goto error;
		}
	}

	datafiles_iter_free(it);
	xb_data_files_close();

	return(TRUE);

error:
	if (it != NULL) {
		datafiles_iter_free(it);
	}

	xb_data_files_close();

	return(FALSE);
}

/******************************************************************************
Callback used in buf_page_io_complete() to detect compacted pages.
@return TRUE if the page is marked as compacted, FALSE otherwise. */
ibool
buf_page_is_compacted(
/*==================*/
	const byte*	page)	/*!< in: a database page */
{
	return !memcmp(page + compacted_page_magic_offset,
		       compacted_page_magic, compacted_page_magic_size);
}

/*****************************************************************************
Builds an index definition corresponding to an index object. It is roughly
similar to innobase_create_index_def() / innobase_create_index_field_def() and
the opposite to dict_mem_index_create() / dict_mem_index_add_field(). */
static
void
xb_build_index_def(
/*=======================*/
	mem_heap_t*		heap,		/*!< in: heap */
	const dict_index_t*	index,		/*!< in: index */
	index_def_t*		index_def)	/*!< out: index definition */
{
	index_field_t*	fields;
	ulint		n_fields;
	ulint		i;

	ut_a(index->n_fields);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	/* Use n_user_defined_cols instead of n_fields, as the index will
	contain a part of the primary key after n_user_defined_cols, and those
	columns will be created automatically in
	dict_index_build_internal_clust(). */
	n_fields = index->n_user_defined_cols;

	memset(index_def, 0, sizeof(*index_def));

	index_def->name = mem_heap_strdup(heap, index->name);
	index_def->ind_type = index->type;

	fields = static_cast<index_field_t *>
		(mem_heap_alloc(heap, n_fields * sizeof(*fields)));

	for (i = 0; i < n_fields; i++) {
		dict_field_t*	field;

		field = dict_index_get_nth_field(index, i);
		fields[i].col_no = dict_col_get_no(field->col);
		fields[i].prefix_len = field->prefix_len;
	}

	index_def->fields = fields;
	index_def->n_fields = n_fields;
}

/* A dummy autoc_inc sequence for row_merge_build_indexes().  */
static ib_sequence_t null_seq(NULL, 0, 0);
/* A dummy table share and table for row_merge_build_indexes() error reporting.
Assumes that no errors are going to be reported. */
static struct TABLE_SHARE dummy_table_share;
static struct TABLE dummy_table;

/********************************************************************//**
Rebuild secondary indexes for a given table. */
static
void
xb_rebuild_indexes_for_table(
/*=========================*/
	dict_table_t*	table,		/*!< in: table */
	trx_t*		trx,		/*!< in: transaction handle */
	ulint		thread_n)	/*!< in: thread number */
{
	dict_index_t*	index;
	dict_index_t**	indexes;
	ulint		n_indexes;
	index_def_t*	index_defs;
	ulint		i;
	mem_heap_t*	heap;
	ulint		error;
	ulint*		add_key_nums;

	ut_ad(!mutex_own(&(dict_sys->mutex)));
	ut_ad(table);

	ut_a(UT_LIST_GET_LEN(table->indexes) > 0);

	n_indexes = UT_LIST_GET_LEN(table->indexes) - 1;
	if (!n_indexes) {
		/* Only the primary key, nothing to do. */
		return;
	}

	heap = mem_heap_create(1024);

	indexes = (dict_index_t**) mem_heap_alloc(heap,
						  n_indexes * sizeof(*indexes));
	index_defs = (index_def_t*) mem_heap_alloc(heap, n_indexes *
							 sizeof(*index_defs));
	add_key_nums = static_cast<ulint *>
		(mem_heap_alloc(heap, n_indexes * sizeof(*add_key_nums)));

	/* Skip the primary key. */
	index = dict_table_get_first_index(table);
	ut_a(dict_index_is_clust(index));

	row_mysql_lock_data_dictionary(trx);

	for (i = 0; (index = dict_table_get_next_index(index)); i++) {

		msg("[%02lu]   Found index %s\n", thread_n, index->name);

		/* Pretend that it's the current trx that created this index.
		Required to avoid 5.6+ debug assertions. */
		index->trx_id = trx->id;

		xb_build_index_def(heap, index, &index_defs[i]);

		/* In 5.6+, row_merge_drop_indexes() drops all the indexes on
		the table that have the temp index prefix.  It does not accept
		an array of indexes to drop as in 5.5-.  */
		row_merge_rename_index_to_drop(trx, table->id, index->id);
	}

	ut_ad(i == n_indexes);

	row_merge_drop_indexes(trx, table, TRUE);

	index = dict_table_get_first_index(table);
	ut_a(dict_index_is_clust(index));
	index = dict_table_get_next_index(index);
	while (index) {

		/* In 5.6+, row_merge_drop_indexes() does not remove the
		indexes from the dictionary cache nor from any foreign key
		list.  This may cause invalid dereferences as we try to access
		the dropped indexes from other tables as FKs.  */

		dict_index_t* next_index = dict_table_get_next_index(index);
		index->to_be_dropped = 1;

		/* Patch up any FK referencing this index with NULL */
		dict_foreign_replace_index(table, NULL, index);

		dict_index_remove_from_cache(table, index);

		index = next_index;
	}

	msg("[%02lu]   Rebuilding %lu index(es).\n", thread_n, n_indexes);

	error = row_merge_lock_table(trx, table, LOCK_X);
	xb_a(error == DB_SUCCESS);

	for (i = 0; i < n_indexes; i++) {
		indexes[i] = row_merge_create_index(trx, table,
						    &index_defs[i]);
		add_key_nums[i] = index_defs[i].key_number;
	}

	/* Commit trx to release latches on system tables */
	trx_commit_for_mysql(trx);
	trx_start_for_ddl(trx, TRX_DICT_OP_INDEX);

	row_mysql_unlock_data_dictionary(trx);

	/* Reacquire table lock for row_merge_build_indexes() */
	error = row_merge_lock_table(trx, table, LOCK_X);
	xb_a(error == DB_SUCCESS);

	error = row_merge_build_indexes(trx, table, table, FALSE, indexes,
					add_key_nums, n_indexes, &dummy_table,
					NULL, NULL, ULINT_UNDEFINED, null_seq);
	ut_a(error == DB_SUCCESS);

	mem_heap_free(heap);

	trx_commit_for_mysql(trx);

	trx_start_for_ddl(trx, TRX_DICT_OP_INDEX);
}

/**************************************************************************
Worker thread function for index rebuild. */
static
void *
xb_rebuild_indexes_thread_func(
/*===========================*/
	void*	arg)	/* thread context */
{
	dict_table_t*		table;
	index_rebuild_table_t*	rebuild_table;
	index_rebuild_thread_t*	thread;
	trx_t*			trx;

	thread = (index_rebuild_thread_t *) arg;

	trx = trx_allocate_for_mysql();

	/* Suppress foreign key checks, as we are going to drop and recreate all
	secondary keys. */
	trx->check_foreigns = FALSE;
	trx_start_for_ddl(trx, TRX_DICT_OP_INDEX);

	/* Loop until there are no more tables in tables list */
	for (;;) {
		pthread_mutex_lock(&table_list_mutex);

		rebuild_table = UT_LIST_GET_FIRST(table_list);

		if (rebuild_table == NULL) {

			pthread_mutex_unlock(&table_list_mutex);
			break;
		}

		UT_LIST_REMOVE(list, table_list, rebuild_table);

		pthread_mutex_unlock(&table_list_mutex);

		ut_ad(rebuild_table->name);
		ut_ad(fil_is_user_tablespace_id(rebuild_table->space_id));

		row_mysql_lock_data_dictionary(trx);

		table = dict_table_get_low(rebuild_table->name);

		ut_d(table->n_ref_count++);

		row_mysql_unlock_data_dictionary(trx);

		ut_a(table != NULL);
		ut_a(table->space == rebuild_table->space_id);

		/* Discard change buffer entries for this space */
		ibuf_delete_for_discarded_space(rebuild_table->space_id);

		msg("[%02lu] Checking if there are indexes to rebuild in table "
		    "%s (space id: %lu)\n",
		    thread->num,
		    rebuild_table->name, rebuild_table->space_id);

		xb_rebuild_indexes_for_table(table, trx, thread->num);

		ut_d(table->n_ref_count--);

		mem_free(rebuild_table->name);
		mem_free(rebuild_table);
	}

	trx_commit_for_mysql(trx);

	trx_free_for_mysql(trx);

	return(NULL);
}

/******************************************************************************
Rebuild all secondary indexes in all tables in separate spaces. Called from
innobase_start_or_create_for_mysql(). */
void
xb_compact_rebuild_indexes(void)
/*=============================*/
{
	dict_table_t*		sys_tables;
	dict_index_t*		sys_index;
	btr_pcur_t		pcur;
	const rec_t*		rec;
	mtr_t			mtr;
	const byte*		field;
	ulint			len;
	ulint			space_id;
	trx_t*			trx;
	index_rebuild_table_t*	rebuild_table;
	index_rebuild_thread_t*	threads;
	ulint			i;

	/* Set up the dummy table for the index rebuild error reporting */
	dummy_table_share.fields = 0;
	dummy_table.s = &dummy_table_share;

	/* Iterate all tables that are not in the system tablespace and add them
	to the list of tables to be rebuilt later. */

	trx = trx_allocate_for_mysql();
	trx_start_for_ddl(trx, TRX_DICT_OP_INDEX);

	row_mysql_lock_data_dictionary(trx);

	/* Enlarge the fatal lock wait timeout during index rebuild
	operation. */
	os_increment_counter_by_amount(server_mutex,
				       srv_fatal_semaphore_wait_threshold,
				       7200);

	mtr_start(&mtr);

	sys_tables = dict_table_get_low("SYS_TABLES");
	sys_index = UT_LIST_GET_FIRST(sys_tables->indexes);
	ut_a(!dict_table_is_comp(sys_tables));

	pthread_mutex_init(&table_list_mutex, NULL);
	UT_LIST_INIT(table_list);

	btr_pcur_open_at_index_side(TRUE, sys_index, BTR_SEARCH_LEAF, &pcur,
				    TRUE, 0, &mtr);
	for (;;) {
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);

		rec = btr_pcur_get_rec(&pcur);

		if (!btr_pcur_is_on_user_rec(&pcur)) {
			/* end of index */

			break;
		}

		if (rec_get_deleted_flag(rec, 0)) {
			continue;
		}

		field = rec_get_nth_field_old(rec, 9, &len);
		ut_a(len == 4);

		space_id = mach_read_from_4(field);

		/* Don't touch tables in the system tablespace */
		if (!fil_is_user_tablespace_id(space_id)) {

			continue;
		}

		field = rec_get_nth_field_old(rec, 0, &len);

		rebuild_table = static_cast<index_rebuild_table_t *>
			(mem_alloc(sizeof(*rebuild_table)));
		rebuild_table->name = mem_strdupl((char*) field, len);
		rebuild_table->space_id = space_id;

		UT_LIST_ADD_LAST(list, table_list, rebuild_table);
	}

	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	row_mysql_unlock_data_dictionary(trx);

	trx_commit_for_mysql(trx);

	trx_free_for_mysql(trx);

	/* Start worker threads for the index rebuild operation */
	ut_ad(xtrabackup_rebuild_threads > 0);

	if (xtrabackup_rebuild_threads > 1) {
		msg("Starting %lu threads to rebuild indexes.\n",
		    xtrabackup_rebuild_threads);
	}

	threads = (index_rebuild_thread_t *)
		mem_alloc(sizeof(*threads) *
			  xtrabackup_rebuild_threads);

	for (i = 0; i < xtrabackup_rebuild_threads; i++) {

		threads[i].num = i+1;
		if (pthread_create(&threads[i].id, NULL,
				   xb_rebuild_indexes_thread_func,
				   &threads[i])) {

			msg("error: pthread_create() failed: errno = %d\n",
			    errno);
			ut_a(0);
		}
	}

	/* Wait for worker threads to finish */
	for (i = 0; i < xtrabackup_rebuild_threads; i++) {
		pthread_join(threads[i].id, NULL);
	}

	mem_free(threads);
}
