/******************************************************
Copyright (c) 2017 Percona LLC and/or its affiliates.

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
#include "xbcrypt.h"
#include "xbcrypt_common.h"
#include "crc_glue.h"

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
	my_bool			failed;
	const uchar 		*from;
	size_t			from_len;
	uchar			*to;
	size_t			to_len;
	size_t			to_size;
	const uchar		*iv;
	size_t			iv_len;
	unsigned long long	offset;
	my_bool			hash_appended;
	gcry_cipher_hd_t	cipher_handle;
	xb_rcrypt_result_t	parse_result;
} crypt_thread_ctxt_t;

typedef struct {
	crypt_thread_ctxt_t	*threads;
	uint			nthreads;
	int			encrypt_algo;
	size_t			chunk_size;
	char			*encrypt_key;
	char			*encrypt_key_file;
} ds_decrypt_ctxt_t;

typedef struct {
	ds_decrypt_ctxt_t	*crypt_ctxt;
	size_t			bytes_processed;
	ds_file_t		*dest_file;
	uchar			*buf;
	size_t			buf_len;
	size_t			buf_size;
} ds_decrypt_file_t;

int		ds_decrypt_encrypt_threads = 1;

static ds_ctxt_t *decrypt_init(const char *root);
static ds_file_t *decrypt_open(ds_ctxt_t *ctxt, const char *path,
				MY_STAT *mystat);
static int decrypt_write(ds_file_t *file, const void *buf, size_t len);
static int decrypt_close(ds_file_t *file);
static void decrypt_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_decrypt = {
	&decrypt_init,
	&decrypt_open,
	&decrypt_write,
	&decrypt_close,
	&decrypt_deinit
};

static crypt_thread_ctxt_t *create_worker_threads(uint n);
static void destroy_worker_threads(crypt_thread_ctxt_t *threads, uint n);
static void *decrypt_worker_thread_func(void *arg);

static
ds_ctxt_t *
decrypt_init(const char *root)
{
	ds_ctxt_t		*ctxt;
	ds_decrypt_ctxt_t	*decrypt_ctxt;
	crypt_thread_ctxt_t	*threads;

	if (xb_crypt_init(NULL)) {
		return NULL;
	}

	/* Create and initialize the worker threads */
	threads = create_worker_threads(ds_decrypt_encrypt_threads);
	if (threads == NULL) {
		msg("decrypt: failed to create worker threads.\n");
		return NULL;
	}

	ctxt = (ds_ctxt_t *) my_malloc(sizeof(ds_ctxt_t) +
				       sizeof(ds_decrypt_ctxt_t),
				       MYF(MY_FAE));

	decrypt_ctxt = (ds_decrypt_ctxt_t *) (ctxt + 1);
	decrypt_ctxt->threads = threads;
	decrypt_ctxt->nthreads = ds_decrypt_encrypt_threads;

	ctxt->ptr = decrypt_ctxt;
	ctxt->root = my_strdup(root, MYF(MY_FAE));

	return ctxt;
}

static
ds_file_t *
decrypt_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *mystat)
{
	ds_ctxt_t		*dest_ctxt;

	ds_decrypt_ctxt_t	*crypt_ctxt;
	ds_decrypt_file_t	*crypt_file;

	char			new_name[FN_REFLEN];
	ds_file_t		*file;

	xb_ad(ctxt->pipe_ctxt != NULL);
	dest_ctxt = ctxt->pipe_ctxt;

	crypt_ctxt = (ds_decrypt_ctxt_t *) ctxt->ptr;


	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
				       sizeof(ds_decrypt_file_t),
				       MYF(MY_FAE|MY_ZEROFILL));

	crypt_file = (ds_decrypt_file_t *) (file + 1);

	/* Remove the .xbcrypt extension from the filename */
	strncpy(new_name, path, FN_REFLEN);
	new_name[strlen(new_name) - 8] = 0;
	crypt_file->dest_file = ds_open(dest_ctxt, new_name, mystat);
	if (crypt_file->dest_file == NULL) {
		msg("decrypt: ds_open(\"%s\") failed.\n", new_name);
		goto err;
	}

	crypt_file->crypt_ctxt = crypt_ctxt;
	crypt_file->buf = NULL;
	crypt_file->buf_size = 0;
	crypt_file->buf_len = 0;

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

#define CHECK_BUF_SIZE(ptr, size, buf, len) \
	if (ptr + size - buf > (ssize_t) len) { \
		result = XB_CRYPT_READ_INCOMPLETE; \
		goto exit; \
	}

static
xb_rcrypt_result_t
parse_xbcrypt_chunk(crypt_thread_ctxt_t *thd, const uchar *buf, size_t len,
		    size_t *bytes_processed)
{
	const uchar *ptr;
	uint version;
	ulong checksum, checksum_exp;
	ulonglong tmp;
	xb_rcrypt_result_t result = XB_CRYPT_READ_CHUNK;

	*bytes_processed = 0;
	ptr = buf;

	CHECK_BUF_SIZE(ptr, XB_CRYPT_CHUNK_MAGIC_SIZE, buf, len);
	if (memcmp(ptr, XB_CRYPT_CHUNK_MAGIC3,
		   XB_CRYPT_CHUNK_MAGIC_SIZE) == 0) {
		version = 3;
	} else if (memcmp(ptr, XB_CRYPT_CHUNK_MAGIC2,
			  XB_CRYPT_CHUNK_MAGIC_SIZE) == 0) {
		version = 2;
	} else if (memcmp(ptr, XB_CRYPT_CHUNK_MAGIC1,
			  XB_CRYPT_CHUNK_MAGIC_SIZE) == 0) {
		version = 1;
	} else {
		msg("%s:%s: wrong chunk magic at offset 0x%llx.\n",
		    my_progname, __FUNCTION__, thd->offset);
		result = XB_CRYPT_READ_ERROR;
		goto exit;
	}

	ptr += XB_CRYPT_CHUNK_MAGIC_SIZE;
	thd->offset += XB_CRYPT_CHUNK_MAGIC_SIZE;

	CHECK_BUF_SIZE(ptr, 8, buf, len);
	tmp = uint8korr(ptr);	/* reserved */
	ptr += 8;
	thd->offset += 8;

	CHECK_BUF_SIZE(ptr, 8, buf, len);
	tmp = uint8korr(ptr);	/* original size */
	ptr += 8;
	if (tmp > INT_MAX) {
		msg("%s:%s: invalid original size at offset 0x%llx.\n",
		    my_progname, __FUNCTION__, thd->offset);
		result = XB_CRYPT_READ_ERROR;
		goto exit;
	}
	thd->offset += 8;
	thd->to_len = (size_t)tmp;

	if (thd->to_size < thd->to_len + XB_CRYPT_HASH_LEN) {
		thd->to = (uchar *) my_realloc(
				thd->to,
				thd->to_len + XB_CRYPT_HASH_LEN,
				MYF(MY_FAE | MY_ALLOW_ZERO_PTR));
		thd->to_size = thd->to_len;
	}

	CHECK_BUF_SIZE(ptr, 8, buf, len);
	tmp = uint8korr(ptr);	/* encrypted size */
	ptr += 8;
	if (tmp > INT_MAX) {
		msg("%s:%s: invalid encrypted size at offset 0x%llx.\n",
		    my_progname, __FUNCTION__, thd->offset);
		result = XB_CRYPT_READ_ERROR;
		goto exit;
	}
	thd->offset += 8;
	thd->from_len = (size_t)tmp;

	xb_a(thd->from_len <= thd->to_len + XB_CRYPT_HASH_LEN);

	CHECK_BUF_SIZE(ptr, 4, buf, len);
	checksum_exp = uint4korr(ptr);	/* checksum */
	ptr += 4;
	thd->offset += 4;

	/* iv size */
	if (version == 1) {
		thd->iv_len = 0;
		thd->iv = NULL;
	} else {
		CHECK_BUF_SIZE(ptr, 8, buf, len);

		tmp = uint8korr(ptr);
		if (tmp > INT_MAX) {
			msg("%s:%s: invalid iv size at offset 0x%llx.\n",
			    my_progname, __FUNCTION__, thd->offset);
			result = XB_CRYPT_READ_ERROR;
			goto exit;
		}
		ptr += 8;
		thd->offset += 8;
		thd->iv_len = (size_t)tmp;
	}

	if (thd->iv_len > 0) {
		CHECK_BUF_SIZE(ptr, thd->iv_len, buf, len);
		thd->iv = ptr;
		ptr += thd->iv_len;
	}

	/* for version euqals 2 we need to read in the iv data but do not init
	CTR with it */
	if (version == 2) {
		thd->iv_len = 0;
		thd->iv = 0;
	}

	if (thd->from_len > 0) {
		CHECK_BUF_SIZE(ptr, thd->from_len, buf, len);
		thd->from = ptr;
		ptr += thd->from_len;
	}

	xb_ad(thd->from_len <= thd->to_len);

	checksum = crc32_iso3309(0, thd->from, thd->from_len);
	if (checksum != checksum_exp) {
		msg("%s:%s invalid checksum at offset 0x%llx, "
		    "expected 0x%lx, actual 0x%lx.\n", my_progname,
		    __FUNCTION__, thd->offset, checksum_exp, checksum);
		result = XB_CRYPT_READ_ERROR;
		goto exit;
	}

	thd->offset += thd->from_len;

	thd->hash_appended = version > 2;

exit:

	*bytes_processed = (size_t) (ptr - buf);

	return result;
}

static
int
decrypt_write(ds_file_t *file, const void *buf, size_t len)
{
	ds_decrypt_file_t	*crypt_file;
	ds_decrypt_ctxt_t	*crypt_ctxt;
	crypt_thread_ctxt_t	*threads;
	crypt_thread_ctxt_t	*thd;
	uint			nthreads;
	uint			i;
	size_t			bytes_processed;
	xb_rcrypt_result_t	parse_result = XB_CRYPT_READ_CHUNK;
	my_bool			err = FALSE;

	crypt_file = (ds_decrypt_file_t *) file->ptr;
	crypt_ctxt = crypt_file->crypt_ctxt;

	threads = crypt_ctxt->threads;
	nthreads = crypt_ctxt->nthreads;

	if (crypt_file->buf_len > 0) {
		thd = threads;

		pthread_mutex_lock(&thd->ctrl_mutex);

		do {
			if (parse_result == XB_CRYPT_READ_INCOMPLETE) {
				crypt_file->buf_size = crypt_file->buf_size * 2;
				crypt_file->buf = (uchar *) my_realloc(
						crypt_file->buf,
						crypt_file->buf_size,
						MYF(MY_FAE|MY_ALLOW_ZERO_PTR));
			}

			memcpy(crypt_file->buf + crypt_file->buf_len,
			       buf, MY_MIN(crypt_file->buf_size -
					   crypt_file->buf_len, len));

			parse_result = parse_xbcrypt_chunk(
				thd, crypt_file->buf,
				crypt_file->buf_size, &bytes_processed);

			if (parse_result == XB_CRYPT_READ_ERROR) {
				pthread_mutex_unlock(&thd->ctrl_mutex);
				return 1;
			}

		} while (parse_result == XB_CRYPT_READ_INCOMPLETE &&
			 crypt_file->buf_size < len);

		if (parse_result != XB_CRYPT_READ_CHUNK) {
			msg("decrypt: incomplete data.\n");
			pthread_mutex_unlock(&thd->ctrl_mutex);
			return 1;
		}

		pthread_mutex_lock(&thd->data_mutex);
		thd->data_avail = TRUE;
		pthread_cond_signal(&thd->data_cond);
		pthread_mutex_unlock(&thd->data_mutex);

		len -= bytes_processed - crypt_file->buf_len;
		buf += bytes_processed - crypt_file->buf_len;

		/* reap */

		pthread_mutex_lock(&thd->data_mutex);
		while (thd->data_avail == TRUE) {
			pthread_cond_wait(&thd->data_cond,
					  &thd->data_mutex);
		}

		if (thd->failed) {
			msg("decrypt: failed to decrypt chunk.\n");
			err = TRUE;
		}

		xb_a(thd->to_len > 0);

		if (!err &&
		    ds_write(crypt_file->dest_file, thd->to, thd->to_len)) {
			msg("decrypt: write to destination failed.\n");
			err = TRUE;
		}

		crypt_file->bytes_processed += thd->from_len;

		pthread_mutex_unlock(&thd->data_mutex);
		pthread_mutex_unlock(&thd->ctrl_mutex);

		crypt_file->buf_len = 0;

		if (err) {
			return 1;
		}
	}

	while (parse_result == XB_CRYPT_READ_CHUNK && len > 0) {
		uint max_thread;

		for (i = 0; i < nthreads; i++) {
			thd = threads + i;

			pthread_mutex_lock(&thd->ctrl_mutex);

			parse_result = parse_xbcrypt_chunk(
				thd, buf, len, &bytes_processed);

			if (parse_result == XB_CRYPT_READ_ERROR) {
				pthread_mutex_unlock(&thd->ctrl_mutex);
				err = TRUE;
				break;
			}

			thd->parse_result = parse_result;

			if (parse_result != XB_CRYPT_READ_CHUNK) {
				pthread_mutex_unlock(&thd->ctrl_mutex);
				break;
			}

			pthread_mutex_lock(&thd->data_mutex);
			thd->data_avail = TRUE;
			pthread_cond_signal(&thd->data_cond);
			pthread_mutex_unlock(&thd->data_mutex);

			len -= bytes_processed;
			buf += bytes_processed;
		}

		max_thread = (i < nthreads) ? i :  nthreads - 1;

		/* Reap and write decrypted data */
		for (i = 0; i <= max_thread; i++) {
			thd = threads + i;

			if (thd->parse_result != XB_CRYPT_READ_CHUNK) {
				break;
			}

			pthread_mutex_lock(&thd->data_mutex);
			while (thd->data_avail == TRUE) {
				pthread_cond_wait(&thd->data_cond,
						  &thd->data_mutex);
			}

			if (thd->failed) {
				msg("decrypt: failed to decrypt chunk.\n");
				err = TRUE;
			}

			xb_a(thd->to_len > 0);

			if (!err && ds_write(crypt_file->dest_file, thd->to,
				     thd->to_len)) {
				msg("decrypt: write to destination failed.\n");
				err = TRUE;
			}

			crypt_file->bytes_processed += thd->from_len;

			pthread_mutex_unlock(&thd->data_mutex);
			pthread_mutex_unlock(&thd->ctrl_mutex);
		}

		if (err) {
			return 1;
		}
	}

	if (parse_result == XB_CRYPT_READ_INCOMPLETE && len > 0) {
		crypt_file->buf_len = len;
		if (crypt_file->buf_size < len) {
			crypt_file->buf = (uchar *) my_realloc(
					crypt_file->buf,
					crypt_file->buf_len,
					MYF(MY_FAE | MY_ALLOW_ZERO_PTR));
			crypt_file->buf_size = len;
		}
		memcpy(crypt_file->buf, buf, len);
	}

	return 0;
}

static
int
decrypt_close(ds_file_t *file)
{
	ds_decrypt_file_t	*crypt_file;
	ds_file_t		*dest_file;
	int			rc = 0;

	crypt_file = (ds_decrypt_file_t *) file->ptr;
	dest_file = crypt_file->dest_file;

	if (ds_close(dest_file)) {
		rc = 1;
	}

	my_free(crypt_file->buf);
	my_free(file);

	return rc;
}

static
void
decrypt_deinit(ds_ctxt_t *ctxt)
{
	ds_decrypt_ctxt_t 	*crypt_ctxt;

	xb_ad(ctxt->pipe_ctxt != NULL);

	crypt_ctxt = (ds_decrypt_ctxt_t *) ctxt->ptr;

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
		my_malloc(sizeof(crypt_thread_ctxt_t) * n,
			  MYF(MY_FAE | MY_ZEROFILL));

	for (i = 0; i < n; i++) {
		crypt_thread_ctxt_t *thd = threads + i;

		thd->num = i + 1;

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

		xb_crypt_cipher_open(&thd->cipher_handle);

		pthread_mutex_lock(&thd->ctrl_mutex);

		if (pthread_create(&thd->id, NULL, decrypt_worker_thread_func,
				   thd)) {
			msg("decrypt: pthread_create() failed: "
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
	}

	my_free(threads);
}

static
void *
decrypt_worker_thread_func(void *arg)
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

		if (xb_crypt_decrypt(thd->cipher_handle, thd->from,
				     thd->from_len, thd->to, &thd->to_len,
				     thd->iv, thd->iv_len,
				     thd->hash_appended)) {
			thd->failed = TRUE;
			continue;
		}

	}

	pthread_mutex_unlock(&thd->data_mutex);

	return NULL;
}
