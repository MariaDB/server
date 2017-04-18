/******************************************************
Copyright (c) 2013 Percona LLC and/or its affiliates.

The xbcrypt utility: decrypt files in the XBCRYPT format.

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
#include <my_getopt.h>
#include "common.h"
#include "xbcrypt.h"
#include <gcrypt.h>

#if !defined(GCRYPT_VERSION_NUMBER) || (GCRYPT_VERSION_NUMBER < 0x010600)
GCRY_THREAD_OPTION_PTHREAD_IMPL;
#endif

#define XBCRYPT_VERSION "1.1"

typedef enum {
	RUN_MODE_NONE,
	RUN_MODE_ENCRYPT,
	RUN_MODE_DECRYPT
} run_mode_t;

const char *xbcrypt_encrypt_algo_names[] =
{ "NONE", "AES128", "AES192", "AES256", NullS};
TYPELIB xbcrypt_encrypt_algo_typelib=
{array_elements(xbcrypt_encrypt_algo_names)-1,"",
	xbcrypt_encrypt_algo_names, NULL};

static run_mode_t	opt_run_mode = RUN_MODE_ENCRYPT;
static char 		*opt_input_file = NULL;
static char 		*opt_output_file = NULL;
static ulong 		opt_encrypt_algo;
static char 		*opt_encrypt_key_file = NULL;
static void 		*opt_encrypt_key = NULL;
static ulonglong	opt_encrypt_chunk_size = 0;
static my_bool		opt_verbose = FALSE;

static uint 		encrypt_algos[] = { GCRY_CIPHER_NONE,
					    GCRY_CIPHER_AES128,
					    GCRY_CIPHER_AES192,
					    GCRY_CIPHER_AES256 };
static int		encrypt_algo = 0;
static int		encrypt_mode = GCRY_CIPHER_MODE_CTR;
static uint 		encrypt_key_len = 0;
static size_t 		encrypt_iv_len = 0;

static struct my_option my_long_options[] =
{
	{"help", '?', "Display this help and exit.",
	 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"decrypt", 'd', "Decrypt data input to output.",
	 0, 0, 0,
	 GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"input", 'i', "Optional input file. If not specified, input"
	 " will be read from standard input.",
	 &opt_input_file, &opt_input_file, 0,
	 GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"output", 'o', "Optional output file. If not specified, output"
	 " will be written to standard output.",
	 &opt_output_file, &opt_output_file, 0,
	 GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"encrypt-algo", 'a', "Encryption algorithm.",
	 &opt_encrypt_algo, &opt_encrypt_algo, &xbcrypt_encrypt_algo_typelib,
	 GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"encrypt-key", 'k', "Encryption key.",
	 &opt_encrypt_key, &opt_encrypt_key, 0,
	 GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"encrypt-key-file", 'f', "File which contains encryption key.",
	 &opt_encrypt_key_file, &opt_encrypt_key_file, 0,
	 GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"encrypt-chunk-size", 's', "Size of working buffer for encryption in"
	 " bytes. The default value is 64K.",
	 &opt_encrypt_chunk_size, &opt_encrypt_chunk_size, 0,
	GET_ULL, REQUIRED_ARG, (1 << 16), 1024, ULONGLONG_MAX, 0, 0, 0},

	{"verbose", 'v', "Display verbose status output.",
	 &opt_verbose, &opt_verbose,
	  0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static
int
get_options(int *argc, char ***argv);

static
my_bool
get_one_option(int optid, const struct my_option *opt __attribute__((unused)),
	       char *argument __attribute__((unused)));

static
void
print_version(void);

static
void
usage(void);

static
int
mode_decrypt(File filein, File fileout);

static
int
mode_encrypt(File filein, File fileout);

int
main(int argc, char **argv)
{
#if !defined(GCRYPT_VERSION_NUMBER) || (GCRYPT_VERSION_NUMBER < 0x010600)
	gcry_error_t 		gcry_error;
#endif
	File filein = 0;
	File fileout = 0;

	MY_INIT(argv[0]);

	if (get_options(&argc, &argv)) {
		goto err;
	}

	/* Acording to gcrypt docs (and my testing), setting up the threading
	   callbacks must be done first, so, lets give it a shot */
#if !defined(GCRYPT_VERSION_NUMBER) || (GCRYPT_VERSION_NUMBER < 0x010600)
	gcry_error = gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
	if (gcry_error) {
		msg("%s: unable to set libgcrypt thread cbs - "
		    "%s : %s\n", my_progname,
		    gcry_strsource(gcry_error),
		    gcry_strerror(gcry_error));
		return 1;
	}
#endif

	/* Version check should be the very first call because it
	makes sure that important subsystems are intialized. */
	if (!gcry_control(GCRYCTL_ANY_INITIALIZATION_P)) {
		const char	*gcrypt_version;
		gcrypt_version = gcry_check_version(NULL);
		/* No other library has already initialized libgcrypt. */
		if (!gcrypt_version) {
			msg("%s: failed to initialize libgcrypt\n",
			    my_progname);
			return 1;
		} else if (opt_verbose) {
			msg("%s: using gcrypt %s\n", my_progname,
			    gcrypt_version);
		}
	}
	gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
	gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

	/* Determine the algorithm */
	encrypt_algo = encrypt_algos[opt_encrypt_algo];

	/* Set up the iv length */
	encrypt_iv_len = gcry_cipher_get_algo_blklen(encrypt_algo);

	/* Now set up the key */
	if (opt_encrypt_key == NULL && opt_encrypt_key_file == NULL) {
		msg("%s: no encryption key or key file specified.\n",
		    my_progname);
		return 1;
	} else if (opt_encrypt_key && opt_encrypt_key_file) {
		msg("%s: both encryption key and key file specified.\n",
		    my_progname);
		return 1;
	} else if (opt_encrypt_key_file) {
		if (!xb_crypt_read_key_file(opt_encrypt_key_file,
					    &opt_encrypt_key,
					    &encrypt_key_len)) {
			msg("%s: unable to read encryption key file \"%s\".\n",
			    opt_encrypt_key_file, my_progname);
			return 1;
		}
	} else {
		encrypt_key_len = strlen(opt_encrypt_key);
	}

	if (opt_input_file) {
		MY_STAT 	mystat;

		if (opt_verbose)
			msg("%s: input file \"%s\".\n", my_progname,
			    opt_input_file);

		if (my_stat(opt_input_file, &mystat, MYF(MY_WME)) == NULL) {
			goto err;
		}
		if (!MY_S_ISREG(mystat.st_mode)) {
			msg("%s: \"%s\" is not a regular file, exiting.\n",
			    my_progname, opt_input_file);
			goto err;
		}
		if ((filein = my_open(opt_input_file, O_RDONLY, MYF(MY_WME)))
		     < 0) {
			msg("%s: failed to open \"%s\".\n", my_progname,
			     opt_input_file);
			goto err;
		}
	} else {
		if (opt_verbose)
			msg("%s: input from standard input.\n", my_progname);
		filein = fileno(stdin);
	}

	if (opt_output_file) {
		if (opt_verbose)
			msg("%s: output file \"%s\".\n", my_progname,
			    opt_output_file);

		if ((fileout = my_create(opt_output_file, 0,
					 O_WRONLY|O_BINARY|O_EXCL|O_NOFOLLOW,
					 MYF(MY_WME))) < 0) {
			msg("%s: failed to create output file \"%s\".\n",
			    my_progname, opt_output_file);
			goto err;
		}
	} else {
		if (opt_verbose)
			msg("%s: output to standard output.\n", my_progname);
		fileout = fileno(stdout);
	}

	if (opt_run_mode == RUN_MODE_DECRYPT
	    && mode_decrypt(filein, fileout)) {
		goto err;
	} else if (opt_run_mode == RUN_MODE_ENCRYPT
		   && mode_encrypt(filein, fileout)) {
		goto err;
	}

	if (opt_input_file && filein) {
		my_close(filein, MYF(MY_WME));
	}
	if (opt_output_file && fileout) {
		my_close(fileout, MYF(MY_WME));
	}

	my_cleanup_options(my_long_options);

	my_end(0);

	return EXIT_SUCCESS;
err:
	if (opt_input_file && filein) {
		my_close(filein, MYF(MY_WME));
	}
	if (opt_output_file && fileout) {
		my_close(fileout, MYF(MY_WME));
	}

	my_cleanup_options(my_long_options);

	my_end(0);

	exit(EXIT_FAILURE);

}


static
size_t
my_xb_crypt_read_callback(void *userdata, void *buf, size_t len)
{
	File* file = (File *) userdata;
	return xb_read_full(*file, buf, len);
}

static
int
mode_decrypt(File filein, File fileout)
{
	xb_rcrypt_t		*xbcrypt_file = NULL;
	void			*chunkbuf = NULL;
	size_t			chunksize;
	size_t			originalsize;
	void			*ivbuf = NULL;
	size_t			ivsize;
	void			*decryptbuf = NULL;
	size_t			decryptbufsize = 0;
	ulonglong		ttlchunksread = 0;
	ulonglong		ttlbytesread = 0;
	xb_rcrypt_result_t	result;
	gcry_cipher_hd_t	cipher_handle;
	gcry_error_t		gcry_error;
	my_bool			hash_appended;

	if (encrypt_algo != GCRY_CIPHER_NONE) {
		gcry_error = gcry_cipher_open(&cipher_handle,
					      encrypt_algo,
					      encrypt_mode, 0);
		if (gcry_error) {
			msg("%s:decrypt: unable to open libgcrypt"
			    " cipher - %s : %s\n", my_progname,
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			return 1;
		}

		gcry_error = gcry_cipher_setkey(cipher_handle,
						opt_encrypt_key,
						encrypt_key_len);
		if (gcry_error) {
			msg("%s:decrypt: unable to set libgcrypt cipher"
			    "key - %s : %s\n", my_progname,
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			goto err;
		}
	}

	/* Initialize the xb_crypt format reader */
	xbcrypt_file = xb_crypt_read_open(&filein, my_xb_crypt_read_callback);
	if (xbcrypt_file == NULL) {
		msg("%s:decrypt: xb_crypt_read_open() failed.\n", my_progname);
		goto err;
	}

	/* Walk the encrypted chunks, decrypting them and writing out */
	while ((result = xb_crypt_read_chunk(xbcrypt_file, &chunkbuf,
					     &originalsize, &chunksize,
					     &ivbuf, &ivsize, &hash_appended))
		== XB_CRYPT_READ_CHUNK) {

		if (encrypt_algo != GCRY_CIPHER_NONE) {
			gcry_error = gcry_cipher_reset(cipher_handle);
			if (gcry_error) {
				msg("%s:decrypt: unable to reset libgcrypt"
				    " cipher - %s : %s\n", my_progname,
				    gcry_strsource(gcry_error),
				    gcry_strerror(gcry_error));
				goto err;
			}

			if (ivsize) {
				gcry_error = gcry_cipher_setctr(cipher_handle,
								ivbuf,
								ivsize);
			}
			if (gcry_error) {
				msg("%s:decrypt: unable to set cipher iv - "
				    "%s : %s\n", my_progname,
				    gcry_strsource(gcry_error),
				    gcry_strerror(gcry_error));
				continue;
			}

			if (decryptbufsize < originalsize) {
				decryptbuf = my_realloc(decryptbuf,
					originalsize,
					MYF(MY_WME | MY_ALLOW_ZERO_PTR));
				decryptbufsize = originalsize;
			}

			/* Try to decrypt it */
			gcry_error = gcry_cipher_decrypt(cipher_handle,
							 decryptbuf,
							 originalsize,
							 chunkbuf,
							 chunksize);
			if (gcry_error) {
				msg("%s:decrypt: unable to decrypt chunk - "
				    "%s : %s\n", my_progname,
				    gcry_strsource(gcry_error),
				    gcry_strerror(gcry_error));
				gcry_cipher_close(cipher_handle);
					goto err;
			}

		} else {
			decryptbuf = chunkbuf;
		}

		if (hash_appended) {
			uchar hash[XB_CRYPT_HASH_LEN];

			originalsize -= XB_CRYPT_HASH_LEN;

			/* ensure that XB_CRYPT_HASH_LEN is the correct length
			of XB_CRYPT_HASH hashing algorithm output */
			assert(gcry_md_get_algo_dlen(XB_CRYPT_HASH) ==
			       XB_CRYPT_HASH_LEN);
			gcry_md_hash_buffer(XB_CRYPT_HASH, hash, decryptbuf,
					    originalsize);
			if (memcmp(hash, (char *) decryptbuf + originalsize,
				   XB_CRYPT_HASH_LEN) != 0) {
				msg("%s:%s invalid plaintext hash. "
				    "Wrong encrytion key specified?\n",
				    my_progname, __FUNCTION__);
				result = XB_CRYPT_READ_ERROR;
				goto err;
			}
		}

		/* Write it out */
		if (my_write(fileout, (const uchar *) decryptbuf, originalsize,
			     MYF(MY_WME | MY_NABP))) {
			msg("%s:decrypt: unable to write output chunk.\n",
			    my_progname);
			goto err;
		}
		ttlchunksread++;
		ttlbytesread += chunksize;
		if (opt_verbose)
			msg("%s:decrypt: %llu chunks read, %llu bytes read\n.",
		    	    my_progname, ttlchunksread, ttlbytesread);
	}

	xb_crypt_read_close(xbcrypt_file);

	if (encrypt_algo != GCRY_CIPHER_NONE)
		gcry_cipher_close(cipher_handle);

	if (decryptbuf && decryptbufsize)
		my_free(decryptbuf);

	if (opt_verbose)
		msg("\n%s:decrypt: done\n", my_progname);

	return 0;
err:
	if (xbcrypt_file)
		xb_crypt_read_close(xbcrypt_file);

	if (encrypt_algo != GCRY_CIPHER_NONE)
		gcry_cipher_close(cipher_handle);

	if (decryptbuf && decryptbufsize)
		my_free(decryptbuf);

	return 1;
}

static
ssize_t
my_xb_crypt_write_callback(void *userdata, const void *buf, size_t len)
{
	File* file = (File *) userdata;

	ssize_t ret = my_write(*file, buf, len, MYF(MY_WME));
	posix_fadvise(*file, 0, 0, POSIX_FADV_DONTNEED);
	return ret;
}

static
int
mode_encrypt(File filein, File fileout)
{
	size_t			bytesread;
	size_t			chunkbuflen;
	uchar			*chunkbuf = NULL;
	void			*ivbuf = NULL;
	size_t			encryptbuflen = 0;
	size_t			encryptedlen = 0;
	void			*encryptbuf = NULL;
	ulonglong		ttlchunkswritten = 0;
	ulonglong		ttlbyteswritten = 0;
	xb_wcrypt_t		*xbcrypt_file = NULL;
	gcry_cipher_hd_t	cipher_handle;
	gcry_error_t		gcry_error;

	if (encrypt_algo != GCRY_CIPHER_NONE) {
		gcry_error = gcry_cipher_open(&cipher_handle,
					      encrypt_algo,
					      encrypt_mode, 0);
		if (gcry_error) {
			msg("%s:encrypt: unable to open libgcrypt cipher - "
			    "%s : %s\n", my_progname,
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			return 1;
		}

		gcry_error = gcry_cipher_setkey(cipher_handle,
						opt_encrypt_key,
						encrypt_key_len);
		if (gcry_error) {
			msg("%s:encrypt: unable to set libgcrypt cipher key - "
			    "%s : %s\n", my_progname,
			    gcry_strsource(gcry_error),
			    gcry_strerror(gcry_error));
			goto err;
		}
	}

	posix_fadvise(filein, 0, 0, POSIX_FADV_SEQUENTIAL);

	xbcrypt_file = xb_crypt_write_open(&fileout,
					   my_xb_crypt_write_callback);
	if (xbcrypt_file == NULL) {
		msg("%s:encrypt: xb_crypt_write_open() failed.\n",
		    my_progname);
		goto err;
	}

	ivbuf = my_malloc(encrypt_iv_len, MYF(MY_FAE));

	/* now read in data in chunk size, encrypt and write out */
	chunkbuflen = opt_encrypt_chunk_size + XB_CRYPT_HASH_LEN;
	chunkbuf = (uchar *) my_malloc(chunkbuflen, MYF(MY_FAE));
	while ((bytesread = my_read(filein, chunkbuf, opt_encrypt_chunk_size,
				    MYF(MY_WME))) > 0) {

		size_t origbuflen = bytesread + XB_CRYPT_HASH_LEN;

		/* ensure that XB_CRYPT_HASH_LEN is the correct length
		of XB_CRYPT_HASH hashing algorithm output */
		assert(XB_CRYPT_HASH_LEN ==
		       gcry_md_get_algo_dlen(XB_CRYPT_HASH));
		gcry_md_hash_buffer(XB_CRYPT_HASH, chunkbuf + bytesread,
				    chunkbuf, bytesread);

		if (encrypt_algo != GCRY_CIPHER_NONE) {
			gcry_error = gcry_cipher_reset(cipher_handle);

			if (gcry_error) {
				msg("%s:encrypt: unable to reset cipher - "
				    "%s : %s\n", my_progname,
				    gcry_strsource(gcry_error),
				    gcry_strerror(gcry_error));
				goto err;
			}

			xb_crypt_create_iv(ivbuf, encrypt_iv_len);
			gcry_error = gcry_cipher_setctr(cipher_handle,
							ivbuf,
							encrypt_iv_len);

			if (gcry_error) {
				msg("%s:encrypt: unable to set cipher iv - "
				    "%s : %s\n", my_progname,
				    gcry_strsource(gcry_error),
				    gcry_strerror(gcry_error));
				continue;
			}

			if (encryptbuflen < origbuflen) {
				encryptbuf = my_realloc(encryptbuf, origbuflen,
					MYF(MY_WME | MY_ALLOW_ZERO_PTR));
				encryptbuflen = origbuflen;
			}

			gcry_error = gcry_cipher_encrypt(cipher_handle,
							 encryptbuf,
							 encryptbuflen,
							 chunkbuf,
							 origbuflen);

			encryptedlen = origbuflen;

			if (gcry_error) {
				msg("%s:encrypt: unable to encrypt chunk - "
				    "%s : %s\n", my_progname,
				    gcry_strsource(gcry_error),
				    gcry_strerror(gcry_error));
				gcry_cipher_close(cipher_handle);
				goto err;
			}
		} else {
			encryptedlen = origbuflen;
			encryptbuf = chunkbuf;
		}

		if (xb_crypt_write_chunk(xbcrypt_file, encryptbuf,
					 bytesread + XB_CRYPT_HASH_LEN,
					 encryptedlen, ivbuf, encrypt_iv_len)) {
			msg("%s:encrypt: abcrypt_write_chunk() failed.\n",
			    my_progname);
			goto err;
		}

		ttlchunkswritten++;
		ttlbyteswritten += encryptedlen;

		if (opt_verbose)
			msg("%s:encrypt: %llu chunks written, %llu bytes "
			    "written\n.", my_progname, ttlchunkswritten,
			    ttlbyteswritten);
	}

	my_free(ivbuf);
	my_free(chunkbuf);

	if (encryptbuf && encryptbuflen)
		my_free(encryptbuf);

	xb_crypt_write_close(xbcrypt_file);

	if (encrypt_algo != GCRY_CIPHER_NONE)
		gcry_cipher_close(cipher_handle);

	if (opt_verbose)
		msg("\n%s:encrypt: done\n", my_progname);

	return 0;
err:
	if (chunkbuf)
		my_free(chunkbuf);

	if (encryptbuf && encryptbuflen)
		my_free(encryptbuf);

	if (xbcrypt_file)
		xb_crypt_write_close(xbcrypt_file);

	if (encrypt_algo != GCRY_CIPHER_NONE)
		gcry_cipher_close(cipher_handle);

	return 1;
}

static
int
get_options(int *argc, char ***argv)
{
	int ho_error;

	if ((ho_error= handle_options(argc, argv, my_long_options,
				      get_one_option))) {
		exit(EXIT_FAILURE);
	}

	return 0;
}

static
my_bool
get_one_option(int optid, const struct my_option *opt __attribute__((unused)),
	       char *argument __attribute__((unused)))
{
	switch (optid) {
	case 'd':
		opt_run_mode = RUN_MODE_DECRYPT;
		break;
	case '?':
		usage();
		exit(0);
	}

	return FALSE;
}

static
void
print_version(void)
{
	printf("%s  Ver %s for %s (%s)\n", my_progname, XBCRYPT_VERSION,
	       SYSTEM_TYPE, MACHINE_TYPE);
}

static
void
usage(void)
{
	print_version();
	puts("Copyright (C) 2011 Percona Inc.");
	puts("This software comes with ABSOLUTELY NO WARRANTY. "
	     "This is free software,\nand you are welcome to modify and "
	     "redistribute it under the GPL license.\n");

	puts("Encrypt or decrypt files in the XBCRYPT format.\n");

	puts("Usage: ");
	printf("  %s [OPTIONS...]"
 	       " # read data from specified input, encrypting or decrypting "
	       " and writing the result to the specified output.\n",
	       my_progname);
	puts("\nOptions:");
	my_print_help(my_long_options);
}
