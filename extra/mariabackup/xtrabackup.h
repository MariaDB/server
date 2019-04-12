/******************************************************
Copyright (c) 2011-2015 Percona LLC and/or its affiliates.

Declarations for xtrabackup.cc

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#ifndef XB_XTRABACKUP_H
#define XB_XTRABACKUP_H

#include <my_getopt.h>
#include "datasink.h"
#include "xbstream.h"
#include "changed_page_bitmap.h"

struct xb_delta_info_t
{
	xb_delta_info_t(ulint page_size, ulint zip_size, ulint space_id)
	: page_size(page_size), zip_size(zip_size), space_id(space_id) {}

	ulint	page_size;
	ulint	zip_size;
	ulint	space_id;
};

/* value of the --incremental option */
extern lsn_t incremental_lsn;

extern char		*xtrabackup_target_dir;
extern char		*xtrabackup_incremental_dir;
extern char		*xtrabackup_incremental_basedir;
extern char		*innobase_data_home_dir;
extern char		*innobase_buffer_pool_filename;
extern char		*xb_plugin_dir;
extern char		*xb_rocksdb_datadir;
extern my_bool	xb_backup_rocksdb;

extern uint		opt_protocol;
extern ds_ctxt_t	*ds_meta;
extern ds_ctxt_t	*ds_data;

/* The last checkpoint LSN at the backup startup time */
extern lsn_t checkpoint_lsn_start;

extern xb_page_bitmap *changed_page_bitmap;

extern char		*xtrabackup_incremental;
extern my_bool		xtrabackup_incremental_force_scan;

extern lsn_t		metadata_to_lsn;

extern xb_stream_fmt_t	xtrabackup_stream_fmt;
extern ibool		xtrabackup_stream;

extern char		*xtrabackup_tables;
extern char		*xtrabackup_tables_file;
extern char		*xtrabackup_databases;
extern char		*xtrabackup_databases_file;
extern char		*xtrabackup_tables_exclude;
extern char		*xtrabackup_databases_exclude;

extern uint		xtrabackup_compress;

extern my_bool		xtrabackup_backup;
extern my_bool		xtrabackup_prepare;
extern my_bool		xtrabackup_copy_back;
extern my_bool		xtrabackup_move_back;
extern my_bool		xtrabackup_decrypt_decompress;

extern char		*innobase_data_file_path;
extern longlong		innobase_page_size;

extern int		xtrabackup_parallel;

extern my_bool		xb_close_files;
extern const char	*xtrabackup_compress_alg;

extern uint		xtrabackup_compress_threads;
extern ulonglong	xtrabackup_compress_chunk_size;

extern my_bool		xtrabackup_export;
extern char		*xtrabackup_extra_lsndir;
extern ulint		xtrabackup_log_copy_interval;
extern char		*xtrabackup_stream_str;
extern long		xtrabackup_throttle;
extern longlong		xtrabackup_use_memory;

extern my_bool		opt_galera_info;
extern my_bool		opt_slave_info;
extern my_bool		opt_no_lock;
extern my_bool		opt_safe_slave_backup;
extern my_bool		opt_rsync;
extern my_bool		opt_force_non_empty_dirs;
extern my_bool		opt_noversioncheck;
extern my_bool		opt_no_backup_locks;
extern my_bool		opt_decompress;
extern my_bool		opt_remove_original;
extern my_bool		opt_extended_validation;
extern my_bool		opt_encrypted_backup;
extern my_bool		opt_lock_ddl_per_table;

extern char		*opt_incremental_history_name;
extern char		*opt_incremental_history_uuid;

extern char		*opt_user;
extern char		*opt_password;
extern char		*opt_host;
extern char		*opt_defaults_group;
extern char		*opt_socket;
extern uint		opt_port;
extern char		*opt_log_bin;

extern const char 	*query_type_names[];

enum query_type_t {QUERY_TYPE_ALL, QUERY_TYPE_UPDATE,
			QUERY_TYPE_SELECT};

extern TYPELIB		query_type_typelib;

extern ulong		opt_lock_wait_query_type;
extern ulong		opt_kill_long_query_type;

extern uint		opt_kill_long_queries_timeout;
extern uint		opt_lock_wait_timeout;
extern uint		opt_lock_wait_threshold;
extern uint		opt_debug_sleep_before_unlock;
extern uint		opt_safe_slave_backup_timeout;

extern const char	*opt_history;

enum binlog_info_enum { BINLOG_INFO_OFF, BINLOG_INFO_ON,
			BINLOG_INFO_AUTO};

extern ulong opt_binlog_info;

void xtrabackup_io_throttling(void);
my_bool xb_write_delta_metadata(const char *filename,
				const xb_delta_info_t *info);

/************************************************************************
Checks if a table specified as a name in the form "database/name" (InnoDB 5.6)
or "./database/name.ibd" (InnoDB 5.5-) should be skipped from backup based on
the --tables or --tables-file options.

@return TRUE if the table should be skipped. */
my_bool
check_if_skip_table(
/******************/
	const char*	name);	/*!< in: path to the table */


/************************************************************************
Checks if a database specified by path should be skipped from backup based on
the --databases, --databases_file or --databases_exclude options.

@return TRUE if the table should be skipped. */
my_bool
check_if_skip_database_by_path(
	const char* path /*!< in: path to the db directory. */
);

/************************************************************************
Check if parameter is set in defaults file or via command line argument
@return true if parameter is set. */
bool
check_if_param_set(const char *param);

#if defined(HAVE_OPENSSL)
extern my_bool opt_ssl_verify_server_cert;
#endif


my_bool
xb_get_one_option(int optid,
		  const struct my_option *opt __attribute__((unused)),
		  char *argument);

const char*
xb_get_copy_action(const char *dflt = "Copying");

void mdl_lock_init();
void mdl_lock_table(ulint space_id);
void mdl_unlock_all();
bool ends_with(const char *str, const char *suffix);
#endif /* XB_XTRABACKUP_H */
