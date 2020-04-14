/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, 2009 Google Inc.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2013, 2020, MariaDB Corporation.

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
#include "mysql/psi/mysql_stage.h"
#include "mysql/psi/psi.h"

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


#include <my_service_manager.h>

/* The following is the maximum allowed duration of a lock wait. */
UNIV_INTERN ulong	srv_fatal_semaphore_wait_threshold =  DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT;

/* How much data manipulation language (DML) statements need to be delayed,
in microseconds, in order to reduce the lagging of the purge thread. */
ulint	srv_dml_needed_delay;

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

/** Set if InnoDB must operate in read-only mode. We don't do any
recovery and open all tables in RO mode instead of RW mode. We don't
sync the max trx id to disk either. */
my_bool	srv_read_only_mode;
/** store to its own file each table created by an user; data
dictionary tables are in the system tablespace 0 */
my_bool	srv_file_per_table;
/** Set if InnoDB operates in read-only mode or innodb-force-recovery
is greater than SRV_FORCE_NO_TRX_UNDO. */
my_bool	high_level_read_only;

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
/** copy of innodb_use_atomic_writes; @see innodb_init_params() */
my_bool	srv_use_atomic_writes;
/** innodb_compression_algorithm; used with page compression */
ulong	innodb_compression_algorithm;

#ifdef UNIV_DEBUG
/** Used by SET GLOBAL innodb_master_thread_disabled_debug = X. */
my_bool	srv_master_thread_disabled_debug;
/** Event used to inform that master thread is disabled. */
static os_event_t	srv_master_thread_disabled_event;
#endif /* UNIV_DEBUG */

/*------------------------- LOG FILES ------------------------ */
char*	srv_log_group_home_dir;

/** The InnoDB redo log file size, or 0 when changing the redo log format
at startup (while disallowing writes to the redo log). */
ulonglong	srv_log_file_size;
/** innodb_log_buffer_size, in bytes */
ulong		srv_log_buffer_size;
/** innodb_flush_log_at_trx_commit */
ulong		srv_flush_log_at_trx_commit;
/** innodb_flush_log_at_timeout */
uint		srv_flush_log_at_timeout;
/** innodb_page_size */
ulong		srv_page_size;
/** log2 of innodb_page_size; @see innodb_init_params() */
ulong		srv_page_size_shift;
/** innodb_log_write_ahead_size */
ulong		srv_log_write_ahead_size;

/** innodb_adaptive_flushing; try to flush dirty pages so as to avoid
IO bursts at the checkpoints. */
my_bool	srv_adaptive_flushing;

/** innodb_flush_sync; whether to ignore io_capacity at log checkpoints */
my_bool	srv_flush_sync;

/** common thread pool*/
tpool::thread_pool* srv_thread_pool;

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
const ulint	srv_buf_pool_min_size	= 5 * 1024 * 1024;
/** Default pool size in bytes */
const ulint	srv_buf_pool_def_size	= 128 * 1024 * 1024;
/** Requested buffer pool chunk size */
ulong	srv_buf_pool_chunk_unit;
/** innodb_page_hash_locks (a debug-only parameter);
number of locks to protect buf_pool.page_hash */
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
/** Abort load after this amount of pages */
#ifdef UNIV_DEBUG
ulong srv_buf_pool_load_pages_abort = LONG_MAX;
#endif
/** Lock table size in bytes */
ulint	srv_lock_table_size	= ULINT_MAX;

/** innodb_idle_flush_pct */
ulong	srv_idle_flush_pct;

/** innodb_read_io_threads */
ulong	srv_n_read_io_threads;
/** innodb_write_io_threads */
ulong	srv_n_write_io_threads;

/** innodb_random_read_ahead */
my_bool	srv_random_read_ahead;
/** innodb_read_ahead_threshold; the number of pages that must be present
in the buffer cache and accessed sequentially for InnoDB to trigger a
readahead request. */
ulong	srv_read_ahead_threshold;

/** innodb_change_buffer_max_size; maximum on-disk size of change
buffer in terms of percentage of the buffer pool. */
uint	srv_change_buffer_max_size;

ulong	srv_file_flush_method;


/** copy of innodb_open_files; @see innodb_init_params() */
ulint	srv_max_n_open_files;

/** innodb_io_capacity */
ulong	srv_io_capacity;
/** innodb_io_capacity_max */
ulong	srv_max_io_capacity;

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

/** innodb_purge_threads; the number of purge tasks to use */
uint srv_n_purge_threads;

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

/** innodb_fast_shutdown=1 skips purge and change buffer merge.
innodb_fast_shutdown=2 effectively crashes the server (no log checkpoint).
innodb_fast_shutdown=3 is a clean shutdown that skips the rollback
of active transaction (to be done on restart). */
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

my_bool	srv_use_doublewrite_buf;

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

my_bool srv_immediate_scrub_data_uncompressed;

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

/* The following counts are used by the srv_master_callback. */

/** Iterations of the loop bounded by 'srv_active' label. */
ulint		srv_main_active_loops;
/** Iterations of the loop bounded by the 'srv_idle' label. */
ulint		srv_main_idle_loops;
/** Iterations of the loop bounded by the 'srv_shutdown' label. */
static ulint		srv_main_shutdown_loops;
/** Log writes involving flush. */
ulint		srv_log_writes_and_flush;

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

/** Simulate compression failures. */
UNIV_INTERN uint srv_simulate_comp_failures;

/** Buffer pool dump status frequence in percentages */
UNIV_INTERN ulong srv_buf_dump_status_frequency;

/** Acquire the system_mutex. */
#define srv_sys_mutex_enter() do {			\
	mutex_enter(&srv_sys.mutex);			\
} while (0)

/** Release the system mutex. */
#define srv_sys_mutex_exit() do {			\
	mutex_exit(&srv_sys.mutex);			\
} while (0)

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
	srv_stats_t::ulint_ctr_1_t
			activity_count;		/*!< For tracking server
						activity */
};

static srv_sys_t	srv_sys;

/*
  Structure shared by timer and coordinator_callback.
  No protection necessary since timer and task never run
  in parallel (being in the same task group of size 1).
*/
struct purge_coordinator_state
{
  /** Snapshot of the last history length before the purge call.*/
  uint32 m_history_length;
  Atomic_counter<int> m_running;
  purge_coordinator_state() : m_history_length(), m_running(0) {}
};

static purge_coordinator_state purge_state;

/** threadpool timer for srv_error_monitor_task(). */
std::unique_ptr<tpool::timer> srv_error_monitor_timer;
std::unique_ptr<tpool::timer> srv_monitor_timer;


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



static void thread_pool_thread_init()
{
	my_thread_init();
	pfs_register_thread(thread_pool_thread_key);
}
static void thread_pool_thread_end()
{
	pfs_delete_thread();
	my_thread_end();
}


#ifndef DBUG_OFF
static void dbug_after_task_callback()
{
  ut_ad(!sync_check_iterate(sync_check()));
}
#endif

void srv_thread_pool_init()
{
  DBUG_ASSERT(!srv_thread_pool);

#if defined (_WIN32)
  srv_thread_pool= tpool::create_thread_pool_win();
#else
  srv_thread_pool= tpool::create_thread_pool_generic();
#endif
  srv_thread_pool->set_thread_callbacks(thread_pool_thread_init,
                                        thread_pool_thread_end);
#ifndef DBUG_OFF
  tpool::set_after_task_callback(dbug_after_task_callback);
#endif
}


void srv_thread_pool_end()
{
  ut_ad(!srv_master_timer);
  delete srv_thread_pool;
  srv_thread_pool= nullptr;
}

static bool need_srv_free;

/** Initialize the server. */
static void srv_init()
{
	mutex_create(LATCH_ID_SRV_INNODB_MONITOR, &srv_innodb_monitor_mutex);
	srv_thread_pool_init();

	if (!srv_read_only_mode) {
		mutex_create(LATCH_ID_SRV_SYS, &srv_sys.mutex);

		mutex_create(LATCH_ID_SRV_SYS_TASKS, &srv_sys.tasks_mutex);


		buf_flush_event = os_event_create("buf_flush_event");

		UT_LIST_INIT(srv_sys.tasks, &que_thr_t::queue);
	}

	need_srv_free = true;
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
}

/*********************************************************************//**
Frees the data structures created in srv_init(). */
void
srv_free(void)
/*==========*/
{
	if (!need_srv_free) {
		return;
	}

	mutex_free(&srv_innodb_monitor_mutex);
	mutex_free(&page_zip_stat_per_index_mutex);

	if (!srv_read_only_mode) {
		mutex_free(&srv_sys.mutex);
		mutex_free(&srv_sys.tasks_mutex);
		os_event_destroy(buf_flush_event);
	}

	ut_d(os_event_destroy(srv_master_thread_disabled_event));

	trx_i_s_cache_free(trx_i_s_cache);
	srv_thread_pool_end();
}

/*********************************************************************//**
Boots the InnoDB server. */
void
srv_boot(void)
/*==========*/
{
	sync_check_init();
	trx_pool_init();
	row_mysql_init();
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

	buf_refresh_io_stats();

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
	for (ulint i = 0; i < btr_ahi_parts; ++i) {
		const hash_table_t* table = btr_search_sys->hash_tables[i];

		ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);
		/* this is only used for buf_pool.page_hash */
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

	fprintf(file,
		"%.2f hash searches/s, %.2f non-hash searches/s\n",
		static_cast<double>(btr_cur_n_sea - btr_cur_n_sea_old)
		/ time_elapsed,
		static_cast<double>(btr_cur_n_non_sea - btr_cur_n_non_sea_old)
		/ time_elapsed);
	btr_cur_n_sea_old = btr_cur_n_sea;
#else /* BTR_CUR_HASH_ADAPT */
	fprintf(file,
		"%.2f non-hash searches/s\n",
		static_cast<double>(btr_cur_n_non_sea - btr_cur_n_non_sea_old)
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
		ulint{os_total_large_mem_allocated},
		dict_sys.rough_size());

	buf_print_io(file);

	fputs("--------------\n"
	      "ROW OPERATIONS\n"
	      "--------------\n", file);
	fprintf(file,
		ULINTPF " queries inside InnoDB, "
		ULINTPF " queries in queue\n",
		srv_conc_get_active_threads(),
		srv_conc_get_waiting_threads());

	fprintf(file, ULINTPF " read views open inside InnoDB\n",
		trx_sys.view_count());

	if (ulint n_reserved = fil_system.sys_space->n_reserved_extents) {
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
		static_cast<double>(srv_stats.n_rows_inserted
				    - srv_n_rows_inserted_old)
		/ time_elapsed,
		static_cast<double>(srv_stats.n_rows_updated
				    - srv_n_rows_updated_old)
		/ time_elapsed,
		static_cast<double>(srv_stats.n_rows_deleted
				    - srv_n_rows_deleted_old)
		/ time_elapsed,
		static_cast<double>(srv_stats.n_rows_read
				    - srv_n_rows_read_old)
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
		static_cast<double>(srv_stats.n_system_rows_inserted
				    - srv_n_system_rows_inserted_old)
		/ time_elapsed,
		static_cast<double>(srv_stats.n_system_rows_updated
				    - srv_n_system_rows_updated_old)
		/ time_elapsed,
		static_cast<double>(srv_stats.n_system_rows_deleted
				    - srv_n_system_rows_deleted_old)
		/ time_elapsed,
		static_cast<double>(srv_stats.n_system_rows_read
				    - srv_n_system_rows_read_old)
		/ time_elapsed);
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
	fil_crypt_stat_t	crypt_stat;

	if (!srv_read_only_mode) {
		fil_crypt_total_stat(&crypt_stat);
	}

#ifdef BTR_CUR_HASH_ADAPT
	ulint mem_adaptive_hash = 0;
	ut_ad(btr_search_sys->hash_tables);
	for (ulong i = 0; i < btr_ahi_parts; i++) {
		rw_lock_s_lock(btr_search_latches[i]);
		hash_table_t*	ht = btr_search_sys->hash_tables[i];

		ut_ad(ht);
		ut_ad(ht->heap);
		/* Multiple mutexes/heaps are currently never used for adaptive
		hash index tables. */
		ut_ad(!ht->n_sync_obj);
		ut_ad(!ht->heaps);

		mem_adaptive_hash += mem_heap_get_size(ht->heap)
			+ ht->n_cells * sizeof(hash_cell_t);
		rw_lock_s_unlock(btr_search_latches[i]);
	}
	export_vars.innodb_mem_adaptive_hash = mem_adaptive_hash;
#endif

	export_vars.innodb_mem_dictionary = dict_sys.rough_size();

	mutex_enter(&srv_innodb_monitor_mutex);

	export_vars.innodb_data_pending_reads =
		ulint(MONITOR_VALUE(MONITOR_OS_PENDING_READS));

	export_vars.innodb_data_pending_writes =
		ulint(MONITOR_VALUE(MONITOR_OS_PENDING_WRITES));

	export_vars.innodb_data_pending_fsyncs =
		log_sys.get_pending_flushes()
		+ fil_n_pending_tablespace_flushes;

	export_vars.innodb_data_fsyncs = os_n_fsyncs;

	export_vars.innodb_data_read = srv_stats.data_read;

	export_vars.innodb_data_reads = os_n_file_reads;

	export_vars.innodb_data_writes = os_n_file_writes;

	export_vars.innodb_data_written = srv_stats.data_written;

	export_vars.innodb_buffer_pool_read_requests
		= buf_pool.stat.n_page_gets;

	export_vars.innodb_buffer_pool_write_requests =
		srv_stats.buf_pool_write_requests;

	export_vars.innodb_buffer_pool_wait_free =
		srv_stats.buf_pool_wait_free;

	export_vars.innodb_buffer_pool_pages_flushed =
		srv_stats.buf_pool_flushed;

	export_vars.innodb_buffer_pool_reads = srv_stats.buf_pool_reads;

	export_vars.innodb_buffer_pool_read_ahead_rnd =
		buf_pool.stat.n_ra_pages_read_rnd;

	export_vars.innodb_buffer_pool_read_ahead =
		buf_pool.stat.n_ra_pages_read;

	export_vars.innodb_buffer_pool_read_ahead_evicted =
		buf_pool.stat.n_ra_pages_evicted;

	export_vars.innodb_buffer_pool_pages_data =
		UT_LIST_GET_LEN(buf_pool.LRU);

	export_vars.innodb_buffer_pool_bytes_data =
		buf_pool.stat.LRU_bytes
		+ (UT_LIST_GET_LEN(buf_pool.unzip_LRU)
		   << srv_page_size_shift);

	export_vars.innodb_buffer_pool_pages_dirty =
		UT_LIST_GET_LEN(buf_pool.flush_list);

	export_vars.innodb_buffer_pool_pages_made_young
		= buf_pool.stat.n_pages_made_young;
	export_vars.innodb_buffer_pool_pages_made_not_young
		= buf_pool.stat.n_pages_not_made_young;

	export_vars.innodb_buffer_pool_pages_old = buf_pool.LRU_old_len;

	export_vars.innodb_buffer_pool_bytes_dirty =
		buf_pool.stat.flush_list_bytes;

	export_vars.innodb_buffer_pool_pages_free =
		UT_LIST_GET_LEN(buf_pool.free);

#ifdef UNIV_DEBUG
	export_vars.innodb_buffer_pool_pages_latched =
		buf_get_latched_pages_number();
#endif /* UNIV_DEBUG */
	export_vars.innodb_buffer_pool_pages_total = buf_pool.get_n_pages();

	export_vars.innodb_buffer_pool_pages_misc =
		buf_pool.get_n_pages()
		- UT_LIST_GET_LEN(buf_pool.LRU)
		- UT_LIST_GET_LEN(buf_pool.free);

	export_vars.innodb_max_trx_id = trx_sys.get_max_trx_id();
	export_vars.innodb_history_list_length = trx_sys.rseg_history_len;

	export_vars.innodb_log_waits = srv_stats.log_waits;

	export_vars.innodb_os_log_written = srv_stats.os_log_written;

	export_vars.innodb_os_log_fsyncs = log_sys.get_flushes();

	export_vars.innodb_os_log_pending_fsyncs
		= log_sys.get_pending_flushes();

	export_vars.innodb_os_log_pending_writes =
		srv_stats.os_log_pending_writes;

	export_vars.innodb_log_write_requests = srv_stats.log_write_requests;

	export_vars.innodb_log_writes = srv_stats.log_writes;

	export_vars.innodb_dblwr_pages_written =
		srv_stats.dblwr_pages_written;

	export_vars.innodb_dblwr_writes = srv_stats.dblwr_writes;

	export_vars.innodb_pages_created = buf_pool.stat.n_pages_created;

	export_vars.innodb_pages_read = buf_pool.stat.n_pages_read;

	export_vars.innodb_pages_written = buf_pool.stat.n_pages_written;

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
		lock_sys.n_lock_max_wait_time / 1000;

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

	export_vars.innodb_num_open_files = fil_system.n_open;

	export_vars.innodb_truncated_status_writes =
		srv_truncated_status_writes;

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
	}

	mutex_exit(&srv_innodb_monitor_mutex);

	log_mutex_enter();
	export_vars.innodb_lsn_current = log_sys.get_lsn();
	export_vars.innodb_lsn_flushed = log_sys.get_flushed_lsn();
	export_vars.innodb_lsn_last_checkpoint = log_sys.last_checkpoint_lsn;
	export_vars.innodb_checkpoint_max_age = static_cast<ulint>(
		log_sys.max_checkpoint_age);
	log_mutex_exit();

	export_vars.innodb_checkpoint_age = static_cast<ulint>(
		export_vars.innodb_lsn_current
		- export_vars.innodb_lsn_last_checkpoint);
}

struct srv_monitor_state_t
{
  time_t last_monitor_time;
  ulint mutex_skipped;
  bool last_srv_print_monitor;
  srv_monitor_state_t() : mutex_skipped(0), last_srv_print_monitor(false)
  {
    srv_last_monitor_time = time(NULL);
    last_monitor_time= srv_last_monitor_time;
  }
};

static srv_monitor_state_t monitor_state;

/** A task which prints the info output by various InnoDB monitors.*/
void srv_monitor_task(void*)
{
	double		time_elapsed;
	time_t		current_time;

	ut_ad(!srv_read_only_mode);

	current_time = time(NULL);

	time_elapsed = difftime(current_time, monitor_state.last_monitor_time);

	if (time_elapsed > 15) {
		monitor_state.last_monitor_time = current_time;

		if (srv_print_innodb_monitor) {
			/* Reset mutex_skipped counter everytime
			srv_print_innodb_monitor changes. This is to
			ensure we will not be blocked by lock_sys.mutex
			for short duration information printing,
			such as requested by sync_array_print_long_waits() */
			if (!monitor_state.last_srv_print_monitor) {
				monitor_state.mutex_skipped = 0;
				monitor_state.last_srv_print_monitor = true;
			}

			if (!srv_printf_innodb_monitor(stderr,
						MUTEX_NOWAIT(monitor_state.mutex_skipped),
						NULL, NULL)) {
				monitor_state.mutex_skipped++;
			} else {
				/* Reset the counter */
				monitor_state.mutex_skipped = 0;
			}
		} else {
			monitor_state.last_monitor_time = 0;
		}


		/* We don't create the temp files or associated
		mutexes in read-only-mode */

		if (!srv_read_only_mode && srv_innodb_status) {
			mutex_enter(&srv_monitor_file_mutex);
			rewind(srv_monitor_file);
			if (!srv_printf_innodb_monitor(srv_monitor_file,
						MUTEX_NOWAIT(monitor_state.mutex_skipped),
						NULL, NULL)) {
				monitor_state.mutex_skipped++;
			} else {
				monitor_state.mutex_skipped = 0;
			}

			os_file_set_eof(srv_monitor_file);
			mutex_exit(&srv_monitor_file_mutex);
		}
	}

	srv_refresh_innodb_monitor_stats();
}

/*********************************************************************//**
A task which prints warnings about semaphore waits which have lasted
too long. These can be used to track bugs which cause hangs.
*/
void srv_error_monitor_task(void*)
{
	/* number of successive fatal timeouts observed */
	static ulint		fatal_cnt;
	static lsn_t		old_lsn = recv_sys.recovered_lsn;
	/* longest waiting thread for a semaphore */
	os_thread_id_t	waiter;
	static os_thread_id_t	old_waiter = os_thread_get_curr_id();
	/* the semaphore that is being waited for */
	const void*	sema		= NULL;
	static const void*	old_sema	= NULL;

	ut_ad(!srv_read_only_mode);

	/* Try to track a strange bug reported by Harald Fuchs and others,
	where the lsn seems to decrease at times */

	lsn_t new_lsn = log_sys.get_lsn();
	if (new_lsn < old_lsn) {
		ib::error() << "Old log sequence number " << old_lsn << " was"
			<< " greater than the new log sequence number "
			<< new_lsn << ". Please submit a bug report to"
			" https://jira.mariadb.org/";
			ut_ad(0);
	}

	old_lsn = new_lsn;

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
}

/******************************************************************//**
Increment the server activity count. */
void
srv_inc_activity_count(void)
/*========================*/
{
	srv_sys.activity_count.inc();
}

#ifdef UNIV_DEBUG
/** @return whether purge or master task is active */
bool srv_any_background_activity()
{
  if (purge_sys.enabled() || srv_master_timer.get())
  {
    ut_ad(!srv_read_only_mode);
    return true;
  }
  return false;
}
#endif /* UNIV_DEBUG */

/** Wake up the InnoDB master thread if it was suspended (not sleeping). */
void
srv_active_wake_master_thread_low()
{
	ut_ad(!srv_read_only_mode);
	ut_ad(!mutex_own(&srv_sys.mutex));

	srv_inc_activity_count();
}


static void purge_worker_callback(void*);
static void purge_coordinator_callback(void*);
static void purge_coordinator_timer_callback(void*);

static tpool::task_group purge_task_group;
tpool::waitable_task purge_worker_task(purge_worker_callback, nullptr,
                                       &purge_task_group);
static tpool::task_group purge_coordinator_task_group(1);
static tpool::waitable_task purge_coordinator_task
  (purge_coordinator_callback, nullptr, &purge_coordinator_task_group);

static tpool::timer *purge_coordinator_timer;

/** Wake up the purge threads if there is work to do. */
void
srv_wake_purge_thread_if_not_active()
{
	ut_ad(!srv_read_only_mode);
	ut_ad(!mutex_own(&srv_sys.mutex));

	if (purge_sys.enabled() && !purge_sys.paused()
	    && trx_sys.rseg_history_len) {
		if(++purge_state.m_running == 1) {
			srv_thread_pool->submit_task(&purge_coordinator_task);
		}
	}
}

/** @return whether the purge tasks are active */
bool purge_sys_t::running() const
{
  return purge_coordinator_task.is_running();
}

/** Stop purge during FLUSH TABLES FOR EXPORT */
void purge_sys_t::stop()
{
  rw_lock_x_lock(&latch);

  if (!enabled())
  {
    /* Shutdown must have been initiated during FLUSH TABLES FOR EXPORT. */
    ut_ad(!srv_undo_sources);
    rw_lock_x_unlock(&latch);
    return;
  }

  ut_ad(srv_n_purge_threads > 0);

  const auto paused= m_paused++;

  rw_lock_x_unlock(&latch);

  if (!paused)
  {
    ib::info() << "Stopping purge";
    MONITOR_ATOMIC_INC(MONITOR_PURGE_STOP_COUNT);
    purge_coordinator_task.disable();
  }
}

/** Resume purge at UNLOCK TABLES after FLUSH TABLES FOR EXPORT */
void purge_sys_t::resume()
{
   if (!enabled())
   {
     /* Shutdown must have been initiated during FLUSH TABLES FOR EXPORT. */
     ut_ad(!srv_undo_sources);
     return;
   }
   ut_ad(!srv_read_only_mode);
   ut_ad(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);
   ut_ad(!sync_check_iterate(sync_check()));
   purge_coordinator_task.enable();
   rw_lock_x_lock(&latch);
   int32_t paused= m_paused--;
   ut_a(paused);

   if (paused == 1)
   {
     ib::info() << "Resuming purge";
     purge_state.m_running = 0;
     srv_wake_purge_thread_if_not_active();
     MONITOR_ATOMIC_INC(MONITOR_PURGE_RESUME_COUNT);
   }
   rw_lock_x_unlock(&latch);
}

/** Wake up the master thread if it is suspended or being suspended. */
void
srv_wake_master_thread()
{
	srv_inc_activity_count();
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
		log_sys.initiate_write(true);
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

	dict_sys_lock();

	n_tables_evicted = dict_make_room_in_cache(
		innobase_get_table_cache_size(), pct_check);

	dict_sys_unlock();

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
@param[in]	save		immediate result from check function */
void
srv_master_thread_disabled_debug_update(THD*, st_mysql_sys_var*, void*,
					const void* save)
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

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		return;
	}

	/* make sure that there is enough reusable space in the redo
	log files */
	srv_main_thread_op_info = "checking free log space";
	log_free_check();

	/* Flush logs if needed */
	srv_main_thread_op_info = "flushing log";
	srv_sync_log_buffer_in_background();
	MONITOR_INC_TIME_IN_MICRO_SECS(
		MONITOR_SRV_LOG_FLUSH_MICROSECOND, counter_time);

	/* Now see if various tasks that are performed at defined
	intervals need to be performed. */

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
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

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		return;
	}

	/* Make a new checkpoint */
	if (cur_time % SRV_MASTER_CHECKPOINT_INTERVAL == 0) {
		srv_main_thread_op_info = "making checkpoint";
		log_checkpoint();
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

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		return;
	}

	/* make sure that there is enough reusable space in the redo
	log files */
	srv_main_thread_op_info = "checking free log space";
	log_free_check();

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
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

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		return;
	}

	/* Make a new checkpoint */
	srv_main_thread_op_info = "making checkpoint";
	log_checkpoint();
	MONITOR_INC_TIME_IN_MICRO_SECS(MONITOR_SRV_CHECKPOINT_MICROSECOND,
				       counter_time);
}

/**
Complete the shutdown tasks such as background DROP TABLE,
and optionally change buffer merge (on innodb_fast_shutdown=0). */
void srv_shutdown(bool ibuf_merge)
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
			n_bytes_merged = ibuf_merge_all();

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

/** The periodic master task controlling the server. */
void srv_master_callback(void*)
{
	static ulint old_activity_count;

	ut_a(srv_shutdown_state == SRV_SHUTDOWN_NONE);

	srv_main_thread_op_info = "";
	MONITOR_INC(MONITOR_MASTER_THREAD_SLEEP);
	if (srv_check_activity(old_activity_count)) {
		old_activity_count = srv_get_activity_count();
		srv_master_do_active_tasks();
	} else {
		srv_master_do_idle_tasks();
	}
	srv_main_thread_op_info = "sleeping";
}

/** @return whether purge should exit due to shutdown */
static bool srv_purge_should_exit()
{
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_NONE
	      || srv_shutdown_state == SRV_SHUTDOWN_CLEANUP);

	if (srv_undo_sources) {
		return(false);
	}
	if (srv_fast_shutdown) {
		return(true);
	}
	/* Slow shutdown was requested. */
	uint32_t history_size = trx_sys.rseg_history_len;
	if (history_size) {
#if defined HAVE_SYSTEMD && !defined EMBEDDED_LIBRARY
		static time_t progress_time;
		time_t now = time(NULL);
		if (now - progress_time >= 15) {
			progress_time = now;
			service_manager_extend_timeout(
				INNODB_EXTEND_TIMEOUT_INTERVAL,
				"InnoDB: to purge %u transactions",
				history_size);
		}
#endif
		return false;
	}

	return !trx_sys.any_active_transactions();
}

/*********************************************************************//**
Fetch and execute a task from the work queue.
@param [in,out]	slot	purge worker thread slot
@return true if a task was executed */
static bool srv_task_execute()
{
	ut_ad(!srv_read_only_mode);
	ut_ad(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);

	mutex_enter(&srv_sys.tasks_mutex);

	if (que_thr_t* thr = UT_LIST_GET_FIRST(srv_sys.tasks)) {
		ut_a(que_node_get_type(thr->child) == QUE_NODE_PURGE);
		UT_LIST_REMOVE(srv_sys.tasks, thr);
		mutex_exit(&srv_sys.tasks_mutex);
		que_run_threads(thr);
		return true;
	}

	ut_ad(UT_LIST_GET_LEN(srv_sys.tasks) == 0);
	mutex_exit(&srv_sys.tasks_mutex);
	return false;
}


/** Do the actual purge operation.
@param[in,out]	n_total_purged	total number of purged pages
@return length of history list before the last purge batch. */
static uint32_t srv_do_purge(ulint* n_total_purged)
{
	ulint		n_pages_purged;

	static ulint	count = 0;
	static ulint	n_use_threads = 0;
	static uint32_t	rseg_history_len = 0;
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
		if (trx_sys.rseg_history_len > rseg_history_len
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
		if (!(rseg_history_len = trx_sys.rseg_history_len)) {
			break;
		}

		n_pages_purged = trx_purge(
			n_use_threads,
			!(++count % srv_purge_rseg_truncate_frequency)
			|| purge_sys.truncate.current);

		*n_total_purged += n_pages_purged;
	} while (n_pages_purged > 0 && !purge_sys.paused()
		 && !srv_purge_should_exit());

	return(rseg_history_len);
}


static std::queue<THD*> purge_thds;
static std::mutex purge_thd_mutex;

static void purge_create_background_thds(int n)
{
  THD *thd= current_thd;
  std::unique_lock<std::mutex> lk(purge_thd_mutex);
  while (n--)
    purge_thds.push(innobase_create_background_thd("InnoDB purge worker"));
  set_current_thd(thd);
}

extern void* thd_attach_thd(THD*);
extern void thd_detach_thd(void *);

THD* acquire_thd(void **ctx)
{
	std::unique_lock<std::mutex> lk(purge_thd_mutex);
	ut_a(!purge_thds.empty());
	THD* thd = purge_thds.front();
	purge_thds.pop();
	lk.unlock();

	/* Set current thd, and thd->mysys_var as well,
	it might be used by something in the server.*/
	*ctx = thd_attach_thd(thd);
	return thd;
}

void release_thd(THD *thd, void *ctx)
{
	thd_detach_thd(ctx);
	std::unique_lock<std::mutex> lk(purge_thd_mutex);
	purge_thds.push(thd);
	lk.unlock();
	set_current_thd(0);
}



/*
  Called by timer when purge coordinator decides
  to delay processing of purge records.
*/
static void purge_coordinator_timer_callback(void *)
{
  if (!purge_sys.enabled() || purge_sys.paused() ||
      purge_state.m_running || !trx_sys.rseg_history_len)
    return;

  if (purge_state.m_history_length < 5000 &&
      purge_state.m_history_length == trx_sys.rseg_history_len)
    /* No new records were added since wait started.
    Simply wait for new records. The magic number 5000 is an
    approximation for the case where we	have cached UNDO
    log records which prevent truncate of the UNDO segments.*/
    return;
  srv_wake_purge_thread_if_not_active();
}

static void purge_worker_callback(void*)
{
  ut_ad(!current_thd);
  ut_ad(!srv_read_only_mode);
  ut_ad(srv_force_recovery < SRV_FORCE_NO_BACKGROUND);
  void *ctx;
  THD *thd= acquire_thd(&ctx);
  while (srv_task_execute())
    ut_ad(purge_sys.running());
  release_thd(thd,ctx);
}

static void purge_coordinator_callback_low()
{
  ulint n_total_purged= ULINT_UNDEFINED;
  purge_state.m_history_length= 0;

  if (!purge_sys.enabled() || purge_sys.paused())
    return;
  do
  {
    n_total_purged = 0;
    int sigcount= purge_state.m_running;

    purge_state.m_history_length= srv_do_purge(&n_total_purged);

    /* Check if purge was woken by srv_wake_purge_thread_if_not_active() */

    bool woken_during_purge= purge_state.m_running > sigcount;

    /* If last purge batch processed less than 1 page and there is
    still work to do, delay the next batch by 10ms. Unless
    someone added work and woke us up. */
    if (n_total_purged == 0)
    {
      if (trx_sys.rseg_history_len == 0)
        return;
      if (!woken_during_purge)
      {
        /* Delay next purge round*/
        purge_coordinator_timer->set_time(10, 0);
        return;
      }
    }
  }
  while ((purge_sys.enabled() && !purge_sys.paused()) ||
         !srv_purge_should_exit());
}

static void purge_coordinator_callback(void*)
{
  void *ctx;
  THD *thd= acquire_thd(&ctx);
  purge_coordinator_callback_low();
  release_thd(thd,ctx);
  purge_state.m_running= 0;
}

void srv_init_purge_tasks(uint n_tasks)
{
  purge_task_group.set_max_tasks(n_tasks - 1);
  purge_create_background_thds(n_tasks);
  purge_coordinator_timer= srv_thread_pool->create_timer
    (purge_coordinator_timer_callback, nullptr);
}

static void srv_shutdown_purge_tasks()
{
  purge_coordinator_task.wait();
  delete purge_coordinator_timer;
  purge_coordinator_timer= nullptr;
  purge_worker_task.wait();
  while (!purge_thds.empty())
  {
    innobase_destroy_background_thd(purge_thds.front());
    purge_thds.pop();
  }
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
}

#ifdef UNIV_DEBUG
/** @return number of tasks in queue */
ulint srv_get_task_queue_length()
{
	ulint	n_tasks;

	ut_ad(!srv_read_only_mode);

	mutex_enter(&srv_sys.tasks_mutex);

	n_tasks = UT_LIST_GET_LEN(srv_sys.tasks);

	mutex_exit(&srv_sys.tasks_mutex);

	return(n_tasks);
}
#endif

/** Shut down the purge threads. */
void srv_purge_shutdown()
{
	if (purge_sys.enabled()) {
		while(!srv_purge_should_exit()) {
			ut_a(!purge_sys.paused());
			srv_wake_purge_thread_if_not_active();
			os_thread_sleep(100);
		}
		purge_sys.coordinator_shutdown();
		srv_shutdown_purge_tasks();
	}
}
