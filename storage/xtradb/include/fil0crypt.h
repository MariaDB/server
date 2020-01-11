/*****************************************************************************
Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (c) 2015, 2017, MariaDB Corporation.

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

#include "os0sync.h"

/**
* Magic pattern in start of crypt data on page 0
*/
#define MAGIC_SZ 6

static const unsigned char CRYPT_MAGIC[MAGIC_SZ] = {
	's', 0xE, 0xC, 'R', 'E', 't' };

/* This key will be used if nothing else is given */
#define FIL_DEFAULT_ENCRYPTION_KEY ENCRYPTION_KEY_SYSTEM_DATA

extern os_event_t fil_crypt_threads_event;

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

#ifdef UNIV_PFS_MUTEX
extern mysql_pfs_key_t fil_crypt_data_mutex_key;
#endif

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
	ulint next_offset;	/*!< next "free" offset */
	ulint max_offset;	/*!< max offset needing to be rotated */
	uint  min_key_version_found; /*!< min key version found but not
				     rotated */
	lsn_t end_lsn;		/*!< max lsn created when rotating this
				space */
	bool starting;		/*!< initial write of IV */
	bool flushing;		/*!< space is being flushed at end of rotate */
	struct {
		bool is_active; /*!< is scrubbing active in this space */
		time_t last_scrub_completed; /*!< when was last scrub
					     completed */
	} scrubbing;
};

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
		page0_offset(0),
		encryption(new_encryption),
		key_found(0),
		rotate_state()
	{
		key_id = new_key_id;
		my_random_bytes(iv, sizeof(iv));
		mutex_create(fil_crypt_data_mutex_key,
			&mutex, SYNC_NO_ORDER_CHECK);
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
		mutex_free(&mutex);
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

	/** Write crypt data to a page (0)
	@param[in,out]	page0		Page 0 where to write
	@param[in,out]	mtr		Minitransaction */
	void write_page0(byte* page0, mtr_t* mtr);

	uint min_key_version; // min key version for this space
	ulint page0_offset;   // byte offset on page 0 for crypt data
	fil_encryption_t encryption; // Encryption setup

	ib_mutex_t mutex;   // mutex protecting following variables

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
	ulint key_id;            /*!< current key_id */
	bool rotating;           /*!< is key rotation ongoing */
	bool flushing;           /*!< is flush at end of rotation ongoing */
	ulint rotate_next_page_number; /*!< next page if key rotating */
	ulint rotate_max_page_number;  /*!< max page if key rotating */
};

/** Statistics about encryption key rotation */
struct fil_crypt_stat_t {
	ulint pages_read_from_cache;
	ulint pages_read_from_disk;
	ulint pages_modified;
	ulint pages_flushed;
	ulint estimated_iops;
};

/** Status info about scrubbing */
struct fil_space_scrub_status_t {
	ulint space;             /*!< tablespace id */
	bool compressed;        /*!< is space compressed  */
	time_t last_scrub_completed;  /*!< when was last scrub completed */
	bool scrubbing;               /*!< is scrubbing ongoing */
	time_t current_scrub_started; /*!< when started current scrubbing */
	ulint current_scrub_active_threads; /*!< current scrub active threads */
	ulint current_scrub_page_number; /*!< current scrub page no */
	ulint current_scrub_max_page_number; /*!< current scrub max page no */
};

/*********************************************************************
Init space crypt */
UNIV_INTERN
void
fil_space_crypt_init();

/*********************************************************************
Cleanup space crypt */
UNIV_INTERN
void
fil_space_crypt_cleanup();

/******************************************************************
Create a fil_space_crypt_t object
@param[in]	encrypt_mode	FIL_ENCRYPTION_DEFAULT or
				FIL_ENCRYPTION_ON or
				FIL_ENCRYPTION_OFF

@param[in]	key_id		Encryption key id
@return crypt object */
UNIV_INTERN
fil_space_crypt_t*
fil_space_create_crypt_data(
	fil_encryption_t	encrypt_mode,
	uint			key_id)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************
Merge fil_space_crypt_t object
@param[in,out]	dst		Destination cryp data
@param[in]	src		Source crypt data */
UNIV_INTERN
void
fil_space_merge_crypt_data(
	fil_space_crypt_t* dst,
	const fil_space_crypt_t* src);

/******************************************************************
Read crypt data from a page (0)
@param[in]	space		space_id
@param[in]	page		Page 0
@param[in]	offset		Offset to crypt data
@return crypt data from page 0 or NULL. */
UNIV_INTERN
fil_space_crypt_t*
fil_space_read_crypt_data(
	ulint		space,
	const byte*	page,
	ulint		offset)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************
Free a crypt data object
@param[in,out] crypt_data	crypt data to be freed */
UNIV_INTERN
void
fil_space_destroy_crypt_data(
	fil_space_crypt_t **crypt_data);

/******************************************************************
Parse a MLOG_FILE_WRITE_CRYPT_DATA log entry
@param[in]	ptr		Log entry start
@param[in]	end_ptr		Log entry end
@param[in]	block		buffer block
@param[out]	err		DB_SUCCESS or DB_DECRYPTION_FAILED
@return position on log buffer */
UNIV_INTERN
byte*
fil_parse_write_crypt_data(
	byte*			ptr,
	const byte*		end_ptr,
	const buf_block_t*	block,
	dberr_t*		err)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************
Encrypt a buffer
@param[in,out]		crypt_data	Crypt data
@param[in]		space		space_id
@param[in]		offset		Page offset
@param[in]		lsn		Log sequence number
@param[in]		src_frame	Page to encrypt
@param[in]		zip_size	Compressed size or 0
@param[in,out]		dst_frame	Output buffer
@return encrypted buffer or NULL */
UNIV_INTERN
byte*
fil_encrypt_buf(
	fil_space_crypt_t* crypt_data,
	ulint		space,
	ulint		offset,
	lsn_t		lsn,
	const byte*	src_frame,
	ulint		zip_size,
	byte*		dst_frame)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************
Encrypt a page

@param[in]		space		Tablespace
@param[in]		offset		Page offset
@param[in]		lsn		Log sequence number
@param[in]		src_frame	Page to encrypt
@param[in,out]		dst_frame	Output buffer
@return encrypted buffer or NULL */
UNIV_INTERN
byte*
fil_space_encrypt(
	const fil_space_t* space,
	ulint		offset,
	lsn_t		lsn,
	byte*		src_frame,
	byte*		dst_frame)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************
Decrypt a page
@param[in,out]	crypt_data		crypt_data
@param[in]	tmp_frame		Temporary buffer
@param[in]	page_size		Page size
@param[in,out]	src_frame		Page to decrypt
@param[out]	err			DB_SUCCESS or error
@return true if page decrypted, false if not.*/
UNIV_INTERN
bool
fil_space_decrypt(
	fil_space_crypt_t*	crypt_data,
	byte*			tmp_frame,
	ulint			page_size,
	byte*			src_frame,
	dberr_t*		err);

/******************************************************************
Decrypt a page
@param[in]	space			Tablespace
@param[in]	tmp_frame		Temporary buffer used for decrypting
@param[in]	page_size		Page size
@param[in,out]	src_frame		Page to decrypt
@param[out]	decrypted		true if page was decrypted
@return decrypted page, or original not encrypted page if decryption is
not needed.*/
UNIV_INTERN
byte*
fil_space_decrypt(
	const fil_space_t* space,
	byte*		tmp_frame,
	byte*		src_frame,
	bool*		decrypted)
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************
Calculate post encryption checksum
@param[in]	zip_size	zip_size or 0
@param[in]	dst_frame	Block where checksum is calculated
@return page checksum or BUF_NO_CHECKSUM_MAGIC
not needed. */
UNIV_INTERN
ulint
fil_crypt_calculate_checksum(
	ulint		zip_size,
	const byte*	dst_frame)
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************
Verify that post encryption checksum match calculated checksum.
This function should be called only if tablespace contains crypt_data
metadata (this is strong indication that tablespace is encrypted).
Function also verifies that traditional checksum does not match
calculated checksum as if it does page could be valid unencrypted,
encrypted, or corrupted.

@param[in]	page		Page to verify
@param[in]	zip_size	zip size
@return whether the encrypted page is OK */
UNIV_INTERN
bool fil_space_verify_crypt_checksum(const byte* page, ulint zip_size)
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************
Adjust thread count for key rotation
@param[in]	enw_cnt		Number of threads to be used */
UNIV_INTERN
void
fil_crypt_set_thread_cnt(
	uint	new_cnt);

/*********************************************************************
Adjust max key age
@param[in]	val		New max key age */
UNIV_INTERN
void
fil_crypt_set_rotate_key_age(
	uint	val);

/*********************************************************************
Adjust rotation iops
@param[in]	val		New max roation iops */
UNIV_INTERN
void
fil_crypt_set_rotation_iops(
	uint val);

/*********************************************************************
Adjust encrypt tables
@param[in]	val		New setting for innodb-encrypt-tables */
UNIV_INTERN
void
fil_crypt_set_encrypt_tables(
	uint val);

/*********************************************************************
Init threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_init();

/*********************************************************************
Clean up key rotation threads resources */
UNIV_INTERN
void
fil_crypt_threads_cleanup();

/*********************************************************************
Wait for crypt threads to stop accessing space
@param[in]	space		Tablespace */
UNIV_INTERN
void
fil_space_crypt_close_tablespace(
	const fil_space_t*	space);

/*********************************************************************
Get crypt status for a space (used by information_schema)
@param[in]	space		Tablespace
@param[out]	status		Crypt status
return 0 if crypt data present */
UNIV_INTERN
void
fil_space_crypt_get_status(
	const fil_space_t*			space,
	struct fil_space_crypt_status_t*	status);

/*********************************************************************
Return crypt statistics
@param[out]	stat		Crypt statistics */
UNIV_INTERN
void
fil_crypt_total_stat(
	fil_crypt_stat_t *stat);

/*********************************************************************
Get scrub status for a space (used by information_schema)

@param[in]	space		Tablespace
@param[out]	status		Scrub status
return 0 if data found */
UNIV_INTERN
void
fil_space_get_scrub_status(
	const fil_space_t*			space,
	struct fil_space_scrub_status_t*	status);

#ifndef UNIV_NONINL
#include "fil0crypt.ic"
#endif

#endif /* fil0crypt_h */
