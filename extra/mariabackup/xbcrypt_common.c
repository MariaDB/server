/******************************************************
Copyright (c) 2013, 2017 Percona LLC and/or its affiliates.

Encryption configuration file interface for XtraBackup.

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
#include "xbcrypt.h"
#include "xbcrypt_common.h"

/* Encryption options */
char		*ds_encrypt_key = NULL;
char		*ds_encrypt_key_file = NULL;
ulong		ds_encrypt_algo;

static uint	encrypt_key_len;
static uint	encrypt_iv_len;

static const uint encrypt_mode = GCRY_CIPHER_MODE_CTR;

static uint encrypt_algos[] = { GCRY_CIPHER_NONE, GCRY_CIPHER_AES128,
				GCRY_CIPHER_AES192, GCRY_CIPHER_AES256 };
static uint encrypt_algo;

#if !defined(GCRYPT_VERSION_NUMBER) || (GCRYPT_VERSION_NUMBER < 0x010600)
GCRY_THREAD_OPTION_PTHREAD_IMPL;
#endif


my_bool
xb_crypt_read_key_file(const char *filename, void** key, uint *keylength)
{
	FILE *fp;

	if (!(fp = my_fopen(filename, O_RDONLY, MYF(0)))) {
		msg("%s:%s: unable to open config file \"%s\", errno(%d)\n",
			my_progname, __FUNCTION__, filename, my_errno);
		return FALSE;
	}

	fseek(fp, 0 , SEEK_END);
	*keylength = ftell(fp);
	rewind(fp);
	*key = my_malloc(*keylength, MYF(MY_FAE));
	*keylength = fread(*key, 1, *keylength, fp);
	my_fclose(fp, MYF(0));
	return TRUE;
}

void
xb_crypt_create_iv(void* ivbuf, size_t ivlen)
{
	gcry_create_nonce(ivbuf, ivlen);
}

gcry_error_t
xb_crypt_init(uint *iv_len)
{
	gcry_error_t 		gcry_error;

	/* Acording to gcrypt docs (and my testing), setting up the threading
	   callbacks must be done first, so, lets give it a shot */
#if !defined(GCRYPT_VERSION_NUMBER) || (GCRYPT_VERSION_NUMBER < 0x010600)
	gcry_error = gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
	if (gcry_error) {
		msg("encryption: unable to set libgcrypt thread cbs - "
		    "%s : %s\n",
		    gcry_strsource(gcry_error),
		    gcry_strerror(gcry_error));
		return gcry_error;
	}
#endif

	/* Version check should be the very next call because it
	makes sure that important subsystems are intialized. */
	if (!gcry_control(GCRYCTL_ANY_INITIALIZATION_P)) {
		const char	*gcrypt_version;
		gcrypt_version = gcry_check_version(NULL);
		/* No other library has already initialized libgcrypt. */
		if (!gcrypt_version) {
			msg("encryption: failed to initialize libgcrypt\n");
			return 1;
		} else {
			msg("encryption: using gcrypt %s\n", gcrypt_version);
		}
	}

	/* Disable the gcry secure memory, not dealing with this for now */
	gcry_error = gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
	if (gcry_error) {
		msg("encryption: unable to disable libgcrypt secmem - "
		    "%s : %s\n",
		    gcry_strsource(gcry_error),
		    gcry_strerror(gcry_error));
		return gcry_error;
	}

	/* Finalize gcry initialization. */
	gcry_error = gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
	if (gcry_error) {
		msg("encryption: unable to finish libgcrypt initialization - "
		    "%s : %s\n",
		    gcry_strsource(gcry_error),
		    gcry_strerror(gcry_error));
		return gcry_error;
	}

	/* Determine the algorithm */
	encrypt_algo = encrypt_algos[ds_encrypt_algo];

	/* Set up the iv length */
	encrypt_iv_len = gcry_cipher_get_algo_blklen(encrypt_algo);
	xb_a(encrypt_iv_len > 0);
	if (iv_len != NULL) {
		*iv_len = encrypt_iv_len;
	}

	/* Now set up the key */
	if (ds_encrypt_key == NULL &&
	    ds_encrypt_key_file == NULL) {
		msg("encryption: no encryption key or key file specified.\n");
		return gcry_error;
	} else if (ds_encrypt_key && ds_encrypt_key_file) {
		msg("encryption: both encryption key and key file specified.\n");
		return gcry_error;
	} else if (ds_encrypt_key_file) {
		if (!xb_crypt_read_key_file(ds_encrypt_key_file,
					    (void**)&ds_encrypt_key,
					    &encrypt_key_len)) {
			msg("encryption: unable to read encryption key file"
			    " \"%s\".\n", ds_encrypt_key_file);
			return gcry_error;
		}
	} else if (ds_encrypt_key) {
		encrypt_key_len = strlen(ds_encrypt_key);
	} else {
		msg("encryption: no encryption key or key file specified.\n");
		return gcry_error;
	}

	return 0;
}

gcry_error_t
xb_crypt_cipher_open(gcry_cipher_hd_t *cipher_handle)
{
	if (encrypt_algo != GCRY_CIPHER_NONE) {
		gcry_error_t 		gcry_error;

		gcry_error = gcry_cipher_open(cipher_handle,
					      encrypt_algo,
					      encrypt_mode, 0);
		if (gcry_error) {
			msg("encryption: unable to open libgcrypt"
			    " cipher - %s : %s\n",
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			gcry_cipher_close(*cipher_handle);
			return gcry_error;
		}

		gcry_error = gcry_cipher_setkey(*cipher_handle,
						ds_encrypt_key,
						encrypt_key_len);
		if (gcry_error) {
			msg("encryption: unable to set libgcrypt"
			    " cipher key - %s : %s\n",
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			gcry_cipher_close(*cipher_handle);
			return gcry_error;
		}
		return gcry_error;
	}
	return 0;
}

void
xb_crypt_cipher_close(gcry_cipher_hd_t cipher_handle)
{
	if (encrypt_algo != GCRY_CIPHER_NONE)
		gcry_cipher_close(cipher_handle);
}

gcry_error_t
xb_crypt_decrypt(gcry_cipher_hd_t cipher_handle, const uchar *from,
		 size_t from_len, uchar *to, size_t *to_len,
		 const uchar *iv, size_t iv_len, my_bool hash_appended)
{
	*to_len = from_len;

	if (encrypt_algo != GCRY_CIPHER_NONE) {

		gcry_error_t	gcry_error;

		gcry_error = gcry_cipher_reset(cipher_handle);
		if (gcry_error) {
			msg("%s:encryption: unable to reset libgcrypt"
			    " cipher - %s : %s\n", my_progname,
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			return gcry_error;
		}

		if (iv_len > 0) {
			gcry_error = gcry_cipher_setctr(cipher_handle,
							iv, iv_len);
		}
		if (gcry_error) {
			msg("%s:encryption: unable to set cipher iv - "
			    "%s : %s\n", my_progname,
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			return gcry_error;
		}

		/* Try to decrypt it */
		gcry_error = gcry_cipher_decrypt(cipher_handle, to, *to_len,
						 from, from_len);
		if (gcry_error) {
			msg("%s:encryption: unable to decrypt chunk - "
			    "%s : %s\n", my_progname,
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			gcry_cipher_close(cipher_handle);
			return gcry_error;
		}

		if (hash_appended) {
			uchar hash[XB_CRYPT_HASH_LEN];

			*to_len -= XB_CRYPT_HASH_LEN;

			/* ensure that XB_CRYPT_HASH_LEN is the correct length
			of XB_CRYPT_HASH hashing algorithm output */
			xb_ad(gcry_md_get_algo_dlen(XB_CRYPT_HASH) ==
			      XB_CRYPT_HASH_LEN);
			gcry_md_hash_buffer(XB_CRYPT_HASH, hash, to,
					    *to_len);
			if (memcmp(hash, (char *) to + *to_len,
				   XB_CRYPT_HASH_LEN) != 0) {
				msg("%s:%s invalid plaintext hash. "
				    "Wrong encrytion key specified?\n",
				    my_progname, __FUNCTION__);
				return 1;
			}
		}

	} else {
		memcpy(to, from, *to_len);
	}

	return 0;
}

gcry_error_t
xb_crypt_encrypt(gcry_cipher_hd_t cipher_handle, const uchar *from,
		 size_t from_len, uchar *to, size_t *to_len, uchar *iv)
{
	gcry_error_t 		gcry_error;

	/* ensure that XB_CRYPT_HASH_LEN is the correct length
	of XB_CRYPT_HASH hashing algorithm output */
	xb_ad(gcry_md_get_algo_dlen(XB_CRYPT_HASH) ==
	      XB_CRYPT_HASH_LEN);

	memcpy(to, from, from_len);
	gcry_md_hash_buffer(XB_CRYPT_HASH, to + from_len,
			    from, from_len);

	*to_len = from_len;

	if (encrypt_algo != GCRY_CIPHER_NONE) {

		gcry_error = gcry_cipher_reset(cipher_handle);
		if (gcry_error) {
			msg("encrypt: unable to reset cipher - "
			    "%s : %s\n",
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			return gcry_error;
		}

		xb_crypt_create_iv(iv, encrypt_iv_len);
		gcry_error = gcry_cipher_setctr(cipher_handle, iv,
						encrypt_iv_len);
		if (gcry_error) {
			msg("encrypt: unable to set cipher ctr - "
			    "%s : %s\n",
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			return gcry_error;
		}

		gcry_error = gcry_cipher_encrypt(cipher_handle, to,
						 *to_len + XB_CRYPT_HASH_LEN,
						 to,
						 from_len + XB_CRYPT_HASH_LEN);
		if (gcry_error) {
			msg("encrypt: unable to encrypt buffer - "
			    "%s : %s\n", gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			return gcry_error;
		}
	} else {
		memcpy(to, from, from_len + XB_CRYPT_HASH_LEN);
	}

	*to_len += XB_CRYPT_HASH_LEN;

	return 0;
}
#endif