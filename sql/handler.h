#ifndef HANDLER_INCLUDED
#define HANDLER_INCLUDED
/*
   Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2009, 2023, MariaDB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/* Definitions for parameters to do with handler-routines */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "sql_const.h"
#include "sql_basic_types.h"
#include "mysqld.h"                             /* server_id */
#include "optimizer_costs.h"
#include "sql_plugin.h"        /* plugin_ref, st_plugin_int, plugin */
#include "thr_lock.h"          /* thr_lock_type, THR_LOCK_DATA */
#include "sql_cache.h"
#include "structs.h"                            /* SHOW_COMP_OPTION */
#include "sql_array.h"          /* Dynamic_array<> */
#include "mdl.h"
#include "vers_string.h"
#include "ha_handler_stats.h"
#include "optimizer_costs.h"

#include "sql_analyze_stmt.h" // for Exec_time_tracker 

#include <my_compare.h>
#include <ft_global.h>
#include <keycache.h>
#include <mysql/psi/mysql_table.h>
#include "sql_sequence.h"
#include "mem_root_array.h"
#include <utility>     // pair
#include <my_attribute.h> /* __attribute__ */

class Alter_info;
class Virtual_column_info;
class sequence_definition;
class Rowid_filter;
class Field_string;
class Field_varstring;
class Field_blob;
class Column_definition;
class select_result;

// the following is for checking tables

#define HA_ADMIN_ALREADY_DONE	  1
#define HA_ADMIN_OK               0
#define HA_ADMIN_NOT_IMPLEMENTED -1
#define HA_ADMIN_FAILED		 -2
#define HA_ADMIN_CORRUPT         -3
#define HA_ADMIN_INTERNAL_ERROR  -4
#define HA_ADMIN_INVALID         -5
#define HA_ADMIN_REJECT          -6
#define HA_ADMIN_TRY_ALTER       -7
#define HA_ADMIN_WRONG_CHECKSUM  -8
#define HA_ADMIN_NOT_BASE_TABLE  -9
#define HA_ADMIN_NEEDS_UPGRADE  -10
#define HA_ADMIN_NEEDS_ALTER    -11
#define HA_ADMIN_NEEDS_CHECK    -12
#define HA_ADMIN_COMMIT_ERROR   -13

/**
   Return values for check_if_supported_inplace_alter().

   @see check_if_supported_inplace_alter() for description of
   the individual values.
*/
enum enum_alter_inplace_result {
  HA_ALTER_ERROR,
  HA_ALTER_INPLACE_COPY_NO_LOCK,
  HA_ALTER_INPLACE_COPY_LOCK,
  HA_ALTER_INPLACE_NOCOPY_LOCK,
  HA_ALTER_INPLACE_NOCOPY_NO_LOCK,
  HA_ALTER_INPLACE_INSTANT,
  HA_ALTER_INPLACE_NOT_SUPPORTED,
  HA_ALTER_INPLACE_EXCLUSIVE_LOCK,
  HA_ALTER_INPLACE_SHARED_LOCK,
  HA_ALTER_INPLACE_NO_LOCK
};

/* Flags for create_partitioning_metadata() */

enum chf_create_flags {
  CHF_CREATE_FLAG,
  CHF_DELETE_FLAG,
  CHF_RENAME_FLAG,
  CHF_INDEX_FLAG
};

/* Bits in table_flags() to show what database can do */

#define HA_NO_TRANSACTIONS     (1ULL << 0) /* Doesn't support transactions */
#define HA_PARTIAL_COLUMN_READ (1ULL << 1) /* read may not return all columns */
#define HA_TABLE_SCAN_ON_INDEX (1ULL << 2) /* No separate data/index file */
/*
  The following should be set if the following is not true when scanning
  a table with rnd_next()
  - We will see all rows (including deleted ones)
  - Row positions are 'table->s->db_record_offset' apart
  If this flag is not set, filesort will do a position() call for each matched
  row to be able to find the row later.
*/
#define HA_REC_NOT_IN_SEQ      (1ULL << 3)
#define HA_CAN_GEOMETRY        (1ULL << 4)
/*
  Reading keys in random order is as fast as reading keys in sort order
  (Used in records.cc to decide if we should use a record cache and by
  filesort to decide if we should sort key + data or key + pointer-to-row
*/
#define HA_FAST_KEY_READ       (1ULL << 5)
/*
  Set the following flag if we on delete should force all key to be read
  and on update read all keys that changes
*/
#define HA_REQUIRES_KEY_COLUMNS_FOR_DELETE (1ULL << 6)
#define HA_NULL_IN_KEY         (1ULL << 7) /* One can have keys with NULL */
#define HA_DUPLICATE_POS       (1ULL << 8)    /* ha_position() gives dup row */
#define HA_NO_BLOBS            (1ULL << 9) /* Doesn't support blobs */
#define HA_CAN_INDEX_BLOBS     (1ULL << 10)
#define HA_AUTO_PART_KEY       (1ULL << 11) /* auto-increment in multi-part key */
/*
  The engine requires every table to have a user-specified PRIMARY KEY.
  Do not set the flag if the engine can generate a hidden primary key internally.
  This flag is ignored if a SEQUENCE is created (which, in turn, needs
  HA_CAN_TABLES_WITHOUT_ROLLBACK flag)
*/
#define HA_REQUIRE_PRIMARY_KEY (1ULL << 12)
#define HA_STATS_RECORDS_IS_EXACT (1ULL << 13) /* stats.records is exact */
/*
  INSERT_DELAYED only works with handlers that uses MySQL internal table
  level locks
*/
#define HA_CAN_INSERT_DELAYED  (1ULL << 14)
/*
  If we get the primary key columns for free when we do an index read
  (usually, it also implies that HA_PRIMARY_KEY_REQUIRED_FOR_POSITION
  flag is set).
*/
#define HA_PRIMARY_KEY_IN_READ_INDEX (1ULL << 15)
/*
  If HA_PRIMARY_KEY_REQUIRED_FOR_POSITION is set, it means that to position()
  uses a primary key given by the record argument.
  Without primary key, we can't call position().
  If not set, the position is returned as the current rows position
  regardless of what argument is given.
*/ 
#define HA_PRIMARY_KEY_REQUIRED_FOR_POSITION (1ULL << 16) 
#define HA_CAN_RTREEKEYS       (1ULL << 17)
#define HA_NOT_DELETE_WITH_CACHE (1ULL << 18) /* unused */
/*
  The following is we need to a primary key to delete (and update) a row.
  If there is no primary key, all columns needs to be read on update and delete
*/
#define HA_PRIMARY_KEY_REQUIRED_FOR_DELETE (1ULL << 19)
#define HA_NO_PREFIX_CHAR_KEYS (1ULL << 20)
#define HA_CAN_FULLTEXT        (1ULL << 21)
#define HA_CAN_SQL_HANDLER     (1ULL << 22)
#define HA_NO_AUTO_INCREMENT   (1ULL << 23)
/* Has automatic checksums and uses the old checksum format */
#define HA_HAS_OLD_CHECKSUM    (1ULL << 24)
/* Table data are stored in separate files (for lower_case_table_names) */
#define HA_FILE_BASED	       (1ULL << 26)
#define HA_CAN_BIT_FIELD       (1ULL << 28) /* supports bit fields */
#define HA_NEED_READ_RANGE_BUFFER (1ULL << 29) /* for read_multi_range */
#define HA_ANY_INDEX_MAY_BE_UNIQUE (1ULL << 30)
#define HA_NO_COPY_ON_ALTER    (1ULL << 31)
#define HA_HAS_RECORDS	       (1ULL << 32) /* records() gives exact count*/
/* Has it's own method of binlog logging */
#define HA_HAS_OWN_BINLOGGING  (1ULL << 33)
/*
  Engine is capable of row-format and statement-format logging,
  respectively
*/
#define HA_BINLOG_ROW_CAPABLE  (1ULL << 34)
#define HA_BINLOG_STMT_CAPABLE (1ULL << 35)

/*
    When a multiple key conflict happens in a REPLACE command mysql
    expects the conflicts to be reported in the ascending order of
    key names.

    For e.g.

    CREATE TABLE t1 (a INT, UNIQUE (a), b INT NOT NULL, UNIQUE (b), c INT NOT
                     NULL, INDEX(c));

    REPLACE INTO t1 VALUES (1,1,1),(2,2,2),(2,1,3);

    MySQL expects the conflict with 'a' to be reported before the conflict with
    'b'.

    If the underlying storage engine does not report the conflicting keys in
    ascending order, it causes unexpected errors when the REPLACE command is
    executed.

    This flag helps the underlying SE to inform the server that the keys are not
    ordered.
*/
#define HA_DUPLICATE_KEY_NOT_IN_ORDER    (1ULL << 36)

/*
  Engine supports REPAIR TABLE. Used by CHECK TABLE FOR UPGRADE if an
  incompatible table is detected. If this flag is set, CHECK TABLE FOR UPGRADE
  will report ER_TABLE_NEEDS_UPGRADE, otherwise ER_TABLE_NEED_REBUILD.
*/
#define HA_CAN_REPAIR                    (1ULL << 37)

/* Has automatic checksums and uses the new checksum format */
#define HA_HAS_NEW_CHECKSUM    (1ULL << 38)
#define HA_CAN_VIRTUAL_COLUMNS (1ULL << 39)
#define HA_MRR_CANT_SORT       (1ULL << 40)
/* All of VARCHAR is stored, including bytes after real varchar data */
#define HA_RECORD_MUST_BE_CLEAN_ON_WRITE (1ULL << 41)

/*
  This storage engine supports condition pushdown
*/
#define HA_CAN_TABLE_CONDITION_PUSHDOWN (1ULL << 42)
/* old name for the same flag */
#define HA_MUST_USE_TABLE_CONDITION_PUSHDOWN HA_CAN_TABLE_CONDITION_PUSHDOWN

/**
  The handler supports read before write removal optimization

  Read before write removal may be used for storage engines which support
  write without previous read of the row to be updated. Handler returning
  this flag must implement start_read_removal() and end_read_removal().
  The handler may return "fake" rows constructed from the key of the row
  asked for. This is used to optimize UPDATE and DELETE by reducing the
  number of roundtrips between handler and storage engine.
  
  Example:
  UPDATE a=1 WHERE pk IN (<keys>)

  Sql_cmd_update::update_single_table()
  {
    if (<conditions for starting read removal>)
      start_read_removal()
      -> handler returns true if read removal supported for this table/query

    while(read_record("pk=<key>"))
      -> handler returns fake row with column "pk" set to <key>

      ha_update_row()
      -> handler sends write "a=1" for row with "pk=<key>"

    end_read_removal()
    -> handler returns the number of rows actually written
  }

  @note This optimization in combination with batching may be used to
        remove even more roundtrips.
*/
#define HA_READ_BEFORE_WRITE_REMOVAL  (1ULL << 43)

/*
  Engine supports extended fulltext API
 */
#define HA_CAN_FULLTEXT_EXT              (1ULL << 44)

/*
  Storage engine supports table export using the
  FLUSH TABLE <table_list> FOR EXPORT statement
  (meaning, after this statement one can copy table files out of the
  datadir and later "import" (somehow) in another MariaDB instance)
 */
#define HA_CAN_EXPORT                 (1ULL << 45)

/*
  Storage engine does not require an exclusive metadata lock
  on the table during optimize. (TODO and repair?).
  It can allow other connections to open the table.
  (it does not necessarily mean that other connections can
  read or modify the table - this is defined by THR locks and the
  ::store_lock() method).
*/
#define HA_CONCURRENT_OPTIMIZE          (1ULL << 46)

/*
  If the storage engine support tables that will not roll back on commit
  In addition the table should not lock rows and support READ and WRITE
  UNCOMMITTED.
  This is useful for implementing things like SEQUENCE but can also in
  the future be useful to do logging that should never roll back.
*/
#define HA_CAN_TABLES_WITHOUT_ROLLBACK  (1ULL << 47)

/*
  Mainly for usage by SEQUENCE engine. Setting this flag means
  that the table will never roll back and that all operations
  for this table should stored in the non transactional log
  space that will always be written, even on rollback.
*/

#define HA_PERSISTENT_TABLE              (1ULL << 48)

/*
  If storage engine uses another engine as a base
  This flag is also needed if the table tries to open the .frm file
  as part of drop table.
*/
#define HA_REUSES_FILE_NAMES             (1ULL << 49)

/*
  Set of all binlog flags. Currently only contain the capabilities
  flags.
 */
#define HA_BINLOG_FLAGS (HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE)

/* The following are used by Spider */
#define HA_CAN_FORCE_BULK_UPDATE (1ULL << 50)
#define HA_CAN_FORCE_BULK_DELETE (1ULL << 51)
#define HA_CAN_DIRECT_UPDATE_AND_DELETE (1ULL << 52)

/* The following is for partition handler */
#define HA_CAN_MULTISTEP_MERGE (1LL << 53)

/* calling cmp_ref() on the engine is expensive */
#define HA_SLOW_CMP_REF         (1ULL << 54)
#define HA_CMP_REF_IS_EXPENSIVE HA_SLOW_CMP_REF

/**
  Some engines are unable to provide an efficient implementation for rnd_pos().
  Server will try to avoid it, if possible

  TODO better to do it with cost estimates, not with an explicit flag
*/
#define HA_SLOW_RND_POS  (1ULL << 55)

/* Safe for online backup */
#define HA_CAN_ONLINE_BACKUPS (1ULL << 56)

/* Support native hash index */
#define HA_CAN_HASH_KEYS        (1ULL << 57)
#define HA_CRASH_SAFE           (1ULL << 58)

/*
  There is no need to evict the table from the table definition cache having
  run ANALYZE TABLE on it
 */
#define HA_ONLINE_ANALYZE             (1ULL << 59)
/*
  Rowid's are not comparable. This is set if the rowid is unique to the
  current open handler, like it is with federated where the rowid is a
  pointer to a local result set buffer. The effect of having this set is
  that the optimizer will not consider the following optimizations for
  the table:
  ror scans, filtering or duplicate weedout
*/
#define HA_NON_COMPARABLE_ROWID (1ULL << 60)

/* Implements SELECT ... FOR UPDATE SKIP LOCKED */
#define HA_CAN_SKIP_LOCKED  (1ULL << 61)

/* This engine is not compatible with Online ALTER TABLE */
#define HA_NO_ONLINE_ALTER  (1ULL << 62)

#define HA_LAST_TABLE_FLAG HA_NO_ONLINE_ALTER


/* bits in index_flags(index_number) for what you can do with index */
#define HA_READ_NEXT            1       /* TODO really use this flag */
#define HA_READ_PREV            2       /* supports ::index_prev */
#define HA_READ_ORDER           4       /* index_next/prev follow sort order */
#define HA_READ_RANGE           8       /* can find all records in a range */
#define HA_ONLY_WHOLE_INDEX	16	/* Can't use part key searches */
#define HA_KEYREAD_ONLY         64	/* Support HA_EXTRA_KEYREAD */

/*
  Index scan will not return records in rowid order. Not guaranteed to be
  set for unordered (e.g. HASH) indexes.
*/
#define HA_KEY_SCAN_NOT_ROR     128 
#define HA_DO_INDEX_COND_PUSHDOWN  256 /* Supports Index Condition Pushdown */
/*
  Data is clustered on this key. This means that when you read the key
  you also get the row data without any additional disk reads.
*/
#define HA_CLUSTERED_INDEX      512

#define HA_DO_RANGE_FILTER_PUSHDOWN  1024

/*
  bits in alter_table_flags:
*/
/*
  These bits are set if different kinds of indexes can be created or dropped
  in-place without re-creating the table using a temporary table.
  NO_READ_WRITE indicates that the handler needs concurrent reads and writes
  of table data to be blocked.
  Partitioning needs both ADD and DROP to be supported by its underlying
  handlers, due to error handling, see bug#57778.
*/
#define HA_INPLACE_ADD_INDEX_NO_READ_WRITE         (1UL << 0)
#define HA_INPLACE_DROP_INDEX_NO_READ_WRITE        (1UL << 1)
#define HA_INPLACE_ADD_UNIQUE_INDEX_NO_READ_WRITE  (1UL << 2)
#define HA_INPLACE_DROP_UNIQUE_INDEX_NO_READ_WRITE (1UL << 3)
#define HA_INPLACE_ADD_PK_INDEX_NO_READ_WRITE      (1UL << 4)
#define HA_INPLACE_DROP_PK_INDEX_NO_READ_WRITE     (1UL << 5)
/*
  These are set if different kinds of indexes can be created or dropped
  in-place while still allowing concurrent reads (but not writes) of table
  data. If a handler is capable of one or more of these, it should also set
  the corresponding *_NO_READ_WRITE bit(s).
*/
#define HA_INPLACE_ADD_INDEX_NO_WRITE              (1UL << 6)
#define HA_INPLACE_DROP_INDEX_NO_WRITE             (1UL << 7)
#define HA_INPLACE_ADD_UNIQUE_INDEX_NO_WRITE       (1UL << 8)
#define HA_INPLACE_DROP_UNIQUE_INDEX_NO_WRITE      (1UL << 9)
#define HA_INPLACE_ADD_PK_INDEX_NO_WRITE           (1UL << 10)
#define HA_INPLACE_DROP_PK_INDEX_NO_WRITE          (1UL << 11)
/*
  HA_PARTITION_FUNCTION_SUPPORTED indicates that the function is
  supported at all.
  HA_FAST_CHANGE_PARTITION means that optimised variants of the changes
  exists but they are not necessarily done online.

  HA_ONLINE_DOUBLE_WRITE means that the handler supports writing to both
  the new partition and to the old partitions when updating through the
  old partitioning schema while performing a change of the partitioning.
  This means that we can support updating of the table while performing
  the copy phase of the change. For no lock at all also a double write
  from new to old must exist and this is not required when this flag is
  set.
  This is actually removed even before it was introduced the first time.
  The new idea is that handlers will handle the lock level already in
  store_lock for ALTER TABLE partitions.

  HA_PARTITION_ONE_PHASE is a flag that can be set by handlers that take
  care of changing the partitions online and in one phase. Thus all phases
  needed to handle the change are implemented inside the storage engine.
  The storage engine must also support auto-discovery since the frm file
  is changed as part of the change and this change must be controlled by
  the storage engine. A typical engine to support this is NDB (through
  WL #2498).
*/
#define HA_PARTITION_FUNCTION_SUPPORTED         (1UL << 12)
#define HA_FAST_CHANGE_PARTITION                (1UL << 13)
#define HA_PARTITION_ONE_PHASE                  (1UL << 14)

/* operations for disable/enable indexes */
#define HA_KEY_SWITCH_NONUNIQ      0
#define HA_KEY_SWITCH_ALL          1
#define HA_KEY_SWITCH_NONUNIQ_SAVE 2
#define HA_KEY_SWITCH_ALL_SAVE     3

/*
  Note: the following includes binlog and closing 0.
  TODO remove the limit, use dynarrays
*/
#define MAX_HA 64

/*
  Use this instead of 0 as the initial value for the slot number of
  handlerton, so that we can distinguish uninitialized slot number
  from slot 0.
*/
#define HA_SLOT_UNDEF ((uint)-1)

/*
  Parameters for open() (in register form->filestat)
  HA_GET_INFO does an implicit HA_ABORT_IF_LOCKED
*/

#define HA_OPEN_KEYFILE		1U
#define HA_READ_ONLY		16U	/* File opened as readonly */
/* Try readonly if can't open with read and write */
#define HA_TRY_READ_ONLY	32U

	/* Some key definitions */
#define HA_KEY_NULL_LENGTH	1
#define HA_KEY_BLOB_LENGTH	2

/* Maximum length of any index lookup key, in bytes */

#define MAX_KEY_LENGTH (MAX_DATA_LENGTH_FOR_KEY \
                         +(MAX_REF_PARTS \
                          *(HA_KEY_NULL_LENGTH + HA_KEY_BLOB_LENGTH)))

#define HA_LEX_CREATE_TMP_TABLE	1U
#define HA_CREATE_TMP_ALTER     8U
#define HA_LEX_CREATE_SEQUENCE  16U
#define HA_VERSIONED_TABLE      32U
#define HA_SKIP_KEY_SORT        64U

#define HA_MAX_REC_LENGTH	65535

/* Table caching type */
#define HA_CACHE_TBL_NONTRANSACT 0
#define HA_CACHE_TBL_NOCACHE     1U
#define HA_CACHE_TBL_ASKTRANSACT 2U
#define HA_CACHE_TBL_TRANSACT    4U

/**
  Options for the START TRANSACTION statement.

  Note that READ ONLY and READ WRITE are logically mutually exclusive.
  This is enforced by the parser and depended upon by trans_begin().

  We need two flags instead of one in order to differentiate between
  situation when no READ WRITE/ONLY clause were given and thus transaction
  is implicitly READ WRITE and the case when READ WRITE clause was used
  explicitly.
*/

// WITH CONSISTENT SNAPSHOT option
static const uint MYSQL_START_TRANS_OPT_WITH_CONS_SNAPSHOT = 1;
// READ ONLY option
static const uint MYSQL_START_TRANS_OPT_READ_ONLY          = 2;
// READ WRITE option
static const uint MYSQL_START_TRANS_OPT_READ_WRITE         = 4;

/* Flags for method is_fatal_error */
#define HA_CHECK_DUP_KEY 1U
#define HA_CHECK_DUP_UNIQUE 2U
#define HA_CHECK_FK_ERROR 4U
#define HA_CHECK_DUP (HA_CHECK_DUP_KEY + HA_CHECK_DUP_UNIQUE)
#define HA_CHECK_ALL (~0U)

/* Options for info_push() */
#define INFO_KIND_UPDATE_FIELDS       101
#define INFO_KIND_UPDATE_VALUES       102
#define INFO_KIND_FORCE_LIMIT_BEGIN   103
#define INFO_KIND_FORCE_LIMIT_END     104

enum legacy_db_type
{
  /* note these numerical values are fixed and can *not* be changed */
  DB_TYPE_UNKNOWN=0,
  DB_TYPE_HEAP=6,
  DB_TYPE_MYISAM=9,
  DB_TYPE_MRG_MYISAM=10,
  DB_TYPE_INNODB=12,
  DB_TYPE_EXAMPLE_DB=15,
  DB_TYPE_ARCHIVE_DB=16,
  DB_TYPE_CSV_DB=17,
  DB_TYPE_FEDERATED_DB=18,
  DB_TYPE_BLACKHOLE_DB=19,
  DB_TYPE_PARTITION_DB=20,
  DB_TYPE_BINLOG=21,
  DB_TYPE_ONLINE_ALTER=22,
  DB_TYPE_PBXT=23,
  DB_TYPE_PERFORMANCE_SCHEMA=28,
  DB_TYPE_S3=41,
  DB_TYPE_ARIA=42,
  DB_TYPE_TOKUDB=43, /* disabled in MariaDB Server 10.5, removed in 10.6 */
  DB_TYPE_SEQUENCE=44,
  DB_TYPE_FIRST_DYNAMIC=45,
  DB_TYPE_DEFAULT=127 // Must be last
};
/*
  Better name for DB_TYPE_UNKNOWN. Should be used for engines that do not have
  a hard-coded type value here.
 */
#define DB_TYPE_AUTOASSIGN DB_TYPE_UNKNOWN

enum row_type { ROW_TYPE_NOT_USED=-1, ROW_TYPE_DEFAULT, ROW_TYPE_FIXED,
		ROW_TYPE_DYNAMIC, ROW_TYPE_COMPRESSED,
		ROW_TYPE_REDUNDANT, ROW_TYPE_COMPACT, ROW_TYPE_PAGE };

/* not part of the enum, so that it shouldn't be in switch(row_type) */
#define ROW_TYPE_MAX ((uint)ROW_TYPE_PAGE + 1)

/* Specifies data storage format for individual columns */
enum column_format_type {
  COLUMN_FORMAT_TYPE_DEFAULT=   0, /* Not specified (use engine default) */
  COLUMN_FORMAT_TYPE_FIXED=     1, /* FIXED format */
  COLUMN_FORMAT_TYPE_DYNAMIC=   2  /* DYNAMIC format */
};

enum enum_binlog_func {
  BFN_RESET_LOGS=        1,
  BFN_RESET_SLAVE=       2,
  BFN_BINLOG_WAIT=       3,
  BFN_BINLOG_END=        4,
  BFN_BINLOG_PURGE_FILE= 5
};

enum enum_binlog_command {
  LOGCOM_CREATE_TABLE,
  LOGCOM_ALTER_TABLE,
  LOGCOM_RENAME_TABLE,
  LOGCOM_DROP_TABLE,
  LOGCOM_CREATE_DB,
  LOGCOM_ALTER_DB,
  LOGCOM_DROP_DB
};

/* struct to hold information about the table that should be created */

/* Bits in used_fields */
#define HA_CREATE_USED_AUTO             (1UL << 0)
#define HA_CREATE_USED_RAID             (1UL << 1) //RAID is no longer available
#define HA_CREATE_USED_UNION            (1UL << 2)
#define HA_CREATE_USED_INSERT_METHOD    (1UL << 3)
#define HA_CREATE_USED_MIN_ROWS         (1UL << 4)
#define HA_CREATE_USED_MAX_ROWS         (1UL << 5)
#define HA_CREATE_USED_AVG_ROW_LENGTH   (1UL << 6)
#define HA_CREATE_USED_PACK_KEYS        (1UL << 7)
#define HA_CREATE_USED_CHARSET          (1UL << 8)
#define HA_CREATE_USED_DEFAULT_CHARSET  (1UL << 9)
#define HA_CREATE_USED_DATADIR          (1UL << 10)
#define HA_CREATE_USED_INDEXDIR         (1UL << 11)
#define HA_CREATE_USED_ENGINE           (1UL << 12)
#define HA_CREATE_USED_CHECKSUM         (1UL << 13)
#define HA_CREATE_USED_DELAY_KEY_WRITE  (1UL << 14)
#define HA_CREATE_USED_ROW_FORMAT       (1UL << 15)
#define HA_CREATE_USED_COMMENT          (1UL << 16)
#define HA_CREATE_USED_PASSWORD         (1UL << 17)
#define HA_CREATE_USED_CONNECTION       (1UL << 18)
#define HA_CREATE_USED_KEY_BLOCK_SIZE   (1UL << 19)
/* The following two are used by Maria engine: */
#define HA_CREATE_USED_TRANSACTIONAL    (1UL << 20)
#define HA_CREATE_USED_PAGE_CHECKSUM    (1UL << 21)
/** This is set whenever STATS_PERSISTENT=0|1|default has been
specified in CREATE/ALTER TABLE. See also HA_OPTION_STATS_PERSISTENT in
include/my_base.h. It is possible to distinguish whether
STATS_PERSISTENT=default has been specified or no STATS_PERSISTENT= is
given at all. */
#define HA_CREATE_USED_STATS_PERSISTENT (1UL << 22)
/**
   This is set whenever STATS_AUTO_RECALC=0|1|default has been
   specified in CREATE/ALTER TABLE. See enum_stats_auto_recalc.
   It is possible to distinguish whether STATS_AUTO_RECALC=default
   has been specified or no STATS_AUTO_RECALC= is given at all.
*/
#define HA_CREATE_USED_STATS_AUTO_RECALC (1UL << 23)
/**
   This is set whenever STATS_SAMPLE_PAGES=N|default has been
   specified in CREATE/ALTER TABLE. It is possible to distinguish whether
   STATS_SAMPLE_PAGES=default has been specified or no STATS_SAMPLE_PAGES= is
   given at all.
*/
#define HA_CREATE_USED_STATS_SAMPLE_PAGES (1UL << 24)

/* Create a sequence */
#define HA_CREATE_USED_SEQUENCE           (1UL << 25)
/* Tell binlog_show_create_table to print all engine options */
#define HA_CREATE_PRINT_ALL_OPTIONS       (1UL << 26)

typedef ulonglong alter_table_operations;

class Event_log;
class Cache_flip_event_log;
class binlog_cache_data;
class online_alter_cache_data;
typedef bool Log_func(THD*, TABLE*, Event_log *, binlog_cache_data *, bool,
                      ulong, const uchar*, const uchar*);

/*
  These flags are set by the parser and describes the type of
  operation(s) specified by the ALTER TABLE statement.
*/

// Set by parser for ADD [COLUMN]
#define ALTER_PARSER_ADD_COLUMN     (1ULL <<  0)
// Set by parser for DROP [COLUMN]
#define ALTER_PARSER_DROP_COLUMN    (1ULL <<  1)
// Set for CHANGE [COLUMN] | MODIFY [CHANGE] & mysql_recreate_table
#define ALTER_CHANGE_COLUMN         (1ULL <<  2)
// Set for ADD INDEX | ADD KEY | ADD PRIMARY KEY | ADD UNIQUE KEY |
//         ADD UNIQUE INDEX | ALTER ADD [COLUMN]
#define ALTER_ADD_INDEX             (1ULL <<  3)
// Set for DROP PRIMARY KEY | DROP FOREIGN KEY | DROP KEY | DROP INDEX
#define ALTER_DROP_INDEX            (1ULL <<  4)
// Set for RENAME [TO]
#define ALTER_RENAME                (1ULL <<  5)
// Set for ORDER BY
#define ALTER_ORDER                 (1ULL <<  6)
// Set for table_options, like table comment
#define ALTER_OPTIONS               (1ULL <<  7)
// Set for ALTER [COLUMN] ... SET DEFAULT ... | DROP DEFAULT
#define ALTER_CHANGE_COLUMN_DEFAULT (1ULL <<  8)
// Set for DISABLE KEYS | ENABLE KEYS
#define ALTER_KEYS_ONOFF            (1ULL <<  9)
// Set for FORCE, ENGINE(same engine), by mysql_recreate_table()
#define ALTER_RECREATE              (1ULL << 10)
// Set for CONVERT TO
#define ALTER_CONVERT_TO            (1ULL << 11)
// Set for DROP ... ADD some_index
#define ALTER_RENAME_INDEX          (1ULL << 12)
// Set for ADD FOREIGN KEY
#define ALTER_ADD_FOREIGN_KEY       (1ULL << 21)
// Set for DROP FOREIGN KEY
#define ALTER_DROP_FOREIGN_KEY      (1ULL << 22)
#define ALTER_CHANGE_INDEX_COMMENT  (1ULL << 23)
// Set for ADD [COLUMN] FIRST | AFTER
#define ALTER_COLUMN_ORDER          (1ULL << 25)
#define ALTER_ADD_CHECK_CONSTRAINT  (1ULL << 27)
#define ALTER_DROP_CHECK_CONSTRAINT (1ULL << 28)
#define ALTER_RENAME_COLUMN         (1ULL << 29)
#define ALTER_COLUMN_UNVERSIONED    (1ULL << 30)
#define ALTER_ADD_SYSTEM_VERSIONING (1ULL << 31)
#define ALTER_DROP_SYSTEM_VERSIONING (1ULL << 32)
#define ALTER_ADD_PERIOD             (1ULL << 33)
#define ALTER_DROP_PERIOD            (1ULL << 34)

/*
  Following defines are used by ALTER_INPLACE_TABLE

  They do describe in more detail the type operation(s) to be executed
  by the storage engine. For example, which type of type of index to be
  added/dropped.  These are set by fill_alter_inplace_info().
*/

#define ALTER_RECREATE_TABLE	     ALTER_RECREATE
#define ALTER_CHANGE_CREATE_OPTION   ALTER_OPTIONS
#define ALTER_ADD_COLUMN             (ALTER_ADD_VIRTUAL_COLUMN | \
                                      ALTER_ADD_STORED_BASE_COLUMN | \
                                      ALTER_ADD_STORED_GENERATED_COLUMN)
#define ALTER_DROP_COLUMN             (ALTER_DROP_VIRTUAL_COLUMN | \
                                       ALTER_DROP_STORED_COLUMN)
#define ALTER_COLUMN_DEFAULT          ALTER_CHANGE_COLUMN_DEFAULT

// Add non-unique, non-primary index
#define ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX  (1ULL << 35)

// Drop non-unique, non-primary index
#define ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX (1ULL << 36)

// Add unique, non-primary index
#define ALTER_ADD_UNIQUE_INDEX               (1ULL << 37)

// Drop unique, non-primary index
#define ALTER_DROP_UNIQUE_INDEX              (1ULL << 38)

// Add primary index
#define ALTER_ADD_PK_INDEX                   (1ULL << 39)

// Drop primary index
#define ALTER_DROP_PK_INDEX                  (1ULL << 40)

// Virtual generated column
#define ALTER_ADD_VIRTUAL_COLUMN             (1ULL << 41)
// Stored base (non-generated) column
#define ALTER_ADD_STORED_BASE_COLUMN         (1ULL << 42)
// Stored generated column
#define ALTER_ADD_STORED_GENERATED_COLUMN    (1ULL << 43)

// Drop column
#define ALTER_DROP_VIRTUAL_COLUMN            (1ULL << 44)
#define ALTER_DROP_STORED_COLUMN             (1ULL << 45)

// Rename column (verified; ALTER_RENAME_COLUMN may use original name)
#define ALTER_COLUMN_NAME          	     (1ULL << 46)

// Change column datatype
#define ALTER_VIRTUAL_COLUMN_TYPE            (1ULL << 47)
#define ALTER_STORED_COLUMN_TYPE             (1ULL << 48)


// Engine can handle type change by itself in ALGORITHM=INPLACE
#define ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE       (1ULL << 49)

// Reorder column
#define ALTER_STORED_COLUMN_ORDER            (1ULL << 50)

// Reorder column
#define ALTER_VIRTUAL_COLUMN_ORDER           (1ULL << 51)

// Change column from NOT NULL to NULL
#define ALTER_COLUMN_NULLABLE                (1ULL << 52)

// Change column from NULL to NOT NULL
#define ALTER_COLUMN_NOT_NULLABLE            (1ULL << 53)

// Change column generation expression
#define ALTER_VIRTUAL_GCOL_EXPR              (1ULL << 54)
#define ALTER_STORED_GCOL_EXPR               (1ULL << 55)

// column's engine options changed, something in field->option_struct
#define ALTER_COLUMN_OPTION                  (1ULL << 56)

// MySQL alias for the same thing:
#define ALTER_COLUMN_STORAGE_TYPE            ALTER_COLUMN_OPTION

// Change the column format of column
#define ALTER_COLUMN_COLUMN_FORMAT           (1ULL << 57)

/**
  Changes in generated columns that affect storage,
  for example, when a vcol type or expression changes
  and this vcol is indexed or used in a partitioning expression
*/
#define ALTER_COLUMN_VCOL                    (1ULL << 58)

/**
  ALTER TABLE for a partitioned table. The engine needs to commit
  online alter of all partitions atomically (using group_commit_ctx)
*/
#define ALTER_PARTITIONED                    (1ULL << 59)

/**
   Change in index length such that it doesn't require index rebuild.
*/
#define ALTER_COLUMN_INDEX_LENGTH            (1ULL << 60)

/**
  Indicate that index order might have been changed. Disables inplace algorithm
  by default (not for InnoDB).
*/
#define ALTER_INDEX_ORDER                    (1ULL << 61)

/**
  Means that the ignorability of an index is changed.
*/
#define ALTER_INDEX_IGNORABILITY              (1ULL << 62)

/*
  Flags set in partition_flags when altering partitions
*/

// Set for ADD PARTITION
#define ALTER_PARTITION_ADD         (1ULL << 1)
// Set for DROP PARTITION
#define ALTER_PARTITION_DROP        (1ULL << 2)
// Set for COALESCE PARTITION
#define ALTER_PARTITION_COALESCE    (1ULL << 3)
// Set for REORGANIZE PARTITION ... INTO
#define ALTER_PARTITION_REORGANIZE  (1ULL << 4)
// Set for partition_options
#define ALTER_PARTITION_INFO        (1ULL << 5)
// Set for LOAD INDEX INTO CACHE ... PARTITION
// Set for CACHE INDEX ... PARTITION
#define ALTER_PARTITION_ADMIN       (1ULL << 6)
// Set for REBUILD PARTITION
#define ALTER_PARTITION_REBUILD     (1ULL << 7)
// Set for partitioning operations specifying ALL keyword
#define ALTER_PARTITION_ALL         (1ULL << 8)
// Set for REMOVE PARTITIONING
#define ALTER_PARTITION_REMOVE      (1ULL << 9)
// Set for EXCHANGE PARITION
#define ALTER_PARTITION_EXCHANGE    (1ULL << 10)
// Set by Sql_cmd_alter_table_truncate_partition::execute()
#define ALTER_PARTITION_TRUNCATE    (1ULL << 11)
// Set for REORGANIZE PARTITION
#define ALTER_PARTITION_TABLE_REORG (1ULL << 12)
#define ALTER_PARTITION_CONVERT_IN  (1ULL << 13)
#define ALTER_PARTITION_CONVERT_OUT (1ULL << 14)
// Set for vers_add_auto_hist_parts() operation
#define ALTER_PARTITION_AUTO_HIST   (1ULL << 15)

/*
  This is master database for most of system tables. However there
  can be other databases which can hold system tables. Respective
  storage engines define their own system database names.
*/
extern const char *mysqld_system_database;

/*
  Structure to hold list of system_database.system_table.
  This is used at both mysqld and storage engine layer.
*/
struct st_system_tablename
{
  const char *db;
  const char *tablename;
};


typedef ulonglong my_xid; // this line is the same as in log_event.h
#define MYSQL_XID_PREFIX "MySQLXid"
#define MYSQL_XID_PREFIX_LEN 8 // must be a multiple of 8
#define MYSQL_XID_OFFSET (MYSQL_XID_PREFIX_LEN+sizeof(server_id))
#define MYSQL_XID_GTRID_LEN (MYSQL_XID_OFFSET+sizeof(my_xid))

#define XIDDATASIZE MYSQL_XIDDATASIZE
#define MAXGTRIDSIZE 64
#define MAXBQUALSIZE 64

#define COMPATIBLE_DATA_YES 0
#define COMPATIBLE_DATA_NO  1


/**
  struct xid_t is binary compatible with the XID structure as
  in the X/Open CAE Specification, Distributed Transaction Processing:
  The XA Specification, X/Open Company Ltd., 1991.
  http://www.opengroup.org/bookstore/catalog/c193.htm

  @see MYSQL_XID in mysql/plugin.h
*/
struct xid_t {
  long formatID;
  long gtrid_length;
  long bqual_length;
  char data[XIDDATASIZE];  // not \0-terminated !

  xid_t() = default;                                /* Remove gcc warning */
  bool eq(struct xid_t *xid) const
  { return !xid->is_null() && eq(xid->gtrid_length, xid->bqual_length, xid->data); }
  bool eq(long g, long b, const char *d) const
  { return !is_null() && g == gtrid_length && b == bqual_length && !memcmp(d, data, g+b); }
  void set(struct xid_t *xid)
  { memcpy(this, xid, xid->length()); }
  void set(long f, const char *g, long gl, const char *b, long bl)
  {
    formatID= f;
    if ((gtrid_length= gl))
      memcpy(data, g, gl);
    if ((bqual_length= bl))
      memcpy(data+gl, b, bl);
  }
  // Populate server_id if it's specified, otherwise use the current server_id
  void set(ulonglong xid, decltype(::server_id) trx_server_id= server_id)
  {
    my_xid tmp;
    formatID= 1;
    set(MYSQL_XID_PREFIX_LEN, 0, MYSQL_XID_PREFIX);
    memcpy(data+MYSQL_XID_PREFIX_LEN, &trx_server_id, sizeof(trx_server_id));
    tmp= xid;
    memcpy(data+MYSQL_XID_OFFSET, &tmp, sizeof(tmp));
    gtrid_length=MYSQL_XID_GTRID_LEN;
  }
  void set(long g, long b, const char *d)
  {
    formatID= 1;
    gtrid_length= g;
    bqual_length= b;
    memcpy(data, d, g+b);
  }
  bool is_null() const { return formatID == -1; }
  void null() { formatID= -1; }
  my_xid quick_get_my_xid()
  {
    my_xid tmp;
    memcpy(&tmp, data+MYSQL_XID_OFFSET, sizeof(tmp));
    return tmp;
  }
  my_xid get_my_xid()
  {
    return gtrid_length == MYSQL_XID_GTRID_LEN && bqual_length == 0 &&
           !memcmp(data, MYSQL_XID_PREFIX, MYSQL_XID_PREFIX_LEN) ?
           quick_get_my_xid() : 0;
  }
  decltype(::server_id) get_trx_server_id()
  {
    decltype(::server_id) trx_server_id;
    memcpy(&trx_server_id, data+MYSQL_XID_PREFIX_LEN, sizeof(trx_server_id));
    return trx_server_id;
  }
  uint length()
  {
    return static_cast<uint>(sizeof(formatID)) + key_length();
  }
  uchar *key() const
  {
    return (uchar *)&gtrid_length;
  }
  uint key_length() const
  {
    return static_cast<uint>(sizeof(gtrid_length)+sizeof(bqual_length)+
                             gtrid_length+bqual_length);
  }
};
typedef struct xid_t XID;

struct Online_alter_cache_list;
struct XA_data: XID
{
  Online_alter_cache_list *online_alter_cache= NULL;
  XA_data &operator=(const XID &x) { XID::operator=(x); return *this; }
};

/*
  Enumerates a sequence in the order of
  their creation that is in the top-down order of the index file.
  Ranges from zero through MAX_binlog_id.
  Not confuse the value with the binlog file numerical suffix,
  neither with the binlog file line in the binlog index file.
*/
typedef uint Binlog_file_id;
const Binlog_file_id MAX_binlog_id= UINT_MAX;
const my_off_t       MAX_off_t    = (~(my_off_t) 0);
/*
  Compound binlog-id and byte offset of transaction's first event
  in a sequence (e.g the recovery sequence) of binlog files.
  Binlog_offset(0,0) is the minimum value to mean
  the first byte of the first binlog file.
*/
typedef std::pair<Binlog_file_id, my_off_t> Binlog_offset;

/* binlog-based recovery transaction descriptor */
struct xid_recovery_member
{
  my_xid xid;
  uint in_engine_prepare;  // number of engines that have xid prepared
  bool decided_to_commit;
  /*
    Semisync recovery binlog offset. It's initialized with the maximum
    unreachable offset. The max value will remain for any transaction
    not found in binlog to yield its rollback decision as it's guaranteed
    to be within a truncated tail part of the binlog.
  */
  Binlog_offset binlog_coord;
  XID *full_xid;           // needed by wsrep or past it recovery
  decltype(::server_id) server_id;         // server id of orginal server

  xid_recovery_member(my_xid xid_arg, uint prepare_arg, bool decided_arg,
                      XID *full_xid_arg, decltype(::server_id) server_id_arg)
    : xid(xid_arg), in_engine_prepare(prepare_arg),
      decided_to_commit(decided_arg),
      binlog_coord(Binlog_offset(MAX_binlog_id, MAX_off_t)),
      full_xid(full_xid_arg), server_id(server_id_arg) {};
};

/* for recover() handlerton call */
#define MIN_XID_LIST_SIZE  128
#define MAX_XID_LIST_SIZE  (1024*128)

/* Statistics about batch operations like bulk_insert */
struct ha_copy_info
{
  ha_rows records;        /* Used to check if rest of variables can be used */
  ha_rows touched;
  ha_rows copied;
  ha_rows deleted;
  ha_rows updated;
};

/* The handler for a table type.  Will be included in the TABLE structure */

struct TABLE;

/*
  Make sure that the order of schema_tables and enum_schema_tables are the same.
*/
enum enum_schema_tables
{
  SCH_ALL_PLUGINS,
  SCH_APPLICABLE_ROLES,
  SCH_CHARSETS,
  SCH_CHECK_CONSTRAINTS,
  SCH_COLLATIONS,
  SCH_COLLATION_CHARACTER_SET_APPLICABILITY,
  SCH_COLUMNS,
  SCH_COLUMN_PRIVILEGES,
  SCH_ENABLED_ROLES,
  SCH_ENGINES,
  SCH_EVENTS,
  SCH_EXPLAIN_TABULAR,
  SCH_EXPLAIN_JSON,
  SCH_ANALYZE_TABULAR,
  SCH_ANALYZE_JSON,
  SCH_FILES,
  SCH_GLOBAL_STATUS,
  SCH_GLOBAL_VARIABLES,
  SCH_KEYWORDS,
  SCH_KEY_CACHES,
  SCH_KEY_COLUMN_USAGE,
  SCH_OPEN_TABLES,
  SCH_OPTIMIZER_COSTS,
  SCH_OPT_TRACE,
  SCH_PARAMETERS,
  SCH_PARTITIONS,
  SCH_PLUGINS,
  SCH_PROCESSLIST,
  SCH_PROFILES,
  SCH_REFERENTIAL_CONSTRAINTS,
  SCH_PROCEDURES,
  SCH_SCHEMATA,
  SCH_SCHEMA_PRIVILEGES,
  SCH_SESSION_STATUS,
  SCH_SESSION_VARIABLES,
  SCH_STATISTICS,
  SCH_SQL_FUNCTIONS,
  SCH_SYSTEM_VARIABLES,
  SCH_TABLES,
  SCH_TABLESPACES,
  SCH_TABLE_CONSTRAINTS,
  SCH_TABLE_NAMES,
  SCH_TABLE_PRIVILEGES,
  SCH_TRIGGERS,
  SCH_USER_PRIVILEGES,
  SCH_VIEWS
};

struct TABLE_SHARE;
struct HA_CREATE_INFO;
struct st_foreign_key_info;
typedef struct st_foreign_key_info FOREIGN_KEY_INFO;
typedef bool (stat_print_fn)(THD *thd, const char *type, size_t type_len,
                             const char *file, size_t file_len,
                             const char *status, size_t status_len);
enum ha_stat_type { HA_ENGINE_STATUS, HA_ENGINE_LOGS, HA_ENGINE_MUTEX };
extern MYSQL_PLUGIN_IMPORT st_plugin_int *hton2plugin[MAX_HA];

struct handlerton;

#define view_pseudo_hton ((handlerton *)1)

/*
  Definitions for engine-specific table/field/index options in the CREATE TABLE.

  Options are declared with HA_*OPTION_* macros (HA_TOPTION_NUMBER,
  HA_FOPTION_ENUM, HA_IOPTION_STRING, etc).

  Every macros takes the option name, and the name of the underlying field of
  the appropriate C structure. The "appropriate C structure" is
  ha_table_option_struct for table level options,
  ha_field_option_struct for field level options,
  ha_index_option_struct for key level options. The engine either
  defines a structure of this name, or uses #define's to map
  these "appropriate" names to the actual structure type name.

  ULL options use a ulonglong as the backing store.
  HA_*OPTION_NUMBER() takes the option name, the structure field name,
  the default value for the option, min, max, and blk_siz values.

  STRING options use a char* as a backing store.
  HA_*OPTION_STRING takes the option name and the structure field name.
  The default value will be 0.

  ENUM options use a uint as a backing store (not enum!!!).
  HA_*OPTION_ENUM takes the option name, the structure field name,
  the default value for the option as a number, and a string with the
  permitted values for this enum - one string with comma separated values,
  for example: "gzip,bzip2,lzma"

  BOOL options use a bool as a backing store.
  HA_*OPTION_BOOL takes the option name, the structure field name,
  and the default value for the option.
  From the SQL, BOOL options accept YES/NO, ON/OFF, and 1/0.

  The name of the option is limited to 255 bytes,
  the value (for string options) - to the 32767 bytes.

  See ha_example.cc for an example.
*/

struct ha_table_option_struct;
struct ha_field_option_struct;
struct ha_index_option_struct;

enum ha_option_type { HA_OPTION_TYPE_ULL,    /* unsigned long long */
                      HA_OPTION_TYPE_STRING, /* char * */
                      HA_OPTION_TYPE_ENUM,   /* uint */
                      HA_OPTION_TYPE_BOOL,   /* bool */
                      HA_OPTION_TYPE_SYSVAR};/* type of the sysval */

#define HA_xOPTION_NUMBER(name, struc, field, def, min, max, blk_siz)   \
  { HA_OPTION_TYPE_ULL, name, sizeof(name)-1,                        \
    offsetof(struc, field), def, min, max, blk_siz, 0, 0 }
#define HA_xOPTION_STRING(name, struc, field)                        \
  { HA_OPTION_TYPE_STRING, name, sizeof(name)-1,                     \
    offsetof(struc, field), 0, 0, 0, 0, 0, 0}
#define HA_xOPTION_ENUM(name, struc, field, values, def)             \
  { HA_OPTION_TYPE_ENUM, name, sizeof(name)-1,                       \
    offsetof(struc, field), def, 0,                                  \
    sizeof(values)-1, 0, values, 0 }
#define HA_xOPTION_BOOL(name, struc, field, def)                     \
  { HA_OPTION_TYPE_BOOL, name, sizeof(name)-1,                       \
    offsetof(struc, field), def, 0, 1, 0, 0, 0 }
#define HA_xOPTION_SYSVAR(name, struc, field, sysvar)                \
  { HA_OPTION_TYPE_SYSVAR, name, sizeof(name)-1,                     \
    offsetof(struc, field), 0, 0, 0, 0, 0, MYSQL_SYSVAR(sysvar) }
#define HA_xOPTION_END { HA_OPTION_TYPE_ULL, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

#define HA_TOPTION_NUMBER(name, field, def, min, max, blk_siz)          \
  HA_xOPTION_NUMBER(name, ha_table_option_struct, field, def, min, max, blk_siz)
#define HA_TOPTION_STRING(name, field)                               \
  HA_xOPTION_STRING(name, ha_table_option_struct, field)
#define HA_TOPTION_ENUM(name, field, values, def)                    \
  HA_xOPTION_ENUM(name, ha_table_option_struct, field, values, def)
#define HA_TOPTION_BOOL(name, field, def)                            \
  HA_xOPTION_BOOL(name, ha_table_option_struct, field, def)
#define HA_TOPTION_SYSVAR(name, field, sysvar)                       \
  HA_xOPTION_SYSVAR(name, ha_table_option_struct, field, sysvar)
#define HA_TOPTION_END HA_xOPTION_END

#define HA_FOPTION_NUMBER(name, field, def, min, max, blk_siz)          \
  HA_xOPTION_NUMBER(name, ha_field_option_struct, field, def, min, max, blk_siz)
#define HA_FOPTION_STRING(name, field)                               \
  HA_xOPTION_STRING(name, ha_field_option_struct, field)
#define HA_FOPTION_ENUM(name, field, values, def)                    \
  HA_xOPTION_ENUM(name, ha_field_option_struct, field, values, def)
#define HA_FOPTION_BOOL(name, field, def)                            \
  HA_xOPTION_BOOL(name, ha_field_option_struct, field, def)
#define HA_FOPTION_SYSVAR(name, field, sysvar)                       \
  HA_xOPTION_SYSVAR(name, ha_field_option_struct, field, sysvar)
#define HA_FOPTION_END HA_xOPTION_END

#define HA_IOPTION_NUMBER(name, field, def, min, max, blk_siz)          \
  HA_xOPTION_NUMBER(name, ha_index_option_struct, field, def, min, max, blk_siz)
#define HA_IOPTION_STRING(name, field)                               \
  HA_xOPTION_STRING(name, ha_index_option_struct, field)
#define HA_IOPTION_ENUM(name, field, values, def)                    \
  HA_xOPTION_ENUM(name, ha_index_option_struct, field, values, def)
#define HA_IOPTION_BOOL(name, field, def)                            \
  HA_xOPTION_BOOL(name, ha_index_option_struct, field, def)
#define HA_IOPTION_SYSVAR(name, field, sysvar)                       \
  HA_xOPTION_SYSVAR(name, ha_index_option_struct, field, sysvar)
#define HA_IOPTION_END HA_xOPTION_END

typedef struct st_ha_create_table_option {
  enum ha_option_type type;
  const char *name;
  size_t name_length;
  ptrdiff_t offset;
  ulonglong def_value;
  ulonglong min_value, max_value, block_size;
  const char *values;
  struct st_mysql_sys_var *var;
} ha_create_table_option;

class handler;
class group_by_handler;
class derived_handler;
class select_handler;
struct Query;
typedef class st_select_lex SELECT_LEX;
typedef class st_select_lex_unit SELECT_LEX_UNIT;
typedef struct st_order ORDER;

/*
  handlerton is a singleton structure - one instance per storage engine -
  to provide access to storage engine functionality that works on the
  "global" level (unlike handler class that works on a per-table basis)

  usually handlerton instance is defined statically in ha_xxx.cc as

  static handlerton { ... } xxx_hton;

  savepoint_*, prepare, recover, and *_by_xid pointers can be 0.
*/
struct handlerton
{
  /*
    Historical number used for frm file to determine the correct
    storage engine.  This is going away and new engines will just use
    "name" for this.
  */
  enum legacy_db_type db_type;
  /*
    each storage engine has it's own memory area (actually a pointer)
    in the thd, for storing per-connection information.
    It is accessed as

      thd->ha_data[xxx_hton.slot]

   slot number is initialized by MySQL after xxx_init() is called.
   */
   uint slot;
   /*
     to store per-savepoint data storage engine is provided with an area
     of a requested size (0 is ok here).
     savepoint_offset must be initialized statically to the size of
     the needed memory to store per-savepoint information.
     After xxx_init it is changed to be an offset to savepoint storage
     area and need not be used by storage engine.
     see binlog_hton and binlog_savepoint_set/rollback for an example.
   */
   uint savepoint_offset;
   /*
     handlerton methods:

     close_connection is only called if
     thd->ha_data[xxx_hton.slot] is non-zero, so even if you don't need
     this storage area - set it to something, so that MySQL would know
     this storage engine was accessed in this connection
   */
   int  (*close_connection)(handlerton *hton, THD *thd);
   /*
     Tell handler that query has been killed.
   */
   void (*kill_query)(handlerton *hton, THD *thd, enum thd_kill_levels level);
   /*
     sv points to an uninitialized storage area of requested size
     (see savepoint_offset description)
   */
   int  (*savepoint_set)(handlerton *hton, THD *thd, void *sv);
   /*
     sv points to a storage area, that was earlier passed
     to the savepoint_set call
   */
   int  (*savepoint_rollback)(handlerton *hton, THD *thd, void *sv);
   /**
     Check if storage engine allows to release metadata locks which were
     acquired after the savepoint if rollback to savepoint is done.
     @return true  - If it is safe to release MDL locks.
             false - If it is not.
   */
   bool (*savepoint_rollback_can_release_mdl)(handlerton *hton, THD *thd);
   int  (*savepoint_release)(handlerton *hton, THD *thd, void *sv);
   /*
     'all' is true if it's a real commit, that makes persistent changes
     'all' is false if it's not in fact a commit but an end of the
     statement that is part of the transaction.
     NOTE 'all' is also false in auto-commit mode where 'end of statement'
     and 'real commit' mean the same event.
   */
   int (*commit)(handlerton *hton, THD *thd, bool all);
   /*
     The commit_ordered() method is called prior to the commit() method, after
     the transaction manager has decided to commit (not rollback) the
     transaction. Unlike commit(), commit_ordered() is called only when the
     full transaction is committed, not for each commit of statement
     transaction in a multi-statement transaction.

     Not that like prepare(), commit_ordered() is only called when 2-phase
     commit takes place. Ie. when no binary log and only a single engine
     participates in a transaction, one commit() is called, no
     commit_ordered(). So engines must be prepared for this.

     The calls to commit_ordered() in multiple parallel transactions is
     guaranteed to happen in the same order in every participating
     handler. This can be used to ensure the same commit order among multiple
     handlers (eg. in table handler and binlog). So if transaction T1 calls
     into commit_ordered() of handler A before T2, then T1 will also call
     commit_ordered() of handler B before T2.

     Engines that implement this method should during this call make the
     transaction visible to other transactions, thereby making the order of
     transaction commits be defined by the order of commit_ordered() calls.

     The intention is that commit_ordered() should do the minimal amount of
     work that needs to happen in consistent commit order among handlers. To
     preserve ordering, calls need to be serialised on a global mutex, so
     doing any time-consuming or blocking operations in commit_ordered() will
     limit scalability.

     Handlers can rely on commit_ordered() calls to be serialised (no two
     calls can run in parallel, so no extra locking on the handler part is
     required to ensure this).

     Note that commit_ordered() can be called from a different thread than the
     one handling the transaction! So it can not do anything that depends on
     thread local storage, in particular it can not call my_error() and
     friends (instead it can store the error code and delay the call of
     my_error() to the commit() method).

     Similarly, since commit_ordered() returns void, any return error code
     must be saved and returned from the commit() method instead.

     The commit_ordered method is optional, and can be left unset if not
     needed in a particular handler (then there will be no ordering guarantees
     wrt. other engines and binary log).
   */
   void (*commit_ordered)(handlerton *hton, THD *thd, bool all);
   int  (*rollback)(handlerton *hton, THD *thd, bool all);
   int  (*prepare)(handlerton *hton, THD *thd, bool all);
   /*
     The prepare_ordered method is optional. If set, it will be called after
     successful prepare() in all handlers participating in 2-phase
     commit. Like commit_ordered(), it is called only when the full
     transaction is committed, not for each commit of statement transaction.

     The calls to prepare_ordered() among multiple parallel transactions are
     ordered consistently with calls to commit_ordered(). This means that
     calls to prepare_ordered() effectively define the commit order, and that
     each handler will see the same sequence of transactions calling into
     prepare_ordered() and commit_ordered().

     Thus, prepare_ordered() can be used to define commit order for handlers
     that need to do this in the prepare step (like binlog). It can also be
     used to release transaction's locks early in an order consistent with the
     order transactions will be eventually committed.

     Like commit_ordered(), prepare_ordered() calls are serialised to maintain
     ordering, so the intention is that they should execute fast, with only
     the minimal amount of work needed to define commit order. Handlers can
     rely on this serialisation, and do not need to do any extra locking to
     avoid two prepare_ordered() calls running in parallel.

     Like commit_ordered(), prepare_ordered() is not guaranteed to be called
     in the context of the thread handling the rest of the transaction. So it
     cannot invoke code that relies on thread local storage, in particular it
     cannot call my_error().

     prepare_ordered() cannot cause a rollback by returning an error, all
     possible errors must be handled in prepare() (the prepare_ordered()
     method returns void). In case of some fatal error, a record of the error
     must be made internally by the engine and returned from commit() later.

     Note that for user-level XA SQL commands, no consistent ordering among
     prepare_ordered() and commit_ordered() is guaranteed (as that would
     require blocking all other commits for an indefinite time).

     When 2-phase commit is not used (eg. only one engine (and no binlog) in
     transaction), neither prepare() nor prepare_ordered() is called.
   */
   void (*prepare_ordered)(handlerton *hton, THD *thd, bool all);
   int  (*recover)(handlerton *hton, XID *xid_list, uint len);
   int  (*commit_by_xid)(handlerton *hton, XID *xid);
   int  (*rollback_by_xid)(handlerton *hton, XID *xid);
   /*
     The commit_checkpoint_request() handlerton method is used to checkpoint
     the XA recovery process for storage engines that support two-phase
     commit.

     The method is optional - an engine that does not implemented is expected
     to work the traditional way, where every commit() durably flushes the
     transaction to disk in the engine before completion, so XA recovery will
     no longer be needed for that transaction.

     An engine that does implement commit_checkpoint_request() is also
     expected to implement commit_ordered(), so that ordering of commits is
     consistent between 2pc participants. Such engine is no longer required to
     durably flush to disk transactions in commit(), provided that the
     transaction has been successfully prepare()d and commit_ordered(); thus
     potentionally saving one fsync() call. (Engine must still durably flush
     to disk in commit() when no prepare()/commit_ordered() steps took place,
     at least if durable commits are wanted; this happens eg. if binlog is
     disabled).

     The TC will periodically (eg. once per binlog rotation) call
     commit_checkpoint_request(). When this happens, the engine must arrange
     for all transaction that have completed commit_ordered() to be durably
     flushed to disk (this does not include transactions that might be in the
     middle of executing commit_ordered()). When such flush has completed, the
     engine must call commit_checkpoint_notify_ha(), passing back the opaque
     "cookie".

     The flush and call of commit_checkpoint_notify_ha() need not happen
     immediately - it can be scheduled and performed asynchronously (ie. as
     part of next prepare(), or sync every second, or whatever), but should
     not be postponed indefinitely. It is however also permissible to do it
     immediately, before returning from commit_checkpoint_request().

     When commit_checkpoint_notify_ha() is called, the TC will know that the
     transactions are durably committed, and thus no longer require XA
     recovery. It uses that to reduce the work needed for any subsequent XA
     recovery process.
   */
   void (*commit_checkpoint_request)(void *cookie);
  /*
    "Disable or enable checkpointing internal to the storage engine. This is
    used for FLUSH TABLES WITH READ LOCK AND DISABLE CHECKPOINT to ensure that
    the engine will never start any recovery from a time between
    FLUSH TABLES ... ; UNLOCK TABLES.

    While checkpointing is disabled, the engine should pause any background
    write activity (such as tablespace checkpointing) that require consistency
    between different files (such as transaction log and tablespace files) for
    crash recovery to succeed. The idea is to use this to make safe
    multi-volume LVM snapshot backups.
  */
   int  (*checkpoint_state)(handlerton *hton, bool disabled);
   void *(*create_cursor_read_view)(handlerton *hton, THD *thd);
   void (*set_cursor_read_view)(handlerton *hton, THD *thd, void *read_view);
   void (*close_cursor_read_view)(handlerton *hton, THD *thd, void *read_view);
   handler *(*create)(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root);
   void (*drop_database)(handlerton *hton, char* path);
   /*
     return 0 if dropped successfully,
           -1 if nothing was done by design (as in e.g. blackhole)
           an error code (e.g. HA_ERR_NO_SUCH_TABLE) otherwise
   */
   int (*drop_table)(handlerton *hton, const char* path);
   int (*panic)(handlerton *hton, enum ha_panic_function flag);
   int (*start_consistent_snapshot)(handlerton *hton, THD *thd);
   bool (*flush_logs)(handlerton *hton);
   bool (*show_status)(handlerton *hton, THD *thd, stat_print_fn *print, enum ha_stat_type stat);
   uint (*partition_flags)();
   alter_table_operations (*alter_table_flags)(alter_table_operations flags);
   int (*fill_is_table)(handlerton *hton, THD *thd, TABLE_LIST *tables,
                        class Item *cond, 
                        enum enum_schema_tables);
   uint32 flags;                                /* global handler flags */
   /*
      Those handlerton functions below are properly initialized at handler
      init.
   */
   int (*binlog_func)(handlerton *hton, THD *thd, enum_binlog_func fn, void *arg);
   void (*binlog_log_query)(handlerton *hton, THD *thd, 
                            enum_binlog_command binlog_command,
                            const char *query, uint query_length,
                            const char *db, const char *table_name);

   void (*abort_transaction)(handlerton *hton, THD *bf_thd, THD *victim_thd,
                             my_bool signal) __attribute__((nonnull));
   int (*set_checkpoint)(handlerton *hton, const XID *xid);
   int (*get_checkpoint)(handlerton *hton, XID* xid);
  /**
     Check if the version of the table matches the version in the .frm
     file.

     This is mainly used to verify in recovery to check if an inplace
     ALTER TABLE succeded.
     Storage engines that does not support inplace alter table does not
     have to implement this function.

     @param hton      handlerton
     @param path      Path for table
     @param version   The unique id that is stored in the .frm file for
                      CREATE and updated for each ALTER TABLE (but not for
                      simple renames).
                      This is the ID used for the final table.
     @param create_id The value returned from handler->table_version() for
                      the original table (before ALTER TABLE).

     @retval 0     If id matches or table is newer than create_id (depending
                   on what version check the engine supports. This means that
                   The (inplace) alter table did succeed.
     @retval # > 0 Alter table did not succeed.

     Related to handler::discover_check_version().
   */
  int (*check_version)(handlerton *hton, const char *path,
                       const LEX_CUSTRING *version, ulonglong create_id);

  /* Called for all storage handlers after ddl recovery is done */
  void (*signal_ddl_recovery_done)(handlerton *hton);

  /* Called at startup to update default engine costs */
  void (*update_optimizer_costs)(OPTIMIZER_COSTS *costs);
  void *optimizer_costs;                        /* Costs are stored here */

   /*
     Optional clauses in the CREATE/ALTER TABLE
   */
   ha_create_table_option *table_options; // table level options
   ha_create_table_option *field_options; // these are specified per field
   ha_create_table_option *index_options; // these are specified per index

   /**
     The list of extensions of files created for a single table in the
     database directory (datadir/db_name/).

     Used by open_table_error(), by the default rename_table and delete_table
     handler methods, and by the default discovery implementation.
  
     For engines that have more than one file name extensions (separate
     metadata, index, and/or data files), the order of elements is relevant.
     First element of engine file name extensions array should be metadata
     file extention. This is implied by the open_table_error()
     and the default discovery implementation.
     
     Second element - data file extension. This is implied
     assumed by REPAIR TABLE ... USE_FRM implementation.
   */
   const char **tablefile_extensions; // by default - empty list

  /**********************************************************************
   Functions to intercept queries
  **********************************************************************/

  /*
    Create and return a group_by_handler, if the storage engine can execute
    the summary / group by query.
    If the storage engine can't do that, return NULL.

    The server guaranteeds that all tables in the list belong to this
    storage engine.
  */
  group_by_handler *(*create_group_by)(THD *thd, Query *query);

  /*
    Create and return a derived_handler if the storage engine can execute
    the derived table 'derived', otherwise return NULL.
    In a general case 'derived' may contain tables not from the engine.
    If the engine cannot handle or does not want to handle such pushed derived
    the function create_group_by has to return NULL.
  */
  derived_handler *(*create_derived)(THD *thd, TABLE_LIST *derived);

  /*
    Create and return a select_handler for a single SELECT.
    If the storage engine cannot execute the select statement, return NULL
  */
  select_handler *(*create_select) (THD *thd, SELECT_LEX *select_lex,
                                   SELECT_LEX_UNIT *select_lex_unit);

  /*
    Create and return a select_handler for a unit (i.e. multiple SELECTs
    combined with UNION/EXCEPT/INTERSECT). If the storage engine cannot execute
    the statement, return NULL
  */
  select_handler *(*create_unit)(THD *thd, SELECT_LEX_UNIT *select_unit);
   
   /*********************************************************************
     Table discovery API.
     It allows the server to "discover" tables that exist in the storage
     engine, without user issuing an explicit CREATE TABLE statement.
   **********************************************************************/

   /*
     This method is required for any engine that supports automatic table
     discovery, there is no default implementation.

     Given a TABLE_SHARE discover_table() fills it in with a correct table
     structure using one of the TABLE_SHARE::init_from_* methods.

     Returns HA_ERR_NO_SUCH_TABLE if the table did not exist in the engine,
     zero if the table was discovered successfully, or any other
     HA_ERR_* error code as appropriate if the table existed, but the
     discovery failed.
   */
   int (*discover_table)(handlerton *hton, THD* thd, TABLE_SHARE *share);

   /*
     The discover_table_names method tells the server
     about all tables in the specified database that the engine
     knows about. Tables (or file names of tables) are added to
     the provided discovered_list collector object using
     add_table() or add_file() methods.
   */
   class discovered_list
   {
     public:
     virtual bool add_table(const char *tname, size_t tlen) = 0;
     virtual bool add_file(const char *fname) = 0;
     protected: virtual ~discovered_list() = default;
   };

   /*
     By default (if not implemented by the engine, but the discover_table() is
     implemented) it will perform a file-based discovery:

     - if tablefile_extensions[0] is not null, this will discovers all tables
       with the tablefile_extensions[0] extension.

     Returns 0 on success and 1 on error.
   */
   int (*discover_table_names)(handlerton *hton, LEX_CSTRING *db, MY_DIR *dir,
                               discovered_list *result);

   /*
     This is a method that allows to server to check if a table exists without
     an overhead of the complete discovery.

     By default (if not implemented by the engine, but the discovery_table() is
     implemented) it will try to perform a file-based discovery:

     - if tablefile_extensions[0] is not null this will look for a file name
       with the tablefile_extensions[0] extension.

     - if tablefile_extensions[0] is null, this will resort to discover_table().

     Note that resorting to discover_table() is slow and the engine
     should probably implement its own discover_table_existence() method,
     if its tablefile_extensions[0] is null.

     Returns 1 if the table exists and 0 if it does not.
   */
   int (*discover_table_existence)(handlerton *hton, const char *db,
                                   const char *table_name);

   /*
     This is the assisted table discovery method. Unlike the fully
     automatic discovery as above, here a user is expected to issue an
     explicit CREATE TABLE with the appropriate table attributes to
     "assist" the discovery of a table. But this "discovering" CREATE TABLE
     statement will not specify the table structure - the engine discovers
     it using this method. For example, FederatedX uses it in

      CREATE TABLE t1 ENGINE=FEDERATED CONNECTION="mysql://foo/bar/t1";

     Given a TABLE_SHARE discover_table_structure() fills it in with a correct
     table structure using one of the TABLE_SHARE::init_from_* methods.

     Assisted discovery works independently from the automatic discover.
     An engine is allowed to support only assisted discovery and not
     support automatic one. Or vice versa.
   */
   int (*discover_table_structure)(handlerton *hton, THD* thd,
                                   TABLE_SHARE *share, HA_CREATE_INFO *info);

  /*
    Notify the storage engine that the definition of the table (and the .frm
    file) has changed. Returns 0 if ok.
  */
  int (*notify_tabledef_changed)(handlerton *hton, LEX_CSTRING *db,
                                 LEX_CSTRING *table_name, LEX_CUSTRING *frm,
                                 LEX_CUSTRING *org_tabledef_version,
                                 handler *file);

   /*
     System Versioning
   */
   /** Determine if system-versioned data was modified by the transaction.
   @param[in,out] thd          current session
   @param[out]    trx_id       transaction start ID
   @return transaction commit ID
   @retval 0 if no system-versioned data was affected by the transaction */
   ulonglong (*prepare_commit_versioned)(THD *thd, ulonglong *trx_id);

  /** Disable or enable the internal writes of a storage engine */
  void (*disable_internal_writes)(bool disable);

  /* backup */
  void (*prepare_for_backup)(void);
  void (*end_backup)(void);

  /* Server shutdown early notification.*/
  void (*pre_shutdown)(void);

  /*
    Inform handler that partitioning engine has changed the .frm and the .par
    files
  */
  int (*create_partitioning_metadata)(const char *path,
                                      const char *old_path,
                                      chf_create_flags action_flag);
};


extern const char *hton_no_exts[];

static inline LEX_CSTRING *hton_name(const handlerton *hton)
{
  return &(hton2plugin[hton->slot]->name);
}

static inline handlerton *plugin_hton(plugin_ref plugin)
{
  return plugin_data(plugin, handlerton *);
}

static inline sys_var *find_hton_sysvar(handlerton *hton, st_mysql_sys_var *var)
{
  return find_plugin_sysvar(hton2plugin[hton->slot], var);
}

handlerton *ha_default_handlerton(THD *thd);
handlerton *ha_default_tmp_handlerton(THD *thd);

/* Possible flags of a handlerton (there can be 32 of them) */
#define HTON_NO_FLAGS                 0
#define HTON_CLOSE_CURSORS_AT_COMMIT (1 << 0)
#define HTON_ALTER_NOT_SUPPORTED     (1 << 1) //Engine does not support alter
#define HTON_CAN_RECREATE            (1 << 2) //Delete all is used for truncate
#define HTON_HIDDEN                  (1 << 3) //Engine does not appear in lists
#define HTON_NOT_USER_SELECTABLE     (1 << 5)
#define HTON_TEMPORARY_NOT_SUPPORTED (1 << 6) //Having temporary tables not supported
#define HTON_SUPPORT_LOG_TABLES      (1 << 7) //Engine supports log tables
#define HTON_NO_PARTITION            (1 << 8) //Not partition of these tables

/*
  This flag should be set when deciding that the engine does not allow
  row based binary logging (RBL) optimizations.

  Currently, setting this flag, means that table's read/write_set will
  be left untouched when logging changes to tables in this engine. In
  practice this means that the server will not mess around with
  table->write_set and/or table->read_set when using RBL and deciding
  whether to log full or minimal rows.

  It's valuable for instance for virtual tables, eg: Performance
  Schema which have no meaning for replication.
*/
#define HTON_NO_BINLOG_ROW_OPT       (1 << 9)
#define HTON_SUPPORTS_EXTENDED_KEYS  (1 <<10) //supports extended keys
#define HTON_NATIVE_SYS_VERSIONING (1 << 11) //Engine supports System Versioning

// MySQL compatibility. Unused.
#define HTON_SUPPORTS_FOREIGN_KEYS   (1 << 0) //Foreign key constraint supported.

#define HTON_CAN_MERGE               (1 <<11) //Merge type table
// Engine needs to access the main connect string in partitions
#define HTON_CAN_READ_CONNECT_STRING_IN_PARTITION (1 <<12)

/* can be replicated by wsrep replication provider plugin */
#define HTON_WSREP_REPLICATION (1 << 13)

/*
  Set this on the *slave* that's connected to a shared with a master storage.
  The slave will ignore any CREATE TABLE, DROP or updates for this engine.
*/
#define HTON_IGNORE_UPDATES (1 << 14)

/*
  Set this on the *master* that's connected to a shared with a slave storage.
  The table may not exists on the slave. The effects of having this flag are:
  - ALTER TABLE that changes engine from this table to another engine will
    be replicated as CREATE + INSERT
  - CREATE ... LIKE shared_table will be replicated as a full CREATE TABLE
  - ALTER TABLE for this engine will have "IF EXISTS" added.
  - RENAME TABLE for this engine will have "IF EXISTS" added.
  - DROP TABLE for this engine will have "IF EXISTS" added.
*/
#define HTON_TABLE_MAY_NOT_EXIST_ON_SLAVE (1 << 15)

/*
  True if handler cannot rollback transactions. If not true, the transaction
  will be put in the transactional binlog cache.
  For some engines, like Aria, the rollback can happen in case of crash, but
  not trough a handler rollback call.
*/
#define HTON_NO_ROLLBACK (1 << 16)

/*
  This storage engine can support both transactional and non transactional
  tables
*/
#define HTON_TRANSACTIONAL_AND_NON_TRANSACTIONAL (1 << 17)

/*
  Table requires and close and reopen after truncate
  If the handler has HTON_CAN_RECREATE, this flag is not used
*/
#define HTON_REQUIRES_CLOSE_AFTER_TRUNCATE (1 << 18)

/* Truncate requires that all other handlers are closed */
#define HTON_TRUNCATE_REQUIRES_EXCLUSIVE_USE (1 << 19)
/*
  Used by mysql_inplace_alter_table() to decide if we should call
  hton->notify_tabledef_changed() before commit (MyRocks) or after (InnoDB).
*/
#define HTON_REQUIRES_NOTIFY_TABLEDEF_CHANGED_AFTER_COMMIT (1 << 20)

class Ha_trx_info;

struct THD_TRANS
{
  /* true is not all entries in the ht[] support 2pc */
  bool        no_2pc;
  /* storage engines that registered in this transaction */
  Ha_trx_info *ha_list;
  /* 
    The purpose of this flag is to keep track of non-transactional
    tables that were modified in scope of:
    - transaction, when the variable is a member of
    THD::transaction.all
    - top-level statement or sub-statement, when the variable is a
    member of THD::transaction.stmt
    This member has the following life cycle:
    * stmt.modified_non_trans_table is used to keep track of
    modified non-transactional tables of top-level statements. At
    the end of the previous statement and at the beginning of the session,
    it is reset to FALSE.  If such functions
    as mysql_insert(), Sql_cmd_update::update_single_table,
    Sql_cmd_delete::delete_single_table modify a
    non-transactional table, they set this flag to TRUE.  At the
    end of the statement, the value of stmt.modified_non_trans_table 
    is merged with all.modified_non_trans_table and gets reset.
    * all.modified_non_trans_table is reset at the end of transaction
    
    * Since we do not have a dedicated context for execution of a
    sub-statement, to keep track of non-transactional changes in a
    sub-statement, we re-use stmt.modified_non_trans_table. 
    At entrance into a sub-statement, a copy of the value of
    stmt.modified_non_trans_table (containing the changes of the
    outer statement) is saved on stack. Then 
    stmt.modified_non_trans_table is reset to FALSE and the
    substatement is executed. Then the new value is merged with the
    saved value.
  */
  bool modified_non_trans_table;

  void reset() {
    no_2pc= FALSE;
    modified_non_trans_table= FALSE;
    m_unsafe_rollback_flags= 0;
  }
  bool is_empty() const { return ha_list == NULL; }
  THD_TRANS() = default;                        /* Remove gcc warning */

  unsigned int m_unsafe_rollback_flags;
 /*
    Define the type of statements which cannot be rolled back safely.
    Each type occupies one bit in m_unsafe_rollback_flags.
    MODIFIED_NON_TRANS_TABLE is limited to mark only the temporary
    non-transactional table *when* it's cached along with the transactional
    events; the regular table is covered by the "namesake" bool var.
  */
  enum unsafe_statement_types
  {
    MODIFIED_NON_TRANS_TABLE= 1,
    CREATED_TEMP_TABLE= 2,
    DROPPED_TEMP_TABLE= 4,
    DID_WAIT= 8,
    DID_DDL= 0x10,
    EXECUTED_TABLE_ADMIN_CMD= 0x20
  };

  void mark_modified_non_trans_temp_table()
  {
    m_unsafe_rollback_flags|= MODIFIED_NON_TRANS_TABLE;
  }
  bool has_modified_non_trans_temp_table() const
  {
    return (m_unsafe_rollback_flags & MODIFIED_NON_TRANS_TABLE) != 0;
  }
  void mark_executed_table_admin_cmd()
  {
    DBUG_PRINT("debug", ("mark_executed_table_admin_cmd"));
    m_unsafe_rollback_flags|= EXECUTED_TABLE_ADMIN_CMD;
  }
  bool trans_executed_admin_cmd()
  {
    return (m_unsafe_rollback_flags & EXECUTED_TABLE_ADMIN_CMD) != 0;
  }
  void mark_created_temp_table()
  {
    DBUG_PRINT("debug", ("mark_created_temp_table"));
    m_unsafe_rollback_flags|= CREATED_TEMP_TABLE;
  }
  void mark_dropped_temp_table()
  {
    DBUG_PRINT("debug", ("mark_dropped_temp_table"));
    m_unsafe_rollback_flags|= DROPPED_TEMP_TABLE;
  }
  bool has_created_dropped_temp_table() const {
    return
      (m_unsafe_rollback_flags & (CREATED_TEMP_TABLE|DROPPED_TEMP_TABLE)) != 0;
  }
  void mark_trans_did_wait() { m_unsafe_rollback_flags|= DID_WAIT; }
  bool trans_did_wait() const {
    return (m_unsafe_rollback_flags & DID_WAIT) != 0;
  }
  bool is_trx_read_write() const;
  void mark_trans_did_ddl() { m_unsafe_rollback_flags|= DID_DDL; }
  bool trans_did_ddl() const {
    return (m_unsafe_rollback_flags & DID_DDL) != 0;
  }

};

/**
  Either statement transaction or normal transaction - related
  thread-specific storage engine data.

  If a storage engine participates in a statement/transaction,
  an instance of this class is present in
  thd->transaction.{stmt|all}.ha_list. The addition to
  {stmt|all}.ha_list is made by trans_register_ha().

  When it's time to commit or rollback, each element of ha_list
  is used to access storage engine's prepare()/commit()/rollback()
  methods, and also to evaluate if a full two phase commit is
  necessary.

  @sa General description of transaction handling in handler.cc.
*/

class Ha_trx_info
{
public:
  /** Register this storage engine in the given transaction context. */
  void register_ha(THD_TRANS *trans, handlerton *ht_arg)
  {
    DBUG_ASSERT(m_flags == 0);
    DBUG_ASSERT(m_ht == NULL);
    DBUG_ASSERT(m_next == NULL);

    m_ht= ht_arg;
    m_flags= (int) TRX_READ_ONLY; /* Assume read-only at start. */

    m_next= trans->ha_list;
    trans->ha_list= this;
  }

  /** Clear, prepare for reuse. */
  void reset()
  {
    m_next= NULL;
    m_ht= NULL;
    m_flags= 0;
  }

  Ha_trx_info() { reset(); }

  void set_trx_read_write()
  {
    DBUG_ASSERT(is_started());
    m_flags|= (int) TRX_READ_WRITE;
  }
  bool is_trx_read_write() const
  {
    DBUG_ASSERT(is_started());
    return m_flags & (int) TRX_READ_WRITE;
  }
  bool is_started() const { return m_ht != NULL; }
  /** Mark this transaction read-write if the argument is read-write. */
  void coalesce_trx_with(const Ha_trx_info *stmt_trx)
  {
    /*
      Must be called only after the transaction has been started.
      Can be called many times, e.g. when we have many
      read-write statements in a transaction.
    */
    DBUG_ASSERT(is_started());
    if (stmt_trx->is_trx_read_write())
      set_trx_read_write();
  }
  Ha_trx_info *next() const
  {
    DBUG_ASSERT(is_started());
    return m_next;
  }
  handlerton *ht() const
  {
    DBUG_ASSERT(is_started());
    return m_ht;
  }
private:
  enum { TRX_READ_ONLY= 0, TRX_READ_WRITE= 1 };
  /** Auxiliary, used for ha_list management */
  Ha_trx_info *m_next;
  /**
    Although a given Ha_trx_info instance is currently always used
    for the same storage engine, 'ht' is not-NULL only when the
    corresponding storage is a part of a transaction.
  */
  handlerton *m_ht;
  /**
    Transaction flags related to this engine.
    Not-null only if this instance is a part of transaction.
    May assume a combination of enum values above.
  */
  uchar       m_flags;
};


inline bool THD_TRANS::is_trx_read_write() const
{
  Ha_trx_info *ha_info;
  for (ha_info= ha_list; ha_info; ha_info= ha_info->next())
    if (ha_info->is_trx_read_write())
      return TRUE;
  return FALSE;
}


enum enum_tx_isolation { ISO_READ_UNCOMMITTED, ISO_READ_COMMITTED,
			 ISO_REPEATABLE_READ, ISO_SERIALIZABLE};


typedef struct {
  ulonglong data_file_length;
  ulonglong max_data_file_length;
  ulonglong index_file_length;
  ulonglong max_index_file_length;
  ulonglong delete_length;
  ha_rows records;
  ulong mean_rec_length;
  time_t create_time;
  time_t check_time;
  time_t update_time;
  ulonglong check_sum;
  bool check_sum_null;
} PARTITION_STATS;

#define UNDEF_NODEGROUP 65535
class Item;
struct st_table_log_memory_entry;

class partition_info;

struct st_partition_iter;

enum ha_choice { HA_CHOICE_UNDEF, HA_CHOICE_NO, HA_CHOICE_YES, HA_CHOICE_MAX };

enum enum_stats_auto_recalc { HA_STATS_AUTO_RECALC_DEFAULT= 0,
                              HA_STATS_AUTO_RECALC_ON,
                              HA_STATS_AUTO_RECALC_OFF };

/**
  A helper struct for schema DDL statements:
    CREATE SCHEMA [IF NOT EXISTS] name [ schema_specification... ]
    ALTER SCHEMA name [ schema_specification... ]

  It stores the "schema_specification" part of the CREATE/ALTER statements and
  is passed to mysql_create_db() and  mysql_alter_db().
  Currently consists of the schema default character set, collation
  and schema_comment.
*/
struct Schema_specification_st
{
  CHARSET_INFO *default_table_charset;
  LEX_CSTRING *schema_comment;
  void init()
  {
    bzero(this, sizeof(*this));
  }
};

class Create_field;

struct Table_period_info: Sql_alloc
{
  Table_period_info() :
    create_if_not_exists(false),
    constr(NULL),
    unique_keys(0) {}
  Table_period_info(const char *name_arg, size_t size) :
    name(name_arg, size),
    create_if_not_exists(false),
    constr(NULL),
    unique_keys(0){}

  Lex_ident name;

  struct start_end_t
  {
    start_end_t() = default;
    start_end_t(const LEX_CSTRING& _start, const LEX_CSTRING& _end) :
      start(_start),
      end(_end) {}
    Lex_ident start;
    Lex_ident end;
  };
  start_end_t period;
  bool create_if_not_exists;
  Virtual_column_info *constr;
  uint unique_keys;

  bool is_set() const
  {
    DBUG_ASSERT(bool(period.start) == bool(period.end));
    return period.start;
  }

  void set_period(const Lex_ident& start, const Lex_ident& end)
  {
    period.start= start;
    period.end= end;
  }
  bool check_field(const Create_field* f, const Lex_ident& f_name) const;
};

struct Vers_parse_info: public Table_period_info
{
  Vers_parse_info() :
    Table_period_info(STRING_WITH_LEN("SYSTEM_TIME")),
    versioned_fields(false),
    unversioned_fields(false),
    can_native(-1)
  {}

  Table_period_info::start_end_t as_row;

  friend struct Table_scope_and_contents_source_st;
  void set_start(const LEX_CSTRING field_name)
  {
    as_row.start= field_name;
    period.start= field_name;
  }
  void set_end(const LEX_CSTRING field_name)
  {
    as_row.end= field_name;
    period.end= field_name;
  }

protected:
  bool is_start(const char *name) const;
  bool is_end(const char *name) const;
  bool is_start(const Create_field &f) const;
  bool is_end(const Create_field &f) const;
  bool fix_implicit(THD *thd, Alter_info *alter_info);
  operator bool() const
  {
    return as_row.start || as_row.end || period.start || period.end;
  }
  bool need_check(const Alter_info *alter_info) const;
  bool check_conditions(const Lex_table_name &table_name,
                        const Lex_table_name &db) const;
  bool create_sys_field(THD *thd, const char *field_name,
                        Alter_info *alter_info, int flags);

public:
  static const Lex_ident default_start;
  static const Lex_ident default_end;

  bool fix_alter_info(THD *thd, Alter_info *alter_info,
                       HA_CREATE_INFO *create_info, TABLE *table);
  bool fix_create_like(Alter_info &alter_info, HA_CREATE_INFO &create_info,
                       TABLE_LIST &src_table, TABLE_LIST &table);
  bool check_sys_fields(const Lex_table_name &table_name,
                        const Lex_table_name &db, Alter_info *alter_info) const;

  /**
     At least one field was specified 'WITH/WITHOUT SYSTEM VERSIONING'.
     Useful for error handling.
  */
  bool versioned_fields : 1;
  bool unversioned_fields : 1;
  int can_native;
};

/**
  A helper struct for table DDL statements, e.g.:
  CREATE [OR REPLACE] [TEMPORARY]
    TABLE [IF NOT EXISTS] tbl_name table_contents_source;

  Represents a combinations of:
  1. The scope, i.e. TEMPORARY or not TEMPORARY
  2. The "table_contents_source" part of the table DDL statements,
     which can be initialized from either of these:
     - table_element_list ...      // Explicit definition (column and key list)
     - LIKE another_table_name ... // Copy structure from another table
     - [AS] SELECT ...             // Copy structure from a subquery
*/

struct Table_scope_and_contents_source_pod_st // For trivial members
{
  CHARSET_INFO *alter_table_convert_to_charset;
  LEX_CUSTRING tabledef_version;
  LEX_CUSTRING org_tabledef_version;            /* version of dropped table */
  LEX_CSTRING connect_string;
  LEX_CSTRING comment;
  LEX_CSTRING alias;
  LEX_CSTRING org_storage_engine_name, new_storage_engine_name;
  const char *password, *tablespace;
  const char *data_file_name, *index_file_name;
  ulonglong max_rows,min_rows;
  ulonglong auto_increment_value;
  ulong table_options;                  ///< HA_OPTION_ values
  ulong avg_row_length;
  ulong used_fields;
  ulong key_block_size;
  ulong expression_length;
  ulong field_check_constraints;
  /*
    number of pages to sample during
    stats estimation, if used, otherwise 0.
  */
  uint stats_sample_pages;
  uint null_bits;                       /* NULL bits at start of record */
  uint options;				/* OR of HA_CREATE_ options */
  uint merge_insert_method;
  uint extra_size;                      /* length of extra data segment */
  handlerton *db_type;
  /**
    Row type of the table definition.

    Defaults to ROW_TYPE_DEFAULT for all non-ALTER statements.
    For ALTER TABLE defaults to ROW_TYPE_NOT_USED (means "keep the current").

    Can be changed either explicitly by the parser.
    If nothing specified inherits the value of the original table (if present).
  */
  enum row_type row_type;
  enum ha_choice transactional;
  enum ha_storage_media storage_media;  ///< DEFAULT, DISK or MEMORY
  enum ha_choice page_checksum;         ///< If we have page_checksums
  engine_option_value *option_list;     ///< list of table create options
  enum_stats_auto_recalc stats_auto_recalc;
  bool varchar;                         ///< 1 if table has a VARCHAR
  bool sequence;                        // If SEQUENCE=1 was used

  List<Virtual_column_info> *check_constraint_list;

  /* the following three are only for ALTER TABLE, check_if_incompatible_data() */
  ha_table_option_struct *option_struct;           ///< structure with parsed table options
  ha_field_option_struct **fields_option_struct;   ///< array of field option structures
  ha_index_option_struct **indexes_option_struct;  ///< array of index option structures

  /* The following is used to remember the old state for CREATE OR REPLACE */
  TABLE *table;
  TABLE_LIST *pos_in_locked_tables;
  TABLE_LIST *merge_list;
  MDL_ticket *mdl_ticket;
  bool table_was_deleted;
  sequence_definition *seq_create_info;

  void init()
  {
    bzero(this, sizeof(*this));
  }
  bool tmp_table() const { return options & HA_LEX_CREATE_TMP_TABLE; }
  void use_default_db_type(THD *thd)
  {
    db_type= tmp_table() ? ha_default_tmp_handlerton(thd)
                         : ha_default_handlerton(thd);
  }

  bool versioned() const
  {
    return options & HA_VERSIONED_TABLE;
  }
};


struct Table_scope_and_contents_source_st:
         public Table_scope_and_contents_source_pod_st
{
  Vers_parse_info vers_info;
  Table_period_info period_info;

  void init()
  {
    Table_scope_and_contents_source_pod_st::init();
    vers_info= {};
    period_info= {};
  }

  bool fix_create_fields(THD *thd, Alter_info *alter_info,
                         const TABLE_LIST &create_table);
  bool fix_period_fields(THD *thd, Alter_info *alter_info);
  bool check_fields(THD *thd, Alter_info *alter_info,
                    const Lex_table_name &table_name,
                    const Lex_table_name &db,
                    int select_count= 0);
  bool check_period_fields(THD *thd, Alter_info *alter_info);

  void vers_check_native();
  bool vers_fix_system_fields(THD *thd, Alter_info *alter_info,
                              const TABLE_LIST &create_table);

  bool vers_check_system_fields(THD *thd, Alter_info *alter_info,
                                const Lex_table_name &table_name,
                                const Lex_table_name &db,
                                int select_count= 0);
};


/**
  This struct is passed to handler table routines, e.g. ha_create().
  It does not include the "OR REPLACE" and "IF NOT EXISTS" parts, as these
  parts are handled on the SQL level and are not needed on the handler level.
*/
struct HA_CREATE_INFO: public Table_scope_and_contents_source_st,
                       public Schema_specification_st
{
  /* TODO: remove after MDEV-20865 */
  Alter_info *alter_info;

  void init()
  {
    Table_scope_and_contents_source_st::init();
    Schema_specification_st::init();
    alter_info= NULL;
  }
  ulong table_options_with_row_type()
  {
    if (row_type == ROW_TYPE_DYNAMIC || row_type == ROW_TYPE_PAGE)
      return table_options | HA_OPTION_PACK_RECORD;
    else
      return table_options;
  }
  bool resolve_to_charset_collation_context(THD *thd,
                  const Lex_table_charset_collation_attrs_st &default_cscl,
                  const Lex_table_charset_collation_attrs_st &convert_cscl,
                  const Charset_collation_context &ctx);
};


/**
  This struct is passed to mysql_create_table() and similar creation functions,
  as well as to show_create_table().
*/
struct Table_specification_st: public HA_CREATE_INFO,
                               public DDL_options_st
{
  Lex_table_charset_collation_attrs_st default_charset_collation;
  Lex_table_charset_collation_attrs_st convert_charset_collation;

  // Deep initialization
  void init()
  {
    HA_CREATE_INFO::init();
    DDL_options_st::init();
    default_charset_collation.init();
    convert_charset_collation.init();
  }
  void init(DDL_options_st::Options options_arg)
  {
    HA_CREATE_INFO::init();
    DDL_options_st::init(options_arg);
    default_charset_collation.init();
    convert_charset_collation.init();
  }
  /*
    Quick initialization, for parser.
    Most of the HA_CREATE_INFO is left uninitialized.
    It gets fully initialized in sql_yacc.yy, only when the parser
    scans a related keyword (e.g. CREATE, ALTER).
  */
  void lex_start()
  {
    HA_CREATE_INFO::options= 0;
    DDL_options_st::init();
    default_charset_collation.init();
    convert_charset_collation.init();
  }

  bool add_table_option_convert_charset(Sql_used *used,
                                        const Charset_collation_map_st &map,
                                        CHARSET_INFO *cs)
  {
    // cs can be NULL, e.g.: ALTER TABLE t1 CONVERT TO CHARACTER SET DEFAULT;
    used_fields|= (HA_CREATE_USED_CHARSET | HA_CREATE_USED_DEFAULT_CHARSET);
    return cs ?
      convert_charset_collation.merge_exact_charset(used, map,
                                                    Lex_exact_charset(cs)) :
      convert_charset_collation.merge_charset_default();
  }
  bool add_table_option_convert_collation(Sql_used *used,
                                          const Charset_collation_map_st &map,
                                          const Lex_extended_collation_st &cl)
  {
    used_fields|= (HA_CREATE_USED_CHARSET | HA_CREATE_USED_DEFAULT_CHARSET);
    return convert_charset_collation.merge_collation(used, map, cl);
  }

  bool add_table_option_default_charset(Sql_used *used,
                                        const Charset_collation_map_st &map,
                                        CHARSET_INFO *cs)
  {
    // cs can be NULL, e.g.:  CREATE TABLE t1 (..) CHARACTER SET DEFAULT;
    used_fields|= HA_CREATE_USED_DEFAULT_CHARSET;
    return cs ?
      default_charset_collation.merge_exact_charset(used, map,
                                                    Lex_exact_charset(cs)) :
      default_charset_collation.merge_charset_default();
  }
  bool add_table_option_default_collation(Sql_used *used,
                                          const Charset_collation_map_st &map,
                                          const Lex_extended_collation_st &cl)
  {
    used_fields|= HA_CREATE_USED_DEFAULT_CHARSET;
    return default_charset_collation.merge_collation(used, map, cl);
  }

  bool resolve_to_charset_collation_context(THD *thd,
                                         const Charset_collation_context &ctx)
  {
    return HA_CREATE_INFO::
             resolve_to_charset_collation_context(thd,
                                                  default_charset_collation,
                                                  convert_charset_collation,
                                                  ctx);
  }
};


/**
  Structure describing changes to an index to be caused by ALTER TABLE.
*/

struct KEY_PAIR
{
  /**
    Pointer to KEY object describing old version of index in
    TABLE::key_info array for TABLE instance representing old
    version of table.
  */
  KEY *old_key;
  /**
    Pointer to KEY object describing new version of index in
    Alter_inplace_info::key_info_buffer array.
  */
  KEY *new_key;
};


/**
  In-place alter handler context.

  This is a superclass intended to be subclassed by individual handlers
  in order to store handler unique context between in-place alter API calls.

  The handler is responsible for creating the object. This can be done
  as early as during check_if_supported_inplace_alter().

  The SQL layer is responsible for destroying the object.
  The class extends Sql_alloc so the memory will be mem root allocated.

  @see Alter_inplace_info
*/

class inplace_alter_handler_ctx : public Sql_alloc
{
public:
  inplace_alter_handler_ctx() = default;

  virtual ~inplace_alter_handler_ctx() = default;
  virtual void set_shared_data(const inplace_alter_handler_ctx& ctx) {}
};


/**
  Class describing changes to be done by ALTER TABLE.
  Instance of this class is passed to storage engine in order
  to determine if this ALTER TABLE can be done using in-place
  algorithm. It is also used for executing the ALTER TABLE
  using in-place algorithm.
*/

class Alter_inplace_info
{
public:

  /**
    Create options (like MAX_ROWS) for the new version of table.

    @note The referenced instance of HA_CREATE_INFO object was already
          used to create new .FRM file for table being altered. So it
          has been processed by mysql_prepare_create_table() already.
          For example, this means that it has HA_OPTION_PACK_RECORD
          flag in HA_CREATE_INFO::table_options member correctly set.
  */
  HA_CREATE_INFO *create_info;

  /**
    Alter options, fields and keys for the new version of table.

    @note The referenced instance of Alter_info object was already
          used to create new .FRM file for table being altered. So it
          has been processed by mysql_prepare_create_table() already.
          In particular, this means that in Create_field objects for
          fields which were present in some form in the old version
          of table, Create_field::field member points to corresponding
          Field instance for old version of table.
  */
  Alter_info *alter_info;

  /**
    Array of KEYs for new version of table - including KEYs to be added.

    @note Currently this array is produced as result of
          mysql_prepare_create_table() call.
          This means that it follows different convention for
          KEY_PART_INFO::fieldnr values than objects in TABLE::key_info
          array.

    @todo This is mainly due to the fact that we need to keep compatibility
          with removed handler::add_index() call. We plan to switch to
          TABLE::key_info numbering later.

    KEYs are sorted - see sort_keys().
  */
  KEY  *key_info_buffer;

  /** Size of key_info_buffer array. */
  uint key_count;

  /** Size of index_drop_buffer array. */
  uint index_drop_count= 0;

  /**
     Array of pointers to KEYs to be dropped belonging to the TABLE instance
     for the old version of the table.
  */
  KEY  **index_drop_buffer= nullptr;

  /** Size of index_add_buffer array. */
  uint index_add_count= 0;

  /**
     Array of indexes into key_info_buffer for KEYs to be added,
     sorted in increasing order.
  */
  uint *index_add_buffer= nullptr;

  KEY_PAIR  *index_altered_ignorability_buffer= nullptr;

  /** Size of index_altered_ignorability_buffer array. */
  uint index_altered_ignorability_count= 0;

  /**
     Old and new index names. Used for index rename.
  */
  struct Rename_key_pair
  {
    Rename_key_pair(const KEY *old_key, const KEY *new_key)
        : old_key(old_key), new_key(new_key)
    {
    }
    const KEY *old_key;
    const KEY *new_key;
  };
  /**
     Vector of key pairs from DROP/ADD index which can be renamed.
  */
  typedef Mem_root_array<Rename_key_pair, true> Rename_keys_vector;

  /**
     A list of indexes which should be renamed.
     Index definitions stays the same.
  */
  Rename_keys_vector rename_keys;

  /**
     Context information to allow handlers to keep context between in-place
     alter API calls.

     @see inplace_alter_handler_ctx for information about object lifecycle.
  */
  inplace_alter_handler_ctx *handler_ctx= nullptr;

  /**
    If the table uses several handlers, like ha_partition uses one handler
    per partition, this contains a Null terminated array of ctx pointers
    that should all be committed together.
    Or NULL if only handler_ctx should be committed.
    Set to NULL if the low level handler::commit_inplace_alter_table uses it,
    to signal to the main handler that everything was committed as atomically.

    @see inplace_alter_handler_ctx for information about object lifecycle.
  */
  inplace_alter_handler_ctx **group_commit_ctx= nullptr;

  /**
     Flags describing in detail which operations the storage engine is to
     execute. Flags are defined in sql_alter.h
  */
  alter_table_operations handler_flags= 0;

  /* Alter operations involving parititons are strored here */
  ulong partition_flags;

  /**
     Partition_info taking into account the partition changes to be performed.
     Contains all partitions which are present in the old version of the table
     with partitions to be dropped or changed marked as such + all partitions
     to be added in the new version of table marked as such.
  */
  partition_info * const modified_part_info;

  /** true for ALTER IGNORE TABLE ... */
  const bool ignore;

  /** true for online operation (LOCK=NONE) */
  bool online= false;

  /**
    When ha_commit_inplace_alter_table() is called the the engine can
    set this to a function to be called after the ddl log
    is committed.
  */
  typedef void (inplace_alter_table_commit_callback)(void *);
  inplace_alter_table_commit_callback *inplace_alter_table_committed= nullptr;

  /* This will be used as the argument to the above function when called */
  void *inplace_alter_table_committed_argument= nullptr;

  /** which ALGORITHM and LOCK are supported by the storage engine */
  enum_alter_inplace_result inplace_supported;

  /**
     Can be set by handler to describe why a given operation cannot be done
     in-place (HA_ALTER_INPLACE_NOT_SUPPORTED) or why it cannot be done
     online (HA_ALTER_INPLACE_NO_LOCK or HA_ALTER_INPLACE_COPY_NO_LOCK)
     If set, it will be used with ER_ALTER_OPERATION_NOT_SUPPORTED_REASON if
     results from handler::check_if_supported_inplace_alter() doesn't match
     requirements set by user. If not set, the more generic
     ER_ALTER_OPERATION_NOT_SUPPORTED will be used.

     Please set to a properly localized string, for example using
     my_get_err_msg(), so that the error message as a whole is localized.
  */
  const char *unsupported_reason= nullptr;

  /** true when InnoDB should abort the alter when table is not empty */
  const bool error_if_not_empty;

  /** True when DDL should avoid downgrading the MDL */
  bool mdl_exclusive_after_prepare= false;

  Alter_inplace_info(HA_CREATE_INFO *create_info_arg,
                     Alter_info *alter_info_arg,
                     KEY *key_info_arg, uint key_count_arg,
                     partition_info *modified_part_info_arg,
                     bool ignore_arg, bool error_non_empty);

  ~Alter_inplace_info()
  {
    delete handler_ctx;
  }

  /**
    Used after check_if_supported_inplace_alter() to report
    error if the result does not match the LOCK/ALGORITHM
    requirements set by the user.

    @param not_supported  Part of statement that was not supported.
    @param try_instead    Suggestion as to what the user should
                          replace not_supported with.
  */
  void report_unsupported_error(const char *not_supported,
                                const char *try_instead) const;
 void add_altered_index_ignorability(KEY *old_key, KEY *new_key)
 {
   KEY_PAIR *key_pair= index_altered_ignorability_buffer +
                       index_altered_ignorability_count++;
   key_pair->old_key= old_key;
   key_pair->new_key= new_key;
   DBUG_PRINT("info", ("index had ignorability altered: %i to %i",
                      old_key->is_ignored,
                      new_key->is_ignored));
 }


};


typedef struct st_key_create_information
{
  enum ha_key_alg algorithm;
  ulong block_size;
  uint flags;                                   /* HA_USE.. flags */
  LEX_CSTRING parser_name;
  LEX_CSTRING comment;
  bool is_ignored;
} KEY_CREATE_INFO;


typedef struct st_savepoint SAVEPOINT;
extern ulong savepoint_alloc_size;
extern KEY_CREATE_INFO default_key_create_info;

/* Forward declaration for condition pushdown to storage engine */
typedef class Item COND;

typedef struct st_ha_check_opt
{
  st_ha_check_opt() = default;                        /* Remove gcc warning */
  uint flags;       /* isam layer flags (e.g. for myisamchk) */
  uint sql_flags;   /* sql layer flags - for something myisamchk cannot do */
  time_t start_time;   /* When check/repair starts */
  KEY_CACHE *key_cache; /* new key cache when changing key cache */
  void init();
} HA_CHECK_OPT;


/********************************************************************************
 * MRR
 ********************************************************************************/

typedef void *range_seq_t;

typedef struct st_range_seq_if
{
  /*
    Get key information
 
    SYNOPSIS
      get_key_info()
        init_params  The seq_init_param parameter 
        length       OUT length of the keys in this range sequence
        map          OUT key_part_map of the keys in this range sequence

    DESCRIPTION
      This function is set only when using HA_MRR_FIXED_KEY mode. In that mode, 
      all ranges are single-point equality ranges that use the same set of key
      parts. This function allows the MRR implementation to get the length of
      a key, and which keyparts it uses.
  */
  void (*get_key_info)(void *init_params, uint *length, key_part_map *map);

  /*
    Initialize the traversal of range sequence
    
    SYNOPSIS
      init()
        init_params  The seq_init_param parameter 
        n_ranges     The number of ranges obtained 
        flags        A combination of HA_MRR_SINGLE_POINT, HA_MRR_FIXED_KEY

    RETURN
      An opaque value to be used as RANGE_SEQ_IF::next() parameter
  */
  range_seq_t (*init)(void *init_params, uint n_ranges, uint flags);


  /*
    Get the next range in the range sequence

    SYNOPSIS
      next()
        seq    The value returned by RANGE_SEQ_IF::init()
        range  OUT Information about the next range
    
    RETURN
      FALSE - Ok, the range structure filled with info about the next range
      TRUE  - No more ranges
  */
  bool (*next) (range_seq_t seq, KEY_MULTI_RANGE *range);

  /*
    Check whether range_info orders to skip the next record

    SYNOPSIS
      skip_record()
        seq         The value returned by RANGE_SEQ_IF::init()
        range_info  Information about the next range 
                    (Ignored if MRR_NO_ASSOCIATION is set)
        rowid       Rowid of the record to be checked (ignored if set to 0)
    
    RETURN
      1 - Record with this range_info and/or this rowid shall be filtered
          out from the stream of records returned by multi_range_read_next()
      0 - The record shall be left in the stream
  */ 
  bool (*skip_record) (range_seq_t seq, range_id_t range_info, uchar *rowid);

  /*
    Check if the record combination matches the index condition
    SYNOPSIS
      skip_index_tuple()
        seq         The value returned by RANGE_SEQ_IF::init()
        range_info  Information about the next range 
    
    RETURN
      0 - The record combination satisfies the index condition
      1 - Otherwise
  */ 
  bool (*skip_index_tuple) (range_seq_t seq, range_id_t range_info);
} RANGE_SEQ_IF;

typedef bool (*SKIP_INDEX_TUPLE_FUNC) (range_seq_t seq, range_id_t range_info);

#define MARIADB_NEW_COST_MODEL 1
/* Separated costs for IO and CPU */

struct IO_AND_CPU_COST
{
  double io;
  double cpu;

  void add(IO_AND_CPU_COST cost)
  {
    io+= cost.io;
    cpu+= cost.cpu;
  }
};

/* Cost for reading a row through an index */
struct ALL_READ_COST
{
  IO_AND_CPU_COST index_cost, row_cost;
  longlong max_index_blocks, max_row_blocks;
  /* index_only_read = index_cost + copy_cost */
  double   copy_cost;

  void reset()
  {
    row_cost= {0,0};
    index_cost= {0,0};
    max_index_blocks= max_row_blocks= 0;
    copy_cost= 0.0;
  }
};


class Cost_estimate
{ 
public:
  double avg_io_cost;     /* cost of an average I/O oper. to fetch records */
  double cpu_cost;        /* Cpu cost unrelated to engine costs */
  double comp_cost;       /* Cost of comparing found rows with WHERE clause */
  double copy_cost;       /* Copying the data to 'record' */
  double limit_cost;      /* Total cost when restricting rows with limit */
  double setup_cost;      /* MULTI_RANGE_READ_SETUP_COST or similar */
  IO_AND_CPU_COST index_cost;
  IO_AND_CPU_COST row_cost;

  Cost_estimate()
  {
    reset();
  }

  /*
    Total cost for the range
    Note that find_cost() + compare_cost() + data_copy_cost() == total_cost()
  */

  double total_cost() const
  {
    return ((index_cost.io + row_cost.io) * avg_io_cost+
            index_cost.cpu + row_cost.cpu + copy_cost +
            comp_cost + cpu_cost + setup_cost);
  }

  /* Cost for just fetching and copying a row (no compare costs) */
  double fetch_cost() const
  {
    return ((index_cost.io + row_cost.io) * avg_io_cost+
            index_cost.cpu + row_cost.cpu + copy_cost);
  }

  /*
    Cost of copying the row or key to 'record'
  */
  inline double data_copy_cost() const
  {
    return copy_cost;
  }

  /*
    Multiply costs to simulate a scan where we read
    We assume that io blocks will be cached and we only
    allocate memory once. There should also be no import_cost
    that needs to be done multiple times
  */
  void multiply(uint n)
  {
    index_cost.io*=  n;
    index_cost.cpu*= n;
    row_cost.io*=    n;
    row_cost.cpu*=   n;
    copy_cost*=      n;
    comp_cost*=      n;
    cpu_cost*=       n;
  }

  void add(Cost_estimate *cost)
  {
    avg_io_cost=     cost->avg_io_cost;
    index_cost.io+=  cost->index_cost.io;
    index_cost.cpu+= cost->index_cost.cpu;
    row_cost.io+=    cost->row_cost.io;
    row_cost.cpu+=   cost->row_cost.cpu;
    copy_cost+=      cost->copy_cost;
    comp_cost+=      cost->comp_cost;
    cpu_cost+=       cost->cpu_cost;
    setup_cost+=     cost->setup_cost;
  }

  inline void reset()
  {
    avg_io_cost= 0;
    comp_cost= cpu_cost= 0.0;
    copy_cost= limit_cost= 0.0;
    setup_cost= 0.0;
    index_cost= {0,0};
    row_cost=   {0,0};
  }
  inline void reset(handler *file);

  /*
    To be used when we go from old single value-based cost calculations to
    the new Cost_estimate-based.
  */
  void convert_from_cost(double cost)
  {
    reset();
    cpu_cost= cost;
  }
};

/*
  Indicates that all scanned ranges will be singlepoint (aka equality) ranges.
  The ranges may not use the full key but all of them will use the same number
  of key parts.
*/
#define HA_MRR_SINGLE_POINT 1U
#define HA_MRR_FIXED_KEY  2U

/* 
  Indicates that RANGE_SEQ_IF::next(&range) doesn't need to fill in the
  'range' parameter.
*/
#define HA_MRR_NO_ASSOCIATION 4U

/* 
  The MRR user will provide ranges in key order, and MRR implementation
  must return rows in key order.
*/
#define HA_MRR_SORTED 8U

/* MRR implementation doesn't have to retrieve full records */
#define HA_MRR_INDEX_ONLY 16U

/* 
  The passed memory buffer is of maximum possible size, the caller can't
  assume larger buffer.
*/
#define HA_MRR_LIMITS 32U


/*
  Flag set <=> default MRR implementation is used
  (The choice is made by **_info[_const]() function which may set this
   flag. SQL layer remembers the flag value and then passes it to
   multi_read_range_init().
*/
#define HA_MRR_USE_DEFAULT_IMPL 64U

/*
  Used only as parameter to multi_range_read_info():
  Flag set <=> the caller guarantees that the bounds of the scanned ranges
  will not have NULL values.
*/
#define HA_MRR_NO_NULL_ENDPOINTS 128U

/*
  The MRR user has materialized range keys somewhere in the user's buffer.
  This can be used for optimization of the procedure that sorts these keys
  since in this case key values don't have to be copied into the MRR buffer.

  In other words, it is guaranteed that after RANGE_SEQ_IF::next() call the 
  pointer in range->start_key.key will point to a key value that will remain 
  there until the end of the MRR scan.
*/
#define HA_MRR_MATERIALIZED_KEYS 256U

/*
  The following bits are reserved for use by MRR implementation. The intended
  use scenario:

  * sql layer calls handler->multi_range_read_info[_const]() 
    - MRR implementation figures out what kind of scan it will perform, saves
      the result in *mrr_mode parameter.
  * sql layer remembers what was returned in *mrr_mode

  * the optimizer picks the query plan (which may or may not include the MRR 
    scan that was estimated by the multi_range_read_info[_const] call)

  * if the query is an EXPLAIN statement, sql layer will call 
    handler->multi_range_read_explain_info(mrr_mode) to get a text description
    of the picked MRR scan; the description will be a part of EXPLAIN output.
*/
#define HA_MRR_IMPLEMENTATION_FLAG1 512U
#define HA_MRR_IMPLEMENTATION_FLAG2 1024U
#define HA_MRR_IMPLEMENTATION_FLAG3 2048U
#define HA_MRR_IMPLEMENTATION_FLAG4 4096U
#define HA_MRR_IMPLEMENTATION_FLAG5 8192U
#define HA_MRR_IMPLEMENTATION_FLAG6 16384U

#define HA_MRR_IMPLEMENTATION_FLAGS \
  (512U | 1024U | 2048U | 4096U | 8192U | 16384U)

/*
  This is a buffer area that the handler can use to store rows.
  'end_of_used_area' should be kept updated after calls to
  read-functions so that other parts of the code can use the
  remaining area (until next read calls is issued).
*/

typedef struct st_handler_buffer
{
  /* const? */uchar *buffer;         /* Buffer one can start using */
  /* const? */uchar *buffer_end;     /* End of buffer */
  uchar *end_of_used_area;     /* End of area that was used by handler */
} HANDLER_BUFFER;

typedef struct system_status_var SSV;

class ha_statistics
{
public:
  ulonglong data_file_length;		/* Length off data file */
  ulonglong max_data_file_length;	/* Length off data file */
  ulonglong index_file_length;
  ulonglong max_index_file_length;
  ulonglong delete_length;		/* Free bytes */
  ulonglong auto_increment_value;
  /*
    The number of records in the table. 
      0    - means the table has exactly 0 rows
    other  - if (table_flags() & HA_STATS_RECORDS_IS_EXACT)
               the value is the exact number of records in the table
             else
               it is an estimate
  */
  ha_rows records;
  ha_rows deleted;			/* Deleted records */
  ulong mean_rec_length;		/* physical reclength */
  time_t create_time;			/* When table was created */
  time_t check_time;
  time_t update_time;
  uint block_size;			/* index block size */
  ha_checksum checksum;
  bool checksum_null;

  /*
    number of buffer bytes that native mrr implementation needs,
  */
  uint mrr_length_per_rec; 

  ha_statistics():
    data_file_length(0), max_data_file_length(0),
    index_file_length(0), max_index_file_length(0), delete_length(0),
    auto_increment_value(0), records(0), deleted(0), mean_rec_length(0),
    create_time(0), check_time(0), update_time(0), block_size(8192),
    checksum(0), checksum_null(FALSE), mrr_length_per_rec(0)
  {}
};

extern "C" check_result_t handler_index_cond_check(void* h_arg);

extern "C" check_result_t handler_rowid_filter_check(void* h_arg);
extern "C" int handler_rowid_filter_is_active(void* h_arg);

uint calculate_key_len(TABLE *, uint, const uchar *, key_part_map);
/*
  bitmap with first N+1 bits set
  (keypart_map for a key prefix of [0..N] keyparts)
*/
#define make_keypart_map(N) (((key_part_map)2 << (N)) - 1)
/*
  bitmap with first N bits set
  (keypart_map for a key prefix of [0..N-1] keyparts)
*/
#define make_prev_keypart_map(N) (((key_part_map)1 << (N)) - 1)


/** Base class to be used by handlers different shares */
class Handler_share
{
public:
  Handler_share() = default;
  virtual ~Handler_share() = default;
};

enum class Compare_keys : uint32_t
{
  Equal= 0,
  EqualButKeyPartLength,
  EqualButComment,
  NotEqual
};


/**
  The handler class is the interface for dynamically loadable
  storage engines. Do not add ifdefs and take care when adding or
  changing virtual functions to avoid vtable confusion

  Functions in this class accept and return table columns data. Two data
  representation formats are used:
  1. TableRecordFormat - Used to pass [partial] table records to/from
     storage engine

  2. KeyTupleFormat - used to pass index search tuples (aka "keys") to
     storage engine. See opt_range.cc for description of this format.

  TableRecordFormat
  =================
  [Warning: this description is work in progress and may be incomplete]
  The table record is stored in a fixed-size buffer:
   
    record: null_bytes, column1_data, column2_data, ...
  
  The offsets of the parts of the buffer are also fixed: every column has 
  an offset to its column{i}_data, and if it is nullable it also has its own
  bit in null_bytes. 

  The record buffer only includes data about columns that are marked in the
  relevant column set (table->read_set and/or table->write_set, depending on
  the situation). 
  <not-sure>It could be that it is required that null bits of non-present
  columns are set to 1</not-sure>

  VARIOUS EXCEPTIONS AND SPECIAL CASES

  If the table has no nullable columns, then null_bytes is still 
  present, its length is one byte <not-sure> which must be set to 0xFF 
  at all times. </not-sure>
  
  If the table has columns of type BIT, then certain bits from those columns
  may be stored in null_bytes as well. Grep around for Field_bit for
  details.

  For blob columns (see Field_blob), the record buffer stores length of the 
  data, following by memory pointer to the blob data. The pointer is owned 
  by the storage engine and is valid until the next operation.

  If a blob column has NULL value, then its length and blob data pointer
  must be set to 0.
*/

class handler :public Sql_alloc
{
public:
  typedef ulonglong Table_flags;
protected:
  TABLE_SHARE *table_share;   /* The table definition */
  TABLE *table;               /* The current open table */
  Table_flags cached_table_flags;       /* Set on init() and open() */

  ha_rows estimation_rows_to_insert;
  handler *lookup_handler;
  /* Statistics for the query. Updated if handler_stats.in_use is set */
  ha_handler_stats active_handler_stats;
  void set_handler_stats();
public:
  handlerton *ht;               /* storage engine of this handler */
  OPTIMIZER_COSTS *costs;       /* Points to table->share->costs */
  uchar *ref;			/* Pointer to current row */
  uchar *dup_ref;		/* Pointer to duplicate row */
  uchar *lookup_buffer;

  /* General statistics for the table like number of row, file sizes etc */
  ha_statistics stats;
  /*
    Collect query stats here if pointer is != NULL.
    This is a pointer because if we do a clone of the handler, we want to
    use the original handler for collecting statistics.
  */
  ha_handler_stats *handler_stats;

  /** MultiRangeRead-related members: */
  range_seq_t mrr_iter;    /* Iterator to traverse the range sequence */
  RANGE_SEQ_IF mrr_funcs;  /* Range sequence traversal functions */
  HANDLER_BUFFER *multi_range_buffer; /* MRR buffer info */
  uint ranges_in_seq; /* Total number of ranges in the traversed sequence */
  /** Current range (the one we're now returning rows from) */

  KEY_MULTI_RANGE mrr_cur_range;

  /** The following are for read_range() */
  key_range save_end_range, *end_range;
  KEY_PART_INFO *range_key_part;
  int key_compare_result_on_equal;

  /* TRUE <=> source MRR ranges and the output are ordered */
  bool mrr_is_output_sorted;
  /** TRUE <=> we're currently traversing a range in mrr_cur_range. */
  bool mrr_have_range;
  bool eq_range;
  bool internal_tmp_table;                 /* If internal tmp table */
  bool implicit_emptied;                   /* Can be !=0 only if HEAP */
  bool mark_trx_read_write_done;           /* mark_trx_read_write was called */
  bool check_table_binlog_row_based_done; /* check_table_binlog.. was called */
  bool check_table_binlog_row_based_result; /* cached check_table_binlog... */
  /* 
    TRUE <=> the engine guarantees that returned records are within the range
    being scanned.
  */
  bool in_range_check_pushed_down;

  uint lookup_errkey;
  uint errkey;                             /* Last dup key */
  uint key_used_on_scan;
  uint active_index, keyread;

  /** Length of ref (1-8 or the clustered key length) */
  uint ref_length;
  FT_INFO *ft_handler;
  enum init_stat { NONE=0, INDEX, RND };
  init_stat inited, pre_inited;

  const COND *pushed_cond;
  /**
    next_insert_id is the next value which should be inserted into the
    auto_increment column: in a inserting-multi-row statement (like INSERT
    SELECT), for the first row where the autoinc value is not specified by the
    statement, get_auto_increment() called and asked to generate a value,
    next_insert_id is set to the next value, then for all other rows
    next_insert_id is used (and increased each time) without calling
    get_auto_increment().
  */
  ulonglong next_insert_id;
  /**
    insert id for the current row (*autogenerated*; if not
    autogenerated, it's 0).
    At first successful insertion, this variable is stored into
    THD::first_successful_insert_id_in_cur_stmt.
  */
  ulonglong insert_id_for_cur_row;
  /**
    Interval returned by get_auto_increment() and being consumed by the
    inserter.
  */
  /* Statistics  variables */
  ulonglong rows_read;
  ulonglong rows_tmp_read;
  ulonglong rows_changed;
  /* One bigger than needed to avoid to test if key == MAX_KEY */
  ulonglong index_rows_read[MAX_KEY+1];
  ha_copy_info copy_info;

private:
  /* ANALYZE time tracker, if present */
  Exec_time_tracker *tracker;
public:
  void set_time_tracker(Exec_time_tracker *tracker_arg) { tracker=tracker_arg;}
  Exec_time_tracker *get_time_tracker() { return tracker; }

  Item *pushed_idx_cond;
  uint pushed_idx_cond_keyno;  /* The index which the above condition is for */

  /* Rowid filter pushed into the engine */
  Rowid_filter *pushed_rowid_filter;
  /* true when the pushed rowid filter has been already filled */
  bool rowid_filter_is_active;
  /* Used for disabling/enabling pushed_rowid_filter */
  Rowid_filter *save_pushed_rowid_filter;
  bool save_rowid_filter_is_active;

  Discrete_interval auto_inc_interval_for_cur_row;
  /**
     Number of reserved auto-increment intervals. Serves as a heuristic
     when we have no estimation of how many records the statement will insert:
     the more intervals we have reserved, the bigger the next one. Reset in
     handler::ha_release_auto_increment().
  */
  uint auto_inc_intervals_count;

  /**
    Instrumented table associated with this handler.
    This member should be set to NULL when no instrumentation is in place,
    so that linking an instrumented/non instrumented server/plugin works.
    For example:
    - the server is compiled with the instrumentation.
    The server expects either NULL or valid pointers in m_psi.
    - an engine plugin is compiled without instrumentation.
    The plugin can not leave this pointer uninitialized,
    or can not leave a trash value on purpose in this pointer,
    as this would crash the server.
  */
  PSI_table *m_psi;

private:
  /** Internal state of the batch instrumentation. */
  enum batch_mode_t
  {
    /** Batch mode not used. */
    PSI_BATCH_MODE_NONE,
    /** Batch mode used, before first table io. */
    PSI_BATCH_MODE_STARTING,
    /** Batch mode used, after first table io. */
    PSI_BATCH_MODE_STARTED
  };
  /**
    Batch mode state.
    @sa start_psi_batch_mode.
    @sa end_psi_batch_mode.
  */
  batch_mode_t m_psi_batch_mode;
  /**
    The number of rows in the batch.
    @sa start_psi_batch_mode.
    @sa end_psi_batch_mode.
  */
  ulonglong m_psi_numrows;
  /**
    The current event in a batch.
    @sa start_psi_batch_mode.
    @sa end_psi_batch_mode.
  */
  PSI_table_locker *m_psi_locker;
  /**
    Storage for the event in a batch.
    @sa start_psi_batch_mode.
    @sa end_psi_batch_mode.
  */
  PSI_table_locker_state m_psi_locker_state;

public:
  virtual void unbind_psi();
  virtual void rebind_psi();
  /* Return error if definition doesn't match for already opened table */
  virtual int discover_check_version() { return 0; }

  /**
    Put the handler in 'batch' mode when collecting
    table io instrumented events.
    When operating in batch mode:
    - a single start event is generated in the performance schema.
    - all table io performed between @c start_psi_batch_mode
      and @c end_psi_batch_mode is not instrumented:
      the number of rows affected is counted instead in @c m_psi_numrows.
    - a single end event is generated in the performance schema
      when the batch mode ends with @c end_psi_batch_mode.
  */
  void start_psi_batch_mode();
  /** End a batch started with @c start_psi_batch_mode. */
  void end_psi_batch_mode();

  /* If we have row logging enabled for this table */
  bool row_logging, row_logging_init;
  /* If the row logging should be done in transaction cache */
  bool row_logging_has_trans;

private:
  /**
    The lock type set by when calling::ha_external_lock(). This is 
    propagated down to the storage engine. The reason for also storing 
    it here, is that when doing MRR we need to create/clone a second handler
    object. This cloned handler object needs to know about the lock_type used.
  */
  int m_lock_type;
  /**
    Pointer where to store/retrieve the Handler_share pointer.
    For non partitioned handlers this is &TABLE_SHARE::ha_share.
  */
  Handler_share **ha_share;
public:

  double optimizer_where_cost;          // Copy of THD->...optimzer_where_cost
  double optimizer_scan_setup_cost;     // Copy of THD->...optimzer_scan_...

  handler(handlerton *ht_arg, TABLE_SHARE *share_arg)
    :table_share(share_arg), table(0),
    estimation_rows_to_insert(0),
    lookup_handler(this),
    ht(ht_arg), costs(0), ref(0), lookup_buffer(NULL), handler_stats(NULL),
    end_range(NULL), implicit_emptied(0),
    mark_trx_read_write_done(0),
    check_table_binlog_row_based_done(0),
    check_table_binlog_row_based_result(0),
    in_range_check_pushed_down(FALSE), lookup_errkey(-1), errkey(-1),
    key_used_on_scan(MAX_KEY),
    active_index(MAX_KEY), keyread(MAX_KEY),
    ref_length(sizeof(my_off_t)),
    ft_handler(0), inited(NONE), pre_inited(NONE),
    pushed_cond(0), next_insert_id(0), insert_id_for_cur_row(0),
    tracker(NULL),
    pushed_idx_cond(NULL),
    pushed_idx_cond_keyno(MAX_KEY),
    pushed_rowid_filter(NULL),
    rowid_filter_is_active(0),
    save_pushed_rowid_filter(NULL),
    save_rowid_filter_is_active(false),
    auto_inc_intervals_count(0),
    m_psi(NULL),
    m_psi_batch_mode(PSI_BATCH_MODE_NONE),
    m_psi_numrows(0),
    m_psi_locker(NULL),
    row_logging(0), row_logging_init(0),
    m_lock_type(F_UNLCK), ha_share(NULL), optimizer_where_cost(0),
    optimizer_scan_setup_cost(0)
  {
    DBUG_PRINT("info",
               ("handler created F_UNLCK %d F_RDLCK %d F_WRLCK %d",
                F_UNLCK, F_RDLCK, F_WRLCK));
    reset_statistics();
    /*
      The following variables should be updated in set_optimizer_costs()
      which is to be run as part of setting up the table for the query
    */
    MEM_UNDEFINED(&optimizer_where_cost, sizeof(optimizer_where_cost));
    MEM_UNDEFINED(&optimizer_scan_setup_cost, sizeof(optimizer_scan_setup_cost));
  }
  virtual ~handler(void)
  {
    DBUG_ASSERT(m_lock_type == F_UNLCK);
    DBUG_ASSERT(inited == NONE);
  }
  /* To check if table has been properely opened */
  bool is_open()
  {
    return ref != 0;
  }
  virtual handler *clone(const char *name, MEM_ROOT *mem_root);
  /** This is called after create to allow us to set up cached variables */
  void init()
  {
    cached_table_flags= table_flags();
  }
  /* ha_ methods: public wrappers for private virtual API */
  
  int ha_open(TABLE *table, const char *name, int mode, uint test_if_locked,
              MEM_ROOT *mem_root= 0, List<String> *partitions_to_open=NULL);
  int ha_index_init(uint idx, bool sorted)
  {
    DBUG_EXECUTE_IF("ha_index_init_fail", return HA_ERR_TABLE_DEF_CHANGED;);
    int result;
    DBUG_ENTER("ha_index_init");
    DBUG_ASSERT(inited==NONE);
    if (!(result= index_init(idx, sorted)))
    {
      inited=       INDEX;
      active_index= idx;
      end_range= NULL;
    }
    DBUG_RETURN(result);
  }
  int ha_index_end()
  {
    DBUG_ENTER("ha_index_end");
    DBUG_ASSERT(inited==INDEX);
    inited=       NONE;
    active_index= MAX_KEY;
    end_range=    NULL;
    DBUG_RETURN(index_end());
  }
  /* This is called after index_init() if we need to do a index scan */
  virtual int prepare_index_scan() { return 0; }
  virtual int prepare_index_key_scan_map(const uchar * key, key_part_map keypart_map)
  {
    uint key_len= calculate_key_len(table, active_index, key, keypart_map);
    return  prepare_index_key_scan(key, key_len);
  }
  virtual int prepare_index_key_scan( const uchar * key, uint key_len )
  { return 0; }
  virtual int prepare_range_scan(const key_range *start_key, const key_range *end_key)
  { return 0; }

  int ha_rnd_init(bool scan) __attribute__ ((warn_unused_result))
  {
    DBUG_EXECUTE_IF("ha_rnd_init_fail", return HA_ERR_TABLE_DEF_CHANGED;);
    int result;
    DBUG_ENTER("ha_rnd_init");
    DBUG_ASSERT(inited==NONE || (inited==RND && scan));
    inited= (result= rnd_init(scan)) ? NONE: RND;
    end_range= NULL;
    DBUG_RETURN(result);
  }
  int ha_rnd_end()
  {
    DBUG_ENTER("ha_rnd_end");
    DBUG_ASSERT(inited==RND);
    inited=NONE;
    end_range= NULL;
    DBUG_RETURN(rnd_end());
  }
  int ha_rnd_init_with_error(bool scan) __attribute__ ((warn_unused_result));
  int ha_reset();
  /* this is necessary in many places, e.g. in HANDLER command */
  int ha_index_or_rnd_end()
  {
    return inited == INDEX ? ha_index_end() : inited == RND ? ha_rnd_end() : 0;
  }
  /**
    The cached_table_flags is set at ha_open and ha_external_lock
  */
  Table_flags ha_table_flags() const
  {
    DBUG_ASSERT(cached_table_flags < (HA_LAST_TABLE_FLAG << 1));
    return cached_table_flags;
  }
  /**
    These functions represent the public interface to *users* of the
    handler class, hence they are *not* virtual. For the inheritance
    interface, see the (private) functions write_row(), update_row(),
    and delete_row() below.
  */
  int ha_external_lock(THD *thd, int lock_type);
  int ha_external_unlock(THD *thd) { return ha_external_lock(thd, F_UNLCK); }
  int ha_write_row(const uchar * buf);
  int ha_update_row(const uchar * old_data, const uchar * new_data);
  int ha_delete_row(const uchar * buf);
  void ha_release_auto_increment();

  inline bool keyread_enabled() { return keyread < MAX_KEY; }
  inline int ha_start_keyread(uint idx)
  {
    DBUG_ASSERT(!keyread_enabled());
    keyread= idx;
    return extra_opt(HA_EXTRA_KEYREAD, idx);
  }
  inline int ha_end_keyread()
  {
    if (!keyread_enabled())                    /* Enably lazy usage */
      return 0;
    keyread= MAX_KEY;
    return extra(HA_EXTRA_NO_KEYREAD);
  }

  /*
    End any active keyread. Return state so that we can restore things
    at end.
  */
  int ha_end_active_keyread()
  {
    int org_keyread;
    if (!keyread_enabled())
      return MAX_KEY;
    org_keyread= keyread;
    ha_end_keyread();
    return org_keyread;
  }
  /* Restore state to before ha_end_active_keyread */
  void ha_restart_keyread(int org_keyread)
  {
    DBUG_ASSERT(!keyread_enabled());
    if (org_keyread != MAX_KEY)
      ha_start_keyread(org_keyread);
  }

protected:
  bool is_root_handler() const;

public:
  int check_collation_compatibility();
  int check_long_hash_compatibility() const;
  int ha_check_for_upgrade(HA_CHECK_OPT *check_opt);
  /** to be actually called to get 'check()' functionality*/
  int ha_check(THD *thd, HA_CHECK_OPT *check_opt);
  int ha_repair(THD* thd, HA_CHECK_OPT* check_opt);
  void ha_start_bulk_insert(ha_rows rows, uint flags= 0)
  {
    DBUG_ENTER("handler::ha_start_bulk_insert");
    estimation_rows_to_insert= rows;
    bzero(&copy_info,sizeof(copy_info));
    start_bulk_insert(rows, flags);
    DBUG_VOID_RETURN;
  }
  int ha_end_bulk_insert();
  int ha_bulk_update_row(const uchar *old_data, const uchar *new_data,
                         ha_rows *dup_key_found);
  int ha_delete_all_rows();
  int ha_truncate();
  int ha_reset_auto_increment(ulonglong value);
  int ha_optimize(THD* thd, HA_CHECK_OPT* check_opt);
  int ha_analyze(THD* thd, HA_CHECK_OPT* check_opt);
  bool ha_check_and_repair(THD *thd);
  int ha_disable_indexes(uint mode);
  int ha_enable_indexes(uint mode);
  int ha_discard_or_import_tablespace(my_bool discard);
  int ha_rename_table(const char *from, const char *to);
  void ha_drop_table(const char *name);

  int ha_create(const char *name, TABLE *form, HA_CREATE_INFO *info);

  int ha_create_partitioning_metadata(const char *name, const char *old_name,
                                      chf_create_flags action_flag);

  int ha_change_partitions(HA_CREATE_INFO *create_info,
                           const char *path,
                           ulonglong * const copied,
                           ulonglong * const deleted,
                           const uchar *pack_frm_data,
                           size_t pack_frm_len);
  int ha_drop_partitions(const char *path);
  int ha_rename_partitions(const char *path);

  void adjust_next_insert_id_after_explicit_value(ulonglong nr);
  int update_auto_increment();
  virtual void print_error(int error, myf errflag);
  virtual bool get_error_message(int error, String *buf);
  uint get_dup_key(int error);
  /**
    Retrieves the names of the table and the key for which there was a
    duplicate entry in the case of HA_ERR_FOREIGN_DUPLICATE_KEY.

    If any of the table or key name is not available this method will return
    false and will not change any of child_table_name or child_key_name.

    @param child_table_name[out]    Table name
    @param child_table_name_len[in] Table name buffer size
    @param child_key_name[out]      Key name
    @param child_key_name_len[in]   Key name buffer size

    @retval  true                  table and key names were available
                                   and were written into the corresponding
                                   out parameters.
    @retval  false                 table and key names were not available,
                                   the out parameters were not touched.
  */
  virtual bool get_foreign_dup_key(char *child_table_name,
                                   uint child_table_name_len,
                                   char *child_key_name,
                                   uint child_key_name_len)
  { DBUG_ASSERT(false); return(false); }
  void reset_statistics()
  {
    rows_read= rows_changed= rows_tmp_read= 0;
    bzero(index_rows_read, sizeof(index_rows_read));
    bzero(&copy_info, sizeof(copy_info));
  }
  virtual void reset_copy_info() {}
  void ha_reset_copy_info()
  {
    bzero(&copy_info, sizeof(copy_info));
    reset_copy_info();
  }
  virtual void change_table_ptr(TABLE *table_arg, TABLE_SHARE *share);

  inline double io_cost(IO_AND_CPU_COST cost)
  {
    return cost.io * DISK_READ_COST * DISK_READ_RATIO;
  }

  inline double cost(IO_AND_CPU_COST cost)
  {
    return io_cost(cost) + cost.cpu;
  }

  /*
    Calculate cost with capping io_blocks to the given maximum.
    This is done here instead of earlier to allow filtering to work
    with the original' io_block counts.
  */
  inline double cost(ALL_READ_COST *cost)
  {
    double blocks= (MY_MIN(cost->index_cost.io,(double) cost->max_index_blocks) +
                    MY_MIN(cost->row_cost.io,  (double) cost->max_row_blocks));
    return ((cost->index_cost.cpu + cost->row_cost.cpu + cost->copy_cost) +
            blocks * DISK_READ_COST * DISK_READ_RATIO);
  }
  /*
    Same as above but without capping.
    This is only used for comparing cost with s->quick_read time, which
    does not do any capping.
  */

 inline double cost_no_capping(ALL_READ_COST *cost)
  {
    double blocks= (cost->index_cost.io + cost->row_cost.io);
    return ((cost->index_cost.cpu + cost->row_cost.cpu + cost->copy_cost) +
            blocks * DISK_READ_COST * DISK_READ_RATIO);
  }

  /*
    Calculate cost when we are going to excute the given read method
    multiple times
  */
  inline double cost_for_reading_multiple_times(double multiple,
                                                ALL_READ_COST *cost)

  {
    double blocks= (MY_MIN(cost->index_cost.io * multiple,
                              (double) cost->max_index_blocks) +
                    MY_MIN(cost->row_cost.io * multiple,
                           (double) cost->max_row_blocks));
    return ((cost->index_cost.cpu + cost->row_cost.cpu + cost->copy_cost) *
            multiple +
            blocks * DISK_READ_COST * DISK_READ_RATIO);
  }

  virtual ulonglong row_blocks()
  {
    return (stats.data_file_length + IO_SIZE-1) / IO_SIZE;
  }

  virtual ulonglong index_blocks(uint index, uint ranges, ha_rows rows);

  inline ulonglong index_blocks(uint index)
  {
    return index_blocks(index, 1, stats.records);
  }

  /*
    Time for a full table data scan. To be overrided by engines, should not
    be used by the sql level.
  */
protected:
  virtual IO_AND_CPU_COST scan_time()
  {
    IO_AND_CPU_COST cost;
    ulonglong length= stats.data_file_length;
    cost.io= (double) (length / IO_SIZE);
    cost.cpu= (!stats.block_size ? 0.0 :
               (double) ((length + stats.block_size-1)/stats.block_size) *
               INDEX_BLOCK_COPY_COST);
    return cost;
  }
public:

  /*
     Time for a full table scan

     @param records   Number of records from the engine or records from
                      status tables stored by ANALYZE TABLE.

     The TABLE_SCAN_SETUP_COST is there to prefer range scans to full
     table scans.  This is mainly to make the test suite happy as
     many tests has very few rows. In real life tables has more than
     a few rows and the extra cost has no practical effect.
  */

  inline IO_AND_CPU_COST ha_scan_time(ha_rows rows)
  {
    IO_AND_CPU_COST cost= scan_time();
    cost.cpu+= (TABLE_SCAN_SETUP_COST +
                (double) rows * (ROW_NEXT_FIND_COST + ROW_COPY_COST));
    return cost;
  }

  /*
    Time for a full table scan, fetching the rows from the table and comparing
    the row with the where clause
  */
  inline IO_AND_CPU_COST ha_scan_and_compare_time(ha_rows rows)
  {
    IO_AND_CPU_COST cost= ha_scan_time(rows);
    cost.cpu+= (double) rows * WHERE_COST;
    return cost;
  }

  /*
    Update table->share optimizer costs for this particular table.
    Called once when table is opened the first time.
  */
  virtual void update_optimizer_costs(OPTIMIZER_COSTS *costs) {}

  /*
    Set handler optimizer cost variables.
    Called for each table used by the statment
    This is virtual mainly for the partition engine.
  */
  virtual void set_optimizer_costs(THD *thd);

protected:
  /*
    Cost of reading 'rows' number of rows with a rowid
  */
  virtual IO_AND_CPU_COST rnd_pos_time(ha_rows rows)
  {
    double r= rows2double(rows);
    return
    {
      r * ((stats.block_size + IO_SIZE -1 )/IO_SIZE),  // Blocks read
      r * INDEX_BLOCK_COPY_COST                        // Copy block from cache
     };
  }
public:

  /*
    Time for doing and internal rnd_pos() inside the engine.  For some
    engine, this is more efficient than the SQL layer calling
    rnd_pos() as there is no overhead in converting/checking the
    rnd_pos_value.  This is used when calculating the cost of fetching
    a key+row in one go (like when scanning an index and fetching the
    row).
  */

  inline IO_AND_CPU_COST ha_rnd_pos_time(ha_rows rows)
  {
    IO_AND_CPU_COST cost= rnd_pos_time(rows);
    set_if_smaller(cost.io, (double) row_blocks());
    cost.cpu+= rows2double(rows) * (ROW_LOOKUP_COST + ROW_COPY_COST);
    return cost;
  }

  /*
    This cost if when we are calling rnd_pos() explict in the call
    For the moment this function is identical to ha_rnd_pos time,
    but that may change in the future after we do more cost checks for
    more engines.
  */
  inline IO_AND_CPU_COST ha_rnd_pos_call_time(ha_rows rows)
  {
    IO_AND_CPU_COST cost= rnd_pos_time(rows);
    set_if_smaller(cost.io, (double) row_blocks());
    cost.cpu+= rows2double(rows) * (ROW_LOOKUP_COST + ROW_COPY_COST);
    return cost;
  }

  inline IO_AND_CPU_COST ha_rnd_pos_call_and_compare_time(ha_rows rows)
  {
    IO_AND_CPU_COST cost;
    cost= ha_rnd_pos_call_time(rows);
    cost.cpu+= rows2double(rows) * WHERE_COST;
    return cost;
  }

  /**
    Calculate cost of 'index_only' scan for given index, a number of ranges
    and number of records.

    @param index   Index to read
    @param rows    #of records to read
    @param blocks  Number of IO blocks that needs to be accessed.
                   0 if not known (in which case it's calculated)
  */
protected:
  virtual IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                                       ulonglong blocks);
public:

  /*
    Calculate cost of 'keyread' scan for given index and number of records
    including fetching the key to the 'record' buffer.
  */
  IO_AND_CPU_COST ha_keyread_time(uint index, ulong ranges, ha_rows rows,
                                  ulonglong blocks);

  /* Same as above, but take into account copying the key the the SQL layer */
  inline IO_AND_CPU_COST ha_keyread_and_copy_time(uint index, ulong ranges,
                                                  ha_rows rows,
                                                  ulonglong blocks)
  {
    IO_AND_CPU_COST cost= ha_keyread_time(index, ranges, rows, blocks);
    cost.cpu+= (double) rows * KEY_COPY_COST;
    return cost;
  }

  inline IO_AND_CPU_COST ha_keyread_and_compare_time(uint index, ulong ranges,
                                                     ha_rows rows,
                                                     ulonglong blocks)
  {
    IO_AND_CPU_COST cost= ha_keyread_time(index, ranges, rows, blocks);
    cost.cpu+= (double) rows * (KEY_COPY_COST + WHERE_COST);
    return cost;
  }

  IO_AND_CPU_COST ha_keyread_clustered_time(uint index,
                                            ulong ranges,
                                            ha_rows rows,
                                            ulonglong blocks);
  /*
    Time for a full table index scan (without copy or compare cost).
    To be overrided by engines, sql level should use ha_key_scan_time().
    Note that IO_AND_CPU_COST does not include avg_io_cost() !
  */
protected:
  virtual IO_AND_CPU_COST key_scan_time(uint index, ha_rows rows)
  {
    return keyread_time(index, 1, MY_MAX(rows, 1), 0);
  }
public:

  /* Cost of doing a full index scan */
  inline IO_AND_CPU_COST ha_key_scan_time(uint index, ha_rows rows)
  {
    IO_AND_CPU_COST cost= key_scan_time(index, rows);
    cost.cpu+= (INDEX_SCAN_SETUP_COST + KEY_LOOKUP_COST +
                (double) rows * (KEY_NEXT_FIND_COST + KEY_COPY_COST));
    return cost;
  }

  /*
    Cost of doing a full index scan with record copy and compare
    @param rows  Rows from stat tables
  */
  inline IO_AND_CPU_COST ha_key_scan_and_compare_time(uint index, ha_rows rows)
  {
    IO_AND_CPU_COST cost= ha_key_scan_time(index, rows);
    cost.cpu+= (double) rows * WHERE_COST;
    return cost;
  }

  virtual const key_map *keys_to_use_for_scanning() { return &key_map_empty; }

  /*
    True if changes to the table is persistent (if there are no rollback)
    This is used to decide:
    - If the table is stored in the transaction or non transactional binary
      log
    - How things are tracked in trx and in add_changed_table().
    - If we can combine several statements under one commit in the binary log.
  */
  bool has_transactions() const
  {
    return ((ha_table_flags() & (HA_NO_TRANSACTIONS | HA_PERSISTENT_TABLE))
            == 0);
  }
  /*
    True if table has both transactions and rollback. This is used to decide
    if we should write the changes to the binary log.  If this is true,
    we don't have to write failed statements to the log as they can be
    rolled back.
  */
  bool has_transactions_and_rollback() const
  {
    return has_transactions() && has_rollback();
  }
  /*
    True if the underlaying table support transactions and rollback
  */
  bool has_transaction_manager() const
  {
    return ((ha_table_flags() & HA_NO_TRANSACTIONS) == 0 && has_rollback());
  }

  /*
    True if the underlaying table support TRANSACTIONAL table option
  */
  bool has_transactional_option() const
  {
    extern handlerton *maria_hton;
    return partition_ht() == maria_hton || has_transaction_manager();
  }

  /*
    True if table has rollback. Used to check if an update on the table
    can be killed fast.
  */

  bool has_rollback() const
  {
    return ((ht->flags & HTON_NO_ROLLBACK) == 0);
  }

  /**
    This method is used to analyse the error to see whether the error
    is ignorable or not, certain handlers can have more error that are
    ignorable than others. E.g. the partition handler can get inserts
    into a range where there is no partition and this is an ignorable
    error.
    HA_ERR_FOUND_DUP_UNIQUE is a special case in MyISAM that means the
    same thing as HA_ERR_FOUND_DUP_KEY but can in some cases lead to
    a slightly different error message.
  */
  virtual bool is_fatal_error(int error, uint flags)
  {
    if (!error ||
        ((flags & HA_CHECK_DUP_KEY) &&
         (error == HA_ERR_FOUND_DUPP_KEY ||
          error == HA_ERR_FOUND_DUPP_UNIQUE)) ||
        error == HA_ERR_AUTOINC_ERANGE ||
        ((flags & HA_CHECK_FK_ERROR) &&
         (error == HA_ERR_ROW_IS_REFERENCED ||
          error == HA_ERR_NO_REFERENCED_ROW)))
      return FALSE;
    return TRUE;
  }

  /**
    Number of rows in table. It will only be called if
    (table_flags() & (HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT)) != 0
  */
  virtual int pre_records() { return 0; }
  virtual ha_rows records() { return stats.records; }
  /**
    Return upper bound of current number of records in the table
    (max. of how many records one will retrieve when doing a full table scan)
    If upper bound is not known, HA_POS_ERROR should be returned as a max
    possible upper bound.
  */
  virtual ha_rows estimate_rows_upper_bound()
  { return stats.records+EXTRA_RECORDS; }

  /**
    Get the row type from the storage engine.  If this method returns
    ROW_TYPE_NOT_USED, the information in HA_CREATE_INFO should be used.
  */
  virtual enum row_type get_row_type() const { return ROW_TYPE_NOT_USED; }

  virtual const char *index_type(uint key_number) { DBUG_ASSERT(0); return "";}


  /**
    Signal that the table->read_set and table->write_set table maps changed
    The handler is allowed to set additional bits in the above map in this
    call. Normally the handler should ignore all calls until we have done
    a ha_rnd_init() or ha_index_init(), write_row(), update_row or delete_row()
    as there may be several calls to this routine.
  */
  virtual void column_bitmaps_signal();
  /*
    We have to check for inited as some engines, like innodb, sets
    active_index during table scan.
  */
  uint get_index(void) const
  { return inited == INDEX ? active_index : MAX_KEY; }
  int ha_close(void);

  /**
    @retval  0   Bulk update used by handler
    @retval  1   Bulk update not used, normal operation used
  */
  virtual bool start_bulk_update() { return 1; }
  /**
    @retval  0   Bulk delete used by handler
    @retval  1   Bulk delete not used, normal operation used
  */
  virtual bool start_bulk_delete() { return 1; }
  /**
    After this call all outstanding updates must be performed. The number
    of duplicate key errors are reported in the duplicate key parameter.
    It is allowed to continue to the batched update after this call, the
    handler has to wait until end_bulk_update with changing state.

    @param    dup_key_found       Number of duplicate keys found

    @retval  0           Success
    @retval  >0          Error code
  */
  virtual int exec_bulk_update(ha_rows *dup_key_found)
  {
    DBUG_ASSERT(FALSE);
    return HA_ERR_WRONG_COMMAND;
  }
  /**
    Perform any needed clean-up, no outstanding updates are there at the
    moment.
  */
  virtual int end_bulk_update() { return 0; }
  /**
    Execute all outstanding deletes and close down the bulk delete.

    @retval 0             Success
    @retval >0            Error code
  */
  virtual int end_bulk_delete()
  {
    DBUG_ASSERT(FALSE);
    return HA_ERR_WRONG_COMMAND;
  }
  virtual int pre_index_read_map(const uchar *key,
                                 key_part_map keypart_map,
                                 enum ha_rkey_function find_flag,
                                 bool use_parallel)
   { return 0; }
  virtual int pre_index_first(bool use_parallel)
   { return 0; }
  virtual int pre_index_last(bool use_parallel)
   { return 0; }
  virtual int pre_index_read_last_map(const uchar *key,
                                      key_part_map keypart_map,
                                      bool use_parallel)
   { return 0; }
/*
  virtual int pre_read_multi_range_first(KEY_MULTI_RANGE **found_range_p,
                                         KEY_MULTI_RANGE *ranges,
                                         uint range_count,
                                         bool sorted, HANDLER_BUFFER *buffer,
                                         bool use_parallel);
*/
  virtual int pre_multi_range_read_next(bool use_parallel)
  { return 0; }
  virtual int pre_read_range_first(const key_range *start_key,
                                   const key_range *end_key,
                                   bool eq_range, bool sorted,
                                   bool use_parallel)
   { return 0; }
  virtual int pre_ft_read(bool use_parallel)
   { return 0; }
  virtual int pre_rnd_next(bool use_parallel)
   { return 0; }
  int ha_pre_rnd_init(bool scan)
  {
    int result;
    DBUG_ENTER("ha_pre_rnd_init");
    DBUG_ASSERT(pre_inited==NONE || (pre_inited==RND && scan));
    pre_inited= (result= pre_rnd_init(scan)) ? NONE: RND;
    DBUG_RETURN(result);
  }
  int ha_pre_rnd_end()
  {
    DBUG_ENTER("ha_pre_rnd_end");
    DBUG_ASSERT(pre_inited==RND);
    pre_inited=NONE;
    DBUG_RETURN(pre_rnd_end());
  }
  virtual int pre_rnd_init(bool scan) { return 0; }
  virtual int pre_rnd_end() { return 0; }
  virtual int pre_index_init(uint idx, bool sorted) { return 0; }
  virtual int pre_index_end() { return 0; }
  int ha_pre_index_init(uint idx, bool sorted)
  {
    int result;
    DBUG_ENTER("ha_pre_index_init");
    DBUG_ASSERT(pre_inited==NONE);
    if (!(result= pre_index_init(idx, sorted)))
      pre_inited=INDEX;
    DBUG_RETURN(result);
  }
  int ha_pre_index_end()
  {
    DBUG_ENTER("ha_pre_index_end");
    DBUG_ASSERT(pre_inited==INDEX);
    pre_inited=NONE;
    DBUG_RETURN(pre_index_end());
  }
  int ha_pre_index_or_rnd_end()
  {
    return (pre_inited == INDEX ?
            ha_pre_index_end() :
            pre_inited == RND ? ha_pre_rnd_end() : 0 );
  }
  virtual bool vers_can_native(THD *thd)
  {
    return ht->flags & HTON_NATIVE_SYS_VERSIONING;
  }

  /**
     @brief
     Positions an index cursor to the index specified in the
     handle. Fetches the row if available. If the key value is null,
     begin at the first key of the index.
  */
protected:
  virtual int index_read_map(uchar * buf, const uchar * key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag)
  {
    uint key_len= calculate_key_len(table, active_index, key, keypart_map);
    return index_read(buf, key, key_len, find_flag);
  }
  /**
     @brief
     Positions an index cursor to the index specified in the
     handle. Fetches the row if available. If the key value is null,
     begin at the first key of the index.
  */
  virtual int index_read_idx_map(uchar * buf, uint index, const uchar * key,
                                 key_part_map keypart_map,
                                 enum ha_rkey_function find_flag);
  virtual int index_next(uchar * buf)
   { return  HA_ERR_WRONG_COMMAND; }
  virtual int index_prev(uchar * buf)
   { return  HA_ERR_WRONG_COMMAND; }
  virtual int index_first(uchar * buf)
   { return  HA_ERR_WRONG_COMMAND; }
  virtual int index_last(uchar * buf)
   { return  HA_ERR_WRONG_COMMAND; }
  virtual int index_next_same(uchar *buf, const uchar *key, uint keylen);
  /**
     @brief
     The following functions works like index_read, but it find the last
     row with the current key value or prefix.
     @returns @see index_read_map().
  */
  virtual int index_read_last_map(uchar * buf, const uchar * key,
                                  key_part_map keypart_map)
  {
    uint key_len= calculate_key_len(table, active_index, key, keypart_map);
    return index_read_last(buf, key, key_len);
  }
  virtual int close(void)=0;
  inline void update_rows_read()
  {
    if (likely(!internal_tmp_table))
      rows_read++;
    else
      rows_tmp_read++;
  }
  inline void update_index_statistics()
  {
    index_rows_read[active_index]++;
    update_rows_read();
  }
public:

  int ha_index_read_map(uchar * buf, const uchar * key,
                        key_part_map keypart_map,
                        enum ha_rkey_function find_flag);
  int ha_index_read_idx_map(uchar * buf, uint index, const uchar * key,
                            key_part_map keypart_map,
                            enum ha_rkey_function find_flag);
  int ha_index_next(uchar * buf);
  int ha_index_prev(uchar * buf);
  int ha_index_first(uchar * buf);
  int ha_index_last(uchar * buf);
  int ha_index_next_same(uchar *buf, const uchar *key, uint keylen);
  /*
    TODO: should we make for those functions non-virtual ha_func_name wrappers,
    too?
  */
  virtual ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                              void *seq_init_param, 
                                              uint n_ranges, uint *bufsz,
                                              uint *mrr_mode, ha_rows limit,
                                              Cost_estimate *cost);
  virtual ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                        uint key_parts, uint *bufsz, 
                                        uint *mrr_mode, Cost_estimate *cost);
  virtual int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                    uint n_ranges, uint mrr_mode, 
                                    HANDLER_BUFFER *buf);
  virtual int multi_range_read_next(range_id_t *range_info);
private:
  inline void calculate_costs(Cost_estimate *cost, uint keyno,
                              uint ranges, uint multi_row_ranges, uint flags,
                              ha_rows total_rows,
                              ulonglong io_blocks,
                              ulonglong unassigned_single_point_ranges);
public:
  /*
    Return string representation of the MRR plan.

    This is intended to be used for EXPLAIN, via the following scenario:
    1. SQL layer calls handler->multi_range_read_info().
    1.1. Storage engine figures out whether it will use some non-default
         MRR strategy, sets appropritate bits in *mrr_mode, and returns 
         control to SQL layer
    2. SQL layer remembers the returned mrr_mode
    3. SQL layer compares various options and choses the final query plan. As
       a part of that, it makes a choice of whether to use the MRR strategy
       picked in 1.1
    4. EXPLAIN code converts the query plan to its text representation. If MRR
       strategy is part of the plan, it calls
       multi_range_read_explain_info(mrr_mode) to get a text representation of
       the picked MRR strategy.

    @param mrr_mode   Mode which was returned by multi_range_read_info[_const]
    @param str        INOUT string to be printed for EXPLAIN
    @param str_end    End of the string buffer. The function is free to put the 
                      string into [str..str_end] memory range.
  */
  virtual int multi_range_read_explain_info(uint mrr_mode, char *str, 
                                            size_t size)
  { return 0; }

  virtual int read_range_first(const key_range *start_key,
                               const key_range *end_key,
                               bool eq_range, bool sorted);
  virtual int read_range_next();
  void set_end_range(const key_range *end_key);
  int compare_key(key_range *range);
  int compare_key2(key_range *range) const;
  virtual int ft_init() { return HA_ERR_WRONG_COMMAND; }
  virtual int pre_ft_init() { return HA_ERR_WRONG_COMMAND; }
  virtual void ft_end() {}
  virtual int pre_ft_end() { return 0; }
  virtual FT_INFO *ft_init_ext(uint flags, uint inx,String *key)
    { return NULL; }
public:
  virtual int ft_read(uchar *buf) { return HA_ERR_WRONG_COMMAND; }
  virtual int rnd_next(uchar *buf)=0;
  virtual int rnd_pos(uchar * buf, uchar *pos)=0;
  /**
    This function only works for handlers having
    HA_PRIMARY_KEY_REQUIRED_FOR_POSITION set.
    It will return the row with the PK given in the record argument.
  */
  virtual int rnd_pos_by_record(uchar *record)
  {
    int error;
    DBUG_ASSERT(table_flags() & HA_PRIMARY_KEY_REQUIRED_FOR_POSITION);

    error = ha_rnd_init(false);
    if (error != 0)
      return error;

    position(record);
    error = ha_rnd_pos(record, ref);
    ha_rnd_end();
    return error;
  }
  virtual int read_first_row(uchar *buf, uint primary_key);
public:

  /* Same as above, but with statistics */
  inline int ha_ft_read(uchar *buf);
  inline void ha_ft_end() { ft_end(); ft_handler=NULL; }
  int ha_rnd_next(uchar *buf);
  int ha_rnd_pos(uchar *buf, uchar *pos);
  inline int ha_rnd_pos_by_record(uchar *buf);
  inline int ha_read_first_row(uchar *buf, uint primary_key);

  /**
    The following 2 function is only needed for tables that may be
    internal temporary tables during joins.
  */
  virtual int remember_rnd_pos()
    { return HA_ERR_WRONG_COMMAND; }
  virtual int restart_rnd_next(uchar *buf)
    { return HA_ERR_WRONG_COMMAND; }

  virtual ha_rows records_in_range(uint inx, const key_range *min_key,
                                   const key_range *max_key,
                                   page_range *res)
    { return (ha_rows) 10; }
  /*
    If HA_PRIMARY_KEY_REQUIRED_FOR_POSITION is set, then it sets ref
    (reference to the row, aka position, with the primary key given in
    the record).
    Otherwise it set ref to the current row.
  */
  virtual void position(const uchar *record)=0;
  virtual int info(uint)=0; // see my_base.h for full description
  virtual void get_dynamic_partition_info(PARTITION_STATS *stat_info,
                                          uint part_id);
  virtual void set_partitions_to_open(List<String> *partition_names) {}
  virtual bool check_if_updates_are_ignored(const char *op) const;
  virtual int change_partitions_to_open(List<String> *partition_names)
  { return 0; }
  virtual int extra(enum ha_extra_function operation)
  { return 0; }
  virtual int extra_opt(enum ha_extra_function operation, ulong arg)
  { return extra(operation); }
  /*
    Table version id for the the table. This should change for each
    sucessfull ALTER TABLE.
    This is used by the handlerton->check_version() to ask the engine
    if the table definition has been updated.
    Storage engines that does not support inplace alter table does not
    have to support this call.
  */
  virtual ulonglong table_version() const { return 0; }

  /**
    In an UPDATE or DELETE, if the row under the cursor was locked by another
    transaction, and the engine used an optimistic read of the last
    committed row value under the cursor, then the engine returns 1 from this
    function. MySQL must NOT try to update this optimistic value. If the
    optimistic value does not match the WHERE condition, MySQL can decide to
    skip over this row. Currently only works for InnoDB. This can be used to
    avoid unnecessary lock waits.

    If this method returns nonzero, it will also signal the storage
    engine that the next read will be a locking re-read of the row.
  */
  bool ha_was_semi_consistent_read();
  virtual bool was_semi_consistent_read() { return 0; }
  /**
    Tell the engine whether it should avoid unnecessary lock waits.
    If yes, in an UPDATE or DELETE, if the row under the cursor was locked
    by another transaction, the engine may try an optimistic read of
    the last committed row value under the cursor.
  */
  virtual void try_semi_consistent_read(bool) {}
  virtual void unlock_row() {}
  virtual int start_stmt(THD *thd, thr_lock_type lock_type) {return 0;}
  virtual bool need_info_for_auto_inc() { return 0; }
  virtual bool can_use_for_auto_inc_init() { return 1; }
  virtual void get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values);
  void set_next_insert_id(ulonglong id)
  {
    DBUG_PRINT("info",("auto_increment: next value %lu", (ulong)id));
    next_insert_id= id;
  }
  virtual void restore_auto_increment(ulonglong prev_insert_id)
  {
    /*
      Insertion of a row failed, re-use the lastly generated auto_increment
      id, for the next row. This is achieved by resetting next_insert_id to
      what it was before the failed insertion (that old value is provided by
      the caller). If that value was 0, it was the first row of the INSERT;
      then if insert_id_for_cur_row contains 0 it means no id was generated
      for this first row, so no id was generated since the INSERT started, so
      we should set next_insert_id to 0; if insert_id_for_cur_row is not 0, it
      is the generated id of the first and failed row, so we use it.
    */
    next_insert_id= (prev_insert_id > 0) ? prev_insert_id :
      insert_id_for_cur_row;
  }

  virtual void update_create_info(HA_CREATE_INFO *create_info) {}
  int check_old_types();
  virtual int assign_to_keycache(THD* thd, HA_CHECK_OPT* check_opt)
  { return HA_ADMIN_NOT_IMPLEMENTED; }
  virtual int preload_keys(THD* thd, HA_CHECK_OPT* check_opt)
  { return HA_ADMIN_NOT_IMPLEMENTED; }
  /* end of the list of admin commands */

  virtual int indexes_are_disabled(void) {return 0;}
  virtual void append_create_info(String *packet) {}
  /**
    If index == MAX_KEY then a check for table is made and if index <
    MAX_KEY then a check is made if the table has foreign keys and if
    a foreign key uses this index (and thus the index cannot be dropped).

    @param  index            Index to check if foreign key uses it

    @retval   TRUE            Foreign key defined on table or index
    @retval   FALSE           No foreign key defined
  */
  virtual bool is_fk_defined_on_table_or_index(uint index)
  { return FALSE; }
  virtual char* get_foreign_key_create_info()
  { return(NULL);}  /* gets foreign key create string from InnoDB */
  /**
    Used in ALTER TABLE to check if changing storage engine is allowed.

    @note Called without holding thr_lock.c lock.

    @retval true   Changing storage engine is allowed.
    @retval false  Changing storage engine not allowed.
  */
  virtual bool can_switch_engines() { return true; }
  virtual int can_continue_handler_scan() { return 0; }
  /**
    Get the list of foreign keys in this table.

    @remark Returns the set of foreign keys where this table is the
            dependent or child table.

    @param thd  The thread handle.
    @param f_key_list[out]  The list of foreign keys.

    @return The handler error code or zero for success.
  */
  virtual int
  get_foreign_key_list(const THD *thd, List<FOREIGN_KEY_INFO> *f_key_list)
  { return 0; }
  /**
    Get the list of foreign keys referencing this table.

    @remark Returns the set of foreign keys where this table is the
            referenced or parent table.

    @param thd  The thread handle.
    @param f_key_list[out]  The list of foreign keys.

    @return The handler error code or zero for success.
  */
  virtual int
  get_parent_foreign_key_list(const THD *thd, List<FOREIGN_KEY_INFO> *f_key_list)
  { return 0; }
  virtual uint referenced_by_foreign_key() { return 0;}
  virtual void init_table_handle_for_HANDLER()
  { return; }       /* prepare InnoDB for HANDLER */
  virtual void free_foreign_key_create_info(char* str) {}
  /** The following can be called without an open handler */
  virtual const char *table_type() const { return hton_name(ht)->str; }
  /* The following is same as table_table(), except for partition engine */
  virtual const char *real_table_type() const { return hton_name(ht)->str; }
  const char **bas_ext() const { return ht->tablefile_extensions; }

  virtual int get_default_no_partitions(HA_CREATE_INFO *create_info)
  { return 1;}
  virtual void set_auto_partitions(partition_info *part_info) { return; }
  virtual bool get_no_parts(const char *name,
                            uint *no_parts)
  {
    *no_parts= 0;
    return 0;
  }
  virtual void set_part_info(partition_info *part_info) {return;}
  virtual void return_record_by_parent() { return; }

  /* Information about index. Both index and part starts from 0 */
  virtual ulong index_flags(uint idx, uint part, bool all_parts) const =0;

  uint max_record_length() const
  { return MY_MIN(HA_MAX_REC_LENGTH, max_supported_record_length()); }
  uint max_keys() const
  { return MY_MIN(MAX_KEY, max_supported_keys()); }
  uint max_key_parts() const
  { return MY_MIN(MAX_REF_PARTS, max_supported_key_parts()); }
  uint max_key_length() const
  { return MY_MIN(MAX_DATA_LENGTH_FOR_KEY, max_supported_key_length()); }
  uint max_key_part_length() const
  { return MY_MIN(MAX_DATA_LENGTH_FOR_KEY, max_supported_key_part_length()); }

  virtual uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }
  virtual uint max_supported_keys() const { return 0; }
  virtual uint max_supported_key_parts() const { return MAX_REF_PARTS; }
  virtual uint max_supported_key_length() const { return MAX_DATA_LENGTH_FOR_KEY; }
  virtual uint max_supported_key_part_length() const { return 255; }
  virtual uint min_record_length(uint options) const { return 1; }

  virtual int pre_calculate_checksum() { return 0; }
  virtual int calculate_checksum();
  virtual bool is_crashed() const  { return 0; }
  virtual bool auto_repair(int error) const { return 0; }

  void update_global_table_stats();
  void update_global_index_stats();

  /**
    @note lock_count() can return > 1 if the table is MERGE or partitioned.
  */
  virtual uint lock_count(void) const { return 1; }
  /**
    Get the lock(s) for the table and perform conversion of locks if needed.

    Is not invoked for non-transactional temporary tables.

    @note store_lock() can return more than one lock if the table is MERGE
    or partitioned.

    @note that one can NOT rely on table->in_use in store_lock().  It may
    refer to a different thread if called from mysql_lock_abort_for_thread().

    @note If the table is MERGE, store_lock() can return less locks
    than lock_count() claimed. This can happen when the MERGE children
    are not attached when this is called from another thread.
  */
  virtual THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
				     enum thr_lock_type lock_type)=0;

  /** Type of table for caching query */
  virtual uint8 table_cache_type() { return HA_CACHE_TBL_NONTRANSACT; }


  /**
    @brief Register a named table with a call back function to the query cache.

    @param thd The thread handle
    @param table_key A pointer to the table name in the table cache
    @param key_length The length of the table name
    @param[out] engine_callback The pointer to the storage engine call back
      function
    @param[out] engine_data Storage engine specific data which could be
      anything

    This method offers the storage engine, the possibility to store a reference
    to a table name which is going to be used with query cache. 
    The method is called each time a statement is written to the cache and can
    be used to verify if a specific statement is cacheable. It also offers
    the possibility to register a generic (but static) call back function which
    is called each time a statement is matched against the query cache.

    @note If engine_data supplied with this function is different from
      engine_data supplied with the callback function, and the callback returns
      FALSE, a table invalidation on the current table will occur.

    @return Upon success the engine_callback will point to the storage engine
      call back function, if any, and engine_data will point to any storage
      engine data used in the specific implementation.
      @retval TRUE Success
      @retval FALSE The specified table or current statement should not be
        cached
  */

  virtual my_bool register_query_cache_table(THD *thd, const char *table_key,
                                             uint key_length,
                                             qc_engine_callback *callback,
                                             ulonglong *engine_data)
  {
    *callback= 0;
    return TRUE;
  }

  /*
    Count tables invisible from all tables list on which current one built
    (like myisammrg and partitioned tables)

    tables_type          mask for the tables should be added herdde

    returns number of such tables
  */

  virtual uint count_query_cache_dependant_tables(uint8 *tables_type
                                                  __attribute__((unused)))
  {
    return 0;
  }

  /*
    register tables invisible from all tables list on which current one built
    (like myisammrg and partitioned tables).

    @note they should be counted by method above

    cache                Query cache pointer
    block                Query cache block to write the table
    n                    Number of the table

    @retval FALSE - OK
    @retval TRUE  - Error
  */

  virtual my_bool
    register_query_cache_dependant_tables(THD *thd
                                          __attribute__((unused)),
                                          Query_cache *cache
                                          __attribute__((unused)),
                                          Query_cache_block_table **block
                                          __attribute__((unused)),
                                          uint *n __attribute__((unused)))
  {
    return FALSE;
  }

 /*
   Check if the key is a clustering key

   - Data is stored together with the primary key (no secondary lookup
     needed to find the row data). The optimizer uses this to find out
     the cost of fetching data.

     Note that in many cases a clustered key is also a reference key.
     This means that:

   - The key is part of each secondary key and is used
     to find the row data in the primary index when reading trough
     secondary indexes.
   - When doing a HA_KEYREAD_ONLY we get also all the primary key parts
     into the row. This is critical property used by index_merge.

   All the above is usually true for engines that store the row
   data in the primary key index (e.g. in a b-tree), and use the key
   key value as a position().  InnoDB is an example of such an engine.

   For a clustered (primary) key, the following should also hold:
   index_flags() should contain HA_CLUSTERED_INDEX
   index_flags() should not contain HA_KEYREAD_ONLY or HA_DO_RANGE_FILTER_PUSHDOWN
   table_flags() should contain HA_TABLE_SCAN_ON_INDEX

   For a reference key the following should also hold:
   table_flags() should contain HA_PRIMARY_KEY_IS_READ_INDEX.

   @retval TRUE   yes
   @retval FALSE  No.
 */

 /* The following code is for primary keys */
 inline bool pk_is_clustering_key(uint index) const;
 /* Same as before but for other keys, in which case we can skip the check */
 inline bool is_clustering_key(uint index) const;

 virtual int cmp_ref(const uchar *ref1, const uchar *ref2)
 {
   return memcmp(ref1, ref2, ref_length);
 }

 /*
   Condition pushdown to storage engines
 */

 /**
   Push condition down to the table handler.

   @param  cond   Condition to be pushed. The condition tree must not be
                  modified by the by the caller.

   @return
     The 'remainder' condition that caller must use to filter out records.
     NULL means the handler will not return rows that do not match the
     passed condition.

   @note
   The pushed conditions form a stack (from which one can remove the
   last pushed condition using cond_pop).
   The table handler filters out rows using (pushed_cond1 AND pushed_cond2 
   AND ... AND pushed_condN)
   or less restrictive condition, depending on handler's capabilities.

   handler->ha_reset() call empties the condition stack.
   Calls to rnd_init/rnd_end, index_init/index_end etc do not affect the
   condition stack.
 */ 
 virtual const COND *cond_push(const COND *cond) { return cond; };
 /**
   Pop the top condition from the condition stack of the handler instance.

   Pops the top if condition stack, if stack is not empty.
 */
 virtual void cond_pop() { return; };

 /**
   Push metadata for the current operation down to the table handler.
 */
 virtual int info_push(uint info_type, void *info) { return 0; };

 /**
   Push down an index condition to the handler.

   The server will use this method to push down a condition it wants
   the handler to evaluate when retrieving records using a specified
   index. The pushed index condition will only refer to fields from
   this handler that is contained in the index (but it may also refer
   to fields in other handlers). Before the handler evaluates the
   condition it must read the content of the index entry into the 
   record buffer.

   The handler is free to decide if and how much of the condition it
   will take responsibility for evaluating. Based on this evaluation
   it should return the part of the condition it will not evaluate.
   If it decides to evaluate the entire condition it should return
   NULL. If it decides not to evaluate any part of the condition it
   should return a pointer to the same condition as given as argument.

   @param keyno    the index number to evaluate the condition on
   @param idx_cond the condition to be evaluated by the handler

   @return The part of the pushed condition that the handler decides
           not to evaluate
 */
 virtual Item *idx_cond_push(uint keyno, Item* idx_cond) { return idx_cond; }

 /** Reset information about pushed index conditions */
 virtual void cancel_pushed_idx_cond()
 {
   pushed_idx_cond= NULL;
   pushed_idx_cond_keyno= MAX_KEY;
   in_range_check_pushed_down= false;
 }

 virtual void cancel_pushed_rowid_filter()
 {
   pushed_rowid_filter= NULL;
   if (rowid_filter_is_active)
   {
     rowid_filter_is_active= false;
     rowid_filter_changed();
   }
 }

 virtual void disable_pushed_rowid_filter()
 {
   DBUG_ASSERT(pushed_rowid_filter != NULL &&
               save_pushed_rowid_filter == NULL);
   save_pushed_rowid_filter= pushed_rowid_filter;
   save_rowid_filter_is_active= rowid_filter_is_active;
   pushed_rowid_filter= NULL;

   if (rowid_filter_is_active)
   {
     rowid_filter_is_active= false;
     rowid_filter_changed();
   }
 }

 virtual void enable_pushed_rowid_filter()
 {
   DBUG_ASSERT(save_pushed_rowid_filter != NULL &&
               pushed_rowid_filter == NULL);
   pushed_rowid_filter= save_pushed_rowid_filter;
   save_pushed_rowid_filter= NULL;
   if (save_rowid_filter_is_active)
   {
     rowid_filter_is_active= true;
     rowid_filter_changed();
   }
 }

 virtual bool rowid_filter_push(Rowid_filter *rowid_filter) { return true; }
 /* Signal that rowid filter may have been enabled / disabled */
 virtual void rowid_filter_changed() {}

 /* Needed for partition / spider */
  virtual TABLE_LIST *get_next_global_for_child() { return NULL; }

 /**
   Part of old, deprecated in-place ALTER API.
 */
 virtual bool check_if_incompatible_data(HA_CREATE_INFO *create_info,
					 uint table_changes)
 { return COMPATIBLE_DATA_NO; }

 /* On-line/in-place ALTER TABLE interface. */

 /*
   Here is an outline of on-line/in-place ALTER TABLE execution through
   this interface.

   Phase 1 : Initialization
   ========================
   During this phase we determine which algorithm should be used
   for execution of ALTER TABLE and what level concurrency it will
   require.

   *) This phase starts by opening the table and preparing description
      of the new version of the table.
   *) Then we check if it is impossible even in theory to carry out
      this ALTER TABLE using the in-place algorithm. For example, because
      we need to change storage engine or the user has explicitly requested
      usage of the "copy" algorithm.
   *) If in-place ALTER TABLE is theoretically possible, we continue
      by compiling differences between old and new versions of the table
      in the form of HA_ALTER_FLAGS bitmap. We also build a few
      auxiliary structures describing requested changes and store
      all these data in the Alter_inplace_info object.
   *) Then the handler::check_if_supported_inplace_alter() method is called
      in order to find if the storage engine can carry out changes requested
      by this ALTER TABLE using the in-place algorithm. To determine this,
      the engine can rely on data in HA_ALTER_FLAGS/Alter_inplace_info
      passed to it as well as on its own checks. If the in-place algorithm
      can be used for this ALTER TABLE, the level of required concurrency for
      its execution is also returned.
      If any errors occur during the handler call, ALTER TABLE is aborted
      and no further handler functions are called.
   *) Locking requirements of the in-place algorithm are compared to any
      concurrency requirements specified by user. If there is a conflict
      between them, we either switch to the copy algorithm or emit an error.

   Phase 2 : Execution
   ===================

   In this phase the operations are executed.

   *) As the first step, we acquire a lock corresponding to the concurrency
      level which was returned by handler::check_if_supported_inplace_alter()
      and requested by the user. This lock is held for most of the
      duration of in-place ALTER (if HA_ALTER_INPLACE_COPY_LOCK
      or HA_ALTER_INPLACE_COPY_NO_LOCK were returned we acquire an
      exclusive lock for duration of the next step only).
   *) After that we call handler::ha_prepare_inplace_alter_table() to give the
      storage engine a chance to update its internal structures with a higher
      lock level than the one that will be used for the main step of algorithm.
      After that we downgrade the lock if it is necessary.
   *) After that, the main step of this phase and algorithm is executed.
      We call the handler::ha_inplace_alter_table() method, which carries out the
      changes requested by ALTER TABLE but does not makes them visible to other
      connections yet.
   *) We ensure that no other connection uses the table by upgrading our
      lock on it to exclusive.
   *) a) If the previous step succeeds, handler::ha_commit_inplace_alter_table() is
         called to allow the storage engine to do any final updates to its structures,
         to make all earlier changes durable and visible to other connections.
      b) If we have failed to upgrade lock or any errors have occurred during the
         handler functions calls (including commit), we call
         handler::ha_commit_inplace_alter_table()
         to rollback all changes which were done during previous steps.

  Phase 3 : Final
  ===============

  In this phase we:

  *) Update SQL-layer data-dictionary by installing .FRM file for the new version
     of the table.
  *) Inform the storage engine about this change by calling the
     hton::notify_table_changed()
  *) Destroy the Alter_inplace_info and handler_ctx objects.

 */

 /**
    Check if a storage engine supports a particular alter table in-place

    @param    altered_table     TABLE object for new version of table.
    @param    ha_alter_info     Structure describing changes to be done
                                by ALTER TABLE and holding data used
                                during in-place alter.

    @retval   HA_ALTER_ERROR                  Unexpected error.
    @retval   HA_ALTER_INPLACE_NOT_SUPPORTED  Not supported, must use copy.
    @retval   HA_ALTER_INPLACE_EXCLUSIVE_LOCK Supported, but requires X lock.
    @retval   HA_ALTER_INPLACE_COPY_LOCK
                                              Supported, but requires SNW lock
                                              during main phase. Prepare phase
                                              requires X lock.
    @retval   HA_ALTER_INPLACE_SHARED_LOCK    Supported, but requires SNW lock.
    @retval   HA_ALTER_INPLACE_COPY_NO_LOCK
                                              Supported, concurrent reads/writes
                                              allowed. However, prepare phase
                                              requires X lock.
    @retval   HA_ALTER_INPLACE_NO_LOCK        Supported, concurrent
                                              reads/writes allowed.

    @note The default implementation uses the old in-place ALTER API
    to determine if the storage engine supports in-place ALTER or not.

    @note Called without holding thr_lock.c lock.
 */
 virtual enum_alter_inplace_result
 check_if_supported_inplace_alter(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info);


 /**
    Public functions wrapping the actual handler call.
    @see prepare_inplace_alter_table()
 */
 bool ha_prepare_inplace_alter_table(TABLE *altered_table,
                                     Alter_inplace_info *ha_alter_info);


 /**
    Public function wrapping the actual handler call.
    @see inplace_alter_table()
 */
 bool ha_inplace_alter_table(TABLE *altered_table,
                             Alter_inplace_info *ha_alter_info)
 {
   return inplace_alter_table(altered_table, ha_alter_info);
 }


 /**
    Public function wrapping the actual handler call.
    Allows us to enforce asserts regardless of handler implementation.
    @see commit_inplace_alter_table()
 */
 bool ha_commit_inplace_alter_table(TABLE *altered_table,
                                    Alter_inplace_info *ha_alter_info,
                                    bool commit);


protected:
 /**
    Allows the storage engine to update internal structures with concurrent
    writes blocked. If check_if_supported_inplace_alter() returns
    HA_ALTER_INPLACE_COPY_NO_LOCK or HA_ALTER_INPLACE_COPY_LOCK,
    this function is called with exclusive lock otherwise the same level
    of locking as for inplace_alter_table() will be used.

    @note Storage engines are responsible for reporting any errors by
    calling my_error()/print_error()

    @note If this function reports error, commit_inplace_alter_table()
    will be called with commit= false.

    @note For partitioning, failing to prepare one partition, means that
    commit_inplace_alter_table() will be called to roll back changes for
    all partitions. This means that commit_inplace_alter_table() might be
    called without prepare_inplace_alter_table() having been called first
    for a given partition.

    @param    altered_table     TABLE object for new version of table.
    @param    ha_alter_info     Structure describing changes to be done
                                by ALTER TABLE and holding data used
                                during in-place alter.

    @retval   true              Error
    @retval   false             Success
 */
 virtual bool prepare_inplace_alter_table(TABLE *altered_table,
                                          Alter_inplace_info *ha_alter_info)
 { return false; }


 /**
    Alter the table structure in-place with operations specified using HA_ALTER_FLAGS
    and Alter_inplace_info. The level of concurrency allowed during this
    operation depends on the return value from check_if_supported_inplace_alter().

    @note Storage engines are responsible for reporting any errors by
    calling my_error()/print_error()

    @note If this function reports error, commit_inplace_alter_table()
    will be called with commit= false.

    @param    altered_table     TABLE object for new version of table.
    @param    ha_alter_info     Structure describing changes to be done
                                by ALTER TABLE and holding data used
                                during in-place alter.

    @retval   true              Error
    @retval   false             Success
 */
 virtual bool inplace_alter_table(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info)
 { return false; }


 /**
    Commit or rollback the changes made during prepare_inplace_alter_table()
    and inplace_alter_table() inside the storage engine.
    Note that in case of rollback the allowed level of concurrency during
    this operation will be the same as for inplace_alter_table() and thus
    might be higher than during prepare_inplace_alter_table(). (For example,
    concurrent writes were blocked during prepare, but might not be during
    rollback).

    @note Storage engines are responsible for reporting any errors by
    calling my_error()/print_error()

    @note If this function with commit= true reports error, it will be called
    again with commit= false.

    @note In case of partitioning, this function might be called for rollback
    without prepare_inplace_alter_table() having been called first.
    Also partitioned tables sets ha_alter_info->group_commit_ctx to a NULL
    terminated array of the partitions handlers and if all of them are
    committed as one, then group_commit_ctx should be set to NULL to indicate
    to the partitioning handler that all partitions handlers are committed.
    @see prepare_inplace_alter_table().

    @param    altered_table     TABLE object for new version of table.
    @param    ha_alter_info     Structure describing changes to be done
                                by ALTER TABLE and holding data used
                                during in-place alter.
    @param    commit            True => Commit, False => Rollback.

    @retval   true              Error
    @retval   false             Success
 */
 virtual bool commit_inplace_alter_table(TABLE *altered_table,
                                         Alter_inplace_info *ha_alter_info,
                                         bool commit)
{
  /* Nothing to commit/rollback, mark all handlers committed! */
  ha_alter_info->group_commit_ctx= NULL;
  return false;
}

public:
 /* End of On-line/in-place ALTER TABLE interface. */


  /**
    use_hidden_primary_key() is called in case of an update/delete when
    (table_flags() and HA_PRIMARY_KEY_REQUIRED_FOR_DELETE) is defined
    but we don't have a primary key
  */
  virtual void use_hidden_primary_key();
  virtual alter_table_operations alter_table_flags(alter_table_operations flags)
  {
    if (ht->alter_table_flags)
      return ht->alter_table_flags(flags);
    return 0;
  }

  virtual LEX_CSTRING *engine_name();
  
  TABLE* get_table() { return table; }
  TABLE_SHARE* get_table_share() { return table_share; }
protected:
  /* Service methods for use by storage engines. */
  THD *ha_thd(void) const;

  /**
    Acquire the instrumented table information from a table share.
    @return an instrumented table share, or NULL.
  */
  PSI_table_share *ha_table_share_psi() const;

  /**
    Default rename_table() and delete_table() rename/delete files with a
    given name and extensions from bas_ext().

    These methods can be overridden, but their default implementation
    provide useful functionality.
  */
  virtual int rename_table(const char *from, const char *to);


public:
  /**
    Delete a table in the engine. Called for base as well as temporary
    tables.
  */
  virtual int delete_table(const char *name);
  bool check_table_binlog_row_based();
  bool prepare_for_row_logging();
  int prepare_for_insert(bool do_create);
  int binlog_log_row(const uchar *before_record,
                     const uchar *after_record,
                     Log_func *log_func);

  inline void clear_cached_table_binlog_row_based_flag()
  {
    check_table_binlog_row_based_done= 0;
  }
  virtual void handler_stats_updated() {}

  inline void ha_handler_stats_reset()
  {
    handler_stats= &active_handler_stats;
    active_handler_stats.reset();
    active_handler_stats.active= 1;
    handler_stats_updated();
  }
  inline void ha_handler_stats_disable()
  {
    handler_stats= 0;
    active_handler_stats.active= 0;
    handler_stats_updated();
  }

private:
  /* Cache result to avoid extra calls */
  inline void mark_trx_read_write()
  {
    if (unlikely(!mark_trx_read_write_done))
    {
      mark_trx_read_write_done= 1;
      mark_trx_read_write_internal();
    }
  }

  void mark_trx_read_write_internal();
  bool check_table_binlog_row_based_internal();

  int create_lookup_handler();
  void alloc_lookup_buffer();
  int check_duplicate_long_entries(const uchar *new_rec);
  int check_duplicate_long_entries_update(const uchar *new_rec);
  int check_duplicate_long_entry_key(const uchar *new_rec, uint key_no);
  /** PRIMARY KEY/UNIQUE WITHOUT OVERLAPS check */
  int ha_check_overlaps(const uchar *old_data, const uchar* new_data);

protected:
  /*
    These are intended to be used only by handler::ha_xxxx() functions
    However, engines that implement read_range_XXX() (like MariaRocks)
    or embed other engines (like ha_partition) may need to call these also
  */
  /*
    Increment statistics. As a side effect increase accessed_rows_and_keys
    and checks if lex->limit_rows_examined_cnt is reached
  */
  inline void increment_statistics(ulong SSV::*offset) const;
  /* Same as increment_statistics but doesn't increase accessed_rows_and_keys */
  inline void fast_increment_statistics(ulong SSV::*offset) const;
  inline void decrement_statistics(ulong SSV::*offset) const;

private:
  /*
    Low-level primitives for storage engines.  These should be
    overridden by the storage engine class. To call these methods, use
    the corresponding 'ha_*' method above.
  */

  virtual int open(const char *name, int mode, uint test_if_locked)=0;
  /* Note: ha_index_read_idx_map() may bypass index_init() */
  virtual int index_init(uint idx, bool sorted) { return 0; }
  virtual int index_end() { return 0; }
  /**
    rnd_init() can be called two times without rnd_end() in between
    (it only makes sense if scan=1).
    then the second call should prepare for the new table scan (e.g
    if rnd_init allocates the cursor, second call should position it
    to the start of the table, no need to deallocate and allocate it again
  */
  virtual int rnd_init(bool scan)= 0;
  virtual int rnd_end() { return 0; }
  virtual int write_row(const uchar *buf __attribute__((unused)))
  {
    return HA_ERR_WRONG_COMMAND;
  }

  /**
    Update a single row.

    Note: If HA_ERR_FOUND_DUPP_KEY is returned, the handler must read
    all columns of the row so MySQL can create an error message. If
    the columns required for the error message are not read, the error
    message will contain garbage.
  */
  virtual int update_row(const uchar *old_data __attribute__((unused)),
                         const uchar *new_data __attribute__((unused)))
  {
    return HA_ERR_WRONG_COMMAND;
  }

  /*
    Optimized function for updating the first row. Only used by sequence
    tables
  */
  virtual int update_first_row(const uchar *new_data);

  virtual int delete_row(const uchar *buf __attribute__((unused)))
  {
    return HA_ERR_WRONG_COMMAND;
  }

  /* Perform initialization for a direct update request */
public:
  int ha_direct_update_rows(ha_rows *update_rows, ha_rows *found_rows);
  virtual int direct_update_rows_init(List<Item> *update_fields)
  {
    return HA_ERR_WRONG_COMMAND;
  }
private:
  virtual int pre_direct_update_rows_init(List<Item> *update_fields)
  {
    return HA_ERR_WRONG_COMMAND;
  }
  virtual int direct_update_rows(ha_rows *update_rows __attribute__((unused)),
                                 ha_rows *found_rows __attribute__((unused)))
  {
    return HA_ERR_WRONG_COMMAND;
  }
  virtual int pre_direct_update_rows()
  {
    return HA_ERR_WRONG_COMMAND;
  }

  /* Perform initialization for a direct delete request */
public:
  int ha_direct_delete_rows(ha_rows *delete_rows);
  virtual int direct_delete_rows_init()
  {
    return HA_ERR_WRONG_COMMAND;
  }
private:
  virtual int pre_direct_delete_rows_init()
  {
    return HA_ERR_WRONG_COMMAND;
  }
  virtual int direct_delete_rows(ha_rows *delete_rows __attribute__((unused)))
  {
    return HA_ERR_WRONG_COMMAND;
  }
  virtual int pre_direct_delete_rows()
  {
    return HA_ERR_WRONG_COMMAND;
  }

  /**
    Reset state of file to after 'open'.
    This function is called after every statement for all tables used
    by that statement.
  */
  virtual int reset() { return 0; }
  virtual Table_flags table_flags(void) const= 0;
  /**
    Is not invoked for non-transactional temporary tables.

    Tells the storage engine that we intend to read or write data
    from the table. This call is prefixed with a call to handler::store_lock()
    and is invoked only for those handler instances that stored the lock.

    Calls to rnd_init/index_init are prefixed with this call. When table
    IO is complete, we call external_lock(F_UNLCK).
    A storage engine writer should expect that each call to
    ::external_lock(F_[RD|WR]LOCK is followed by a call to
    ::external_lock(F_UNLCK). If it is not, it is a bug in MySQL.

    The name and signature originate from the first implementation
    in MyISAM, which would call fcntl to set/clear an advisory
    lock on the data file in this method.

    @param   lock_type    F_RDLCK, F_WRLCK, F_UNLCK

    @return  non-0 in case of failure, 0 in case of success.
    When lock_type is F_UNLCK, the return value is ignored.
  */
  virtual int external_lock(THD *thd __attribute__((unused)),
                            int lock_type __attribute__((unused)))
  {
    return 0;
  }
  virtual void release_auto_increment() { return; };
  /** admin commands - called from mysql_admin_table */
  virtual int check_for_upgrade(HA_CHECK_OPT *check_opt)
  { return 0; }
  virtual int check(THD* thd, HA_CHECK_OPT* check_opt)
  { return HA_ADMIN_NOT_IMPLEMENTED; }

  /**
     In this method check_opt can be modified
     to specify CHECK option to use to call check()
     upon the table.
  */
  virtual int repair(THD* thd, HA_CHECK_OPT* check_opt)
  {
    DBUG_ASSERT(!(ha_table_flags() & HA_CAN_REPAIR));
    return HA_ADMIN_NOT_IMPLEMENTED;
  }
protected:
  virtual void start_bulk_insert(ha_rows rows, uint flags) {}
  virtual int end_bulk_insert() { return 0; }
  virtual int index_read(uchar * buf, const uchar * key, uint key_len,
                         enum ha_rkey_function find_flag)
   { return  HA_ERR_WRONG_COMMAND; }
  virtual int index_read_last(uchar * buf, const uchar * key, uint key_len)
  {
    my_errno= HA_ERR_WRONG_COMMAND;
    return HA_ERR_WRONG_COMMAND;
  }
  friend class ha_partition;
  friend class ha_sequence;
public:
  /**
    This method is similar to update_row, however the handler doesn't need
    to execute the updates at this point in time. The handler can be certain
    that another call to bulk_update_row will occur OR a call to
    exec_bulk_update before the set of updates in this query is concluded.

    @param    old_data       Old record
    @param    new_data       New record
    @param    dup_key_found  Number of duplicate keys found

    @retval  0   Bulk delete used by handler
    @retval  1   Bulk delete not used, normal operation used
  */
  virtual int bulk_update_row(const uchar *old_data, const uchar *new_data,
                              ha_rows *dup_key_found)
  {
    DBUG_ASSERT(FALSE);
    return HA_ERR_WRONG_COMMAND;
  }
  /**
    This is called to delete all rows in a table
    If the handler don't support this, then this function will
    return HA_ERR_WRONG_COMMAND and MySQL will delete the rows one
    by one.
  */
  virtual int delete_all_rows()
  { return (my_errno=HA_ERR_WRONG_COMMAND); }
  /**
    Quickly remove all rows from a table.

    @remark This method is responsible for implementing MySQL's TRUNCATE
            TABLE statement, which is a DDL operation. As such, a engine
            can bypass certain integrity checks and in some cases avoid
            fine-grained locking (e.g. row locks) which would normally be
            required for a DELETE statement.

    @remark Typically, truncate is not used if it can result in integrity
            violation. For example, truncate is not used when a foreign
            key references the table, but it might be used if foreign key
            checks are disabled.

    @remark Engine is responsible for resetting the auto-increment counter.

    @remark The table is locked in exclusive mode.
  */
  virtual int truncate()
  {
    int error= delete_all_rows();
    return error ? error : reset_auto_increment(0);
  }
  /**
    Reset the auto-increment counter to the given value, i.e. the next row
    inserted will get the given value.
  */
  virtual int reset_auto_increment(ulonglong value)
  { return 0; }
  virtual int optimize(THD* thd, HA_CHECK_OPT* check_opt)
  { return HA_ADMIN_NOT_IMPLEMENTED; }
  virtual int analyze(THD* thd, HA_CHECK_OPT* check_opt)
  { return HA_ADMIN_NOT_IMPLEMENTED; }
  virtual bool check_and_repair(THD *thd) { return TRUE; }
  virtual int disable_indexes(uint mode) { return HA_ERR_WRONG_COMMAND; }
  virtual int enable_indexes(uint mode) { return HA_ERR_WRONG_COMMAND; }
  virtual int discard_or_import_tablespace(my_bool discard)
  { return (my_errno=HA_ERR_WRONG_COMMAND); }
  virtual void drop_table(const char *name);
  virtual int create(const char *name, TABLE *form, HA_CREATE_INFO *info)=0;

  virtual int create_partitioning_metadata(const char *name,
                                           const char *old_name,
                                           chf_create_flags action_flag)
  { return FALSE; }

  virtual int change_partitions(HA_CREATE_INFO *create_info,
                                const char *path,
                                ulonglong * const copied,
                                ulonglong * const deleted,
                                const uchar *pack_frm_data,
                                size_t pack_frm_len)
  { return HA_ERR_WRONG_COMMAND; }
  /* @return true if it's necessary to switch current statement log format from
   STATEMENT to ROW if binary log format is MIXED and autoincrement values
   are changed in the statement */
  virtual bool autoinc_lock_mode_stmt_unsafe() const
  { return false; }
  virtual int drop_partitions(const char *path)
  { return HA_ERR_WRONG_COMMAND; }
  virtual int rename_partitions(const char *path)
  { return HA_ERR_WRONG_COMMAND; }
  virtual bool set_ha_share_ref(Handler_share **arg_ha_share)
  {
    DBUG_ASSERT(!ha_share);
    DBUG_ASSERT(arg_ha_share);
    if (ha_share || !arg_ha_share)
      return true;
    ha_share= arg_ha_share;
    return false;
  }
  inline void set_table(TABLE* table_arg);
  int get_lock_type() const { return m_lock_type; }
public:
  /* XXX to be removed, see ha_partition::partition_ht() */
  virtual handlerton *partition_ht() const
  { return ht; }
  virtual bool partition_engine() { return 0;}
  inline int ha_write_tmp_row(uchar *buf);
  inline int ha_delete_tmp_row(uchar *buf);
  inline int ha_update_tmp_row(const uchar * old_data, uchar * new_data);

  virtual void set_lock_type(enum thr_lock_type lock);
  friend check_result_t handler_index_cond_check(void* h_arg);
  friend check_result_t handler_rowid_filter_check(void *h_arg);

  /**
    Find unique record by index or unique constrain

    @param record        record to find (also will be fillded with
                         actual record fields)
    @param unique_ref    index or unique constraiun number (depends
                         on what used in the engine

    @retval -1 Error
    @retval  1 Not found
    @retval  0 Found
  */
  virtual int find_unique_row(uchar *record, uint unique_ref)
  { return -1; /*unsupported */}

  bool native_versioned() const
  { DBUG_ASSERT(ht); return partition_ht()->flags & HTON_NATIVE_SYS_VERSIONING; }
  virtual void update_partition(uint	part_id)
  {}

  /**
    Some engines can perform column type conversion with ALGORITHM=INPLACE.
    These functions check for such possibility.
    Implementation could be based on Field_xxx::is_equal()
   */
  virtual bool can_convert_nocopy(const Field &,
                                  const Column_definition &) const
  {
    return false;
  }
  /* If the table is using sql level unique constraints on some column */
  inline bool has_long_unique();

  /* Used for ALTER TABLE.
  Some engines can handle some differences in indexes by themself. */
  virtual Compare_keys compare_key_parts(const Field &old_field,
                                         const Column_definition &new_field,
                                         const KEY_PART_INFO &old_part,
                                         const KEY_PART_INFO &new_part) const;


/*
  If lower_case_table_names == 2 (case-preserving but case-insensitive
  file system) and the storage is not HA_FILE_BASED, we need to provide
  a lowercase file name for the engine.
*/
  inline bool needs_lower_case_filenames()
  {
    return (lower_case_table_names == 2 && !(ha_table_flags() & HA_FILE_BASED));
  }

  bool log_not_redoable_operation(const char *operation);

protected:
  Handler_share *get_ha_share_ptr();
  void set_ha_share_ptr(Handler_share *arg_ha_share);
  void lock_shared_ha_data();
  void unlock_shared_ha_data();

  /*
    Mroonga needs to call some xxx_time() directly for it's internal handler
    methods
  */
  friend class ha_mroonga;
};

#include "multi_range_read.h"
#include "group_by_handler.h"

bool key_uses_partial_cols(TABLE_SHARE *table, uint keyno);

	/* Some extern variables used with handlers */

extern const LEX_CSTRING ha_row_type[];
extern MYSQL_PLUGIN_IMPORT const char *tx_isolation_names[];
extern MYSQL_PLUGIN_IMPORT const char *binlog_format_names[];
extern TYPELIB tx_isolation_typelib;
extern const char *myisam_stats_method_names[];
extern ulong total_ha, total_ha_2pc;

/* lookups */
plugin_ref ha_resolve_by_name(THD *thd, const LEX_CSTRING *name, bool tmp_table);
plugin_ref ha_lock_engine(THD *thd, const handlerton *hton);
handlerton *ha_resolve_by_legacy_type(THD *thd, enum legacy_db_type db_type);
handler *get_new_handler(TABLE_SHARE *share, MEM_ROOT *alloc,
                         handlerton *db_type);
handlerton *ha_checktype(THD *thd, handlerton *hton, bool no_substitute);

static inline handlerton *ha_checktype(THD *thd, enum legacy_db_type type,
                                       bool no_substitute = 0)
{
  return ha_checktype(thd, ha_resolve_by_legacy_type(thd, type), no_substitute);
}

static inline enum legacy_db_type ha_legacy_type(const handlerton *db_type)
{
  return (db_type == NULL) ? DB_TYPE_UNKNOWN : db_type->db_type;
}

static inline const char *ha_resolve_storage_engine_name(const handlerton *db_type)
{
  return (db_type == NULL ? "UNKNOWN" :
          db_type == view_pseudo_hton ? "VIEW" : hton_name(db_type)->str);
}

static inline bool ha_check_storage_engine_flag(const handlerton *db_type, uint32 flag)
{
  return db_type && (db_type->flags & flag);
}

static inline bool ha_storage_engine_is_enabled(const handlerton *db_type)
{
  return db_type && db_type->create;
}

/* basic stuff */
int ha_init_errors(void);
int ha_init(void);
int ha_end(void);
int ha_initialize_handlerton(st_plugin_int *plugin);
int ha_finalize_handlerton(st_plugin_int *plugin);

TYPELIB *ha_known_exts(void);
int ha_panic(enum ha_panic_function flag);
void ha_close_connection(THD* thd);
void ha_kill_query(THD* thd, enum thd_kill_levels level);
void ha_signal_ddl_recovery_done();
bool ha_flush_logs();
void ha_drop_database(const char* path);
void ha_checkpoint_state(bool disable);
void ha_commit_checkpoint_request(void *cookie, void (*pre_hook)(void *));
int ha_create_table(THD *thd, const char *path, const char *db,
                    const char *table_name, HA_CREATE_INFO *create_info,
                    LEX_CUSTRING *frm, bool skip_frm_file);
int ha_delete_table(THD *thd, handlerton *db_type, const char *path,
                    const LEX_CSTRING *db, const LEX_CSTRING *alias,
                    bool generate_warning);
int ha_delete_table_force(THD *thd, const char *path, const LEX_CSTRING *db,
                          const LEX_CSTRING *alias);

void ha_prepare_for_backup();
void ha_end_backup();
void ha_pre_shutdown();

void ha_disable_internal_writes(bool disable);

/* statistics and info */
bool ha_show_status(THD *thd, handlerton *db_type, enum ha_stat_type stat);

/* discovery */
#ifdef MYSQL_SERVER
class Discovered_table_list: public handlerton::discovered_list
{
  THD *thd;
  const char *wild, *wend;
  bool with_temps; // whether to include temp tables in the result
public:
  Dynamic_array<LEX_CSTRING*> *tables;

  Discovered_table_list(THD *thd_arg, Dynamic_array<LEX_CSTRING*> *tables_arg,
                        const LEX_CSTRING *wild_arg);
  Discovered_table_list(THD *thd_arg, Dynamic_array<LEX_CSTRING*> *tables_arg)
    : thd(thd_arg), wild(NULL), with_temps(true), tables(tables_arg) {}
  ~Discovered_table_list() = default;

  bool add_table(const char *tname, size_t tlen);
  bool add_file(const char *fname);

  void sort();
  void remove_duplicates(); // assumes that the list is sorted
#ifndef DBUG_OFF
  /*
     Used to find unstable mtr tests querying
     INFORMATION_SCHEMA.TABLES without ORDER BY.
  */
  void sort_desc();
#endif /* DBUG_OFF */
};

int ha_discover_table(THD *thd, TABLE_SHARE *share);
int ha_discover_table_names(THD *thd, LEX_CSTRING *db, MY_DIR *dirp,
                            Discovered_table_list *result, bool reusable);
bool ha_table_exists(THD *thd, const LEX_CSTRING *db,
                     const LEX_CSTRING *table_name,
                     LEX_CUSTRING *table_version= 0,
                     LEX_CSTRING *partition_engine_name= 0,
                     handlerton **hton= 0, bool *is_sequence= 0);
bool ha_check_if_updates_are_ignored(THD *thd, handlerton *hton,
                                     const char *op);
#endif /* MYSQL_SERVER */

/* key cache */
extern "C" int ha_init_key_cache(const char *name, KEY_CACHE *key_cache, void *);
int ha_resize_key_cache(KEY_CACHE *key_cache);
int ha_change_key_cache_param(KEY_CACHE *key_cache);
int ha_repartition_key_cache(KEY_CACHE *key_cache);
int ha_change_key_cache(KEY_CACHE *old_key_cache, KEY_CACHE *new_key_cache);

/* transactions: interface to handlerton functions */
int ha_start_consistent_snapshot(THD *thd);
int ha_commit_or_rollback_by_xid(XID *xid, bool commit);
int ha_commit_one_phase(THD *thd, bool all);
int ha_commit_trans(THD *thd, bool all);
int ha_rollback_trans(THD *thd, bool all);
int ha_prepare(THD *thd);
int ha_recover(HASH *commit_list, MEM_ROOT *mem_root= NULL);
uint ha_recover_complete(HASH *commit_list, Binlog_offset *coord= NULL);

/* transactions: these functions never call handlerton functions directly */
int ha_enable_transaction(THD *thd, bool on);

/* savepoints */
int ha_rollback_to_savepoint(THD *thd, SAVEPOINT *sv);
bool ha_rollback_to_savepoint_can_release_mdl(THD *thd);
int ha_savepoint(THD *thd, SAVEPOINT *sv);
int ha_release_savepoint(THD *thd, SAVEPOINT *sv);
#ifdef WITH_WSREP
int ha_abort_transaction(THD *bf_thd, THD *victim_thd, my_bool signal);
#endif

/* these are called by storage engines */
void trans_register_ha(THD *thd, bool all, handlerton *ht,
                       ulonglong trxid);

/*
  Storage engine has to assume the transaction will end up with 2pc if
   - there is more than one 2pc-capable storage engine available
   - in the current transaction 2pc was not disabled yet
*/
#define trans_need_2pc(thd, all)                   ((total_ha_2pc > 1) && \
        !((all ? &thd->transaction.all : &thd->transaction.stmt)->no_2pc))

const char *get_canonical_filename(handler *file, const char *path,
                                   char *tmp_path);
void commit_checkpoint_notify_ha(void *cookie);

inline const LEX_CSTRING *table_case_name(HA_CREATE_INFO *info, const LEX_CSTRING *name)
{
  return ((lower_case_table_names == 2 && info->alias.str) ? &info->alias : name);
}


/**
  @def MYSQL_TABLE_IO_WAIT
  Instrumentation helper for table io_waits.
  Note that this helper is intended to be used from
  within the handler class only, as it uses members
  from @c handler
  Performance schema events are instrumented as follows:
  - in non batch mode, one event is generated per call
  - in batch mode, the number of rows affected is saved
  in @c m_psi_numrows, so that @c end_psi_batch_mode()
  generates a single event for the batch.
  @param OP the table operation to be performed
  @param INDEX the table index used if any, or MAX_KEY.
  @param PAYLOAD instrumented code to execute
  @sa handler::end_psi_batch_mode.
*/
#ifdef HAVE_PSI_TABLE_INTERFACE
  #define MYSQL_TABLE_IO_WAIT(OP, INDEX, RESULT, PAYLOAD)     \
    {                                                         \
      if (m_psi != NULL)                                      \
      {                                                       \
        switch (m_psi_batch_mode)                             \
        {                                                     \
          case PSI_BATCH_MODE_NONE:                           \
          {                                                   \
            PSI_table_locker *sub_locker= NULL;               \
            PSI_table_locker_state reentrant_safe_state;      \
            sub_locker= PSI_TABLE_CALL(start_table_io_wait)   \
              (& reentrant_safe_state, m_psi, OP, INDEX,      \
               __FILE__, __LINE__);                           \
            PAYLOAD                                           \
            if (sub_locker != NULL)                           \
              PSI_TABLE_CALL(end_table_io_wait)               \
                (sub_locker, 1);                              \
            break;                                            \
          }                                                   \
          case PSI_BATCH_MODE_STARTING:                       \
          {                                                   \
            m_psi_locker= PSI_TABLE_CALL(start_table_io_wait) \
              (& m_psi_locker_state, m_psi, OP, INDEX,        \
               __FILE__, __LINE__);                           \
            PAYLOAD                                           \
            if (!RESULT)                                      \
              m_psi_numrows++;                                \
            m_psi_batch_mode= PSI_BATCH_MODE_STARTED;         \
            break;                                            \
          }                                                   \
          case PSI_BATCH_MODE_STARTED:                        \
          default:                                            \
          {                                                   \
            DBUG_ASSERT(m_psi_batch_mode                      \
                        == PSI_BATCH_MODE_STARTED);           \
            PAYLOAD                                           \
            if (!RESULT)                                      \
              m_psi_numrows++;                                \
            break;                                            \
          }                                                   \
        }                                                     \
      }                                                       \
      else                                                    \
      {                                                       \
        PAYLOAD                                               \
      }                                                       \
    }
#else
  #define MYSQL_TABLE_IO_WAIT(OP, INDEX, RESULT, PAYLOAD) \
    PAYLOAD
#endif

#define TABLE_IO_WAIT(TRACKER, OP, INDEX, RESULT, PAYLOAD) \
  { \
    Exec_time_tracker *this_tracker; \
    if (unlikely((this_tracker= tracker))) \
      tracker->start_tracking(table->in_use); \
    \
    MYSQL_TABLE_IO_WAIT(OP, INDEX, RESULT, PAYLOAD); \
    \
    if (unlikely(this_tracker)) \
      tracker->stop_tracking(table->in_use); \
  }
void print_keydup_error(TABLE *table, KEY *key, const char *msg, myf errflag);
void print_keydup_error(TABLE *table, KEY *key, myf errflag);

int del_global_index_stat(THD *thd, TABLE* table, KEY* key_info);
int del_global_table_stat(THD *thd, const  LEX_CSTRING *db, const LEX_CSTRING *table);
uint ha_count_rw_all(THD *thd, Ha_trx_info **ptr_ha_info);
bool non_existing_table_error(int error);
uint ha_count_rw_2pc(THD *thd, bool all);
uint ha_check_and_coalesce_trx_read_only(THD *thd, Ha_trx_info *ha_list,
                                         bool all);

inline void Cost_estimate::reset(handler *file)
{
  reset();
  avg_io_cost= file->DISK_READ_COST * file->DISK_READ_RATIO;
}

#endif /* HANDLER_INCLUDED */
