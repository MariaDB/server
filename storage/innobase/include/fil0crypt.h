/*****************************************************************************

Copyright (c) 2015, MariaDB Corporation.

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

/* This key will be used if nothing else is given */
#define FIL_DEFAULT_ENCRYPTION_KEY 1

/** Enum values for encryption table option */
typedef enum {
	FIL_SPACE_ENCRYPTION_DEFAULT = 0,	/* Tablespace encrypted if
						srv_encrypt_tables = ON */
	FIL_SPACE_ENCRYPTION_ON = 1,		/* Tablespace is encrypted always */
	FIL_SPACE_ENCRYPTION_OFF = 2		/* Tablespace is not encrypted */
} fil_encryption_t;

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

struct fil_space_rotate_state_t
{
	time_t start_time;    // time when rotation started
	ulint active_threads; // active threads in space
	ulint next_offset;    // next "free" offset
	ulint max_offset;     // max offset needing to be rotated
	uint  min_key_version_found; // min key version found but not rotated
	lsn_t end_lsn;		     // max lsn created when rotating this space
	bool starting;		     // initial write of IV
	bool flushing;		     // space is being flushed at end of rotate
	struct {
		bool is_active; // is scrubbing active in this space
		time_t last_scrub_completed; // when was last scrub completed
	} scrubbing;
};

struct fil_space_crypt_struct
{
	ulint type;	    // CRYPT_SCHEME
	uint keyserver_requests; // no of key requests to key server
	uint key_count;	    // No of initalized key-structs
	uint key_id;	    // Key id for this space
	key_struct keys[3]; // cached L = AES_ECB(KEY, IV)
	uint min_key_version; // min key version for this space
	ulint page0_offset;   // byte offset on page 0 for crypt data
	fil_encryption_t encryption; // Encryption setup

	ib_mutex_t mutex;   // mutex protecting following variables
	bool closing;	    // is tablespace being closed
	fil_space_rotate_state_t rotate_state;

	uint iv_length;	    // length of IV
	byte iv[1];	    // IV-data
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
fil_space_create_crypt_data(uint key_id);

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
/*======================*/
	ulint space); /*!< in: tablespace id */

/*********************************************************************
Set crypt data for a space*/
UNIV_INTERN
void
fil_space_set_crypt_data(
/*======================*/
	ulint space,                    /*!< in: tablespace id */
	fil_space_crypt_t* crypt_data); /*!< in: crypt data to set */

/*********************************************************************
Compare crypt data*/
UNIV_INTERN
int
fil_space_crypt_compare(
/*======================*/
	const fil_space_crypt_t* crypt_data1,  /*!< in: crypt data */
	const fil_space_crypt_t* crypt_data2); /*!< in: crypt data */

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
/*======================*/
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
/*==============================*/
	ulint space);          /*!< in: tablespace id */

/*********************************************************************
Check if page shall be encrypted before write */
UNIV_INTERN
bool
fil_space_check_encryption_write(
/*==============================*/
	ulint space);          /*!< in: tablespace id */

/*********************************************************************
Encrypt buffer page */
UNIV_INTERN
void
fil_space_encrypt(
/*===============*/
	ulint space,          /*!< in: tablespace id */
	ulint offset,         /*!< in: page no */
	lsn_t lsn,            /*!< in: page lsn */
	const byte* src_frame,/*!< in: page frame */
	ulint size,           /*!< in: size of data to encrypt */
	byte* dst_frame);      /*!< in: where to encrypt to */

/*********************************************************************
Decrypt buffer page */
UNIV_INTERN
void
fil_space_decrypt(
/*===============*/
	ulint space,          /*!< in: tablespace id */
	const byte* src_frame,/*!< in: page frame */
	ulint page_size,      /*!< in: size of data to encrypt */
	byte* dst_frame);     /*!< in: where to decrypt to */


/*********************************************************************
Decrypt buffer page
@return true if page was encrypted */
UNIV_INTERN
bool
fil_space_decrypt(
/*===============*/
	fil_space_crypt_t* crypt_data, /*!< in: crypt data */
	const byte* src_frame,/*!< in: page frame */
	ulint page_size,      /*!< in: page size */
	byte* dst_frame);     /*!< in: where to decrypt to */

/*********************************************************************
fil_space_verify_crypt_checksum
NOTE: currently this function can only be run in single threaded mode
as it modifies srv_checksum_algorithm (temporarily)
@return true if page is encrypted AND OK, false otherwise */
UNIV_INTERN
bool
fil_space_verify_crypt_checksum(
/*===============*/
	const byte* src_frame,/*!< in: page frame */
	ulint zip_size);      /*!< in: size of data to encrypt */

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
End threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_end();

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
/*=====================*/
	uint rotate_age); /*!< in: requested rotate age */

/*********************************************************************
Set rotation threads iops */
UNIV_INTERN
void
fil_crypt_set_rotation_iops(
/*=====================*/
	uint iops); /*!< in: requested iops */

/*********************************************************************
Mark a space as closing */
UNIV_INTERN
void
fil_space_crypt_mark_space_closing(
/*===============*/
	ulint space);          /*!< in: tablespace id */

/*********************************************************************
Wait for crypt threads to stop accessing space */
UNIV_INTERN
void
fil_space_crypt_close_tablespace(
/*===============*/
	ulint space);          /*!< in: tablespace id */

/** Struct for retreiving info about encryption */
struct fil_space_crypt_status_t {
	ulint space;             /*!< tablespace id */
	ulint scheme;            /*!< encryption scheme */
	uint  min_key_version;   /*!< min key version */
	uint  current_key_version;/*!< current key version */
	uint  keyserver_requests;/*!< no of key requests to key server */
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
/*==================*/
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
/*==================*/
	ulint id,	                           /*!< in: space id */
	struct fil_space_scrub_status_t * status); /*!< out: status  */

#ifndef UNIV_NONINL
#include "fil0crypt.ic"
#endif

#endif /* fil0crypt_h */
