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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#include <my_base.h>
#include "common.h"
#include "datasink.h"
#include "ds_compress.h"
#include "ds_archive.h"
#include "ds_xbstream.h"
#include "ds_local.h"
#include "ds_stdout.h"
#include "ds_tmpfile.h"
#include "ds_encrypt.h"
#include "ds_buffer.h"

/************************************************************************
Create a datasink of the specified type */
ds_ctxt_t *
ds_create(const char *root, ds_type_t type)
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
		msg("Error : mariabackup was built without libarchive support");
		exit(EXIT_FAILURE);
#endif
		break;
	case DS_TYPE_XBSTREAM:
		ds = &datasink_xbstream;
		break;
	case DS_TYPE_COMPRESS:
		ds = &datasink_compress;
		break;
	case DS_TYPE_ENCRYPT:
		msg("Error : mariabackup does not support encrypted backups.");
		exit(EXIT_FAILURE);
		break;

	case DS_TYPE_TMPFILE:
		ds = &datasink_tmpfile;
		break;
	case DS_TYPE_BUFFER:
		ds = &datasink_buffer;
		break;
	default:
		msg("Unknown datasink type: %d\n", type);
		xb_ad(0);
		return NULL;
	}

	ctxt = ds->init(root);
	if (ctxt != NULL) {
		ctxt->datasink = ds;
	} else {
		msg("Error: failed to initialize datasink.\n");
		exit(EXIT_FAILURE);
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
	return file->datasink->write(file, buf, len);
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
