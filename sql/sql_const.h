/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

/**
  @file
  File containing constants that can be used throughout the server.

  @note This file shall not contain or include any declarations of any kinds.
*/

#ifndef SQL_CONST_INCLUDED
#define SQL_CONST_INCLUDED

#include <mysql_version.h>

#define LIBLEN FN_REFLEN-FN_LEN			/* Max l{ngd p} dev */
/* extra 4+4 bytes for slave tmp tables */
#define MAX_DBKEY_LENGTH (NAME_LEN*2+1+1+4+4)
#define MAX_ALIAS_NAME 256
#define MAX_FIELD_NAME 34			/* Max colum name length +2 */
#define MAX_SYS_VAR_LENGTH 32
#define MAX_KEY MAX_INDEXES                     /* Max used keys */
#define MAX_REF_PARTS 32			/* Max parts used as ref */

/*
  Maximum length of the data part of an index lookup key.

  The "data part" is defined as the value itself, not including the
  NULL-indicator bytes or varchar length bytes ("the Extras"). We need this
  value because there was a bug where length of the Extras were not counted.

  You probably need MAX_KEY_LENGTH, not this constant.
*/
#define MAX_DATA_LENGTH_FOR_KEY 3072
#if SIZEOF_OFF_T > 4
#define MAX_REFLENGTH 8				/* Max length for record ref */
#else
#define MAX_REFLENGTH 4				/* Max length for record ref */
#endif
#define MAX_HOSTNAME  (HOSTNAME_LENGTH + 1)	/* len+1 in mysql.user */
#define MAX_CONNECTION_NAME NAME_LEN

#define MAX_MBWIDTH		3		/* Max multibyte sequence */
#define MAX_FILENAME_MBWIDTH    5
#define MAX_FIELD_CHARLENGTH	255
/*
  In MAX_FIELD_VARCHARLENGTH we reserve extra bytes for the overhead:
  - 2 bytes for the length
  - 1 byte for NULL bits
  to avoid the "Row size too large" error for these three corner definitions:
    CREATE TABLE t1 (c VARBINARY(65533));
    CREATE TABLE t1 (c VARBINARY(65534));
    CREATE TABLE t1 (c VARBINARY(65535));
  Like VARCHAR(65536), they will be converted to BLOB automatically
  in non-strict mode.
*/
#define MAX_FIELD_VARCHARLENGTH	(65535-2-1)
#define MAX_FIELD_BLOBLENGTH UINT_MAX32         /* cf field_blob::get_length() */
#define CONVERT_IF_BIGGER_TO_BLOB 512           /* Threshold *in characters*   */

/* Max column width +1 */
#define MAX_FIELD_WIDTH		(MAX_FIELD_CHARLENGTH*MAX_MBWIDTH+1)

#define MAX_BIT_FIELD_LENGTH    64      /* Max length in bits for bit fields */

#define MAX_DATE_WIDTH		10	/* YYYY-MM-DD */
#define MIN_TIME_WIDTH          10      /* -HHH:MM:SS */
#define MAX_TIME_WIDTH          16      /* -DDDDDD HH:MM:SS */
#define MAX_TIME_FULL_WIDTH     23      /* -DDDDDD HH:MM:SS.###### */
#define MAX_DATETIME_FULL_WIDTH 26	/* YYYY-MM-DD HH:MM:SS.###### */
#define MAX_DATETIME_WIDTH	19	/* YYYY-MM-DD HH:MM:SS */
#define MAX_DATETIME_COMPRESSED_WIDTH 14  /* YYYYMMDDHHMMSS */
#define MAX_DATETIME_PRECISION  6

#define MAX_TABLES	(sizeof(table_map)*8-3)	/* Max tables in join */
#define PARAM_TABLE_BIT	(((table_map) 1) << (sizeof(table_map)*8-3))
#define OUTER_REF_TABLE_BIT	(((table_map) 1) << (sizeof(table_map)*8-2))
#define RAND_TABLE_BIT	(((table_map) 1) << (sizeof(table_map)*8-1))
#define PSEUDO_TABLE_BITS (PARAM_TABLE_BIT | OUTER_REF_TABLE_BIT | \
                           RAND_TABLE_BIT)
#define CONNECT_STRING_MAXLEN   65535           /* stored in 2 bytes in .frm */
#define MAX_FIELDS	4096			/* Limit in the .frm file */
#define MAX_PARTITIONS  8192

#define MAX_SELECT_NESTING (SELECT_NESTING_MAP_SIZE - 1)

#define MAX_SORT_MEMORY 2048*1024
#define MIN_SORT_MEMORY 1024

/* Some portable defines */

#define STRING_BUFFER_USUAL_SIZE 80

/* Memory allocated when parsing a statement / saving a statement */
#define MEM_ROOT_BLOCK_SIZE       8192
#define MEM_ROOT_PREALLOC         8192
#define TRANS_MEM_ROOT_BLOCK_SIZE 4096
#define TRANS_MEM_ROOT_PREALLOC   4096

#define DEFAULT_ERROR_COUNT	64
#define EXTRA_RECORDS	10			/* Extra records in sort */
#define SCROLL_EXTRA	5			/* Extra scroll-rows. */
#define FIELD_NAME_USED ((uint) 32768)		/* Bit set if fieldname used */
#define FORM_NAME_USED	((uint) 16384)		/* Bit set if formname used */
#define FIELD_NR_MASK	16383			/* To get fieldnumber */
#define FERR		-1			/* Error from my_functions */
#define CREATE_MODE	0			/* Default mode on new files */
#define NAMES_SEP_CHAR	255			/* Char to sep. names */

/*
   This is used when reading large blocks, sequential read.
   We assume that reading this much will be roughly the same cost as 1
   seek / fetching one row from the storage engine.
   Cost of one read of DISK_CHUNK_SIZE is DISK_SEEK_BASE_COST (ms).
*/
#define DISK_CHUNK_SIZE	(uint) (65536) /* Size of diskbuffer for tmpfiles */
#define TMPFILE_CREATE_COST        2.0  /* Creating and deleting tmp file */

#define FRM_VER_TRUE_VARCHAR (FRM_VER+4) /* 10 */
#define FRM_VER_EXPRESSSIONS (FRM_VER+5) /* 11 */
#define FRM_VER_CURRENT  FRM_VER_EXPRESSSIONS

/***************************************************************************
  Configuration parameters
****************************************************************************/

#define ACL_CACHE_SIZE		256
#define MAX_PASSWORD_LENGTH	32
#define HOST_CACHE_SIZE		128
#define MAX_ACCEPT_RETRY	10	// Test accept this many times
#define MAX_FIELDS_BEFORE_HASH	32
#define USER_VARS_HASH_SIZE     16
#define SEQUENCES_HASH_SIZE     16
#define TABLE_OPEN_CACHE_MIN    200
#define TABLE_OPEN_CACHE_DEFAULT 2000
#define TABLE_DEF_CACHE_DEFAULT 400
/**
  We must have room for at least 400 table definitions in the table
  cache, since otherwise there is no chance prepared
  statements that use these many tables can work.
  Prepared statements use table definition cache ids (table_map_id)
  as table version identifiers. If the table definition
  cache size is less than the number of tables used in a statement,
  the contents of the table definition cache is guaranteed to rotate
  between a prepare and execute. This leads to stable validation
  errors. In future we shall use more stable version identifiers,
  for now the only solution is to ensure that the table definition
  cache can contain at least all tables of a given statement.
*/
#define TABLE_DEF_CACHE_MIN     400

/**
 Maximum number of connections default value.
 151 is larger than Apache's default max children,
 to avoid "too many connections" error in a common setup.
*/
#define MAX_CONNECTIONS_DEFAULT 151

/*
  Stack reservation.
  Feel free to raise this by the smallest amount you can to get the
  "execution_constants" test to pass.
*/
#define STACK_MIN_SIZE          16000   // Abort if less stack during eval.

#define STACK_MIN_SIZE_FOR_OPEN (1024*80)
#define STACK_BUFF_ALLOC        352     ///< For stack overrun checks
#ifndef MYSQLD_NET_RETRY_COUNT
#define MYSQLD_NET_RETRY_COUNT  10	///< Abort read after this many int.
#endif

#define QUERY_ALLOC_BLOCK_SIZE		16384
#define QUERY_ALLOC_PREALLOC_SIZE   	24576
#define TRANS_ALLOC_BLOCK_SIZE		8192
#define TRANS_ALLOC_PREALLOC_SIZE	4096
#define RANGE_ALLOC_BLOCK_SIZE		4096
#define ACL_ALLOC_BLOCK_SIZE		1024
#define UDF_ALLOC_BLOCK_SIZE		1024
#define TABLE_ALLOC_BLOCK_SIZE		1024
#define WARN_ALLOC_BLOCK_SIZE		2048
#define WARN_ALLOC_PREALLOC_SIZE	1024
/*
  Note that if we are using 32K or less, then TCmalloc will use a local
  heap without locks!
*/
#define SHOW_ALLOC_BLOCK_SIZE           (32768-MALLOC_OVERHEAD)

/*
  The following parameters is to decide when to use an extra cache to
  optimise seeks when reading a big table in sorted order
*/
#define MIN_FILE_LENGTH_TO_USE_ROW_CACHE (10L*1024*1024)
#define MIN_ROWS_TO_USE_TABLE_CACHE	 100
#define MIN_ROWS_TO_USE_BULK_INSERT	 100

/*
  The table/index cache hit ratio in %. 0 means that a searched for key or row
  will never be in the cache while 100 means it always in the cache.

  According to folklore, one need at least 80 % hit rate in the cache for
  MariaDB to run very well. We set CACHE_HIT_RATIO to a bit smaller
  as there is still a cost involved in finding the row in the B tree, hash
  or other seek structure.

  Increasing CACHE_HIT_RATIO will make MariaDB prefer key lookups over
  table scans as the impact of ROW_COPY_COST and INDEX_COPY cost will
  have a larger impact when more rows are exmined..

  Note that avg_io_cost() is multipled with this constant!
*/
#define DEFAULT_CACHE_HIT_RATIO 80

/* Convert ratio to cost */

static inline double cache_hit_ratio(double ratio)
{
  return ((100.0 - ratio) / 100.0);
}

/*
  All costs should be based on milliseconds (1 cost = 1 ms)
*/

/* Cost for finding the first key in a key scan */
#define DEFAULT_INDEX_LOOKUP_COST    ((double) 0.0005)
/* Modifier for reading a block when doing a table scan */
#define DEFAULT_SCAN_LOOKUP_COST     ((double) 1.0)

/* Cost of finding a key from a row_ID (not used for clustered keys) */
#define DEFAULT_ROW_LOOKUP_COST      ((double) 0.0005)
#define ROW_LOOKUP_COST_THD(thd)     ((thd)->variables.optimizer_row_lookup_cost)

#define INDEX_LOOKUP_COST optimizer_index_lookup_cost
#define ROW_LOOKUP_COST   optimizer_row_lookup_cost
#define SCAN_LOOKUP_COST  optimizer_scan_lookup_cost

/* Default fill factor of an (b-tree) index block */
#define INDEX_BLOCK_FILL_FACTOR 0.75

/*
   These constants impact the cost of QSORT and priority queue sorting,
   scaling the "n * log(n)" operations cost proportionally.
   These factors are < 1.0 to scale down the sorting cost to be comparable
   to 'read a row' = 1.0, (or 0.55 with default caching).
   A factor of 0.1 makes the cost of get_pq_sort_cost(10, 10, false) =0.52
   (Reading 10 rows into a priority queue of 10 elements).

   One consenquence if this factor is too high is that priority_queue will
   not use addon fields (to solve the sort without having to do an extra
   re-read of rows) even if the number of LIMIT is low.
*/
#define QSORT_SORT_SLOWNESS_CORRECTION_FACTOR    (0.1)
#define PQ_SORT_SLOWNESS_CORRECTION_FACTOR       (0.1)

/*
  Creating a record from the join cache is faster than getting a row from
  the engine. JOIN_CACHE_ROW_COPY_COST_FACTOR is the factor used to
  take this into account. This is multiplied with ROW_COPY_COST.
*/
#define JOIN_CACHE_ROW_COPY_COST_FACTOR(thd) (0.75 * ROW_LOOKUP_COST_THD(thd))

/*
  Cost of finding and copying keys from the storage engine index cache to
  an internal cache as part of an index scan. This includes all mutexes
  that needs to be taken to get exclusive access to a page.
  The number is taken from accessing an existing blocks from Aria page cache.
  Used in handler::scan_time() and handler::keyread_time()
*/
#define DEFAULT_INDEX_BLOCK_COPY_COST  ((double) 3.56e-05)
#define INDEX_BLOCK_COPY_COST           optimizer_index_block_copy_cost

/*
  Cost of finding the next row during table scan and copying it to
  'table->record'.
  If this is too small, then table scans will be prefered over 'ref'
  as with table scans there are no key read (INDEX_LOOKUP_COST), fewer
  disk reads but more record copying and row comparisions.  If it's
  too big then MariaDB will used key lookup even when table scan is
  better.
*/
#define DEFAULT_ROW_COPY_COST     ((double)  2.334e-06)
#define ROW_COPY_COST             optimizer_row_copy_cost
#define ROW_COPY_COST_THD(THD)    ((THD)->variables.optimizer_row_copy_cost)

/*
  Cost of finding the next key during index scan and copying it to
  'table->record'

  If this is too small, then index scans will be prefered over 'ref'
  as with table scans there are no key read (INDEX_LOOKUP_COST) and
  fewer disk reads.
*/
#define DEFAULT_KEY_COPY_COST     DEFAULT_ROW_COPY_COST/5
#define KEY_COPY_COST             optimizer_key_copy_cost
#define KEY_COPY_COST_THD(THD)    ((THD)->variables.optimizer_key_copy_cost)

/*
  Cost of finding the next index entry and checking it against filter
  This cost is very low as it's done inside the storage engine.
  Should be smaller than KEY_COPY_COST.
 */
#define DEFAULT_INDEX_NEXT_FIND_COST DEFAULT_KEY_COPY_COST/10
#define INDEX_NEXT_FIND_COST         optimizer_index_next_find_cost

/* Cost of finding the next row when scanning a table */
#define DEFAULT_ROW_NEXT_FIND_COST   DEFAULT_INDEX_NEXT_FIND_COST
#define ROW_NEXT_FIND_COST           optimizer_row_next_find_cost

/**
  The following is used to decide if MariaDB should use table scanning
  instead of reading with keys.  The number says how many evaluation of the
  WHERE clause is comparable to reading one extra row from a table.
*/
#define DEFAULT_WHERE_COST             ((double) 3.2e-05)
#define WHERE_COST                      optimizer_where_cost
#define WHERE_COST_THD(THD)            ((THD)->variables.optimizer_where_cost)

/* The cost of comparing a key when using range access */
#define DEFAULT_KEY_COMPARE_COST        DEFAULT_WHERE_COST/4
#define KEY_COMPARE_COST                optimizer_key_cmp_cost

/*
  Cost of comparing two rowids. This is set relative to KEY_COMPARE_COST
*/
#define ROWID_COMPARE_COST          (KEY_COMPARE_COST/2)
#define ROWID_COMPARE_COST_THD(THD) ((THD)->variables.optimizer_key_cmp_cost)

/*
  Setup cost for different operations
*/

/* Extra cost for doing a range scan. Used to prefer 'ref' over range */
#define MULTI_RANGE_READ_SETUP_COST ((double) 0.2)

/*
  These costs are mainly to handle small tables, like the one we have in the
  mtr test suite
*/
/* Extra cost for full table scan. Used to prefer range over table scans */
#define TABLE_SCAN_SETUP_COST 1.0
/* Extra cost for full index scan. Used to prefer range over index scans */
#define INDEX_SCAN_SETUP_COST 1.0

/*
  The lower bound of accepted rows when using filter.
  This is used to ensure that filters are not too agressive.
*/
#define MIN_ROWS_AFTER_FILTERING 1.0

/*
  cost1 is better that cost2 only if cost1 + COST_EPS < cost2
  The main purpose of this is to ensure we use the first index or plan
  when there are identical plans. Without COST_EPS some plans in the
  test suite would vary depending on floating point calculations done
  in different paths.
 */
#define COST_EPS  0.0000001

/*
  Average disk seek time on a hard disk is 8-10 ms, which is also
  about the time to read a IO_SIZE (8192) block.

  A medium ssd is about 400MB/second, which gives us the time for
  reading an IO_SIZE block to IO_SIZE/400000000 = 0.0000204 sec= 0.02 ms.

  For sequential hard disk seeks the cost formula is:
    DISK_SEEK_BASE_COST + DISK_SEEK_PROP_COST * #blocks_to_skip  
  
  The cost of average seek 
    DISK_SEEK_BASE_COST + DISK_SEEK_PROP_COST*BLOCKS_IN_AVG_SEEK = 10.
*/

#define DEFAULT_DISK_READ_COST ((double) IO_SIZE / 400000000.0 * 1000)
#define DISK_READ_COST          optimizer_disk_read_cost
#define DISK_READ_COST_THD(thd) (thd)->variables.DISK_READ_COST
#define BLOCKS_IN_AVG_SEEK  1
/* #define DISK_SEEK_PROP_COST ((double)1/BLOCKS_IN_AVG_SEEK) */

/**
  Number of rows in a reference table when refereed through a not unique key.
  This value is only used when we don't know anything about the key
  distribution.
*/
#define MATCHING_ROWS_IN_OTHER_TABLE 10

/*
  Subquery materialization-related constants
*/
/* This should match ha_heap::read_time() */
#define HEAP_TEMPTABLE_LOOKUP_COST 1.91e-4   /* see ha_heap.h */
#define HEAP_TEMPTABLE_CREATE_COST 1.0
#define DISK_TEMPTABLE_LOOKUP_COST(thd) DISK_READ_COST_THD(thd)
#define DISK_TEMPTABLE_CREATE_COST TMPFILE_CREATE_COST*2 /* 2 tmp tables */
#define DISK_TEMPTABLE_BLOCK_SIZE  IO_SIZE

#define SORT_INDEX_CMP_COST 0.02

#define COST_MAX (DBL_MAX * (1.0 - DBL_EPSILON))

static inline double COST_ADD(double c, double d)
{
  DBUG_ASSERT(c >= 0);
  DBUG_ASSERT(d >= 0);
  return (COST_MAX - (d) > (c) ? (c) + (d) : COST_MAX);
}

static inline double COST_MULT(double c, double f)
{
  DBUG_ASSERT(c >= 0);
  DBUG_ASSERT(f >= 0);
  return (COST_MAX / (f) > (c) ? (c) * (f) : COST_MAX);
}

#define MY_CHARSET_BIN_MB_MAXLEN 1

/** Don't pack string keys shorter than this (if PACK_KEYS=1 isn't used). */
#define KEY_DEFAULT_PACK_LENGTH 8

/** Characters shown for the command in 'show processlist'. */
#define PROCESS_LIST_WIDTH 100
/* Characters shown for the command in 'information_schema.processlist' */
#define PROCESS_LIST_INFO_WIDTH 65535

#define PRECISION_FOR_DOUBLE 53
#define PRECISION_FOR_FLOAT  24

/* -[digits].E+## */
#define MAX_FLOAT_STR_LENGTH (FLT_DIG + 6)
/* -[digits].E+### */
#define MAX_DOUBLE_STR_LENGTH (DBL_DIG + 7)

/*
  Default time to wait before aborting a new client connection
  that does not respond to "initial server greeting" timely
*/
#define CONNECT_TIMEOUT		10
 /* Wait 5 minutes before removing thread from thread cache */
#define THREAD_CACHE_TIMEOUT	5*60

/* The following can also be changed from the command line */
#define DEFAULT_CONCURRENCY	10
#define DELAYED_LIMIT		100		/**< pause after xxx inserts */
#define DELAYED_QUEUE_SIZE	1000
#define DELAYED_WAIT_TIMEOUT	(5*60)		/**< Wait for delayed insert */
#define MAX_CONNECT_ERRORS	100		///< errors before disabling host

#define LONG_TIMEOUT ((ulong) 3600L*24L*365L)

/**
  Maximum length of time zone name that we support (Time zone name is
  char(64) in db). mysqlbinlog needs it.
*/
#define MAX_TIME_ZONE_NAME_LENGTH       (NAME_LEN + 1)

#define SP_PSI_STATEMENT_INFO_COUNT 19

#endif /* SQL_CONST_INCLUDED */
