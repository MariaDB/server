/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

Compressing datasink implementation for XtraBackup.

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

#include <mysql_version.h>
#include <my_base.h>
#include <quicklz.h>
#include <zlib.h>
#include "common.h"
#include "datasink.h"

#define COMPRESS_CHUNK_SIZE ((size_t) (xtrabackup_compress_chunk_size))
#define MY_QLZ_COMPRESS_OVERHEAD 400

typedef struct {
	pthread_t		id;
	uint			num;
	pthread_mutex_t 	ctrl_mutex;
	pthread_cond_t		ctrl_cond;
	pthread_mutex_t		data_mutex;
	pthread_cond_t  	data_cond;
	my_bool			started;
	my_bool			data_avail;
	my_bool			cancelled;
	const char 		*from;
	size_t			from_len;
	char			*to;
	size_t			to_len;
	qlz_state_compress	state;
	ulong			adler;
} comp_thread_ctxt_t;

typedef struct {
	comp_thread_ctxt_t	*threads;
	uint			nthreads;
} ds_compress_ctxt_t;

typedef struct {
	ds_file_t		*dest_file;
	ds_compress_ctxt_t	*comp_ctxt;
	size_t			bytes_processed;
} ds_compress_file_t;

/* Compression options */
extern char		*xtrabackup_compress_alg;
extern uint		xtrabackup_compress_threads;
extern ulonglong	xtrabackup_compress_chunk_size;

static ds_ctxt_t *compress_init(const char *root);
static ds_file_t *compress_open(ds_ctxt_t *ctxt, const char *path,
				MY_STAT *mystat);
static int compress_write(ds_file_t *file, const uchar *buf, size_t len);
static int compress_close(ds_file_t *file);
static void compress_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_compress = {
	&compress_init,
	&compress_open,
	&compress_write,
	&compress_close,
	&dummy_remove,
	&compress_deinit
};

static inline int write_uint32_le(ds_file_t *file, ulong n);
static inline int write_uint64_le(ds_file_t *file, ulonglong n);

static comp_thread_ctxt_t *create_worker_threads(uint n);
static void destroy_worker_threads(comp_thread_ctxt_t *threads, uint n);
static void *compress_worker_thread_func(void *arg);

static
ds_ctxt_t *
compress_init(const char *root)
{
	ds_ctxt_t		*ctxt;
	ds_compress_ctxt_t	*compress_ctxt;
	comp_thread_ctxt_t	*threads;

	/* Create and initialize the worker threads */
	threads = create_worker_threads(xtrabackup_compress_threads);
	if (threads == NULL) {
		msg("compress: failed to create worker threads.");
		return NULL;
	}

	ctxt = (ds_ctxt_t *) my_malloc(sizeof(ds_ctxt_t) +
				       sizeof(ds_compress_ctxt_t),
				       MYF(MY_FAE));

	compress_ctxt = (ds_compress_ctxt_t *) (ctxt + 1);
	compress_ctxt->threads = threads;
	compress_ctxt->nthreads = xtrabackup_compress_threads;

	ctxt->ptr = compress_ctxt;
	ctxt->root = my_strdup(root, MYF(MY_FAE));

	return ctxt;
}

static
ds_file_t *
compress_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *mystat)
{
	ds_compress_ctxt_t	*comp_ctxt;
	ds_ctxt_t		*dest_ctxt;
 	ds_file_t		*dest_file;
	char			new_name[FN_REFLEN];
	size_t			name_len;
	ds_file_t		*file;
	ds_compress_file_t	*comp_file;

	xb_ad(ctxt->pipe_ctxt != NULL);
	dest_ctxt = ctxt->pipe_ctxt;

	comp_ctxt = (ds_compress_ctxt_t *) ctxt->ptr;

	/* Append the .qp extension to the filename */
	fn_format(new_name, path, "", ".qp", MYF(MY_APPEND_EXT));

	dest_file = ds_open(dest_ctxt, new_name, mystat);
	if (dest_file == NULL) {
		return NULL;
	}

	/* Write the qpress archive header */
	if (ds_write(dest_file, "qpress10", 8) ||
	    write_uint64_le(dest_file, COMPRESS_CHUNK_SIZE)) {
		goto err;
	}

	/* We are going to create a one-file "flat" (i.e. with no
	subdirectories) archive. So strip the directory part from the path and
	remove the '.qp' suffix. */
	fn_format(new_name, path, "", "", MYF(MY_REPLACE_DIR));

	/* Write the qpress file header */
	name_len = strlen(new_name);
	if (ds_write(dest_file, "F", 1) ||
	    write_uint32_le(dest_file, (uint)name_len) ||
	    /* we want to write the terminating \0 as well */
	    ds_write(dest_file, new_name, name_len + 1)) {
		goto err;
	}

	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
				       sizeof(ds_compress_file_t),
				       MYF(MY_FAE));
	comp_file = (ds_compress_file_t *) (file + 1);
	comp_file->dest_file = dest_file;
	comp_file->comp_ctxt = comp_ctxt;
	comp_file->bytes_processed = 0;

	file->ptr = comp_file;
	file->path = dest_file->path;

	return file;

err:
	ds_close(dest_file);
	return NULL;
}

static
int
compress_write(ds_file_t *file, const uchar *buf, size_t len)
{
	ds_compress_file_t	*comp_file;
	ds_compress_ctxt_t	*comp_ctxt;
	comp_thread_ctxt_t	*threads;
	comp_thread_ctxt_t	*thd;
	uint			nthreads;
	uint			i;
	const char		*ptr;
	ds_file_t		*dest_file;

	comp_file = (ds_compress_file_t *) file->ptr;
	comp_ctxt = comp_file->comp_ctxt;
	dest_file = comp_file->dest_file;

	threads = comp_ctxt->threads;
	nthreads = comp_ctxt->nthreads;

	ptr = (const char *) buf;
	while (len > 0) {
		uint max_thread;

		/* Send data to worker threads for compression */
		for (i = 0; i < nthreads; i++) {
			size_t chunk_len;

			thd = threads + i;

			pthread_mutex_lock(&thd->ctrl_mutex);

			chunk_len = (len > COMPRESS_CHUNK_SIZE) ?
				COMPRESS_CHUNK_SIZE : len;
			thd->from = ptr;
			thd->from_len = chunk_len;

			pthread_mutex_lock(&thd->data_mutex);
			thd->data_avail = TRUE;
			pthread_cond_signal(&thd->data_cond);
			pthread_mutex_unlock(&thd->data_mutex);

			len -= chunk_len;
			if (len == 0) {
				break;
			}
			ptr += chunk_len;
		}

		max_thread = (i < nthreads) ? i :  nthreads - 1;

		/* Reap and stream the compressed data */
		for (i = 0; i <= max_thread; i++) {
			thd = threads + i;

			pthread_mutex_lock(&thd->data_mutex);
			while (thd->data_avail == TRUE) {
				pthread_cond_wait(&thd->data_cond,
						  &thd->data_mutex);
			}

			xb_a(threads[i].to_len > 0);

			if (ds_write(dest_file, "NEWBNEWB", 8) ||
			    write_uint64_le(dest_file,
					    comp_file->bytes_processed)) {
				msg("compress: write to the destination stream "
				    "failed.");
				return 1;
			}

			comp_file->bytes_processed += threads[i].from_len;

			if (write_uint32_le(dest_file, threads[i].adler) ||
			    ds_write(dest_file, threads[i].to,
					   threads[i].to_len)) {
				msg("compress: write to the destination stream "
				    "failed.");
				return 1;
			}

			pthread_mutex_unlock(&threads[i].data_mutex);
			pthread_mutex_unlock(&threads[i].ctrl_mutex);
		}
	}

	return 0;
}

static
int
compress_close(ds_file_t *file)
{
	ds_compress_file_t	*comp_file;
	ds_file_t		*dest_file;
	int			rc;

	comp_file = (ds_compress_file_t *) file->ptr;
	dest_file = comp_file->dest_file;

	/* Write the qpress file trailer */
	ds_write(dest_file, "ENDSENDS", 8);

	/* Supposedly the number of written bytes should be written as a
	"recovery information" in the file trailer, but in reality qpress
	always writes 8 zeros here. Let's do the same */

	write_uint64_le(dest_file, 0);

	rc = ds_close(dest_file);

	my_free(file);

	return rc;
}

static
void
compress_deinit(ds_ctxt_t *ctxt)
{
	ds_compress_ctxt_t 	*comp_ctxt;

	xb_ad(ctxt->pipe_ctxt != NULL);

	comp_ctxt = (ds_compress_ctxt_t *) ctxt->ptr;;

	destroy_worker_threads(comp_ctxt->threads, comp_ctxt->nthreads);

	my_free(ctxt->root);
	my_free(ctxt);
}

static inline
int
write_uint32_le(ds_file_t *file, ulong n)
{
	char tmp[4];

	int4store(tmp, n);
	return ds_write(file, tmp, sizeof(tmp));
}

static inline
int
write_uint64_le(ds_file_t *file, ulonglong n)
{
	char tmp[8];

	int8store(tmp, n);
	return ds_write(file, tmp, sizeof(tmp));
}

static
comp_thread_ctxt_t *
create_worker_threads(uint n)
{
	comp_thread_ctxt_t	*threads;
	uint 			i;

	threads = (comp_thread_ctxt_t *)
		my_malloc(sizeof(comp_thread_ctxt_t) * n, MYF(MY_FAE));

	for (i = 0; i < n; i++) {
		comp_thread_ctxt_t *thd = threads + i;

		thd->num = i + 1;
		thd->started = FALSE;
		thd->cancelled = FALSE;
		thd->data_avail = FALSE;

		thd->to = (char *) my_malloc(COMPRESS_CHUNK_SIZE +
						   MY_QLZ_COMPRESS_OVERHEAD,
						   MYF(MY_FAE));

		/* Initialize the control mutex and condition var */
		if (pthread_mutex_init(&thd->ctrl_mutex, NULL) ||
		    pthread_cond_init(&thd->ctrl_cond, NULL)) {
			goto err;
		}

		/* Initialize and data mutex and condition var */
		if (pthread_mutex_init(&thd->data_mutex, NULL) ||
		    pthread_cond_init(&thd->data_cond, NULL)) {
			goto err;
		}

		pthread_mutex_lock(&thd->ctrl_mutex);

		if (pthread_create(&thd->id, NULL, compress_worker_thread_func,
				   thd)) {
			msg("compress: pthread_create() failed: "
			    "errno = %d", errno);
			pthread_mutex_unlock(&thd->ctrl_mutex);
			goto err;
		}
	}

	/* Wait for the threads to start */
	for (i = 0; i < n; i++) {
		comp_thread_ctxt_t *thd = threads + i;

		while (thd->started == FALSE)
			pthread_cond_wait(&thd->ctrl_cond, &thd->ctrl_mutex);
		pthread_mutex_unlock(&thd->ctrl_mutex);
	}

	return threads;

err:
	while (i > 0) {
		comp_thread_ctxt_t *thd;
		i--;
		thd = threads + i;
		pthread_mutex_unlock(&thd->ctrl_mutex);
	}

	my_free(threads);
	return NULL;
}

static
void
destroy_worker_threads(comp_thread_ctxt_t *threads, uint n)
{
	uint i;

	for (i = 0; i < n; i++) {
		comp_thread_ctxt_t *thd = threads + i;

		pthread_mutex_lock(&thd->data_mutex);
		threads[i].cancelled = TRUE;
		pthread_cond_signal(&thd->data_cond);
		pthread_mutex_unlock(&thd->data_mutex);

		pthread_join(thd->id, NULL);

		pthread_cond_destroy(&thd->data_cond);
		pthread_mutex_destroy(&thd->data_mutex);
		pthread_cond_destroy(&thd->ctrl_cond);
		pthread_mutex_destroy(&thd->ctrl_mutex);

		my_free(thd->to);
	}

	my_free(threads);
}

static
void *
compress_worker_thread_func(void *arg)
{
	comp_thread_ctxt_t *thd = (comp_thread_ctxt_t *) arg;

	pthread_mutex_lock(&thd->ctrl_mutex);

	pthread_mutex_lock(&thd->data_mutex);

	thd->started = TRUE;
	pthread_cond_signal(&thd->ctrl_cond);

	pthread_mutex_unlock(&thd->ctrl_mutex);

	while (1) {
		thd->data_avail = FALSE;
		pthread_cond_signal(&thd->data_cond);

		while (!thd->data_avail && !thd->cancelled) {
			pthread_cond_wait(&thd->data_cond, &thd->data_mutex);
		}

		if (thd->cancelled)
			break;

		thd->to_len = qlz_compress(thd->from, thd->to, thd->from_len,
					   &thd->state);

		/* qpress uses 0x00010000 as the initial value, but its own
		Adler-32 implementation treats the value differently:
		  1. higher order bits are the sum of all bytes in the sequence
		  2. lower order bits are the sum of resulting values at every
		     step.
		So it's the other way around as compared to zlib's adler32().
		That's why  0x00000001 is being passed here to be compatible
		with qpress implementation. */

		thd->adler = adler32(0x00000001, (uchar *) thd->to,
				     (uInt)thd->to_len);
	}

	pthread_mutex_unlock(&thd->data_mutex);

	return NULL;
}
