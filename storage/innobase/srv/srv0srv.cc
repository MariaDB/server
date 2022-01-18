/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, 2009 Google Inc.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2013, 2021, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

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
@file srv/srv0srv.cc
The database server main program

Created 10/8/1995 Heikki Tuuri
*******************************************************/

#include "my_global.h"
// JAN: TODO: MySQL 5.7 missing header
//#include "my_thread.h"
//
// #include "mysql/psi/mysql_stage.h"
// #include "mysql/psi/psi.h"

#include "btr0sea.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "ibuf0ibuf.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "mem0mem.h"
#include "os0proc.h"
#include "pars0pars.h"
#include "que0que.h"
#include "row0mysql.h"
#include "row0trunc.h"
#include "row0log.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "sync0sync.h"
#include "trx0i_s.h"
#include "trx0purge.h"
#include "ut0crc32.h"
#include "btr0defragment.h"
#include "ut0mem.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "fil0pagecompress.h"
#include "btr0scrub.h"

#include <my_service_manager.h>

#ifdef WITH_WSREP
extern int wsrep_debug;
extern int wsrep_trx_is_aborting(void *thd_ptr);
#endif
/* The following is the maximum allowed duration of a lock wait. */
UNIV_INTERN ulong	srv_fatal_semaphore_wait_threshold =  DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT;

/* How much data manipulation language (DML) statements need to be delayed,
in microseconds, in order to reduce the lagging of the purge thread. */
ulint	srv_dml_needed_delay;

bool	srv_monitor_active;
bool	srv_error_monitor_active;
bool	srv_buf_dump_thread_active;
bool	srv_dict_stats_thread_active;
bool	srv_buf_resize_thread_active;

my_bool	srv_scrub_log;

const char*	srv_main_thread_op_info = "";

/** Prefix used by MySQL to indicate pre-5.1 table name encoding */
const char		srv_mysql50_table_name_prefix[10] = "#mysql50#";

/* Server parameters which are read from the initfile */

/* The following three are dir paths which are catenated before file
names, where the file name itself may also contain a path */

char*	srv_data_home;

/** Rollback files directory, can be absolute. */
char*	srv_undo_dir;

/** The number of tablespaces to use for rollback segments. */
ulong	srv_undo_tablespaces;

/** The number of UNDO tablespaces that are open and ready to use. */
ulint	srv_undo_tablespaces_open;

/** The number of UNDO tablespaces that are active (hosting some rollback
segment). It is quite possible that some of the tablespaces doesn't host
any of the rollback-segment based on configuration used. */
ulint	srv_undo_tablespaces_active;

/* The number of rollback segments to use */
ulong	srv_undo_logs;

/** Rate at which UNDO records should be purged. */
ulong	srv_purge_rseg_truncate_frequency;

/** Enable or Disable Truncate of UNDO tablespace.
Note: If enabled then UNDO tablespace will be selected for truncate.
While Server waits for undo-tablespace to truncate if user disables
it, truncate action is completed but no new tablespace is marked
for truncate (action is never aborted). */
my_bool	srv_undo_log_truncate;

/** Maximum size of undo tablespace. */
unsigned long long	srv_max_undo_log_size;

/** Default undo tablespace size in UNIV_PAGEs count (10MB). */
const ulint SRV_UNDO_TABLESPACE_SIZE_IN_PAGES =
	((1024 * 1024) * 10) / UNIV_PAGE_SIZE_DEF;

/** Set if InnoDB must operate in read-only mode. We don't do any
recovery and open all tables in RO mode instead of RW mode. We don't
sync the max trx id to disk either. */
my_bool	srv_read_only_mode;
/** store to its own file each table created by an user; data
dictionary tables are in the system tablespace 0 */
my_bool	srv_file_per_table;
/** whether to use backup-safe TRUNCATE and crash-safe RENAME
instead of the MySQL 5.7 WL#6501 TRUNCATE TABLE implementation */
my_bool	srv_safe_truncate;
/** The file format to use on new *.ibd files. */
ulint	srv_file_format;
/** Whether to check file format during startup.  A value of
UNIV_FORMAT_MAX + 1 means no checking ie. FALSE.  The default is to
set it to the highest format we support. */
ulint	srv_max_file_format_at_startup = UNIV_FORMAT_MAX;
/** Set if InnoDB operates in read-only mode or innodb-force-recovery
is greater than SRV_FORCE_NO_TRX_UNDO. */
my_bool	high_level_read_only;

#if UNIV_FORMAT_A
# error "UNIV_FORMAT_A must be 0!"
#endif

/** Place locks to records only i.e. do not use next-key locking except
on duplicate key checking and foreign key checking */
ibool	srv_locks_unsafe_for_binlog;
/** Sort buffer size in index creation */
ulong	srv_sort_buf_size;
/** Maximum modification log file size for online index creation */
unsigned long long	srv_online_max_size;

/* If this flag is TRUE, then we will use the native aio of the
OS (provided we compiled Innobase with it in), otherwise we will
use simulated aio we build below with threads.
Currently we support native aio on windows and linux */
my_bool	srv_use_native_aio;
my_bool	srv_numa_interleave;
/** innodb_use_trim; whether to use fallocate(PUNCH_HOLE) with
page_compression */
my_bool	srv_use_trim;
/** copy of innodb_use_atomic_writes; @see innobase_init() */
my_bool	srv_use_atomic_writes;
/** innodb_compression_algorithm; used with page compression */
ulong	innodb_compression_algorithm;
/** innodb_mtflush_threads; number of threads used for multi-threaded flush */
long srv_mtflush_threads;
/** innodb_use_mtflush; whether to use multi threaded flush. */
my_bool	srv_use_mtflush;

#ifdef UNIV_DEBUG
/** Used by SET GLOBAL innodb_master_thread_disabled_debug = X. */
my_bool	srv_master_thread_disabled_debug;
/** Event used to inform that master thread is disabled. */
static os_event_t	srv_master_thread_disabled_event;
#endif /* UNIV_DEBUG */

/*------------------------- LOG FILES ------------------------ */
char*	srv_log_group_home_dir;

ulong	srv_n_log_files;
/** The InnoDB redo log file size, or 0 when changing the redo log format
at startup (while disallowing writes to the redo log). */
ulonglong	srv_log_file_size;
/** copy of innodb_log_buffer_size, but in database pages */
ulint		srv_log_buffer_size;
/** innodb_flush_log_at_trx_commit */
ulong		srv_flush_log_at_trx_commit;
/** innodb_flush_log_at_timeout */
uint		srv_flush_log_at_timeout;
/** innodb_page_size */
ulong		srv_page_size;
/** log2 of innodb_page_size; @see innobase_init() */
ulong		srv_page_size_shift;
/** innodb_log_write_ahead_size */
ulong		srv_log_write_ahead_size;

page_size_t	univ_page_size(0, 0, false);

/** innodb_adaptive_flushing; try to flush dirty pages so as to avoid
IO bursts at the checkpoints. */
my_bool	srv_adaptive_flushing;

/** innodb_flush_sync; whether to ignore io_capacity at log checkpoints */
my_bool	srv_flush_sync;

/** Maximum number of times allowed to conditionally acquire
mutex before switching to blocking wait on the mutex */
#define MAX_MUTEX_NOWAIT	20

/** Check whether the number of failed nonblocking mutex
acquisition attempts exceeds maximum allowed value. If so,
srv_printf_innodb_monitor() will request mutex acquisition
with mutex_enter(), which will wait until it gets the mutex. */
#define MUTEX_NOWAIT(mutex_skipped)	((mutex_skipped) < MAX_MUTEX_NOWAIT)

#ifdef WITH_INNODB_DISALLOW_WRITES
UNIV_INTERN os_event_t	srv_allow_writes_event;
#endif /* WITH_INNODB_DISALLOW_WRITES */

/** copy of innodb_buffer_pool_size */
ulint	srv_buf_pool_size;
/** Requested buffer pool chunk size. Each buffer pool instance consists
of one or more chunks. */
ulong	srv_buf_pool_chunk_unit;
/** innodb_buffer_pool_instances (0 is interpreted as 1) */
ulong	srv_buf_pool_instances;
/** Default value of innodb_buffer_pool_instances */
const ulong	srv_buf_pool_instances_default = 0;
/** innodb_page_hash_locks (a debug-only parameter);
number of locks to protect buf_pool->page_hash */
ulong	srv_n_page_hash_locks = 16;
/** innodb_lru_scan_depth; number of blocks scanned in LRU flush batch */
ulong	srv_LRU_scan_depth;
/** innodb_flush_neighbors; whether or not to flush neighbors of a block */
ulong	srv_flush_neighbors;
/** Previously requested size */
ulint	srv_buf_pool_old_size;
/** Current size as scaling factor for the other components */
ulint	srv_buf_pool_base_size;
/** Current size in bytes */
ulint	srv_buf_pool_curr_size;
/** Dump this % of each buffer pool during BP dump */
ulong	srv_buf_pool_dump_pct;
/** Lock table size in bytes */
ulint	srv_lock_table_size	= ULINT_MAX;

/** copy of innodb_read_io_threads */
ulint	srv_n_read_io_threads;
/** copy of innodb_write_io_threads */
ulint	srv_n_write_io_threads;

/** innodb_random_read_ahead */
my_bool	srv_random_read_ahead;
/** innodb_read_ahead_threshold; the number of pages that must be present
in the buffer cache and accessed sequentially for InnoDB to trigger a
readahead request. */
ulong	srv_read_ahead_threshold;

/** innodb_change_buffer_max_size; maximum on-disk size of change
buffer in terms of percentage of the buffer pool. */
uint	srv_change_buffer_max_size;

char*	srv_file_flush_method_str;


enum srv_flush_t	srv_file_flush_method = IF_WIN(SRV_ALL_O_DIRECT_FSYNC,SRV_FSYNC);


/** copy of innodb_open_files, initialized by innobase_init() */
ulint	srv_max_n_open_files;

/** innodb_io_capacity */
ulong	srv_io_capacity;
/** innodb_io_capacity_max */
ulong	srv_max_io_capacity;

/** innodb_page_cleaners; the number of page cleaner threads */
ulong	srv_n_page_cleaners;

/* The InnoDB main thread tries to keep the ratio of modified pages
in the buffer pool to all database pages in the buffer pool smaller than
the following number. But it is not guaranteed that the value stays below
that during a time of heavy update/insert activity. */

/** innodb_max_dirty_pages_pct */
double	srv_max_buf_pool_modified_pct;
/** innodb_max_dirty_pages_pct_lwm */
double	srv_max_dirty_pages_pct_lwm;

/** innodb_adaptive_flushing_lwm; the percentage of log capacity at
which adaptive flushing, if enabled, will kick in. */
double	srv_adaptive_flushing_lwm;

/** innodb_flushing_avg_loops; number of iterations over which
adaptive flushing is averaged */
ulong	srv_flushing_avg_loops;

/** innodb_purge_threads; the number of purge threads to use */
ulong	srv_n_purge_threads;

/** innodb_purge_batch_size, in pages */
ulong	srv_purge_batch_size;

/** innodb_stats_method decides how InnoDB treats
NULL value when collecting statistics. By default, it is set to
SRV_STATS_NULLS_EQUAL(0), ie. all NULL value are treated equal */
ulong srv_innodb_stats_method;

srv_stats_t	srv_stats;

/* structure to pass status variables to MySQL */
export_var_t export_vars;

/** Normally 0. When nonzero, skip some phases of crash recovery,
starting from SRV_FORCE_IGNORE_CORRUPT, so that data can be recovered
by SELECT or mysqldump. When this is nonzero, we do not allow any user
modifications to the data. */
ulong	srv_force_recovery;

/** innodb_print_all_deadlocks; whether to print all user-level
transactions deadlocks to the error log */
my_bool	srv_print_all_deadlocks;

/** innodb_cmp_per_index_enabled; enable
INFORMATION_SCHEMA.innodb_cmp_per_index */
my_bool	srv_cmp_per_index_enabled;

/** innodb_fast_shutdown; if 1 then we do not run purge and insert buffer
merge to completion before shutdown. If it is set to 2, do not even flush the
buffer pool to data files at the shutdown: we effectively 'crash'
InnoDB (but lose no committed transactions). */
uint	srv_fast_shutdown;

/** copy of innodb_status_file; generate a innodb_status.<pid> file */
ibool	srv_innodb_status;

/** innodb_prefix_index_cluster_optimization; whether to optimize
prefix index queries to skip cluster index lookup when possible */
my_bool	srv_prefix_index_cluster_optimization;

/** innodb_stats_transient_sample_pages;
When estimating number of different key values in an index, sample
this many index pages, there are 2 ways to calculate statistics:
* persistent stats that are calculated by ANALYZE TABLE and saved
  in the innodb database.
* quick transient stats, that are used if persistent stats for the given
  table/index are not found in the innodb database */
unsigned long long	srv_stats_transient_sample_pages;
/** innodb_stats_persistent */
my_bool		srv_stats_persistent;
/** innodb_stats_include_delete_marked */
my_bool		srv_stats_include_delete_marked;
/** innodb_stats_persistent_sample_pages */
unsigned long long	srv_stats_persistent_sample_pages;
/** innodb_stats_auto_recalc */
my_bool		srv_stats_auto_recalc;

/** innodb_stats_modified_counter; The number of rows modified before
we calculate new statistics (default 0 = current limits) */
unsigned long long srv_stats_modified_counter;

/** innodb_stats_traditional; enable traditional statistic calculation
based on number of configured pages */
my_bool	srv_stats_sample_traditional;

/** copy of innodb_doublewrite */
ibool	srv_use_doublewrite_buf;

/** innodb_doublewrite_batch_size (a debug parameter) specifies the
number of pages to use in LRU and flush_list batch flushing.
The rest of the doublewrite buffer is used for single-page flushing. */
ulong	srv_doublewrite_batch_size = 120;

/** innodb_replication_delay */
ulong	srv_replication_delay;

/** innodb_sync_spin_loops */
ulong	srv_n_spin_wait_rounds;
/** innodb_spin_wait_delay */
uint	srv_spin_wait_delay;

static ulint		srv_n_rows_inserted_old;
static ulint		srv_n_rows_updated_old;
static ulint		srv_n_rows_deleted_old;
static ulint		srv_n_rows_read_old;
static ulint		srv_n_system_rows_inserted_old;
static ulint		srv_n_system_rows_updated_old;
static ulint		srv_n_system_rows_deleted_old;
static ulint		srv_n_system_rows_read_old;

ulint	srv_truncated_status_writes;
/** Number of initialized rollback segments for persistent undo log */
ulong	srv_available_undo_logs;

/* Defragmentation */
UNIV_INTERN my_bool	srv_defragment;
/** innodb_defragment_n_pages */
UNIV_INTERN uint	srv_defragment_n_pages;
UNIV_INTERN uint	srv_defragment_stats_accuracy;
/** innodb_defragment_fill_factor_n_recs */
UNIV_INTERN uint	srv_defragment_fill_factor_n_recs;
/** innodb_defragment_fill_factor */
UNIV_INTERN double	srv_defragment_fill_factor;
/** innodb_defragment_frequency */
UNIV_INTERN uint	srv_defragment_frequency;
/** derived from innodb_defragment_frequency;
@see innodb_defragment_frequency_update() */
UNIV_INTERN ulonglong	srv_defragment_interval;

/** Current mode of operation */
UNIV_INTERN enum srv_operation_mode srv_operation;

/* Set the following to 0 if you want InnoDB to write messages on
stderr on startup/shutdown. Not enabled on the embedded server. */
ibool	srv_print_verbose_log;
my_bool	srv_print_innodb_monitor;
my_bool	srv_print_innodb_lock_monitor;
/** innodb_force_primary_key; whether to disallow CREATE TABLE without
PRIMARY KEY */
my_bool	srv_force_primary_key;

/** Key version to encrypt the temporary tablespace */
my_bool innodb_encrypt_temporary_tables;

/* Array of English strings describing the current state of an
i/o handler thread */

const char* srv_io_thread_op_info[SRV_MAX_N_IO_THREADS];
const char* srv_io_thread_function[SRV_MAX_N_IO_THREADS];

static time_t	srv_last_monitor_time;

static ib_mutex_t	srv_innodb_monitor_mutex;

/** Mutex protecting page_zip_stat_per_index */
ib_mutex_t	page_zip_stat_per_index_mutex;

/* Mutex for locking srv_monitor_file. Not created if srv_read_only_mode */
ib_mutex_t	srv_monitor_file_mutex;

/** Temporary file for innodb monitor output */
FILE*	srv_monitor_file;
/** Mutex for locking srv_misc_tmpfile. Not created if srv_read_only_mode.
This mutex has a very low rank; threads reserving it should not
acquire any further latches or sleep before releasing this one. */
ib_mutex_t	srv_misc_tmpfile_mutex;
/** Temporary file for miscellanous diagnostic output */
FILE*	srv_misc_tmpfile;

static ulint	srv_main_thread_process_no;
static ulint	srv_main_thread_id;

/* The following counts are used by the srv_master_thread. */

/** Iterations of the loop bounded by 'srv_active' label. */
static ulint		srv_main_active_loops;
/** Iterations of the loop bounded by the 'srv_idle' label. */
static ulint		srv_main_idle_loops;
/** Iterations of the loop bounded by the 'srv_shutdown' label. */
static ulint		srv_main_shutdown_loops;
/** Log writes involving flush. */
static ulint		srv_log_writes_and_flush;

/* This is only ever touched by the master thread. It records the
time when the last flush of log file has happened. The master
thread ensures that we flush the log files at least once per
second. */
static time_t	srv_last_log_flush_time;

/* Interval in seconds at which various tasks are performed by the
master thread when server is active. In order to balance the workload,
we should try to keep intervals such that they are not multiple of
each other. For example, if we have intervals for various tasks
defined as 5, 10, 15, 60 then all tasks will be performed when
current_time % 60 == 0 and no tasks will be performed when
current_time % 5 != 0. */

# define	SRV_MASTER_CHECKPOINT_INTERVAL		(7)
#ifdef MEM_PERIODIC_CHECK
# define	SRV_MASTER_MEM_VALIDATE_INTERVAL	(13)
#endif /* MEM_PERIODIC_CHECK */
# define	SRV_MASTER_DICT_LRU_INTERVAL		(47)

/** Buffer pool dump status frequence in percentages */
UNIV_INTERN ulong srv_buf_dump_status_frequency;

/** Acquire the system_mutex. */
#define srv_sys_mutex_enter() do {			\
	mutex_enter(&srv_sys.mutex);			\
} while (0)

/** Test if the system mutex is owned. */
#define srv_sys_mutex_own() (mutex_own(&srv_sys.mutex)	\
			     && !srv_read_only_mode)

/** Release the system mutex. */
#define srv_sys_mutex_exit() do {			\
	mutex_exit(&srv_sys.mutex);			\
} while (0)

#define fetch_lock_wait_timeout(trx)			\
	((trx)->lock.allowed_to_wait			\
	 ? thd_lock_wait_timeout((trx)->mysql_thd)	\
	 : 0)

/*
	IMPLEMENTATION OF THE SERVER MAIN PROGRAM
	=========================================

There is the following analogue between this database
server and an operating system kernel:

DB concept			equivalent OS concept
----------			---------------------
transaction		--	process;

query thread		--	thread;

lock			--	semaphore;

kernel			--	kernel;

query thread execution:
(a) without lock mutex
reserved		--	process executing in user mode;
(b) with lock mutex reserved
			--	process executing in kernel mode;

The server has several backgroind threads all running at the same
priority as user threads. It periodically checks if here is anything
happening in the server which requires intervention of the master
thread. Such situations may be, for example, when flushing of dirty
blocks is needed in the buffer pool or old version of database rows
have to be cleaned away (purged). The user can configure a separate
dedicated purge thread(s) too, in which case the master thread does not
do any purging.

The threads which we call user threads serve the queries of the MySQL
server. They run at normal priority.

When there is no activity in the system, also the master thread
suspends itself to wait for an event making the server totally silent.

There is still one complication in our server design. If a
background utility thread obtains a resource (e.g., mutex) needed by a user
thread, and there is also some other user activity in the system,
the user thread may have to wait indefinitely long for the
resource, as the OS does not schedule a background thread if
there is some other runnable user thread. This problem is called
priority inversion in real-time programming.

One solution to the priority inversion problem would be to keep record
of which thread owns which resource and in the above case boost the
priority of the background thread so that it will be scheduled and it
can release the resource.  This solution is called priority inheritance
in real-time programming.  A drawback of this solution is that the overhead
of acquiring a mutex increases slightly, maybe 0.2 microseconds on a 100
MHz Pentium, because the thread has to call os_thread_get_curr_id.  This may
be compared to 0.5 microsecond overhead for a mutex lock-unlock pair. Note
that the thread cannot store the information in the resource , say mutex,
itself, because competing threads could wipe out the information if it is
stored before acquiring the mutex, and if it stored afterwards, the
information is outdated for the time of one machine instruction, at least.
(To be precise, the information could be stored to lock_word in mutex if
the machine supports atomic swap.)

The above solution with priority inheritance may become actual in the
future, currently we do not implement any priority twiddling solution.
Our general aim is to reduce the contention of all mutexes by making
them more fine grained.

The thread table contains information of the current status of each
thread existing in the system, and also the event semaphores used in
suspending the master thread and utility threads when they have nothing
to do.  The thread table can be seen as an analogue to the process table
in a traditional Unix implementation. */

/** The server system struct */
struct srv_sys_t{
	ib_mutex_t	tasks_mutex;		/*!< variable protecting the
						tasks queue */
	UT_LIST_BASE_NODE_T(que_thr_t)
			tasks;			/*!< task queue */

	ib_mutex_t	mutex;			/*!< variable protecting the
						fields below. */
	ulint		n_sys_threads;		/*!< size of the sys_threads
						array */

	srv_slot_t
	sys_threads[srv_max_purge_threads + 1];	/*!< server thread table;
						os_event_set() and
						os_event_reset() on
						sys_threads[]->event are
						covered by srv_sys_t::mutex */

	ulint		n_threads_active[SRV_MASTER + 1];
						/*!< number of threads active
						in a thread class; protected
						by both my_atomic_addlint()
						and mutex */

	srv_stats_t::ulint_ctr_1_t
			activity_count;		/*!< For tracking server
						activity */
};

static srv_sys_t	srv_sys;

/** Event to signal srv_monitor_thread. Not protected by a mutex.
Set after setting srv_print_innodb_monitor. */
os_event_t	srv_monitor_event;

/** Event to signal the shutdown of srv_error_monitor_thread.
Not protected by a mutex. */
os_event_t	srv_error_event;

/** Event for waking up buf_dump_thread. Not protected by a mutex.
Set on shutdown or by buf_dump_start() or buf_load_start(). */
os_event_t	srv_buf_dump_event;

/** Event to signal the buffer pool resize thread */
os_event_t	srv_buf_resize_event;

/** The buffer pool dump/load file name */
char*	srv_buf_dump_filename;

/** Boolean config knobs that tell InnoDB to dump the buffer pool at shutdown
and/or load it during startup. */
char	srv_buffer_pool_dump_at_shutdown = TRUE;
char	srv_buffer_pool_load_at_startup = TRUE;

/** Slot index in the srv_sys.sys_threads array for the master thread. */
#define SRV_MASTER_SLOT 0

/** Slot index in the srv_sys.sys_threads array for the purge thread. */
#define SRV_PURGE_SLOT 1

/** Slot index in the srv_sys.sys_threads array from which purge workers start.
  */
#define SRV_WORKER_SLOTS_START 2

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Performance schema stage event for monitoring ALTER TABLE progress
everything after flush log_make_checkpoint(). */
PSI_stage_info	srv_stage_alter_table_end
	= {0, "alter table (end)", PSI_FLAG_STAGE_PROGRESS};

/** Performance schema stage event for monitoring ALTER TABLE progress
log_make_checkpoint(). */
PSI_stage_info	srv_stage_alter_table_flush
	= {0, "alter table (flush)", PSI_FLAG_STAGE_PROGRESS};

/** Performance schema stage event for monitoring ALTER TABLE progress
row_merge_insert_index_tuples(). */
PSI_stage_info	srv_stage_alter_table_insert
	= {0, "alter table (insert)", PSI_FLAG_STAGE_PROGRESS};

/** Performance schema stage event for monitoring ALTER TABLE progress
row_log_apply(). */
PSI_stage_info	srv_stage_alter_table_log_index
	= {0, "alter table (log apply index)", PSI_FLAG_STAGE_PROGRESS};

/** Performance schema stage event for monitoring ALTER TABLE progress
row_log_table_apply(). */
PSI_stage_info	srv_stage_alter_table_log_table
	= {0, "alter table (log apply table)", PSI_FLAG_STAGE_PROGRESS};

/** Performance schema stage event for monitoring ALTER TABLE progress
row_merge_sort(). */
PSI_stage_info	srv_stage_alter_table_merge_sort
	= {0, "alter table (merge sort)", PSI_FLAG_STAGE_PROGRESS};

/** Performance schema stage event for monitoring ALTER TABLE progress
row_merge_read_clustered_index(). */
PSI_stage_info	srv_stage_alter_table_read_pk_internal_sort
	= {0, "alter table (read PK and internal sort)", PSI_FLAG_STAGE_PROGRESS};

/** Performance schema stage event for monitoring buffer pool load progress. */
PSI_stage_info	srv_stage_buffer_pool_load
	= {0, "buffer pool load", PSI_FLAG_STAGE_PROGRESS};
#endif /* HAVE_PSI_STAGE_INTERFACE */

/*********************************************************************//**
Prints counters for work done by srv_master_thread. */
static
void
srv_print_master_thread_info(
/*=========================*/
	FILE  *file)    /* in: output stream */
{
	fprintf(file, "srv_master_thread loops: " ULINTPF " srv_active, "
		ULINTPF " srv_shutdown, " ULINTPF " srv_idle\n"
		"srv_master_thread log flush and writes: " ULINTPF "\n",
		srv_main_active_loops,
		srv_main_shutdown_loops,
		srv_main_idle_loops,
		srv_log_writes_and_flush);
}

/*********************************************************************//**
Sets the info describing an i/o thread current state. */
void
srv_set_io_thread_op_info(
/*======================*/
	ulint		i,	/*!< in: the 'segment' of the i/o thread */
	const char*	str)	/*!< in: constant char string describing the
				state */
{
	ut_a(i < SRV_MAX_N_IO_THREADS);

	srv_io_thread_op_info[i] = str;
}

/*********************************************************************//**
Resets the info describing an i/o thread current state. */
void
srv_reset_io_thread_op_info()
/*=========================*/
{
	for (ulint i = 0; i < UT_ARR_SIZE(srv_io_thread_op_info); ++i) {
		srv_io_thread_op_info[i] = "not started yet";
	}
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Validates the type of a thread table slot.
@return TRUE if ok */
static
ibool
srv_thread_type_validate(
/*=====================*/
	srv_thread_type	type)	/*!< in: thread type */
{
	switch (type) {
	case SRV_NONE:
		break;
	case SRV_WORKER:
	case SRV_PURGE:
	case SRV_MASTER:
		return(TRUE);
	}
	ut_error;
	return(FALSE);
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Gets the type of a thread table slot.
@return thread type */
static
srv_thread_type
srv_slot_get_type(
/*==============*/
	const srv_slot_t*	slot)	/*!< in: thread slot */
{
	srv_thread_type	type = slot->type;
	ut_ad(srv_thread_type_validate(type));
	return(type);
}

/*********************************************************************//**
Reserves a slot in the thread table for the current thread.
@return reserved slot */
static
srv_slot_t*
srv_reserve_slot(
/*=============*/
	srv_thread_type	type)	/*!< in: type of the thread */
{
	srv_slot_t*	slot = 0;

	srv_sys_mutex_enter();

	ut_ad(srv_thread_type_validate(type));

	switch (type) {
	case SRV_MASTER:
		slot = &srv_sys.sys_threads[SRV_MASTER_SLOT];
		break;

	case SRV_PURGE:
		slot = &srv_sys.sys_threads[SRV_PURGE_SLOT];
		break;

	case SRV_WORKER:
		/* Find an empty slot, skip the master and purge slots. */
		for (slot = &srv_sys.sys_threads[SRV_WORKER_SLOTS_START];
		     slot->in_use;
		     ++slot) {

			ut_a(slot < &srv_sys.sys_threads[
				     srv_sys.n_sys_threads]);
		}
		break;

	case SRV_NONE:
		ut_error;
	}

	ut_a(!slot->in_use);

	slot->in_use = TRUE;
	slot->suspended = FALSE;
	slot->type = type;

	ut_ad(srv_slot_get_type(slot) == type);

	my_atomic_addlint(&srv_sys.n_threads_active[type], 1);

	srv_sys_mutex_exit();

	return(slot);
}

/*********************************************************************//**
Suspends the calling thread to wait for the event in its thread slot.
@return the current signal count of the event. */
static
int64_t
srv_suspend_thread_low(
/*===================*/
	srv_slot_t*	slot)	/*!< in/out: thread slot */
{
	ut_ad(!srv_read_only_mode);
	ut_ad(srv_sys_mutex_own());

	ut_ad(slot->in_use);

	srv_thread_type	type = srv_slot_get_type(slot);

	switch (type) {
	case SRV_NONE:
		ut_error;

	case SRV_MASTER:
		/* We have only one master thread and it
		should be the first entry always. */
		ut_a(srv_sys.n_threads_active[type] == 1);
		break;

	case SRV_PURGE:
		/* We have only one purge coordinator thread
		and it should be the second entry always. */
		ut_a(srv_sys.n_threads_active[type] == 1);
		break;

	case SRV_WORKER:
		ut_a(srv_n_purge_threads > 1);
		break;
	}

	ut_a(!slot->suspended);
	slot->suspended = TRUE;

	if (my_atomic_addlint(&srv_sys.n_threads_active[type], -1) < 0) {
		ut_error;
	}

	return(os_event_reset(slot->event));
}

/*********************************************************************//**
Suspends the calling thread to wait for the event in its thread slot.
@return the current signal count of the event. */
static
int64_t
srv_suspend_thread(
/*===============*/
	srv_slot_t*	slot)	/*!< in/out: thread slot */
{
	srv_sys_mutex_enter();

	int64_t		sig_count = srv_suspend_thread_low(slot);

	srv_sys_mutex_exit();

	return(sig_count);
}

/** Resume the calling thread.
@param[in,out]	slot		thread slot
@param[in]	sig_count	signal count (if wait)
@param[in]	wait		whether to wait for the event
@param[in]	timeout_usec	timeout in microseconds (0=infinite)
@return	whether the wait timed out */
static
bool
srv_resume_thread(srv_slot_t* slot, int64_t sig_count = 0, bool wait = true,
		  ulint timeout_usec = 0)
{
	bool	timeout;

	ut_ad(!srv_read_only_mode);
	ut_ad(slot->in_use);
	ut_ad(slot->suspended);

	if (!wait) {
		timeout = false;
	} else if (timeout_usec) {
		timeout = OS_SYNC_TIME_EXCEEDED == os_event_wait_time_low(
			slot->event, timeout_usec, sig_count);
	} else {
		timeout = false;
		os_event_wait_low(slot->event, sig_count);
	}

	srv_sys_mutex_enter();
	ut_ad(slot->in_use);
	ut_ad(slot->suspended);

	slot->suspended = FALSE;
	my_atomic_addlint(&srv_sys.n_threads_active[slot->type], 1);
	srv_sys_mutex_exit();
	return(timeout);
}

/** Ensure that a given number of threads of the type given are running
(or are already terminated).
@param[in]	type	thread type
@param[in]	n	number of threads that have to run */
void
srv_release_threads(enum srv_thread_type type, ulint n)
{
	ulint	running;

	ut_ad(srv_thread_type_validate(type));
	ut_ad(n > 0);

	do {
		running = 0;

		srv_sys_mutex_enter();

		for (ulint i = 0; i < srv_sys.n_sys_threads; i++) {
			srv_slot_t*	slot = &srv_sys.sys_threads[i];

			if (!slot->in_use || srv_slot_get_type(slot) != type) {
				continue;
			} else if (!slot->suspended) {
				if (++running >= n) {
					break;
				}
				continue;
			}

			switch (type) {
			case SRV_NONE:
				ut_error;

			case SRV_MASTER:
				/* We have only one master thread and it
				should be the first entry always. */
				ut_a(n == 1);
				ut_a(i == SRV_MASTER_SLOT);
				ut_a(srv_sys.n_threads_active[type] == 0);
				break;

			case SRV_PURGE:
				/* We have only one purge coordinator thread
				and it should be the second entry always. */
				ut_a(n == 1);
				ut_a(i == SRV_PURGE_SLOT);
				ut_a(srv_n_purge_threads > 0);
				ut_a(srv_sys.n_threads_active[type] == 0);
				break;

			case SRV_WORKER:
				ut_a(srv_n_purge_threads > 1);
				ut_a(srv_sys.n_threads_active[type]
				     < srv_n_purge_threads - 1);
				break;
			}

			os_event_set(slot->event);
		}

		srv_sys_mutex_exit();
	} while (running && running < n);
}

/*********************************************************************//**
Release a thread's slot. */
static
void
srv_free_slot(
/*==========*/
	srv_slot_t*	slot)	/*!< in/out: thread slot */
{
	srv_sys_mutex_enter();

	/* Mark the thread as inactive. */
	srv_suspend_thread_low(slot);
	/* Free the slot for reuse. */
	ut_ad(slot->in_use);
	slot->in_use = FALSE;

	srv_sys_mutex_exit();
}

/** Initialize the server. */
static
void
srv_init()
{
	mutex_create(LATCH_ID_SRV_INNODB_MONITOR, &srv_innodb_monitor_mutex);

	srv_sys.n_sys_threads = srv_read_only_mode
		? 0
		: srv_n_purge_threads + 1/* purge coordinator */;

	if (!srv_read_only_mode) {
		mutex_create(LATCH_ID_SRV_SYS, &srv_sys.mutex);

		mutex_create(LATCH_ID_SRV_SYS_TASKS, &srv_sys.tasks_mutex);

		for (ulint i = 0; i < srv_sys.n_sys_threads; ++i) {
			srv_slot_t*	slot = &srv_sys.sys_threads[i];

			slot->event = os_event_create(0);

			ut_a(slot->event);
		}

		srv_error_event = os_event_create(0);

		srv_monitor_event = os_event_create(0);

		srv_buf_dump_event = os_event_create(0);

		buf_flush_event = os_event_create("buf_flush_event");

		UT_LIST_INIT(srv_sys.tasks, &que_thr_t::queue);
	}

	srv_buf_resize_event = os_event_create(0);

	ut_d(srv_master_thread_disabled_event = os_event_create(0));

	/* page_zip_stat_per_index_mutex is acquired from:
	1. page_zip_compress() (after SYNC_FSP)
	2. page_zip_decompress()
	3. i_s_cmp_per_index_fill_low() (where SYNC_DICT is acquired)
	4. innodb_cmp_per_index_update(), no other latches
	since we do not acquire any other latches while holding this mutex,
	it can have very low level. We pick SYNC_ANY_LATCH for it. */
	mutex_create(LATCH_ID_PAGE_ZIP_STAT_PER_INDEX,
		     &page_zip_stat_per_index_mutex);

	/* Create dummy indexes for infimum and supremum records */

	dict_ind_init();

#ifdef WITH_INNODB_DISALLOW_WRITES
	/* Writes have to be enabled on init or else we hang. Thus, we
	always set the event here regardless of innobase_disallow_writes.
	That flag will always be 0 at this point because it isn't settable
	via my.cnf or command line arg. */
	srv_allow_writes_event = os_event_create(0);
	os_event_set(srv_allow_writes_event);
#endif /* WITH_INNODB_DISALLOW_WRITES */

	/* Initialize some INFORMATION SCHEMA internal structures */
	trx_i_s_cache_init(trx_i_s_cache);

	ut_crc32_init();

	dict_mem_init();
}

/*********************************************************************//**
Frees the data structures created in srv_init(). */
void
srv_free(void)
/*==========*/
{
	if (!srv_buf_resize_event) {
		return;
	}

	mutex_free(&srv_innodb_monitor_mutex);
	mutex_free(&page_zip_stat_per_index_mutex);

	if (!srv_read_only_mode) {
		mutex_free(&srv_sys.mutex);
		mutex_free(&srv_sys.tasks_mutex);

		for (ulint i = 0; i < srv_sys.n_sys_threads; ++i) {
			os_event_destroy(srv_sys.sys_threads[i].event);
		}

		os_event_destroy(srv_error_event);
		os_event_destroy(srv_monitor_event);
		os_event_destroy(srv_buf_dump_event);
		os_event_destroy(buf_flush_event);
	}

	os_event_destroy(srv_buf_resize_event);

	ut_d(os_event_destroy(srv_master_thread_disabled_event));

	dict_ind_free();

	trx_i_s_cache_free(trx_i_s_cache);
}

/*********************************************************************//**
Normalizes init parameter values to use units we use inside InnoDB. */
static
void
srv_normalize_init_values(void)
/*===========================*/
{
	srv_sys_space.normalize();

	srv_tmp_space.normalize();

	srv_log_buffer_size /= UNIV_PAGE_SIZE;

	srv_lock_table_size = 5 * (srv_buf_pool_size / UNIV_PAGE_SIZE);
}

/*********************************************************************//**
Boots the InnoDB server. */
void
srv_boot(void)
/*==========*/
{
	/* Transform the init parameter values given by MySQL to
	use units we use inside InnoDB: */

	srv_normalize_init_values();

	sync_check_init();
	/* Reset the system variables in the recovery module. */
	recv_sys_var_init();
	trx_pool_init();
	row_mysql_init();

	/* Initialize this module */

	srv_init();
}

/******************************************************************//**
Refreshes the values used to calculate per-second averages. */
static
void
srv_refresh_innodb_monitor_stats(void)
/*==================================*/
{
	mutex_enter(&srv_innodb_monitor_mutex);

	time_t current_time = time(NULL);

	if (difftime(current_time, srv_last_monitor_time) <= 60) {
		/* We referesh InnoDB Monitor values so that averages are
		printed from at most 60 last seconds */
		mutex_exit(&srv_innodb_monitor_mutex);
		return;
	}

	srv_last_monitor_time = current_time;

	os_aio_refresh_stats();

#ifdef BTR_CUR_HASH_ADAPT
	btr_cur_n_sea_old = btr_cur_n_sea;
#endif /* BTR_CUR_HASH_ADAPT */
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;

	log_refresh_stats();

	buf_refresh_io_stats_all();

	srv_n_rows_inserted_old = srv_stats.n_rows_inserted;
	srv_n_rows_updated_old = srv_stats.n_rows_updated;
	srv_n_rows_deleted_old = srv_stats.n_rows_deleted;
	srv_n_rows_read_old = srv_stats.n_rows_read;

	srv_n_system_rows_inserted_old = srv_stats.n_system_rows_inserted;
	srv_n_system_rows_updated_old = srv_stats.n_system_rows_updated;
	srv_n_system_rows_deleted_old = srv_stats.n_system_rows_deleted;
	srv_n_system_rows_read_old = srv_stats.n_system_rows_read;

	mutex_exit(&srv_innodb_monitor_mutex);
}

/******************************************************************//**
Outputs to a file the output of the InnoDB Monitor.
@return FALSE if not all information printed
due to failure to obtain necessary mutex */
ibool
srv_printf_innodb_monitor(
/*======================*/
	FILE*	file,		/*!< in: output stream */
	ibool	nowait,		/*!< in: whether to wait for the
				lock_sys_t:: mutex */
	ulint*	trx_start_pos,	/*!< out: file position of the start of
				the list of active transactions */
	ulint*	trx_end)	/*!< out: file position of the end of
				the list of active transactions */
{
	double	time_elapsed;
	time_t	current_time;
	ulint	n_reserved;
	ibool	ret;

	mutex_enter(&srv_innodb_monitor_mutex);

	current_time = time(NULL);

	/* We add 0.001 seconds to time_elapsed to prevent division
	by zero if two users happen to call SHOW ENGINE INNODB STATUS at the
	same time */

	time_elapsed = difftime(current_time, srv_last_monitor_time)
		+ 0.001;

	srv_last_monitor_time = time(NULL);

	fputs("\n=====================================\n", file);

	ut_print_timestamp(file);
	fprintf(file,
		" INNODB MONITOR OUTPUT\n"
		"=====================================\n"
		"Per second averages calculated from the last %lu seconds\n",
		(ulong) time_elapsed);

	fputs("-----------------\n"
	      "BACKGROUND THREAD\n"
	      "-----------------\n", file);
	srv_print_master_thread_info(file);

	fputs("----------\n"
	      "SEMAPHORES\n"
	      "----------\n", file);

	sync_print(file);

	/* Conceptually, srv_innodb_monitor_mutex has a very high latching
	order level in sync0sync.h, while dict_foreign_err_mutex has a very
	low level 135. Therefore we can reserve the latter mutex here without
	a danger of a deadlock of threads. */

	mutex_enter(&dict_foreign_err_mutex);

	if (!srv_read_only_mode && ftell(dict_foreign_err_file) != 0L) {
		fputs("------------------------\n"
		      "LATEST FOREIGN KEY ERROR\n"
		      "------------------------\n", file);
		ut_copy_file(file, dict_foreign_err_file);
	}

	mutex_exit(&dict_foreign_err_mutex);

	/* Only if lock_print_info_summary proceeds correctly,
	before we call the lock_print_info_all_transactions
	to print all the lock information. IMPORTANT NOTE: This
	function acquires the lock mutex on success. */
	ret = lock_print_info_summary(file, nowait);

	if (ret) {
		if (trx_start_pos) {
			long	t = ftell(file);
			if (t < 0) {
				*trx_start_pos = ULINT_UNDEFINED;
			} else {
				*trx_start_pos = (ulint) t;
			}
		}

		/* NOTE: If we get here then we have the lock mutex. This
		function will release the lock mutex that we acquired when
		we called the lock_print_info_summary() function earlier. */

		lock_print_info_all_transactions(file);

		if (trx_end) {
			long	t = ftell(file);
			if (t < 0) {
				*trx_end = ULINT_UNDEFINED;
			} else {
				*trx_end = (ulint) t;
			}
		}
	}

	fputs("--------\n"
	      "FILE I/O\n"
	      "--------\n", file);
	os_aio_print(file);

	fputs("-------------------------------------\n"
	      "INSERT BUFFER AND ADAPTIVE HASH INDEX\n"
	      "-------------------------------------\n", file);
	ibuf_print(file);

#ifdef BTR_CUR_HASH_ADAPT
	btr_search_x_lock_all();
	for (ulint i = 0; i < btr_ahi_parts && btr_search_enabled; ++i) {
		const hash_table_t* table = btr_search_sys->hash_tables[i];

		ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);
		/* this is only used for buf_pool->page_hash */
		ut_ad(!table->heaps);
		/* this is used for the adaptive hash index */
		ut_ad(table->heap);

		const mem_heap_t* heap = table->heap;
		/* The heap may change during the following call,
		so the data displayed may be garbage. We intentionally
		avoid acquiring btr_search_latches[] so that the
		diagnostic output will not stop here even in case another
		thread hangs while holding btr_search_latches[].

		This should be safe from crashes, because
		table->heap will be pointing to the same object
		for the full lifetime of the server. Even during
		btr_search_disable() the heap will stay valid. */
		fprintf(file, "Hash table size " ULINTPF
			", node heap has " ULINTPF " buffer(s)\n",
			table->n_cells, heap->base.count - !heap->free_block);
	}
	btr_search_x_unlock_all();

	fprintf(file,
		"%.2f hash searches/s, %.2f non-hash searches/s\n",
		(btr_cur_n_sea - btr_cur_n_sea_old)
		/ time_elapsed,
		(btr_cur_n_non_sea - btr_cur_n_non_sea_old)
		/ time_elapsed);
	btr_cur_n_sea_old = btr_cur_n_sea;
#else /* BTR_CUR_HASH_ADAPT */
	fprintf(file,
		"%.2f non-hash searches/s\n",
		(btr_cur_n_non_sea - btr_cur_n_non_sea_old)
		/ time_elapsed);
#endif /* BTR_CUR_HASH_ADAPT */
	btr_cur_n_non_sea_old = btr_cur_n_non_sea;

	fputs("---\n"
	      "LOG\n"
	      "---\n", file);
	log_print(file);

	fputs("----------------------\n"
	      "BUFFER POOL AND MEMORY\n"
	      "----------------------\n", file);
	fprintf(file,
		"Total large memory allocated " ULINTPF "\n"
		"Dictionary memory allocated " ULINTPF "\n",
		os_total_large_mem_allocated,
		dict_sys_get_size());

	buf_print_io(file);

	fputs("--------------\n"
	      "ROW OPERATIONS\n"
	      "--------------\n", file);
	fprintf(file,
		ULINTPF " queries inside InnoDB, "
		ULINTPF " queries in queue\n",
		srv_conc_get_active_threads(),
		srv_conc_get_waiting_threads());

	/* This is a dirty read, without holding trx_sys->mutex. */
	fprintf(file, ULINTPF " read views open inside InnoDB\n",
		trx_sys->mvcc->size());

	n_reserved = fil_space_get_n_reserved_extents(0);
	if (n_reserved > 0) {
		fprintf(file,
			ULINTPF " tablespace extents now reserved for"
			" B-tree split operations\n",
			n_reserved);
	}

	fprintf(file,
		"Process ID=" ULINTPF
		", Main thread ID=" ULINTPF
		", state: %s\n",
		srv_main_thread_process_no,
		srv_main_thread_id,
		srv_main_thread_op_info);
	fprintf(file,
		"Number of rows inserted " ULINTPF
		", updated " ULINTPF
		", deleted " ULINTPF
		", read " ULINTPF "\n",
		(ulint) srv_stats.n_rows_inserted,
		(ulint) srv_stats.n_rows_updated,
		(ulint) srv_stats.n_rows_deleted,
		(ulint) srv_stats.n_rows_read);
	fprintf(file,
		"%.2f inserts/s, %.2f updates/s,"
		" %.2f deletes/s, %.2f reads/s\n",
		((ulint) srv_stats.n_rows_inserted - srv_n_rows_inserted_old)
		/ time_elapsed,
		((ulint) srv_stats.n_rows_updated - srv_n_rows_updated_old)
		/ time_elapsed,
		((ulint) srv_stats.n_rows_deleted - srv_n_rows_deleted_old)
		/ time_elapsed,
		((ulint) srv_stats.n_rows_read - srv_n_rows_read_old)
		/ time_elapsed);
	fprintf(file,
		"Number of system rows inserted " ULINTPF
		", updated " ULINTPF ", deleted " ULINTPF
		", read " ULINTPF "\n",
		(ulint) srv_stats.n_system_rows_inserted,
		(ulint) srv_stats.n_system_rows_updated,
		(ulint) srv_stats.n_system_rows_deleted,
		(ulint) srv_stats.n_system_rows_read);
	fprintf(file,
		"%.2f inserts/s, %.2f updates/s,"
		" %.2f deletes/s, %.2f reads/s\n",
		((ulint) srv_stats.n_system_rows_inserted
		 - srv_n_system_rows_inserted_old) / time_elapsed,
		((ulint) srv_stats.n_system_rows_updated
		 - srv_n_system_rows_updated_old) / time_elapsed,
		((ulint) srv_stats.n_system_rows_deleted
		 - srv_n_system_rows_deleted_old) / time_elapsed,
		((ulint) srv_stats.n_system_rows_read
		 - srv_n_system_rows_read_old) / time_elapsed);
	srv_n_rows_inserted_old = srv_stats.n_rows_inserted;
	srv_n_rows_updated_old = srv_stats.n_rows_updated;
	srv_n_rows_deleted_old = srv_stats.n_rows_deleted;
	srv_n_rows_read_old = srv_stats.n_rows_read;
	srv_n_system_rows_inserted_old = srv_stats.n_system_rows_inserted;
	srv_n_system_rows_updated_old = srv_stats.n_system_rows_updated;
	srv_n_system_rows_deleted_old = srv_stats.n_system_rows_deleted;
	srv_n_system_rows_read_old = srv_stats.n_system_rows_read;

	fputs("----------------------------\n"
	      "END OF INNODB MONITOR OUTPUT\n"
	      "============================\n", file);
	mutex_exit(&srv_innodb_monitor_mutex);
	fflush(file);

	return(ret);
}

/******************************************************************//**
Function to pass InnoDB status variables to MySQL */
void
srv_export_innodb_status(void)
/*==========================*/
{
	buf_pool_stat_t		stat;
	buf_pools_list_size_t	buf_pools_list_size;
	ulint			LRU_len;
	ulint			free_len;
	ulint			flush_list_len;
	fil_crypt_stat_t	crypt_stat;
	btr_scrub_stat_t	scrub_stat;

	buf_get_total_stat(&stat);
	buf_get_total_list_len(&LRU_len, &free_len, &flush_list_len);
	buf_get_total_list_size_in_bytes(&buf_pools_list_size);
	if (!srv_read_only_mode) {
		fil_crypt_total_stat(&crypt_stat);
		btr_scrub_total_stat(&scrub_stat);
	}

	mutex_enter(&srv_innodb_monitor_mutex);

	export_vars.innodb_data_pending_reads =
		ulint(MONITOR_VALUE(MONITOR_OS_PENDING_READS));

	export_vars.innodb_data_pending_writes =
		ulint(MONITOR_VALUE(MONITOR_OS_PENDING_WRITES));

	export_vars.innodb_data_pending_fsyncs =
		fil_n_pending_log_flushes
		+ fil_n_pending_tablespace_flushes;

	export_vars.innodb_data_fsyncs = os_n_fsyncs;

	export_vars.innodb_data_read = srv_stats.data_read;

	export_vars.innodb_data_reads = os_n_file_reads;

	export_vars.innodb_data_writes = os_n_file_writes;

	export_vars.innodb_data_written = srv_stats.data_written;

	export_vars.innodb_buffer_pool_read_requests = stat.n_page_gets;

	export_vars.innodb_buffer_pool_write_requests =
		srv_stats.buf_pool_write_requests;

	export_vars.innodb_buffer_pool_wait_free =
		srv_stats.buf_pool_wait_free;

	export_vars.innodb_buffer_pool_pages_flushed =
		srv_stats.buf_pool_flushed;

	export_vars.innodb_buffer_pool_reads = srv_stats.buf_pool_reads;

	export_vars.innodb_buffer_pool_read_ahead_rnd =
		stat.n_ra_pages_read_rnd;

	export_vars.innodb_buffer_pool_read_ahead =
		stat.n_ra_pages_read;

	export_vars.innodb_buffer_pool_read_ahead_evicted =
		stat.n_ra_pages_evicted;

	export_vars.innodb_buffer_pool_pages_data = LRU_len;

	export_vars.innodb_buffer_pool_bytes_data =
		buf_pools_list_size.LRU_bytes
		+ buf_pools_list_size.unzip_LRU_bytes;

	export_vars.innodb_buffer_pool_pages_dirty = flush_list_len;

	export_vars.innodb_buffer_pool_bytes_dirty =
		buf_pools_list_size.flush_list_bytes;

	export_vars.innodb_buffer_pool_pages_free = free_len;

#ifdef UNIV_DEBUG
	export_vars.innodb_buffer_pool_pages_latched =
		buf_get_latched_pages_number();
#endif /* UNIV_DEBUG */
	export_vars.innodb_buffer_pool_pages_total = buf_pool_get_n_pages();

	export_vars.innodb_buffer_pool_pages_misc =
		buf_pool_get_n_pages() - LRU_len - free_len;

#ifdef HAVE_ATOMIC_BUILTINS
	export_vars.innodb_have_atomic_builtins = 1;
#else
	export_vars.innodb_have_atomic_builtins = 0;
#endif

	export_vars.innodb_page_size = UNIV_PAGE_SIZE;

	export_vars.innodb_log_waits = srv_stats.log_waits;

	export_vars.innodb_os_log_written = srv_stats.os_log_written;

	export_vars.innodb_os_log_fsyncs = fil_n_log_flushes;

	export_vars.innodb_os_log_pending_fsyncs = fil_n_pending_log_flushes;

	export_vars.innodb_os_log_pending_writes =
		srv_stats.os_log_pending_writes;

	export_vars.innodb_log_write_requests = srv_stats.log_write_requests;

	export_vars.innodb_log_writes = srv_stats.log_writes;

	export_vars.innodb_dblwr_pages_written =
		srv_stats.dblwr_pages_written;

	export_vars.innodb_dblwr_writes = srv_stats.dblwr_writes;

	export_vars.innodb_pages_created = stat.n_pages_created;

	export_vars.innodb_pages_read = stat.n_pages_read;
	export_vars.innodb_page0_read = srv_stats.page0_read;

	export_vars.innodb_pages_written = stat.n_pages_written;

	export_vars.innodb_row_lock_waits = srv_stats.n_lock_wait_count;

	export_vars.innodb_row_lock_current_waits =
		srv_stats.n_lock_wait_current_count;

	export_vars.innodb_row_lock_time = srv_stats.n_lock_wait_time / 1000;

	if (srv_stats.n_lock_wait_count > 0) {

		export_vars.innodb_row_lock_time_avg = (ulint)
			(srv_stats.n_lock_wait_time
			 / 1000 / srv_stats.n_lock_wait_count);

	} else {
		export_vars.innodb_row_lock_time_avg = 0;
	}

	export_vars.innodb_row_lock_time_max =
		lock_sys->n_lock_max_wait_time / 1000;

	export_vars.innodb_rows_read = srv_stats.n_rows_read;

	export_vars.innodb_rows_inserted = srv_stats.n_rows_inserted;

	export_vars.innodb_rows_updated = srv_stats.n_rows_updated;

	export_vars.innodb_rows_deleted = srv_stats.n_rows_deleted;

	export_vars.innodb_system_rows_read = srv_stats.n_system_rows_read;

	export_vars.innodb_system_rows_inserted =
		srv_stats.n_system_rows_inserted;

	export_vars.innodb_system_rows_updated =
		srv_stats.n_system_rows_updated;

	export_vars.innodb_system_rows_deleted =
		srv_stats.n_system_rows_deleted;

	export_vars.innodb_num_open_files = fil_system->n_open;

	export_vars.innodb_truncated_status_writes =
		srv_truncated_status_writes;

	export_vars.innodb_available_undo_logs = srv_available_undo_logs;
	export_vars.innodb_page_compression_saved = srv_stats.page_compression_saved;
	export_vars.innodb_index_pages_written = srv_stats.index_pages_written;
	export_vars.innodb_non_index_pages_written = srv_stats.non_index_pages_written;
	export_vars.innodb_pages_page_compressed = srv_stats.pages_page_compressed;
	export_vars.innodb_page_compressed_trim_op = srv_stats.page_compressed_trim_op;
	export_vars.innodb_pages_page_decompressed = srv_stats.pages_page_decompressed;
	export_vars.innodb_pages_page_compression_error = srv_stats.pages_page_compression_error;
	export_vars.innodb_pages_decrypted = srv_stats.pages_decrypted;
	export_vars.innodb_pages_encrypted = srv_stats.pages_encrypted;
	export_vars.innodb_n_merge_blocks_encrypted = srv_stats.n_merge_blocks_encrypted;
	export_vars.innodb_n_merge_blocks_decrypted = srv_stats.n_merge_blocks_decrypted;
	export_vars.innodb_n_rowlog_blocks_encrypted = srv_stats.n_rowlog_blocks_encrypted;
	export_vars.innodb_n_rowlog_blocks_decrypted = srv_stats.n_rowlog_blocks_decrypted;

	export_vars.innodb_n_temp_blocks_encrypted =
		srv_stats.n_temp_blocks_encrypted;

	export_vars.innodb_n_temp_blocks_decrypted =
		srv_stats.n_temp_blocks_decrypted;

	export_vars.innodb_defragment_compression_failures =
		btr_defragment_compression_failures;
	export_vars.innodb_defragment_failures = btr_defragment_failures;
	export_vars.innodb_defragment_count = btr_defragment_count;

	export_vars.innodb_onlineddl_rowlog_rows = onlineddl_rowlog_rows;
	export_vars.innodb_onlineddl_rowlog_pct_used = onlineddl_rowlog_pct_used;
	export_vars.innodb_onlineddl_pct_progress = onlineddl_pct_progress;

	export_vars.innodb_sec_rec_cluster_reads =
		srv_stats.n_sec_rec_cluster_reads;
	export_vars.innodb_sec_rec_cluster_reads_avoided =
		srv_stats.n_sec_rec_cluster_reads_avoided;

	if (!srv_read_only_mode) {
	export_vars.innodb_encryption_rotation_pages_read_from_cache =
		crypt_stat.pages_read_from_cache;
	export_vars.innodb_encryption_rotation_pages_read_from_disk =
		crypt_stat.pages_read_from_disk;
	export_vars.innodb_encryption_rotation_pages_modified =
		crypt_stat.pages_modified;
	export_vars.innodb_encryption_rotation_pages_flushed =
		crypt_stat.pages_flushed;
	export_vars.innodb_encryption_rotation_estimated_iops =
		crypt_stat.estimated_iops;
	export_vars.innodb_encryption_key_requests =
		srv_stats.n_key_requests;
	export_vars.innodb_key_rotation_list_length =
		srv_stats.key_rotation_list_length;

	export_vars.innodb_scrub_page_reorganizations =
		scrub_stat.page_reorganizations;
	export_vars.innodb_scrub_page_splits =
		scrub_stat.page_splits;
	export_vars.innodb_scrub_page_split_failures_underflow =
		scrub_stat.page_split_failures_underflow;
	export_vars.innodb_scrub_page_split_failures_out_of_filespace =
		scrub_stat.page_split_failures_out_of_filespace;
	export_vars.innodb_scrub_page_split_failures_missing_index =
		scrub_stat.page_split_failures_missing_index;
	export_vars.innodb_scrub_page_split_failures_unknown =
		scrub_stat.page_split_failures_unknown;
	export_vars.innodb_scrub_log = srv_stats.n_log_scrubs;
	}

	mutex_exit(&srv_innodb_monitor_mutex);
}

/*********************************************************************//**
A thread which prints the info output by various InnoDB monitors.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(srv_monitor_thread)(void*)
{
	int64_t		sig_count;
	double		time_elapsed;
	time_t		current_time;
	time_t		last_monitor_time;
	ulint		mutex_skipped;
	ibool		last_srv_print_monitor;

	ut_ad(!srv_read_only_mode);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Lock timeout thread starts, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_monitor_thread_key);
#endif /* UNIV_PFS_THREAD */

	current_time = time(NULL);
	srv_last_monitor_time = current_time;
	last_monitor_time = current_time;
	mutex_skipped = 0;
	last_srv_print_monitor = srv_print_innodb_monitor;
loop:
	/* Wake up every 5 seconds to see if we need to print
	monitor information or if signalled at shutdown. */

	sig_count = os_event_reset(srv_monitor_event);

	os_event_wait_time_low(srv_monitor_event, 5000000, sig_count);

	current_time = time(NULL);

	time_elapsed = difftime(current_time, last_monitor_time);

	if (time_elapsed > 15) {
		last_monitor_time = current_time;

		if (srv_print_innodb_monitor) {
			/* Reset mutex_skipped counter everytime
			srv_print_innodb_monitor changes. This is to
			ensure we will not be blocked by lock_sys->mutex
			for short duration information printing,
			such as requested by sync_array_print_long_waits() */
			if (!last_srv_print_monitor) {
				mutex_skipped = 0;
				last_srv_print_monitor = TRUE;
			}

			if (!srv_printf_innodb_monitor(stderr,
						MUTEX_NOWAIT(mutex_skipped),
						NULL, NULL)) {
				mutex_skipped++;
			} else {
				/* Reset the counter */
				mutex_skipped = 0;
			}
		} else {
			last_srv_print_monitor = FALSE;
		}


		/* We don't create the temp files or associated
		mutexes in read-only-mode */

		if (!srv_read_only_mode && srv_innodb_status) {
			mutex_enter(&srv_monitor_file_mutex);
			rewind(srv_monitor_file);
			if (!srv_printf_innodb_monitor(srv_monitor_file,
						MUTEX_NOWAIT(mutex_skipped),
						NULL, NULL)) {
				mutex_skipped++;
			} else {
				mutex_skipped = 0;
			}

			os_file_set_eof(srv_monitor_file);
			mutex_exit(&srv_monitor_file_mutex);
		}
	}

	srv_refresh_innodb_monitor_stats();

	if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
		goto exit_func;
	}

	if (srv_print_innodb_monitor
	    || srv_print_innodb_lock_monitor) {
		goto loop;
	}

	goto loop;

exit_func:
	srv_monitor_active = false;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/*********************************************************************//**
A thread which prints warnings about semaphore waits which have lasted
too long. These can be used to track bugs which cause hangs.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(srv_error_monitor_thread)(void*)
{
	/* number of successive fatal timeouts observed */
	ulint		fatal_cnt	= 0;
	lsn_t		old_lsn;
	lsn_t		new_lsn;
	int64_t		sig_count;
	/* longest waiting thread for a semaphore */
	os_thread_id_t	waiter		= os_thread_get_curr_id();
	os_thread_id_t	old_waiter	= waiter;
	/* the semaphore that is being waited for */
	const void*	sema		= NULL;
	const void*	old_sema	= NULL;

	ut_ad(!srv_read_only_mode);

	old_lsn = srv_start_lsn;

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Error monitor thread starts, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_error_monitor_thread_key);
#endif /* UNIV_PFS_THREAD */

loop:
	/* Try to track a strange bug reported by Harald Fuchs and others,
	where the lsn seems to decrease at times */

	if (log_peek_lsn(&new_lsn)) {
		if (new_lsn < old_lsn) {
		ib::error() << "Old log sequence number " << old_lsn << " was"
			<< " greater than the new log sequence number "
			<< new_lsn << ". Please submit a bug report to"
			" https://jira.mariadb.org/";
			ut_ad(0);
		}

		old_lsn = new_lsn;
	}

	/* Update the statistics collected for deciding LRU
	eviction policy. */
	buf_LRU_stat_update();

	if (sync_array_print_long_waits(&waiter, &sema)
	    && sema == old_sema && os_thread_eq(waiter, old_waiter)) {
#if defined(WITH_WSREP) && defined(WITH_INNODB_DISALLOW_WRITES)
	  if (os_event_is_set(srv_allow_writes_event)) {
#endif /* WITH_WSREP */
		fatal_cnt++;
#if defined(WITH_WSREP) && defined(WITH_INNODB_DISALLOW_WRITES)
	  } else {
		fprintf(stderr,
			"WSREP: avoiding InnoDB self crash due to long "
			"semaphore wait of  > %lu seconds\n"
			"Server is processing SST donor operation, "
			"fatal_cnt now: " ULINTPF,
			srv_fatal_semaphore_wait_threshold, fatal_cnt);
	  }
#endif /* WITH_WSREP */
		if (fatal_cnt > 10) {
			ib::fatal() << "Semaphore wait has lasted > "
				<< srv_fatal_semaphore_wait_threshold
				<< " seconds. We intentionally crash the"
				" server because it appears to be hung.";
		}
	} else {
		fatal_cnt = 0;
		old_waiter = waiter;
		old_sema = sema;
	}

	/* Flush stderr so that a database user gets the output
	to possible MySQL error file */

	fflush(stderr);

	sig_count = os_event_reset(srv_error_event);

	os_event_wait_time_low(srv_error_event, 1000000, sig_count);

	if (srv_shutdown_state <= SRV_SHUTDOWN_INITIATED) {

		goto loop;
	}

	srv_error_monitor_active = false;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/******************************************************************//**
Increment the server activity count. */
void
srv_inc_activity_count(void)
/*========================*/
{
	srv_sys.activity_count.inc();
}

/**********************************************************************//**
Check whether any background thread is active. If so return the thread
type.
@return SRV_NONE if all are suspended or have exited, thread
type if any are still active. */
srv_thread_type
srv_get_active_thread_type(void)
/*============================*/
{
	srv_thread_type ret = SRV_NONE;

	if (srv_read_only_mode) {
		return(SRV_NONE);
	}

	srv_sys_mutex_enter();

	for (ulint i = SRV_WORKER; i <= SRV_MASTER; ++i) {
		if (srv_sys.n_threads_active[i] != 0) {
			ret = static_cast<srv_thread_type>(i);
			break;
		}
	}

	srv_sys_mutex_exit();

	if (ret == SRV_NONE && srv_shutdown_state > SRV_SHUTDOWN_INITIATED
	    && purge_sys != NULL) {
		/* Check only on shutdown. */
		switch (trx_purge_state()) {
		case PURGE_STATE_RUN:
		case PURGE_STATE_STOP:
			ret = SRV_PURGE;
			break;
		case PURGE_STATE_INIT:
		case PURGE_STATE_DISABLED:
		case PURGE_STATE_EXIT:
			break;
		}
	}

	return(ret);
}

/** Wake up the InnoDB master thread if it was suspended (not sleeping). */
void
srv_active_wake_master_thread_low()
{
	ut_ad(!srv_read_only_mode);
	ut_ad(!srv_sys_mutex_own());

	srv_inc_activity_count();

	if (my_atomic_loadlint(&srv_sys.n_threads_active[SRV_MASTER]) == 0) {
		srv_slot_t*	slot;

		srv_sys_mutex_enter();

		slot = &srv_sys.sys_threads[SRV_MASTER_SLOT];

		/* Only if the master thread has been started. */

		if (slot->in_use) {
			ut_a(srv_slot_get_type(slot) == SRV_MASTER);
			os_event_set(slot->event);
		}

		srv_sys_mutex_exit();
	}
}

/** Wake up the purge threads if there is work to do. */
void
srv_wake_purge_thread_if_not_active()
{
	ut_ad(!srv_sys_mutex_own());

	if (purge_sys->state == PURGE_STATE_RUN
	    && !my_atomic_loadlint(&srv_sys.n_threads_active[SRV_PURGE])
	    && my_atomic_loadlint(&trx_sys->rseg_history_len)) {

		srv_release_threads(SRV_PURGE, 1);
	}
}

/** Wake up the master thread if it is suspended or being suspended. */
void
srv_wake_master_thread()
{
	srv_inc_activity_count();
	srv_release_threads(SRV_MASTER, 1);
}

/*******************************************************************//**
Get current server activity count. We don't hold srv_sys::mutex while
reading this value as it is only used in heuristics.
@return activity count. */
ulint
srv_get_activity_count(void)
/*========================*/
{
	return(srv_sys.activity_count);
}

/*******************************************************************//**
Check if there has been any activity.
@return FALSE if no change in activity counter. */
ibool
srv_check_activity(
/*===============*/
	ulint		old_activity_count)	/*!< in: old activity count */
{
	return(srv_sys.activity_count != old_activity_count);
}

/********************************************************************//**
The master thread is tasked to ensure that flush of log file happens
once every second in the background. This is to ensure that not more
than one second of trxs are lost in case of crash when
innodb_flush_logs_at_trx_commit != 1 */
static
void
srv_sync_log_buffer_in_background(void)
/*===================================*/
{
	time_t	current_time = time(NULL);

	srv_main_thread_op_info = "flushing log";
	if (difftime(current_time, srv_last_log_flush_time)
	    >= srv_flush_log_at_timeout) {
		log_buffer_sync_in_background(true);
		srv_last_log_flush_time = current_time;
		srv_log_writes_and_flush++;
	}
}

/********************************************************************//**
Make room in the table cache by evicting an unused table.
@return number of tables evicted. */
static
ulint
srv_master_evict_from_table_cache(
/*==============================*/
	ulint	pct_check)	/*!< in: max percent to check */
{
	ulint	n_tables_evicted = 0;

	rw_lock_x_lock(&dict_operation_lock);

	dict_mutex_enter_for_mysql();

	n_tables_evicted = dict_make_room_in_cache(
		innobase_get_table_cache_size(), pct_check);

	dict_mutex_exit_for_mysql();

	rw_lock_x_unlock(&dict_operation_lock);

	return(n_tables_evicted);
}

/*********************************************************************//**
This function prints progress message every 60 seconds during server
shutdown, for any activities that master thread is pending on. */
static
void
srv_shutdown_print_master_pending(
/*==============================*/
	time_t*		last_print_time,	/*!< last time the function
						print the message */
	ulint		n_tables_to_drop,	/*!< number of tables to
						be dropped */
	ulint		n_bytes_merged)		/*!< number of change buffer
						just merged */
{
	time_t current_time = time(NULL);

	if (difftime(current_time, *last_print_time) > 60) {
		*last_print_time = current_time;

		if (n_tables_to_drop) {
			ib::info() << "Waiting for " << n_tables_to_drop
				<< " table(s) to be dropped";
		}

		/* Check change buffer merge, we only wait for change buffer
		merge if it is a slow shutdown */
		if (!srv_fast_shutdown && n_bytes_merged) {
			ib::info() << "Waiting for change buffer merge to"
				" complete number of bytes of change buffer"
				" just merged: " << n_bytes_merged;
		}
	}
}

#ifdef UNIV_DEBUG
/** Waits in loop as long as master thread is disabled (debug) */
static
void
srv_master_do_disabled_loop(void)
{
	if (!srv_master_thread_disabled_debug) {
		/* We return here to avoid changing op_info. */
		return;
	}

	srv_main_thread_op_info = "disabled";

	while (srv_master_thread_disabled_debug) {
		os_event_set(srv_master_thread_disabled_event);
		if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
			break;
		}
		os_thread_sleep(100000);
	}

	srv_main_thread_op_info = "";
}

/** Disables master thread. It's used by:
	SET GLOBAL innodb_master_thread_disabled_debug = 1 (0).
@param[in]	thd		thread handle
@param[in]	var		pointer to system variable
@param[out]	var_ptr		where the formal string goes
@param[in]	save		immediate result from check function */
void
srv_master_thread_disabled_debug_update(
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				var_ptr,
	const void*			save)
{
	/* This method is protected by mutex, as every SET GLOBAL .. */
	ut_ad(srv_master_thread_disabled_event != NULL);

	const bool disable = *static_cast<const my_bool*>(save);

	const int64_t sig_count = os_event_reset(
		srv_master_thread_disabled_event);

	srv_master_thread_disabled_debug = disable;

	if (disable) {
		os_event_wait_low(
			srv_master_thread_disabled_event, sig_count);
	}
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Perform the tasks that the master thread is supposed to do when the
server is active. There are two types of tasks. The first category is
of such tasks which are performed at each inovcation of this function.
We assume that this function is called roughly every second when the
server is active. The second category is of such tasks which are
performed at some interval e.g.: purge, dict_LRU cleanup etc. */
static
void
srv_master_do_active_tasks(void)
/*============================*/
{
	time_t		cur_time = time(NULL);
	ulonglong	counter_time = microsecond_interval_timer();

	/* First do the tasks that we are suppose to do at each
	invocation of this function. */

	++srv_main_active_loops;

	MONITOR_INC(MONITOR_MASTER_ACTIVE_LOOPS);

	/* ALTER TABLE in MySQL requires on Unix that the table handler
	can drop tables lazily after there no longer are SELECT
	queries to them. */
	srv_main_thread_op_info = "doing background drop tables";
	row_drop_tables_for_mysql_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_BACKGROUND_DROP_TABLE_MICROSECOND, counter_time);

	ut_d(srv_master_do_disabled_loop());

	if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
		return;
	}

	/* make sure that there is enough reusable space in the redo
	log files */
	srv_main_thread_op_info = "checking free log space";
	log_free_check();

	/* Do an ibuf merge */
	srv_main_thread_op_info = "doing insert buffer merge";
	counter_time = microsecond_interval_timer();
	ibuf_merge_in_background(false);
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_IBUF_MERGE_MICROSECOND, counter_time);

	/* Flush logs if needed */
	srv_main_thread_op_info = "flushing log";
	srv_sync_log_buffer_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_LOG_FLUSH_MICROSECOND, counter_time);

	/* Now see if various tasks that are performed at defined
	intervals need to be performed. */

	if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
		return;
	}

	if (cur_time % SRV_MASTER_DICT_LRU_INTERVAL == 0) {
		srv_main_thread_op_info = "enforcing dict cache limit";
		ulint	n_evicted = srv_master_evict_from_table_cache(50);
		if (n_evicted != 0) {
			MONITOR_INC_VALUE(
				MONITOR_SRV_DICT_LRU_EVICT_COUNT_ACTIVE, n_evicted);
		}
		MONITOR_INC_TIME_IN_MICRO_SECS(
			MONITOR_SRV_DICT_LRU_MICROSECOND, counter_time);
	}

	/* The periodic log_checkpoint() call here makes it harder to
	reproduce bugs in crash recovery or mariabackup --prepare, or
	in code that writes the redo log records. Omitting the call
	here should not affect correctness, because log_free_check()
	should still be invoking checkpoints when needed. In a
	production server, those calls could cause "furious flushing"
	and stall the server. Normally we want to perform checkpoints
	early and often to avoid those situations. */
	DBUG_EXECUTE_IF("ib_log_checkpoint_avoid", return;);

	if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
		return;
	}

	/* Make a new checkpoint */
	if (cur_time % SRV_MASTER_CHECKPOINT_INTERVAL == 0) {
		srv_main_thread_op_info = "making checkpoint";
		log_checkpoint(true);
		MONITOR_INC_TIME_IN_MICRO_SECS(
			MONITOR_SRV_CHECKPOINT_MICROSECOND, counter_time);
	}
}

/*********************************************************************//**
Perform the tasks that the master thread is supposed to do whenever the
server is idle. We do check for the server state during this function
and if the server has entered the shutdown phase we may return from
the function without completing the required tasks.
Note that the server can move to active state when we are executing this
function but we don't check for that as we are suppose to perform more
or less same tasks when server is active. */
static
void
srv_master_do_idle_tasks(void)
/*==========================*/
{
	++srv_main_idle_loops;

	MONITOR_INC(MONITOR_MASTER_IDLE_LOOPS);


	/* ALTER TABLE in MySQL requires on Unix that the table handler
	can drop tables lazily after there no longer are SELECT
	queries to them. */
	ulonglong counter_time = microsecond_interval_timer();
	srv_main_thread_op_info = "doing background drop tables";
	row_drop_tables_for_mysql_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_BACKGROUND_DROP_TABLE_MICROSECOND,
			 counter_time);

	ut_d(srv_master_do_disabled_loop());

	if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
		return;
	}

	/* make sure that there is enough reusable space in the redo
	log files */
	srv_main_thread_op_info = "checking free log space";
	log_free_check();

	/* Do an ibuf merge */
	counter_time = microsecond_interval_timer();
	srv_main_thread_op_info = "doing insert buffer merge";
	ibuf_merge_in_background(true);
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_IBUF_MERGE_MICROSECOND, counter_time);

	if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
		return;
	}

	srv_main_thread_op_info = "enforcing dict cache limit";
	ulint	n_evicted = srv_master_evict_from_table_cache(100);
	if (n_evicted != 0) {
		MONITOR_INC_VALUE(
			MONITOR_SRV_DICT_LRU_EVICT_COUNT_IDLE, n_evicted);
	}
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_DICT_LRU_MICROSECOND, counter_time);

	/* Flush logs if needed */
	srv_sync_log_buffer_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_LOG_FLUSH_MICROSECOND, counter_time);

	/* The periodic log_checkpoint() call here makes it harder to
	reproduce bugs in crash recovery or mariabackup --prepare, or
	in code that writes the redo log records. Omitting the call
	here should not affect correctness, because log_free_check()
	should still be invoking checkpoints when needed. In a
	production server, those calls could cause "furious flushing"
	and stall the server. Normally we want to perform checkpoints
	early and often to avoid those situations. */
	DBUG_EXECUTE_IF("ib_log_checkpoint_avoid", return;);

	if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
		return;
	}

	/* Make a new checkpoint */
	srv_main_thread_op_info = "making checkpoint";
	log_checkpoint(true);
	MONITOR_INC_TIME_IN_MICRO_SECS(MONITOR_SRV_CHECKPOINT_MICROSECOND,
				       counter_time);

	/* This is a workaround to avoid the InnoDB hang when OS datetime
	changed backwards.*/
	os_event_set(buf_flush_event);
}

/** Perform shutdown tasks.
@param[in]	ibuf_merge	whether to complete the change buffer merge */
static
void
srv_shutdown(bool ibuf_merge)
{
	ulint		n_bytes_merged	= 0;
	ulint		n_tables_to_drop;
	time_t		now = time(NULL);

	do {
		ut_ad(!srv_read_only_mode);
		ut_ad(srv_shutdown_state == SRV_SHUTDOWN_CLEANUP);
		++srv_main_shutdown_loops;

		/* FIXME: Remove the background DROP TABLE queue; it is not
		crash-safe and breaks ACID. */
		srv_main_thread_op_info = "doing background drop tables";
		n_tables_to_drop = row_drop_tables_for_mysql_in_background();

		if (ibuf_merge) {
			srv_main_thread_op_info = "checking free log space";
			log_free_check();
			srv_main_thread_op_info = "doing insert buffer merge";
			n_bytes_merged = ibuf_merge_in_background(true);

			/* Flush logs if needed */
			srv_sync_log_buffer_in_background();
		}

		/* Print progress message every 60 seconds during shutdown */
		if (srv_print_verbose_log) {
			srv_shutdown_print_master_pending(
				&now, n_tables_to_drop, n_bytes_merged);
		}
	} while (n_bytes_merged || n_tables_to_drop);
}

/*********************************************************************//**
Puts master thread to sleep. At this point we are using polling to
service various activities. Master thread sleeps for one second before
checking the state of the server again */
static
void
srv_master_sleep(void)
/*==================*/
{
	srv_main_thread_op_info = "sleeping";
	os_thread_sleep(1000000);
	srv_main_thread_op_info = "";
}

/*********************************************************************//**
The master thread controlling the server.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(srv_master_thread)(
/*==============================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	my_thread_init();
	DBUG_ENTER("srv_master_thread");

	srv_slot_t*	slot;
	ulint		old_activity_count = srv_get_activity_count();

	ut_ad(!srv_read_only_mode);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Master thread starts, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_master_thread_key);
#endif /* UNIV_PFS_THREAD */

	srv_main_thread_process_no = os_proc_get_number();
	srv_main_thread_id = os_thread_pf(os_thread_get_curr_id());

	slot = srv_reserve_slot(SRV_MASTER);
	ut_a(slot == srv_sys.sys_threads);

loop:
	while (srv_shutdown_state <= SRV_SHUTDOWN_INITIATED) {
		srv_master_sleep();

		MONITOR_INC(MONITOR_MASTER_THREAD_SLEEP);

		if (srv_check_activity(old_activity_count)) {
			old_activity_count = srv_get_activity_count();
			srv_master_do_active_tasks();
		} else {
			srv_master_do_idle_tasks();
		}
	}

	switch (srv_shutdown_state) {
	case SRV_SHUTDOWN_NONE:
	case SRV_SHUTDOWN_INITIATED:
		break;
	case SRV_SHUTDOWN_FLUSH_PHASE:
	case SRV_SHUTDOWN_LAST_PHASE:
		ut_ad(0);
		/* fall through */
	case SRV_SHUTDOWN_EXIT_THREADS:
		/* srv_init_abort() must have been invoked */
	case SRV_SHUTDOWN_CLEANUP:
		if (srv_shutdown_state == SRV_SHUTDOWN_CLEANUP
		    && srv_fast_shutdown < 2) {
			srv_shutdown(srv_fast_shutdown == 0);
		}
		srv_suspend_thread(slot);
		my_thread_end();
		os_thread_exit();
	}

	srv_main_thread_op_info = "suspending";

	srv_suspend_thread(slot);

	/* DO NOT CHANGE THIS STRING. innobase_start_or_create_for_mysql()
	waits for database activity to die down when converting < 4.1.x
	databases, and relies on this string being exactly as it is. InnoDB
	manual also mentions this string in several places. */
	srv_main_thread_op_info = "waiting for server activity";

	srv_resume_thread(slot);
	goto loop;
}

/** Check if purge should stop.
@param[in]	n_purged	pages purged in the last batch
@return whether purge should exit */
static
bool
srv_purge_should_exit(ulint n_purged)
{
	ut_ad(srv_shutdown_state <= SRV_SHUTDOWN_CLEANUP);

	if (srv_undo_sources) {
		return(false);
	}
	if (srv_fast_shutdown) {
		return(true);
	}
	/* Slow shutdown was requested. */
	if (ulint history_size = n_purged ? trx_sys->rseg_history_len : 0) {
		static time_t progress_time;
		time_t now = time(NULL);
		if (now - progress_time >= 15) {
			progress_time = now;
#if defined HAVE_SYSTEMD && !defined EMBEDDED_LIBRARY
			service_manager_extend_timeout(
				INNODB_EXTEND_TIMEOUT_INTERVAL,
				"InnoDB: to purge " ULINTPF " transactions",
				history_size);
#endif
			ib::info() << "to purge " << history_size
				   << " transactions";
		}
		/* The previous round still did some work. */
		return(false);
	}
	/* Exit if there are no active transactions to roll back. */
	return(trx_sys_any_active_transactions() == 0);
}

/*********************************************************************//**
Fetch and execute a task from the work queue.
@param [in,out]	slot	purge worker thread slot
@return true if a task was executed */
static
bool
srv_task_execute(ut_d(srv_slot_t *slot))
/*==================*/
{
	que_thr_t*	thr = NULL;

	ut_ad(!srv_read_only_mode);
	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

	mutex_enter(&srv_sys.tasks_mutex);

	if (UT_LIST_GET_LEN(srv_sys.tasks) > 0) {

		thr = UT_LIST_GET_FIRST(srv_sys.tasks);

		ut_a(que_node_get_type(thr->child) == QUE_NODE_PURGE);

		UT_LIST_REMOVE(srv_sys.tasks, thr);
	}

	mutex_exit(&srv_sys.tasks_mutex);

	if (thr != NULL) {
		ut_d(thr->thread_slot = slot);

		que_run_threads(thr);

		my_atomic_addlint(
			&purge_sys->n_completed, 1);
	}

	return(thr != NULL);
}

/*********************************************************************//**
Worker thread that reads tasks from the work queue and executes them.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(srv_worker_thread)(
/*==============================*/
	void*	arg MY_ATTRIBUTE((unused)))	/*!< in: a dummy parameter
						required by os_thread_create */
{
	my_thread_init();

	srv_slot_t*	slot;

	ut_ad(!srv_read_only_mode);
	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);
	my_thread_init();
	THD*		thd = innobase_create_background_thd("InnoDB purge worker");

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Worker thread starting, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

	slot = srv_reserve_slot(SRV_WORKER);

#ifdef UNIV_DEBUG
	UT_LIST_INIT(slot->debug_sync,
		     &srv_slot_t::debug_sync_t::debug_sync_list);
	rw_lock_create(PFS_NOT_INSTRUMENTED, &slot->debug_sync_lock,
		       SYNC_NO_ORDER_CHECK);
#endif

	ut_a(srv_n_purge_threads > 1);
	ut_a(ulong(my_atomic_loadlint(&srv_sys.n_threads_active[SRV_WORKER]))
	     < srv_n_purge_threads);

	/* We need to ensure that the worker threads exit after the
	purge coordinator thread. Otherwise the purge coordinator can
	end up waiting forever in trx_purge_wait_for_workers_to_complete() */

	do {
		srv_suspend_thread(slot);
		srv_resume_thread(slot);

		if (srv_task_execute(ut_d(slot))) {

			/* If there are tasks in the queue, wakeup
			the purge coordinator thread. */

			srv_wake_purge_thread_if_not_active();
		}

		/* Note: we are checking the state without holding the
		purge_sys->latch here. */
	} while (purge_sys->state != PURGE_STATE_EXIT);

	ut_d(rw_lock_free(&slot->debug_sync_lock));

	srv_free_slot(slot);

	rw_lock_x_lock(&purge_sys->latch);

	ut_a(!purge_sys->running);
	ut_a(purge_sys->state == PURGE_STATE_EXIT);

	rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Purge worker thread exiting, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

	innobase_destroy_background_thd(thd);
	my_thread_end();
	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/** Do the actual purge operation.
@param[in,out]	n_total_purged	total number of purged pages
@param[in,out]	slot		purge coordinator thread slot
@return length of history list before the last purge batch. */
static
ulint
srv_do_purge(ulint* n_total_purged
#ifdef UNIV_DEBUG
	, srv_slot_t *slot
#endif
)
{
	ulint		n_pages_purged;

	static ulint	count = 0;
	static ulint	n_use_threads = 0;
	static ulint	rseg_history_len = 0;
	ulint		old_activity_count = srv_get_activity_count();
	const ulint	n_threads = srv_n_purge_threads;

	ut_a(n_threads > 0);
	ut_ad(!srv_read_only_mode);

	/* Purge until there are no more records to purge and there is
	no change in configuration or server state. If the user has
	configured more than one purge thread then we treat that as a
	pool of threads and only use the extra threads if purge can't
	keep up with updates. */

	if (n_use_threads == 0) {
		n_use_threads = n_threads;
	}

	do {
		if (trx_sys->rseg_history_len > rseg_history_len
		    || (srv_max_purge_lag > 0
			&& rseg_history_len > srv_max_purge_lag)) {

			/* History length is now longer than what it was
			when we took the last snapshot. Use more threads. */

			if (n_use_threads < n_threads) {
				++n_use_threads;
			}

		} else if (srv_check_activity(old_activity_count)
			   && n_use_threads > 1) {

			/* History length same or smaller since last snapshot,
			use fewer threads. */

			--n_use_threads;

			old_activity_count = srv_get_activity_count();
		}

		/* Ensure that the purge threads are less than what
		was configured. */

		ut_a(n_use_threads > 0);
		ut_a(n_use_threads <= n_threads);

		/* Take a snapshot of the history list before purge. */
		if ((rseg_history_len = trx_sys->rseg_history_len) == 0) {
			break;
		}

		ulint	undo_trunc_freq =
			purge_sys->undo_trunc.get_rseg_truncate_frequency();

		ulint	rseg_truncate_frequency = ut_min(
			static_cast<ulint>(srv_purge_rseg_truncate_frequency),
			undo_trunc_freq);

		n_pages_purged = trx_purge(
			n_use_threads, srv_purge_batch_size,
			(++count % rseg_truncate_frequency) == 0
#ifdef UNIV_DEBUG
			, slot
#endif
		);

		*n_total_purged += n_pages_purged;
	} while (!srv_purge_should_exit(n_pages_purged)
		 && n_pages_purged > 0
		 && purge_sys->state == PURGE_STATE_RUN);

	return(rseg_history_len);
}

/*********************************************************************//**
Suspend the purge coordinator thread. */
static
void
srv_purge_coordinator_suspend(
/*==========================*/
	srv_slot_t*	slot,			/*!< in/out: Purge coordinator
						thread slot */
	ulint		rseg_history_len)	/*!< in: history list length
						before last purge */
{
	ut_ad(!srv_read_only_mode);
	ut_a(slot->type == SRV_PURGE);

	bool		stop = false;

	/** Maximum wait time on the purge event, in micro-seconds. */
	static const ulint SRV_PURGE_MAX_TIMEOUT = 10000;

	int64_t		sig_count = srv_suspend_thread(slot);

	do {
		rw_lock_x_lock(&purge_sys->latch);

		purge_sys->running = false;

		rw_lock_x_unlock(&purge_sys->latch);

		/* We don't wait right away on the the non-timed wait because
		we want to signal the thread that wants to suspend purge. */
		const bool wait = stop
			|| rseg_history_len <= trx_sys->rseg_history_len;
		const bool timeout = srv_resume_thread(
			slot, sig_count, wait,
			stop ? 0 : SRV_PURGE_MAX_TIMEOUT);

		sig_count = srv_suspend_thread(slot);

		rw_lock_x_lock(&purge_sys->latch);

		stop = (srv_shutdown_state <= SRV_SHUTDOWN_INITIATED
			&& purge_sys->state == PURGE_STATE_STOP);

		if (!stop) {
			ut_a(purge_sys->n_stop == 0);
			purge_sys->running = true;

			if (timeout
			    && rseg_history_len == trx_sys->rseg_history_len
			    && trx_sys->rseg_history_len < 5000) {
				/* No new records were added since the
				wait started. Simply wait for new
				records. The magic number 5000 is an
				approximation for the case where we
				have cached UNDO log records which
				prevent truncate of the UNDO
				segments. */
				stop = true;
			}
		} else {
			ut_a(purge_sys->n_stop > 0);

			/* Signal that we are suspended. */
			os_event_set(purge_sys->event);
		}

		rw_lock_x_unlock(&purge_sys->latch);
	} while (stop && srv_undo_sources);

	srv_resume_thread(slot, 0, false);
}

/*********************************************************************//**
Purge coordinator thread that schedules the purge tasks.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(srv_purge_coordinator_thread)(
/*=========================================*/
	void*	arg MY_ATTRIBUTE((unused)))	/*!< in: a dummy parameter
						required by os_thread_create */
{
	my_thread_init();
	THD*		thd = innobase_create_background_thd("InnoDB purge coordinator");
	srv_slot_t*	slot;
	ulint           n_total_purged = ULINT_UNDEFINED;

	ut_ad(!srv_read_only_mode);
	ut_a(srv_n_purge_threads >= 1);
	ut_a(trx_purge_state() == PURGE_STATE_INIT);
	ut_a(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

	rw_lock_x_lock(&purge_sys->latch);

	purge_sys->running = true;
	purge_sys->state = PURGE_STATE_RUN;

	rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(srv_purge_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Purge coordinator thread created, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

	slot = srv_reserve_slot(SRV_PURGE);

#ifdef UNIV_DEBUG
	UT_LIST_INIT(slot->debug_sync,
		     &srv_slot_t::debug_sync_t::debug_sync_list);
	rw_lock_create(PFS_NOT_INSTRUMENTED, &slot->debug_sync_lock,
		       SYNC_NO_ORDER_CHECK);
#endif
	ulint	rseg_history_len = trx_sys->rseg_history_len;

	do {
		/* If there are no records to purge or the last
		purge didn't purge any records then wait for activity. */

		if (srv_shutdown_state <= SRV_SHUTDOWN_INITIATED
		    && srv_undo_sources
		    && (purge_sys->state == PURGE_STATE_STOP
			|| n_total_purged == 0)) {

			srv_purge_coordinator_suspend(slot, rseg_history_len);
		}

		ut_ad(!slot->suspended);

		if (srv_purge_should_exit(n_total_purged)) {
			break;
		}

		n_total_purged = 0;

		rseg_history_len = srv_do_purge(&n_total_purged
#ifdef UNIV_DEBUG
						, slot
#endif
		);
	} while (!srv_purge_should_exit(n_total_purged));

	/* The task queue should always be empty, independent of fast
	shutdown state. */
	ut_a(srv_get_task_queue_length() == 0);

	ut_d(rw_lock_free(&slot->debug_sync_lock));

	srv_free_slot(slot);

	/* Note that we are shutting down. */
	rw_lock_x_lock(&purge_sys->latch);

	purge_sys->state = PURGE_STATE_EXIT;

	/* If there are any pending undo-tablespace truncate then clear
	it off as we plan to shutdown the purge thread. */
	purge_sys->undo_trunc.clear();

	purge_sys->running = false;

	/* Ensure that the wait in trx_purge_stop() will terminate. */
	os_event_set(purge_sys->event);

	rw_lock_x_unlock(&purge_sys->latch);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Purge coordinator exiting, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

	/* Ensure that all the worker threads quit. */
	if (ulint n_workers = srv_n_purge_threads - 1) {
		const srv_slot_t* slot;
		const srv_slot_t* const end = &srv_sys.sys_threads[
			srv_sys.n_sys_threads];

		do {
			srv_release_threads(SRV_WORKER, n_workers);
			srv_sys_mutex_enter();
			for (slot = &srv_sys.sys_threads[2];
			     !slot++->in_use && slot < end; );
			srv_sys_mutex_exit();
		} while (slot < end);
	}

	innobase_destroy_background_thd(thd);
	my_thread_end();
	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;	/* Not reached, avoid compiler warning */
}

/**********************************************************************//**
Enqueues a task to server task queue and releases a worker thread, if there
is a suspended one. */
void
srv_que_task_enqueue_low(
/*=====================*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	ut_ad(!srv_read_only_mode);
	mutex_enter(&srv_sys.tasks_mutex);

	UT_LIST_ADD_LAST(srv_sys.tasks, thr);

	mutex_exit(&srv_sys.tasks_mutex);

	srv_release_threads(SRV_WORKER, 1);
}

/**********************************************************************//**
Get count of tasks in the queue.
@return number of tasks in queue */
ulint
srv_get_task_queue_length(void)
/*===========================*/
{
	ulint	n_tasks;

	ut_ad(!srv_read_only_mode);

	mutex_enter(&srv_sys.tasks_mutex);

	n_tasks = UT_LIST_GET_LEN(srv_sys.tasks);

	mutex_exit(&srv_sys.tasks_mutex);

	return(n_tasks);
}

/** Wake up the purge threads. */
void
srv_purge_wakeup()
{
	ut_ad(!srv_read_only_mode);
	ut_ad(!sync_check_iterate(sync_check()));

	if (srv_force_recovery >= SRV_FORCE_NO_BACKGROUND) {
		return;
	}

	do {
		srv_release_threads(SRV_PURGE, 1);

		if (srv_n_purge_threads > 1) {
			ulint	n_workers = srv_n_purge_threads - 1;

			srv_release_threads(SRV_WORKER, n_workers);
		}
	} while (!my_atomic_loadptr_explicit(reinterpret_cast<void**>
					     (&srv_running),
					     MY_MEMORY_ORDER_RELAXED)
		 && (srv_sys.n_threads_active[SRV_WORKER]
		     || srv_sys.n_threads_active[SRV_PURGE]));
}

/** Shut down the purge threads. */
void srv_purge_shutdown()
{
	do {
		ut_ad(!srv_undo_sources);
		srv_purge_wakeup();
	} while (srv_sys.sys_threads[SRV_PURGE_SLOT].in_use);
}

/** Check if tablespace is being truncated.
(Ignore system-tablespace as we don't re-create the tablespace
and so some of the action that are suppressed by this function
for independent tablespace are not applicable to system-tablespace).
@param	space_id	space_id to check for truncate action
@return true		if being truncated, false if not being
			truncated or tablespace is system-tablespace. */
bool
srv_is_tablespace_truncated(ulint space_id)
{
	if (is_system_tablespace(space_id)) {
		return(false);
	}

	return(truncate_t::is_tablespace_truncated(space_id)
	       || undo::Truncate::is_tablespace_truncated(space_id));

}

/** Check if tablespace was truncated.
@param[in]	space	space object to check for truncate action
@return true if tablespace was truncated and we still have an active
MLOG_TRUNCATE REDO log record. */
bool
srv_was_tablespace_truncated(const fil_space_t* space)
{
	if (space == NULL) {
		ut_ad(0);
		return(false);
	}

	return (!is_system_tablespace(space->id)
		&& truncate_t::was_tablespace_truncated(space->id));
}

#ifdef UNIV_DEBUG
static ulint get_first_slot(srv_thread_type type)
{
	switch (type) {
	case SRV_MASTER:
		return SRV_MASTER_SLOT;
	case SRV_PURGE:
		return SRV_PURGE_SLOT;
	case SRV_WORKER:
		/* Find an empty slot, skip the master and purge slots. */
		return SRV_WORKER_SLOTS_START;
	default:
		ut_error;
	}
}

void srv_for_each_thread(srv_thread_type type,
			 srv_slot_callback_t callback,
			 const void *arg)
{
	for (ulint slot_idx= get_first_slot(type);
	     slot_idx < srv_sys.n_sys_threads
		     && srv_sys.sys_threads[slot_idx].in_use
		     && srv_sys.sys_threads[slot_idx].type == type;
	     slot_idx++) {
		callback(&srv_sys.sys_threads[slot_idx], arg);
	}
}
#endif
