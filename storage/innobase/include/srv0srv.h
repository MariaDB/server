/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2008, 2009, Google Inc.
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
@file include/srv0srv.h
The server main program

Created 10/10/1995 Heikki Tuuri
*******************************************************/

#ifndef srv0srv_h
#define srv0srv_h

#include "log0log.h"
#include "os0event.h"
#include "que0types.h"
#include "trx0types.h"
#include "srv0conc.h"
#include "fil0fil.h"

#include "mysql/psi/mysql_stage.h"
#include "mysql/psi/psi.h"

/** Global counters used inside InnoDB. */
struct srv_stats_t
{
	typedef ib_counter_t<ulint, 64> ulint_ctr_64_t;
	typedef simple_counter<lsn_t> lsn_ctr_1_t;
	typedef simple_counter<ulint> ulint_ctr_1_t;
	typedef simple_counter<int64_t> int64_ctr_1_t;

	/** Count the amount of data written in total (in bytes) */
	ulint_ctr_1_t		data_written;

	/** Number of the log write requests done */
	ulint_ctr_1_t		log_write_requests;

	/** Number of physical writes to the log performed */
	ulint_ctr_1_t		log_writes;

	/** Amount of data padded for log write ahead */
	ulint_ctr_1_t		log_padded;

	/** Amount of data written to the log files in bytes */
	lsn_ctr_1_t		os_log_written;

	/** Number of writes being done to the log files.
	Protected by log_sys.write_mutex. */
	ulint_ctr_1_t		os_log_pending_writes;

	/** We increase this counter, when we don't have enough
	space in the log buffer and have to flush it */
	ulint_ctr_1_t		log_waits;

	/** Count the number of times the doublewrite buffer was flushed */
	ulint_ctr_1_t		dblwr_writes;

	/** Store the number of pages that have been flushed to the
	doublewrite buffer */
	ulint_ctr_1_t		dblwr_pages_written;

	/** Store the number of write requests issued */
	ulint_ctr_1_t		buf_pool_write_requests;

	/** Store the number of times when we had to wait for a free page
	in the buffer pool. It happens when the buffer pool is full and we
	need to make a flush, in order to be able to read or create a page. */
	ulint_ctr_1_t		buf_pool_wait_free;

	/** Count the number of pages that were written from buffer
	pool to the disk */
	ulint_ctr_1_t		buf_pool_flushed;

	/** Number of buffer pool reads that led to the reading of
	a disk page */
	ulint_ctr_1_t		buf_pool_reads;

	/** Number of bytes saved by page compression */
	ulint_ctr_64_t          page_compression_saved;
	/* Number of index pages written */
	ulint_ctr_64_t          index_pages_written;
	/* Number of non index pages written */
	ulint_ctr_64_t          non_index_pages_written;
	/* Number of pages compressed with page compression */
        ulint_ctr_64_t          pages_page_compressed;
	/* Number of TRIM operations induced by page compression */
        ulint_ctr_64_t          page_compressed_trim_op;
	/* Number of pages decompressed with page compression */
        ulint_ctr_64_t          pages_page_decompressed;
	/* Number of page compression errors */
	ulint_ctr_64_t          pages_page_compression_error;
	/* Number of pages encrypted */
	ulint_ctr_64_t          pages_encrypted;
   	/* Number of pages decrypted */
	ulint_ctr_64_t          pages_decrypted;
	/* Number of merge blocks encrypted */
	ulint_ctr_64_t          n_merge_blocks_encrypted;
	/* Number of merge blocks decrypted */
	ulint_ctr_64_t          n_merge_blocks_decrypted;
	/* Number of row log blocks encrypted */
	ulint_ctr_64_t          n_rowlog_blocks_encrypted;
	/* Number of row log blocks decrypted */
	ulint_ctr_64_t          n_rowlog_blocks_decrypted;

	/** Number of data read in total (in bytes) */
	ulint_ctr_1_t		data_read;

	/** Wait time of database locks */
	int64_ctr_1_t		n_lock_wait_time;

	/** Number of database lock waits */
	ulint_ctr_1_t		n_lock_wait_count;

	/** Number of threads currently waiting on database locks */
	MY_ALIGNED(CACHE_LINE_SIZE) Atomic_counter<ulint>
				n_lock_wait_current_count;

	/** Number of rows read. */
	ulint_ctr_64_t		n_rows_read;

	/** Number of rows updated */
	ulint_ctr_64_t		n_rows_updated;

	/** Number of rows deleted */
	ulint_ctr_64_t		n_rows_deleted;

	/** Number of rows inserted */
	ulint_ctr_64_t		n_rows_inserted;

	/** Number of system rows read. */
	ulint_ctr_64_t		n_system_rows_read;

	/** Number of system rows updated */
	ulint_ctr_64_t		n_system_rows_updated;

	/** Number of system rows deleted */
	ulint_ctr_64_t		n_system_rows_deleted;

	/** Number of system rows inserted */
	ulint_ctr_64_t		n_system_rows_inserted;

	/** Number of times secondary index lookup triggered cluster lookup */
	ulint_ctr_64_t		n_sec_rec_cluster_reads;

	/** Number of times prefix optimization avoided triggering cluster lookup */
	ulint_ctr_64_t		n_sec_rec_cluster_reads_avoided;

	/** Number of encryption_get_latest_key_version calls */
	ulint_ctr_64_t		n_key_requests;

	/** Number of log scrub operations */
	ulint_ctr_64_t		n_log_scrubs;

	/** Number of spaces in keyrotation list */
	ulint_ctr_64_t		key_rotation_list_length;

	/** Number of temporary tablespace blocks encrypted */
	ulint_ctr_64_t		n_temp_blocks_encrypted;

	/** Number of temporary tablespace blocks decrypted */
	ulint_ctr_64_t		n_temp_blocks_decrypted;
};

extern const char*	srv_main_thread_op_info;

/** Prefix used by MySQL to indicate pre-5.1 table name encoding */
extern const char	srv_mysql50_table_name_prefix[10];

/** Event to signal srv_monitor_thread. Not protected by a mutex.
Set after setting srv_print_innodb_monitor. */
extern os_event_t	srv_monitor_event;

/** Event to signal the shutdown of srv_error_monitor_thread.
Not protected by a mutex. */
extern os_event_t	srv_error_event;

/** Event for waking up buf_dump_thread. Not protected by a mutex.
Set on shutdown or by buf_dump_start() or buf_load_start(). */
extern os_event_t	srv_buf_dump_event;

/** The buffer pool resize thread waits on this event. */
extern os_event_t	srv_buf_resize_event;

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
extern ib_mutex_t	page_zip_stat_per_index_mutex;
/* Mutex for locking srv_monitor_file. Not created if srv_read_only_mode */
extern ib_mutex_t	srv_monitor_file_mutex;
/* Temporary file for innodb monitor output */
extern FILE*	srv_monitor_file;
/* Mutex for locking srv_misc_tmpfile. Only created if !srv_read_only_mode.
This mutex has a very low rank; threads reserving it should not
acquire any further latches or sleep before releasing this one. */
extern ib_mutex_t	srv_misc_tmpfile_mutex;
/* Temporary file for miscellanous diagnostic output */
extern FILE*	srv_misc_tmpfile;

/* Server parameters which are read from the initfile */

extern char*	srv_data_home;

/** Set if InnoDB must operate in read-only mode. We don't do any
recovery and open all tables in RO mode instead of RW mode. We don't
sync the max trx id to disk either. */
extern my_bool	srv_read_only_mode;
/** Set if InnoDB operates in read-only mode or innodb-force-recovery
is greater than SRV_FORCE_NO_TRX_UNDO. */
extern my_bool	high_level_read_only;
/** store to its own file each table created by an user; data
dictionary tables are in the system tablespace 0 */
extern my_bool	srv_file_per_table;
/** Sleep delay for threads waiting to enter InnoDB. In micro-seconds. */
extern	ulong	srv_thread_sleep_delay;
/** Maximum sleep delay (in micro-seconds), value of 0 disables it.*/
extern	ulong	srv_adaptive_max_sleep_delay;

/** Place locks to records only i.e. do not use next-key locking except
on duplicate key checking and foreign key checking */
extern ibool	srv_locks_unsafe_for_binlog;

/** Sort buffer size in index creation */
extern ulong	srv_sort_buf_size;
/** Maximum modification log file size for online index creation */
extern unsigned long long	srv_online_max_size;

/* If this flag is TRUE, then we will use the native aio of the
OS (provided we compiled Innobase with it in), otherwise we will
use simulated aio we build below with threads.
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
extern ulong	srv_undo_tablespaces;

/** The number of UNDO tablespaces that are open and ready to use. */
extern ulint	srv_undo_tablespaces_open;

/** The number of UNDO tablespaces that are active (hosting some rollback
segment). It is quite possible that some of the tablespaces doesn't host
any of the rollback-segment based on configuration used. */
extern ulint	srv_undo_tablespaces_active;

/** Undo tablespaces starts with space_id. */
extern	ulint	srv_undo_space_id_start;

/** Check whether given space id is undo tablespace id
@param[in]	space_id	space id to check
@return true if it is undo tablespace else false. */
inline
bool
srv_is_undo_tablespace(ulint space_id)
{
	return srv_undo_space_id_start > 0
		&& space_id >= srv_undo_space_id_start
		&& space_id < (srv_undo_space_id_start
			       + srv_undo_tablespaces_open);
}

/** The number of undo segments to use */
extern ulong	srv_undo_logs;

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

/** Default size of UNDO tablespace while it is created new. */
extern const ulint	SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;

extern char*	srv_log_group_home_dir;

extern ulong	srv_n_log_files;
/** The InnoDB redo log file size, or 0 when changing the redo log format
at startup (while disallowing writes to the redo log). */
extern ulonglong	srv_log_file_size;
extern ulong	srv_log_buffer_size;
extern ulong	srv_flush_log_at_trx_commit;
extern uint	srv_flush_log_at_timeout;
extern ulong	srv_log_write_ahead_size;
extern my_bool	srv_adaptive_flushing;
extern my_bool	srv_flush_sync;

#ifdef WITH_INNODB_DISALLOW_WRITES
/* When this event is reset we do not allow any file writes to take place. */
extern os_event_t	srv_allow_writes_event;
#endif /* WITH_INNODB_DISALLOW_WRITES */

/* If this flag is TRUE, then we will load the indexes' (and tables') metadata
even if they are marked as "corrupted". Mostly it is for DBA to process
corrupted index and table */
extern my_bool	srv_load_corrupted;

/** Requested size in bytes */
extern ulint		srv_buf_pool_size;
/** Minimum pool size in bytes */
extern const ulint	srv_buf_pool_min_size;
/** Default pool size in bytes */
extern const ulint	srv_buf_pool_def_size;
/** Requested buffer pool chunk size. Each buffer pool instance consists
of one or more chunks. */
extern ulong		srv_buf_pool_chunk_unit;
/** Requested number of buffer pool instances */
extern ulong		srv_buf_pool_instances;
/** Default number of buffer pool instances */
extern const ulong	srv_buf_pool_instances_default;
/** Number of locks to protect buf_pool->page_hash */
extern ulong	srv_n_page_hash_locks;
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

extern ulint	srv_n_file_io_threads;
extern my_bool	srv_random_read_ahead;
extern ulong	srv_read_ahead_threshold;
extern ulong	srv_n_read_io_threads;
extern ulong	srv_n_write_io_threads;

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
/* Returns the number of IO operations that is X percent of the
capacity. PCT_IO(5) -> returns the number of IO operations that
is 5% of the max where max is srv_io_capacity.  */
#define PCT_IO(p) ((ulong) (srv_io_capacity * ((double) (p) / 100.0)))

/* The "innodb_stats_method" setting, decides how InnoDB is going
to treat NULL value when collecting statistics. It is not defined
as enum type because the configure option takes unsigned integer type. */
extern ulong	srv_innodb_stats_method;

extern ulint	srv_max_n_open_files;

extern ulong	srv_n_page_cleaners;

extern double	srv_max_dirty_pages_pct;
extern double	srv_max_dirty_pages_pct_lwm;

extern double	srv_adaptive_flushing_lwm;
extern ulong	srv_flushing_avg_loops;

extern ulong	srv_force_recovery;

extern uint	srv_fast_shutdown;	/*!< If this is 1, do not do a
					purge and index buffer merge.
					If this 2, do not even flush the
					buffer pool to data files at the
					shutdown: we effectively 'crash'
					InnoDB (but lose no committed
					transactions). */

/** Signal to shut down InnoDB (NULL if shutdown was signaled, or if
running in innodb_read_only mode, srv_read_only_mode) */
extern std::atomic<st_my_thread_var *> srv_running;

extern ibool	srv_innodb_status;

extern unsigned long long	srv_stats_transient_sample_pages;
extern my_bool			srv_stats_persistent;
extern unsigned long long	srv_stats_persistent_sample_pages;
extern my_bool			srv_stats_auto_recalc;
extern my_bool			srv_stats_include_delete_marked;
extern unsigned long long	srv_stats_modified_counter;
extern my_bool			srv_stats_sample_traditional;

extern my_bool	srv_use_doublewrite_buf;
extern ulong	srv_doublewrite_batch_size;
extern ulong	srv_checksum_algorithm;

extern double	srv_max_buf_pool_modified_pct;
extern my_bool	srv_force_primary_key;

extern double	srv_max_buf_pool_modified_pct;
extern ulong	srv_max_purge_lag;
extern ulong	srv_max_purge_lag_delay;

extern ulong	srv_replication_delay;

extern my_bool	innodb_encrypt_temporary_tables;

/*-------------------------------------------*/

/** Modes of operation */
enum srv_operation_mode {
	/** Normal mode (MariaDB Server) */
	SRV_OPERATION_NORMAL,
	/** Mariabackup taking a backup */
	SRV_OPERATION_BACKUP,
	/** Mariabackup restoring a backup for subsequent --copy-back */
	SRV_OPERATION_RESTORE,
	/** Mariabackup restoring a backup with rolling back prepared XA's*/
	SRV_OPERATION_RESTORE_ROLLBACK_XA,
	/** Mariabackup restoring the incremental part of a backup */
	SRV_OPERATION_RESTORE_DELTA,
	/** Mariabackup restoring a backup for subsequent --export */
	SRV_OPERATION_RESTORE_EXPORT
};

/** Current mode of operation */
extern enum srv_operation_mode srv_operation;

inline bool is_mariabackup_restore()
{
	/* To rollback XA's trx_sys must be initialized, the rest is the same
	as regular backup restore, that is why we join this two operations in
	the most cases. */
	return srv_operation == SRV_OPERATION_RESTORE
	       || srv_operation == SRV_OPERATION_RESTORE_ROLLBACK_XA;
}

inline bool is_mariabackup_restore_or_export()
{
	return is_mariabackup_restore()
	       || srv_operation == SRV_OPERATION_RESTORE_EXPORT;
}

extern my_bool	srv_print_innodb_monitor;
extern my_bool	srv_print_innodb_lock_monitor;
extern ibool	srv_print_verbose_log;

extern bool	srv_monitor_active;
extern bool	srv_error_monitor_active;

/* TRUE during the lifetime of the buffer pool dump/load thread */
extern bool	srv_buf_dump_thread_active;

/* true during the lifetime of the buffer pool resize thread */
extern bool	srv_buf_resize_thread_active;

/* TRUE during the lifetime of the stats thread */
extern bool	srv_dict_stats_thread_active;

/* TRUE if enable log scrubbing */
extern my_bool	srv_scrub_log;

extern ulong	srv_n_spin_wait_rounds;
extern ulong	srv_n_free_tickets_to_enter;
extern ulong	srv_thread_sleep_delay;
extern uint	srv_spin_wait_delay;

extern ulint	srv_truncated_status_writes;
/** Number of initialized rollback segments for persistent undo log */
extern ulong	srv_available_undo_logs;

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
extern my_bool	srv_ibuf_disable_background_merge;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

#ifdef UNIV_DEBUG
extern my_bool	innodb_evict_tables_on_commit_debug;
extern my_bool	srv_sync_debug;
extern my_bool	srv_purge_view_update_only_debug;

/** Value of MySQL global used to disable master thread. */
extern my_bool	srv_master_thread_disabled_debug;
/** InnoDB system tablespace to set during recovery */
extern uint	srv_sys_space_size_debug;
/** whether redo log files have been created at startup */
extern bool	srv_log_files_created;
#endif /* UNIV_DEBUG */

extern ulint	srv_dml_needed_delay;

#define SRV_MAX_N_IO_THREADS	130

/* Array of English strings describing the current state of an
i/o handler thread */
extern const char* srv_io_thread_op_info[];
extern const char* srv_io_thread_function[];

/* the number of purge threads to use from the worker pool (currently 0 or 1) */
extern ulong srv_n_purge_threads;

/* the number of pages to purge in one batch */
extern ulong srv_purge_batch_size;

/* the number of sync wait arrays */
extern ulong srv_sync_array_size;

/* print all user-level transactions deadlocks to mysqld stderr */
extern my_bool srv_print_all_deadlocks;

extern my_bool	srv_cmp_per_index_enabled;

/* is encryption enabled */
extern ulong	srv_encrypt_tables;

/** Status variables to be passed to MySQL */
extern struct export_var_t export_vars;

/** Global counters */
extern srv_stats_t	srv_stats;

/** Simulate compression failures. */
extern uint srv_simulate_comp_failures;

/** Fatal semaphore wait threshold = maximum number of seconds
that semaphore times out in InnoDB */
#define DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT 600
extern ulong	srv_fatal_semaphore_wait_threshold;

/** Buffer pool dump status frequence in percentages */
extern ulong srv_buf_dump_status_frequency;

#define srv_max_purge_threads 32

# ifdef UNIV_PFS_THREAD
/* Keys to register InnoDB threads with performance schema */
extern mysql_pfs_key_t	buf_dump_thread_key;
extern mysql_pfs_key_t	dict_stats_thread_key;
extern mysql_pfs_key_t	io_handler_thread_key;
extern mysql_pfs_key_t	io_ibuf_thread_key;
extern mysql_pfs_key_t	io_log_thread_key;
extern mysql_pfs_key_t	io_read_thread_key;
extern mysql_pfs_key_t	io_write_thread_key;
extern mysql_pfs_key_t	page_cleaner_thread_key;
extern mysql_pfs_key_t	recv_writer_thread_key;
extern mysql_pfs_key_t	srv_error_monitor_thread_key;
extern mysql_pfs_key_t	srv_lock_timeout_thread_key;
extern mysql_pfs_key_t	srv_master_thread_key;
extern mysql_pfs_key_t	srv_monitor_thread_key;
extern mysql_pfs_key_t	srv_purge_thread_key;
extern mysql_pfs_key_t	srv_worker_thread_key;
extern mysql_pfs_key_t	trx_rollback_clean_thread_key;

/* This macro register the current thread and its key with performance
schema */
#  define pfs_register_thread(key)			\
do {								\
	struct PSI_thread* psi = PSI_CALL_new_thread(key, NULL, 0);\
	/* JAN: TODO: MYSQL 5.7 PSI                             \
	PSI_CALL_set_thread_os_id(psi);	*/		\
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
everything after flush log_make_checkpoint(). */
extern PSI_stage_info	srv_stage_alter_table_end;

/** Performance schema stage event for monitoring ALTER TABLE progress
log_make_checkpoint(). */
extern PSI_stage_info	srv_stage_alter_table_flush;

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


/** Alternatives for innodb_flush_method */
enum srv_flush_t {
	SRV_FSYNC = 0,	/*!< fsync, the default */
	SRV_O_DSYNC,	/*!< open log files in O_SYNC mode */
	SRV_LITTLESYNC,	/*!< do not call os_file_flush()
				when writing data files, but do flush
				after writing to log files */
	SRV_NOSYNC,	/*!< do not flush after writing */
	SRV_O_DIRECT,	/*!< invoke os_file_set_nocache() on
				data files. This implies using
				non-buffered IO but still using fsync,
				the reason for which is that some FS
				do not flush meta-data when
				unbuffered IO happens */
	SRV_O_DIRECT_NO_FSYNC
				/*!< do not use fsync() when using
				direct IO i.e.: it can be set to avoid
				the fsync() call that we make when
				using SRV_UNIX_O_DIRECT. However, in
				this case user/DBA should be sure about
				the integrity of the meta-data */
#ifdef _WIN32
	,SRV_ALL_O_DIRECT_FSYNC
				/*!< Traditional Windows appoach to open 
				all files without caching, and do FileFlushBuffers()*/
#endif
};
/** innodb_flush_method */
extern ulong srv_file_flush_method;

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
	SRV_FORCE_NO_TRX_UNDO = 3,	/*!< do not run trx rollback after
					recovery */
	SRV_FORCE_NO_IBUF_MERGE = 4,	/*!< prevent also ibuf operations:
					if they would cause a crash, better
					not do them */
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

/** Types of threads existing in the system. */
enum srv_thread_type {
	SRV_NONE,			/*!< None */
	SRV_WORKER,			/*!< threads serving parallelized
					queries and queries released from
					lock wait */
	SRV_PURGE,			/*!< Purge coordinator thread */
	SRV_MASTER			/*!< the master thread, (whose type
					number must be biggest) */
};

/*********************************************************************//**
Boots Innobase server. */
void
srv_boot(void);
/*==========*/
/*********************************************************************//**
Frees the data structures created in srv_init(). */
void
srv_free(void);
/*==========*/
/*********************************************************************//**
Sets the info describing an i/o thread current state. */
void
srv_set_io_thread_op_info(
/*======================*/
	ulint		i,	/*!< in: the 'segment' of the i/o thread */
	const char*	str);	/*!< in: constant char string describing the
				state */
/*********************************************************************//**
Resets the info describing an i/o thread current state. */
void
srv_reset_io_thread_op_info();

/** Wake up the purge threads if there is work to do. */
void
srv_wake_purge_thread_if_not_active();
/** Wake up the InnoDB master thread if it was suspended (not sleeping). */
void
srv_active_wake_master_thread_low();

#define srv_active_wake_master_thread()					\
	do {								\
		if (!srv_read_only_mode) {				\
			srv_active_wake_master_thread_low();		\
		}							\
	} while (0)
/** Wake up the master thread if it is suspended or being suspended. */
void
srv_wake_master_thread();

/******************************************************************//**
Outputs to a file the output of the InnoDB Monitor.
@return FALSE if not all information printed
due to failure to obtain necessary mutex */
ibool
srv_printf_innodb_monitor(
/*======================*/
	FILE*	file,		/*!< in: output stream */
	ibool	nowait,		/*!< in: whether to wait for the
				lock_sys_t::mutex */
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
Get current server activity count. We don't hold srv_sys::mutex while
reading this value as it is only used in heuristics.
@return activity count. */
ulint
srv_get_activity_count(void);
/*========================*/
/*******************************************************************//**
Check if there has been any activity.
@return FALSE if no change in activity counter. */
ibool
srv_check_activity(
/*===============*/
	ulint		old_activity_count);	/*!< old activity count */
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

/**********************************************************************//**
Check whether any background thread is active. If so, return the thread
type.
@return SRV_NONE if all are are suspended or have exited, thread
type if any are still active. */
enum srv_thread_type
srv_get_active_thread_type(void);
/*============================*/

extern "C" {

/*********************************************************************//**
A thread which prints the info output by various InnoDB monitors.
@return a dummy parameter */
os_thread_ret_t
DECLARE_THREAD(srv_monitor_thread)(
/*===============================*/
	void*	arg);	/*!< in: a dummy parameter required by
			os_thread_create */

/*********************************************************************//**
The master thread controlling the server.
@return a dummy parameter */
os_thread_ret_t
DECLARE_THREAD(srv_master_thread)(
/*==============================*/
	void*	arg);	/*!< in: a dummy parameter required by
			os_thread_create */

/*************************************************************************
A thread which prints warnings about semaphore waits which have lasted
too long. These can be used to track bugs which cause hangs.
@return a dummy parameter */
os_thread_ret_t
DECLARE_THREAD(srv_error_monitor_thread)(
/*=====================================*/
	void*	arg);	/*!< in: a dummy parameter required by
			os_thread_create */

/*********************************************************************//**
Purge coordinator thread that schedules the purge tasks.
@return a dummy parameter */
os_thread_ret_t
DECLARE_THREAD(srv_purge_coordinator_thread)(
/*=========================================*/
	void*	arg MY_ATTRIBUTE((unused)));	/*!< in: a dummy parameter
						required by os_thread_create */

/*********************************************************************//**
Worker thread that reads tasks from the work queue and executes them.
@return a dummy parameter */
os_thread_ret_t
DECLARE_THREAD(srv_worker_thread)(
/*==============================*/
	void*	arg MY_ATTRIBUTE((unused)));	/*!< in: a dummy parameter
						required by os_thread_create */
} /* extern "C" */

/**********************************************************************//**
Get count of tasks in the queue.
@return number of tasks in queue */
ulint
srv_get_task_queue_length(void);
/*===========================*/

/** Ensure that a given number of threads of the type given are running
(or are already terminated).
@param[in]	type	thread type
@param[in]	n	number of threads that have to run */
void
srv_release_threads(enum srv_thread_type type, ulint n);

/** Wakeup the purge threads. */
void
srv_purge_wakeup();

/** Shut down the purge threads. */
void srv_purge_shutdown();

#ifdef UNIV_DEBUG
/** Disables master thread. It's used by:
	SET GLOBAL innodb_master_thread_disabled_debug = 1 (0).
@param[in]	save		immediate result from check function */
void
srv_master_thread_disabled_debug_update(THD*, st_mysql_sys_var*, void*,
					const void* save);
#endif /* UNIV_DEBUG */

/** Status variables to be passed to MySQL */
struct export_var_t{
	ulint innodb_data_pending_reads;	/*!< Pending reads */
	ulint innodb_data_pending_writes;	/*!< Pending writes */
	ulint innodb_data_pending_fsyncs;	/*!< Pending fsyncs */
	ulint innodb_data_fsyncs;		/*!< Number of fsyncs so far */
	ulint innodb_data_read;			/*!< Data bytes read */
	ulint innodb_data_writes;		/*!< I/O write requests */
	ulint innodb_data_written;		/*!< Data bytes written */
	ulint innodb_data_reads;		/*!< I/O read requests */
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
	ulint innodb_buffer_pool_read_requests;	/*!< buf_pool->stat.n_page_gets */
	ulint innodb_buffer_pool_reads;		/*!< srv_buf_pool_reads */
	ulint innodb_buffer_pool_wait_free;	/*!< srv_buf_pool_wait_free */
	ulint innodb_buffer_pool_pages_flushed;	/*!< srv_buf_pool_flushed */
	ulint innodb_buffer_pool_write_requests;/*!< srv_buf_pool_write_requests */
	ulint innodb_buffer_pool_read_ahead_rnd;/*!< srv_read_ahead_rnd */
	ulint innodb_buffer_pool_read_ahead;	/*!< srv_read_ahead */
	ulint innodb_buffer_pool_read_ahead_evicted;/*!< srv_read_ahead evicted*/
	ulint innodb_dblwr_pages_written;	/*!< srv_dblwr_pages_written */
	ulint innodb_dblwr_writes;		/*!< srv_dblwr_writes */
	ibool innodb_have_atomic_builtins;	/*!< HAVE_ATOMIC_BUILTINS */
	ulint innodb_log_waits;			/*!< srv_log_waits */
	ulint innodb_log_write_requests;	/*!< srv_log_write_requests */
	ulint innodb_log_writes;		/*!< srv_log_writes */
	lsn_t innodb_os_log_written;		/*!< srv_os_log_written */
	ulint innodb_os_log_fsyncs;		/*!< fil_n_log_flushes */
	ulint innodb_os_log_pending_writes;	/*!< srv_os_log_pending_writes */
	ulint innodb_os_log_pending_fsyncs;	/*!< fil_n_pending_log_flushes */
	ulint innodb_page_size;			/*!< srv_page_size */
	ulint innodb_pages_created;		/*!< buf_pool->stat.n_pages_created */
	ulint innodb_pages_read;		/*!< buf_pool->stat.n_pages_read*/
	ulint innodb_pages_written;		/*!< buf_pool->stat.n_pages_written */
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
	ulint innodb_num_open_files;		/*!< fil_system_t::n_open */
	ulint innodb_truncated_status_writes;	/*!< srv_truncated_status_writes */
	ulint innodb_available_undo_logs;       /*!< srv_available_undo_logs
						*/
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
	int64_t innodb_index_pages_written;  /*!< Number of index pages
						written */
	int64_t innodb_non_index_pages_written;  /*!< Number of non index pages
						written */
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
	int64_t innodb_key_rotation_list_length;

	ulint innodb_scrub_page_reorganizations;
	ulint innodb_scrub_page_splits;
	ulint innodb_scrub_page_split_failures_underflow;
	ulint innodb_scrub_page_split_failures_out_of_filespace;
	ulint innodb_scrub_page_split_failures_missing_index;
	ulint innodb_scrub_page_split_failures_unknown;
	int64_t innodb_scrub_log;
};

/** Thread slot in the thread table.  */
struct srv_slot_t{
	srv_thread_type type;			/*!< thread type: user,
						utility etc. */
	ibool		in_use;			/*!< TRUE if this slot
						is in use */
	ibool		suspended;		/*!< TRUE if the thread is
						waiting for the event of this
						slot */
 	/** time(NULL) when the thread was suspended.
 	FIXME: Use my_interval_timer() or similar, to avoid bogus
 	timeouts in lock_wait_check_and_cancel() or lock_wait_suspend_thread()
	when the system time is adjusted to the past!

	FIXME: This is duplicating trx_lock_t::wait_started,
	which is being used for diagnostic purposes only. */
	time_t		suspend_time;
	ulong		wait_timeout;		/*!< wait time that if exceeded
						the thread will be timed out.
						Initialized by
						lock_wait_table_reserve_slot()
						for lock wait */
	os_event_t	event;			/*!< event used in suspending
						the thread when it has nothing
						to do */
	que_thr_t*	thr;			/*!< suspended query thread
						(only used for user threads) */
};

#ifdef UNIV_DEBUG
typedef void srv_slot_callback_t(srv_slot_t*, const void*);

void srv_for_each_thread(srv_thread_type type,
			 srv_slot_callback_t callback,
			 const void *arg);
#endif

#ifdef WITH_WSREP
UNIV_INTERN
void
wsrep_srv_conc_cancel_wait(
/*==================*/
	trx_t*	trx);	/*!< in: transaction object associated with the
			thread */
#endif /* WITH_WSREP */

#endif
