/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

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
@file include/srv0start.h
Starts the Innobase database server

Created 10/10/1995 Heikki Tuuri
*******************************************************/

#ifndef srv0start_h
#define srv0start_h

#include "log0log.h"
#include "ut0byte.h"

// Forward declaration
struct dict_table_t;

/** If buffer pool is less than the size,
only one buffer pool instance is used. */
#define BUF_POOL_SIZE_THRESHOLD		(1024 * 1024 * 1024)

/** Open the configured number of dedicated undo tablespaces.
@param[in]	create_new_db	whether the database is being initialized
@return DB_SUCCESS or error code */
dberr_t
srv_undo_tablespaces_init(bool create_new_db);

/****************************************************************//**
Starts Innobase and creates a new database if database files
are not found and the user wants.
@return DB_SUCCESS or error code */
dberr_t
innobase_start_or_create_for_mysql();

/** Shut down InnoDB. */
void
innodb_shutdown();

/** Shut down background threads that can generate undo log. */
void
srv_shutdown_bg_undo_sources();

/*************************************************************//**
Copy the file path component of the physical file to parameter. It will
copy up to and including the terminating path separator.
@return number of bytes copied or ULINT_UNDEFINED if destination buffer
	is smaller than the path to be copied. */
ulint
srv_path_copy(
/*==========*/
	char*		dest,		/*!< out: destination buffer */
	ulint		dest_len,	/*!< in: max bytes to copy */
	const char*	basedir,	/*!< in: base directory */
	const char*	table_name)	/*!< in: source table name */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Get the meta-data filename from the table name for a
single-table tablespace.
@param[in]	table		table object
@param[out]	filename	filename
@param[in]	max_len		filename max length */
void
srv_get_meta_data_filename(
	dict_table_t*	table,
	char*		filename,
	ulint		max_len);

/** Get the encryption-data filename from the table name for a
single-table tablespace.
@param[in]	table		table object
@param[out]	filename	filename
@param[in]	max_len		filename max length */
void
srv_get_encryption_data_filename(
	dict_table_t*	table,
	char*		filename,
	ulint		max_len);

/** Log sequence number at shutdown */
extern	lsn_t	srv_shutdown_lsn;
/** Log sequence number immediately after startup */
extern	lsn_t	srv_start_lsn;

/** TRUE if the server is being started */
extern	bool	srv_is_being_started;
/** TRUE if SYS_TABLESPACES is available for lookups */
extern	bool	srv_sys_tablespaces_open;
/** TRUE if the server is being started, before rolling back any
incomplete transactions */
extern	bool	srv_startup_is_before_trx_rollback_phase;

/** TRUE if a raw partition is in use */
extern	ibool	srv_start_raw_disk_in_use;

/** Shutdown state */
enum srv_shutdown_t {
	SRV_SHUTDOWN_NONE = 0,	/*!< Database running normally */
	/** Shutdown initiated in srv_shutdown_bg_undo_sources() */
	SRV_SHUTDOWN_INITIATED,
	SRV_SHUTDOWN_CLEANUP,	/*!< Cleaning up in
				logs_empty_and_mark_files_at_shutdown() */
	SRV_SHUTDOWN_FLUSH_PHASE,/*!< At this phase the master and the
				purge threads must have completed their
				work. Once we enter this phase the
				page_cleaner can clean up the buffer
				pool and exit */
	SRV_SHUTDOWN_LAST_PHASE,/*!< Last phase after ensuring that
				the buffer pool can be freed: flush
				all file spaces and close all files */
	SRV_SHUTDOWN_EXIT_THREADS/*!< Exit all threads */
};

/** Whether any undo log records can be generated */
extern bool srv_undo_sources;

/** At a shutdown this value climbs from SRV_SHUTDOWN_NONE to
SRV_SHUTDOWN_CLEANUP and then to SRV_SHUTDOWN_LAST_PHASE, and so on */
extern	enum srv_shutdown_t	srv_shutdown_state;

/** Files comprising the system tablespace */
extern pfs_os_file_t	files[1000];
#endif
