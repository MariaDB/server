/******************************************************
Copyright (c) 2013 Percona LLC and/or its affiliates.

Encryption datasink implementation for XtraBackup.

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
#include "xbcrypt_common.h"
#ifdef HAVE_GRYPT
#include "xbcrypt.h"

#define XB_CRYPT_CHUNK_SIZE ((size_t) (ds_encrypt_encrypt_chunk_size))

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
	const uchar 		*from;
	size_t			from_len;
	uchar			*to;
	uchar			*iv;
	size_t			to_len;
	gcry_cipher_hd_t	cipher_handle;
} crypt_thread_ctxt_t;

typedef struct {
	crypt_thread_ctxt_t	*threads;
	uint			nthreads;
} ds_encrypt_ctxt_t;

typedef struct {
	xb_wcrypt_t		*xbcrypt_file;
	ds_encrypt_ctxt_t	*crypt_ctxt;
	size_t			bytes_processed;
	ds_file_t		*dest_file;
} ds_encrypt_file_t;

/* Encryption options */
uint		ds_encrypt_encrypt_threads;
ulonglong	ds_encrypt_encrypt_chunk_size;

static ds_ctxt_t *encrypt_init(const char *root);
static ds_file_t *encrypt_open(ds_ctxt_t *ctxt, const char *path,
				MY_STAT *mystat);
static int encrypt_write(ds_file_t *file, const void *buf, size_t len);
static int encrypt_close(ds_file_t *file);
static void encrypt_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_encrypt = {
	&encrypt_init,
	&encrypt_open,
	&encrypt_write,
	&encrypt_close,
	&encrypt_deinit
};

static crypt_thread_ctxt_t *create_worker_threads(uint n);
static void destroy_worker_threads(crypt_thread_ctxt_t *threads, uint n);
static void *encrypt_worker_thread_func(void *arg);

static uint encrypt_iv_len = 0;

static
ssize_t
my_xb_crypt_write_callback(void *userdata, const void *buf, size_t len)
{
	ds_encrypt_file_t		*encrypt_file;

	encrypt_file = (ds_encrypt_file_t *) userdata;

	xb_ad(encrypt_file != NULL);
	xb_ad(encrypt_file->dest_file != NULL);

	if (!ds_write(encrypt_file->dest_file, buf, len)) {
		return len;
	}
	return -1;
}

static
ds_ctxt_t *
encrypt_init(const char *root)
{
	ds_ctxt_t		*ctxt;
	ds_encrypt_ctxt_t	*encrypt_ctxt;
	crypt_thread_ctxt_t	*threads;

	if (xb_crypt_init(&encrypt_iv_len)) {
		return NULL;
	}

	/* Create and initialize the worker threads */
	threads = create_worker_threads(ds_encrypt_encrypt_threads);
	if (threads == NULL) {
		msg("encrypt: failed to create worker threads.\n");
		return NULL;
	}

	ctxt = (ds_ctxt_t *) my_malloc(sizeof(ds_ctxt_t) +
				       sizeof(ds_encrypt_ctxt_t),
				       MYF(MY_FAE));

	encrypt_ctxt = (ds_encrypt_ctxt_t *) (ctxt + 1);
	encrypt_ctxt->threads = threads;
	encrypt_ctxt->nthreads = ds_encrypt_encrypt_threads;

	ctxt->ptr = encrypt_ctxt;
	ctxt->root = my_strdup(root, MYF(MY_FAE));

	return ctxt;
}

static
ds_file_t *
encrypt_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *mystat)
{
	ds_ctxt_t		*dest_ctxt;

	ds_encrypt_ctxt_t	*crypt_ctxt;
	ds_encrypt_file_t	*crypt_file;

	char			new_name[FN_REFLEN];
	ds_file_t		*file;

	xb_ad(ctxt->pipe_ctxt != NULL);
	dest_ctxt = ctxt->pipe_ctxt;

	crypt_ctxt = (ds_encrypt_ctxt_t *) ctxt->ptr;


	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
				       sizeof(ds_encrypt_file_t),
				       MYF(MY_FAE|MY_ZEROFILL));

	crypt_file = (ds_encrypt_file_t *) (file + 1);

	/* Append the .xbcrypt extension to the filename */
	fn_format(new_name, path, "", ".xbcrypt", MYF(MY_APPEND_EXT));
	crypt_file->dest_file = ds_open(dest_ctxt, new_name, mystat);
	if (crypt_file->dest_file == NULL) {
		msg("encrypt: ds_open(\"%s\") failed.\n", new_name);
		goto err;
	}

	crypt_file->crypt_ctxt = crypt_ctxt;
	crypt_file->xbcrypt_file = xb_crypt_write_open(crypt_file,
					   my_xb_crypt_write_callback);

	if (crypt_file->xbcrypt_file == NULL) {
		msg("encrypt: xb_crypt_write_open() failed.\n");
		goto err;
	}


	file->ptr = crypt_file;
	file->path = crypt_file->dest_file->path;

	return file;

err:
	if (crypt_file->dest_file) {
		ds_close(crypt_file->dest_file);
	}
	my_free(file);
	return NULL;
}

static
int
encrypt_write(ds_file_t *file, const void *buf, size_t len)
{
	ds_encrypt_file_t	*crypt_file;
	ds_encrypt_ctxt_t	*crypt_ctxt;
	crypt_thread_ctxt_t	*threads;
	crypt_thread_ctxt_t	*thd;
	uint			nthreads;
	uint			i;
	const uchar		*ptr;

	crypt_file = (ds_encrypt_file_t *) file->ptr;
	crypt_ctxt = crypt_file->crypt_ctxt;

	threads = crypt_ctxt->threads;
	nthreads = crypt_ctxt->nthreads;

	ptr = (const uchar *) buf;
	while (len > 0) {
		uint max_thread;

		/* Send data to worker threads for encryption */
		for (i = 0; i < nthreads; i++) {
			size_t chunk_len;

			thd = threads + i;

			pthread_mutex_lock(&thd->ctrl_mutex);

			chunk_len = (len > XB_CRYPT_CHUNK_SIZE) ?
				XB_CRYPT_CHUNK_SIZE : len;
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

		/* Reap and stream the encrypted data */
		for (i = 0; i <= max_thread; i++) {
			thd = threads + i;

			pthread_mutex_lock(&thd->data_mutex);
			while (thd->data_avail == TRUE) {
				pthread_cond_wait(&thd->data_cond,
						  &thd->data_mutex);
			}

			xb_a(threads[i].to_len > 0);

			if (xb_crypt_write_chunk(crypt_file->xbcrypt_file,
						 threads[i].to,
						 threads[i].from_len +
							XB_CRYPT_HASH_LEN,
						 threads[i].to_len,
						 threads[i].iv,
						 encrypt_iv_len)) {
				msg("encrypt: write to the destination file "
				    "failed.\n");
				return 1;
			}

			crypt_file->bytes_processed += threads[i].from_len;

			pthread_mutex_unlock(&threads[i].data_mutex);
			pthread_mutex_unlock(&threads[i].ctrl_mutex);
		}
	}

	return 0;
}

static
int
encrypt_close(ds_file_t *file)
{
	ds_encrypt_file_t	*crypt_file;
	ds_file_t		*dest_file;
	int			rc = 0;

	crypt_file = (ds_encrypt_file_t *) file->ptr;
	dest_file = crypt_file->dest_file;

	rc = xb_crypt_write_close(crypt_file->xbcrypt_file);

	if (ds_close(dest_file)) {
		rc = 1;
	}

	my_free(file);

	return rc;
}

static
void
encrypt_deinit(ds_ctxt_t *ctxt)
{
	ds_encrypt_ctxt_t 	*crypt_ctxt;

	xb_ad(ctxt->pipe_ctxt != NULL);

	crypt_ctxt = (ds_encrypt_ctxt_t *) ctxt->ptr;

	destroy_worker_threads(crypt_ctxt->threads, crypt_ctxt->nthreads);

	my_free(ctxt->root);
	my_free(ctxt);
}

static
crypt_thread_ctxt_t *
create_worker_threads(uint n)
{
	crypt_thread_ctxt_t	*threads;
	uint 			i;

	threads = (crypt_thread_ctxt_t *)
		my_malloc(sizeof(crypt_thread_ctxt_t) * n, MYF(MY_FAE));

	for (i = 0; i < n; i++) {
		crypt_thread_ctxt_t *thd = threads + i;

		thd->num = i + 1;
		thd->started = FALSE;
		thd->cancelled = FALSE;
		thd->data_avail = FALSE;

		thd->to = (uchar *) my_malloc(XB_CRYPT_CHUNK_SIZE +
					      XB_CRYPT_HASH_LEN, MYF(MY_FAE));

		thd->iv = (uchar *) my_malloc(encrypt_iv_len, MYF(MY_FAE));

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

		if (xb_crypt_cipher_open(&thd->cipher_handle)) {
			goto err;
		}

		pthread_mutex_lock(&thd->ctrl_mutex);

		if (pthread_create(&thd->id, NULL, encrypt_worker_thread_func,
				   thd)) {
			msg("encrypt: pthread_create() failed: "
			    "errno = %d\n", errno);
			goto err;
		}
	}

	/* Wait for the threads to start */
	for (i = 0; i < n; i++) {
		crypt_thread_ctxt_t *thd = threads + i;

		while (thd->started == FALSE)
			pthread_cond_wait(&thd->ctrl_cond, &thd->ctrl_mutex);
		pthread_mutex_unlock(&thd->ctrl_mutex);
	}

	return threads;

err:
	return NULL;
}

static
void
destroy_worker_threads(crypt_thread_ctxt_t *threads, uint n)
{
	uint i;

	for (i = 0; i < n; i++) {
		crypt_thread_ctxt_t *thd = threads + i;

		pthread_mutex_lock(&thd->data_mutex);
		threads[i].cancelled = TRUE;
		pthread_cond_signal(&thd->data_cond);
		pthread_mutex_unlock(&thd->data_mutex);

		pthread_join(thd->id, NULL);

		pthread_cond_destroy(&thd->data_cond);
		pthread_mutex_destroy(&thd->data_mutex);
		pthread_cond_destroy(&thd->ctrl_cond);
		pthread_mutex_destroy(&thd->ctrl_mutex);

		xb_crypt_cipher_close(thd->cipher_handle);

		my_free(thd->to);
		my_free(thd->iv);
	}

	my_free(threads);
}

static
void *
encrypt_worker_thread_func(void *arg)
{
	crypt_thread_ctxt_t *thd = (crypt_thread_ctxt_t *) arg;

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

		thd->to_len = thd->from_len;

		if (xb_crypt_encrypt(thd->cipher_handle, thd->from,
				     thd->from_len, thd->to, &thd->to_len,
				     thd->iv)) {
			thd->to_len = 0;
			continue;
		}
	}

	pthread_mutex_unlock(&thd->data_mutex);

	return NULL;
}
#endif /* HAVE_GCRYPT*/
