/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 1995, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */


/* This file includes constants used with all databases */

#ifndef _my_base_h
#define _my_base_h

#include <my_dir.h>			/* This includes types */
#include <my_sys.h>
#include <m_string.h>
#include <errno.h>

#ifndef EOVERFLOW
#define EOVERFLOW 84
#endif

#include <my_list.h>

/* The following is bits in the flag parameter to ha_open() */

#define HA_OPEN_ABORT_IF_LOCKED		0U	/* default */
#define HA_OPEN_WAIT_IF_LOCKED		1U
#define HA_OPEN_IGNORE_IF_LOCKED	2U      /* Ignore lock error */
#define HA_OPEN_TMP_TABLE		4U	/* Table is a temp table */
#define HA_OPEN_DELAY_KEY_WRITE		8U	/* Don't update index  */
#define HA_OPEN_ABORT_IF_CRASHED	16U
#define HA_OPEN_FOR_REPAIR		32U	/* open even if crashed */
#define HA_OPEN_FROM_SQL_LAYER          64U
#define HA_OPEN_MMAP                    128U    /* open memory mapped */
#define HA_OPEN_COPY			256U    /* Open copy (for repair) */
/* Internal temp table, used for temporary results */
#define HA_OPEN_INTERNAL_TABLE          512U
#define HA_OPEN_NO_PSI_CALL             1024U   /* Don't call/connect PSI */
#define HA_OPEN_MERGE_TABLE		2048U
#define HA_OPEN_FOR_CREATE              4096U
#define HA_OPEN_FOR_DROP                (1U << 13) /* Open part of drop */
#define HA_OPEN_GLOBAL_TMP_TABLE	(1U << 14) /* TMP table used by repliction */

/*
  Allow opening even if table is incompatible as this is for ALTER TABLE which
  will fix the table structure.
*/
#define HA_OPEN_FOR_ALTER		8192U

/* Open table for FLUSH */
#define HA_OPEN_FOR_FLUSH               8192U


/* The following is parameter to ha_rkey() how to use key */

/*
  We define a complete-field prefix of a key value as a prefix where
  the last included field in the prefix contains the full field, not
  just some bytes from the start of the field. A partial-field prefix
  is allowed to contain only a few first bytes from the last included
  field.

  Below HA_READ_KEY_EXACT, ..., HA_READ_BEFORE_KEY can take a
  complete-field prefix of a key value as the search
  key. HA_READ_PREFIX and HA_READ_PREFIX_LAST could also take a
  partial-field prefix, but currently (4.0.10) they are only used with
  complete-field prefixes. MySQL uses a padding trick to implement
  LIKE 'abc%' queries.

  NOTE that in InnoDB HA_READ_PREFIX_LAST will NOT work with a
  partial-field prefix because InnoDB currently strips spaces from the
  end of varchar fields!
*/

enum ha_rkey_function {
  HA_READ_KEY_EXACT,              /* Find first record else error */
  HA_READ_KEY_OR_NEXT,            /* Record or next record */
  HA_READ_KEY_OR_PREV,            /* Record or previous */
  HA_READ_AFTER_KEY,              /* Find next rec. after key-record */
  HA_READ_BEFORE_KEY,             /* Find next rec. before key-record */
  HA_READ_PREFIX,                 /* Key which as same prefix */
  HA_READ_PREFIX_LAST,            /* Last key with the same prefix */
  HA_READ_PREFIX_LAST_OR_PREV,    /* Last or prev key with the same prefix */
  HA_READ_MBR_CONTAIN,
  HA_READ_MBR_INTERSECT,
  HA_READ_MBR_WITHIN,
  HA_READ_MBR_DISJOINT,
  HA_READ_MBR_EQUAL
};

	/* Key algorithm types */

enum ha_key_alg {
  HA_KEY_ALG_UNDEF=	0,		/* Not specified (old file) */
  HA_KEY_ALG_BTREE=	1,		/* B-tree, default one          */
  HA_KEY_ALG_RTREE=	2,		/* R-tree, for spatial searches */
  HA_KEY_ALG_HASH=	3,		/* HASH keys (HEAP tables) */
  HA_KEY_ALG_FULLTEXT=	4,		/* FULLTEXT (MyISAM tables) */
  HA_KEY_ALG_LONG_HASH= 5,		/* long BLOB keys */
  HA_KEY_ALG_UNIQUE_HASH= 6		/* Internal UNIQUE hash (Aria) */
};

        /* Storage media types */ 

enum ha_storage_media {
  HA_SM_DEFAULT=        0,		/* Not specified (engine default) */
  HA_SM_DISK=           1,		/* DISK storage */
  HA_SM_MEMORY=         2		/* MAIN MEMORY storage */
};

	/* The following is parameter to ha_extra() */

enum ha_extra_function {
  HA_EXTRA_NORMAL=0,			/* Optimize for space (def) */
  HA_EXTRA_QUICK=1,			/* Optimize for speed */
  HA_EXTRA_NOT_USED=2,			/* Should be ignored by handler */
  HA_EXTRA_CACHE=3,			/* Cache record in HA_rrnd() */
  HA_EXTRA_NO_CACHE=4,			/* End caching of records (def) */
  HA_EXTRA_NO_READCHECK=5,		/* No readcheck on update */
  HA_EXTRA_READCHECK=6,			/* Use readcheck (def) */
  HA_EXTRA_KEYREAD=7,			/* Read only key to database */
  HA_EXTRA_NO_KEYREAD=8,		/* Normal read of records (def) */
  HA_EXTRA_NO_USER_CHANGE=9,		/* No user is allowed to write */
  HA_EXTRA_KEY_CACHE=10,
  HA_EXTRA_NO_KEY_CACHE=11,
  HA_EXTRA_WAIT_LOCK=12,		/* Wait until file is available (def) */
  HA_EXTRA_NO_WAIT_LOCK=13,		/* If file is locked, return quickly */
  HA_EXTRA_WRITE_CACHE=14,		/* Use write cache in ha_write() */
  HA_EXTRA_FLUSH_CACHE=15,		/* flush write_record_cache */
  HA_EXTRA_NO_KEYS=16,			/* Remove all update of keys */
  HA_EXTRA_KEYREAD_CHANGE_POS=17,	/* Keyread, but change pos */
					/* xxxxchk -r must be used */
  HA_EXTRA_REMEMBER_POS=18,		/* Remember pos for next/prev */
  HA_EXTRA_RESTORE_POS=19,
  HA_EXTRA_REINIT_CACHE=20,		/* init cache from current record */
  HA_EXTRA_FORCE_REOPEN=21,		/* Datafile have changed on disk */
  HA_EXTRA_FLUSH,			/* Flush tables to disk */
  HA_EXTRA_NO_ROWS,			/* Don't write rows */
  HA_EXTRA_RESET_STATE,			/* Reset positions */
  HA_EXTRA_IGNORE_DUP_KEY,		/* Dup keys don't rollback everything*/
  HA_EXTRA_NO_IGNORE_DUP_KEY,
  HA_EXTRA_PREPARE_FOR_DROP,
  HA_EXTRA_PREPARE_FOR_UPDATE,		/* Remove read cache if problems */
  HA_EXTRA_PRELOAD_BUFFER_SIZE,         /* Set buffer size for preloading */
  /*
    On-the-fly switching between unique and non-unique key inserting.
  */
  HA_EXTRA_CHANGE_KEY_TO_UNIQUE,
  HA_EXTRA_CHANGE_KEY_TO_DUP,
  /*
    When using HA_EXTRA_KEYREAD, overwrite only key member fields and keep 
    other fields intact. When this is off (by default) InnoDB will use memcpy
    to overwrite entire row.
  */
  HA_EXTRA_KEYREAD_PRESERVE_FIELDS,
  HA_EXTRA_MMAP,
  /*
    Ignore if the a tuple is not found, continue processing the
    transaction and ignore that 'row'.  Needed for idempotency
    handling on the slave
  */
  HA_EXTRA_IGNORE_NO_KEY,
  HA_EXTRA_NO_IGNORE_NO_KEY,
  /*
    Mark the table as a log table. For some handlers (e.g. CSV) this results
    in a special locking for the table.
  */
  HA_EXTRA_MARK_AS_LOG_TABLE,
  /*
    Informs handler that write_row() which tries to insert new row into the
    table and encounters some already existing row with same primary/unique
    key can replace old row with new row instead of reporting error (basically
    it informs handler that we do REPLACE instead of simple INSERT).
    Off by default.
  */
  HA_EXTRA_WRITE_CAN_REPLACE,
  HA_EXTRA_WRITE_CANNOT_REPLACE,
  /*
    Inform handler that delete_row()/update_row() cannot batch deletes/updates
    and should perform them immediately. This may be needed when table has 
    AFTER DELETE/UPDATE triggers which access to subject table.
    These flags are reset by the handler::extra(HA_EXTRA_RESET) call.
  */
  HA_EXTRA_DELETE_CANNOT_BATCH,
  HA_EXTRA_UPDATE_CANNOT_BATCH,
  /*
    Inform handler that an "INSERT...ON DUPLICATE KEY UPDATE" will be
    executed. This condition is unset by HA_EXTRA_NO_IGNORE_DUP_KEY.
  */
  HA_EXTRA_INSERT_WITH_UPDATE,
  /* Inform handler that we will do a rename */
  HA_EXTRA_PREPARE_FOR_RENAME,
  /*
    Special actions for MERGE tables.
  */
  HA_EXTRA_ADD_CHILDREN_LIST,
  HA_EXTRA_ATTACH_CHILDREN,
  HA_EXTRA_IS_ATTACHED_CHILDREN,
  HA_EXTRA_DETACH_CHILDREN,
  HA_EXTRA_DETACH_CHILD,
  /* Inform handler we will force a close as part of flush */
  HA_EXTRA_PREPARE_FOR_FORCED_CLOSE,
  /* Inform handler that we will do an alter table */
  HA_EXTRA_PREPARE_FOR_ALTER_TABLE,
  /*
    Used in ha_partition::handle_ordered_index_scan() to inform engine
    that we are starting an ordered index scan. Needed by Spider
  */
  HA_EXTRA_STARTING_ORDERED_INDEX_SCAN,
  /** Start writing rows during ALTER TABLE...ALGORITHM=COPY. */
  HA_EXTRA_BEGIN_ALTER_COPY,
  /** Finish writing rows during ALTER TABLE...ALGORITHM=COPY. */
  HA_EXTRA_END_ALTER_COPY
};

/* Compatible option, to be deleted in 6.0 */
#define HA_EXTRA_PREPARE_FOR_DELETE HA_EXTRA_PREPARE_FOR_DROP

	/* The following is parameter to ha_panic() */

enum ha_panic_function {
  HA_PANIC_CLOSE,			/* Close all databases */
  HA_PANIC_WRITE,			/* Unlock and write status */
  HA_PANIC_READ				/* Lock and read keyinfo */
};

	/* The following is parameter to ha_create(); keytypes */

enum ha_base_keytype {
  HA_KEYTYPE_END=0,
  HA_KEYTYPE_TEXT=1,			/* Key is sorted as letters */
  HA_KEYTYPE_BINARY=2,			/* Key is sorted as unsigned chars */
  HA_KEYTYPE_SHORT_INT=3,
  HA_KEYTYPE_LONG_INT=4,
  HA_KEYTYPE_FLOAT=5,
  HA_KEYTYPE_DOUBLE=6,
  HA_KEYTYPE_NUM=7,			/* Not packed num with pre-space */
  HA_KEYTYPE_USHORT_INT=8,
  HA_KEYTYPE_ULONG_INT=9,
  HA_KEYTYPE_LONGLONG=10,
  HA_KEYTYPE_ULONGLONG=11,
  HA_KEYTYPE_INT24=12,
  HA_KEYTYPE_UINT24=13,
  HA_KEYTYPE_INT8=14,
  /* Varchar (0-255 bytes) with length packed with 1 byte */
  HA_KEYTYPE_VARTEXT1=15,               /* Key is sorted as letters */
  HA_KEYTYPE_VARBINARY1=16,             /* Key is sorted as unsigned chars */
  /* Varchar (0-65535 bytes) with length packed with 2 bytes */
  HA_KEYTYPE_VARTEXT2=17,		/* Key is sorted as letters */
  HA_KEYTYPE_VARBINARY2=18,		/* Key is sorted as unsigned chars */
  HA_KEYTYPE_BIT=19
};

#define HA_MAX_KEYTYPE	31		/* Must be log2-1 */

/*
  These flags kan be OR:ed to key-flag
  Note that these can only be up to 16 bits!
*/

#define HA_NOSAME		 1U	/* Set if not dupplicated records */
#define HA_PACK_KEY		 2U	/* Pack string key to previous key */
#define HA_AUTO_KEY		 16U    /* MEMORY/MyISAM/Aria internal */
#define HA_BINARY_PACK_KEY	 32U	/* Packing of all keys to prev key */
#define HA_FULLTEXT		128U    /* For full-text search */
#define HA_SPATIAL		1024U   /* For spatial search */
#define HA_NULL_ARE_EQUAL	2048U	/* NULL in key are cmp as equal */
#define HA_GENERATED_KEY	8192U	/* Automatically generated key */
/* 
  Part of unique hash key. Used only for temporary (work) tables so is not
  written to .frm files.
*/
#define HA_UNIQUE_HASH          262144U

        /* The combination of the above can be used for key type comparison. */
#define HA_KEYFLAG_MASK (HA_NOSAME | HA_AUTO_KEY | \
                         HA_FULLTEXT | \
                         HA_SPATIAL | HA_NULL_ARE_EQUAL | HA_GENERATED_KEY | \
                         HA_UNIQUE_HASH)

/*
  Key contains partial segments.

  This flag is internal to the MySQL server by design. It is not supposed
  neither to be saved in FRM-files, nor to be passed to storage engines.
  It is intended to pass information into internal static sort_keys(KEY *,
  KEY *) function.

  This flag can be calculated -- it's based on key lengths comparison.
*/
#define HA_KEY_HAS_PART_KEY_SEG 65536
/* Internal Flag Can be calculated */
#define HA_INVISIBLE_KEY 2<<18
	/* Automatic bits in key-flag */

#define HA_SPACE_PACK_USED	 4	/* Test for if SPACE_PACK used */
#define HA_VAR_LENGTH_KEY	 8
#define HA_NULL_PART_KEY	 64
#define HA_USES_COMMENT          4096
#define HA_USES_PARSER           16384  /* Fulltext index uses [pre]parser */
#define HA_USES_BLOCK_SIZE	 ((uint) 32768)
#define HA_SORT_ALLOWS_SAME      512    /* Intern bit when sorting records */

/* This flag can be used only in KEY::ext_key_flags */
#define HA_EXT_NOSAME            131072

	/* These flags can be added to key-seg-flag */

#define HA_SPACE_PACK		 1	/* Pack space in key-seg */
#define HA_PART_KEY_SEG		 4	/* Used by MySQL for part-key-cols */
#define HA_VAR_LENGTH_PART	 8
#define HA_NULL_PART		 16
#define HA_BLOB_PART		 32
#define HA_SWAP_KEY		 64
#define HA_REVERSE_SORT		 128	/* Sort key in reverse order */
#define HA_NO_SORT               256 /* do not bother sorting on this keyseg */

#define HA_BIT_PART		1024
#define HA_CAN_MEMCMP           2048 /* internal, never stored in frm */

	/* optionbits for database */
#define HA_OPTION_PACK_RECORD		1U
#define HA_OPTION_PACK_KEYS		2U
#define HA_OPTION_COMPRESS_RECORD	4U
#define HA_OPTION_LONG_BLOB_PTR		8U /* new ISAM format */
#define HA_OPTION_TMP_TABLE		16U
#define HA_OPTION_CHECKSUM		32U
#define HA_OPTION_DELAY_KEY_WRITE	64U
#define HA_OPTION_NO_PACK_KEYS		128U  /* Reserved for MySQL */
/* unused                               256 */
#define HA_OPTION_RELIES_ON_SQL_LAYER   512U
#define HA_OPTION_NULL_FIELDS		1024U
#define HA_OPTION_PAGE_CHECKSUM		2048U
/*
   STATS_PERSISTENT=1 has been specified in the SQL command (either CREATE
   or ALTER TABLE). Table and index statistics that are collected by the
   storage engine and used by the optimizer for query optimization will be
   stored on disk and will not change after a server restart.
*/
#define HA_OPTION_STATS_PERSISTENT	4096U
/*
  STATS_PERSISTENT=0 has been specified in CREATE/ALTER TABLE. Statistics
  for the table will be wiped away on server shutdown and new ones recalculated
  after the server is started again. If none of HA_OPTION_STATS_PERSISTENT or
  HA_OPTION_NO_STATS_PERSISTENT is set, this means that the setting is not
  explicitly set at table level and the corresponding table will use whatever
  is the global server default.
*/
#define HA_OPTION_NO_STATS_PERSISTENT	8192U

/* .frm has extra create options in linked-list format */
#define HA_OPTION_TEXT_CREATE_OPTIONS_legacy (1U << 14) /* 5.2 to 5.5, unused since 10.0 */
#define HA_OPTION_TEMP_COMPRESS_RECORD  (1U << 15)      /* set by isamchk */
#define HA_OPTION_READ_ONLY_DATA        (1U << 16)      /* Set by isamchk */
#define HA_OPTION_NO_CHECKSUM           (1U << 17)
#define HA_OPTION_NO_DELAY_KEY_WRITE    (1U << 18)

	/* Bits in flag to create() */

#define HA_DONT_TOUCH_DATA	1U	/* Don't empty datafile (isamchk) */
#define HA_PACK_RECORD		2U	/* Request packed record format */
#define HA_CREATE_TMP_TABLE	4U
#define HA_CREATE_CHECKSUM	8U
#define HA_CREATE_KEEP_FILES	16U     /* don't overwrite .MYD and MYI */
#define HA_CREATE_PAGE_CHECKSUM	32U
#define HA_CREATE_DELAY_KEY_WRITE 64U
#define HA_CREATE_RELIES_ON_SQL_LAYER 128U
#define HA_CREATE_INTERNAL_TABLE 256U
#define HA_PRESERVE_INSERT_ORDER 512U
#define HA_CREATE_NO_ROLLBACK    1024U
/*
  A temporary table that can be used by different threads, eg. replication
  threads. This flag ensure that memory is not allocated with THREAD_SPECIFIC,
  as we do for other temporary tables.
*/
#define HA_CREATE_GLOBAL_TMP_TABLE 2048U

/* Flags used by start_bulk_insert */

#define HA_CREATE_UNIQUE_INDEX_BY_SORT   1U


/*
  The following flags (OR-ed) are passed to handler::info() method.
  The method copies misc handler information out of the storage engine
  to data structures accessible from MySQL

  Same flags are also passed down to mi_status, myrg_status, etc.
*/

/* this one is not used */
#define HA_STATUS_POS            1U
/*
  assuming the table keeps shared actual copy of the 'info' and
  local, possibly outdated copy, the following flag means that
  it should not try to get the actual data (locking the shared structure)
  slightly outdated version will suffice
*/
#define HA_STATUS_NO_LOCK        2U
/* update the time of the last modification (in handler::update_time) */
#define HA_STATUS_TIME           4U
/*
  update the 'constant' part of the info:
  handler::max_data_file_length, max_index_file_length, create_time
  sortkey, ref_length, block_size, data_file_name, index_file_name.
  handler::table->s->keys_in_use, keys_for_keyread, rec_per_key
*/
#define HA_STATUS_CONST          8U
/*
  update the 'variable' part of the info:
  handler::records, deleted, data_file_length, index_file_length,
  check_time, mean_rec_length
*/
#define HA_STATUS_VARIABLE      16U
/*
  get the information about the key that caused last duplicate value error
  update handler::errkey and handler::dupp_ref
  see handler::get_dup_key()
*/
#define HA_STATUS_ERRKEY        32U
/*
  update handler::auto_increment_value
*/
#define HA_STATUS_AUTO          64U
/*
  Get also delete_length when HA_STATUS_VARIABLE is called. It's ok to set it also
  when only HA_STATUS_VARIABLE but it won't be used.
*/
#define HA_STATUS_VARIABLE_EXTRA 128U
/*
  Treat empty table as empty (ignore HA_STATUS_TIME hack).
*/
#define HA_STATUS_OPEN           256U

/*
  Errorcodes given by handler functions

  opt_sum_query() assumes these codes are > 1
  Do not add error numbers before HA_ERR_FIRST.
  If necessary to add lower numbers, change HA_ERR_FIRST accordingly.
*/
#define HA_ERR_FIRST            120     /* Copy of first error nr.*/

#define HA_ERR_KEY_NOT_FOUND	120	/* Didn't find key on read or update */
#define HA_ERR_FOUND_DUPP_KEY	121	/* Duplicate key on write */
#define HA_ERR_INTERNAL_ERROR   122     /* Internal error */
#define HA_ERR_RECORD_CHANGED	123	/* Update with is recoverable */
#define HA_ERR_WRONG_INDEX	124	/* Wrong index given to function */
#define HA_ERR_CRASHED		126	/* Indexfile is crashed */
#define HA_ERR_WRONG_IN_RECORD	127	/* Record-file is crashed */
#define HA_ERR_OUT_OF_MEM	128	/* Out of memory */
#define HA_ERR_RETRY_INIT 129 /* Initialization failed and should be retried */
#define HA_ERR_NOT_A_TABLE      130     /* not a MYI file - no signature */
#define HA_ERR_WRONG_COMMAND	131	/* Command not supported */
#define HA_ERR_OLD_FILE		132	/* old databasfile */
#define HA_ERR_NO_ACTIVE_RECORD 133	/* No record read in update() */
#define HA_ERR_RECORD_DELETED	134	/* A record is not there */
#define HA_ERR_RECORD_FILE_FULL 135	/* No more room in file */
#define HA_ERR_INDEX_FILE_FULL	136	/* No more room in file */
#define HA_ERR_END_OF_FILE	137	/* end in next/prev/first/last */
#define HA_ERR_UNSUPPORTED	138	/* unsupported extension used */
#define HA_ERR_TO_BIG_ROW	139	/* Too big row */
#define HA_WRONG_CREATE_OPTION	140	/* Wrong create option */
#define HA_ERR_FOUND_DUPP_UNIQUE 141	/* Duplicate unique on write */
#define HA_ERR_UNKNOWN_CHARSET	 142	/* Can't open charset */
#define HA_ERR_WRONG_MRG_TABLE_DEF 143  /* conflicting tables in MERGE */
#define HA_ERR_CRASHED_ON_REPAIR 144	/* Last (automatic?) repair failed */
#define HA_ERR_CRASHED_ON_USAGE  145	/* Table must be repaired */
#define HA_ERR_LOCK_WAIT_TIMEOUT 146
#define HA_ERR_LOCK_TABLE_FULL   147
#define HA_ERR_READ_ONLY_TRANSACTION 148 /* Updates not allowed */
#define HA_ERR_LOCK_DEADLOCK	 149
#define HA_ERR_CANNOT_ADD_FOREIGN 150    /* Cannot add a foreign key constr. */
#define HA_ERR_NO_REFERENCED_ROW 151     /* Cannot add a child row */
#define HA_ERR_ROW_IS_REFERENCED 152     /* Cannot delete a parent row */
#define HA_ERR_NO_SAVEPOINT	 153     /* No savepoint with that name */
#define HA_ERR_NON_UNIQUE_BLOCK_SIZE 154 /* Non unique key block size */
#define HA_ERR_NO_SUCH_TABLE     155  /* The table does not exist in engine */
#define HA_ERR_TABLE_EXIST       156  /* The table existed in storage engine */
#define HA_ERR_NO_CONNECTION     157  /* Could not connect to storage engine */
/* NULLs are not supported in spatial index */
#define HA_ERR_NULL_IN_SPATIAL   158
#define HA_ERR_TABLE_DEF_CHANGED 159  /* The table changed in storage engine */
/* There's no partition in table for given value */
#define HA_ERR_NO_PARTITION_FOUND 160
#define HA_ERR_RBR_LOGGING_FAILED 161  /* Row-based binlogging of row failed */
#define HA_ERR_DROP_INDEX_FK      162  /* Index needed in foreign key constr */
/*
  Upholding foreign key constraints would lead to a duplicate key error
  in some other table.
*/
#define HA_ERR_FOREIGN_DUPLICATE_KEY 163
/* The table changed in storage engine */
#define HA_ERR_TABLE_NEEDS_UPGRADE 164
#define HA_ERR_TABLE_READONLY      165   /* The table is not writable */

#define HA_ERR_AUTOINC_READ_FAILED 166   /* Failed to get next autoinc value */
#define HA_ERR_AUTOINC_ERANGE    167     /* Failed to set row autoinc value */
#define HA_ERR_GENERIC           168     /* Generic error */
/* row not actually updated: new values same as the old values */
#define HA_ERR_RECORD_IS_THE_SAME 169
#define HA_ERR_LOGGING_IMPOSSIBLE 170    /* It is not possible to log this
                                            statement */
#define HA_ERR_CORRUPT_EVENT      171	 /* The event was corrupt, leading to
                                            illegal data being read */
#define HA_ERR_NEW_FILE	          172	 /* New file format */
#define HA_ERR_ROWS_EVENT_APPLY   173    /* The event could not be processed
                                            no other handler error happened */
#define HA_ERR_INITIALIZATION     174    /* Error during initialization */
#define HA_ERR_FILE_TOO_SHORT	  175	 /* File too short */
#define HA_ERR_WRONG_CRC	  176	 /* Wrong CRC on page */
#define HA_ERR_TOO_MANY_CONCURRENT_TRXS 177 /*Too many active concurrent transactions */
/* There's no explicitly listed partition in table for the given value */
#define HA_ERR_NOT_IN_LOCK_PARTITIONS 178
#define HA_ERR_INDEX_COL_TOO_LONG 179    /* Index column length exceeds limit */
#define HA_ERR_INDEX_CORRUPT      180    /* Index corrupted */
#define HA_ERR_UNDO_REC_TOO_BIG   181    /* Undo log record too big */
#define HA_FTS_INVALID_DOCID      182	 /* Invalid InnoDB Doc ID */
/* #define HA_ERR_TABLE_IN_FK_CHECK  183 */ /* Table being used in foreign key check */
#define HA_ERR_TABLESPACE_EXISTS  184    /* The tablespace existed in storage engine */
#define HA_ERR_TOO_MANY_FIELDS    185    /* Table has too many columns */
#define HA_ERR_ROW_IN_WRONG_PARTITION 186 /* Row in wrong partition */
#define HA_ERR_ROW_NOT_VISIBLE    187
#define HA_ERR_ABORTED_BY_USER    188
#define HA_ERR_DISK_FULL          189
#define HA_ERR_INCOMPATIBLE_DEFINITION 190
#define HA_ERR_FTS_TOO_MANY_WORDS_IN_PHRASE 191 /* Too many words in a phrase */
#define HA_ERR_DECRYPTION_FAILED  192 /* Table encrypted but decrypt failed */
#define HA_ERR_FK_DEPTH_EXCEEDED  193 /* FK cascade depth exceeded */
#define HA_ERR_TABLESPACE_MISSING 194  /* Missing Tablespace */
#define HA_ERR_SEQUENCE_INVALID_DATA 195
#define HA_ERR_SEQUENCE_RUN_OUT   196
#define HA_ERR_COMMIT_ERROR       197
#define HA_ERR_PARTITION_LIST     198
#define HA_ERR_NO_ENCRYPTION      199
#define HA_ERR_LAST               199  /* Copy of last error nr * */

/* Number of different errors */
#define HA_ERR_ERRORS            (HA_ERR_LAST - HA_ERR_FIRST + 1)

/* aliases */
#define HA_ERR_TABLE_CORRUPT HA_ERR_WRONG_IN_RECORD
#define HA_ERR_QUERY_INTERRUPTED HA_ERR_ABORTED_BY_USER
#define HA_ERR_NOT_ALLOWED_COMMAND HA_ERR_WRONG_COMMAND

	/* Other constants */

#define HA_NAMELEN 64			/* Max length of saved filename */
#define NO_SUCH_KEY (~(uint)0)          /* used as a key no. */

typedef ulong key_part_map;
#define HA_WHOLE_KEY  (~(key_part_map)0)

	/* Intern constants in databases */

	/* bits in _search */
#define SEARCH_FIND	1U
#define SEARCH_NO_FIND	2U
#define SEARCH_SAME	4U
#define SEARCH_BIGGER	8U
#define SEARCH_SMALLER	16U
#define SEARCH_SAVE_BUFF	32U
#define SEARCH_UPDATE	64U
#define SEARCH_PREFIX	128U
#define SEARCH_LAST	256U
#define MBR_CONTAIN     512U
#define MBR_INTERSECT   1024U
#define MBR_WITHIN      2048U
#define MBR_DISJOINT    4096U
#define MBR_EQUAL       8192U
#define MBR_DATA        16384U
#define SEARCH_NULL_ARE_EQUAL 32768U	/* NULL in keys are equal */
#define SEARCH_NULL_ARE_NOT_EQUAL 65536U/* NULL in keys are not equal */
/* Use this when inserting a key in position order */
#define SEARCH_INSERT   (SEARCH_NULL_ARE_NOT_EQUAL*2)
/* Only part of the key is specified while reading */
#define SEARCH_PART_KEY (SEARCH_INSERT*2)
/* Used when user key (key 2) contains transaction id's */
#define SEARCH_USER_KEY_HAS_TRANSID (SEARCH_PART_KEY*2)
/* Used when page key (key 1) contains transaction id's */
#define SEARCH_PAGE_KEY_HAS_TRANSID (SEARCH_USER_KEY_HAS_TRANSID*2)

	/* bits in opt_flag */
#define QUICK_USED	1U
#define READ_CACHE_USED	2U
#define READ_CHECK_USED 4U
#define KEY_READ_USED	8U
#define WRITE_CACHE_USED 16U
#define OPT_NO_ROWS	32U

	/* bits in update */
#define HA_STATE_CHANGED	1U	/* Database has changed */
#define HA_STATE_AKTIV		2U	/* Has a current record */
#define HA_STATE_WRITTEN	4U	/* Record is written */
#define HA_STATE_DELETED	8U
#define HA_STATE_NEXT_FOUND	16U	/* Next found record (record before) */
#define HA_STATE_PREV_FOUND	32U	/* Prev found record (record after) */
#define HA_STATE_NO_KEY		64U	/* Last read didn't find record */
#define HA_STATE_KEY_CHANGED	128U
#define HA_STATE_WRITE_AT_END	256U	/* set in _ps_find_writepos */
#define HA_STATE_BUFF_SAVED	512U	/* If current keybuff is info->buff */
#define HA_STATE_ROW_CHANGED	1024U	/* To invalidate ROW cache */
#define HA_STATE_EXTEND_BLOCK	2048U
#define HA_STATE_RNEXT_SAME	4096U	/* rnext_same occupied lastkey2 */

/* myisampack expects no more than 32 field types. */
enum en_fieldtype {
  FIELD_LAST=-1,FIELD_NORMAL,FIELD_SKIP_ENDSPACE,FIELD_SKIP_PRESPACE,
  FIELD_SKIP_ZERO,FIELD_BLOB,FIELD_CONSTANT,FIELD_INTERVALL,FIELD_ZERO,
  FIELD_VARCHAR,FIELD_CHECK,
  FIELD_enum_val_count
};

enum data_file_type {
  STATIC_RECORD, DYNAMIC_RECORD, COMPRESSED_RECORD, BLOCK_RECORD, NO_RECORD
};

/* For key ranges */

#define NO_MIN_RANGE	1U
#define NO_MAX_RANGE	2U
#define NEAR_MIN	4U
#define NEAR_MAX	8U
#define UNIQUE_RANGE	16U
#define EQ_RANGE	32U
#define NULL_RANGE	64U
#define GEOM_FLAG      128U

typedef struct st_key_range
{
  const uchar *key;
  uint length;
  key_part_map keypart_map;
  enum ha_rkey_function flag;
} key_range;

typedef void *range_id_t;

typedef struct st_key_multi_range
{
  key_range start_key;
  key_range end_key;
  range_id_t ptr;                 /* Free to use by caller (ptr to row etc) */
  /*
    A set of range flags that describe both endpoints: UNIQUE_RANGE,
    NULL_RANGE, EQ_RANGE, GEOM_FLAG.
    (Flags that describe one endpoint, NO_{MIN|MAX}_RANGE, NEAR_{MIN|MAX} will
     not be set here)
  */
  uint  range_flag;
} KEY_MULTI_RANGE;


/* Store first and last leaf page accessed by records_in_range */

typedef struct st_page_range
{
  ulonglong first_page;
  ulonglong last_page;
} page_range;

#define UNUSED_PAGE_NO ULONGLONG_MAX
#define unused_page_range { UNUSED_PAGE_NO, UNUSED_PAGE_NO }

/* For number of records */
#ifdef BIG_TABLES
#define rows2double(A)	ulonglong2double(A)
typedef my_off_t	ha_rows;
#else
#define rows2double(A)	(double) (A)
typedef ulong		ha_rows;
#endif

#define HA_POS_ERROR	(~ (ha_rows) 0)
#define HA_OFFSET_ERROR	(~ (my_off_t) 0)
#define HA_ROWS_MAX        HA_POS_ERROR

#if SIZEOF_OFF_T == 4
#define MAX_FILE_SIZE	INT_MAX32
#else
#define MAX_FILE_SIZE	LONGLONG_MAX
#endif

#define HA_VARCHAR_PACKLENGTH(field_length) ((field_length) < 256 ? 1 :2)

/* invalidator function reference for Query Cache */
C_MODE_START
typedef void (* invalidator_by_filename)(const char * filename);
C_MODE_END

#endif /* _my_base_h */
