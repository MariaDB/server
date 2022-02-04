/*****************************************************************************
Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (c) 2015, 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/fil0crypt.h
The low-level file system encryption support functions

Created 04/01/2015 Jan Lindström
*******************************************************/

#ifndef fil0crypt_h
#define fil0crypt_h

#include "my_crypt.h"
#include "fil0fil.h"

/**
* Magic pattern in start of crypt data on page 0
*/
#define MAGIC_SZ 6

static const unsigned char CRYPT_MAGIC[MAGIC_SZ] = {
	's', 0xE, 0xC, 'R', 'E', 't' };

/* This key will be used if nothing else is given */
#define FIL_DEFAULT_ENCRYPTION_KEY ENCRYPTION_KEY_SYSTEM_DATA

/** Wake up the encryption threads */
void fil_crypt_threads_signal(bool broadcast= false);

/**
 * CRYPT_SCHEME_UNENCRYPTED
 *
 * Used as intermediate state when convering a space from unencrypted
 * to encrypted
 */
/**
 * CRYPT_SCHEME_1
 *
 * xxx is AES_CTR or AES_CBC (or another block cypher with the same key and iv lengths)
 *  L = AES_ECB(KEY, IV)
 *  CRYPT(PAGE) = xxx(KEY=L, IV=C, PAGE)
 */

#define CRYPT_SCHEME_1 1
#define CRYPT_SCHEME_1_IV_LEN 16
#define CRYPT_SCHEME_UNENCRYPTED 0

/* Cached L or key for given key_version */
struct key_struct
{
	uint key_version;			/*!< Version of the key */
	uint key_length;			/*!< Key length */
	unsigned char key[MY_AES_MAX_KEY_LENGTH]; /*!< Cached key
                                                (that is L in CRYPT_SCHEME_1) */
};

/** is encryption enabled */
extern ulong	srv_encrypt_tables;

/** Mutex helper for crypt_data->scheme
@param[in, out]	schme	encryption scheme
@param[in]	exit	should we exit or enter mutex ? */
void
crypt_data_scheme_locker(
	st_encryption_scheme*	scheme,
	int			exit);

struct fil_space_rotate_state_t
{
	time_t start_time;	/*!< time when rotation started */
	ulint active_threads;	/*!< active threads in space */
	uint32_t next_offset;	/*!< next "free" offset */
	uint32_t max_offset;	/*!< max offset needing to be rotated */
	uint  min_key_version_found; /*!< min key version found but not
				     rotated */
	lsn_t end_lsn;		/*!< max lsn created when rotating this
				space */
	bool starting;		/*!< initial write of IV */
	bool flushing;		/*!< space is being flushed at end of rotate */
};

#ifndef UNIV_INNOCHECKSUM

struct fil_space_crypt_t : st_encryption_scheme
{
 public:
	/** Constructor. Does not initialize the members!
	The object is expected to be placed in a buffer that
	has been zero-initialized. */
	fil_space_crypt_t(
		uint new_type,
		uint new_min_key_version,
		uint new_key_id,
		fil_encryption_t new_encryption)
		: st_encryption_scheme(),
		min_key_version(new_min_key_version),
		encryption(new_encryption),
		key_found(0),
		rotate_state()
	{
		key_id = new_key_id;
		my_random_bytes(iv, sizeof(iv));
		mysql_mutex_init(0, &mutex, nullptr);
		locker = crypt_data_scheme_locker;
		type = new_type;

		if (new_encryption == FIL_ENCRYPTION_OFF ||
			(!srv_encrypt_tables &&
			 new_encryption == FIL_ENCRYPTION_DEFAULT)) {
			type = CRYPT_SCHEME_UNENCRYPTED;
		} else {
			type = CRYPT_SCHEME_1;
			min_key_version = key_get_latest_version();
		}

		key_found = min_key_version;
	}

	/** Destructor */
	~fil_space_crypt_t()
	{
		mysql_mutex_destroy(&mutex);
	}

	/** Get latest key version from encryption plugin
	@retval key_version or
	@retval ENCRYPTION_KEY_VERSION_INVALID if used key_id
	is not found from encryption plugin. */
	uint key_get_latest_version(void);

	/** Returns true if key was found from encryption plugin
	and false if not. */
	bool is_key_found() const {
		return key_found != ENCRYPTION_KEY_VERSION_INVALID;
	}

	/** Returns true if tablespace should be encrypted */
	bool should_encrypt() const {
		return ((encryption == FIL_ENCRYPTION_ON) ||
			(srv_encrypt_tables &&
				encryption == FIL_ENCRYPTION_DEFAULT));
	}

	/** Return true if tablespace is encrypted. */
	bool is_encrypted() const {
		return (encryption != FIL_ENCRYPTION_OFF);
	}

	/** Return true if default tablespace encryption is used, */
	bool is_default_encryption() const {
		return (encryption == FIL_ENCRYPTION_DEFAULT);
	}

	/** Return true if tablespace is not encrypted. */
	bool not_encrypted() const {
		return (encryption == FIL_ENCRYPTION_OFF);
	}

	/** Write encryption metadata to the first page.
	@param[in,out]	block	first page of the tablespace
	@param[in,out]	mtr	mini-transaction */
	void write_page0(buf_block_t* block, mtr_t* mtr);

	uint min_key_version; // min key version for this space
	fil_encryption_t encryption; // Encryption setup

	mysql_mutex_t mutex;   // mutex protecting following variables

	/** Return code from encryption_key_get_latest_version.
        If ENCRYPTION_KEY_VERSION_INVALID encryption plugin
	could not find the key and there is no need to call
	get_latest_key_version again as keys are read only
	at startup. */
	uint key_found;

	fil_space_rotate_state_t rotate_state;
};

/** Status info about encryption */
struct fil_space_crypt_status_t {
	ulint space;             /*!< tablespace id */
	ulint scheme;            /*!< encryption scheme */
	uint  min_key_version;   /*!< min key version */
	uint  current_key_version;/*!< current key version */
	uint  keyserver_requests;/*!< no of key requests to key server */
	uint key_id;            /*!< current key_id */
	bool rotating;           /*!< is key rotation ongoing */
	bool flushing;           /*!< is flush at end of rotation ongoing */
	ulint rotate_next_page_number; /*!< next page if key rotating */
	ulint rotate_max_page_number;  /*!< max page if key rotating */
};

/** Statistics about encryption key rotation */
struct fil_crypt_stat_t
{
  ulint pages_read_from_cache= 0;
  ulint pages_read_from_disk= 0;
  ulint pages_modified= 0;
  ulint pages_flushed= 0;
  ulint estimated_iops= 0;
};

/** Init space crypt */
void fil_space_crypt_init();

/** Cleanup space crypt */
void fil_space_crypt_cleanup();

/**
Create a fil_space_crypt_t object
@param[in]	encrypt_mode	FIL_ENCRYPTION_DEFAULT or
				FIL_ENCRYPTION_ON or
				FIL_ENCRYPTION_OFF

@param[in]	key_id		Encryption key id
@return crypt object */
fil_space_crypt_t*
fil_space_create_crypt_data(
	fil_encryption_t	encrypt_mode,
	uint			key_id)
	MY_ATTRIBUTE((warn_unused_result));

/** Initialize encryption parameters from a tablespace header page.
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	page		first page of the tablespace
@return crypt data from page 0
@retval	NULL	if not present or not valid */
fil_space_crypt_t* fil_space_read_crypt_data(ulint zip_size, const byte* page)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/**
Free a crypt data object
@param[in,out] crypt_data	crypt data to be freed */
void fil_space_destroy_crypt_data(fil_space_crypt_t **crypt_data);

/** Amend encryption information from redo log.
@param[in]	space	tablespace
@param[in]	data	encryption metadata */
void fil_crypt_parse(fil_space_t* space, const byte* data);

/** Encrypt a buffer.
@param[in,out]		crypt_data		Crypt data
@param[in]		space			space_id
@param[in]		offset			Page offset
@param[in]		src_frame		Page to encrypt
@param[in]		zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]		dst_frame		Output buffer
@param[in]		use_full_checksum	full crc32 algo is used
@return encrypted buffer or NULL */
byte*
fil_encrypt_buf(
	fil_space_crypt_t*	crypt_data,
	ulint			space,
	ulint			offset,
	const byte*		src_frame,
	ulint			zip_size,
	byte*			dst_frame,
	bool			use_full_checksum)
	MY_ATTRIBUTE((warn_unused_result));

/**
Encrypt a page.

@param[in]		space		Tablespace
@param[in]		offset		Page offset
@param[in]		src_frame	Page to encrypt
@param[in,out]		dst_frame	Output buffer
@return encrypted buffer or NULL */
byte* fil_space_encrypt(
	const fil_space_t* space,
	ulint		offset,
	byte*		src_frame,
	byte*		dst_frame)
	MY_ATTRIBUTE((warn_unused_result));

/** Decrypt a page.
@param]in]	space_id		space id
@param[in]	fsp_flags		Tablespace flags
@param[in]	crypt_data		crypt_data
@param[in]	tmp_frame		Temporary buffer
@param[in]	physical_size		page size
@param[in,out]	src_frame		Page to decrypt
@return DB_SUCCESS or error */
dberr_t
fil_space_decrypt(
	uint32_t		space_id,
	uint32_t		fsp_flags,
	fil_space_crypt_t*	crypt_data,
	byte*			tmp_frame,
	ulint			physical_size,
	byte*			src_frame);

/******************************************************************
Decrypt a page
@param[in]	space			Tablespace
@param[in]	tmp_frame		Temporary buffer used for decrypting
@param[in,out]	src_frame		Page to decrypt
@return decrypted page, or original not encrypted page if decryption is
not needed.*/
byte*
fil_space_decrypt(
	const fil_space_t* space,
	byte*		tmp_frame,
	byte*		src_frame)
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************
Adjust thread count for key rotation
@param[in]	enw_cnt		Number of threads to be used */
void fil_crypt_set_thread_cnt(const uint new_cnt);

/*********************************************************************
Adjust max key age
@param[in]	val		New max key age */
void fil_crypt_set_rotate_key_age(uint val);

/*********************************************************************
Adjust rotation iops
@param[in]	val		New max roation iops */
void fil_crypt_set_rotation_iops(uint val);

/*********************************************************************
Adjust encrypt tables
@param[in]	val		New setting for innodb-encrypt-tables */
void fil_crypt_set_encrypt_tables(ulong val);

/*********************************************************************
Init threads for key rotation */
void fil_crypt_threads_init();

/*********************************************************************
Clean up key rotation threads resources */
void fil_crypt_threads_cleanup();

/*********************************************************************
Wait for crypt threads to stop accessing space
@param[in]	space		Tablespace */
void fil_space_crypt_close_tablespace(const fil_space_t *space);

/*********************************************************************
Get crypt status for a space (used by information_schema)
@param[in]	space		Tablespace
@param[out]	status		Crypt status
return 0 if crypt data present */
void
fil_space_crypt_get_status(
	const fil_space_t*			space,
	struct fil_space_crypt_status_t*	status);

/*********************************************************************
Return crypt statistics
@param[out]	stat		Crypt statistics */
void fil_crypt_total_stat(fil_crypt_stat_t *stat);

#include "fil0crypt.inl"
#endif /* !UNIV_INNOCHECKSUM */

/**
Verify that post encryption checksum match calculated checksum.
This function should be called only if tablespace contains crypt_data
metadata (this is strong indication that tablespace is encrypted).
Function also verifies that traditional checksum does not match
calculated checksum as if it does page could be valid unencrypted,
encrypted, or corrupted.

@param[in,out]	page		page frame (checksum is temporarily modified)
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return true if page is encrypted AND OK, false otherwise */
bool fil_space_verify_crypt_checksum(const byte* page, ulint zip_size)
	MY_ATTRIBUTE((warn_unused_result));

/** Add the tablespace to the rotation list if
innodb_encrypt_rotate_key_age is 0 or encryption plugin does
not do key version rotation
@return whether the tablespace should be added to rotation list */
bool fil_crypt_must_default_encrypt();

#endif /* fil0crypt_h */
