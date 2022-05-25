/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2008, 2009, Google Inc.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2013, 2022, MariaDB Corporation.

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
@file include/srv0srv.h
The server main program

Created 10/10/1995 Heikki Tuuri
*******************************************************/

#pragma once

#include "log0log.h"
#include "que0types.h"
#include "trx0types.h"
#include "fil0fil.h"
#include "ut0counter.h"

#include "mysql/psi/mysql_stage.h"
#include "mysql/psi/psi.h"
#include <tpool.h>
#include <memory>

/** Simple non-atomic counter
@tparam	Type  the integer type of the counter */
template <typename Type>
struct alignas(CPU_LEVEL1_DCACHE_LINESIZE) simple_counter
{
  /** Increment the counter */
  Type inc() { return add(1); }
  /** Decrement the counter */
  Type dec() { return add(Type(~0)); }

  /** Add to the counter
  @param i  amount to be added
  @return the value of the counter after adding */
  Type add(Type i) { return m_counter += i; }

  /** @return the value of the counter */
  operator Type() const { return m_counter; }

private:
  /** The counter */
  Type m_counter;
};

/** Global counters used inside InnoDB. */
struct srv_stats_t
{
	typedef ib_counter_t<ulint> ulint_ctr_n_t;
	typedef simple_counter<lsn_t> lsn_ctr_1_t;
	typedef simple_counter<ulint> ulint_ctr_1_t;
	typedef simple_counter<int64_t> int64_ctr_1_t;

	/** Count the amount of data written in total (in bytes) */
	ulint_ctr_1_t		data_written;

	/** Number of buffer pool reads that led to the reading of
	a disk page */
	ulint_ctr_1_t		buf_pool_reads;

	/** Number of bytes saved by page compression */
	ulint_ctr_n_t          page_compression_saved;
	/* Number of pages compressed with page compression */
        ulint_ctr_n_t          pages_page_compressed;
	/* Number of TRIM operations induced by page compression */
        ulint_ctr_n_t          page_compressed_trim_op;
	/* Number of pages decompressed with page compression */
        ulint_ctr_n_t          pages_page_decompressed;
	/* Number of page compression errors */
	ulint_ctr_n_t          pages_page_compression_error;
	/* Number of pages encrypted */
	ulint_ctr_n_t          pages_encrypted;
   	/* Number of pages decrypted */
	ulint_ctr_n_t          pages_decrypted;
	/* Number of merge blocks encrypted */
	ulint_ctr_n_t          n_merge_blocks_encrypted;
	/* Number of merge blocks decrypted */
	ulint_ctr_n_t          n_merge_blocks_decrypted;
	/* Number of row log blocks encrypted */
	ulint_ctr_n_t          n_rowlog_blocks_encrypted;
	/* Number of row log blocks decrypted */
	ulint_ctr_n_t          n_rowlog_blocks_decrypted;

	/** Number of data read in total (in bytes) */
	ulint_ctr_1_t		data_read;

	/** Number of rows read. */
	ulint_ctr_n_t		n_rows_read;

	/** Number of rows updated */
	ulint_ctr_n_t		n_rows_updated;

	/** Number of rows deleted */
	ulint_ctr_n_t		n_rows_deleted;

	/** Number of rows inserted */
	ulint_ctr_n_t		n_rows_inserted;

	/** Number of system rows read. */
	ulint_ctr_n_t		n_system_rows_read;

	/** Number of system rows updated */
	ulint_ctr_n_t		n_system_rows_updated;

	/** Number of system rows deleted */
	ulint_ctr_n_t		n_system_rows_deleted;

	/** Number of system rows inserted */
	ulint_ctr_n_t		n_system_rows_inserted;

	/** Number of times secondary index lookup triggered cluster lookup */
	ulint_ctr_n_t		n_sec_rec_cluster_reads;

	/** Number of times prefix optimization avoided triggering cluster lookup */
	ulint_ctr_n_t		n_sec_rec_cluster_reads_avoided;

	/** Number of encryption_get_latest_key_version calls */
	ulint_ctr_n_t		n_key_requests;

	/** Number of temporary tablespace blocks encrypted */
	ulint_ctr_n_t		n_temp_blocks_encrypted;

	/** Number of temporary tablespace blocks decrypted */
	ulint_ctr_n_t		n_temp_blocks_decrypted;
};

/** We are prepared for a situation that we have this many threads waiting for
a transactional lock inside InnoDB. srv_start() sets the value. */
extern ulint srv_max_n_threads;

extern const char*	srv_main_thread_op_info;

/** Prefix used by MySQL to indicate pre-5.1 table name encoding */
extern const char	srv_mysql50_table_name_prefix[10];

/** The buffer pool dump/load file name */
#define SRV_BUF_DUMP_FILENAME_DEFAULT	"ib_buffer_pool"
extern char*		srv_buf_dump_filename;

/** Boolean config knobs that tell InnoDB to dump the buffer pool at shutdown
and/or load it during startup. */
extern char		srv_buffer_pool_dump_at_shutdown;
extern char		srv_buffer_pool_load_at_startup;

/* Whether to disable file system cache if it is defined */
extern char		srv_disable_sort_file_cache;

/* If the last data file is auto-extended, we add this many pages to it
at a time */
#define SRV_AUTO_EXTEND_INCREMENT (srv_sys_space.get_autoextend_increment())

/** Mutex protecting page_zip_stat_per_index */
extern mysql_mutex_t page_zip_stat_per_index_mutex;
/** Mutex for locking srv_monitor_file */
extern mysql_mutex_t srv_monitor_file_mutex;
/* Temporary file for innodb monitor output */
extern FILE*	srv_monitor_file;
/** Mutex for locking srv_misc_tmpfile */
extern mysql_mutex_t srv_misc_tmpfile_mutex;
/* Temporary file for miscellanous diagnostic output */
extern FILE*	srv_misc_tmpfile;

/* Server parameters which are read from the initfile */

extern char*	srv_data_home;

/** Set if InnoDB must operate in read-only mode. We don't do any
recovery and open all tables in RO mode instead of RW mode. We don't
sync the max trx id to disk either. */
extern my_bool	srv_read_only_mode;
/** Set if InnoDB operates in read-only mode or innodb-force-recovery
is greater than SRV_FORCE_NO_IBUF_MERGE. */
extern my_bool	high_level_read_only;
/** store to its own file each table created by an user; data
dictionary tables are in the system tablespace 0 */
extern my_bool	srv_file_per_table;

/** Sort buffer size in index creation */
extern ulong	srv_sort_buf_size;
/** Maximum modification log file size for online index creation */
extern unsigned long long	srv_online_max_size;

/* If this flag is TRUE, then we will use the native aio of the
OS (provided we compiled Innobase with it in), otherwise we will
use simulated aio.
Currently we support native aio on windows and linux */
extern my_bool	srv_use_native_aio;
extern my_bool	srv_numa_interleave;

/* Use atomic writes i.e disable doublewrite buffer */
extern my_bool srv_use_atomic_writes;

/* Compression algorithm*/
extern ulong innodb_compression_algorithm;

/** TRUE if the server was successfully started */
extern bool	srv_was_started;

/** Server undo tablespaces directory, can be absolute path. */
extern char*	srv_undo_dir;

/** Number of undo tablespaces to use. */
extern uint	srv_undo_tablespaces;

/** The number of UNDO tablespaces that are active (hosting some rollback
segment). It is quite possible that some of the tablespaces doesn't host
any of the rollback-segment based on configuration used. */
extern uint32_t srv_undo_tablespaces_active;

/** Maximum size of undo tablespace. */
extern unsigned long long	srv_max_undo_log_size;

extern uint	srv_n_fil_crypt_threads;
extern uint	srv_n_fil_crypt_threads_started;

/** Rate at which UNDO records should be purged. */
extern ulong	srv_purge_rseg_truncate_frequency;

/** Enable or Disable Truncate of UNDO tablespace. */
extern my_bool	srv_undo_log_truncate;

/* Optimize prefix index queries to skip cluster index lookup when possible */
/* Enables or disables this prefix optimization.  Disabled by default. */
extern my_bool	srv_prefix_index_cluster_optimization;

/** Default size of UNDO tablespace (10MiB for innodb_page_size=16k) */
constexpr ulint SRV_UNDO_TABLESPACE_SIZE_IN_PAGES= (10U << 20) /
  UNIV_PAGE_SIZE_DEF;

extern char*	srv_log_group_home_dir;

/** The InnoDB redo log file size, or 0 when changing the redo log format
at startup (while disallowing writes to the redo log). */
extern ulonglong	srv_log_file_size;
extern ulong	srv_flush_log_at_trx_commit;
extern uint	srv_flush_log_at_timeout;
extern my_bool	srv_adaptive_flushing;
extern my_bool	srv_flush_sync;

/** Requested size in bytes */
extern ulint		srv_buf_pool_size;
/** Requested buffer pool chunk size */
extern size_t		srv_buf_pool_chunk_unit;
/** Scan depth for LRU flush batch i.e.: number of blocks scanned*/
extern ulong	srv_LRU_scan_depth;
/** Whether or not to flush neighbors of a block */
extern ulong	srv_flush_neighbors;
/** Previously requested size */
extern ulint	srv_buf_pool_old_size;
/** Current size as scaling factor for the other components */
extern ulint	srv_buf_pool_base_size;
/** Current size in bytes */
extern ulint	srv_buf_pool_curr_size;
/** Dump this % of each buffer pool during BP dump */
extern ulong	srv_buf_pool_dump_pct;
#ifdef UNIV_DEBUG
/** Abort load after this amount of pages */
extern ulong srv_buf_pool_load_pages_abort;
#endif
/** Lock table size in bytes */
extern ulint	srv_lock_table_size;

/** the value of innodb_checksum_algorithm */
extern ulong	srv_checksum_algorithm;

extern uint	srv_n_file_io_threads;
extern my_bool	srv_random_read_ahead;
extern ulong	srv_read_ahead_threshold;
extern uint	srv_n_read_io_threads;
extern uint	srv_n_write_io_threads;

/* Defragmentation, Origianlly facebook default value is 100, but it's too high */
#define SRV_DEFRAGMENT_FREQUENCY_DEFAULT 40
extern my_bool	srv_defragment;
extern uint	srv_defragment_n_pages;
extern uint	srv_defragment_stats_accuracy;
extern uint	srv_defragment_fill_factor_n_recs;
extern double	srv_defragment_fill_factor;
extern uint	srv_defragment_frequency;
extern ulonglong	srv_defragment_interval;

extern uint	srv_change_buffer_max_size;

/* Number of IO operations per second the server can do */
extern ulong    srv_io_capacity;

/* We use this dummy default value at startup for max_io_capacity.
The real value is set based on the value of io_capacity. */
#define SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT	(~0UL)
#define SRV_MAX_IO_CAPACITY_LIMIT		(~0UL)
extern ulong    srv_max_io_capacity;

/* The "innodb_stats_method" setting, decides how InnoDB is going
to treat NULL value when collecting statistics. It is not defined
as enum type because the configure option takes unsigned integer type. */
extern ulong	srv_innodb_stats_method;

extern ulint	srv_max_n_open_files;

extern double	srv_max_buf_pool_modified_pct;
extern double	srv_max_dirty_pages_pct_lwm;

extern double	srv_adaptive_flushing_lwm;
extern ulong	srv_flushing_avg_loops;

extern ulong	srv_force_recovery;

/** innodb_fast_shutdown=1 skips purge and change buffer merge.
innodb_fast_shutdown=2 effectively crashes the server (no log checkpoint).
innodb_fast_shutdown=3 is a clean shutdown that skips the rollback
of active transaction (to be done on restart). */
extern uint	srv_fast_shutdown;

extern ibool	srv_innodb_status;

extern unsigned long long	srv_stats_transient_sample_pages;
extern my_bool			srv_stats_persistent;
extern unsigned long long	srv_stats_persistent_sample_pages;
extern my_bool			srv_stats_auto_recalc;
extern my_bool			srv_stats_include_delete_marked;
extern unsigned long long	srv_stats_modified_counter;
extern my_bool			srv_stats_sample_traditional;

extern my_bool	srv_use_doublewrite_buf;
extern ulong	srv_checksum_algorithm;

extern my_bool	srv_force_primary_key;

extern ulong	srv_max_purge_lag;
extern ulong	srv_max_purge_lag_delay;

extern my_bool	innodb_encrypt_temporary_tables;

extern my_bool  srv_immediate_scrub_data_uncompressed;
/*-------------------------------------------*/

/** Modes of operation */
enum srv_operation_mode {
	/** Normal mode (MariaDB Server) */
	SRV_OPERATION_NORMAL,
	/** Mariabackup taking a backup */
	SRV_OPERATION_BACKUP,
	/** Mariabackup restoring a backup for subsequent --copy-back */
	SRV_OPERATION_RESTORE,
	/** Mariabackup restoring the incremental part of a backup */
	SRV_OPERATION_RESTORE_DELTA,
	/** Mariabackup restoring a backup for subsequent --export */
	SRV_OPERATION_RESTORE_EXPORT,
	/** Mariabackup taking a backup and avoid deferring
	any tablespace */
	SRV_OPERATION_BACKUP_NO_DEFER
};

/** Current mode of operation */
extern enum srv_operation_mode srv_operation;

/** whether this is the server's first start after mariabackup --prepare */
extern bool srv_start_after_restore;

extern my_bool	srv_print_innodb_monitor;
extern my_bool	srv_print_innodb_lock_monitor;
extern ibool	srv_print_verbose_log;

extern bool	srv_monitor_active;


extern ulong	srv_n_spin_wait_rounds;
extern uint	srv_spin_wait_delay;

extern ulint	srv_truncated_status_writes;
/** Number of initialized rollback segments for persistent undo log */
extern ulong	srv_available_undo_logs;
/** Iterations of the loop bounded by 'srv_active' label. */
extern ulint	srv_main_active_loops;
/** Iterations of the loop bounded by the 'srv_idle' label. */
extern ulint	srv_main_idle_loops;
/** Log writes involving flush. */
extern ulint	srv_log_writes_and_flush;

#ifdef UNIV_DEBUG
extern my_bool	innodb_evict_tables_on_commit_debug;
extern my_bool	srv_purge_view_update_only_debug;

/** InnoDB system tablespace to set during recovery */
extern uint	srv_sys_space_size_debug;
/** whether redo log file has been created at startup */
extern bool	srv_log_file_created;
#endif /* UNIV_DEBUG */

extern ulint	srv_dml_needed_delay;

#define SRV_MAX_N_IO_THREADS	130

/** innodb_purge_threads; the number of purge tasks to use */
extern uint srv_n_purge_threads;

/* the number of pages to purge in one batch */
extern ulong srv_purge_batch_size;

/* print all user-level transactions deadlocks to mysqld stderr */
extern my_bool srv_print_all_deadlocks;

extern my_bool	srv_cmp_per_index_enabled;

/** innodb_encrypt_log */
extern my_bool	srv_encrypt_log;

/* is encryption enabled */
extern ulong	srv_encrypt_tables;


/** Status variables to be passed to MySQL */
extern struct export_var_t export_vars;

/** Global counters */
extern srv_stats_t	srv_stats;

/** Fatal semaphore wait threshold = maximum number of seconds
that semaphore times out in InnoDB */
#define DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT 600
extern ulong	srv_fatal_semaphore_wait_threshold;

/** Buffer pool dump status frequence in percentages */
extern ulong srv_buf_dump_status_frequency;

# ifdef UNIV_PFS_THREAD
extern mysql_pfs_key_t	page_cleaner_thread_key;
extern mysql_pfs_key_t	trx_rollback_clean_thread_key;
extern mysql_pfs_key_t	thread_pool_thread_key;

/* This macro register the current thread and its key with performance
schema */
#  define pfs_register_thread(key)			\
do {							\
	struct PSI_thread* psi __attribute__((unused))	\
		= PSI_CALL_new_thread(key, NULL, 0);	\
	PSI_CALL_set_thread_os_id(psi);			\
	PSI_CALL_set_thread(psi);			\
} while (0)

/* This macro delist the current thread from performance schema */
#  define pfs_delete_thread()				\
do {								\
	PSI_CALL_delete_current_thread();		\
} while (0)
# else
#  define pfs_register_thread(key)
#  define pfs_delete_thread()
# endif /* UNIV_PFS_THREAD */

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Performance schema stage event for monitoring ALTER TABLE progress
in ha_innobase::commit_inplace_alter_table(). */
extern PSI_stage_info	srv_stage_alter_table_end;

/** Performance schema stage event for monitoring ALTER TABLE progress
row_merge_insert_index_tuples(). */
extern PSI_stage_info	srv_stage_alter_table_insert;

/** Performance schema stage event for monitoring ALTER TABLE progress
row_log_apply(). */
extern PSI_stage_info	srv_stage_alter_table_log_index;

/** Performance schema stage event for monitoring ALTER TABLE progress
row_log_table_apply(). */
extern PSI_stage_info	srv_stage_alter_table_log_table;

/** Performance schema stage event for monitoring ALTER TABLE progress
row_merge_sort(). */
extern PSI_stage_info	srv_stage_alter_table_merge_sort;

/** Performance schema stage event for monitoring ALTER TABLE progress
row_merge_read_clustered_index(). */
extern PSI_stage_info	srv_stage_alter_table_read_pk_internal_sort;

/** Performance schema stage event for monitoring buffer pool load progress. */
extern PSI_stage_info	srv_stage_buffer_pool_load;
#endif /* HAVE_PSI_STAGE_INTERFACE */

/** Alternatives for srv_force_recovery. Non-zero values are intended
to help the user get a damaged database up so that he can dump intact
tables and rows with SELECT INTO OUTFILE. The database must not otherwise
be used with these options! A bigger number below means that all precautions
of lower numbers are included. */
enum {
	SRV_FORCE_IGNORE_CORRUPT = 1,	/*!< let the server run even if it
					detects a corrupt page */
	SRV_FORCE_NO_BACKGROUND	= 2,	/*!< prevent the main thread from
					running: if a crash would occur
					in purge, this prevents it */
	SRV_FORCE_NO_TRX_UNDO = 3,	/*!< do not run DML rollback after
					recovery */
	SRV_FORCE_NO_DDL_UNDO = 4,	/*!< prevent also DDL rollback */
	SRV_FORCE_NO_UNDO_LOG_SCAN = 5,	/*!< do not look at undo logs when
					starting the database: InnoDB will
					treat even incomplete transactions
					as committed */
	SRV_FORCE_NO_LOG_REDO = 6	/*!< do not do the log roll-forward
					in connection with recovery */
};

/* Alternatives for srv_innodb_stats_method, which could be changed by
setting innodb_stats_method */
enum srv_stats_method_name_enum {
	SRV_STATS_NULLS_EQUAL,		/* All NULL values are treated as
					equal. This is the default setting
					for innodb_stats_method */
	SRV_STATS_NULLS_UNEQUAL,	/* All NULL values are treated as
					NOT equal. */
	SRV_STATS_NULLS_IGNORED		/* NULL values are ignored */
};

typedef enum srv_stats_method_name_enum		srv_stats_method_name_t;

/*********************************************************************//**
Boots Innobase server. */
void
srv_boot(void);
/*==========*/
/*********************************************************************//**
Frees the data structures created in srv_init(). */
void
srv_free(void);

/** Wake up the purge if there is work to do. */
void
srv_wake_purge_thread_if_not_active();

/******************************************************************//**
Outputs to a file the output of the InnoDB Monitor.
@return FALSE if not all information printed
due to failure to obtain necessary mutex */
ibool
srv_printf_innodb_monitor(
/*======================*/
	FILE*	file,		/*!< in: output stream */
	ibool	nowait,		/*!< in: whether to wait for lock_sys.latch */
	ulint*	trx_start,	/*!< out: file position of the start of
				the list of active transactions */
	ulint*	trx_end);	/*!< out: file position of the end of
				the list of active transactions */

/******************************************************************//**
Function to pass InnoDB status variables to MySQL */
void
srv_export_innodb_status(void);
/*==========================*/
/*******************************************************************//**
Get current server activity count.
@return activity count. */
ulint
srv_get_activity_count(void);
/*========================*/

/******************************************************************//**
Increment the server activity counter. */
void
srv_inc_activity_count(void);
/*=========================*/

/**********************************************************************//**
Enqueues a task to server task queue and releases a worker thread, if there
is a suspended one. */
void
srv_que_task_enqueue_low(
/*=====================*/
	que_thr_t*	thr);	/*!< in: query thread */

/**
Flag which is set, whenever innodb_purge_threads changes.
It is read and reset in srv_do_purge().

Thus it is Atomic_counter<int>, not bool, since unprotected
reads are used. We just need an atomic with relaxed memory
order, to please Thread Sanitizer.
*/
extern Atomic_counter<int> srv_purge_thread_count_changed;

#ifdef UNIV_DEBUG
/** @return whether purge or master task is active */
bool srv_any_background_activity();
#endif

extern "C" {


/** Periodic task which prints the info output by various InnoDB monitors.*/
void srv_monitor_task(void*);


/** The periodic master task controlling the server. */
void srv_master_callback(void*);


/**
Complete the shutdown tasks such as background DROP TABLE,
and optionally change buffer merge (on innodb_fast_shutdown=0). */
void srv_shutdown(bool ibuf_merge);

} /* extern "C" */

#ifdef UNIV_DEBUG
/** @return number of tasks in queue */
ulint srv_get_task_queue_length();
#endif

/** Shut down the purge threads. */
void srv_purge_shutdown();

/** Init purge tasks*/
void srv_init_purge_tasks();

/** Status variables to be passed to MySQL */
struct export_var_t{
#ifdef BTR_CUR_HASH_ADAPT
	ulint innodb_ahi_hit;
	ulint innodb_ahi_miss;
#endif /* BTR_CUR_HASH_ADAPT */
	char  innodb_buffer_pool_dump_status[OS_FILE_MAX_PATH + 128];/*!< Buf pool dump status */
	char  innodb_buffer_pool_load_status[OS_FILE_MAX_PATH + 128];/*!< Buf pool load status */
	char  innodb_buffer_pool_resize_status[512];/*!< Buf pool resize status */
	my_bool innodb_buffer_pool_load_incomplete;/*!< Buf pool load incomplete */
	ulint innodb_buffer_pool_pages_total;	/*!< Buffer pool size */
	ulint innodb_buffer_pool_pages_data;	/*!< Data pages */
	ulint innodb_buffer_pool_bytes_data;	/*!< File bytes used */
	ulint innodb_buffer_pool_pages_dirty;	/*!< Dirty data pages */
	ulint innodb_buffer_pool_bytes_dirty;	/*!< File bytes modified */
	ulint innodb_buffer_pool_pages_misc;	/*!< Miscellanous pages */
	ulint innodb_buffer_pool_pages_free;	/*!< Free pages */
#ifdef UNIV_DEBUG
	ulint innodb_buffer_pool_pages_latched;	/*!< Latched pages */
#endif /* UNIV_DEBUG */
	ulint innodb_buffer_pool_pages_made_not_young;
	ulint innodb_buffer_pool_pages_made_young;
	ulint innodb_buffer_pool_pages_old;
	ulint innodb_buffer_pool_read_requests;	/*!< buf_pool.stat.n_page_gets */
	ulint innodb_buffer_pool_reads;		/*!< srv_buf_pool_reads */
	ulint innodb_buffer_pool_read_ahead_rnd;/*!< srv_read_ahead_rnd */
	ulint innodb_buffer_pool_read_ahead;	/*!< srv_read_ahead */
	ulint innodb_buffer_pool_read_ahead_evicted;/*!< srv_read_ahead evicted*/
	ulint innodb_checkpoint_age;
	ulint innodb_checkpoint_max_age;
	ulint innodb_data_pending_reads;	/*!< Pending reads */
	ulint innodb_data_pending_writes;	/*!< Pending writes */
	ulint innodb_data_read;			/*!< Data bytes read */
	ulint innodb_data_writes;		/*!< I/O write requests */
	ulint innodb_data_written;		/*!< Data bytes written */
	ulint innodb_data_reads;		/*!< I/O read requests */
	ulint innodb_dblwr_pages_written;	/*!< srv_dblwr_pages_written */
	ulint innodb_dblwr_writes;		/*!< srv_dblwr_writes */
	ulint innodb_deadlocks;
	ulint innodb_history_list_length;
	lsn_t innodb_lsn_current;
	lsn_t innodb_lsn_flushed;
	lsn_t innodb_lsn_last_checkpoint;
	trx_id_t innodb_max_trx_id;
#ifdef BTR_CUR_HASH_ADAPT
	ulint innodb_mem_adaptive_hash;
#endif
	ulint innodb_mem_dictionary;
	/** log_sys.get_lsn() - recv_sys.lsn */
	lsn_t innodb_os_log_written;
	ulint innodb_row_lock_waits;		/*!< srv_n_lock_wait_count */
	ulint innodb_row_lock_current_waits;	/*!< srv_n_lock_wait_current_count */
	int64_t innodb_row_lock_time;		/*!< srv_n_lock_wait_time
						/ 1000 */
	ulint innodb_row_lock_time_avg;		/*!< srv_n_lock_wait_time
						/ 1000
						/ srv_n_lock_wait_count */
	ulint innodb_row_lock_time_max;		/*!< srv_n_lock_max_wait_time
						/ 1000 */
	ulint innodb_rows_read;			/*!< srv_n_rows_read */
	ulint innodb_rows_inserted;		/*!< srv_n_rows_inserted */
	ulint innodb_rows_updated;		/*!< srv_n_rows_updated */
	ulint innodb_rows_deleted;		/*!< srv_n_rows_deleted */
	ulint innodb_system_rows_read; /*!< srv_n_system_rows_read */
	ulint innodb_system_rows_inserted; /*!< srv_n_system_rows_inserted */
	ulint innodb_system_rows_updated; /*!< srv_n_system_rows_updated */
	ulint innodb_system_rows_deleted; /*!< srv_n_system_rows_deleted*/
	ulint innodb_truncated_status_writes;	/*!< srv_truncated_status_writes */

	/** Number of undo tablespace truncation operations */
	ulong innodb_undo_truncations;
	ulint innodb_defragment_compression_failures; /*!< Number of
						defragment re-compression
						failures */

	ulint innodb_defragment_failures;	/*!< Number of defragment
						failures*/
	ulint innodb_defragment_count;		/*!< Number of defragment
						operations*/

	/** Number of instant ALTER TABLE operations that affect columns */
	ulong innodb_instant_alter_column;

	ulint innodb_onlineddl_rowlog_rows;	/*!< Online alter rows */
	ulint innodb_onlineddl_rowlog_pct_used; /*!< Online alter percentage
						of used row log buffer */
	ulint innodb_onlineddl_pct_progress;	/*!< Online alter progress */

	int64_t innodb_page_compression_saved;/*!< Number of bytes saved
						by page compression */
	int64_t innodb_pages_page_compressed;/*!< Number of pages
						compressed by page compression */
	int64_t innodb_page_compressed_trim_op;/*!< Number of TRIM operations
						induced by page compression */
	int64_t innodb_pages_page_decompressed;/*!< Number of pages
						decompressed by page
						compression */
	int64_t innodb_pages_page_compression_error;/*!< Number of page
						compression errors */
	int64_t innodb_pages_encrypted;      /*!< Number of pages
						encrypted */
	int64_t innodb_pages_decrypted;      /*!< Number of pages
						decrypted */

	/*!< Number of merge blocks encrypted */
	ib_int64_t innodb_n_merge_blocks_encrypted;
	/*!< Number of merge blocks decrypted */
	ib_int64_t innodb_n_merge_blocks_decrypted;
	/*!< Number of row log blocks encrypted */
	ib_int64_t innodb_n_rowlog_blocks_encrypted;
	/*!< Number of row log blocks decrypted */
	ib_int64_t innodb_n_rowlog_blocks_decrypted;

	/* Number of temporary tablespace pages encrypted */
	ib_int64_t innodb_n_temp_blocks_encrypted;

	/* Number of temporary tablespace pages decrypted */
	ib_int64_t innodb_n_temp_blocks_decrypted;

	ulint innodb_sec_rec_cluster_reads;	/*!< srv_sec_rec_cluster_reads */
	ulint innodb_sec_rec_cluster_reads_avoided;/*!< srv_sec_rec_cluster_reads_avoided */

	ulint innodb_encryption_rotation_pages_read_from_cache;
	ulint innodb_encryption_rotation_pages_read_from_disk;
	ulint innodb_encryption_rotation_pages_modified;
	ulint innodb_encryption_rotation_pages_flushed;
	ulint innodb_encryption_rotation_estimated_iops;
	int64_t innodb_encryption_key_requests;
};

extern tpool::thread_pool *srv_thread_pool;
extern std::unique_ptr<tpool::timer> srv_master_timer;
extern std::unique_ptr<tpool::timer> srv_monitor_timer;

/** The interval at which srv_monitor_task is invoked, in milliseconds */
constexpr unsigned SRV_MONITOR_INTERVAL= 15000; /* 4 times per minute */

static inline void srv_monitor_timer_schedule_now()
{
  srv_monitor_timer->set_time(0, SRV_MONITOR_INTERVAL);
}
static inline void srv_start_periodic_timer(std::unique_ptr<tpool::timer>& t,
                                            void (*func)(void*), int period)
{
  t.reset(srv_thread_pool->create_timer(func));
  t->set_time(0, period);
}

void srv_thread_pool_init();
void srv_thread_pool_end();
