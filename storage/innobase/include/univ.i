/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.
Copyright (c) 2008, Google Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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

/***********************************************************************//**
@file include/univ.i
Version control for database, common definitions, and include files

Created 1/20/1994 Heikki Tuuri
****************************************************************************/

#pragma once

/* aux macros to convert M into "123" (string) if M is defined like
#define M 123 */
#define _IB_TO_STR(s)	#s
#define IB_TO_STR(s)	_IB_TO_STR(s)

/* The following is the InnoDB version as shown in
SELECT plugin_version FROM information_schema.plugins;
calculated in make_version_string() in sql/sql_show.cc like this:
"version >> 8" . "version & 0xff"
because the version is shown with only one dot, we skip the last
component, i.e. we show M.N.P as M.N */
#define INNODB_VERSION_SHORT	\
	(MYSQL_VERSION_MAJOR << 8 | MYSQL_VERSION_MINOR)

#define INNODB_VERSION_STR			\
	IB_TO_STR(MYSQL_VERSION_MAJOR) "."	\
	IB_TO_STR(MYSQL_VERSION_MINOR) "."	\
	IB_TO_STR(MYSQL_VERSION_PATCH)

/** How far ahead should we tell the service manager the timeout
(time in seconds) */
#define INNODB_EXTEND_TIMEOUT_INTERVAL 30

#if defined(_WIN32)
# include <windows.h>
#endif /* _WIN32 */

/* Include a minimum number of SQL header files so that few changes
made in SQL code cause a complete InnoDB rebuild.  These headers are
used throughout InnoDB but do not include too much themselves.  They
support cross-platform development and expose comonly used SQL names. */

#include <my_global.h>
#include "my_counter.h"
#include <m_string.h>
#include <mysqld_error.h>

/* Include <sys/stat.h> to get S_I... macros defined for os0file.cc */
#include <sys/stat.h>

#ifndef _WIN32
# include <sched.h>
# include "my_config.h"
#endif

#include <stdint.h>
#include <inttypes.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "my_pthread.h"

/* Following defines are to enable performance schema
instrumentation in each of five InnoDB modules if
HAVE_PSI_INTERFACE is defined. */
#ifdef HAVE_PSI_INTERFACE
# define UNIV_PFS_MUTEX
# define UNIV_PFS_RWLOCK
# define UNIV_PFS_IO
# define UNIV_PFS_THREAD

# include "mysql/psi/psi.h" /* HAVE_PSI_MEMORY_INTERFACE */
# ifdef HAVE_PSI_MEMORY_INTERFACE
#  define UNIV_PFS_MEMORY
# endif /* HAVE_PSI_MEMORY_INTERFACE */

/* There are mutexes/rwlocks that we want to exclude from
instrumentation even if their corresponding performance schema
define is set. And this PFS_NOT_INSTRUMENTED is used
as the key value to identify those objects that would
be excluded from instrumentation. */
# define PFS_NOT_INSTRUMENTED		ULINT32_UNDEFINED

# define PFS_IS_INSTRUMENTED(key)	((key) != PFS_NOT_INSTRUMENTED)

#ifdef HAVE_PFS_THREAD_PROVIDER_H
/* For PSI_MUTEX_CALL() and similar. */
#include "pfs_thread_provider.h"
#endif

#include "mysql/psi/mysql_thread.h"
/* For PSI_FILE_CALL(). */
#ifdef HAVE_PFS_FILE_PROVIDER_H
#include "pfs_file_provider.h"
#endif

#include "mysql/psi/mysql_file.h"

#endif /* HAVE_PSI_INTERFACE */

#ifdef _WIN32
# define YY_NO_UNISTD_H 1
/* VC++ tries to optimise for size by default, from V8+. The size of
the pointer to member depends on whether the type is defined before the
compiler sees the type in the translation unit. This default behaviour
can cause the pointer to be a different size in different translation
units, depending on the above rule. We force optimise for size behaviour
for all cases. This is used by ut0lst.h related code. */
# pragma pointers_to_members(full_generality, multiple_inheritance)
#endif /* _WIN32 */

/*			DEBUG VERSION CONTROL
			===================== */

/* When this macro is defined then additional test functions will be
compiled. These functions live at the end of each relevant source file
and have "test_" prefix. These functions can be called from the end of
innodb_init() or they can be called from gdb after srv_start() has executed
using the call command. */
/*
#define UNIV_COMPILE_TEST_FUNCS
#define UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR
#define UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
#define UNIV_ENABLE_UNIT_TEST_DICT_STATS
#define UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT
*/

#ifdef DBUG_OFF
# undef UNIV_DEBUG
#elif !defined UNIV_DEBUG
# define UNIV_DEBUG
#endif

#if 0
#define UNIV_DEBUG_PRINT			/* Enable the compilation of
						some debug print functions */
#define UNIV_AHI_DEBUG				/* Enable adaptive hash index
						debugging without UNIV_DEBUG */
#define UNIV_BLOB_LIGHT_DEBUG			/* Enable off-page column
						debugging without UNIV_DEBUG */
#define UNIV_DEBUG_LOCK_VALIDATE		/* Enable
						ut_ad(lock_rec_validate_page())
						assertions. */
#define UNIV_LRU_DEBUG				/* debug the buffer pool LRU */
#define UNIV_HASH_DEBUG				/* debug HASH_ macros */
#define UNIV_IBUF_DEBUG				/* debug the insert buffer */
#define UNIV_PERF_DEBUG                         /* debug flag that enables
                                                light weight performance
                                                related stuff. */
#define UNIV_SEARCH_PERF_STAT			/* statistics for the
						adaptive hash index */
#define UNIV_BTR_PRINT				/* enable functions for
						printing B-trees */
#define UNIV_ZIP_DEBUG				/* extensive consistency checks
						for compressed pages */
#define UNIV_ZIP_COPY				/* call page_zip_copy_recs()
						more often */
#define UNIV_AIO_DEBUG				/* prints info about
						submitted and reaped AIO
						requests to the log. */
#define UNIV_STATS_DEBUG			/* prints various stats
						related debug info from
						dict0stats.c */
#define FTS_INTERNAL_DIAG_PRINT                 /* FTS internal debugging
                                                info output */
#endif

#define UNIV_BTR_DEBUG				/* check B-tree links */
#define UNIV_LIGHT_MEM_DEBUG			/* light memory debugging */

// #define UNIV_SQL_DEBUG

#ifndef MY_ATTRIBUTE
#if defined(__GNUC__)
#  define MY_ATTRIBUTE(A) __attribute__(A)
#else
#  define MY_ATTRIBUTE(A)
#endif
#endif

#define UNIV_INLINE static inline

#define UNIV_WORD_SIZE		SIZEOF_SIZE_T

/** The following alignment is used in memory allocations in memory heap
management to ensure correct alignment for doubles etc. */
#define UNIV_MEM_ALIGNMENT	8U

/*
			DATABASE VERSION CONTROL
			========================
*/

#if defined (HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE) || defined(_WIN32)
#define IF_PUNCH_HOLE(A,B) A
#else
#define IF_PUNCH_HOLE(A,B) B
#endif

/** log2 of smallest compressed page size (1<<10 == 1024 bytes)
Note: This must never change! */
#define UNIV_ZIP_SIZE_SHIFT_MIN		10U

/** log2 of largest compressed page size (1<<14 == 16384 bytes).
A compressed page directory entry reserves 14 bits for the start offset
and 2 bits for flags. This limits the uncompressed page size to 16k.
*/
#define UNIV_ZIP_SIZE_SHIFT_MAX		14U

/* Define the Min, Max, Default page sizes. */
/** Minimum Page Size Shift (power of 2) */
#define UNIV_PAGE_SIZE_SHIFT_MIN	12U
/** log2 of largest page size (1<<16 == 64436 bytes). */
/** Maximum Page Size Shift (power of 2) */
#define UNIV_PAGE_SIZE_SHIFT_MAX	16U
/** log2 of default page size (1<<14 == 16384 bytes). */
/** Default Page Size Shift (power of 2) */
#define UNIV_PAGE_SIZE_SHIFT_DEF	14U
/** Original 16k InnoDB Page Size Shift, in case the default changes */
#define UNIV_PAGE_SIZE_SHIFT_ORIG	14U
/** Original 16k InnoDB Page Size as an ssize (log2 - 9) */
#define UNIV_PAGE_SSIZE_ORIG		(UNIV_PAGE_SIZE_SHIFT_ORIG - 9U)

/** Minimum page size InnoDB currently supports. */
#define UNIV_PAGE_SIZE_MIN	(1U << UNIV_PAGE_SIZE_SHIFT_MIN)
/** Maximum page size InnoDB currently supports. */
#define UNIV_PAGE_SIZE_MAX	(1U << UNIV_PAGE_SIZE_SHIFT_MAX)
/** Default page size for InnoDB tablespaces. */
#define UNIV_PAGE_SIZE_DEF	(1U << UNIV_PAGE_SIZE_SHIFT_DEF)
/** Original 16k page size for InnoDB tablespaces. */
#define UNIV_PAGE_SIZE_ORIG	(1U << UNIV_PAGE_SIZE_SHIFT_ORIG)

/** Smallest compressed page size */
#define UNIV_ZIP_SIZE_MIN	(1U << UNIV_ZIP_SIZE_SHIFT_MIN)

/** Largest compressed page size */
#define UNIV_ZIP_SIZE_MAX	(1U << UNIV_ZIP_SIZE_SHIFT_MAX)

/** Largest possible ssize for an uncompressed page.
(The convention 'ssize' is used for 'log2 minus 9' or the number of
shifts starting with 512.)
This max number varies depending on srv_page_size. */
#define UNIV_PAGE_SSIZE_MAX	\
	ulint(srv_page_size_shift - UNIV_ZIP_SIZE_SHIFT_MIN + 1U)

/** Smallest possible ssize for an uncompressed page. */
#define UNIV_PAGE_SSIZE_MIN	\
	ulint(UNIV_PAGE_SIZE_SHIFT_MIN - UNIV_ZIP_SIZE_SHIFT_MIN + 1U)

/** Maximum number of parallel threads in a parallelized operation */
#define UNIV_MAX_PARALLELISM	32

/** This is the "mbmaxlen" for my_charset_filename (defined in
strings/ctype-utf8.c), which is used to encode File and Database names. */
#define FILENAME_CHARSET_MAXNAMLEN	5

/** The maximum length of an encode table name in bytes.  The max
table and database names are NAME_CHAR_LEN (64) characters. After the
encoding, the max length would be NAME_CHAR_LEN (64) *
FILENAME_CHARSET_MAXNAMLEN (5) = 320 bytes. The number does not include a
terminating '\0'. InnoDB can handle longer names internally */
#define MAX_TABLE_NAME_LEN	320

/** The maximum length of a database name. Like MAX_TABLE_NAME_LEN this is
the MySQL's NAME_LEN, see check_and_convert_db_name(). */
#define MAX_DATABASE_NAME_LEN	MAX_TABLE_NAME_LEN

/** MAX_FULL_NAME_LEN defines the full name path including the
database name and table name. In addition, 14 bytes is added for:
	2 for surrounding quotes around table name
	1 for the separating dot (.)
	9 for the #mysql50# prefix */
#define MAX_FULL_NAME_LEN				\
	(MAX_TABLE_NAME_LEN + MAX_DATABASE_NAME_LEN + 14)

/** Maximum length of the compression alogrithm string. Currently we support
only (NONE | ZLIB | LZ4). */
#define MAX_COMPRESSION_LEN     4

/** The maximum length in bytes that a database name can occupy when stored in
UTF8, including the terminating '\0', see dict_fs2utf8(). You must include
mysql_com.h if you are to use this macro. */
#define MAX_DB_UTF8_LEN		(NAME_LEN + 1)

/** The maximum length in bytes that a table name can occupy when stored in
UTF8, including the terminating '\0', see dict_fs2utf8(). You must include
mysql_com.h if you are to use this macro. */
#define MAX_TABLE_UTF8_LEN	(NAME_LEN + sizeof(srv_mysql50_table_name_prefix))

/*
			UNIVERSAL TYPE DEFINITIONS
			==========================
*/

/** Unsigned octet of bits */
typedef unsigned char byte;
/** Machine-word-width unsigned integer */
typedef size_t ulint;
/** Machine-word-width signed integer */
typedef ssize_t lint;

/** ulint format for the printf() family of functions */
#define ULINTPF "%zu"
/** ulint hexadecimal format for the printf() family of functions */
#define ULINTPFx "%zx"

#ifdef _WIN32
/* Use the integer types and formatting strings defined in Visual Studio. */
# define UINT32PF	"%u"
# define UINT64scan     "llu"
# define UINT64PFx	"%016llx"
#elif defined __APPLE__
/* Apple prefers to call the 64-bit types 'long long'
in both 32-bit and 64-bit environments. */
# define UINT32PF	"%" PRIu32
# define UINT64scan     "llu"
# define UINT64PFx	"%016llx"
#elif defined _AIX
/* Workaround for macros expension trouble */
# define UINT32PF      "%u"
# define UINT64scan    "lu"
# define UINT64PFx     "%016lx"
#else
/* Use the integer types and formatting strings defined in the C99 standard. */
# define UINT32PF	"%" PRIu32
# define INT64PF	"%" PRId64
# define UINT64scan	PRIu64
# define UINT64PFx	"%016" PRIx64
#endif

typedef int64_t ib_int64_t;
typedef uint64_t ib_uint64_t;
typedef uint32_t ib_uint32_t;

#define UINT64PF	"%" UINT64scan
#define IB_ID_FMT	UINT64PF

/** Log sequence number (also used for redo log byte arithmetics) */
typedef	ib_uint64_t		lsn_t;

/** The 'undefined' value for a ulint */
#define ULINT_UNDEFINED		((ulint)(-1))

/** The 'undefined' value for a ib_uint64_t */
#define UINT64_UNDEFINED	((ib_uint64_t)(-1))

/** The bitmask of 32-bit unsigned integer */
#define ULINT32_MASK		0xFFFFFFFFU
/** The undefined 32-bit unsigned integer */
#define	ULINT32_UNDEFINED	ULINT32_MASK

/** Maximum value for a ulint */
#define ULINT_MAX		((ulint)(-2))

/** Maximum value for ib_uint64_t */
#define IB_UINT64_MAX		((ib_uint64_t) (~0ULL))

/** The generic InnoDB system object identifier data type */
typedef ib_uint64_t	        ib_id_t;
#define IB_ID_MAX               (~(ib_id_t) 0)
#define IB_ID_FMT               UINT64PF

#ifndef UINTMAX_MAX
#define UINTMAX_MAX		IB_UINT64_MAX
#endif
/** This 'ibool' type is used within Innobase. Remember that different included
headers may define 'bool' differently. Do not assume that 'bool' is a ulint! */
#define ibool			ulint

#ifndef TRUE

#define TRUE    1
#define FALSE   0

#endif

#define UNIV_NOTHROW

/** The following number as the length of a logical field means that the field
has the SQL NULL as its value. NOTE that because we assume that the length
of a field is a 32-bit integer when we store it, for example, to an undo log
on disk, we must have also this number fit in 32 bits, also in 64-bit
computers! */

#define UNIV_SQL_NULL ULINT32_UNDEFINED

/** Lengths which are not UNIV_SQL_NULL, but bigger than the following
number indicate that a field contains a reference to an externally
stored part of the field in the tablespace. The length field then
contains the sum of the following flag and the locally stored len. */

#define UNIV_EXTERN_STORAGE_FIELD (UNIV_SQL_NULL - UNIV_PAGE_SIZE_DEF)

#if defined(__GNUC__)
/* Tell the compiler that variable/function is unused. */
# define UNIV_UNUSED    MY_ATTRIBUTE ((unused))
#else
# define UNIV_UNUSED
#endif /* CHECK FOR GCC VER_GT_2 */

/* Some macros to improve branch prediction and reduce cache misses */
#if defined(COMPILER_HINTS) && defined(__GNUC__)
/* Tell the compiler that 'expr' probably evaluates to 'constant'. */
# define UNIV_EXPECT(expr,constant) __builtin_expect(expr, constant)
/* Tell the compiler that a pointer is likely to be NULL */
# define UNIV_LIKELY_NULL(ptr) __builtin_expect((ptr) != 0, 0)
/* Minimize cache-miss latency by moving data at addr into a cache before
it is read. */
# define UNIV_PREFETCH_R(addr) __builtin_prefetch(addr, 0, 3)
/* Minimize cache-miss latency by moving data at addr into a cache before
it is read or written. */
# define UNIV_PREFETCH_RW(addr) __builtin_prefetch(addr, 1, 3)

/* Sun Studio includes sun_prefetch.h as of version 5.9 */
#elif (defined(__SUNPRO_C) || defined(__SUNPRO_CC))

# include <sun_prefetch.h>

# define UNIV_EXPECT(expr,value) (expr)
# define UNIV_LIKELY_NULL(expr) (expr)

# if defined(COMPILER_HINTS)
//# define UNIV_PREFETCH_R(addr) sun_prefetch_read_many((void*) addr)
#  define UNIV_PREFETCH_R(addr) ((void) 0)
#  define UNIV_PREFETCH_RW(addr) sun_prefetch_write_many(addr)
# else
#  define UNIV_PREFETCH_R(addr) ((void) 0)
#  define UNIV_PREFETCH_RW(addr) ((void) 0)
# endif /* COMPILER_HINTS */

# elif defined _MSC_VER && defined COMPILER_HINTS
# define UNIV_EXPECT(expr,value) (expr)
# define UNIV_LIKELY_NULL(expr) (expr)
# if defined _M_IX86 || defined _M_X64
   // __MM_HINT_T0 - (temporal data)
   // prefetch data into all levels of the cache hierarchy.
#  define UNIV_PREFETCH_R(addr) _mm_prefetch((char *) addr, _MM_HINT_T0)
#  define UNIV_PREFETCH_RW(addr) _mm_prefetch((char *) addr, _MM_HINT_T0)
# elif defined _M_ARM64
#  define UNIV_PREFETCH_R(addr) __prefetch(addr)
#  define UNIV_PREFETCH_RW(addr) __prefetch(addr)
# else
#  define UNIV_PREFETCH_R ((void) 0)
#  define  UNIV_PREFETCH_RW(addr) ((void) 0)
# endif
#else
/* Dummy versions of the macros */
# define UNIV_EXPECT(expr,value) (expr)
# define UNIV_LIKELY_NULL(expr) (expr)
# define UNIV_PREFETCH_R(addr) ((void) 0)
# define UNIV_PREFETCH_RW(addr) ((void) 0)
#endif

/* Tell the compiler that cond is likely to hold */
#define UNIV_LIKELY(cond) UNIV_EXPECT(cond, TRUE)
/* Tell the compiler that cond is unlikely to hold */
#define UNIV_UNLIKELY(cond) UNIV_EXPECT(cond, FALSE)

/* Compile-time constant of the given array's size. */
#define UT_ARR_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#include <stdio.h>
#include "db0err.h"
#include "ut0dbg.h"
#include "ut0lst.h"
#include "ut0ut.h"

extern uint32_t srv_page_size_shift;
extern ulong	srv_page_size;

/* Dimension of spatial object we support so far. It has its root in
myisam/sp_defs.h. We only support 2 dimension data */
#define SPDIMS          2

#ifdef HAVE_PSI_INTERFACE
typedef unsigned int mysql_pfs_key_t;

# ifdef UNIV_PFS_MUTEX
extern mysql_pfs_key_t buf_pool_mutex_key;
extern mysql_pfs_key_t dict_foreign_err_mutex_key;
extern mysql_pfs_key_t fil_system_mutex_key;
extern mysql_pfs_key_t flush_list_mutex_key;
extern mysql_pfs_key_t fts_cache_mutex_key;
extern mysql_pfs_key_t fts_cache_init_mutex_key;
extern mysql_pfs_key_t fts_delete_mutex_key;
extern mysql_pfs_key_t fts_doc_id_mutex_key;
extern mysql_pfs_key_t ibuf_bitmap_mutex_key;
extern mysql_pfs_key_t ibuf_mutex_key;
extern mysql_pfs_key_t ibuf_pessimistic_insert_mutex_key;
extern mysql_pfs_key_t recalc_pool_mutex_key;
extern mysql_pfs_key_t purge_sys_pq_mutex_key;
extern mysql_pfs_key_t recv_sys_mutex_key;
extern mysql_pfs_key_t rtr_active_mutex_key;
extern mysql_pfs_key_t rtr_match_mutex_key;
extern mysql_pfs_key_t rtr_path_mutex_key;
extern mysql_pfs_key_t page_zip_stat_per_index_mutex_key;
extern mysql_pfs_key_t srv_innodb_monitor_mutex_key;
extern mysql_pfs_key_t srv_misc_tmpfile_mutex_key;
extern mysql_pfs_key_t srv_monitor_file_mutex_key;
extern mysql_pfs_key_t buf_dblwr_mutex_key;
extern mysql_pfs_key_t trx_pool_mutex_key;
extern mysql_pfs_key_t trx_pool_manager_mutex_key;
extern mysql_pfs_key_t lock_wait_mutex_key;
extern mysql_pfs_key_t srv_threads_mutex_key;
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_RWLOCK
extern mysql_pfs_key_t dict_operation_lock_key;
extern mysql_pfs_key_t fil_space_latch_key;
extern mysql_pfs_key_t trx_i_s_cache_lock_key;
extern mysql_pfs_key_t trx_purge_latch_key;
extern mysql_pfs_key_t index_tree_rw_lock_key;
extern mysql_pfs_key_t index_online_log_key;
extern mysql_pfs_key_t trx_sys_rw_lock_key;
extern mysql_pfs_key_t lock_latch_key;
extern mysql_pfs_key_t log_latch_key;
extern mysql_pfs_key_t trx_rseg_latch_key;
# endif /* UNIV_PFS_RWLOCK */
#endif /* HAVE_PSI_INTERFACE */
