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
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

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

/** Enum values for encryption table option */
typedef enum {
	FIL_SPACE_ENCRYPTION_DEFAULT = 0,	/* Tablespace encrypted if
						srv_encrypt_tables = ON */
	FIL_SPACE_ENCRYPTION_ON = 1,		/* Tablespace is encrypted always */
	FIL_SPACE_ENCRYPTION_OFF = 2		/* Tablespace is not encrypted */
} fil_encryption_t;

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

struct fil_space_crypt_struct : st_encryption_scheme
{
 public:
	/** Constructor. Does not initialize the members!
	The object is expected to be placed in a buffer that
	has been zero-initialized. */
	fil_space_crypt_struct(
		ulint new_type,
		uint new_min_key_version,
		uint new_key_id,
		ulint offset,
		fil_encryption_t new_encryption)
		: st_encryption_scheme(),
		min_key_version(new_min_key_version),
		page0_offset(offset),
		encryption(new_encryption),
		closing(false),
		key_found(),
		rotate_state()
	{
		key_found = new_min_key_version;
		key_id = new_key_id;
		my_random_bytes(iv, sizeof(iv));
		mutex_create(fil_crypt_data_mutex_key,
			&mutex, SYNC_NO_ORDER_CHECK);
		locker = crypt_data_scheme_locker;
		type = new_type;

		if (new_encryption == FIL_SPACE_ENCRYPTION_OFF ||
			(!srv_encrypt_tables &&
			 new_encryption == FIL_SPACE_ENCRYPTION_DEFAULT)) {
			type = CRYPT_SCHEME_UNENCRYPTED;
		} else {
			type = CRYPT_SCHEME_1;
			min_key_version = key_get_latest_version();
		}
	}

	/** Destructor */
	~fil_space_crypt_struct()
	{
		closing = true;
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
		return ((encryption == FIL_SPACE_ENCRYPTION_ON) ||
			(srv_encrypt_tables &&
				encryption == FIL_SPACE_ENCRYPTION_DEFAULT));
	}

	/** Return true if tablespace is encrypted. */
	bool is_encrypted() const {
		return (encryption != FIL_SPACE_ENCRYPTION_OFF);
	}

	/** Return true if default tablespace encryption is used, */
	bool is_default_encryption() const {
		return (encryption == FIL_SPACE_ENCRYPTION_DEFAULT);
	}

	/** Return true if tablespace is not encrypted. */
	bool not_encrypted() const {
		return (encryption == FIL_SPACE_ENCRYPTION_OFF);
	}

	/** Is this tablespace closing. */
	bool is_closing(bool is_fixed) {
		bool closed;
		if (!is_fixed) {
			mutex_enter(&mutex);
		}
		closed = closing;
		if (!is_fixed) {
			mutex_exit(&mutex);
		}
		return closed;
	}

	uint min_key_version; // min key version for this space
	ulint page0_offset;   // byte offset on page 0 for crypt data
	fil_encryption_t encryption; // Encryption setup

	ib_mutex_t mutex;   // mutex protecting following variables
	bool closing;	    // is tablespace being closed

	/** Return code from encryption_key_get_latest_version.
        If ENCRYPTION_KEY_VERSION_INVALID encryption plugin
	could not find the key and there is no need to call
	get_latest_key_version again as keys are read only
	at startup. */
	uint key_found;

	fil_space_rotate_state_t rotate_state;
};

/* structure containing encryption specification */
typedef struct fil_space_crypt_struct fil_space_crypt_t;

/*********************************************************************
Init global resources needed for tablespace encryption/decryption */
UNIV_INTERN
void
fil_space_crypt_init();

/*********************************************************************
Cleanup global resources needed for tablespace encryption/decryption */
UNIV_INTERN
void
fil_space_crypt_cleanup();

/*********************************************************************
Create crypt data, i.e data that is used for a single tablespace */
UNIV_INTERN
fil_space_crypt_t *
fil_space_create_crypt_data(
/*========================*/
	fil_encryption_t	encrypt_mode,	/*!< in: encryption mode */
	uint			key_id);	/*!< in: encryption key id */

/*********************************************************************
Destroy crypt data */
UNIV_INTERN
void
fil_space_destroy_crypt_data(
/*=========================*/
	fil_space_crypt_t **crypt_data); /*!< in/out: crypt data */

/*********************************************************************
Get crypt data for a space*/
UNIV_INTERN
fil_space_crypt_t *
fil_space_get_crypt_data(
/*=====================*/
	ulint space); /*!< in: tablespace id */

/*********************************************************************
Set crypt data for a space*/
UNIV_INTERN
fil_space_crypt_t*
fil_space_set_crypt_data(
/*=====================*/
	ulint space,                    /*!< in: tablespace id */
	fil_space_crypt_t* crypt_data); /*!< in: crypt data to set */

/*********************************************************************
Merge crypt data */
UNIV_INTERN
void
fil_space_merge_crypt_data(
/*=======================*/
	fil_space_crypt_t* dst_crypt_data,  /*!< in: crypt_data */
	const fil_space_crypt_t* src_crypt_data); /*!< in: crypt data */

/*********************************************************************
Read crypt data from buffer page */
UNIV_INTERN
fil_space_crypt_t *
fil_space_read_crypt_data(
/*======================*/
	ulint space,      /*!< in: tablespace id */
	const byte* page, /*!< in: buffer page */
	ulint offset);    /*!< in: offset where crypt data is stored */

/*********************************************************************
Write crypt data to buffer page */
UNIV_INTERN
void
fil_space_write_crypt_data(
/*=======================*/
	ulint space,   /*!< in: tablespace id */
	byte* page,    /*!< in: buffer page */
	ulint offset,  /*!< in: offset where to store data */
	ulint maxsize, /*!< in: max space available to store crypt data in */
	mtr_t * mtr);  /*!< in: mini-transaction */

/*********************************************************************
Clear crypt data from page 0 (used for import tablespace) */
UNIV_INTERN
void
fil_space_clear_crypt_data(
/*=======================*/
	byte* page,    /*!< in: buffer page */
	ulint offset); /*!< in: offset where crypt data is stored */

/*********************************************************************
Parse crypt data log record */
UNIV_INTERN
byte*
fil_parse_write_crypt_data(
/*=======================*/
	byte* ptr,     /*!< in: start of log record */
	byte* end_ptr, /*!< in: end of log record */
	buf_block_t*); /*!< in: buffer page to apply record to */

/*********************************************************************
Check if extra buffer shall be allocated for decrypting after read */
UNIV_INTERN
bool
fil_space_check_encryption_read(
/*============================*/
	ulint space);          /*!< in: tablespace id */

/******************************************************************
Decrypt a page
@return true if page decrypted, false if not.*/
UNIV_INTERN
bool
fil_space_decrypt(
/*==============*/
	fil_space_crypt_t*	crypt_data,	/*!< in: crypt data */
	byte*			tmp_frame,	/*!< in: temporary buffer */
	ulint			page_size,	/*!< in: page size */
	byte*			src_frame,	/*!< in: out: page buffer */
	dberr_t*		err)		/*!< in: out: DB_SUCCESS or
						error code */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************
Encrypt buffer page
@return encrypted page, or original not encrypted page if encrypt
is not needed. */
UNIV_INTERN
byte*
fil_space_encrypt(
/*==============*/
	ulint	space,		/*!< in: tablespace id */
	ulint	offset,		/*!< in: page no */
	lsn_t	lsn,		/*!< in: page lsn */
	byte*	src_frame,	/*!< in: page frame */
	ulint	size,		/*!< in: size of data to encrypt */
	byte*	dst_frame);	/*!< in: where to encrypt to */

/******************************************************************
Decrypt a page
@param[in]	space			Tablespace id
@param[in]	tmp_frame		Temporary buffer used for decrypting
@param[in]	page_size		Page size
@param[in,out]	src_frame		Page to decrypt
@param[out]	decrypted		true if page was decrypted
@return decrypted page, or original not encrypted page if decryption is
not needed.*/
UNIV_INTERN
byte*
fil_space_decrypt(
/*==============*/
	ulint	space,
	byte*	src_frame,
	ulint	page_size,
	byte*	dst_frame,
	bool*	decrypted)
	__attribute__((warn_unused_result));

/*********************************************************************
Verify that post encryption checksum match calculated checksum.
This function should be called only if tablespace contains crypt_data
metadata (this is strong indication that tablespace is encrypted).
Function also verifies that traditional checksum does not match
calculated checksum as if it does page could be valid unencrypted,
encrypted, or corrupted.
@param[in]	page		Page to verify
@param[in]	zip_size	zip size
@param[in]	space		Tablespace
@param[in]	pageno		Page no
@return true if page is encrypted AND OK, false otherwise */
UNIV_INTERN
bool
fil_space_verify_crypt_checksum(
	byte*			page,
	ulint			zip_size,
	const fil_space_t*	space,
	ulint			pageno)
	__attribute__((warn_unused_result));

/*********************************************************************
Init threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_init();

/*********************************************************************
Set thread count (e.g start or stops threads) used for key rotation */
UNIV_INTERN
void
fil_crypt_set_thread_cnt(
/*=====================*/
	uint new_cnt); /*!< in: requested #threads */

/*********************************************************************
Cleanup resources for threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_cleanup();

/*********************************************************************
Set rotate key age */
UNIV_INTERN
void
fil_crypt_set_rotate_key_age(
/*=========================*/
	uint rotate_age); /*!< in: requested rotate age */

/*********************************************************************
Set rotation threads iops */
UNIV_INTERN
void
fil_crypt_set_rotation_iops(
/*========================*/
	uint iops); /*!< in: requested iops */

/*********************************************************************
Mark a space as closing */
UNIV_INTERN
void
fil_space_crypt_mark_space_closing(
/*===============================*/
	ulint			space,		/*!< in: tablespace id */
	fil_space_crypt_t*	crypt_data);	/*!< in: crypt_data or NULL */

/*********************************************************************
Wait for crypt threads to stop accessing space */
UNIV_INTERN
void
fil_space_crypt_close_tablespace(
/*=============================*/
	ulint space);          /*!< in: tablespace id */

/** Struct for retreiving info about encryption */
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

/*********************************************************************
Get crypt status for a space
@return 0 if crypt data found */
UNIV_INTERN
int
fil_space_crypt_get_status(
/*=======================*/
	ulint id,	                           /*!< in: space id */
	struct fil_space_crypt_status_t * status); /*!< out: status  */

/** Struct for retreiving statistics about encryption key rotation */
struct fil_crypt_stat_t {
	ulint pages_read_from_cache;
	ulint pages_read_from_disk;
	ulint pages_modified;
	ulint pages_flushed;
	ulint estimated_iops;
};

/*********************************************************************
Get crypt rotation statistics */
UNIV_INTERN
void
fil_crypt_total_stat(
/*==================*/
	fil_crypt_stat_t* stat); /*!< out: crypt stat */

/** Struct for retreiving info about scrubbing */
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
Get scrub status for a space
@return 0 if no scrub info found */
UNIV_INTERN
int
fil_space_get_scrub_status(
/*=======================*/
	ulint id,	                           /*!< in: space id */
	struct fil_space_scrub_status_t * status); /*!< out: status  */

/*********************************************************************
Adjust encrypt tables */
UNIV_INTERN
void
fil_crypt_set_encrypt_tables(
/*=========================*/
	uint val);      /*!< in: New srv_encrypt_tables setting */

/******************************************************************
Encrypt a buffer */
UNIV_INTERN
byte*
fil_encrypt_buf(
/*============*/
	fil_space_crypt_t* crypt_data,	/*!< in: crypt data */
	ulint		space,		/*!< in: Space id */
	ulint		offset,		/*!< in: Page offset */
	lsn_t		lsn,		/*!< in: lsn */
	byte*		src_frame,	/*!< in: Source page to be encrypted */
	ulint		zip_size,	/*!< in: compressed size if
					row_format compressed */
	byte*		dst_frame);	/*!< in: outbut buffer */

/******************************************************************
Calculate post encryption checksum
@return page checksum or BUF_NO_CHECKSUM_MAGIC
not needed. */
UNIV_INTERN
ulint
fil_crypt_calculate_checksum(
/*=========================*/
	ulint	zip_size,	/*!< in: zip_size or 0 */
	byte*	dst_frame);	/*!< in: page where to calculate */

#ifndef UNIV_NONINL
#include "fil0crypt.ic"
#endif

#endif /* fil0crypt_h */
