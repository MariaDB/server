/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

Data sink interface.

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

#include <my_global.h>
#include <my_base.h>
#include "common.h"
#include "datasink.h"
#include "ds_compress.h"
#include "ds_archive.h"
#include "ds_xbstream.h"
#include "ds_local.h"
#include "ds_stdout.h"
#include "ds_tmpfile.h"
#include "ds_buffer.h"
#ifdef WITH_S3_STORAGE_ENGINE
#include "ds_s3.h"
#endif // WITH_S3_STORAGE_ENGINE

/************************************************************************
Create a datasink of the specified type */
ds_ctxt_t *
ds_create(const void *ds_data, ds_type_t type)
{
	datasink_t	*ds;
	ds_ctxt_t	*ctxt;

	switch (type) {
	case DS_TYPE_STDOUT:
		ds = &datasink_stdout;
		break;
	case DS_TYPE_LOCAL:
		ds = &datasink_local;
		break;
	case DS_TYPE_ARCHIVE:
#ifdef HAVE_LIBARCHIVE
		ds = &datasink_archive;
#else
		die("mariabackup was built without libarchive support");
#endif
		break;
	case DS_TYPE_XBSTREAM:
		ds = &datasink_xbstream;
		break;
	case DS_TYPE_COMPRESS:
		ds = &datasink_compress;
		break;
	case DS_TYPE_ENCRYPT:
  case DS_TYPE_DECRYPT:
		die("mariabackup does not support encrypted backups.");
		break;

	case DS_TYPE_TMPFILE:
		ds = &datasink_tmpfile;
		break;
	case DS_TYPE_BUFFER:
		ds = &datasink_buffer;
		break;
#ifdef WITH_S3_STORAGE_ENGINE
	case DS_TYPE_S3:
		ds = &datasink_s3;
		break;
#endif // WITH_S3_STORAGE_ENGINE
	default:
		msg("Unknown datasink type: %d", type);
		xb_ad(0);
		return NULL;
	}

	ctxt = ds->init(ds_data);
	if (ctxt != NULL) {
		ctxt->datasink = ds;
	} else {
		die("failed to initialize datasink.");
	}

	return ctxt;
}

/************************************************************************
Open a datasink file */
ds_file_t *
ds_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat)
{
	ds_file_t	*file;

	file = ctxt->datasink->open(ctxt, path, stat);
	if (file != NULL) {
		file->datasink = ctxt->datasink;
	}

	return file;
}

/************************************************************************
Write to a datasink file.
@return 0 on success, 1 on error. */
int
ds_write(ds_file_t *file, const void *buf, size_t len)
{
	if (len == 0) {
		return 0;
	}
	return file->datasink->write(file, (const uchar *)buf, len);
}

/************************************************************************
Close a datasink file.
@return 0 on success, 1, on error. */
int
ds_close(ds_file_t *file)
{
	return file->datasink->close(file);
}

/************************************************************************
Destroy a datasink handle */
void
ds_destroy(ds_ctxt_t *ctxt)
{
	ctxt->datasink->deinit(ctxt);
}

/************************************************************************
Set the destination pipe for a datasink (only makes sense for compress and
tmpfile). */
void ds_set_pipe(ds_ctxt_t *ctxt, ds_ctxt_t *pipe_ctxt)
{
	ctxt->pipe_ctxt = pipe_ctxt;
}
