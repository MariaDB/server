/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2008, Google Inc.
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

/********************************************************************//**
@file srv/srv0start.cc
Starts the InnoDB database server

Created 2/16/1996 Heikki Tuuri
*************************************************************************/

#include "my_global.h"

#include "mysqld.h"
#include "mysql/psi/mysql_stage.h"
#include "mysql/psi/psi.h"

#include "row0ftsort.h"
#include "ut0mem.h"
#include "mem0mem.h"
#include "data0data.h"
#include "data0type.h"
#include "dict0dict.h"
#include "buf0buf.h"
#include "buf0dump.h"
#include "os0file.h"
#include "os0thread.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "fsp0fsp.h"
#include "rem0rec.h"
#include "mtr0mtr.h"
#include "log0crypt.h"
#include "log0recv.h"
#include "page0page.h"
#include "page0cur.h"
#include "trx0trx.h"
#include "trx0sys.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "rem0rec.h"
#include "ibuf0ibuf.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "btr0defragment.h"
#include "row0trunc.h"
#include "mysql/service_wsrep.h" /* wsrep_recovery */
#include "trx0rseg.h"
#include "os0proc.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "buf0mtflu.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "dict0stats_bg.h"
#include "que0que.h"
#include "lock0lock.h"
#include "trx0roll.h"
#include "trx0purge.h"
#include "lock0lock.h"
#include "pars0pars.h"
#include "btr0sea.h"
#include "rem0cmp.h"
#include "dict0crea.h"
#include "row0ins.h"
#include "row0sel.h"
#include "row0upd.h"
#include "row0row.h"
#include "row0mysql.h"
#include "row0trunc.h"
#include "btr0pcur.h"
#include "os0event.h"
#include "zlib.h"
#include "ut0crc32.h"
#include "btr0scrub.h"

/** Log sequence number immediately after startup */
lsn_t	srv_start_lsn;
/** Log sequence number at shutdown */
lsn_t	srv_shutdown_lsn;

/** TRUE if a raw partition is in use */
ibool	srv_start_raw_disk_in_use;

/** Number of IO threads to use */
ulint	srv_n_file_io_threads;

/** UNDO tablespaces starts with space id. */
ulint	srv_undo_space_id_start;

/** TRUE if the server is being started, before rolling back any
incomplete transactions */
bool	srv_startup_is_before_trx_rollback_phase;
/** TRUE if the server is being started */
bool	srv_is_being_started;
/** TRUE if SYS_TABLESPACES is available for lookups */
bool	srv_sys_tablespaces_open;
/** TRUE if the server was successfully started */
bool	srv_was_started;
/** The original value of srv_log_file_size (innodb_log_file_size) */
static ulonglong	srv_log_file_size_requested;
/** TRUE if innobase_start_or_create_for_mysql() has been called */
static bool		srv_start_has_been_called;

/** Whether any undo log records can be generated */
UNIV_INTERN bool	srv_undo_sources;

#ifdef UNIV_DEBUG
/** InnoDB system tablespace to set during recovery */
UNIV_INTERN uint	srv_sys_space_size_debug;
/** whether redo log files have been created at startup */
UNIV_INTERN bool	srv_log_files_created;
#endif /* UNIV_DEBUG */

/** Bit flags for tracking background thread creation. They are used to
determine which threads need to be stopped if we need to abort during
the initialisation step. */
enum srv_start_state_t {
	/** No thread started */
	SRV_START_STATE_NONE = 0,		/*!< No thread started */
	/** lock_wait_timeout_thread started */
	SRV_START_STATE_LOCK_SYS = 1,		/*!< Started lock-timeout
						thread. */
	/** buf_flush_page_cleaner_coordinator,
	buf_flush_page_cleaner_worker started */
	SRV_START_STATE_IO = 2,
	/** srv_error_monitor_thread, srv_monitor_thread started */
	SRV_START_STATE_MONITOR = 4,
	/** srv_master_thread started */
	SRV_START_STATE_MASTER = 8,
	/** srv_purge_coordinator_thread, srv_worker_thread started */
	SRV_START_STATE_PURGE = 16,
	/** fil_crypt_thread, btr_defragment_thread started
	(all background threads that can generate redo log but not undo log */
	SRV_START_STATE_REDO = 32
};

/** Track server thrd starting phases */
static ulint	srv_start_state;

/** At a shutdown this value climbs from SRV_SHUTDOWN_NONE to
SRV_SHUTDOWN_CLEANUP and then to SRV_SHUTDOWN_LAST_PHASE, and so on */
enum srv_shutdown_t	srv_shutdown_state = SRV_SHUTDOWN_NONE;

/** Files comprising the system tablespace */
pfs_os_file_t	files[1000];

/** io_handler_thread parameters for thread identification */
static ulint		n[SRV_MAX_N_IO_THREADS + 6];
/** io_handler_thread identifiers, 32 is the maximum number of purge threads  */
/** 6 is the ? */
#define	START_OLD_THREAD_CNT	(SRV_MAX_N_IO_THREADS + 6 + 32)
static os_thread_id_t	thread_ids[SRV_MAX_N_IO_THREADS + 6 + 32 + MTFLUSH_MAX_WORKER];
/* Thread contex data for multi-threaded flush */
void *mtflush_ctx=NULL;

/** Thead handles */
static os_thread_t	thread_handles[SRV_MAX_N_IO_THREADS + 6 + 32];
static os_thread_t	buf_dump_thread_handle;
static os_thread_t	dict_stats_thread_handle;
/** Status variables, is thread started ?*/
static bool		thread_started[SRV_MAX_N_IO_THREADS + 6 + 32] = {false};
/** Name of srv_monitor_file */
static char*	srv_monitor_file_name;

/** Minimum expected tablespace size. (10M) */
static const ulint MIN_EXPECTED_TABLESPACE_SIZE = 5 * 1024 * 1024;

/** */
#define SRV_MAX_N_PENDING_SYNC_IOS	100

#ifdef UNIV_PFS_THREAD
/* Keys to register InnoDB threads with performance schema */
mysql_pfs_key_t	buf_dump_thread_key;
mysql_pfs_key_t	dict_stats_thread_key;
mysql_pfs_key_t	io_handler_thread_key;
mysql_pfs_key_t	io_ibuf_thread_key;
mysql_pfs_key_t	io_log_thread_key;
mysql_pfs_key_t	io_read_thread_key;
mysql_pfs_key_t	io_write_thread_key;
mysql_pfs_key_t	srv_error_monitor_thread_key;
mysql_pfs_key_t	srv_lock_timeout_thread_key;
mysql_pfs_key_t	srv_master_thread_key;
mysql_pfs_key_t	srv_monitor_thread_key;
mysql_pfs_key_t	srv_purge_thread_key;
mysql_pfs_key_t	srv_worker_thread_key;
#endif /* UNIV_PFS_THREAD */

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Array of all InnoDB stage events for monitoring activities via
performance schema. */
static PSI_stage_info*	srv_stages[] =
{
	&srv_stage_alter_table_end,
	&srv_stage_alter_table_flush,
	&srv_stage_alter_table_insert,
	&srv_stage_alter_table_log_index,
	&srv_stage_alter_table_log_table,
	&srv_stage_alter_table_merge_sort,
	&srv_stage_alter_table_read_pk_internal_sort,
	&srv_stage_buffer_pool_load,
};
#endif /* HAVE_PSI_STAGE_INTERFACE */

/*********************************************************************//**
Check if a file can be opened in read-write mode.
@return true if it doesn't exist or can be opened in rw mode. */
static
bool
srv_file_check_mode(
/*================*/
	const char*	name)		/*!< in: filename to check */
{
	os_file_stat_t	stat;

	memset(&stat, 0x0, sizeof(stat));

	dberr_t		err = os_file_get_status(
		name, &stat, true, srv_read_only_mode);

	if (err == DB_FAIL) {
		ib::error() << "os_file_get_status() failed on '" << name
			<< "'. Can't determine file permissions.";
		return(false);

	} else if (err == DB_SUCCESS) {

		/* Note: stat.rw_perm is only valid of files */

		if (stat.type == OS_FILE_TYPE_FILE) {

			if (!stat.rw_perm) {
				const char*	mode = srv_read_only_mode
					? "read" : "read-write";
				ib::error() << name << " can't be opened in "
					<< mode << " mode.";
				return(false);
			}
		} else {
			/* Not a regular file, bail out. */
			ib::error() << "'" << name << "' not a regular file.";

			return(false);
		}
	} else {

		/* This is OK. If the file create fails on RO media, there
		is nothing we can do. */

		ut_a(err == DB_NOT_FOUND);
	}

	return(true);
}

/********************************************************************//**
I/o-handler thread function.
@return OS_THREAD_DUMMY_RETURN */
extern "C"
os_thread_ret_t
DECLARE_THREAD(io_handler_thread)(
/*==============================*/
	void*	arg)	/*!< in: pointer to the number of the segment in
			the aio array */
{
	ulint	segment;

	segment = *((ulint*) arg);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Io handler thread " << segment << " starts, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif

	/* For read only mode, we don't need ibuf and log I/O thread.
	Please see innobase_start_or_create_for_mysql() */
	ulint   start = (srv_read_only_mode) ? 0 : 2;

	if (segment < start) {
		if (segment == 0) {
			pfs_register_thread(io_ibuf_thread_key);
		} else {
			ut_ad(segment == 1);
			pfs_register_thread(io_log_thread_key);
		}
	} else if (segment >= start
		   && segment < (start + srv_n_read_io_threads)) {
			pfs_register_thread(io_read_thread_key);

	} else if (segment >= (start + srv_n_read_io_threads)
		   && segment < (start + srv_n_read_io_threads
				 + srv_n_write_io_threads)) {
		pfs_register_thread(io_write_thread_key);

	} else {
		pfs_register_thread(io_handler_thread_key);
	}

	while (srv_shutdown_state != SRV_SHUTDOWN_EXIT_THREADS
	       || buf_page_cleaner_is_active
	       || !os_aio_all_slots_free()) {
		fil_aio_wait(segment);
	}

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit.
	The thread actually never comes here because it is exited in an
	os_event_wait(). */

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/*********************************************************************//**
Creates a log file.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
create_log_file(
/*============*/
	pfs_os_file_t*	file,	/*!< out: file handle */
	const char*	name)	/*!< in: log file name */
{
	bool		ret;

	*file = os_file_create(
		innodb_log_file_key, name,
		OS_FILE_CREATE|OS_FILE_ON_ERROR_NO_EXIT, OS_FILE_NORMAL,
		OS_LOG_FILE, srv_read_only_mode, &ret);

	if (!ret) {
		ib::error() << "Cannot create " << name;
		return(DB_ERROR);
	}

	ib::info() << "Setting log file " << name << " size to "
		<< srv_log_file_size << " bytes";

	ret = os_file_set_size(name, *file, srv_log_file_size);
	if (!ret) {
		ib::error() << "Cannot set log file " << name << " size to "
			<< srv_log_file_size << " bytes";
		return(DB_ERROR);
	}

	ret = os_file_close(*file);
	ut_a(ret);

	return(DB_SUCCESS);
}

/** Initial number of the first redo log file */
#define INIT_LOG_FILE0	(SRV_N_LOG_FILES_MAX + 1)

/** Delete all log files.
@param[in,out]	logfilename	buffer for log file name
@param[in]	dirnamelen	length of the directory path
@param[in]	n_files		number of files to delete
@param[in]	i		first file to delete */
static
void
delete_log_files(char* logfilename, size_t dirnamelen, uint n_files, uint i=0)
{
	/* Remove any old log files. */
	for (; i < n_files; i++) {
		sprintf(logfilename + dirnamelen, "ib_logfile%u", i);

		/* Ignore errors about non-existent files or files
		that cannot be removed. The create_log_file() will
		return an error when the file exists. */
#ifdef _WIN32
		DeleteFile((LPCTSTR) logfilename);
#else
		unlink(logfilename);
#endif
	}
}

/*********************************************************************//**
Creates all log files.
@return DB_SUCCESS or error code */
static
dberr_t
create_log_files(
/*=============*/
	char*	logfilename,	/*!< in/out: buffer for log file name */
	size_t	dirnamelen,	/*!< in: length of the directory path */
	lsn_t	lsn,		/*!< in: FIL_PAGE_FILE_FLUSH_LSN value */
	char*&	logfile0)	/*!< out: name of the first log file */
{
	dberr_t err;

	if (srv_read_only_mode) {
		ib::error() << "Cannot create log files in read-only mode";
		return(DB_READ_ONLY);
	}

	if (!log_set_capacity(srv_log_file_size_requested)) {
		return(DB_ERROR);
	}

	/* Crashing after deleting the first file should be
	recoverable. The buffer pool was clean, and we can simply
	create all log files from the scratch. */
	DBUG_EXECUTE_IF("innodb_log_abort_6",
			delete_log_files(logfilename, dirnamelen, 1);
			return(DB_ERROR););

	delete_log_files(logfilename, dirnamelen, INIT_LOG_FILE0 + 1);

	DBUG_PRINT("ib_log", ("After innodb_log_abort_6"));
	ut_ad(!buf_pool_check_no_pending_io());

	DBUG_EXECUTE_IF("innodb_log_abort_7", return(DB_ERROR););
	DBUG_PRINT("ib_log", ("After innodb_log_abort_7"));

	for (unsigned i = 0; i < srv_n_log_files; i++) {
		sprintf(logfilename + dirnamelen,
			"ib_logfile%u", i ? i : INIT_LOG_FILE0);

		err = create_log_file(&files[i], logfilename);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	DBUG_EXECUTE_IF("innodb_log_abort_8", return(DB_ERROR););
	DBUG_PRINT("ib_log", ("After innodb_log_abort_8"));

	/* We did not create the first log file initially as
	ib_logfile0, so that crash recovery cannot find it until it
	has been completed and renamed. */
	sprintf(logfilename + dirnamelen, "ib_logfile%u", INIT_LOG_FILE0);

	fil_space_t*	log_space = fil_space_create(
		"innodb_redo_log", SRV_LOG_SPACE_FIRST_ID, 0, FIL_TYPE_LOG,
		NULL/* innodb_encrypt_log works at a different level */);

	ut_a(fil_validate());
	ut_a(log_space != NULL);

	const ulint size = ulint(srv_log_file_size >> srv_page_size_shift);

	logfile0 = log_space->add(logfilename, OS_FILE_CLOSED, size,
				  false, false)->name;
	ut_a(logfile0);

	for (unsigned i = 1; i < srv_n_log_files; i++) {

		sprintf(logfilename + dirnamelen, "ib_logfile%u", i);

		log_space->add(logfilename, OS_FILE_CLOSED, size,
			       false, false);
	}

	log_init(srv_n_log_files);

	fil_open_log_and_system_tablespace_files();

	/* Create a log checkpoint. */
	log_mutex_enter();
	if (log_sys->is_encrypted() && !log_crypt_init()) {
		return DB_ERROR;
	}
	ut_d(recv_no_log_write = false);
	log_sys->lsn = ut_uint64_align_up(lsn, OS_FILE_LOG_BLOCK_SIZE);

	log_sys->log.lsn = log_sys->lsn;
	log_sys->log.lsn_offset = LOG_FILE_HDR_SIZE;

	log_sys->buf_next_to_write = 0;
	log_sys->write_lsn = log_sys->lsn;

	log_sys->next_checkpoint_no = 0;
	log_sys->last_checkpoint_lsn = 0;

	memset(log_sys->buf, 0, log_sys->buf_size);
	log_block_init(log_sys->buf, log_sys->lsn);
	log_block_set_first_rec_group(log_sys->buf, LOG_BLOCK_HDR_SIZE);

	log_sys->buf_free = LOG_BLOCK_HDR_SIZE;
	log_sys->lsn += LOG_BLOCK_HDR_SIZE;

	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    (log_sys->lsn - log_sys->last_checkpoint_lsn));
	log_mutex_exit();

	log_make_checkpoint();

	return(DB_SUCCESS);
}

/** Rename the first redo log file.
@param[in,out]	logfilename	buffer for the log file name
@param[in]	dirnamelen	length of the directory path
@param[in]	lsn		FIL_PAGE_FILE_FLUSH_LSN value
@param[in,out]	logfile0	name of the first log file
@return	error code
@retval	DB_SUCCESS	on successful operation */
MY_ATTRIBUTE((warn_unused_result, nonnull))
static
dberr_t
create_log_files_rename(
/*====================*/
	char*	logfilename,	/*!< in/out: buffer for log file name */
	size_t	dirnamelen,	/*!< in: length of the directory path */
	lsn_t	lsn,		/*!< in: FIL_PAGE_FILE_FLUSH_LSN value */
	char*	logfile0)	/*!< in/out: name of the first log file */
{
	/* If innodb_flush_method=O_DSYNC,
	we need to explicitly flush the log buffers. */
	fil_flush(SRV_LOG_SPACE_FIRST_ID);

	ut_ad(!srv_log_files_created);
	ut_d(srv_log_files_created = true);

	DBUG_EXECUTE_IF("innodb_log_abort_9", return(DB_ERROR););
	DBUG_PRINT("ib_log", ("After innodb_log_abort_9"));

	/* Close the log files, so that we can rename
	the first one. */
	fil_close_log_files(false);

	/* Rename the first log file, now that a log
	checkpoint has been created. */
	sprintf(logfilename + dirnamelen, "ib_logfile%u", 0);

	ib::info() << "Renaming log file " << logfile0 << " to "
		<< logfilename;

	log_mutex_enter();
	ut_ad(strlen(logfile0) == 2 + strlen(logfilename));
	dberr_t err = os_file_rename(
		innodb_log_file_key, logfile0, logfilename)
		? DB_SUCCESS : DB_ERROR;

	/* Replace the first file with ib_logfile0. */
	strcpy(logfile0, logfilename);
	log_mutex_exit();

	DBUG_EXECUTE_IF("innodb_log_abort_10", err = DB_ERROR;);

	if (err == DB_SUCCESS) {
		fil_open_log_and_system_tablespace_files();
		ib::info() << "New log files created, LSN=" << lsn;
	}

	return(err);
}

/*********************************************************************//**
Create undo tablespace.
@return DB_SUCCESS or error code */
static
dberr_t
srv_undo_tablespace_create(
/*=======================*/
	const char*	name,		/*!< in: tablespace name */
	ulint		size)		/*!< in: tablespace size in pages */
{
	pfs_os_file_t	fh;
	bool		ret;
	dberr_t		err = DB_SUCCESS;

	os_file_create_subdirs_if_needed(name);

	fh = os_file_create(
		innodb_data_file_key,
		name,
		srv_read_only_mode ? OS_FILE_OPEN : OS_FILE_CREATE,
		OS_FILE_NORMAL, OS_DATA_FILE, srv_read_only_mode, &ret);

	if (srv_read_only_mode && ret) {

		ib::info() << name << " opened in read-only mode";

	} else if (ret == FALSE) {
		if (os_file_get_last_error(false) != OS_FILE_ALREADY_EXISTS
#ifdef UNIV_AIX
			/* AIX 5.1 after security patch ML7 may have
			errno set to 0 here, which causes our function
			to return 100; work around that AIX problem */
		    && os_file_get_last_error(false) != 100
#endif /* UNIV_AIX */
		) {
			ib::error() << "Can't create UNDO tablespace "
				<< name;
		}
		err = DB_ERROR;
	} else {
		ut_a(!srv_read_only_mode);

		/* We created the data file and now write it full of zeros */

		ib::info() << "Data file " << name << " did not exist: new to"
			" be created";

		ib::info() << "Setting file " << name << " size to "
			<< (size >> (20 - UNIV_PAGE_SIZE_SHIFT)) << " MB";

		ib::info() << "Database physically writes the file full: "
			<< "wait...";

		ret = os_file_set_size(
			name, fh, os_offset_t(size) << UNIV_PAGE_SIZE_SHIFT);

		if (!ret) {
			ib::info() << "Error in creating " << name
				<< ": probably out of disk space";

			err = DB_ERROR;
		}

		os_file_close(fh);
	}

	return(err);
}

/** Open an undo tablespace.
@param[in]	name		tablespace file name
@param[in]	space_id	tablespace ID
@param[in]	create_new_db	whether undo tablespaces are being created
@return whether the tablespace was opened */
static bool srv_undo_tablespace_open(const char* name, ulint space_id,
				     bool create_new_db)
{
	pfs_os_file_t	fh;
	bool		success;
	char		undo_name[sizeof "innodb_undo000"];

	snprintf(undo_name, sizeof(undo_name),
		 "innodb_undo%03u", static_cast<unsigned>(space_id));

	fh = os_file_create(
		innodb_data_file_key, name, OS_FILE_OPEN
		| OS_FILE_ON_ERROR_NO_EXIT | OS_FILE_ON_ERROR_SILENT,
		OS_FILE_AIO, OS_DATA_FILE, srv_read_only_mode, &success);
	if (!success) {
		return false;
	}

	os_offset_t size = os_file_get_size(fh);
	ut_a(size != os_offset_t(-1));

	/* Load the tablespace into InnoDB's internal data structures. */

	/* We set the biggest space id to the undo tablespace
	because InnoDB hasn't opened any other tablespace apart
	from the system tablespace. */

	fil_set_max_space_id_if_bigger(space_id);

	fil_space_t* space = fil_space_create(
		undo_name, space_id, FSP_FLAGS_PAGE_SSIZE(),
		FIL_TYPE_TABLESPACE, NULL);

	ut_a(fil_validate());
	ut_a(space);

	fil_node_t* file = space->add(name, fh, 0, false, true);

	mutex_enter(&fil_system->mutex);

	if (create_new_db) {
		space->size = file->size = ulint(size >> srv_page_size_shift);
		space->size_in_header = SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;
		space->committed_size = SRV_UNDO_TABLESPACE_SIZE_IN_PAGES;
	} else {
		success = file->read_page0(true);
		if (!success) {
			os_file_close(file->handle);
			file->handle = OS_FILE_CLOSED;
			ut_a(fil_system->n_open > 0);
			fil_system->n_open--;
		}
	}

	mutex_exit(&fil_system->mutex);

	return success;
}

/** Check if undo tablespaces and redo log files exist before creating a
new system tablespace
@retval DB_SUCCESS  if all undo and redo logs are not found
@retval DB_ERROR    if any undo and redo logs are found */
static
dberr_t
srv_check_undo_redo_logs_exists()
{
	bool		ret;
	os_file_t	fh;
	char	name[OS_FILE_MAX_PATH];

	/* Check if any undo tablespaces exist */
	for (ulint i = 1; i <= srv_undo_tablespaces; ++i) {

		snprintf(
			name, sizeof(name),
			"%s%cundo%03zu",
			srv_undo_dir, OS_PATH_SEPARATOR,
			i);

		fh = os_file_create(
			innodb_data_file_key, name,
			OS_FILE_OPEN_RETRY
			| OS_FILE_ON_ERROR_NO_EXIT
			| OS_FILE_ON_ERROR_SILENT,
			OS_FILE_NORMAL,
			OS_DATA_FILE,
			srv_read_only_mode,
			&ret);

		if (ret) {
			os_file_close(fh);
			ib::error()
				<< "undo tablespace '" << name << "' exists."
				" Creating system tablespace with existing undo"
				" tablespaces is not supported. Please delete"
				" all undo tablespaces before creating new"
				" system tablespace.";
			return(DB_ERROR);
		}
	}

	/* Check if any redo log files exist */
	char	logfilename[OS_FILE_MAX_PATH];
	size_t dirnamelen = strlen(srv_log_group_home_dir);
	memcpy(logfilename, srv_log_group_home_dir, dirnamelen);

	for (unsigned i = 0; i < srv_n_log_files; i++) {
		sprintf(logfilename + dirnamelen,
			"ib_logfile%u", i);

		fh = os_file_create(
			innodb_log_file_key, logfilename,
			OS_FILE_OPEN_RETRY
			| OS_FILE_ON_ERROR_NO_EXIT
			| OS_FILE_ON_ERROR_SILENT,
			OS_FILE_NORMAL,
			OS_LOG_FILE,
			srv_read_only_mode,
			&ret);

		if (ret) {
			os_file_close(fh);
			ib::error() << "redo log file '" << logfilename
				<< "' exists. Creating system tablespace with"
				" existing redo log files is not recommended."
				" Please delete all redo log files before"
				" creating new system tablespace.";
			return(DB_ERROR);
		}
	}

	return(DB_SUCCESS);
}

undo::undo_spaces_t	undo::Truncate::s_fix_up_spaces;

/** Open the configured number of dedicated undo tablespaces.
@param[in]	create_new_db	whether the database is being initialized
@return DB_SUCCESS or error code */
dberr_t
srv_undo_tablespaces_init(bool create_new_db)
{
	ulint			i;
	dberr_t			err = DB_SUCCESS;
	ulint			prev_space_id = 0;
	ulint			n_undo_tablespaces;
	ulint			undo_tablespace_ids[TRX_SYS_N_RSEGS + 1];

	srv_undo_tablespaces_open = 0;

	ut_a(srv_undo_tablespaces <= TRX_SYS_N_RSEGS);
	ut_a(!create_new_db || srv_operation == SRV_OPERATION_NORMAL);

	if (srv_undo_tablespaces == 1) { /* 1 is not allowed, make it 0 */
		srv_undo_tablespaces = 0;
	}

	memset(undo_tablespace_ids, 0x0, sizeof(undo_tablespace_ids));

	/* Create the undo spaces only if we are creating a new
	instance. We don't allow creating of new undo tablespaces
	in an existing instance (yet).  This restriction exists because
	we check in several places for SYSTEM tablespaces to be less than
	the min of user defined tablespace ids. Once we implement saving
	the location of the undo tablespaces and their space ids this
	restriction will/should be lifted. */

	for (i = 0; create_new_db && i < srv_undo_tablespaces; ++i) {
		char	name[OS_FILE_MAX_PATH];
		ulint	space_id  = i + 1;

		DBUG_EXECUTE_IF("innodb_undo_upgrade",
				space_id = i + 3;);

		snprintf(
			name, sizeof(name),
			"%s%cundo%03zu",
			srv_undo_dir, OS_PATH_SEPARATOR, space_id);

		if (i == 0) {
			srv_undo_space_id_start = space_id;
			prev_space_id = srv_undo_space_id_start - 1;
		}

		undo_tablespace_ids[i] = space_id;

		err = srv_undo_tablespace_create(
			name, SRV_UNDO_TABLESPACE_SIZE_IN_PAGES);

		if (err != DB_SUCCESS) {
			ib::error() << "Could not create undo tablespace '"
				<< name << "'.";
			return(err);
		}
	}

	/* Get the tablespace ids of all the undo segments excluding
	the system tablespace (0). If we are creating a new instance then
	we build the undo_tablespace_ids ourselves since they don't
	already exist. */
	n_undo_tablespaces = create_new_db
		|| srv_operation == SRV_OPERATION_BACKUP
		|| srv_operation == SRV_OPERATION_RESTORE_DELTA
		? srv_undo_tablespaces
		: trx_rseg_get_n_undo_tablespaces(undo_tablespace_ids);
	srv_undo_tablespaces_active = srv_undo_tablespaces;

	switch (srv_operation) {
	case SRV_OPERATION_RESTORE_DELTA:
	case SRV_OPERATION_BACKUP:
		for (i = 0; i < n_undo_tablespaces; i++) {
			undo_tablespace_ids[i] = i + srv_undo_space_id_start;
		}

		prev_space_id = srv_undo_space_id_start - 1;
		break;
	case SRV_OPERATION_NORMAL:
		if (create_new_db) {
			break;
		}
		/* fall through */
	case SRV_OPERATION_RESTORE_ROLLBACK_XA:
	case SRV_OPERATION_RESTORE:
	case SRV_OPERATION_RESTORE_EXPORT:
		ut_ad(!create_new_db);

		/* Check if any of the UNDO tablespace needs fix-up because
		server crashed while truncate was active on UNDO tablespace.*/
		for (i = 0; i < n_undo_tablespaces; ++i) {

			undo::Truncate	undo_trunc;

			if (undo_trunc.needs_fix_up(undo_tablespace_ids[i])) {

				char	name[OS_FILE_MAX_PATH];

				snprintf(name, sizeof(name),
					 "%s%cundo%03zu",
					 srv_undo_dir, OS_PATH_SEPARATOR,
					 undo_tablespace_ids[i]);

				os_file_delete(innodb_data_file_key, name);

				err = srv_undo_tablespace_create(
					name,
					SRV_UNDO_TABLESPACE_SIZE_IN_PAGES);

				if (err != DB_SUCCESS) {
					ib::error() << "Could not fix-up undo "
						" tablespace truncate '"
						<< name << "'.";
					return(err);
				}

				undo::Truncate::s_fix_up_spaces.push_back(
					undo_tablespace_ids[i]);
			}
		}
		break;
	}

	/* Open all the undo tablespaces that are currently in use. If we
	fail to open any of these it is a fatal error. The tablespace ids
	should be contiguous. It is a fatal error because they are required
	for recovery and are referenced by the UNDO logs (a.k.a RBS). */

	for (i = 0; i < n_undo_tablespaces; ++i) {
		char	name[OS_FILE_MAX_PATH];

		snprintf(
			name, sizeof(name),
			"%s%cundo%03zu",
			srv_undo_dir, OS_PATH_SEPARATOR,
			undo_tablespace_ids[i]);

		/* Should be no gaps in undo tablespace ids. */
		ut_a(!i || prev_space_id + 1 == undo_tablespace_ids[i]);

		/* The system space id should not be in this array. */
		ut_a(undo_tablespace_ids[i] != 0);
		ut_a(undo_tablespace_ids[i] != ULINT_UNDEFINED);

		if (!srv_undo_tablespace_open(name, undo_tablespace_ids[i],
					      create_new_db)) {
			ib::error() << "Unable to open undo tablespace '"
				<< name << "'.";
			return DB_ERROR;
		}

		prev_space_id = undo_tablespace_ids[i];

		/* Note the first undo tablespace id in case of
		no active undo tablespace. */
		if (0 == srv_undo_tablespaces_open++) {
			srv_undo_space_id_start = undo_tablespace_ids[i];
		}
	}

	/* Open any extra unused undo tablespaces. These must be contiguous.
	We stop at the first failure. These are undo tablespaces that are
	not in use and therefore not required by recovery. We only check
	that there are no gaps. */

	for (i = prev_space_id + 1;
	     i < srv_undo_space_id_start + TRX_SYS_N_RSEGS; ++i) {
		char	name[OS_FILE_MAX_PATH];

		snprintf(
			name, sizeof(name),
			"%s%cundo%03zu", srv_undo_dir, OS_PATH_SEPARATOR, i);

		if (!srv_undo_tablespace_open(name, i, create_new_db)) {
			err = DB_ERROR;
			break;
		}

		++n_undo_tablespaces;

		++srv_undo_tablespaces_open;
	}

	/* Initialize srv_undo_space_id_start=0 when there are no
	dedicated undo tablespaces. */
	if (n_undo_tablespaces == 0) {
		srv_undo_space_id_start = 0;
	}

	/* If the user says that there are fewer than what we find we
	tolerate that discrepancy but not the inverse. Because there could
	be unused undo tablespaces for future use. */

	if (srv_undo_tablespaces > n_undo_tablespaces) {
		ib::error() << "Expected to open innodb_undo_tablespaces="
			<< srv_undo_tablespaces
			<< " but was able to find only "
			<< n_undo_tablespaces;

		return(err != DB_SUCCESS ? err : DB_ERROR);

	} else if (n_undo_tablespaces > 0) {

		ib::info() << "Opened " << n_undo_tablespaces
			<< " undo tablespaces";

		if (srv_undo_tablespaces == 0) {
			ib::warn() << "innodb_undo_tablespaces=0 disables"
				" dedicated undo log tablespaces";
		}
	}

	if (create_new_db) {
		mtr_t	mtr;

		mtr_start(&mtr);

		/* The undo log tablespace */
		for (i = 0; i < n_undo_tablespaces; ++i) {

			fsp_header_init(
				undo_tablespace_ids[i],
				SRV_UNDO_TABLESPACE_SIZE_IN_PAGES, &mtr);
		}

		mtr_commit(&mtr);
	}

	if (!undo::Truncate::s_fix_up_spaces.empty()) {

		/* Step-1: Initialize the tablespace header and rsegs header. */
		mtr_t		mtr;
		trx_sysf_t*	sys_header;

		mtr_start(&mtr);
		/* Turn off REDO logging. We are in server start mode and fixing
		UNDO tablespace even before REDO log is read. Let's say we
		do REDO logging here then this REDO log record will be applied
		as part of the current recovery process. We surely don't need
		that as this is fix-up action parallel to REDO logging. */
		mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
		sys_header = trx_sysf_get(&mtr);

		for (undo::undo_spaces_t::const_iterator it
			     = undo::Truncate::s_fix_up_spaces.begin();
		     it != undo::Truncate::s_fix_up_spaces.end();
		     ++it) {

			undo::Truncate::add_space_to_trunc_list(*it);

			fsp_header_init(
				*it, SRV_UNDO_TABLESPACE_SIZE_IN_PAGES, &mtr);

			mtr_x_lock_space(*it, &mtr);

			for (ulint i = 0; i < TRX_SYS_N_RSEGS; i++) {

				ulint	space_id = trx_sysf_rseg_get_space(
						sys_header, i, &mtr);

				if (space_id == *it) {
					trx_rseg_header_create(
						*it, ULINT_MAX, i, &mtr);
				}
			}

			undo::Truncate::clear_trunc_list();
		}
		mtr_commit(&mtr);

		/* Step-2: Flush the dirty pages from the buffer pool. */
		for (undo::undo_spaces_t::const_iterator it
			     = undo::Truncate::s_fix_up_spaces.begin();
		     it != undo::Truncate::s_fix_up_spaces.end();
		     ++it) {
			FlushObserver dummy(TRX_SYS_SPACE, NULL, NULL);
			buf_LRU_flush_or_remove_pages(TRX_SYS_SPACE, &dummy);
			FlushObserver dummy2(*it, NULL, NULL);
			buf_LRU_flush_or_remove_pages(*it, &dummy2);

			/* Remove the truncate redo log file. */
			undo::done(*it);
		}
	}

	return(DB_SUCCESS);
}

/********************************************************************
Wait for the purge thread(s) to start up. */
static
void
srv_start_wait_for_purge_to_start()
/*===============================*/
{
	/* Wait for the purge coordinator and master thread to startup. */

	purge_state_t	state = trx_purge_state();

	ut_a(state != PURGE_STATE_DISABLED);

	while (srv_shutdown_state <= SRV_SHUTDOWN_INITIATED
	       && srv_force_recovery < SRV_FORCE_NO_BACKGROUND
	       && state == PURGE_STATE_INIT) {

		switch (state = trx_purge_state()) {
		case PURGE_STATE_RUN:
		case PURGE_STATE_STOP:
			break;

		case PURGE_STATE_INIT:
			ib::info() << "Waiting for purge to start";

			os_thread_sleep(50000);
			break;

		case PURGE_STATE_EXIT:
		case PURGE_STATE_DISABLED:
			ut_error;
		}
	}
}

/** Create the temporary file tablespace.
@param[in]	create_new_db	whether we are creating a new database
@return DB_SUCCESS or error code. */
static
dberr_t
srv_open_tmp_tablespace(bool create_new_db)
{
	ulint	sum_of_new_sizes;

	/* Will try to remove if there is existing file left-over by last
	unclean shutdown */
	srv_tmp_space.set_sanity_check_status(true);
	srv_tmp_space.delete_files();
	srv_tmp_space.set_ignore_read_only(true);

	ib::info() << "Creating shared tablespace for temporary tables";

	bool	create_new_temp_space;

	srv_tmp_space.set_space_id(SRV_TMP_SPACE_ID);

	dberr_t	err = srv_tmp_space.check_file_spec(
		&create_new_temp_space, 12 * 1024 * 1024);

	if (err == DB_FAIL) {

		ib::error() << "The " << srv_tmp_space.name()
			<< " data file must be writable!";

		err = DB_ERROR;

	} else if (err != DB_SUCCESS) {
		ib::error() << "Could not create the shared "
			<< srv_tmp_space.name() << ".";

	} else if ((err = srv_tmp_space.open_or_create(
			    true, create_new_db, &sum_of_new_sizes, NULL))
		   != DB_SUCCESS) {

		ib::error() << "Unable to create the shared "
			<< srv_tmp_space.name();

	} else {

		mtr_t	mtr;
		ulint	size = srv_tmp_space.get_sum_of_sizes();

		/* Open this shared temp tablespace in the fil_system so that
		it stays open until shutdown. */
		if (fil_space_open(srv_tmp_space.name())) {

			/* Initialize the header page */
			mtr_start(&mtr);
			mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

			fsp_header_init(SRV_TMP_SPACE_ID, size, &mtr);

			mtr_commit(&mtr);
		} else {
			/* This file was just opened in the code above! */
			ib::error() << "The " << srv_tmp_space.name()
				<< " data file cannot be re-opened"
				" after check_file_spec() succeeded!";

			err = DB_ERROR;
		}
	}

	return(err);
}

/****************************************************************//**
Set state to indicate start of particular group of threads in InnoDB. */
UNIV_INLINE
void
srv_start_state_set(
/*================*/
	srv_start_state_t state)	/*!< in: indicate current state of
					thread startup */
{
	srv_start_state |= state;
}

/****************************************************************//**
Check if following group of threads is started.
@return true if started */
UNIV_INLINE
bool
srv_start_state_is_set(
/*===================*/
	srv_start_state_t state)	/*!< in: state to check for */
{
	return(srv_start_state & state);
}

/**
Shutdown all background threads created by InnoDB. */
static
void
srv_shutdown_all_bg_threads()
{
	ut_ad(!srv_undo_sources);
	srv_shutdown_state = SRV_SHUTDOWN_EXIT_THREADS;

	/* All threads end up waiting for certain events. Put those events
	to the signaled state. Then the threads will exit themselves after
	os_event_wait(). */
	for (uint i = 0; i < 1000; ++i) {
		/* NOTE: IF YOU CREATE THREADS IN INNODB, YOU MUST EXIT THEM
		HERE OR EARLIER */

		if (srv_start_state_is_set(SRV_START_STATE_LOCK_SYS)) {
			/* a. Let the lock timeout thread exit */
			os_event_set(lock_sys->timeout_event);
		}

		if (!srv_read_only_mode) {
			/* b. srv error monitor thread exits automatically,
			no need to do anything here */

			if (srv_start_state_is_set(SRV_START_STATE_MASTER)) {
				/* c. We wake the master thread so that
				it exits */
				srv_wake_master_thread();
			}

			if (srv_start_state_is_set(SRV_START_STATE_PURGE)) {
				/* d. Wakeup purge threads. */
				srv_purge_wakeup();
			}

			if (srv_n_fil_crypt_threads_started) {
				os_event_set(fil_crypt_threads_event);
			}

			if (log_scrub_thread_active) {
				os_event_set(log_scrub_event);
			}
		}

		if (srv_start_state_is_set(SRV_START_STATE_IO)) {
			ut_ad(!srv_read_only_mode);

			/* e. Exit the i/o threads */
			if (recv_sys->flush_start != NULL) {
				os_event_set(recv_sys->flush_start);
			}
			if (recv_sys->flush_end != NULL) {
				os_event_set(recv_sys->flush_end);
			}

			os_event_set(buf_flush_event);

			if (srv_use_mtflush) {
				buf_mtflu_io_thread_exit();
			}
		}

		if (!os_thread_count) {
			return;
		}

		switch (srv_operation) {
		case SRV_OPERATION_BACKUP:
		case SRV_OPERATION_RESTORE_DELTA:
			break;
		case SRV_OPERATION_NORMAL:
		case SRV_OPERATION_RESTORE_ROLLBACK_XA:
		case SRV_OPERATION_RESTORE:
		case SRV_OPERATION_RESTORE_EXPORT:
			if (!buf_page_cleaner_is_active
			    && os_aio_all_slots_free()) {
				os_aio_wake_all_threads_at_shutdown();
			}
		}

		os_thread_sleep(100000);
	}

	ib::warn() << os_thread_count << " threads created by InnoDB"
		" had not exited at shutdown!";
	ut_d(os_aio_print_pending_io(stderr));
	ut_ad(0);
}

#ifdef UNIV_DEBUG
# define srv_init_abort(_db_err)	\
	srv_init_abort_low(create_new_db, __FILE__, __LINE__, _db_err)
#else
# define srv_init_abort(_db_err)	\
	srv_init_abort_low(create_new_db, _db_err)
#endif /* UNIV_DEBUG */

/** Innobase start-up aborted. Perform cleanup actions.
@param[in]	create_new_db	TRUE if new db is  being created
@param[in]	file		File name
@param[in]	line		Line number
@param[in]	err		Reason for aborting InnoDB startup
@return DB_SUCCESS or error code. */
MY_ATTRIBUTE((warn_unused_result, nonnull))
static
dberr_t
srv_init_abort_low(
	bool		create_new_db,
#ifdef UNIV_DEBUG
	const char*	file,
	unsigned	line,
#endif /* UNIV_DEBUG */
	dberr_t		err)
{
	if (create_new_db) {
		ib::error() << "Database creation was aborted"
#ifdef UNIV_DEBUG
			" at " << innobase_basename(file) << "[" << line << "]"
#endif /* UNIV_DEBUG */
			" with error " << err << ". You may need"
			" to delete the ibdata1 file before trying to start"
			" up again.";
	} else {
		ib::error() << "Plugin initialization aborted"
#ifdef UNIV_DEBUG
			" at " << innobase_basename(file) << "[" << line << "]"
#endif /* UNIV_DEBUG */
			" with error " << err;
	}

	srv_shutdown_all_bg_threads();
	return(err);
}

/** Prepare to delete the redo log files. Flush the dirty pages from all the
buffer pools.  Flush the redo log buffer to the redo log file.
@param[in]	n_files		number of old redo log files
@return lsn upto which data pages have been flushed. */
static
lsn_t
srv_prepare_to_delete_redo_log_files(
	ulint	n_files)
{
	DBUG_ENTER("srv_prepare_to_delete_redo_log_files");

	lsn_t	flushed_lsn;
	ulint	pending_io = 0;
	ulint	count = 0;

	if (srv_safe_truncate) {
		if ((log_sys->log.format & ~LOG_HEADER_FORMAT_ENCRYPTED)
		    != LOG_HEADER_FORMAT_10_3
		    || log_sys->log.subformat != 1) {
			srv_log_file_size = 0;
		}
	} else {
		if ((log_sys->log.format & ~LOG_HEADER_FORMAT_ENCRYPTED)
		    != LOG_HEADER_FORMAT_10_2) {
			srv_log_file_size = 0;
		}
	}

	do {
		/* Clean the buffer pool. */
		buf_flush_sync_all_buf_pools();

		DBUG_EXECUTE_IF("innodb_log_abort_1", DBUG_RETURN(0););
		DBUG_PRINT("ib_log", ("After innodb_log_abort_1"));

		log_mutex_enter();

		fil_names_clear(log_sys->lsn, false);

		flushed_lsn = log_sys->lsn;

		{
			ib::info	info;
			if (srv_log_file_size == 0) {
				info << ((log_sys->log.format
					  & ~LOG_HEADER_FORMAT_ENCRYPTED)
					 < LOG_HEADER_FORMAT_10_3
					 ? "Upgrading redo log: "
					 : "Downgrading redo log: ");
			} else if (n_files != srv_n_log_files
				   || srv_log_file_size
				   != srv_log_file_size_requested) {
				if (srv_encrypt_log
				    == log_sys->is_encrypted()) {
					info << (srv_encrypt_log
						 ? "Resizing encrypted"
						 : "Resizing");
				} else if (srv_encrypt_log) {
					info << "Encrypting and resizing";
				} else {
					info << "Removing encryption"
						" and resizing";
				}

				info << " redo log from " << n_files
				     << "*" << srv_log_file_size << " to ";
			} else if (srv_encrypt_log) {
				info << "Encrypting redo log: ";
			} else {
				info << "Removing redo log encryption: ";
			}

			info << srv_n_log_files << "*"
			     << srv_log_file_size_requested
			     << " bytes; LSN=" << flushed_lsn;
		}

		srv_start_lsn = flushed_lsn;
		/* Flush the old log files. */
		log_mutex_exit();

		log_write_up_to(flushed_lsn, true);

		/* If innodb_flush_method=O_DSYNC,
		we need to explicitly flush the log buffers. */
		fil_flush(SRV_LOG_SPACE_FIRST_ID);

		ut_ad(flushed_lsn == log_get_lsn());

		/* Check if the buffer pools are clean.  If not
		retry till it is clean. */
		pending_io = buf_pool_check_no_pending_io();

		if (pending_io > 0) {
			count++;
			/* Print a message every 60 seconds if we
			are waiting to clean the buffer pools */
			if (srv_print_verbose_log && count > 600) {
				ib::info() << "Waiting for "
					<< pending_io << " buffer "
					<< "page I/Os to complete";
				count = 0;
			}
		}
		os_thread_sleep(100000);

	} while (buf_pool_check_no_pending_io());

	DBUG_RETURN(flushed_lsn);
}

/********************************************************************
Starts InnoDB and creates a new database if database files
are not found and the user wants.
@return DB_SUCCESS or error code */
dberr_t
innobase_start_or_create_for_mysql()
{
	bool		create_new_db = false;
	lsn_t		flushed_lsn;
	dberr_t		err		= DB_SUCCESS;
	ulint		srv_n_log_files_found = srv_n_log_files;
	mtr_t		mtr;
	char		logfilename[10000];
	char*		logfile0	= NULL;
	size_t		dirnamelen;
	unsigned	i = 0;

	ut_ad(srv_operation == SRV_OPERATION_NORMAL
	      || is_mariabackup_restore_or_export());

	if (srv_force_recovery == SRV_FORCE_NO_LOG_REDO) {
		srv_read_only_mode = true;
	}

	high_level_read_only = srv_read_only_mode
		|| srv_force_recovery > SRV_FORCE_NO_TRX_UNDO
		|| srv_sys_space.created_new_raw();

	/* Reset the start state. */
	srv_start_state = SRV_START_STATE_NONE;

	if (srv_read_only_mode) {
		ib::info() << "Started in read only mode";

		/* There is no write to InnoDB tablespaces (not even
		temporary ones, because also CREATE TEMPORARY TABLE is
		refused in read-only mode). */
		srv_use_doublewrite_buf = FALSE;
	}

	compile_time_assert(sizeof(ulint) == sizeof(void*));

#ifdef UNIV_DEBUG
	ib::info() << "!!!!!!!! UNIV_DEBUG switched on !!!!!!!!!";
#endif

#ifdef UNIV_IBUF_DEBUG
	ib::info() << "!!!!!!!! UNIV_IBUF_DEBUG switched on !!!!!!!!!";
#endif

#ifdef UNIV_LOG_LSN_DEBUG
	ib::info() << "!!!!!!!! UNIV_LOG_LSN_DEBUG switched on !!!!!!!!!";
#endif /* UNIV_LOG_LSN_DEBUG */

#if defined(COMPILER_HINTS_ENABLED)
	ib::info() << "Compiler hints enabled.";
#endif /* defined(COMPILER_HINTS_ENABLED) */

#ifdef _WIN32
	ib::info() << "Mutexes and rw_locks use Windows interlocked functions";
#else
	ib::info() << "Mutexes and rw_locks use GCC atomic builtins";
#endif
	ib::info() << MUTEX_TYPE;

	ib::info() << "Compressed tables use zlib " ZLIB_VERSION
#ifdef UNIV_ZIP_DEBUG
	      " with validation"
#endif /* UNIV_ZIP_DEBUG */
	      ;
#ifdef UNIV_ZIP_COPY
	ib::info() << "and extra copying";
#endif /* UNIV_ZIP_COPY */

	/* Since InnoDB does not currently clean up all its internal data
	structures in MySQL Embedded Server Library server_end(), we
	print an error message if someone tries to start up InnoDB a
	second time during the process lifetime. */

	if (srv_start_has_been_called) {
		ib::error() << "Startup called second time"
			" during the process lifetime."
			" In the MySQL Embedded Server Library"
			" you cannot call server_init() more than"
			" once during the process lifetime.";
	}

	srv_start_has_been_called = true;

	srv_is_being_started = true;

#ifdef _WIN32
	srv_use_native_aio = TRUE;

#elif defined(LINUX_NATIVE_AIO)

	if (srv_use_native_aio) {
		ib::info() << "Using Linux native AIO";
	}
#else
	/* Currently native AIO is supported only on windows and linux
	and that also when the support is compiled in. In all other
	cases, we ignore the setting of innodb_use_native_aio. */
	srv_use_native_aio = FALSE;
#endif /* _WIN32 */

	/* Register performance schema stages before any real work has been
	started which may need to be instrumented. */
	mysql_stage_register("innodb", srv_stages, UT_ARR_SIZE(srv_stages));

	if (srv_file_flush_method_str == NULL) {
		/* These are the default options */
		srv_file_flush_method = IF_WIN(SRV_ALL_O_DIRECT_FSYNC,SRV_FSYNC);
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "fsync")) {
		srv_file_flush_method = SRV_FSYNC;

	} else if (0 == ut_strcmp(srv_file_flush_method_str, "O_DSYNC")) {
		srv_file_flush_method = SRV_O_DSYNC;

	} else if (0 == ut_strcmp(srv_file_flush_method_str, "O_DIRECT")) {
		srv_file_flush_method = SRV_O_DIRECT;

	} else if (0 == ut_strcmp(srv_file_flush_method_str, "O_DIRECT_NO_FSYNC")) {
		srv_file_flush_method = SRV_O_DIRECT_NO_FSYNC;

	} else if (0 == ut_strcmp(srv_file_flush_method_str, "littlesync")) {
		srv_file_flush_method = SRV_LITTLESYNC;

	} else if (0 == ut_strcmp(srv_file_flush_method_str, "nosync")) {
		srv_file_flush_method = SRV_NOSYNC;
#ifdef _WIN32
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "normal")) {
		srv_file_flush_method = SRV_FSYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "unbuffered")) {
	} else if (0 == ut_strcmp(srv_file_flush_method_str,
				  "async_unbuffered")) {
#endif /* _WIN32 */
	} else {
		ib::error() << "Unrecognized value "
			<< srv_file_flush_method_str
			<< " for innodb_flush_method";
		err = DB_ERROR;
	}

	/* Note that the call srv_boot() also changes the values of
	some variables to the units used by InnoDB internally */

	/* Set the maximum number of threads which can wait for a semaphore
	inside InnoDB: this is the 'sync wait array' size, as well as the
	maximum number of threads that can wait in the 'srv_conc array' for
	their time to enter InnoDB. */

	srv_max_n_threads = 1   /* io_ibuf_thread */
			    + 1 /* io_log_thread */
			    + 1 /* lock_wait_timeout_thread */
			    + 1 /* srv_error_monitor_thread */
			    + 1 /* srv_monitor_thread */
			    + 1 /* srv_master_thread */
			    + 1 /* srv_purge_coordinator_thread */
			    + 1 /* buf_dump_thread */
			    + 1 /* dict_stats_thread */
			    + 1 /* fts_optimize_thread */
			    + 1 /* recv_writer_thread */
			    + 1 /* trx_rollback_or_clean_all_recovered */
			    + 128 /* added as margin, for use of
				  InnoDB Memcached etc. */
			    + max_connections
			    + srv_n_read_io_threads
			    + srv_n_write_io_threads
			    + srv_n_purge_threads
			    + srv_n_page_cleaners
			    /* FTS Parallel Sort */
			    + fts_sort_pll_degree * FTS_NUM_AUX_INDEX
			      * max_connections;

	if (srv_buf_pool_size >= BUF_POOL_SIZE_THRESHOLD) {

		if (srv_buf_pool_instances == srv_buf_pool_instances_default) {
#if defined(_WIN32) && !defined(_WIN64)
			/* Do not allocate too large of a buffer pool on
			Windows 32-bit systems, which can have trouble
			allocating larger single contiguous memory blocks. */
			srv_buf_pool_size = static_cast<ulint>(ut_uint64_align_up(srv_buf_pool_size, srv_buf_pool_chunk_unit));
			srv_buf_pool_instances = ut_min(
				static_cast<ulong>(MAX_BUFFER_POOLS),
				static_cast<ulong>(srv_buf_pool_size / srv_buf_pool_chunk_unit));
#else /* defined(_WIN32) && !defined(_WIN64) */
			/* Default to 8 instances when size > 1GB. */
			srv_buf_pool_instances = 8;
#endif /* defined(_WIN32) && !defined(_WIN64) */
		}
	} else {
		/* If buffer pool is less than 1 GiB, assume fewer
		threads. Also use only one buffer pool instance. */
		if (srv_buf_pool_instances != srv_buf_pool_instances_default
		    && srv_buf_pool_instances != 1) {
			/* We can't distinguish whether the user has explicitly
			started mysqld with --innodb-buffer-pool-instances=0,
			(srv_buf_pool_instances_default is 0) or has not
			specified that option at all. Thus we have the
			limitation that if the user started with =0, we
			will not emit a warning here, but we should actually
			do so. */
			ib::info()
				<< "Adjusting innodb_buffer_pool_instances"
				" from " << srv_buf_pool_instances << " to 1"
				" since innodb_buffer_pool_size is less than "
				<< BUF_POOL_SIZE_THRESHOLD / (1024 * 1024)
				<< " MiB";
		}

		srv_buf_pool_instances = 1;
	}

	if (srv_buf_pool_chunk_unit * srv_buf_pool_instances
	    > srv_buf_pool_size) {
		/* Size unit of buffer pool is larger than srv_buf_pool_size.
		adjust srv_buf_pool_chunk_unit for srv_buf_pool_size. */
		srv_buf_pool_chunk_unit
			= static_cast<ulong>(srv_buf_pool_size)
			  / srv_buf_pool_instances;
		if (srv_buf_pool_size % srv_buf_pool_instances != 0) {
			++srv_buf_pool_chunk_unit;
		}
	}

	srv_buf_pool_size = buf_pool_size_align(srv_buf_pool_size);

	if (srv_n_page_cleaners > srv_buf_pool_instances) {
		/* limit of page_cleaner parallelizability
		is number of buffer pool instances. */
		srv_n_page_cleaners = srv_buf_pool_instances;
	}

	srv_boot();

	ib::info() << ut_crc32_implementation;

	if (!srv_read_only_mode) {

		mutex_create(LATCH_ID_SRV_MONITOR_FILE,
			     &srv_monitor_file_mutex);

		if (srv_innodb_status) {

			srv_monitor_file_name = static_cast<char*>(
				ut_malloc_nokey(
					strlen(fil_path_to_mysql_datadir)
					+ 20 + sizeof "/innodb_status."));

			sprintf(srv_monitor_file_name,
				"%s/innodb_status." ULINTPF,
				fil_path_to_mysql_datadir,
				os_proc_get_number());

			srv_monitor_file = fopen(srv_monitor_file_name, "w+");

			if (!srv_monitor_file) {
				ib::error() << "Unable to create "
					<< srv_monitor_file_name << ": "
					<< strerror(errno);
				if (err == DB_SUCCESS) {
					err = DB_ERROR;
				}
			}
		} else {

			srv_monitor_file_name = NULL;
			srv_monitor_file = os_file_create_tmpfile(NULL);

			if (!srv_monitor_file && err == DB_SUCCESS) {
				err = DB_ERROR;
			}
		}

		mutex_create(LATCH_ID_SRV_MISC_TMPFILE,
			     &srv_misc_tmpfile_mutex);

		srv_misc_tmpfile = os_file_create_tmpfile(NULL);

		if (!srv_misc_tmpfile && err == DB_SUCCESS) {
			err = DB_ERROR;
		}
	}

	if (err != DB_SUCCESS) {
		return(srv_init_abort(err));
	}

	srv_n_file_io_threads = srv_n_read_io_threads;

	srv_n_file_io_threads += srv_n_write_io_threads;

	if (!srv_read_only_mode) {
		/* Add the log and ibuf IO threads. */
		srv_n_file_io_threads += 2;
	} else {
		ib::info() << "Disabling background log and ibuf IO write"
			<< " threads.";
	}

	ut_a(srv_n_file_io_threads <= SRV_MAX_N_IO_THREADS);

	if (!os_aio_init(srv_n_read_io_threads,
			 srv_n_write_io_threads,
			 SRV_MAX_N_PENDING_SYNC_IOS)) {

		ib::error() << "Cannot initialize AIO sub-system";

		return(srv_init_abort(DB_ERROR));
	}

	fil_init(srv_file_per_table ? 50000 : 5000, srv_max_n_open_files);

	double	size;
	char	unit;

	if (srv_buf_pool_size >= 1024 * 1024 * 1024) {
		size = ((double) srv_buf_pool_size) / (1024 * 1024 * 1024);
		unit = 'G';
	} else {
		size = ((double) srv_buf_pool_size) / (1024 * 1024);
		unit = 'M';
	}

	double	chunk_size;
	char	chunk_unit;

	if (srv_buf_pool_chunk_unit >= 1024 * 1024 * 1024) {
		chunk_size = srv_buf_pool_chunk_unit / 1024.0 / 1024 / 1024;
		chunk_unit = 'G';
	} else {
		chunk_size = srv_buf_pool_chunk_unit / 1024.0 / 1024;
		chunk_unit = 'M';
	}

	ib::info() << "Initializing buffer pool, total size = "
		<< size << unit << ", instances = " << srv_buf_pool_instances
		<< ", chunk size = " << chunk_size << chunk_unit;

	err = buf_pool_init(srv_buf_pool_size, srv_buf_pool_instances);

	if (err != DB_SUCCESS) {
		ib::error() << "Cannot allocate memory for the buffer pool";

		return(srv_init_abort(DB_ERROR));
	}

	ib::info() << "Completed initialization of buffer pool";

#ifdef UNIV_DEBUG
	/* We have observed deadlocks with a 5MB buffer pool but
	the actual lower limit could very well be a little higher. */

	if (srv_buf_pool_size <= 5 * 1024 * 1024) {

		ib::info() << "Small buffer pool size ("
			<< srv_buf_pool_size / 1024 / 1024
			<< "M), the flst_validate() debug function can cause a"
			<< " deadlock if the buffer pool fills up.";
	}
#endif /* UNIV_DEBUG */

	fsp_init();
	log_sys_init();

	recv_sys_init();
	lock_sys_create(srv_lock_table_size);

	/* Create i/o-handler threads: */

	for (ulint t = 0; t < srv_n_file_io_threads; ++t) {

		n[t] = t;

		thread_handles[t] = os_thread_create(io_handler_thread, n + t, thread_ids + t);
		thread_started[t] = true;
	}

	if (!srv_read_only_mode) {
		buf_flush_page_cleaner_init();

		buf_page_cleaner_is_active = true;
		os_thread_create(buf_flush_page_cleaner_coordinator,
				 NULL, NULL);

		for (i = 1; i < srv_n_page_cleaners; ++i) {
			os_thread_create(buf_flush_page_cleaner_worker,
					 NULL, NULL);
		}

#ifdef UNIV_LINUX
		/* Wait for the setpriority() call to finish. */
		os_event_wait(recv_sys->flush_end);
#endif /* UNIV_LINUX */
		srv_start_state_set(SRV_START_STATE_IO);
	}

	if (srv_n_log_files * srv_log_file_size >= log_group_max_size) {
		/* Log group size is limited by the size of page number. Remove this
		limitation when fil_io() is not used for recovery log io. */
		ib::error() << "Combined size of log files must be < "
			<< log_group_max_size;

		return(srv_init_abort(DB_ERROR));
	}

	os_normalize_path(srv_data_home);

	/* Check if the data files exist or not. */
	err = srv_sys_space.check_file_spec(
		&create_new_db, MIN_EXPECTED_TABLESPACE_SIZE);

	if (err != DB_SUCCESS) {
		return(srv_init_abort(DB_ERROR));
	}

	srv_startup_is_before_trx_rollback_phase = !create_new_db;

	/* Check if undo tablespaces and redo log files exist before creating
	a new system tablespace */
	if (create_new_db) {
		err = srv_check_undo_redo_logs_exists();
		if (err != DB_SUCCESS) {
			return(srv_init_abort(DB_ERROR));
		}
		recv_sys_debug_free();
	}

	/* Open or create the data files. */
	ulint	sum_of_new_sizes;

	err = srv_sys_space.open_or_create(
		false, create_new_db, &sum_of_new_sizes, &flushed_lsn);

	switch (err) {
	case DB_SUCCESS:
		break;
	case DB_CANNOT_OPEN_FILE:
		ib::error()
			<< "Could not open or create the system tablespace. If"
			" you tried to add new data files to the system"
			" tablespace, and it failed here, you should now"
			" edit innodb_data_file_path in my.cnf back to what"
			" it was, and remove the new ibdata files InnoDB"
			" created in this failed attempt. InnoDB only wrote"
			" those files full of zeros, but did not yet use"
			" them in any way. But be careful: do not remove"
			" old data files which contain your precious data!";
		/* fall through */
	default:
		/* Other errors might come from Datafile::validate_first_page() */
		return(srv_init_abort(err));
	}

	dirnamelen = strlen(srv_log_group_home_dir);
	ut_a(dirnamelen < (sizeof logfilename) - 10 - sizeof "ib_logfile");
	memcpy(logfilename, srv_log_group_home_dir, dirnamelen);

	/* Add a path separator if needed. */
	if (dirnamelen && logfilename[dirnamelen - 1] != OS_PATH_SEPARATOR) {
		logfilename[dirnamelen++] = OS_PATH_SEPARATOR;
	}

	srv_log_file_size_requested = srv_log_file_size;

	if (innodb_encrypt_temporary_tables && !log_crypt_init()) {
		return srv_init_abort(DB_ERROR);
	}

	if (create_new_db) {

		buf_flush_sync_all_buf_pools();

		flushed_lsn = log_get_lsn();

		err = create_log_files(
			logfilename, dirnamelen, flushed_lsn, logfile0);

		if (err != DB_SUCCESS) {
			for (Tablespace::const_iterator
			       i = srv_sys_space.begin();
			     i != srv_sys_space.end(); i++) {
				os_file_delete(innodb_data_file_key,
					       i->filepath());
			}
			return(srv_init_abort(err));
		}
	} else {
		srv_log_file_size = 0;

		for (i = 0; i < SRV_N_LOG_FILES_MAX; i++) {
			os_file_stat_t	stat_info;

			sprintf(logfilename + dirnamelen,
				"ib_logfile%u", i);

			err = os_file_get_status(
				logfilename, &stat_info, false,
				srv_read_only_mode);

			if (err == DB_NOT_FOUND) {
				if (i == 0
				    && is_mariabackup_restore_or_export())
					return (DB_SUCCESS);

				/* opened all files */
				break;
			}

			if (stat_info.type != OS_FILE_TYPE_FILE) {
				break;
			}

			if (!srv_file_check_mode(logfilename)) {
				return(srv_init_abort(DB_ERROR));
			}

			const os_offset_t size = stat_info.size;
			ut_a(size != (os_offset_t) -1);

			if (size & (OS_FILE_LOG_BLOCK_SIZE - 1)) {

				ib::error() << "Log file " << logfilename
					<< " size " << size << " is not a"
					" multiple of 512 bytes";
				return(srv_init_abort(DB_ERROR));
			}

			if (i == 0) {
				if (size == 0
				    && is_mariabackup_restore_or_export()) {
					/* Tolerate an empty ib_logfile0
					from a previous run of
					mariabackup --prepare. */
					return(DB_SUCCESS);
				}
				/* The first log file must consist of
				at least the following 512-byte pages:
				header, checkpoint page 1, empty,
				checkpoint page 2, redo log page(s).

				Mariabackup --prepare would create an
				empty ib_logfile0. Tolerate it if there
				are no other ib_logfile* files. */
				if ((size != 0 || i != 0)
				    && size <= OS_FILE_LOG_BLOCK_SIZE * 4) {
					ib::error() << "Log file "
						<< logfilename << " size "
						<< size << " is too small";
					return(srv_init_abort(DB_ERROR));
				}
				srv_log_file_size = size;
			} else if (size != srv_log_file_size) {

				ib::error() << "Log file " << logfilename
					<< " is of different size " << size
					<< " bytes than other log files "
					<< srv_log_file_size << " bytes!";
				return(srv_init_abort(DB_ERROR));
			}
		}

		if (srv_log_file_size == 0) {
			if (flushed_lsn < lsn_t(1000)) {
				ib::error()
					<< "Cannot create log files because"
					" data files are corrupt or the"
					" database was not shut down cleanly"
					" after creating the data files.";
				return srv_init_abort(DB_ERROR);
			}

			strcpy(logfilename + dirnamelen, "ib_logfile0");
			srv_log_file_size = srv_log_file_size_requested;

			err = create_log_files(
				logfilename, dirnamelen,
				flushed_lsn, logfile0);

			if (err == DB_SUCCESS) {
				err = create_log_files_rename(
					logfilename, dirnamelen,
					flushed_lsn, logfile0);
			}

			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}

			/* Suppress the message about
			crash recovery. */
			flushed_lsn = log_get_lsn();
			goto files_checked;
		}

		srv_n_log_files_found = i;

		/* Create the in-memory file space objects. */

		sprintf(logfilename + dirnamelen, "ib_logfile%u", 0);

		/* Disable the doublewrite buffer for log files. */
		fil_space_t*	log_space = fil_space_create(
			"innodb_redo_log",
			SRV_LOG_SPACE_FIRST_ID, 0,
			FIL_TYPE_LOG,
			NULL /* no encryption yet */);

		ut_a(fil_validate());
		ut_a(log_space);

		ut_a(srv_log_file_size <= log_group_max_size);

		const ulint size = 1 + ulint((srv_log_file_size - 1)
					     >> srv_page_size_shift);

		for (unsigned j = 0; j < srv_n_log_files_found; j++) {
			sprintf(logfilename + dirnamelen, "ib_logfile%u", j);

			log_space->add(logfilename, OS_FILE_CLOSED, size,
				       false, false);
		}

		log_init(srv_n_log_files_found);

		if (!log_set_capacity(srv_log_file_size_requested)) {
			return(srv_init_abort(DB_ERROR));
		}
	}

files_checked:
	/* Open all log files and data files in the system
	tablespace: we keep them open until database
	shutdown */

	fil_open_log_and_system_tablespace_files();
	ut_d(fil_space_get(0)->recv_size = srv_sys_space_size_debug);

	err = srv_undo_tablespaces_init(create_new_db);

	/* If the force recovery is set very high then we carry on regardless
	of all errors. Basically this is fingers crossed mode. */

	if (err != DB_SUCCESS
	    && srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {

		return(srv_init_abort(err));
	}

	/* Initialize objects used by dict stats gathering thread, which
	can also be used by recovery if it tries to drop some table */
	if (!srv_read_only_mode) {
		dict_stats_thread_init();
	}

	trx_sys_file_format_init();

	trx_sys_create();

	if (create_new_db) {
		ut_a(!srv_read_only_mode);

		mtr_start(&mtr);

		fsp_header_init(0, sum_of_new_sizes, &mtr);

		compile_time_assert(TRX_SYS_SPACE == 0);
		compile_time_assert(IBUF_SPACE_ID == 0);

		ulint ibuf_root = btr_create(
			DICT_CLUSTERED | DICT_IBUF,
			0, univ_page_size, DICT_IBUF_ID_MIN,
			dict_ind_redundant, NULL, &mtr);

		mtr_commit(&mtr);

		if (ibuf_root == FIL_NULL) {
			return(srv_init_abort(DB_ERROR));
		}

		ut_ad(ibuf_root == IBUF_TREE_ROOT_PAGE_NO);

		/* To maintain backward compatibility we create only
		the first rollback segment before the double write buffer.
		All the remaining rollback segments will be created later,
		after the double write buffer has been created. */
		trx_sys_create_sys_pages();
		trx_sys_init_at_db_start();

		err = dict_create();

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		buf_flush_sync_all_buf_pools();

		flushed_lsn = log_get_lsn();

		err = fil_write_flushed_lsn(flushed_lsn);

		if (err == DB_SUCCESS) {
			err = create_log_files_rename(
				logfilename, dirnamelen,
				flushed_lsn, logfile0);
		}

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}
	} else {

		/* Check if we support the max format that is stamped
		on the system tablespace.
		Note:  We are NOT allowed to make any modifications to
		the TRX_SYS_PAGE_NO page before recovery  because this
		page also contains the max_trx_id etc. important system
		variables that are required for recovery.  We need to
		ensure that we return the system to a state where normal
		recovery is guaranteed to work. We do this by
		invalidating the buffer cache, this will force the
		reread of the page and restoration to its last known
		consistent state, this is REQUIRED for the recovery
		process to work. */
		err = trx_sys_file_format_max_check(
			srv_max_file_format_at_startup);

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		/* Invalidate the buffer pool to ensure that we reread
		the page that we read above, during recovery.
		Note that this is not as heavy weight as it seems. At
		this point there will be only ONE page in the buf_LRU
		and there must be no page in the buf_flush list. */
		buf_pool_invalidate();

		/* Scan and locate truncate log files. Parsed located files
		and add table to truncate information to central vector for
		truncate fix-up action post recovery. */
		err = TruncateLogParser::scan_and_parse(srv_log_group_home_dir);
		if (err != DB_SUCCESS) {

			return(srv_init_abort(DB_ERROR));
		}

		/* We always try to do a recovery, even if the database had
		been shut down normally: this is the normal startup path */

		err = recv_recovery_from_checkpoint_start(flushed_lsn);

		recv_sys->dblwr.pages.clear();

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		switch (srv_operation) {
		case SRV_OPERATION_NORMAL:
		case SRV_OPERATION_RESTORE_ROLLBACK_XA:
		case SRV_OPERATION_RESTORE_EXPORT:
			/* Initialize the change buffer. */
			err = dict_boot();
			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}
			/* This must precede
			recv_apply_hashed_log_recs(true). */
			trx_sys_init_at_db_start();
			break;
		case SRV_OPERATION_RESTORE_DELTA:
		case SRV_OPERATION_BACKUP:
			ut_ad(!"wrong mariabackup mode");
			/* fall through */
		case SRV_OPERATION_RESTORE:
			/* mariabackup --prepare only deals with
			the redo log and the data files, not with
			transactions or the data dictionary. */
			break;
		}

		if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO) {
			/* Apply the hashed log records to the
			respective file pages, for the last batch of
			recv_group_scan_log_recs(). */

			recv_apply_hashed_log_recs(true);

			if (recv_sys->found_corrupt_log
			    || recv_sys->found_corrupt_fs) {
				return(srv_init_abort(DB_CORRUPTION));
			}

			DBUG_PRINT("ib_log", ("apply completed"));

			if (recv_needed_recovery) {
				trx_sys_print_mysql_binlog_offset();
			}
		}

		if (!srv_read_only_mode) {
			const ulint flags = FSP_FLAGS_PAGE_SSIZE();
			for (ulint id = 0; id <= srv_undo_tablespaces; id++) {
				if (fil_space_get(id)) {
					fsp_flags_try_adjust(id, flags);
				}
			}

			if (sum_of_new_sizes > 0) {
				/* New data file(s) were added */
				mtr.start();
				fsp_header_inc_size(0, sum_of_new_sizes, &mtr);
				mtr.commit();
				/* Immediately write the log record about
				increased tablespace size to disk, so that it
				is durable even if mysqld would crash
				quickly */
				log_buffer_flush_to_disk();
			}
		}

		const ulint	tablespace_size_in_header
			= fsp_header_get_tablespace_size();
		const ulint	sum_of_data_file_sizes
			= srv_sys_space.get_sum_of_sizes();
		/* Compare the system tablespace file size to what is
		stored in FSP_SIZE. In srv_sys_space.open_or_create()
		we already checked that the file sizes match the
		innodb_data_file_path specification. */
		if (srv_read_only_mode
		    || sum_of_data_file_sizes == tablespace_size_in_header) {
			/* Do not complain about the size. */
		} else if (!srv_sys_space.can_auto_extend_last_file()
			   || sum_of_data_file_sizes
			   < tablespace_size_in_header) {
			ib::error() << "Tablespace size stored in header is "
				<< tablespace_size_in_header
				<< " pages, but the sum of data file sizes is "
				<< sum_of_data_file_sizes << " pages";

			if (srv_force_recovery == 0
			    && sum_of_data_file_sizes
			    < tablespace_size_in_header) {
				ib::error() <<
					"Cannot start InnoDB. The tail of"
					" the system tablespace is"
					" missing. Have you edited"
					" innodb_data_file_path in my.cnf"
					" in an inappropriate way, removing"
					" data files from there?"
					" You can set innodb_force_recovery=1"
					" in my.cnf to force"
					" a startup if you are trying to"
					" recover a badly corrupt database.";

				return(srv_init_abort(DB_ERROR));
			}
		}

		/* recv_recovery_from_checkpoint_finish needs trx lists which
		are initialized in trx_sys_init_at_db_start(). */

		recv_recovery_from_checkpoint_finish();

		if (is_mariabackup_restore_or_export()) {
			/* After applying the redo log from
			SRV_OPERATION_BACKUP, flush the changes
			to the data files and truncate or delete the log.
			Unless --export is specified, no further change to
			InnoDB files is needed. */
			ut_ad(srv_force_recovery <= SRV_FORCE_IGNORE_CORRUPT);
			ut_ad(srv_n_log_files_found <= 1);
			ut_ad(recv_no_log_write);
			buf_flush_sync_all_buf_pools();
			err = fil_write_flushed_lsn(log_get_lsn());
			ut_ad(!buf_pool_check_no_pending_io());
			fil_close_log_files(true);
			log_group_close_all();
			if (err == DB_SUCCESS) {
				bool trunc = is_mariabackup_restore();
				/* Delete subsequent log files. */
				delete_log_files(logfilename, dirnamelen,
						 srv_n_log_files_found, trunc);
				if (trunc) {
					/* Truncate the first log file. */
					strcpy(logfilename + dirnamelen,
					       "ib_logfile0");
					FILE* f = fopen(logfilename, "w");
					fclose(f);
				}
			}
			return(err);
		}

		/* Upgrade or resize or rebuild the redo logs before
		generating any dirty pages, so that the old redo log
		files will not be written to. */

		if (srv_force_recovery == SRV_FORCE_NO_LOG_REDO) {
			/* Completely ignore the redo log. */
		} else if (srv_read_only_mode) {
			/* Leave the redo log alone. */
		} else if (srv_log_file_size_requested == srv_log_file_size
			   && srv_n_log_files_found == srv_n_log_files
			   && log_sys->log.format
			   == (srv_safe_truncate
			       ? (srv_encrypt_log
				  ? LOG_HEADER_FORMAT_10_3
				  | LOG_HEADER_FORMAT_ENCRYPTED
				  : LOG_HEADER_FORMAT_10_3)
			       : (srv_encrypt_log
				  ? LOG_HEADER_FORMAT_10_2
				  | LOG_HEADER_FORMAT_ENCRYPTED
				  : LOG_HEADER_FORMAT_10_2))
			   && log_sys->log.subformat == !!srv_safe_truncate) {
			/* No need to add or remove encryption,
			upgrade, downgrade, or resize. */
		} else {
			/* Prepare to delete the old redo log files */
			flushed_lsn = srv_prepare_to_delete_redo_log_files(i);

			DBUG_EXECUTE_IF("innodb_log_abort_1",
					return(srv_init_abort(DB_ERROR)););
			/* Prohibit redo log writes from any other
			threads until creating a log checkpoint at the
			end of create_log_files(). */
			ut_d(recv_no_log_write = true);
			ut_ad(!buf_pool_check_no_pending_io());

			DBUG_EXECUTE_IF("innodb_log_abort_3",
					return(srv_init_abort(DB_ERROR)););
			DBUG_PRINT("ib_log", ("After innodb_log_abort_3"));

			/* Stamp the LSN to the data files. */
			err = fil_write_flushed_lsn(flushed_lsn);

			DBUG_EXECUTE_IF("innodb_log_abort_4", err = DB_ERROR;);
			DBUG_PRINT("ib_log", ("After innodb_log_abort_4"));

			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}

			/* Close and free the redo log files, so that
			we can replace them. */
			fil_close_log_files(true);

			DBUG_EXECUTE_IF("innodb_log_abort_5",
					return(srv_init_abort(DB_ERROR)););
			DBUG_PRINT("ib_log", ("After innodb_log_abort_5"));

			/* Free the old log file space. */
			log_group_close_all();

			ib::info() << "Starting to delete and rewrite log"
				" files.";

			srv_log_file_size = srv_log_file_size_requested;

			err = create_log_files(
				logfilename, dirnamelen, flushed_lsn,
				logfile0);

			if (err == DB_SUCCESS) {
				err = create_log_files_rename(
					logfilename, dirnamelen, flushed_lsn,
					logfile0);
			}

			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}
		}

		/* Validate a few system page types that were left
		uninitialized by older versions of MySQL. */
		if (!high_level_read_only) {
			mtr_t		mtr;
			buf_block_t*	block;
			mtr.start();
			mtr.set_sys_modified();
			/* Bitmap page types will be reset in
			buf_dblwr_check_block() without redo logging. */
			block = buf_page_get(
				page_id_t(IBUF_SPACE_ID,
					  FSP_IBUF_HEADER_PAGE_NO),
				univ_page_size, RW_X_LATCH, &mtr);
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			/* Already MySQL 3.23.53 initialized
			FSP_IBUF_TREE_ROOT_PAGE_NO to
			FIL_PAGE_INDEX. No need to reset that one. */
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
				univ_page_size, RW_X_LATCH, &mtr);
			fil_block_check_type(*block, FIL_PAGE_TYPE_TRX_SYS,
					     &mtr);
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE,
					  FSP_FIRST_RSEG_PAGE_NO),
				univ_page_size, RW_X_LATCH, &mtr);
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE, FSP_DICT_HDR_PAGE_NO),
				univ_page_size, RW_X_LATCH, &mtr);
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			mtr.commit();
		}

		/* Roll back any recovered data dictionary transactions, so
		that the data dictionary tables will be free of any locks.
		The data dictionary latch should guarantee that there is at
		most one data dictionary transaction active at a time. */
		if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO) {
			trx_rollback_or_clean_recovered(FALSE);
		}

		/* Fix-up truncate of tables in the system tablespace
		if server crashed while truncate was active. The non-
		system tables are done after tablespace discovery. Do
		this now because this procedure assumes that no pages
		have changed since redo recovery.  Tablespace discovery
		can do updates to pages in the system tablespace.*/
		err = truncate_t::fixup_tables_in_system_tablespace();

		if (srv_force_recovery < SRV_FORCE_NO_IBUF_MERGE) {
			/* Open or Create SYS_TABLESPACES and SYS_DATAFILES
			so that tablespace names and other metadata can be
			found. */
			err = dict_create_or_check_sys_tablespace();
			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}

			/* The following call is necessary for the insert
			buffer to work with multiple tablespaces. We must
			know the mapping between space id's and .ibd file
			names.

			In a crash recovery, we check that the info in data
			dictionary is consistent with what we already know
			about space id's from the calls to fil_ibd_load().

			In a normal startup, we create the space objects for
			every table in the InnoDB data dictionary that has
			an .ibd file.

			We also determine the maximum tablespace id used. */
			dict_check_tablespaces_and_store_max_id();
		}

		/* Fix-up truncate of table if server crashed while truncate
		was active. */
		err = truncate_t::fixup_tables_in_non_system_tablespace();

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		recv_recovery_rollback_active();
		srv_startup_is_before_trx_rollback_phase = FALSE;

		/* It is possible that file_format tag has never
		been set. In this case we initialize it to minimum
		value.  Important to note that we can do it ONLY after
		we have finished the recovery process so that the
		image of TRX_SYS_PAGE_NO is not stale. */
		trx_sys_file_format_tag_init();
	}

	ut_ad(err == DB_SUCCESS);
	ut_a(sum_of_new_sizes != ULINT_UNDEFINED);

	/* Create the doublewrite buffer to a new tablespace */
	if (!srv_read_only_mode && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    && !buf_dblwr_create()) {
		return(srv_init_abort(DB_ERROR));
	}

	/* Here the double write buffer has already been created and so
	any new rollback segments will be allocated after the double
	write buffer. The default segment should already exist.
	We create the new segments only if it's a new database or
	the database was shutdown cleanly. */

	/* Note: When creating the extra rollback segments during an upgrade
	we violate the latching order, even if the change buffer is empty.
	We make an exception in sync0sync.cc and check srv_is_being_started
	for that violation. It cannot create a deadlock because we are still
	running in single threaded mode essentially. Only the IO threads
	should be running at this stage. */

	ut_a(srv_undo_logs > 0);
	ut_a(srv_undo_logs <= TRX_SYS_N_RSEGS);

	if (!trx_sys_create_rsegs()) {
		return(srv_init_abort(DB_ERROR));
	}

	srv_startup_is_before_trx_rollback_phase = false;

	if (!srv_read_only_mode) {
		/* Create the thread which watches the timeouts
		for lock waits */
		thread_handles[2 + SRV_MAX_N_IO_THREADS] = os_thread_create(
			lock_wait_timeout_thread,
			NULL, thread_ids + 2 + SRV_MAX_N_IO_THREADS);
		thread_started[2 + SRV_MAX_N_IO_THREADS] = true;
		lock_sys->timeout_thread_active = true;

		/* Create the thread which warns of long semaphore waits */
		srv_error_monitor_active = true;
		thread_handles[3 + SRV_MAX_N_IO_THREADS] = os_thread_create(
			srv_error_monitor_thread,
			NULL, thread_ids + 3 + SRV_MAX_N_IO_THREADS);
		thread_started[3 + SRV_MAX_N_IO_THREADS] = true;

		/* Create the thread which prints InnoDB monitor info */
		srv_monitor_active = true;
		thread_handles[4 + SRV_MAX_N_IO_THREADS] = os_thread_create(
			srv_monitor_thread,
			NULL, thread_ids + 4 + SRV_MAX_N_IO_THREADS);
		thread_started[4 + SRV_MAX_N_IO_THREADS] = true;
		srv_start_state |= SRV_START_STATE_LOCK_SYS
			| SRV_START_STATE_MONITOR;
	}

	/* Create the SYS_FOREIGN and SYS_FOREIGN_COLS system tables */
	err = dict_create_or_check_foreign_constraint_tables();
	if (err == DB_SUCCESS) {
		err = dict_create_or_check_sys_tablespace();
		if (err == DB_SUCCESS) {
			err = dict_create_or_check_sys_virtual();
		}
	}
	switch (err) {
	case DB_SUCCESS:
		break;
	case DB_READ_ONLY:
		if (srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO) {
			break;
		}
		ib::error() << "Cannot create system tables in read-only mode";
		/* fall through */
	default:
		return(srv_init_abort(err));
	}

	if (!srv_read_only_mode && srv_operation == SRV_OPERATION_NORMAL) {
		/* Initialize the innodb_temporary tablespace and keep
		it open until shutdown. */
		err = srv_open_tmp_tablespace(create_new_db);

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		trx_temp_rseg_create();
	}

	ut_a(trx_purge_state() == PURGE_STATE_INIT);

	/* Create the master thread which does purge and other utility
	operations */

	if (!srv_read_only_mode
	    && srv_force_recovery < SRV_FORCE_NO_BACKGROUND) {
		thread_handles[1 + SRV_MAX_N_IO_THREADS] = os_thread_create(
			srv_master_thread,
			NULL, thread_ids + (1 + SRV_MAX_N_IO_THREADS));
		thread_started[1 + SRV_MAX_N_IO_THREADS] = true;
		srv_start_state_set(SRV_START_STATE_MASTER);
	}

	if (!srv_read_only_mode
	    && (srv_operation == SRV_OPERATION_NORMAL
		|| srv_operation == SRV_OPERATION_RESTORE_ROLLBACK_XA)
	    && srv_force_recovery < SRV_FORCE_NO_BACKGROUND) {
		srv_undo_sources = true;
		/* Create the dict stats gathering thread */
		srv_dict_stats_thread_active = true;
		dict_stats_thread_handle = os_thread_create(
			dict_stats_thread, NULL, NULL);

		/* Create the thread that will optimize the FTS sub-system. */
		fts_optimize_init();

		thread_handles[5 + SRV_MAX_N_IO_THREADS] = os_thread_create(
			srv_purge_coordinator_thread,
			NULL, thread_ids + 5 + SRV_MAX_N_IO_THREADS);

		thread_started[5 + SRV_MAX_N_IO_THREADS] = true;

		ut_a(UT_ARR_SIZE(thread_ids)
		     > 5 + srv_n_purge_threads + SRV_MAX_N_IO_THREADS);

		/* We've already created the purge coordinator thread above. */
		for (i = 1; i < srv_n_purge_threads; ++i) {
			thread_handles[5 + i + SRV_MAX_N_IO_THREADS] = os_thread_create(
				srv_worker_thread, NULL,
				thread_ids + 5 + i + SRV_MAX_N_IO_THREADS);
			thread_started[5 + i + SRV_MAX_N_IO_THREADS] = true;
		}

		srv_start_wait_for_purge_to_start();

		srv_start_state_set(SRV_START_STATE_PURGE);
	} else {
		purge_sys->state = PURGE_STATE_DISABLED;
	}

	srv_is_being_started = false;

	if (!srv_read_only_mode) {
		/* wake main loop of page cleaner up */
		os_event_set(buf_flush_event);

		if (srv_use_mtflush) {
			/* Start multi-threaded flush threads */
			mtflush_ctx = buf_mtflu_handler_init(
				srv_mtflush_threads,
				srv_buf_pool_instances);

			/* Set up the thread ids */
			buf_mtflu_set_thread_ids(
				srv_mtflush_threads,
				mtflush_ctx,
				(thread_ids + 6 + 32));
		}
	}

	if (srv_print_verbose_log) {
		ib::info() << INNODB_VERSION_STR
			<< " started; log sequence number "
			<< srv_start_lsn;
	}

	if (srv_force_recovery > 0) {
		ib::info() << "!!! innodb_force_recovery is set to "
			<< srv_force_recovery << " !!!";
	}

	if (srv_force_recovery == 0) {
		/* In the insert buffer we may have even bigger tablespace
		id's, because we may have dropped those tablespaces, but
		insert buffer merge has not had time to clean the records from
		the ibuf tree. */

		ibuf_update_max_tablespace_id();
	}

	if (!srv_read_only_mode) {
		if (create_new_db) {
			srv_buffer_pool_load_at_startup = FALSE;
		}

#ifdef WITH_WSREP
		/*
		  Create the dump/load thread only when not running with
		  --wsrep-recover.
		*/
		if (!wsrep_recovery) {
#endif /* WITH_WSREP */

		/* Create the buffer pool dump/load thread */
		srv_buf_dump_thread_active = true;
		buf_dump_thread_handle=
			os_thread_create(buf_dump_thread, NULL, NULL);

#ifdef WITH_WSREP
		} else {
			ib::warn() <<
				"Skipping buffer pool dump/restore during "
				"wsrep recovery.";
		}
#endif /* WITH_WSREP */

		/* Create thread(s) that handles key rotation. This is
		needed already here as log_preflush_pool_modified_pages
		will flush dirty pages and that might need e.g.
		fil_crypt_threads_event. */
		fil_system_enter();
		btr_scrub_init();
		fil_crypt_threads_init();
		fil_system_exit();

		/* Initialize online defragmentation. */
		btr_defragment_init();
		btr_defragment_thread_active = true;
		os_thread_create(btr_defragment_thread, NULL, NULL);

		srv_start_state |= SRV_START_STATE_REDO;
	}

	/* Create the buffer pool resize thread */
	srv_buf_resize_thread_active = true;
	os_thread_create(buf_resize_thread, NULL, NULL);

	return(DB_SUCCESS);
}

/** Shut down background threads that can generate undo log. */
void
srv_shutdown_bg_undo_sources()
{
	if (srv_undo_sources) {
		ut_ad(!srv_read_only_mode);
		srv_shutdown_state = SRV_SHUTDOWN_INITIATED;
		fts_optimize_shutdown();
		dict_stats_shutdown();
		while (row_get_background_drop_list_len_low()) {
			srv_wake_master_thread();
			os_thread_yield();
		}
		srv_undo_sources = false;
	}
}

/** Shut down InnoDB. */
void
innodb_shutdown()
{
	ut_ad(!my_atomic_loadptr_explicit(reinterpret_cast<void**>
					  (&srv_running),
					  MY_MEMORY_ORDER_RELAXED));
	ut_ad(!srv_undo_sources);

	switch (srv_operation) {
	case SRV_OPERATION_RESTORE_ROLLBACK_XA:
		if (dberr_t err = fil_write_flushed_lsn(log_sys->lsn))
			ib::error() << "Writing flushed lsn " << log_sys->lsn
				    << " failed; error=" << err;
		/* fall through */
	case SRV_OPERATION_BACKUP:
	case SRV_OPERATION_RESTORE:
	case SRV_OPERATION_RESTORE_DELTA:
	case SRV_OPERATION_RESTORE_EXPORT:
		fil_close_all_files();
		break;
	case SRV_OPERATION_NORMAL:
		/* Shut down the persistent files. */
		logs_empty_and_mark_files_at_shutdown();

		if (ulint n_threads = srv_conc_get_active_threads()) {
			ib::warn() << "Query counter shows "
				   << n_threads << " queries still"
				" inside InnoDB at shutdown";
		}
	}

	/* Exit any remaining threads. */
	srv_shutdown_all_bg_threads();

	if (srv_monitor_file) {
		fclose(srv_monitor_file);
		srv_monitor_file = 0;
		if (srv_monitor_file_name) {
			unlink(srv_monitor_file_name);
			ut_free(srv_monitor_file_name);
		}
	}

	if (srv_misc_tmpfile) {
		fclose(srv_misc_tmpfile);
		srv_misc_tmpfile = 0;
	}

	ut_ad(dict_stats_event || !srv_was_started || srv_read_only_mode);
	ut_ad(dict_sys || !srv_was_started);
	ut_ad(trx_sys || !srv_was_started);
	ut_ad(buf_dblwr || !srv_was_started || srv_read_only_mode
	      || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO);
	ut_ad(lock_sys || !srv_was_started);
#ifdef BTR_CUR_HASH_ADAPT
	ut_ad(btr_search_sys || !srv_was_started);
#endif /* BTR_CUR_HASH_ADAPT */
	ut_ad(ibuf || !srv_was_started);
	ut_ad(log_sys || !srv_was_started);

	if (dict_stats_event) {
		dict_stats_thread_deinit();
	}

	if (srv_start_state_is_set(SRV_START_STATE_REDO)) {
		ut_ad(!srv_read_only_mode);
		/* srv_shutdown_bg_undo_sources() already invoked
		fts_optimize_shutdown(); dict_stats_shutdown(); */

		fil_crypt_threads_cleanup();
		btr_scrub_cleanup();
		btr_defragment_shutdown();
	}

	/* This must be disabled before closing the buffer pool
	and closing the data dictionary.  */

#ifdef BTR_CUR_HASH_ADAPT
	if (dict_sys) {
		btr_search_disable();
	}
#endif /* BTR_CUR_HASH_ADAPT */
	if (ibuf) {
		ibuf_close();
	}
	if (log_sys) {
		log_shutdown();
	}
	if (trx_sys) {
		trx_sys_file_format_close();
		trx_sys_close();
	}
	UT_DELETE(purge_sys);
	purge_sys = NULL;
	if (buf_dblwr) {
		buf_dblwr_free();
	}
	if (lock_sys) {
		lock_sys_close();
	}

	trx_pool_close();

	/* We don't create these mutexes in RO mode because we don't create
	the temp files that the cover. */
	if (!srv_read_only_mode) {
		mutex_free(&srv_monitor_file_mutex);
		mutex_free(&srv_misc_tmpfile_mutex);
	}

	if (dict_sys) {
		dict_close();
	}

#ifdef BTR_CUR_HASH_ADAPT
	if (btr_search_sys) {
		btr_search_sys_free();
	}
#endif /* BTR_CUR_HASH_ADAPT */

	/* 3. Free all InnoDB's own mutexes and the os_fast_mutexes inside
	them */
	os_aio_free();
	row_mysql_close();
	srv_free();
	fil_close();

	/* 4. Free all allocated memory */

	pars_lexer_close();
	recv_sys_close();

	ut_ad(buf_pool_ptr || !srv_was_started);
	if (buf_pool_ptr) {
		buf_pool_free(srv_buf_pool_instances);
	}

	sync_check_close();

	if (dict_foreign_err_file) {
		fclose(dict_foreign_err_file);
	}

	srv_sys_space.shutdown();
	if (srv_tmp_space.get_sanity_check_status()) {
		fil_space_close(srv_tmp_space.name());
		srv_tmp_space.delete_files();
	}
	srv_tmp_space.shutdown();

#ifdef WITH_INNODB_DISALLOW_WRITES
	os_event_destroy(srv_allow_writes_event);
#endif /* WITH_INNODB_DISALLOW_WRITES */

	if (srv_was_started && srv_print_verbose_log) {
		ib::info() << "Shutdown completed; log sequence number "
			<< srv_shutdown_lsn;
	}

	srv_start_state = SRV_START_STATE_NONE;
	srv_was_started = false;
	srv_start_has_been_called = false;
}

/** Get the meta-data filename from the table name for a
single-table tablespace.
@param[in]	table		table object
@param[out]	filename	filename
@param[in]	max_len		filename max length */
void
srv_get_meta_data_filename(
	dict_table_t*	table,
	char*		filename,
	ulint		max_len)
{
	ulint		len;
	char*		path;

	/* Make sure the data_dir_path is set. */
	dict_get_and_save_data_dir_path(table, false);

	if (DICT_TF_HAS_DATA_DIR(table->flags)) {
		ut_a(table->data_dir_path);

		path = fil_make_filepath(
			table->data_dir_path, table->name.m_name, CFG, true);
	} else {
		path = fil_make_filepath(NULL, table->name.m_name, CFG, false);
	}

	ut_a(path);
	len = ut_strlen(path);
	ut_a(max_len >= len);

	strcpy(filename, path);

	ut_free(path);
}
