/******************************************************
Copyright (c) 2013 Percona LLC and/or its affiliates.

Streaming implementation for XtraBackup.

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

#include <my_base.h>
#include <archive.h>
#include <archive_entry.h>
#include "common.h"
#include "datasink.h"

#if ARCHIVE_VERSION_NUMBER < 3000000
#define archive_write_add_filter_none(X) archive_write_set_compression_none(X)
#define archive_write_free(X) archive_write_finish(X)
#endif

typedef struct {
	struct archive	*archive;
	ds_file_t	*dest_file;
	pthread_mutex_t	mutex;
} ds_archive_ctxt_t;

typedef struct {
	struct archive_entry	*entry;
	ds_archive_ctxt_t	*archive_ctxt;
} ds_archive_file_t;


/***********************************************************************
General archive interface */

static ds_ctxt_t *archive_init(const char *root);
static ds_file_t *archive_open(ds_ctxt_t *ctxt, const char *path,
			      MY_STAT *mystat);
static int archive_write(ds_file_t *file, const void *buf, size_t len);
static int archive_close(ds_file_t *file);
static void archive_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_archive = {
	&archive_init,
	&archive_open,
	&archive_write,
	&archive_close,
	&archive_deinit
};

static
int
my_archive_open_callback(struct archive *a __attribute__((unused)),
		      void *data __attribute__((unused)))
{
	return ARCHIVE_OK;
}

static
ssize_t
my_archive_write_callback(struct archive *a __attribute__((unused)),
		       void *data, const void *buffer, size_t length)
{
	ds_archive_ctxt_t	*archive_ctxt;

	archive_ctxt = (ds_archive_ctxt_t *) data;

	xb_ad(archive_ctxt != NULL);
	xb_ad(archive_ctxt->dest_file != NULL);

	if (!ds_write(archive_ctxt->dest_file, buffer, length)) {
		return length;
	}
	return -1;
}

static
int
my_archive_close_callback(struct archive *a __attribute__((unused)),
			  void *data __attribute__((unused)))
{
	return ARCHIVE_OK;
}

static
ds_ctxt_t *
archive_init(const char *root __attribute__((unused)))
{
	ds_ctxt_t		*ctxt;
	ds_archive_ctxt_t	*archive_ctxt;
	struct archive		*a;

	ctxt = my_malloc(sizeof(ds_ctxt_t) + sizeof(ds_archive_ctxt_t),
			 MYF(MY_FAE));
	archive_ctxt = (ds_archive_ctxt_t *)(ctxt + 1);

	if (pthread_mutex_init(&archive_ctxt->mutex, NULL)) {
		msg("archive_init: pthread_mutex_init() failed.\n");
		goto err;
	}

	a = archive_write_new();
	if (a == NULL) {
		msg("archive_write_new() failed.\n");
		goto err;
	}

	archive_ctxt->archive = a;
	archive_ctxt->dest_file = NULL;

        if(archive_write_add_filter_none(a) != ARCHIVE_OK ||
	    archive_write_set_format_pax_restricted(a) != ARCHIVE_OK ||
	    /* disable internal buffering so we don't have to flush the
	    output in xtrabackup */
	    archive_write_set_bytes_per_block(a, 0) != ARCHIVE_OK) {
		msg("failed to set libarchive archive options: %s\n",
		    archive_error_string(a));
                archive_write_free(a);
		goto err;
	}

	if (archive_write_open(a, archive_ctxt, my_archive_open_callback,
			       my_archive_write_callback,
			       my_archive_close_callback) != ARCHIVE_OK) {
		msg("cannot open output archive.\n");
		return NULL;
	}

	ctxt->ptr = archive_ctxt;

	return ctxt;

err:
	my_free(ctxt);
	return NULL;
}

static
ds_file_t *
archive_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *mystat)
{
	ds_archive_ctxt_t	*archive_ctxt;
	ds_ctxt_t		*dest_ctxt;
	ds_file_t		*file;
	ds_archive_file_t	*archive_file;

	struct archive		*a;
	struct archive_entry	*entry;

	xb_ad(ctxt->pipe_ctxt != NULL);
	dest_ctxt = ctxt->pipe_ctxt;

	archive_ctxt = (ds_archive_ctxt_t *) ctxt->ptr;

	pthread_mutex_lock(&archive_ctxt->mutex);
	if (archive_ctxt->dest_file == NULL) {
		archive_ctxt->dest_file = ds_open(dest_ctxt, path, mystat);
		if (archive_ctxt->dest_file == NULL) {
			return NULL;
		}
	}
	pthread_mutex_unlock(&archive_ctxt->mutex);

	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
				       sizeof(ds_archive_file_t),
				       MYF(MY_FAE));

	archive_file = (ds_archive_file_t *) (file + 1);

	a = archive_ctxt->archive;

	entry = archive_entry_new();
	if (entry == NULL) {
		msg("archive_entry_new() failed.\n");
		goto err;
	}

	archive_entry_set_size(entry, mystat->st_size);
	archive_entry_set_mode(entry, 0660);
	archive_entry_set_filetype(entry, AE_IFREG);
	archive_entry_set_pathname(entry, path);
	archive_entry_set_mtime(entry, mystat->st_mtime, 0);

	archive_file->entry = entry;
	archive_file->archive_ctxt = archive_ctxt;

	if (archive_write_header(a, entry) != ARCHIVE_OK) {
		msg("archive_write_header() failed.\n");
		archive_entry_free(entry);
		goto err;
	}

	file->ptr = archive_file;
	file->path = archive_ctxt->dest_file->path;

	return file;

err:
	if (archive_ctxt->dest_file) {
		ds_close(archive_ctxt->dest_file);
		archive_ctxt->dest_file = NULL;
	}
	my_free(file);

	return NULL;
}

static
int
archive_write(ds_file_t *file, const void *buf, size_t len)
{
	ds_archive_file_t	*archive_file;
	struct archive		*a;

	archive_file = (ds_archive_file_t *) file->ptr;

	a = archive_file->archive_ctxt->archive;

	xb_ad(archive_file->archive_ctxt->dest_file != NULL);
	if (archive_write_data(a, buf, len) < 0) {
		msg("archive_write_data() failed: %s (errno = %d)\n",
		    archive_error_string(a), archive_errno(a));
		return 1;
	}

	return 0;
}

static
int
archive_close(ds_file_t *file)
{
	ds_archive_file_t	*archive_file;
	int			rc = 0;

	archive_file = (ds_archive_file_t *)file->ptr;

	archive_entry_free(archive_file->entry);

	my_free(file);

	return rc;
}

static
void
archive_deinit(ds_ctxt_t *ctxt)
{
	struct archive *a;
	ds_archive_ctxt_t	*archive_ctxt;

	archive_ctxt = (ds_archive_ctxt_t *) ctxt->ptr;

	a = archive_ctxt->archive;

	if (archive_write_close(a) != ARCHIVE_OK) {
		msg("archive_write_close() failed.\n");
	}
	archive_write_free(a);

	if (archive_ctxt->dest_file) {
		ds_close(archive_ctxt->dest_file);
		archive_ctxt->dest_file = NULL;
	}

	pthread_mutex_destroy(&archive_ctxt->mutex);

	my_free(ctxt);
}
