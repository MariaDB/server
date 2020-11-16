/*****************************************************************************

Copyright (c) 2000, 2020, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, 2009 Google Inc.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2012, Facebook Inc.
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

/** @file ha_innodb.cc */

#include "univ.i"

/* Include necessary SQL headers */
#include "ha_prototypes.h"
#include <debug_sync.h>
#include <gstream.h>
#include <log.h>
#include <mysys_err.h>
#include <innodb_priv.h>
#include <strfunc.h>
#include <sql_acl.h>
#include <sql_class.h>
#include <sql_show.h>
#include <sql_table.h>
#include <table_cache.h>
#include <my_check_opt.h>
#include <my_bitmap.h>
#include <mysql/service_thd_alloc.h>
#include <mysql/service_thd_wait.h>
#include "field.h"
#include "scope.h"
#include "srv0srv.h"

// MYSQL_PLUGIN_IMPORT extern my_bool lower_case_file_system;
// MYSQL_PLUGIN_IMPORT extern char mysql_unpacked_real_data_home[];

#include <my_service_manager.h>
#include <key.h>

/* Include necessary InnoDB headers */
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0bulk.h"
#include "btr0sea.h"
#include "buf0dblwr.h"
#include "buf0dump.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "btr0defragment.h"
#include "dict0crea.h"
#include "dict0dict.h"
#include "dict0priv.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fts0fts.h"
#include "fts0plugin.h"
#include "fts0priv.h"
#include "fts0types.h"
#include "ibuf0ibuf.h"
#include "lock0lock.h"
#include "log0crypt.h"
#include "mtr0mtr.h"
#include "os0file.h"
#include "page0zip.h"
#include "pars0pars.h"
#include "rem0types.h"
#include "row0import.h"
#include "row0ins.h"
#include "row0merge.h"
#include "row0mysql.h"
#include "row0quiesce.h"
#include "row0sel.h"
#include "row0upd.h"
#include "fil0crypt.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "rem0rec.h"
#ifdef UNIV_DEBUG
#include "trx0purge.h"
#endif /* UNIV_DEBUG */
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "fil0pagecompress.h"
#include "ut0mem.h"
#include "ut0mutex.h"
#include "row0ext.h"

#include <limits>

#define thd_get_trx_isolation(X) ((enum_tx_isolation)thd_tx_isolation(X))

extern "C" void thd_mark_transaction_to_rollback(MYSQL_THD thd, bool all);
unsigned long long thd_get_query_id(const MYSQL_THD thd);
void thd_clear_error(MYSQL_THD thd);

TABLE *find_fk_open_table(THD *thd, const char *db, size_t db_len,
			  const char *table, size_t table_len);
MYSQL_THD create_background_thd();
void destroy_background_thd(MYSQL_THD thd);
void reset_thd(MYSQL_THD thd);
TABLE *get_purge_table(THD *thd);
TABLE *open_purge_table(THD *thd, const char *db, size_t dblen,
			const char *tb, size_t tblen);
void close_thread_tables(THD* thd);

#ifdef MYSQL_DYNAMIC_PLUGIN
#define tc_size  400
#define tdc_size 400
#endif

#include <mysql/plugin.h>
#include <mysql/service_wsrep.h>

#include "ha_innodb.h"
#include "i_s.h"
#include "sync0sync.h"

#include <string>
#include <sstream>

#ifdef WITH_WSREP
#include "dict0priv.h"
#include <mysql/service_md5.h>
#include "wsrep_sst.h"
#endif /* WITH_WSREP */

/** to force correct commit order in binlog */
static mysql_mutex_t pending_checkpoint_mutex;

#define INSIDE_HA_INNOBASE_CC

#define EQ_CURRENT_THD(thd) ((thd) == current_thd)

struct handlerton* innodb_hton_ptr;

static const long AUTOINC_OLD_STYLE_LOCKING = 0;
static const long AUTOINC_NEW_STYLE_LOCKING = 1;
static const long AUTOINC_NO_LOCKING = 2;

static ulong innobase_open_files;
static long innobase_autoinc_lock_mode;

static ulonglong innobase_buffer_pool_size;

/** Percentage of the buffer pool to reserve for 'old' blocks.
Connected to buf_LRU_old_ratio. */
static uint innobase_old_blocks_pct;

static char*	innobase_data_file_path;
static char*	innobase_temp_data_file_path;

/* The default values for the following char* start-up parameters
are determined in innodb_init_params(). */

static char*	innobase_data_home_dir;
static char*	innobase_enable_monitor_counter;
static char*	innobase_disable_monitor_counter;
static char*	innobase_reset_monitor_counter;
static char*	innobase_reset_all_monitor_counter;

static ulong	innodb_flush_method;

/* This variable can be set in the server configure file, specifying
stopword table to be used */
static char*	innobase_server_stopword_table;

static my_bool	innobase_use_atomic_writes;
static my_bool	innobase_rollback_on_timeout;
static my_bool	innobase_create_status_file;
my_bool	innobase_stats_on_metadata;
static my_bool	innodb_optimize_fulltext_only;

static char*	innodb_version_str = (char*) INNODB_VERSION_STR;

extern uint srv_fil_crypt_rotate_key_age;
extern uint srv_n_fil_crypt_iops;

#ifdef UNIV_DEBUG
my_bool innodb_evict_tables_on_commit_debug;
#endif

/** File format constraint for ALTER TABLE */
ulong innodb_instant_alter_column_allowed;

/** Note we cannot use rec_format_enum because we do not allow
COMPRESSED row format for innodb_default_row_format option. */
enum default_row_format_enum {
	DEFAULT_ROW_FORMAT_REDUNDANT = 0,
	DEFAULT_ROW_FORMAT_COMPACT = 1,
	DEFAULT_ROW_FORMAT_DYNAMIC = 2,
};

/** Whether ROW_FORMAT=COMPRESSED tables are read-only */
static my_bool innodb_read_only_compressed;

/** A dummy variable */
static uint innodb_max_purge_lag_wait;

/** Wait for trx_sys_t::rseg_history_len to be below a limit. */
static void innodb_max_purge_lag_wait_update(THD *thd, st_mysql_sys_var *,
                                             void *, const void *limit)
{
  const uint l= *static_cast<const uint*>(limit);
  if (trx_sys.rseg_history_len <= l)
    return;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  while (trx_sys.rseg_history_len > l)
  {
    if (thd_kill_level(thd))
      break;
    srv_wake_purge_thread_if_not_active();
    os_thread_sleep(100000);
  }
  mysql_mutex_lock(&LOCK_global_system_variables);
}

static
void set_my_errno(int err)
{
	errno = err;
}

/** Checks whether the file name belongs to a partition of a table.
@param[in]	file_name	file name
@return pointer to the end of the table name part of the file name, or NULL */
static
char*
is_partition(
/*=========*/
	char*		file_name)
{
	/* We look for pattern #P# to see if the table is partitioned
	MariaDB table. */
	return strstr(file_name, table_name_t::part_suffix);
}



/** Return the InnoDB ROW_FORMAT enum value
@param[in]	row_format	row_format from "innodb_default_row_format"
@return InnoDB ROW_FORMAT value from rec_format_t enum. */
static
rec_format_t
get_row_format(
	ulong row_format)
{
	switch(row_format) {
	case DEFAULT_ROW_FORMAT_REDUNDANT:
		return(REC_FORMAT_REDUNDANT);
	case DEFAULT_ROW_FORMAT_COMPACT:
		return(REC_FORMAT_COMPACT);
	case DEFAULT_ROW_FORMAT_DYNAMIC:
		return(REC_FORMAT_DYNAMIC);
	default:
		ut_ad(0);
		return(REC_FORMAT_DYNAMIC);
	}
}

static ulong	innodb_default_row_format = DEFAULT_ROW_FORMAT_DYNAMIC;

/** Possible values for system variable "innodb_stats_method". The values
are defined the same as its corresponding MyISAM system variable
"myisam_stats_method"(see "myisam_stats_method_names"), for better usability */
static const char* innodb_stats_method_names[] = {
	"nulls_equal",
	"nulls_unequal",
	"nulls_ignored",
	NullS
};

/** Used to define an enumerate type of the system variable innodb_stats_method.
This is the same as "myisam_stats_method_typelib" */
static TYPELIB innodb_stats_method_typelib = {
	array_elements(innodb_stats_method_names) - 1,
	"innodb_stats_method_typelib",
	innodb_stats_method_names,
	NULL
};

/** Possible values of the parameter innodb_checksum_algorithm */
const char* innodb_checksum_algorithm_names[] = {
	"crc32",
	"strict_crc32",
	"innodb",
	"strict_innodb",
	"none",
	"strict_none",
	"full_crc32",
	"strict_full_crc32",
	NullS
};

/** Used to define an enumerate type of the system variable
innodb_checksum_algorithm. */
TYPELIB innodb_checksum_algorithm_typelib = {
	array_elements(innodb_checksum_algorithm_names) - 1,
	"innodb_checksum_algorithm_typelib",
	innodb_checksum_algorithm_names,
	NULL
};

/** Possible values for system variable "innodb_default_row_format". */
static const char* innodb_default_row_format_names[] = {
	"redundant",
	"compact",
	"dynamic",
	NullS
};

/** Used to define an enumerate type of the system variable
innodb_default_row_format. */
static TYPELIB innodb_default_row_format_typelib = {
	array_elements(innodb_default_row_format_names) - 1,
	"innodb_default_row_format_typelib",
	innodb_default_row_format_names,
	NULL
};

/** Names of allowed values of innodb_flush_method */
const char* innodb_flush_method_names[] = {
	"fsync",
	"O_DSYNC",
	"littlesync",
	"nosync",
	"O_DIRECT",
	"O_DIRECT_NO_FSYNC",
#ifdef _WIN32
	"unbuffered",
	"async_unbuffered" /* alias for "unbuffered" */,
	"normal" /* alias for "fsync" */,
#endif
	NullS
};

/** Enumeration of innodb_flush_method */
TYPELIB innodb_flush_method_typelib = {
	array_elements(innodb_flush_method_names) - 1,
	"innodb_flush_method_typelib",
	innodb_flush_method_names,
	NULL
};

/** Allowed values of innodb_change_buffering */
static const char* innodb_change_buffering_names[] = {
	"none",		/* IBUF_USE_NONE */
	"inserts",	/* IBUF_USE_INSERT */
	"deletes",	/* IBUF_USE_DELETE_MARK */
	"changes",	/* IBUF_USE_INSERT_DELETE_MARK */
	"purges",	/* IBUF_USE_DELETE */
	"all",		/* IBUF_USE_ALL */
	NullS
};

/** Enumeration of innodb_change_buffering */
static TYPELIB innodb_change_buffering_typelib = {
	array_elements(innodb_change_buffering_names) - 1,
	"innodb_change_buffering_typelib",
	innodb_change_buffering_names,
	NULL
};

/** Allowed values of innodb_instant_alter_column_allowed */
const char* innodb_instant_alter_column_allowed_names[] = {
	"never", /* compatible with MariaDB 5.5 to 10.2 */
	"add_last",/* allow instant ADD COLUMN ... LAST */
	"add_drop_reorder", /* allow instant ADD anywhere & DROP & reorder */
	NullS
};

/** Enumeration of innodb_instant_alter_column_allowed */
static TYPELIB innodb_instant_alter_column_allowed_typelib = {
	array_elements(innodb_instant_alter_column_allowed_names) - 1,
	"innodb_instant_alter_column_allowed_typelib",
	innodb_instant_alter_column_allowed_names,
	NULL
};

/** Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_retrieve_ranking(
	FT_INFO*	fts_hdl);
/** Free the memory for the FTS handler
@param[in,out]	fts_hdl	FTS handler */
static
void
innobase_fts_close_ranking(
	FT_INFO*	fts_hdl);
/** Find and Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_find_ranking(
	FT_INFO*	fts_hdl,
	uchar*,
	uint);

/* Call back function array defined by MySQL and used to
retrieve FTS results. */
const struct _ft_vft ft_vft_result = {NULL,
				      innobase_fts_find_ranking,
				      innobase_fts_close_ranking,
				      innobase_fts_retrieve_ranking,
				      NULL};

/** @return version of the extended FTS API */
static
uint
innobase_fts_get_version()
{
	/* Currently this doesn't make much sense as returning
	HA_CAN_FULLTEXT_EXT automatically mean this version is supported.
	This supposed to ease future extensions.  */
	return(2);
}

/** @return Which part of the extended FTS API is supported */
static
ulonglong
innobase_fts_flags()
{
	return(FTS_ORDERED_RESULT | FTS_DOCID_IN_RESULT);
}

/** Find and Retrieve the FTS doc_id for the current result row
@param[in,out]	fts_hdl	FTS handler
@return the document ID */
static
ulonglong
innobase_fts_retrieve_docid(
	FT_INFO_EXT*	fts_hdl);

/** Find and retrieve the size of the current result
@param[in,out]	fts_hdl	FTS handler
@return number of matching rows */
static
ulonglong
innobase_fts_count_matches(
	FT_INFO_EXT*	fts_hdl)	/*!< in: FTS handler */
{
	NEW_FT_INFO*	handle = reinterpret_cast<NEW_FT_INFO*>(fts_hdl);

	if (handle->ft_result->rankings_by_id != NULL) {
		return(rbt_size(handle->ft_result->rankings_by_id));
	} else {
		return(0);
	}
}

const struct _ft_vft_ext ft_vft_ext_result = {innobase_fts_get_version,
					      innobase_fts_flags,
					      innobase_fts_retrieve_docid,
					      innobase_fts_count_matches};

#ifdef HAVE_PSI_INTERFACE
# define PSI_KEY(n) {&n##_key, #n, 0}
/* All RWLOCK used in Innodb are SX-locks */
# define PSI_RWLOCK_KEY(n) {&n##_key, #n, PSI_RWLOCK_FLAG_SX}

/* Keys to register pthread mutexes/cond in the current file with
performance schema */
static mysql_pfs_key_t	commit_cond_mutex_key;
static mysql_pfs_key_t	commit_cond_key;
static mysql_pfs_key_t	pending_checkpoint_mutex_key;

static PSI_mutex_info	all_pthread_mutexes[] = {
	PSI_KEY(commit_cond_mutex),
	PSI_KEY(pending_checkpoint_mutex),
};

static PSI_cond_info	all_innodb_conds[] = {
	PSI_KEY(commit_cond)
};

# ifdef UNIV_PFS_MUTEX
/* all_innodb_mutexes array contains mutexes that are
performance schema instrumented if "UNIV_PFS_MUTEX"
is defined */
static PSI_mutex_info all_innodb_mutexes[] = {
	PSI_KEY(buf_pool_mutex),
	PSI_KEY(dict_foreign_err_mutex),
	PSI_KEY(dict_sys_mutex),
	PSI_KEY(recalc_pool_mutex),
	PSI_KEY(fil_system_mutex),
	PSI_KEY(flush_list_mutex),
	PSI_KEY(fts_delete_mutex),
	PSI_KEY(fts_doc_id_mutex),
	PSI_KEY(log_flush_order_mutex),
	PSI_KEY(ibuf_bitmap_mutex),
	PSI_KEY(ibuf_mutex),
	PSI_KEY(ibuf_pessimistic_insert_mutex),
	PSI_KEY(index_online_log),
	PSI_KEY(log_sys_mutex),
	PSI_KEY(page_zip_stat_per_index_mutex),
	PSI_KEY(purge_sys_pq_mutex),
	PSI_KEY(recv_sys_mutex),
	PSI_KEY(redo_rseg_mutex),
	PSI_KEY(noredo_rseg_mutex),
#  ifdef UNIV_DEBUG
	PSI_KEY(rw_lock_debug_mutex),
#  endif /* UNIV_DEBUG */
	PSI_KEY(rw_lock_list_mutex),
	PSI_KEY(srv_innodb_monitor_mutex),
	PSI_KEY(srv_misc_tmpfile_mutex),
	PSI_KEY(srv_monitor_file_mutex),
	PSI_KEY(buf_dblwr_mutex),
	PSI_KEY(trx_pool_mutex),
	PSI_KEY(trx_pool_manager_mutex),
	PSI_KEY(lock_mutex),
	PSI_KEY(lock_wait_mutex),
	PSI_KEY(trx_mutex),
	PSI_KEY(srv_threads_mutex),
	PSI_KEY(rtr_active_mutex),
	PSI_KEY(rtr_match_mutex),
	PSI_KEY(rtr_path_mutex),
	PSI_KEY(trx_sys_mutex),
};
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_RWLOCK
/* all_innodb_rwlocks array contains rwlocks that are
performance schema instrumented if "UNIV_PFS_RWLOCK"
is defined */
static PSI_rwlock_info all_innodb_rwlocks[] = {
	PSI_RWLOCK_KEY(btr_search_latch),
#  ifndef PFS_SKIP_BUFFER_MUTEX_RWLOCK
	PSI_RWLOCK_KEY(buf_block_lock),
#  endif /* !PFS_SKIP_BUFFER_MUTEX_RWLOCK */
#  ifdef UNIV_DEBUG
	PSI_RWLOCK_KEY(buf_block_debug_latch),
#  endif /* UNIV_DEBUG */
	PSI_RWLOCK_KEY(dict_operation_lock),
	PSI_RWLOCK_KEY(fil_space_latch),
	PSI_RWLOCK_KEY(fts_cache_rw_lock),
	PSI_RWLOCK_KEY(fts_cache_init_rw_lock),
	PSI_RWLOCK_KEY(trx_i_s_cache_lock),
	PSI_RWLOCK_KEY(trx_purge_latch),
	PSI_RWLOCK_KEY(index_tree_rw_lock),
};
# endif /* UNIV_PFS_RWLOCK */

# ifdef UNIV_PFS_THREAD
/* all_innodb_threads array contains threads that are
performance schema instrumented if "UNIV_PFS_THREAD"
is defined */
static PSI_thread_info	all_innodb_threads[] = {
	PSI_KEY(page_cleaner_thread),
	PSI_KEY(trx_rollback_clean_thread),
	PSI_KEY(thread_pool_thread)
};
# endif /* UNIV_PFS_THREAD */

# ifdef UNIV_PFS_IO
/* all_innodb_files array contains the type of files that are
performance schema instrumented if "UNIV_PFS_IO" is defined */
static PSI_file_info	all_innodb_files[] = {
	PSI_KEY(innodb_data_file),
	PSI_KEY(innodb_log_file),
	PSI_KEY(innodb_temp_file)
};
# endif /* UNIV_PFS_IO */
#endif /* HAVE_PSI_INTERFACE */

static void innodb_remember_check_sysvar_funcs();
mysql_var_check_func check_sysvar_enum;
mysql_var_check_func check_sysvar_int;

// should page compression be used by default for new tables
static MYSQL_THDVAR_BOOL(compression_default, PLUGIN_VAR_OPCMDARG,
  "Is compression the default for new tables", 
  NULL, NULL, FALSE);

/** Update callback for SET [SESSION] innodb_default_encryption_key_id */
static void
innodb_default_encryption_key_id_update(THD* thd, st_mysql_sys_var* var,
					void* var_ptr, const void *save)
{
	uint key_id = *static_cast<const uint*>(save);
	if (key_id != FIL_DEFAULT_ENCRYPTION_KEY
	    && !encryption_key_id_exists(key_id)) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_default_encryption_key=%u"
				    " is not available", key_id);
	}
	*static_cast<uint*>(var_ptr) = key_id;
}

static MYSQL_THDVAR_UINT(default_encryption_key_id, PLUGIN_VAR_RQCMDARG,
			 "Default encryption key id used for table encryption.",
			 NULL, innodb_default_encryption_key_id_update,
			 FIL_DEFAULT_ENCRYPTION_KEY, 1, UINT_MAX32, 0);

/**
  Structure for CREATE TABLE options (table options).
  It needs to be called ha_table_option_struct.

  The option values can be specified in the CREATE TABLE at the end:
  CREATE TABLE ( ... ) *here*
*/

ha_create_table_option innodb_table_option_list[]=
{
  /* With this option user can enable page compression feature for the
  table */
  HA_TOPTION_SYSVAR("PAGE_COMPRESSED", page_compressed, compression_default),
  /* With this option user can set zip compression level for page
  compression for this table*/
  HA_TOPTION_NUMBER("PAGE_COMPRESSION_LEVEL", page_compression_level, 0, 1, 9, 1),
  /* With this option the user can enable encryption for the table */
  HA_TOPTION_ENUM("ENCRYPTED", encryption, "DEFAULT,YES,NO", 0),
  /* With this option the user defines the key identifier using for the encryption */
  HA_TOPTION_SYSVAR("ENCRYPTION_KEY_ID", encryption_key_id, default_encryption_key_id),

  HA_TOPTION_END
};

/*************************************************************//**
Check whether valid argument given to innodb_ft_*_stopword_table.
This function is registered as a callback with MySQL.
@return 0 for valid stopword table */
static
int
innodb_stopword_table_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value);	/*!< in: incoming string */

static bool is_mysql_datadir_path(const char *path);

/** Validate passed-in "value" is a valid directory name.
This function is registered as a callback with MySQL.
@param[in,out]	thd	thread handle
@param[in]	var	pointer to system variable
@param[out]	save	immediate result for update
@param[in]	value	incoming string
@return 0 for valid name */
static
int
innodb_tmpdir_validate(
	THD*				thd,
	struct st_mysql_sys_var*,
	void*				save,
	struct st_mysql_value*		value)
{

	char*	alter_tmp_dir;
	char*	innodb_tmp_dir;
	char	buff[OS_FILE_MAX_PATH];
	int	len = sizeof(buff);
	char	tmp_abs_path[FN_REFLEN + 2];

	ut_ad(save != NULL);
	ut_ad(value != NULL);

	if (check_global_access(thd, FILE_ACL)) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: FILE Permissions required");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	alter_tmp_dir = (char*) value->val_str(value, buff, &len);

	if (!alter_tmp_dir) {
		*static_cast<const char**>(save) = alter_tmp_dir;
		return(0);
	}

	if (strlen(alter_tmp_dir) > FN_REFLEN) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"Path length should not exceed %d bytes", FN_REFLEN);
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	os_normalize_path(alter_tmp_dir);
	my_realpath(tmp_abs_path, alter_tmp_dir, 0);
	size_t	tmp_abs_len = strlen(tmp_abs_path);

	if (my_access(tmp_abs_path, F_OK)) {

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Path doesn't exist.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	} else if (my_access(tmp_abs_path, R_OK | W_OK)) {
		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Server doesn't have permission in "
			"the given location.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	MY_STAT stat_info_dir;

	if (my_stat(tmp_abs_path, &stat_info_dir, MYF(0))) {
		if ((stat_info_dir.st_mode & S_IFDIR) != S_IFDIR) {

			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_WRONG_ARGUMENTS,
				"Given path is not a directory. ");
			*static_cast<const char**>(save) = NULL;
			return(1);
		}
	}

	if (!is_mysql_datadir_path(tmp_abs_path)) {

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"InnoDB: Path Location should not be same as "
			"mysql data directory location.");
		*static_cast<const char**>(save) = NULL;
		return(1);
	}

	innodb_tmp_dir = static_cast<char*>(
		thd_memdup(thd, tmp_abs_path, tmp_abs_len + 1));
	*static_cast<const char**>(save) = innodb_tmp_dir;
	return(0);
}

/******************************************************************//**
Maps a MySQL trx isolation level code to the InnoDB isolation level code
@return	InnoDB isolation level */
static inline
uint
innobase_map_isolation_level(
/*=========================*/
	enum_tx_isolation	iso);	/*!< in: MySQL isolation level code */

/** Gets field offset for a field in a table.
@param[in]	table	MySQL table object
@param[in]	field	MySQL field object (from table->field array)
@return offset */
static inline
uint
get_field_offset(
	const TABLE*	table,
	const Field*	field)
{
	return field->offset(table->record[0]);
}


/*************************************************************//**
Check for a valid value of innobase_compression_algorithm.
@return	0 for valid innodb_compression_algorithm. */
static
int
innodb_compression_algorithm_validate(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value);	/*!< in: incoming string */

static ibool innodb_have_lzo=IF_LZO(1, 0);
static ibool innodb_have_lz4=IF_LZ4(1, 0);
static ibool innodb_have_lzma=IF_LZMA(1, 0);
static ibool innodb_have_bzip2=IF_BZIP2(1, 0);
static ibool innodb_have_snappy=IF_SNAPPY(1, 0);
static ibool innodb_have_punch_hole=IF_PUNCH_HOLE(1, 0);

static
int
innodb_encrypt_tables_validate(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value);	/*!< in: incoming string */

static const char innobase_hton_name[]= "InnoDB";

static MYSQL_THDVAR_BOOL(table_locks, PLUGIN_VAR_OPCMDARG,
  "Enable InnoDB locking in LOCK TABLES",
  /* check_func */ NULL, /* update_func */ NULL,
  /* default */ TRUE);

static MYSQL_THDVAR_BOOL(strict_mode, PLUGIN_VAR_OPCMDARG,
  "Use strict mode when evaluating create options.",
  NULL, NULL, TRUE);

static MYSQL_THDVAR_BOOL(ft_enable_stopword, PLUGIN_VAR_OPCMDARG,
  "Create FTS index with stopword.",
  NULL, NULL,
  /* default */ TRUE);

static MYSQL_THDVAR_ULONG(lock_wait_timeout, PLUGIN_VAR_RQCMDARG,
  "Timeout in seconds an InnoDB transaction may wait for a lock before being rolled back. Values above 100000000 disable the timeout.",
  NULL, NULL, 50, 0, 1024 * 1024 * 1024, 0);

static MYSQL_THDVAR_STR(ft_user_stopword_table,
  PLUGIN_VAR_OPCMDARG|PLUGIN_VAR_MEMALLOC,
  "User supplied stopword table name, effective in the session level.",
  innodb_stopword_table_validate, NULL, NULL);

static MYSQL_THDVAR_STR(tmpdir,
  PLUGIN_VAR_OPCMDARG|PLUGIN_VAR_MEMALLOC,
  "Directory for temporary non-tablespace files.",
  innodb_tmpdir_validate, NULL, NULL);

static SHOW_VAR innodb_status_variables[]= {
#ifdef BTR_CUR_HASH_ADAPT
  {"adaptive_hash_hash_searches", &btr_cur_n_sea, SHOW_SIZE_T},
  {"adaptive_hash_non_hash_searches", &btr_cur_n_non_sea, SHOW_SIZE_T},
#endif
  {"background_log_sync", &srv_log_writes_and_flush, SHOW_SIZE_T},
#if defined(LINUX_NATIVE_AIO)
  {"buffered_aio_submitted", &srv_stats.buffered_aio_submitted, SHOW_SIZE_T},
#endif
  {"buffer_pool_dump_status",
  (char*) &export_vars.innodb_buffer_pool_dump_status,	  SHOW_CHAR},
  {"buffer_pool_load_status",
  (char*) &export_vars.innodb_buffer_pool_load_status,	  SHOW_CHAR},
  {"buffer_pool_resize_status",
  (char*) &export_vars.innodb_buffer_pool_resize_status,  SHOW_CHAR},
  {"buffer_pool_load_incomplete",
  &export_vars.innodb_buffer_pool_load_incomplete,        SHOW_BOOL},
  {"buffer_pool_pages_data",
   &export_vars.innodb_buffer_pool_pages_data, SHOW_SIZE_T},
  {"buffer_pool_bytes_data",
   &export_vars.innodb_buffer_pool_bytes_data, SHOW_SIZE_T},
  {"buffer_pool_pages_dirty",
   &export_vars.innodb_buffer_pool_pages_dirty, SHOW_SIZE_T},
  {"buffer_pool_bytes_dirty",
   &export_vars.innodb_buffer_pool_bytes_dirty, SHOW_SIZE_T},
  {"buffer_pool_pages_flushed", &buf_flush_page_count, SHOW_SIZE_T},
  {"buffer_pool_pages_free",
   &export_vars.innodb_buffer_pool_pages_free, SHOW_SIZE_T},
#ifdef UNIV_DEBUG
  {"buffer_pool_pages_latched",
   &export_vars.innodb_buffer_pool_pages_latched, SHOW_SIZE_T},
#endif /* UNIV_DEBUG */
  {"buffer_pool_pages_made_not_young",
   &export_vars.innodb_buffer_pool_pages_made_not_young, SHOW_SIZE_T},
  {"buffer_pool_pages_made_young",
   &export_vars.innodb_buffer_pool_pages_made_young, SHOW_SIZE_T},
  {"buffer_pool_pages_misc",
   &export_vars.innodb_buffer_pool_pages_misc, SHOW_SIZE_T},
  {"buffer_pool_pages_old",
   &export_vars.innodb_buffer_pool_pages_old, SHOW_SIZE_T},
  {"buffer_pool_pages_total",
   &export_vars.innodb_buffer_pool_pages_total, SHOW_SIZE_T},
  {"buffer_pool_pages_LRU_flushed", &buf_lru_flush_page_count, SHOW_SIZE_T},
  {"buffer_pool_read_ahead_rnd",
   &export_vars.innodb_buffer_pool_read_ahead_rnd, SHOW_SIZE_T},
  {"buffer_pool_read_ahead",
   &export_vars.innodb_buffer_pool_read_ahead, SHOW_SIZE_T},
  {"buffer_pool_read_ahead_evicted",
   &export_vars.innodb_buffer_pool_read_ahead_evicted, SHOW_SIZE_T},
  {"buffer_pool_read_requests",
   &export_vars.innodb_buffer_pool_read_requests, SHOW_SIZE_T},
  {"buffer_pool_reads",
   &export_vars.innodb_buffer_pool_reads, SHOW_SIZE_T},
  {"buffer_pool_wait_free",
   &export_vars.innodb_buffer_pool_wait_free, SHOW_SIZE_T},
  {"buffer_pool_write_requests",
   &export_vars.innodb_buffer_pool_write_requests, SHOW_SIZE_T},
  {"checkpoint_age", &export_vars.innodb_checkpoint_age, SHOW_SIZE_T},
  {"checkpoint_max_age", &export_vars.innodb_checkpoint_max_age, SHOW_SIZE_T},
  {"data_fsyncs", &export_vars.innodb_data_fsyncs, SHOW_SIZE_T},
  {"data_pending_fsyncs", &export_vars.innodb_data_pending_fsyncs,SHOW_SIZE_T},
  {"data_pending_reads", &export_vars.innodb_data_pending_reads, SHOW_SIZE_T},
  {"data_pending_writes", &export_vars.innodb_data_pending_writes,SHOW_SIZE_T},
  {"data_read", &export_vars.innodb_data_read, SHOW_SIZE_T},
  {"data_reads", &export_vars.innodb_data_reads, SHOW_SIZE_T},
  {"data_writes", &export_vars.innodb_data_writes, SHOW_SIZE_T},
  {"data_written", &export_vars.innodb_data_written, SHOW_SIZE_T},
  {"dblwr_pages_written", &export_vars.innodb_dblwr_pages_written,SHOW_SIZE_T},
  {"dblwr_writes", &export_vars.innodb_dblwr_writes, SHOW_SIZE_T},
  {"deadlocks", &srv_stats.lock_deadlock_count, SHOW_SIZE_T},
  {"history_list_length", &export_vars.innodb_history_list_length,SHOW_SIZE_T},
  {"ibuf_discarded_delete_marks", &ibuf.n_discarded_ops[IBUF_OP_DELETE_MARK],
   SHOW_SIZE_T},
  {"ibuf_discarded_deletes", &ibuf.n_discarded_ops[IBUF_OP_DELETE],
   SHOW_SIZE_T},
  {"ibuf_discarded_inserts", &ibuf.n_discarded_ops[IBUF_OP_INSERT],
   SHOW_SIZE_T},
  {"ibuf_free_list", &ibuf.free_list_len, SHOW_SIZE_T},
  {"ibuf_merged_delete_marks", &ibuf.n_merged_ops[IBUF_OP_DELETE_MARK],
   SHOW_SIZE_T},
  {"ibuf_merged_deletes", &ibuf.n_merged_ops[IBUF_OP_DELETE], SHOW_SIZE_T},
  {"ibuf_merged_inserts", &ibuf.n_merged_ops[IBUF_OP_INSERT], SHOW_SIZE_T},
  {"ibuf_merges", &ibuf.n_merges, SHOW_SIZE_T},
  {"ibuf_segment_size", &ibuf.seg_size, SHOW_SIZE_T},
  {"ibuf_size", &ibuf.size, SHOW_SIZE_T},
  {"log_waits", &export_vars.innodb_log_waits, SHOW_SIZE_T},
  {"log_write_requests", &export_vars.innodb_log_write_requests, SHOW_SIZE_T},
  {"log_writes", &export_vars.innodb_log_writes, SHOW_SIZE_T},
  {"lsn_current", &export_vars.innodb_lsn_current, SHOW_ULONGLONG},
  {"lsn_flushed", &export_vars.innodb_lsn_flushed, SHOW_ULONGLONG},
  {"lsn_last_checkpoint", &export_vars.innodb_lsn_last_checkpoint,
   SHOW_ULONGLONG},
  {"master_thread_active_loops", &srv_main_active_loops, SHOW_SIZE_T},
  {"master_thread_idle_loops", &srv_main_idle_loops, SHOW_SIZE_T},
  {"max_trx_id", &export_vars.innodb_max_trx_id, SHOW_ULONGLONG},
#ifdef BTR_CUR_HASH_ADAPT
  {"mem_adaptive_hash", &export_vars.innodb_mem_adaptive_hash, SHOW_SIZE_T},
#endif
  {"mem_dictionary", &export_vars.innodb_mem_dictionary, SHOW_SIZE_T},
  {"os_log_fsyncs", &export_vars.innodb_os_log_fsyncs, SHOW_SIZE_T},
  {"os_log_pending_fsyncs", &export_vars.innodb_os_log_pending_fsyncs,
   SHOW_SIZE_T},
  {"os_log_pending_writes", &export_vars.innodb_os_log_pending_writes,
   SHOW_SIZE_T},
  {"os_log_written", &export_vars.innodb_os_log_written, SHOW_SIZE_T},
  {"page_size", &srv_page_size, SHOW_ULONG},
  {"pages_created", &export_vars.innodb_pages_created, SHOW_SIZE_T},
  {"pages_read", &export_vars.innodb_pages_read, SHOW_SIZE_T},
  {"pages_written", &export_vars.innodb_pages_written, SHOW_SIZE_T},
  {"row_lock_current_waits", &export_vars.innodb_row_lock_current_waits,
   SHOW_SIZE_T},
  {"row_lock_time", &export_vars.innodb_row_lock_time, SHOW_LONGLONG},
  {"row_lock_time_avg", &export_vars.innodb_row_lock_time_avg, SHOW_SIZE_T},
  {"row_lock_time_max", &export_vars.innodb_row_lock_time_max, SHOW_SIZE_T},
  {"row_lock_waits", &export_vars.innodb_row_lock_waits, SHOW_SIZE_T},
  {"rows_deleted", &export_vars.innodb_rows_deleted, SHOW_SIZE_T},
  {"rows_inserted", &export_vars.innodb_rows_inserted, SHOW_SIZE_T},
  {"rows_read", &export_vars.innodb_rows_read, SHOW_SIZE_T},
  {"rows_updated", &export_vars.innodb_rows_updated, SHOW_SIZE_T},
  {"system_rows_deleted", &export_vars.innodb_system_rows_deleted,SHOW_SIZE_T},
  {"system_rows_inserted", &export_vars.innodb_system_rows_inserted,
   SHOW_SIZE_T},
  {"system_rows_read", &export_vars.innodb_system_rows_read, SHOW_SIZE_T},
  {"system_rows_updated", &export_vars.innodb_system_rows_updated,
   SHOW_SIZE_T},
  {"num_open_files", &fil_system.n_open, SHOW_SIZE_T},
  {"truncated_status_writes", &export_vars.innodb_truncated_status_writes,
   SHOW_SIZE_T},
  {"available_undo_logs", &srv_available_undo_logs, SHOW_ULONG},
  {"undo_truncations", &export_vars.innodb_undo_truncations, SHOW_ULONG},

  /* Status variables for page compression */
  {"page_compression_saved",
   &export_vars.innodb_page_compression_saved, SHOW_LONGLONG},
  {"num_index_pages_written",
   &export_vars.innodb_index_pages_written, SHOW_LONGLONG},
  {"num_non_index_pages_written",
   &export_vars.innodb_non_index_pages_written, SHOW_LONGLONG},
  {"num_pages_page_compressed",
   &export_vars.innodb_pages_page_compressed, SHOW_LONGLONG},
  {"num_page_compressed_trim_op",
   &export_vars.innodb_page_compressed_trim_op, SHOW_LONGLONG},
  {"num_pages_page_decompressed",
   &export_vars.innodb_pages_page_decompressed, SHOW_LONGLONG},
  {"num_pages_page_compression_error",
   &export_vars.innodb_pages_page_compression_error, SHOW_LONGLONG},
  {"num_pages_encrypted",
   &export_vars.innodb_pages_encrypted, SHOW_LONGLONG},
  {"num_pages_decrypted",
   &export_vars.innodb_pages_decrypted, SHOW_LONGLONG},
  {"have_lz4", &innodb_have_lz4, SHOW_BOOL},
  {"have_lzo", &innodb_have_lzo, SHOW_BOOL},
  {"have_lzma", &innodb_have_lzma, SHOW_BOOL},
  {"have_bzip2", &innodb_have_bzip2, SHOW_BOOL},
  {"have_snappy", &innodb_have_snappy, SHOW_BOOL},
  {"have_punch_hole", &innodb_have_punch_hole, SHOW_BOOL},

  /* Defragmentation */
  {"defragment_compression_failures",
   &export_vars.innodb_defragment_compression_failures, SHOW_SIZE_T},
  {"defragment_failures", &export_vars.innodb_defragment_failures,SHOW_SIZE_T},
  {"defragment_count", &export_vars.innodb_defragment_count, SHOW_SIZE_T},

  {"instant_alter_column",
   &export_vars.innodb_instant_alter_column, SHOW_ULONG},

  /* Online alter table status variables */
  {"onlineddl_rowlog_rows",
   &export_vars.innodb_onlineddl_rowlog_rows, SHOW_SIZE_T},
  {"onlineddl_rowlog_pct_used",
   &export_vars.innodb_onlineddl_rowlog_pct_used, SHOW_SIZE_T},
  {"onlineddl_pct_progress",
   &export_vars.innodb_onlineddl_pct_progress, SHOW_SIZE_T},

  /* Times secondary index lookup triggered cluster lookup and
  times prefix optimization avoided triggering cluster lookup */
  {"secondary_index_triggered_cluster_reads",
   &export_vars.innodb_sec_rec_cluster_reads, SHOW_SIZE_T},
  {"secondary_index_triggered_cluster_reads_avoided",
   &export_vars.innodb_sec_rec_cluster_reads_avoided, SHOW_SIZE_T},

  /* Encryption */
  {"encryption_rotation_pages_read_from_cache",
   &export_vars.innodb_encryption_rotation_pages_read_from_cache, SHOW_SIZE_T},
  {"encryption_rotation_pages_read_from_disk",
   &export_vars.innodb_encryption_rotation_pages_read_from_disk, SHOW_SIZE_T},
  {"encryption_rotation_pages_modified",
   &export_vars.innodb_encryption_rotation_pages_modified, SHOW_SIZE_T},
  {"encryption_rotation_pages_flushed",
   &export_vars.innodb_encryption_rotation_pages_flushed, SHOW_SIZE_T},
  {"encryption_rotation_estimated_iops",
   &export_vars.innodb_encryption_rotation_estimated_iops, SHOW_SIZE_T},
  {"encryption_key_rotation_list_length",
   &export_vars.innodb_key_rotation_list_length, SHOW_LONGLONG},
  {"encryption_n_merge_blocks_encrypted",
   &export_vars.innodb_n_merge_blocks_encrypted, SHOW_LONGLONG},
  {"encryption_n_merge_blocks_decrypted",
   &export_vars.innodb_n_merge_blocks_decrypted, SHOW_LONGLONG},
  {"encryption_n_rowlog_blocks_encrypted",
   &export_vars.innodb_n_rowlog_blocks_encrypted, SHOW_LONGLONG},
  {"encryption_n_rowlog_blocks_decrypted",
   &export_vars.innodb_n_rowlog_blocks_decrypted, SHOW_LONGLONG},
  {"encryption_n_temp_blocks_encrypted",
   &export_vars.innodb_n_temp_blocks_encrypted, SHOW_LONGLONG},
  {"encryption_n_temp_blocks_decrypted",
   &export_vars.innodb_n_temp_blocks_decrypted, SHOW_LONGLONG},
  {"encryption_num_key_requests", &export_vars.innodb_encryption_key_requests,
   SHOW_LONGLONG},

  {NullS, NullS, SHOW_LONG}
};

/*****************************************************************//**
Frees a possible InnoDB trx object associated with the current THD.
@return 0 or error number */
static
int
innobase_close_connection(
/*======================*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd);		/*!< in: MySQL thread handle for
					which to close the connection */

/** Cancel any pending lock request associated with the current THD.
@sa THD::awake() @sa ha_kill_query() */
static void innobase_kill_query(handlerton*, THD* thd, enum thd_kill_levels);
static void innobase_commit_ordered(handlerton *hton, THD* thd, bool all);

/*****************************************************************//**
Commits a transaction in an InnoDB database or marks an SQL statement
ended.
@return 0 */
static
int
innobase_commit(
/*============*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd,		/*!< in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	bool		commit_trx);	/*!< in: true - commit transaction
					false - the current SQL statement
					ended */

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback(
/*==============*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back */
	bool		rollback_trx);	/*!< in: TRUE - rollback entire
					transaction FALSE - rollback the current
					statement only */

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback_to_savepoint(
/*===========================*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be rolled back to savepoint */
	void*		savepoint);	/*!< in: savepoint data */

/*****************************************************************//**
Check whether innodb state allows to safely release MDL locks after
rollback to savepoint.
@return true if it is safe, false if its not safe. */
static
bool
innobase_rollback_to_savepoint_can_release_mdl(
/*===========================================*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd);		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be rolled back to savepoint */

/*****************************************************************//**
Sets a transaction savepoint.
@return always 0, that is, always succeeds */
static
int
innobase_savepoint(
/*===============*/
	handlerton*	hton,		/*!< in/out: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user's XA transaction for which
					we need to take a savepoint */
	void*		savepoint);	/*!< in: savepoint data */

/*****************************************************************//**
Release transaction savepoint name.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_release_savepoint(
/*=======================*/
	handlerton*	hton,		/*!< in/out: handlerton for InnoDB */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction's
					savepoint should be released */
	void*		savepoint);	/*!< in: savepoint data */

static void innobase_checkpoint_request(handlerton *hton, void *cookie);

/** @brief Adjust some InnoDB startup parameters based on file contents
or innodb_page_size. */
static
void
innodb_params_adjust();

/*******************************************************************//**
This function is used to prepare an X/Open XA distributed transaction.
@return 0 or error number */
static
int
innobase_xa_prepare(
/*================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be prepared */
	bool		all);		/*!< in: true - prepare transaction
					false - the current SQL statement
					ended */
/*******************************************************************//**
This function is used to recover X/Open XA distributed transactions.
@return number of prepared transactions stored in xid_list */
static
int
innobase_xa_recover(
/*================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	XID*		xid_list,	/*!< in/out: prepared transactions */
	uint		len);		/*!< in: number of slots in xid_list */
/*******************************************************************//**
This function is used to commit one X/Open XA distributed transaction
which is in the prepared state
@return 0 or error number */
static
int
innobase_commit_by_xid(
/*===================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	XID*		xid);		/*!< in: X/Open XA transaction
					identification */
/** Remove all tables in the named database inside InnoDB.
@param[in]	hton	handlerton from InnoDB
@param[in]	path	Database path; Inside InnoDB the name of the last
directory in the path is used as the database name.
For example, in 'mysql/data/test' the database name is 'test'. */
static
void
innobase_drop_database(
	handlerton*	hton,
	char*		path);

/** Shut down the InnoDB storage engine.
@return	0 */
static
int
innobase_end(handlerton*, ha_panic_function);

/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
assigns a new snapshot for a consistent read if the transaction does not yet
have one.
@return 0 */
static
int
innobase_start_trx_and_assign_read_view(
/*====================================*/
	handlerton*	hton,		/* in: InnoDB handlerton */
	THD*		thd);		/* in: MySQL thread handle of the
					user for whom the transaction should
					be committed */

/** Flush InnoDB redo logs to the file system.
@param[in]	hton			InnoDB handlerton
@param[in]	binlog_group_flush	true if we got invoked by binlog
group commit during flush stage, false in other cases.
@return false */
static
bool
innobase_flush_logs(
	handlerton*	hton,
	bool		binlog_group_flush)
{
	DBUG_ENTER("innobase_flush_logs");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (srv_read_only_mode) {
		DBUG_RETURN(false);
	}

	/* If !binlog_group_flush, we got invoked by FLUSH LOGS or similar.
	Else, we got invoked by binlog group commit during flush stage. */

	if (binlog_group_flush && srv_flush_log_at_trx_commit == 0) {
		/* innodb_flush_log_at_trx_commit=0
		(write and sync once per second).
		Do not flush the redo log during binlog group commit. */
		DBUG_RETURN(false);
	}

	/* Flush the redo log buffer to the redo log file.
	Sync it to disc if we are in FLUSH LOGS, or if
	innodb_flush_log_at_trx_commit=1
	(write and sync at each commit). */
	log_buffer_flush_to_disk(!binlog_group_flush
				 || srv_flush_log_at_trx_commit == 1);

	DBUG_RETURN(false);
}

/** Flush InnoDB redo logs to the file system.
@param[in]	hton			InnoDB handlerton
@param[in]	binlog_group_flush	true if we got invoked by binlog
group commit during flush stage, false in other cases.
@return false */
static
bool
innobase_flush_logs(
	handlerton*	hton)
{
	return innobase_flush_logs(hton, true);
}

/************************************************************************//**
Implements the SHOW ENGINE INNODB STATUS command. Sends the output of the
InnoDB Monitor to the client.
@return 0 on success */
static
int
innodb_show_status(
/*===============*/
	handlerton*	hton,		/*!< in: the innodb handlerton */
	THD*		thd,		/*!< in: the MySQL query thread of
					the caller */
	stat_print_fn*	stat_print);
/************************************************************************//**
Return 0 on success and non-zero on failure. Note: the bool return type
seems to be abused here, should be an int. */
static
bool
innobase_show_status(
/*=================*/
	handlerton*		hton,	/*!< in: the innodb handlerton */
	THD*			thd,	/*!< in: the MySQL query thread of
					the caller */
	stat_print_fn*		stat_print,
	enum ha_stat_type	stat_type);

/****************************************************************//**
Parse and enable InnoDB monitor counters during server startup.
User can enable monitor counters/groups by specifying
"loose-innodb_monitor_enable = monitor_name1;monitor_name2..."
in server configuration file or at the command line. */
static
void
innodb_enable_monitor_at_startup(
/*=============================*/
	char*	str);	/*!< in: monitor counter enable list */

#ifdef MYSQL_STORE_FTS_DOC_ID
/** Store doc_id value into FTS_DOC_ID field
@param[in,out]	tbl	table containing FULLTEXT index
@param[in]	doc_id	FTS_DOC_ID value */
static
void
innobase_fts_store_docid(
	TABLE*		tbl,
	ulonglong	doc_id)
{
	my_bitmap_map*	old_map
		= dbug_tmp_use_all_columns(tbl, tbl->write_set);

	tbl->fts_doc_id_field->store(static_cast<longlong>(doc_id), true);

	dbug_tmp_restore_column_map(tbl->write_set, old_map);
}
#endif

/*******************************************************************//**
Function for constructing an InnoDB table handler instance. */
static
handler*
innobase_create_handler(
/*====================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	TABLE_SHARE*	table,
	MEM_ROOT*	mem_root)
{
	return(new (mem_root) ha_innobase(hton, table));
}

/* General functions */

/** Check that a page_size is correct for InnoDB.
If correct, set the associated page_size_shift which is the power of 2
for this page size.
@param[in]	page_size	Page Size to evaluate
@return an associated page_size_shift if valid, 0 if invalid. */
inline
ulong
innodb_page_size_validate(
	ulong	page_size)
{
	ulong		n;

	DBUG_ENTER("innodb_page_size_validate");

	for (n = UNIV_PAGE_SIZE_SHIFT_MIN;
	     n <= UNIV_PAGE_SIZE_SHIFT_MAX;
	     n++) {
		if (page_size == static_cast<ulong>(1 << n)) {
			DBUG_RETURN(n);
		}
	}

	DBUG_RETURN(0);
}

/******************************************************************//**
Returns true if transaction should be flagged as read-only.
@return true if the thd is marked as read-only */
bool
thd_trx_is_read_only(
/*=================*/
	THD*	thd)	/*!< in: thread handle */
{
	return(thd != 0 && thd_tx_is_read_only(thd));
}

static MYSQL_THDVAR_BOOL(background_thread,
			 PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_NOSYSVAR,
			 "Internal (not user visible) flag to mark "
			 "background purge threads", NULL, NULL, 0);

/** Create a MYSQL_THD for a background thread and mark it as such.
@param name thread info for SHOW PROCESSLIST
@return new MYSQL_THD */
MYSQL_THD
innobase_create_background_thd(const char* name)
/*============================*/
{
	MYSQL_THD thd= create_background_thd();
	thd_proc_info(thd, name);
	THDVAR(thd, background_thread) = true;
	return thd;
}


/** Destroy a background purge thread THD.
@param[in]	thd	MYSQL_THD to destroy */
void
innobase_destroy_background_thd(
/*============================*/
	MYSQL_THD thd)
{
	/* need to close the connection explicitly, the server won't do it
	if innodb is in the PLUGIN_IS_DYING state */
	innobase_close_connection(innodb_hton_ptr, thd);
	thd_set_ha_data(thd, innodb_hton_ptr, NULL);
	destroy_background_thd(thd);
}

/** Close opened tables, free memory, delete items for a MYSQL_THD.
@param[in]	thd	MYSQL_THD to reset */
void
innobase_reset_background_thd(MYSQL_THD thd)
{
	if (!thd) {
		thd = current_thd;
	}

	ut_ad(thd);
	ut_ad(THDVAR(thd, background_thread));

	/* background purge thread */
	const char *proc_info= thd_proc_info(thd, "reset");
	reset_thd(thd);
	thd_proc_info(thd, proc_info);
}


/******************************************************************//**
Check if the transaction is an auto-commit transaction. TRUE also
implies that it is a SELECT (read-only) transaction.
@return true if the transaction is an auto commit read-only transaction. */
ibool
thd_trx_is_auto_commit(
/*===================*/
	THD*	thd)	/*!< in: thread handle, can be NULL */
{
	return(thd != NULL
	       && !thd_test_options(
		       thd,
		       OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)
	       && thd_is_select(thd));
}

/******************************************************************//**
Returns the NUL terminated value of glob_hostname.
@return pointer to glob_hostname. */
const char*
server_get_hostname()
/*=================*/
{
	return(glob_hostname);
}

/******************************************************************//**
Returns true if the transaction this thread is processing has edited
non-transactional tables. Used by the deadlock detector when deciding
which transaction to rollback in case of a deadlock - we try to avoid
rolling back transactions that have edited non-transactional tables.
@return true if non-transactional tables have been edited */
ibool
thd_has_edited_nontrans_tables(
/*===========================*/
	THD*	thd)	/*!< in: thread handle */
{
	return((ibool) thd_non_transactional_update(thd));
}

/* Return high resolution timestamp for the start of the current query */
UNIV_INTERN
unsigned long long
thd_query_start_micro(
	const THD*	thd)	/*!< in: thread handle */
{
	return thd_start_utime(thd);
}

/******************************************************************//**
Returns true if the thread is executing a SELECT statement.
@return true if thd is executing SELECT */
ibool
thd_is_select(
/*==========*/
	const THD*	thd)	/*!< in: thread handle */
{
	return(thd_sql_command(thd) == SQLCOM_SELECT);
}

/******************************************************************//**
Returns the lock wait timeout for the current connection.
@return the lock wait timeout, in seconds */
ulong
thd_lock_wait_timeout(
/*==================*/
	THD*	thd)	/*!< in: thread handle, or NULL to query
			the global innodb_lock_wait_timeout */
{
	/* According to <mysql/plugin.h>, passing thd == NULL
	returns the global value of the session variable. */
	return(THDVAR(thd, lock_wait_timeout));
}

/** Get the value of innodb_tmpdir.
@param[in]	thd	thread handle, or NULL to query
			the global innodb_tmpdir.
@retval NULL if innodb_tmpdir="" */
const char*
thd_innodb_tmpdir(
	THD*	thd)
{
	ut_ad(!sync_check_iterate(sync_check()));

	const char*	tmp_dir = THDVAR(thd, tmpdir);

	if (tmp_dir != NULL && *tmp_dir == '\0') {
		tmp_dir = NULL;
	}

	return(tmp_dir);
}

/** Obtain the InnoDB transaction of a MySQL thread.
@param[in,out]	thd	thread handle
@return reference to transaction pointer */
static trx_t* thd_to_trx(THD* thd)
{
	return reinterpret_cast<trx_t*>(thd_get_ha_data(thd, innodb_hton_ptr));
}

#ifdef WITH_WSREP
/********************************************************************//**
Obtain the InnoDB transaction id of a MySQL thread.
@return	transaction id */
__attribute__((warn_unused_result, nonnull))
ulonglong
thd_to_trx_id(
	THD*	thd)	/*!< in: MySQL thread */
{
	return(thd_to_trx(thd)->id);
}

static int
wsrep_abort_transaction(handlerton* hton, THD *bf_thd, THD *victim_thd,
			my_bool signal);
static int innobase_wsrep_set_checkpoint(handlerton* hton, const XID* xid);
static int innobase_wsrep_get_checkpoint(handlerton* hton, XID* xid);
#endif /* WITH_WSREP */
/********************************************************************//**
Converts an InnoDB error code to a MySQL error code and also tells to MySQL
about a possible transaction rollback inside InnoDB caused by a lock wait
timeout or a deadlock.
@return MySQL error code */
static int
convert_error_code_to_mysql(
/*========================*/
	dberr_t	error,	/*!< in: InnoDB error code */
	ulint	flags,  /*!< in: InnoDB table flags, or 0 */
	THD*	thd)	/*!< in: user thread handle or NULL */
{
	switch (error) {
	case DB_SUCCESS:
		return(0);

	case DB_INTERRUPTED:
		return(HA_ERR_ABORTED_BY_USER);

	case DB_FOREIGN_EXCEED_MAX_CASCADE:
		ut_ad(thd);
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_ROW_IS_REFERENCED,
				    "InnoDB: Cannot delete/update "
				    "rows with cascading foreign key "
				    "constraints that exceed max "
				    "depth of %d. Please "
				    "drop extra constraints and try "
				    "again", DICT_FK_MAX_RECURSIVE_LOAD);
		return(HA_ERR_FK_DEPTH_EXCEEDED);

	case DB_CANT_CREATE_GEOMETRY_OBJECT:
		my_error(ER_CANT_CREATE_GEOMETRY_OBJECT, MYF(0));
		return(HA_ERR_NULL_IN_SPATIAL);

	case DB_ERROR:
	default:
		return(HA_ERR_GENERIC); /* unspecified error */

	case DB_DUPLICATE_KEY:
		/* Be cautious with returning this error, since
		mysql could re-enter the storage layer to get
		duplicated key info, the operation requires a
		valid table handle and/or transaction information,
		which might not always be available in the error
		handling stage. */
		return(HA_ERR_FOUND_DUPP_KEY);

	case DB_READ_ONLY:
		return(HA_ERR_TABLE_READONLY);

	case DB_FOREIGN_DUPLICATE_KEY:
		return(HA_ERR_FOREIGN_DUPLICATE_KEY);

	case DB_MISSING_HISTORY:
		return(HA_ERR_TABLE_DEF_CHANGED);

	case DB_RECORD_NOT_FOUND:
		return(HA_ERR_NO_ACTIVE_RECORD);

	case DB_DEADLOCK:
		/* Since we rolled back the whole transaction, we must
		tell it also to MySQL so that MySQL knows to empty the
		cached binlog for this transaction */

		if (thd != NULL) {
			thd_mark_transaction_to_rollback(thd, 1);
		}

		return(HA_ERR_LOCK_DEADLOCK);

	case DB_LOCK_WAIT_TIMEOUT:
		/* Starting from 5.0.13, we let MySQL just roll back the
		latest SQL statement in a lock wait timeout. Previously, we
		rolled back the whole transaction. */

		if (thd) {
			thd_mark_transaction_to_rollback(
				thd, (bool) row_rollback_on_timeout);
		}

		return(HA_ERR_LOCK_WAIT_TIMEOUT);

	case DB_NO_REFERENCED_ROW:
		return(HA_ERR_NO_REFERENCED_ROW);

	case DB_ROW_IS_REFERENCED:
		return(HA_ERR_ROW_IS_REFERENCED);

	case DB_NO_FK_ON_S_BASE_COL:
	case DB_CANNOT_ADD_CONSTRAINT:
	case DB_CHILD_NO_INDEX:
	case DB_PARENT_NO_INDEX:
		return(HA_ERR_CANNOT_ADD_FOREIGN);

	case DB_CANNOT_DROP_CONSTRAINT:

		return(HA_ERR_ROW_IS_REFERENCED); /* TODO: This is a bit
						misleading, a new MySQL error
						code should be introduced */

	case DB_CORRUPTION:
		return(HA_ERR_CRASHED);

	case DB_OUT_OF_FILE_SPACE:
		return(HA_ERR_RECORD_FILE_FULL);

	case DB_TEMP_FILE_WRITE_FAIL:
		my_error(ER_GET_ERRMSG, MYF(0),
                         DB_TEMP_FILE_WRITE_FAIL,
                         ut_strerr(DB_TEMP_FILE_WRITE_FAIL),
                         "InnoDB");
		return(HA_ERR_INTERNAL_ERROR);

	case DB_TABLE_IN_FK_CHECK:
		return(HA_ERR_TABLE_IN_FK_CHECK);

	case DB_TABLE_IS_BEING_USED:
		return(HA_ERR_WRONG_COMMAND);

	case DB_TABLE_NOT_FOUND:
		return(HA_ERR_NO_SUCH_TABLE);

	case DB_DECRYPTION_FAILED:
		return(HA_ERR_DECRYPTION_FAILED);

	case DB_TABLESPACE_NOT_FOUND:
		return(HA_ERR_TABLESPACE_MISSING);

	case DB_TOO_BIG_RECORD: {
		/* If prefix is true then a 768-byte prefix is stored
		locally for BLOB fields. Refer to dict_table_get_format().
		We limit max record size to 16k for 64k page size. */
		bool prefix = !DICT_TF_HAS_ATOMIC_BLOBS(flags);
		bool comp = !!(flags & DICT_TF_COMPACT);
		ulint free_space = page_get_free_space_of_empty(comp) / 2;

		if (free_space >= ulint(comp ? COMPRESSED_REC_MAX_DATA_SIZE :
				          REDUNDANT_REC_MAX_DATA_SIZE)) {
			free_space = (comp ? COMPRESSED_REC_MAX_DATA_SIZE :
				REDUNDANT_REC_MAX_DATA_SIZE) - 1;
		}

		my_printf_error(ER_TOO_BIG_ROWSIZE,
			"Row size too large (> " ULINTPF "). Changing some columns "
			"to TEXT or BLOB %smay help. In current row "
			"format, BLOB prefix of %d bytes is stored inline.",
			MYF(0),
			free_space,
			prefix
			? "or using ROW_FORMAT=DYNAMIC or"
			  " ROW_FORMAT=COMPRESSED "
			: "",
			prefix
			? DICT_MAX_FIXED_COL_LEN
			: 0);
		return(HA_ERR_TO_BIG_ROW);
	}

	case DB_TOO_BIG_INDEX_COL:
		my_error(ER_INDEX_COLUMN_TOO_LONG, MYF(0),
			 (ulong) DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(flags));
		return(HA_ERR_INDEX_COL_TOO_LONG);

	case DB_NO_SAVEPOINT:
		return(HA_ERR_NO_SAVEPOINT);

	case DB_LOCK_TABLE_FULL:
		/* Since we rolled back the whole transaction, we must
		tell it also to MySQL so that MySQL knows to empty the
		cached binlog for this transaction */

		if (thd) {
			thd_mark_transaction_to_rollback(thd, 1);
		}

		return(HA_ERR_LOCK_TABLE_FULL);

	case DB_FTS_INVALID_DOCID:
		return(HA_FTS_INVALID_DOCID);
	case DB_FTS_EXCEED_RESULT_CACHE_LIMIT:
		return(HA_ERR_OUT_OF_MEM);
	case DB_TOO_MANY_CONCURRENT_TRXS:
		return(HA_ERR_TOO_MANY_CONCURRENT_TRXS);
	case DB_UNSUPPORTED:
		return(HA_ERR_UNSUPPORTED);
	case DB_INDEX_CORRUPT:
		return(HA_ERR_INDEX_CORRUPT);
	case DB_UNDO_RECORD_TOO_BIG:
		return(HA_ERR_UNDO_REC_TOO_BIG);
	case DB_OUT_OF_MEMORY:
		return(HA_ERR_OUT_OF_MEM);
	case DB_TABLESPACE_EXISTS:
		return(HA_ERR_TABLESPACE_EXISTS);
	case DB_TABLESPACE_DELETED:
		return(HA_ERR_TABLESPACE_MISSING);
	case DB_IDENTIFIER_TOO_LONG:
		return(HA_ERR_INTERNAL_ERROR);
	case DB_TABLE_CORRUPT:
		return(HA_ERR_TABLE_CORRUPT);
	case DB_FTS_TOO_MANY_WORDS_IN_PHRASE:
		return(HA_ERR_FTS_TOO_MANY_WORDS_IN_PHRASE);
	case DB_COMPUTE_VALUE_FAILED:
		return(HA_ERR_GENERIC); // impossible
	}
}

/*************************************************************//**
Prints info of a THD object (== user session thread) to the given file. */
void
innobase_mysql_print_thd(
/*=====================*/
	FILE*	f,		/*!< in: output stream */
	THD*	thd,		/*!< in: MySQL THD object */
	uint	max_query_len)	/*!< in: max query length to print, or 0 to
				use the default max length */
{
	char	buffer[1024];

	fputs(thd_get_error_context_description(thd, buffer, sizeof buffer,
						max_query_len), f);
	putc('\n', f);
}

/******************************************************************//**
Get the variable length bounds of the given character set. */
void
innobase_get_cset_width(
/*====================*/
	ulint	cset,		/*!< in: MySQL charset-collation code */
	unsigned*mbminlen,	/*!< out: minimum length of a char (in bytes) */
	unsigned*mbmaxlen)	/*!< out: maximum length of a char (in bytes) */
{
	CHARSET_INFO*	cs;
	ut_ad(cset <= MAX_CHAR_COLL_NUM);
	ut_ad(mbminlen);
	ut_ad(mbmaxlen);

	cs = all_charsets[cset];
	if (cs) {
		*mbminlen = cs->mbminlen;
		*mbmaxlen = cs->mbmaxlen;
		ut_ad(*mbminlen < DATA_MBMAX);
		ut_ad(*mbmaxlen < DATA_MBMAX);
	} else {
		THD*	thd = current_thd;

		if (thd && thd_sql_command(thd) == SQLCOM_DROP_TABLE) {

			/* Fix bug#46256: allow tables to be dropped if the
			collation is not found, but issue a warning. */
			if (cset != 0) {

				sql_print_warning(
					"Unknown collation #" ULINTPF ".",
					cset);
			}
		} else {

			ut_a(cset == 0);
		}

		*mbminlen = *mbmaxlen = 0;
	}
}

/******************************************************************//**
Converts an identifier to a table name. */
void
innobase_convert_from_table_id(
/*===========================*/
	CHARSET_INFO*	cs,	/*!< in: the 'from' character set */
	char*		to,	/*!< out: converted identifier */
	const char*	from,	/*!< in: identifier to convert */
	ulint		len)	/*!< in: length of 'to', in bytes */
{
	uint	errors;

	strconvert(cs, from, FN_REFLEN, &my_charset_filename, to, (uint) len, &errors);
}

/**********************************************************************
Check if the length of the identifier exceeds the maximum allowed.
return true when length of identifier is too long. */
my_bool
innobase_check_identifier_length(
/*=============================*/
	const char*	id)	/* in: FK identifier to check excluding the
				database portion. */
{
	int		well_formed_error = 0;
	CHARSET_INFO	*cs = system_charset_info;
	DBUG_ENTER("innobase_check_identifier_length");

	size_t len = my_well_formed_length(
		cs, id, id + strlen(id),
		NAME_CHAR_LEN, &well_formed_error);

	if (well_formed_error || len == NAME_CHAR_LEN) {
		my_error(ER_TOO_LONG_IDENT, MYF(0), id);
		DBUG_RETURN(true);
	}
	DBUG_RETURN(false);
}

/******************************************************************//**
Converts an identifier to UTF-8. */
void
innobase_convert_from_id(
/*=====================*/
	CHARSET_INFO*	cs,	/*!< in: the 'from' character set */
	char*		to,	/*!< out: converted identifier */
	const char*	from,	/*!< in: identifier to convert */
	ulint		len)	/*!< in: length of 'to', in bytes */
{
	uint	errors;

	strconvert(cs, from, FN_REFLEN, system_charset_info, to, (uint) len, &errors);
}

/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively.
@return 0 if a=b, <0 if a<b, >1 if a>b */
int
innobase_strcasecmp(
/*================*/
	const char*	a,	/*!< in: first string to compare */
	const char*	b)	/*!< in: second string to compare */
{
	if (!a) {
		if (!b) {
			return(0);
		} else {
			return(-1);
		}
	} else if (!b) {
		return(1);
	}

	return(my_strcasecmp(system_charset_info, a, b));
}

/******************************************************************//**
Compares NUL-terminated UTF-8 strings case insensitively. The
second string contains wildcards.
@return 0 if a match is found, 1 if not */
static
int
innobase_wildcasecmp(
/*=================*/
	const char*	a,	/*!< in: string to compare */
	const char*	b)	/*!< in: wildcard string to compare */
{
	return(wild_case_compare(system_charset_info, a, b));
}

/** Strip dir name from a full path name and return only the file name
@param[in]	path_name	full path name
@return file name or "null" if no file name */
const char*
innobase_basename(
	const char*	path_name)
{
	const char*	name = base_name(path_name);

	return((name) ? name : "null");
}

/******************************************************************//**
Makes all characters in a NUL-terminated UTF-8 string lower case. */
void
innobase_casedn_str(
/*================*/
	char*	a)	/*!< in/out: string to put in lower case */
{
	my_casedn_str(system_charset_info, a);
}

/** Determines the current SQL statement.
Thread unsafe, can only be called from the thread owning the THD.
@param[in]	thd	MySQL thread handle
@param[out]	length	Length of the SQL statement
@return			SQL statement string */
const char*
innobase_get_stmt_unsafe(
	THD*	thd,
	size_t*	length)
{
	if (const LEX_STRING *stmt = thd_query_string(thd)) {
		*length = stmt->length;
		return stmt->str;
	}

	*length = 0;
	return NULL;
}

/**********************************************************************//**
Get the current setting of the tdc_size global parameter. We do
a dirty read because for one there is no synchronization object and
secondly there is little harm in doing so even if we get a torn read.
@return	value of tdc_size */
ulint
innobase_get_table_cache_size(void)
/*===============================*/
{
	return(tdc_size);
}

/**********************************************************************//**
Get the current setting of the lower_case_table_names global parameter from
mysqld.cc. We do a dirty read because for one there is no synchronization
object and secondly there is little harm in doing so even if we get a torn
read.
@return value of lower_case_table_names */
ulint
innobase_get_lower_case_table_names(void)
/*=====================================*/
{
	return(lower_case_table_names);
}

/**
  Test a file path whether it is same as mysql data directory path.

  @param path null terminated character string

  @return
    @retval TRUE The path is different from mysql data directory.
    @retval FALSE The path is same as mysql data directory.
*/
static bool is_mysql_datadir_path(const char *path)
{
  if (path == NULL)
    return false;

  char mysql_data_dir[FN_REFLEN], path_dir[FN_REFLEN];
  convert_dirname(path_dir, path, NullS);
  convert_dirname(mysql_data_dir, mysql_unpacked_real_data_home, NullS);
  size_t mysql_data_home_len= dirname_length(mysql_data_dir);
  size_t path_len = dirname_length(path_dir);

  if (path_len < mysql_data_home_len)
    return true;

  if (!lower_case_file_system)
    return(memcmp(mysql_data_dir, path_dir, mysql_data_home_len));

  return(files_charset_info->strnncoll((uchar *) path_dir, path_len,
                                       (uchar *) mysql_data_dir,
                                       mysql_data_home_len,
                                       TRUE));
}

/*********************************************************************//**
Wrapper around MySQL's copy_and_convert function.
@return number of bytes copied to 'to' */
static
ulint
innobase_convert_string(
/*====================*/
	void*		to,		/*!< out: converted string */
	ulint		to_length,	/*!< in: number of bytes reserved
					for the converted string */
	CHARSET_INFO*	to_cs,		/*!< in: character set to convert to */
	const void*	from,		/*!< in: string to convert */
	ulint		from_length,	/*!< in: number of bytes to convert */
	CHARSET_INFO*	from_cs,	/*!< in: character set to convert
					from */
	uint*		errors)		/*!< out: number of errors encountered
					during the conversion */
{
	return(copy_and_convert(
			(char*) to, (uint32) to_length, to_cs,
			(const char*) from, (uint32) from_length, from_cs,
			errors));
}

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) that is of
type DATA_(CHAR|VARCHAR|MYSQL|VARMYSQL) using "charset_coll" and writes
the result to "buf". The result is converted to "system_charset_info".
Not more than "buf_size" bytes are written to "buf".
The result is always NUL-terminated (provided buf_size > 0) and the
number of bytes that were written to "buf" is returned (including the
terminating NUL).
@return number of bytes that were written */
ulint
innobase_raw_format(
/*================*/
	const char*	data,		/*!< in: raw data */
	ulint		data_len,	/*!< in: raw data length
					in bytes */
	ulint		charset_coll,	/*!< in: charset collation */
	char*		buf,		/*!< out: output buffer */
	ulint		buf_size)	/*!< in: output buffer size
					in bytes */
{
	/* XXX we use a hard limit instead of allocating
	but_size bytes from the heap */
	CHARSET_INFO*	data_cs;
	char		buf_tmp[8192];
	ulint		buf_tmp_used;
	uint		num_errors;

	data_cs = all_charsets[charset_coll];

	buf_tmp_used = innobase_convert_string(buf_tmp, sizeof(buf_tmp),
					       system_charset_info,
					       data, data_len, data_cs,
					       &num_errors);

	return(ut_str_sql_format(buf_tmp, buf_tmp_used, buf, buf_size));
}

/*********************************************************************//**
Compute the next autoinc value.

For MySQL replication the autoincrement values can be partitioned among
the nodes. The offset is the start or origin of the autoincrement value
for a particular node. For n nodes the increment will be n and the offset
will be in the interval [1, n]. The formula tries to allocate the next
value for a particular node.

Note: This function is also called with increment set to the number of
values we want to reserve for multi-value inserts e.g.,

	INSERT INTO T VALUES(), (), ();

innobase_next_autoinc() will be called with increment set to 3 where
autoinc_lock_mode != TRADITIONAL because we want to reserve 3 values for
the multi-value INSERT above.
@return the next value */
ulonglong
innobase_next_autoinc(
/*==================*/
	ulonglong	current,	/*!< in: Current value */
	ulonglong	need,		/*!< in: count of values needed */
	ulonglong	step,		/*!< in: AUTOINC increment step */
	ulonglong	offset,		/*!< in: AUTOINC offset */
	ulonglong	max_value)	/*!< in: max value for type */
{
	ulonglong	next_value;
	ulonglong	block = need * step;

	/* Should never be 0. */
	ut_a(need > 0);
	ut_a(block > 0);
	ut_a(max_value > 0);

        /*
          Allow auto_increment to go over max_value up to max ulonglong.
          This allows us to detect that all values are exhausted.
          If we don't do this, we will return max_value several times
          and get duplicate key errors instead of auto increment value
          out of range.
        */
        max_value= (~(ulonglong) 0);

	/* According to MySQL documentation, if the offset is greater than
	the step then the offset is ignored. */
	if (offset > block) {
		offset = 0;
	}

	/* Check for overflow. Current can be > max_value if the value is
	in reality a negative value.The visual studio compilers converts
	large double values automatically into unsigned long long datatype
	maximum value */

	if (block >= max_value
	    || offset > max_value
	    || current >= max_value
	    || max_value - offset <= offset) {

		next_value = max_value;
	} else {
		ut_a(max_value > current);

		ulonglong	free = max_value - current;

		if (free < offset || free - offset <= block) {
			next_value = max_value;
		} else {
			next_value = 0;
		}
	}

	if (next_value == 0) {
		ulonglong	next;

		if (current > offset) {
			next = (current - offset) / step;
		} else {
			next = (offset - current) / step;
		}

		ut_a(max_value > next);
		next_value = next * step;
		/* Check for multiplication overflow. */
		ut_a(next_value >= next);
		ut_a(max_value > next_value);

		/* Check for overflow */
		if (max_value - next_value >= block) {

			next_value += block;

			if (max_value - next_value >= offset) {
				next_value += offset;
			} else {
				next_value = max_value;
			}
		} else {
			next_value = max_value;
		}
	}

	ut_a(next_value != 0);
	ut_a(next_value <= max_value);

	return(next_value);
}

/********************************************************************//**
Reset the autoinc value in the table.
@return	DB_SUCCESS if all went well else error code */
UNIV_INTERN
dberr_t
ha_innobase::innobase_reset_autoinc(
/*================================*/
	ulonglong	autoinc)	/*!< in: value to store */
{
	dberr_t		error;

	error = innobase_lock_autoinc();

	if (error == DB_SUCCESS) {

		dict_table_autoinc_initialize(m_prebuilt->table, autoinc);
		m_prebuilt->table->autoinc_mutex.unlock();
	}

	return(error);
}

/*******************************************************************//**
Reset the auto-increment counter to the given value, i.e. the next row
inserted will get the given value. This is called e.g. after TRUNCATE
is emulated by doing a 'DELETE FROM t'. HA_ERR_WRONG_COMMAND is
returned by storage engines that don't support this operation.
@return	0 or error code */
UNIV_INTERN
int
ha_innobase::reset_auto_increment(
/*==============================*/
	ulonglong	value)		/*!< in: new value for table autoinc */
{
	DBUG_ENTER("ha_innobase::reset_auto_increment");

	dberr_t	error;

	update_thd(ha_thd());

	error = row_lock_table_autoinc_for_mysql(m_prebuilt);

	if (error != DB_SUCCESS) {
		DBUG_RETURN(convert_error_code_to_mysql(
				    error, m_prebuilt->table->flags, m_user_thd));
	}

	/* The next value can never be 0. */
	if (value == 0) {
		value = 1;
	}

	innobase_reset_autoinc(value);

	DBUG_RETURN(0);
}

/*********************************************************************//**
Initializes some fields in an InnoDB transaction object. */
static
void
innobase_trx_init(
/*==============*/
	THD*	thd,	/*!< in: user thread handle */
	trx_t*	trx)	/*!< in/out: InnoDB transaction handle */
{
	DBUG_ENTER("innobase_trx_init");
	DBUG_ASSERT(thd == trx->mysql_thd);

	/* Ensure that thd_lock_wait_timeout(), which may be called
	while holding lock_sys.mutex, by lock_rec_enqueue_waiting(),
	will not end up acquiring LOCK_global_system_variables in
	intern_sys_var_ptr(). */
	THDVAR(thd, lock_wait_timeout);

	trx->check_foreigns = !thd_test_options(
		thd, OPTION_NO_FOREIGN_KEY_CHECKS);

	trx->check_unique_secondary = !thd_test_options(
		thd, OPTION_RELAXED_UNIQUE_CHECKS);
#ifdef WITH_WSREP
	trx->wsrep = wsrep_on(thd);
#endif

	DBUG_VOID_RETURN;
}

/*********************************************************************//**
Allocates an InnoDB transaction for a MySQL handler object for DML.
@return InnoDB transaction handle */
trx_t*
innobase_trx_allocate(
/*==================*/
	THD*	thd)	/*!< in: user thread handle */
{
	trx_t*	trx;

	DBUG_ENTER("innobase_trx_allocate");
	DBUG_ASSERT(thd != NULL);
	DBUG_ASSERT(EQ_CURRENT_THD(thd));

	trx = trx_create();

	trx->mysql_thd = thd;

	innobase_trx_init(thd, trx);

	DBUG_RETURN(trx);
}

/*********************************************************************//**
Gets the InnoDB transaction handle for a MySQL handler object, creates
an InnoDB transaction struct if the corresponding MySQL thread struct still
lacks one.
@return InnoDB transaction handle */
static inline
trx_t*
check_trx_exists(
/*=============*/
	THD*	thd)	/*!< in: user thread handle */
{
	if (trx_t* trx = thd_to_trx(thd)) {
		ut_a(trx->magic_n == TRX_MAGIC_N);
		innobase_trx_init(thd, trx);
		return trx;
	} else {
		trx = innobase_trx_allocate(thd);
		thd_set_ha_data(thd, innodb_hton_ptr, trx);
		return trx;
	}
}

/**
  Gets current trx.

  This function may be called during InnoDB initialisation, when
  innodb_hton_ptr->slot is not yet set to meaningful value.
*/

trx_t *current_trx()
{
	THD *thd=current_thd;
	if (likely(thd != 0) && innodb_hton_ptr->slot != HA_SLOT_UNDEF) {
		return thd_to_trx(thd);
	} else {
		return(NULL);
	}
}

/*********************************************************************//**
Note that a transaction has been registered with MySQL.
@return true if transaction is registered with MySQL 2PC coordinator */
static inline
bool
trx_is_registered_for_2pc(
/*======================*/
	const trx_t*	trx)	/* in: transaction */
{
	return(trx->is_registered == 1);
}

/*********************************************************************//**
Note that a transaction has been registered with MySQL 2PC coordinator. */
static inline
void
trx_register_for_2pc(
/*==================*/
	trx_t*	trx)	/* in: transaction */
{
	trx->is_registered = 1;
	ut_ad(!trx->active_commit_ordered);
}

/*********************************************************************//**
Note that a transaction has been deregistered. */
static inline
void
trx_deregister_from_2pc(
/*====================*/
	trx_t*	trx)	/* in: transaction */
{
  trx->is_registered= false;
  trx->active_commit_ordered= false;
}

/*********************************************************************//**
Copy table flags from MySQL's HA_CREATE_INFO into an InnoDB table object.
Those flags are stored in .frm file and end up in the MySQL table object,
but are frequently used inside InnoDB so we keep their copies into the
InnoDB table object. */
static
void
innobase_copy_frm_flags_from_create_info(
/*=====================================*/
	dict_table_t*		innodb_table,	/*!< in/out: InnoDB table */
	const HA_CREATE_INFO*	create_info)	/*!< in: create info */
{
	ibool	ps_on;
	ibool	ps_off;

	if (innodb_table->is_temporary()
	    || innodb_table->no_rollback()) {
		/* Temp tables do not use persistent stats. */
		ps_on = FALSE;
		ps_off = TRUE;
	} else {
		ps_on = create_info->table_options
			& HA_OPTION_STATS_PERSISTENT;
		ps_off = create_info->table_options
			& HA_OPTION_NO_STATS_PERSISTENT;
	}

	dict_stats_set_persistent(innodb_table, ps_on, ps_off);

	dict_stats_auto_recalc_set(
		innodb_table,
		create_info->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON,
		create_info->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF);

	innodb_table->stats_sample_pages = create_info->stats_sample_pages;
}

/*********************************************************************//**
Copy table flags from MySQL's TABLE_SHARE into an InnoDB table object.
Those flags are stored in .frm file and end up in the MySQL table object,
but are frequently used inside InnoDB so we keep their copies into the
InnoDB table object. */
void
innobase_copy_frm_flags_from_table_share(
/*=====================================*/
	dict_table_t*		innodb_table,	/*!< in/out: InnoDB table */
	const TABLE_SHARE*	table_share)	/*!< in: table share */
{
	ibool	ps_on;
	ibool	ps_off;

	if (innodb_table->is_temporary()) {
		/* Temp tables do not use persistent stats */
		ps_on = FALSE;
		ps_off = TRUE;
	} else {
		ps_on = table_share->db_create_options
			& HA_OPTION_STATS_PERSISTENT;
		ps_off = table_share->db_create_options
			& HA_OPTION_NO_STATS_PERSISTENT;
	}

	dict_stats_set_persistent(innodb_table, ps_on, ps_off);

	dict_stats_auto_recalc_set(
		innodb_table,
		table_share->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON,
		table_share->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF);

	innodb_table->stats_sample_pages = table_share->stats_sample_pages;
}

/*********************************************************************//**
Construct ha_innobase handler. */

ha_innobase::ha_innobase(
/*=====================*/
	handlerton*	hton,
	TABLE_SHARE*	table_arg)
	:handler(hton, table_arg),
	m_prebuilt(),
	m_user_thd(),
	m_int_table_flags(HA_REC_NOT_IN_SEQ
			  | HA_NULL_IN_KEY
			  | HA_CAN_VIRTUAL_COLUMNS
			  | HA_CAN_INDEX_BLOBS
			  | HA_CAN_SQL_HANDLER
			  | HA_REQUIRES_KEY_COLUMNS_FOR_DELETE
			  | HA_PRIMARY_KEY_REQUIRED_FOR_POSITION
			  | HA_PRIMARY_KEY_IN_READ_INDEX
			  | HA_BINLOG_ROW_CAPABLE
			  | HA_CAN_GEOMETRY
			  | HA_PARTIAL_COLUMN_READ
			  | HA_TABLE_SCAN_ON_INDEX
			  | HA_CAN_FULLTEXT
			  | HA_CAN_FULLTEXT_EXT
		/* JAN: TODO: MySQL 5.7
			  | HA_CAN_FULLTEXT_HINTS
		*/
			  | HA_CAN_EXPORT
                          | HA_ONLINE_ANALYZE
			  | HA_CAN_RTREEKEYS
                          | HA_CAN_TABLES_WITHOUT_ROLLBACK
                          | HA_CAN_ONLINE_BACKUPS
			  | HA_CONCURRENT_OPTIMIZE
			  |  (srv_force_primary_key ? HA_REQUIRE_PRIMARY_KEY : 0)
		  ),
	m_start_of_scan(),
        m_mysql_has_locked()
{}

/*********************************************************************//**
Destruct ha_innobase handler. */

ha_innobase::~ha_innobase()
/*======================*/
{
}

/*********************************************************************//**
Updates the user_thd field in a handle and also allocates a new InnoDB
transaction handle if needed, and updates the transaction fields in the
m_prebuilt struct. */
void
ha_innobase::update_thd(
/*====================*/
	THD*	thd)	/*!< in: thd to use the handle */
{
	DBUG_ENTER("ha_innobase::update_thd");
	DBUG_PRINT("ha_innobase::update_thd", ("user_thd: %p -> %p",
		   m_user_thd, thd));

	/* The table should have been opened in ha_innobase::open(). */
	DBUG_ASSERT(m_prebuilt->table->get_ref_count() > 0);

	trx_t*	trx = check_trx_exists(thd);

	ut_ad(trx->dict_operation_lock_mode == 0);
	ut_ad(trx->dict_operation == TRX_DICT_OP_NONE);

	if (m_prebuilt->trx != trx) {

		row_update_prebuilt_trx(m_prebuilt, trx);
	}

	m_user_thd = thd;

	DBUG_ASSERT(m_prebuilt->trx->magic_n == TRX_MAGIC_N);
	DBUG_ASSERT(m_prebuilt->trx == thd_to_trx(m_user_thd));

	DBUG_VOID_RETURN;
}

/*********************************************************************//**
Updates the user_thd field in a handle and also allocates a new InnoDB
transaction handle if needed, and updates the transaction fields in the
m_prebuilt struct. */

void
ha_innobase::update_thd()
/*=====================*/
{
	THD*	thd = ha_thd();

	ut_ad(EQ_CURRENT_THD(thd));
	update_thd(thd);
}

/*********************************************************************//**
Registers an InnoDB transaction with the MySQL 2PC coordinator, so that
the MySQL XA code knows to call the InnoDB prepare and commit, or rollback
for the transaction. This MUST be called for every transaction for which
the user may call commit or rollback. Calling this several times to register
the same transaction is allowed, too. This function also registers the
current SQL statement. */
static inline
void
innobase_register_trx(
/*==================*/
	handlerton*	hton,	/* in: Innobase handlerton */
	THD*		thd,	/* in: MySQL thd (connection) object */
	trx_t*		trx)	/* in: transaction to register */
{
	const ulonglong	trx_id = static_cast<ulonglong>(
		trx_get_id_for_print(trx));

	trans_register_ha(thd, FALSE, hton, trx_id);

	if (!trx_is_registered_for_2pc(trx)
	    && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

		trans_register_ha(thd, TRUE, hton, trx_id);
	}

	trx_register_for_2pc(trx);
}

/*	BACKGROUND INFO: HOW THE MYSQL QUERY CACHE WORKS WITH INNODB
	------------------------------------------------------------

1) The use of the query cache for TBL is disabled when there is an
uncommitted change to TBL.

2) When a change to TBL commits, InnoDB stores the current value of
its global trx id counter, let us denote it by INV_TRX_ID, to the table object
in the InnoDB data dictionary, and does only allow such transactions whose
id <= INV_TRX_ID to use the query cache.

3) When InnoDB does an INSERT/DELETE/UPDATE to a table TBL, or an implicit
modification because an ON DELETE CASCADE, we invalidate the MySQL query cache
of TBL immediately.

How this is implemented inside InnoDB:

1) Since every modification always sets an IX type table lock on the InnoDB
table, it is easy to check if there can be uncommitted modifications for a
table: just check if there are locks in the lock list of the table.

2) When a transaction inside InnoDB commits, it reads the global trx id
counter and stores the value INV_TRX_ID to the tables on which it had a lock.

3) If there is an implicit table change from ON DELETE CASCADE or SET NULL,
InnoDB calls an invalidate method for the MySQL query cache for that table.

How this is implemented inside sql_cache.cc:

1) The query cache for an InnoDB table TBL is invalidated immediately at an
INSERT/UPDATE/DELETE, just like in the case of MyISAM. No need to delay
invalidation to the transaction commit.

2) To store or retrieve a value from the query cache of an InnoDB table TBL,
any query must first ask InnoDB's permission. We must pass the thd as a
parameter because InnoDB will look at the trx id, if any, associated with
that thd. Also the full_name which is used as key to search for the table
object. The full_name is a string containing the normalized path to the
table in the canonical format.

3) Use of the query cache for InnoDB tables is now allowed also when
AUTOCOMMIT==0 or we are inside BEGIN ... COMMIT. Thus transactions no longer
put restrictions on the use of the query cache.
*/

/** Check if mysql can allow the transaction to read from/store to
the query cache.
@param[in]	table	table object
@param[in]	trx	transaction object
@return whether the storing or retrieving from the query cache is permitted */
static bool innobase_query_caching_table_check_low(
	const dict_table_t*	table,
	trx_t*			trx)
{
	/* The following conditions will decide the query cache
	retrieval or storing into:

	(1) There should not be any locks on the table.
	(2) Someother trx shouldn't invalidate the cache before this
	transaction started.
	(3) Read view shouldn't exist. If exists then the view
	low_limit_id should be greater than or equal to the transaction that
	invalidates the cache for the particular table.

	For read-only transaction: should satisfy (1) and (3)
	For read-write transaction: should satisfy (1), (2), (3) */

	if (lock_table_get_n_locks(table)) {
		return false;
	}

	if (trx->id && trx->id < table->query_cache_inv_trx_id) {
		return false;
	}

	return !trx->read_view.is_open()
		|| trx->read_view.low_limit_id()
		>= table->query_cache_inv_trx_id;
}

/** Checks if MySQL at the moment is allowed for this table to retrieve a
consistent read result, or store it to the query cache.
@param[in,out]	trx		transaction
@param[in]	norm_name	concatenation of database name,
				'/' char, table name
@return whether storing or retrieving from the query cache is permitted */
static bool innobase_query_caching_table_check(
	trx_t*		trx,
	const char*	norm_name)
{
	dict_table_t*   table = dict_table_open_on_name(
		norm_name, FALSE, FALSE, DICT_ERR_IGNORE_FK_NOKEY);

	if (table == NULL) {
		return false;
	}

	/* Start the transaction if it is not started yet */
	trx_start_if_not_started(trx, false);

	bool allow = innobase_query_caching_table_check_low(table, trx);

	dict_table_close(table, FALSE, FALSE);

	if (allow) {
		/* If the isolation level is high, assign a read view for the
		transaction if it does not yet have one */

		if (trx->isolation_level >= TRX_ISO_REPEATABLE_READ
		    && !srv_read_only_mode
		    && !trx->read_view.is_open()) {

			/* Start the transaction if it is not started yet */
			trx_start_if_not_started(trx, false);

			trx->read_view.open(trx);
		}
	}

	return allow;
}

/******************************************************************//**
The MySQL query cache uses this to check from InnoDB if the query cache at
the moment is allowed to operate on an InnoDB table. The SQL query must
be a non-locking SELECT.

The query cache is allowed to operate on certain query only if this function
returns TRUE for all tables in the query.

If thd is not in the autocommit state, this function also starts a new
transaction for thd if there is no active trx yet, and assigns a consistent
read view to it if there is no read view yet.

Why a deadlock of threads is not possible: the query cache calls this function
at the start of a SELECT processing. Then the calling thread cannot be
holding any InnoDB semaphores. The calling thread is holding the
query cache mutex, and this function will reserve the InnoDB trx_sys.mutex.
Thus, the 'rank' in sync0mutex.h of the MySQL query cache mutex is above
the InnoDB trx_sys.mutex.
@return TRUE if permitted, FALSE if not; note that the value FALSE
does not mean we should invalidate the query cache: invalidation is
called explicitly */
static
my_bool
innobase_query_caching_of_table_permitted(
/*======================================*/
	THD*	thd,		/*!< in: thd of the user who is trying to
				store a result to the query cache or
				retrieve it */
	const char* full_name,	/*!< in: normalized path to the table */
	uint	full_name_len,	/*!< in: length of the normalized path
				to the table */
	ulonglong *)
{
	char	norm_name[1000];
	trx_t*	trx = check_trx_exists(thd);

	ut_a(full_name_len < 999);

	if (trx->isolation_level == TRX_ISO_SERIALIZABLE) {
		/* In the SERIALIZABLE mode we add LOCK IN SHARE MODE to every
		plain SELECT if AUTOCOMMIT is not on. */

		return(false);
	}

	if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)
	    && trx->n_mysql_tables_in_use == 0) {
		/* We are going to retrieve the query result from the query
		cache. This cannot be a store operation to the query cache
		because then MySQL would have locks on tables already.

		TODO: if the user has used LOCK TABLES to lock the table,
		then we open a transaction in the call of row_.. below.
		That trx can stay open until UNLOCK TABLES. The same problem
		exists even if we do not use the query cache. MySQL should be
		modified so that it ALWAYS calls some cleanup function when
		the processing of a query ends!

		We can imagine we instantaneously serialize this consistent
		read trx to the current trx id counter. If trx2 would have
		changed the tables of a query result stored in the cache, and
		trx2 would have already committed, making the result obsolete,
		then trx2 would have already invalidated the cache. Thus we
		can trust the result in the cache is ok for this query. */

		return(true);
	}

	/* Normalize the table name to InnoDB format */
	normalize_table_name(norm_name, full_name);

	innobase_register_trx(innodb_hton_ptr, thd, trx);

	return innobase_query_caching_table_check(trx, norm_name);
}

/*****************************************************************//**
Invalidates the MySQL query cache for the table. */
void
innobase_invalidate_query_cache(
/*============================*/
	trx_t*		trx,		/*!< in: transaction which
					modifies the table */
	const char*	full_name)	/*!< in: concatenation of
					database name, path separator,
					table name, null char NUL;
					NOTE that in Windows this is
					always in LOWER CASE! */
{
	/* Note that the sync0mutex.h rank of the query cache mutex is just
	above the InnoDB trx_sys_t->lock. The caller of this function must
	not have latches of a lower rank. */

#ifdef HAVE_QUERY_CACHE
        char    qcache_key_name[2 * (NAME_LEN + 1)];
        char db_name[NAME_CHAR_LEN * MY_CS_MBMAXLEN + 1];
        const char *key_ptr;
        size_t  tabname_len;

        // Extract the database name.
        key_ptr= strchr(full_name, '/');
        DBUG_ASSERT(key_ptr != NULL); // Database name should be present
        size_t  dbname_len= size_t(key_ptr - full_name);
        memcpy(db_name, full_name, dbname_len);
        db_name[dbname_len]= '\0';

        /* Construct the key("db-name\0table$name\0") for the query cache using
        the path name("db@002dname\0table@0024name\0") of the table in its
        canonical form. */
        dbname_len = filename_to_tablename(db_name, qcache_key_name,
                                           sizeof(qcache_key_name));
        tabname_len = filename_to_tablename(++key_ptr,
                                            (qcache_key_name + dbname_len + 1),
                                            sizeof(qcache_key_name) -
                                            dbname_len - 1);

        /* Argument TRUE below means we are using transactions */
        mysql_query_cache_invalidate4(trx->mysql_thd,
                                      qcache_key_name,
                                      uint(dbname_len + tabname_len + 2),
                                      TRUE);
#endif
}

/** Quote a standard SQL identifier like index or column name.
@param[in]	file	output stream
@param[in]	trx	InnoDB transaction, or NULL
@param[in]	id	identifier to quote */
void
innobase_quote_identifier(
	FILE*		file,
	trx_t*		trx,
	const char*	id)
{
	const int	q = trx != NULL && trx->mysql_thd != NULL
		? get_quote_char_for_identifier(trx->mysql_thd, id, strlen(id))
		: '`';

	if (q == EOF) {
		fputs(id, file);
	} else {
		putc(q, file);

		while (int c = *id++) {
			if (c == q) {
				putc(c, file);
			}
			putc(c, file);
		}

		putc(q, file);
	}
}

/** Quote a standard SQL identifier like tablespace, index or column name.
@param[in]	trx	InnoDB transaction, or NULL
@param[in]	id	identifier to quote
@return quoted identifier */
std::string
innobase_quote_identifier(
/*======================*/
	trx_t*		trx,
	const char*	id)
{
	std::string quoted_identifier;
	const int	q = trx != NULL && trx->mysql_thd != NULL
		? get_quote_char_for_identifier(trx->mysql_thd, id, strlen(id))
		: '`';

	if (q == EOF) {
		quoted_identifier.append(id);
	} else {
		quoted_identifier += char(q);
		quoted_identifier.append(id);
		quoted_identifier += char(q);
	}

	return (quoted_identifier);
}

/** Convert a table name to the MySQL system_charset_info (UTF-8)
and quote it.
@param[out]	buf	buffer for converted identifier
@param[in]	buflen	length of buf, in bytes
@param[in]	id	identifier to convert
@param[in]	idlen	length of id, in bytes
@param[in]	thd	MySQL connection thread, or NULL
@return pointer to the end of buf */
static
char*
innobase_convert_identifier(
	char*		buf,
	ulint		buflen,
	const char*	id,
	ulint		idlen,
	THD*		thd)
{
	const char*	s	= id;

	char nz[MAX_TABLE_NAME_LEN + 1];
	char nz2[MAX_TABLE_NAME_LEN + 1];

	/* Decode the table name.  The MySQL function expects
	a NUL-terminated string.  The input and output strings
	buffers must not be shared. */
	ut_a(idlen <= MAX_TABLE_NAME_LEN);
	memcpy(nz, id, idlen);
	nz[idlen] = 0;

	s = nz2;
	idlen = explain_filename(thd, nz, nz2, sizeof nz2,
				 EXPLAIN_PARTITIONS_AS_COMMENT);
	if (idlen > buflen) {
		idlen = buflen;
	}
	memcpy(buf, s, idlen);
	return(buf + idlen);
}

/*****************************************************************//**
Convert a table name to the MySQL system_charset_info (UTF-8).
@return pointer to the end of buf */
char*
innobase_convert_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	id,	/*!< in: table name to convert */
	ulint		idlen,	/*!< in: length of id, in bytes */
	THD*		thd)	/*!< in: MySQL connection thread, or NULL */
{
	char*		s	= buf;
	const char*	bufend	= buf + buflen;

	const char*	slash = (const char*) memchr(id, '/', idlen);

	if (slash == NULL) {
		return(innobase_convert_identifier(
				buf, buflen, id, idlen, thd));
	}

	/* Print the database name and table name separately. */
	s = innobase_convert_identifier(s, ulint(bufend - s),
					id, ulint(slash - id), thd);
	if (s < bufend) {
		*s++ = '.';
		s = innobase_convert_identifier(s, ulint(bufend - s),
						slash + 1, idlen
						- ulint(slash - id) - 1,
						thd);
	}

	return(s);
}

/*****************************************************************//**
A wrapper function of innobase_convert_name(), convert a table name
to the MySQL system_charset_info (UTF-8) and quote it if needed.
@return pointer to the end of buf */
void
innobase_format_name(
/*==================*/
	char*		buf,	/*!< out: buffer for converted identifier */
	ulint		buflen,	/*!< in: length of buf, in bytes */
	const char*	name)	/*!< in: table name to format */
{
	const char*     bufend;

	bufend = innobase_convert_name(buf, buflen, name, strlen(name), NULL);

	ut_ad((ulint) (bufend - buf) < buflen);

	buf[bufend - buf] = '\0';
}

/**********************************************************************//**
Determines if the currently running transaction has been interrupted.
@return true if interrupted */
bool
trx_is_interrupted(
/*===============*/
	const trx_t*	trx)	/*!< in: transaction */
{
	return(trx && trx->mysql_thd && thd_kill_level(trx->mysql_thd));
}

/**************************************************************//**
Resets some fields of a m_prebuilt struct. The template is used in fast
retrieval of just those column values MySQL needs in its processing. */
void
ha_innobase::reset_template(void)
/*=============================*/
{
	ut_ad(m_prebuilt->magic_n == ROW_PREBUILT_ALLOCATED);
	ut_ad(m_prebuilt->magic_n2 == m_prebuilt->magic_n);

	/* Force table to be freed in close_thread_table(). */
	DBUG_EXECUTE_IF("free_table_in_fts_query",
		if (m_prebuilt->in_fts_query) {
                  table->mark_table_for_reopen();
		}
	);

	m_prebuilt->keep_other_fields_on_keyread = false;
	m_prebuilt->read_just_key = 0;
	m_prebuilt->in_fts_query = 0;

	/* Reset index condition pushdown state. */
	if (m_prebuilt->idx_cond) {
		m_prebuilt->idx_cond = NULL;
		m_prebuilt->idx_cond_n_cols = 0;
		/* Invalidate m_prebuilt->mysql_template
		in ha_innobase::write_row(). */
		m_prebuilt->template_type = ROW_MYSQL_NO_TEMPLATE;
	}
	if (m_prebuilt->pk_filter) {
		m_prebuilt->pk_filter = NULL;
		m_prebuilt->template_type = ROW_MYSQL_NO_TEMPLATE;
	}
}

/*****************************************************************//**
Call this when you have opened a new table handle in HANDLER, before you
call index_read_map() etc. Actually, we can let the cursor stay open even
over a transaction commit! Then you should call this before every operation,
fetch next etc. This function inits the necessary things even after a
transaction commit. */

void
ha_innobase::init_table_handle_for_HANDLER(void)
/*============================================*/
{
	/* If current thd does not yet have a trx struct, create one.
	If the current handle does not yet have a m_prebuilt struct, create
	one. Update the trx pointers in the m_prebuilt struct. Normally
	this operation is done in external_lock. */

	update_thd(ha_thd());

	/* Initialize the m_prebuilt struct much like it would be inited in
	external_lock */

	/* If the transaction is not started yet, start it */

	trx_start_if_not_started_xa(m_prebuilt->trx, false);

	/* Assign a read view if the transaction does not have it yet */

	m_prebuilt->trx->read_view.open(m_prebuilt->trx);

	innobase_register_trx(ht, m_user_thd, m_prebuilt->trx);

	/* We did the necessary inits in this function, no need to repeat them
	in row_search_for_mysql */

	m_prebuilt->sql_stat_start = FALSE;

	/* We let HANDLER always to do the reads as consistent reads, even
	if the trx isolation level would have been specified as SERIALIZABLE */

	m_prebuilt->select_lock_type = LOCK_NONE;
	m_prebuilt->stored_select_lock_type = LOCK_NONE;

	/* Always fetch all columns in the index record */

	m_prebuilt->hint_need_to_fetch_extra_cols = ROW_RETRIEVE_ALL_COLS;

	/* We want always to fetch all columns in the whole row? Or do
	we???? */

	m_prebuilt->used_in_HANDLER = TRUE;

	reset_template();
}

/** Free tablespace resources allocated. */
void innobase_space_shutdown()
{
	DBUG_ENTER("innobase_space_shutdown");

	if (fil_system.temp_space) {
		fil_system.temp_space->close();
	}

	srv_sys_space.shutdown();
	if (srv_tmp_space.get_sanity_check_status()) {
		srv_tmp_space.delete_files();
	}
	srv_tmp_space.shutdown();

#ifdef WITH_INNODB_DISALLOW_WRITES
	os_event_destroy(srv_allow_writes_event);
#endif /* WITH_INNODB_DISALLOW_WRITES */

	DBUG_VOID_RETURN;
}

/** Free any resources that were allocated and return failure.
@return always return 1 */
static int innodb_init_abort()
{
	DBUG_ENTER("innodb_init_abort");
	innobase_space_shutdown();
	DBUG_RETURN(1);
}

/****************************************************************//**
Gives the file extension of an InnoDB single-table tablespace. */
static const char* ha_innobase_exts[] = {
	dot_ext[IBD],
	dot_ext[ISL],
	NullS
};

/** Determine if system-versioned data was modified by the transaction.
@param[in,out]	thd	current session
@param[out]	trx_id	transaction start ID
@return	transaction commit ID
@retval	0	if no system-versioned data was affected by the transaction */
static ulonglong innodb_prepare_commit_versioned(THD* thd, ulonglong *trx_id)
{
	if (const trx_t* trx = thd_to_trx(thd)) {
		*trx_id = trx->id;

		for (trx_mod_tables_t::const_iterator t
			     = trx->mod_tables.begin();
		     t != trx->mod_tables.end(); t++) {
			if (t->second.is_versioned()) {
				DBUG_ASSERT(t->first->versioned_by_id());
				DBUG_ASSERT(trx->rsegs.m_redo.rseg);

				return trx_sys.get_new_trx_id();
			}
		}

		return 0;
	}

	*trx_id = 0;
	return 0;
}

/** Initialize and normalize innodb_buffer_pool_size. */
static void innodb_buffer_pool_size_init()
{
	if (srv_buf_pool_chunk_unit > srv_buf_pool_size) {
		/* Size unit of buffer pool is larger than srv_buf_pool_size.
		adjust srv_buf_pool_chunk_unit for srv_buf_pool_size. */
		srv_buf_pool_chunk_unit = ulong(srv_buf_pool_size);
	}

	srv_buf_pool_size = buf_pool_size_align(srv_buf_pool_size);
	innobase_buffer_pool_size = srv_buf_pool_size;
}

/** Initialize, validate and normalize the InnoDB startup parameters.
@return failure code
@retval 0 on success
@retval HA_ERR_OUT_OF_MEM	when out of memory
@retval HA_ERR_INITIALIZATION	when some parameters are out of range */
static int innodb_init_params()
{
	DBUG_ENTER("innodb_init_params");

	static char	current_dir[3];
	char		*default_path;
	ulong		num_pll_degree;

	/* Check that values don't overflow on 32-bit systems. */
	if (sizeof(ulint) == 4) {
		if (innobase_buffer_pool_size > UINT_MAX32) {
			sql_print_error(
				"innodb_buffer_pool_size can't be over 4GB"
				" on 32-bit systems");
			DBUG_RETURN(HA_ERR_OUT_OF_MEM);
		}
	}

	/* The buffer pool needs to be able to accommodate enough many
	pages, even for larger pages */
	if (srv_page_size > UNIV_PAGE_SIZE_DEF
	    && innobase_buffer_pool_size < (24 * 1024 * 1024)) {
		ib::error() << "innodb_page_size="
			<< srv_page_size << " requires "
			<< "innodb_buffer_pool_size > 24M current "
			<< innobase_buffer_pool_size;
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

#ifndef HAVE_LZ4
	if (innodb_compression_algorithm == PAGE_LZ4_ALGORITHM) {
		sql_print_error("InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				"InnoDB: liblz4 is not installed. \n",
				innodb_compression_algorithm);
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}
#endif

#ifndef HAVE_LZO
	if (innodb_compression_algorithm == PAGE_LZO_ALGORITHM) {
		sql_print_error("InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				"InnoDB: liblzo is not installed. \n",
				innodb_compression_algorithm);
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}
#endif

#ifndef HAVE_LZMA
	if (innodb_compression_algorithm == PAGE_LZMA_ALGORITHM) {
		sql_print_error("InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				"InnoDB: liblzma is not installed. \n",
				innodb_compression_algorithm);
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}
#endif

#ifndef HAVE_BZIP2
	if (innodb_compression_algorithm == PAGE_BZIP2_ALGORITHM) {
		sql_print_error("InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				"InnoDB: libbz2 is not installed. \n",
				innodb_compression_algorithm);
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}
#endif

#ifndef HAVE_SNAPPY
	if (innodb_compression_algorithm == PAGE_SNAPPY_ALGORITHM) {
		sql_print_error("InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				"InnoDB: libsnappy is not installed. \n",
				innodb_compression_algorithm);
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}
#endif

	if ((srv_encrypt_tables || srv_encrypt_log
	     || innodb_encrypt_temporary_tables)
	     && !encryption_key_id_exists(FIL_DEFAULT_ENCRYPTION_KEY)) {
		sql_print_error("InnoDB: cannot enable encryption, "
				"encryption plugin is not available");
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

#ifdef _WIN32
	if (!is_filename_allowed(srv_buf_dump_filename,
				 strlen(srv_buf_dump_filename), FALSE)) {
		sql_print_error("InnoDB: innodb_buffer_pool_filename"
			" cannot have colon (:) in the file name.");
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}
#endif

	/* First calculate the default path for innodb_data_home_dir etc.,
	in case the user has not given any value.

	Note that when using the embedded server, the datadirectory is not
	necessarily the current directory of this program. */

	if (mysqld_embedded) {
		default_path = mysql_real_data_home;
	} else {
		/* It's better to use current lib, to keep paths short */
		current_dir[0] = FN_CURLIB;
		current_dir[1] = FN_LIBCHAR;
		current_dir[2] = 0;
		default_path = current_dir;
	}

	ut_a(default_path);

	fil_path_to_mysql_datadir = default_path;

	/* Set InnoDB initialization parameters according to the values
	read from MySQL .cnf file */

	/* The default dir for data files is the datadir of MySQL */

	srv_data_home = innobase_data_home_dir
		? innobase_data_home_dir : default_path;
#ifdef WITH_WSREP
	/* If we use the wsrep API, then we need to tell the server
	the path to the data files (for passing it to the SST scripts): */
	wsrep_set_data_home_dir(srv_data_home);
#endif /* WITH_WSREP */


	/*--------------- Shared tablespaces -------------------------*/

	/* Check that the value of system variable innodb_page_size was
	set correctly.  Its value was put into srv_page_size. If valid,
	return the associated srv_page_size_shift. */
	srv_page_size_shift = innodb_page_size_validate(srv_page_size);
	if (!srv_page_size_shift) {
		sql_print_error("InnoDB: Invalid page size=%lu.\n",
				srv_page_size);
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

	srv_sys_space.set_space_id(TRX_SYS_SPACE);

	switch (srv_checksum_algorithm) {
	case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
		srv_sys_space.set_flags(FSP_FLAGS_FCRC32_MASK_MARKER
					| FSP_FLAGS_FCRC32_PAGE_SSIZE());
		break;
	default:
		srv_sys_space.set_flags(FSP_FLAGS_PAGE_SSIZE());
	}

	srv_sys_space.set_name("innodb_system");
	srv_sys_space.set_path(srv_data_home);

	/* Supports raw devices */
	if (!srv_sys_space.parse_params(innobase_data_file_path, true)) {
		ib::error() << "Unable to parse innodb_data_file_path="
			    << innobase_data_file_path;
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

	srv_tmp_space.set_name("innodb_temporary");
	srv_tmp_space.set_path(srv_data_home);

	/* Temporary tablespace is in full crc32 format. */
	srv_tmp_space.set_flags(FSP_FLAGS_FCRC32_MASK_MARKER
				| FSP_FLAGS_FCRC32_PAGE_SSIZE());

	if (!srv_tmp_space.parse_params(innobase_temp_data_file_path, false)) {
		ib::error() << "Unable to parse innodb_temp_data_file_path="
			    << innobase_temp_data_file_path;
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

	/* Perform all sanity check before we take action of deleting files*/
	if (srv_sys_space.intersection(&srv_tmp_space)) {
		sql_print_error("%s and %s file names seem to be the same.",
			srv_tmp_space.name(), srv_sys_space.name());
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

	srv_sys_space.normalize_size();
	srv_tmp_space.normalize_size();

	/* ------------ UNDO tablespaces files ---------------------*/
	if (!srv_undo_dir) {
		srv_undo_dir = default_path;
	}

	os_normalize_path(srv_undo_dir);

	if (strchr(srv_undo_dir, ';')) {
		sql_print_error("syntax error in innodb_undo_directory");
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

	/* -------------- All log files ---------------------------*/

	/* The default dir for log files is the datadir of MySQL */

	if (!srv_log_group_home_dir) {
		srv_log_group_home_dir = default_path;
	}

	os_normalize_path(srv_log_group_home_dir);

	if (strchr(srv_log_group_home_dir, ';')) {
		sql_print_error("syntax error in innodb_log_group_home_dir");
		DBUG_RETURN(HA_ERR_INITIALIZATION);
	}

	DBUG_ASSERT(innodb_change_buffering <= IBUF_USE_ALL);

	/* Check that interdependent parameters have sane values. */
	if (srv_max_buf_pool_modified_pct < srv_max_dirty_pages_pct_lwm) {
		sql_print_warning("InnoDB: innodb_max_dirty_pages_pct_lwm"
				  " cannot be set higher than"
				  " innodb_max_dirty_pages_pct.\n"
				  "InnoDB: Setting"
				  " innodb_max_dirty_pages_pct_lwm to %lf\n",
				  srv_max_buf_pool_modified_pct);

		srv_max_dirty_pages_pct_lwm = srv_max_buf_pool_modified_pct;
	}

	if (srv_max_io_capacity == SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT) {

		if (srv_io_capacity >= SRV_MAX_IO_CAPACITY_LIMIT / 2) {
			/* Avoid overflow. */
			srv_max_io_capacity = SRV_MAX_IO_CAPACITY_LIMIT;
		} else {
			/* The user has not set the value. We should
			set it based on innodb_io_capacity. */
			srv_max_io_capacity =
				ut_max(2 * srv_io_capacity, 2000UL);
		}

	} else if (srv_max_io_capacity < srv_io_capacity) {
		sql_print_warning("InnoDB: innodb_io_capacity"
				  " cannot be set higher than"
				  " innodb_io_capacity_max."
				  "Setting innodb_io_capacity=%lu",
				  srv_max_io_capacity);

		srv_io_capacity = srv_max_io_capacity;
	}

	if (UNIV_PAGE_SIZE_DEF != srv_page_size) {
		ib::info() << "innodb_page_size=" << srv_page_size;

		srv_max_undo_log_size = std::max(
			srv_max_undo_log_size,
			ulonglong(SRV_UNDO_TABLESPACE_SIZE_IN_PAGES)
			<< srv_page_size_shift);
	}

	if (srv_log_write_ahead_size > srv_page_size) {
		srv_log_write_ahead_size = srv_page_size;
	} else {
		ulong	srv_log_write_ahead_size_tmp = OS_FILE_LOG_BLOCK_SIZE;

		while (srv_log_write_ahead_size_tmp
		       < srv_log_write_ahead_size) {
			srv_log_write_ahead_size_tmp
				= srv_log_write_ahead_size_tmp * 2;
		}
		if (srv_log_write_ahead_size_tmp
		    != srv_log_write_ahead_size) {
			srv_log_write_ahead_size
				= srv_log_write_ahead_size_tmp / 2;
		}
	}

	srv_buf_pool_size = ulint(innobase_buffer_pool_size);

	row_rollback_on_timeout = (ibool) innobase_rollback_on_timeout;

	if (innobase_open_files < 10) {
		innobase_open_files = 300;
		if (srv_file_per_table && tc_size > 300 && tc_size < open_files_limit) {
			innobase_open_files = tc_size;
		}
	}

	if (innobase_open_files > open_files_limit) {
		ib::warn() << "innodb_open_files " << innobase_open_files
			   << " should not be greater"
			   << " than the open_files_limit " << open_files_limit;
		if (innobase_open_files > tc_size) {
			innobase_open_files = tc_size;
		}
	}

	srv_max_n_open_files = innobase_open_files;
	srv_innodb_status = (ibool) innobase_create_status_file;

	srv_print_verbose_log = mysqld_embedded ? 0 : 1;

	/* Round up fts_sort_pll_degree to nearest power of 2 number */
	for (num_pll_degree = 1;
	     num_pll_degree < fts_sort_pll_degree;
	     num_pll_degree <<= 1) {

		/* No op */
	}

	fts_sort_pll_degree = num_pll_degree;

	/* Store the default charset-collation number of this MySQL
	installation */

	data_mysql_default_charset_coll = (ulint) default_charset_info->number;

	srv_use_atomic_writes
		= innobase_use_atomic_writes && my_may_have_atomic_write;
        if (srv_use_atomic_writes && !srv_file_per_table)
        {
          fprintf(stderr, "InnoDB: Disabling atomic_writes as file_per_table is not used.\n");
          srv_use_atomic_writes= 0;
        }

	if (srv_use_atomic_writes) {
		fprintf(stderr, "InnoDB: using atomic writes.\n");
		/*
                  Force O_DIRECT on Unixes (on Windows writes are always
                  unbuffered)
                */
#ifndef _WIN32
		switch (innodb_flush_method) {
		case SRV_O_DIRECT:
		case SRV_O_DIRECT_NO_FSYNC:
			break;
		default:
			innodb_flush_method = SRV_O_DIRECT;
			fprintf(stderr, "InnoDB: using O_DIRECT due to atomic writes.\n");
		}
#endif
	}

	if (srv_read_only_mode) {
		ib::info() << "Started in read only mode";
		srv_use_doublewrite_buf = FALSE;
	}

#ifdef LINUX_NATIVE_AIO
	if (srv_use_native_aio) {
		ib::info() << "Using Linux native AIO";
	}
#elif !defined _WIN32
	/* Currently native AIO is supported only on windows and linux
	and that also when the support is compiled in. In all other
	cases, we ignore the setting of innodb_use_native_aio. */
	srv_use_native_aio = FALSE;
#endif

#ifndef _WIN32
	ut_ad(innodb_flush_method <= SRV_O_DIRECT_NO_FSYNC);
#else
	switch (innodb_flush_method) {
	case SRV_ALL_O_DIRECT_FSYNC + 1 /* "async_unbuffered"="unbuffered" */:
		innodb_flush_method = SRV_ALL_O_DIRECT_FSYNC;
		break;
	case SRV_ALL_O_DIRECT_FSYNC + 2 /* "normal"="fsync" */:
		innodb_flush_method = SRV_FSYNC;
		break;
	default:
		ut_ad(innodb_flush_method <= SRV_ALL_O_DIRECT_FSYNC);
	}
#endif
	srv_file_flush_method = srv_flush_t(innodb_flush_method);

	innodb_buffer_pool_size_init();

	srv_lock_table_size = 5 * (srv_buf_pool_size >> srv_page_size_shift);
	DBUG_RETURN(0);
}

/** Initialize the InnoDB storage engine plugin.
@param[in,out]	p	InnoDB handlerton
@return error code
@retval 0 on success */
static int innodb_init(void* p)
{
	DBUG_ENTER("innodb_init");
	handlerton* innobase_hton= static_cast<handlerton*>(p);
	innodb_hton_ptr = innobase_hton;

	innobase_hton->db_type = DB_TYPE_INNODB;
	innobase_hton->savepoint_offset = sizeof(trx_named_savept_t);
	innobase_hton->close_connection = innobase_close_connection;
	innobase_hton->kill_query = innobase_kill_query;
	innobase_hton->savepoint_set = innobase_savepoint;
	innobase_hton->savepoint_rollback = innobase_rollback_to_savepoint;

	innobase_hton->savepoint_rollback_can_release_mdl =
				innobase_rollback_to_savepoint_can_release_mdl;

	innobase_hton->savepoint_release = innobase_release_savepoint;
	innobase_hton->prepare_ordered= NULL;
	innobase_hton->commit_ordered= innobase_commit_ordered;
	innobase_hton->commit = innobase_commit;
	innobase_hton->rollback = innobase_rollback;
	innobase_hton->prepare = innobase_xa_prepare;
	innobase_hton->recover = innobase_xa_recover;
	innobase_hton->commit_by_xid = innobase_commit_by_xid;
	innobase_hton->rollback_by_xid = innobase_rollback_by_xid;
	innobase_hton->commit_checkpoint_request=innobase_checkpoint_request;
	innobase_hton->create = innobase_create_handler;

	innobase_hton->drop_database = innobase_drop_database;
	innobase_hton->panic = innobase_end;
	innobase_hton->pre_shutdown = innodb_preshutdown;

	innobase_hton->start_consistent_snapshot =
		innobase_start_trx_and_assign_read_view;

	innobase_hton->flush_logs = innobase_flush_logs;
	innobase_hton->show_status = innobase_show_status;
	innobase_hton->flags =
		HTON_SUPPORTS_EXTENDED_KEYS | HTON_SUPPORTS_FOREIGN_KEYS
		| HTON_NATIVE_SYS_VERSIONING | HTON_WSREP_REPLICATION;

#ifdef WITH_WSREP
	innobase_hton->abort_transaction=wsrep_abort_transaction;
	innobase_hton->set_checkpoint=innobase_wsrep_set_checkpoint;
	innobase_hton->get_checkpoint=innobase_wsrep_get_checkpoint;
#endif /* WITH_WSREP */

	innobase_hton->tablefile_extensions = ha_innobase_exts;
	innobase_hton->table_options = innodb_table_option_list;

	/* System Versioning */
	innobase_hton->prepare_commit_versioned
		= innodb_prepare_commit_versioned;

	innodb_remember_check_sysvar_funcs();

	compile_time_assert(DATA_MYSQL_TRUE_VARCHAR == MYSQL_TYPE_VARCHAR);

#ifndef DBUG_OFF
	static const char	test_filename[] = "-@";
	char			test_tablename[sizeof test_filename
				+ sizeof(srv_mysql50_table_name_prefix) - 1];
	DBUG_ASSERT(sizeof test_tablename - 1
		    == filename_to_tablename(test_filename,
					     test_tablename,
					     sizeof test_tablename, true));
	DBUG_ASSERT(!strncmp(test_tablename,
			     srv_mysql50_table_name_prefix,
			     sizeof srv_mysql50_table_name_prefix - 1));
	DBUG_ASSERT(!strcmp(test_tablename
			    + sizeof srv_mysql50_table_name_prefix - 1,
			    test_filename));
#endif /* DBUG_OFF */

	os_file_set_umask(my_umask);

	/* Setup the memory alloc/free tracing mechanisms before calling
	any functions that could possibly allocate memory. */
	ut_new_boot();

	if (int error = innodb_init_params()) {
		DBUG_RETURN(error);
	}

	/* After this point, error handling has to use
	innodb_init_abort(). */

#ifdef HAVE_PSI_INTERFACE
	/* Register keys with MySQL performance schema */
	int	count;

	count = array_elements(all_pthread_mutexes);
	mysql_mutex_register("innodb", all_pthread_mutexes, count);

# ifdef UNIV_PFS_MUTEX
	count = array_elements(all_innodb_mutexes);
	mysql_mutex_register("innodb", all_innodb_mutexes, count);
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_RWLOCK
	count = array_elements(all_innodb_rwlocks);
	mysql_rwlock_register("innodb", all_innodb_rwlocks, count);
# endif /* UNIV_PFS_MUTEX */

# ifdef UNIV_PFS_THREAD
	count = array_elements(all_innodb_threads);
	mysql_thread_register("innodb", all_innodb_threads, count);
# endif /* UNIV_PFS_THREAD */

# ifdef UNIV_PFS_IO
	count = array_elements(all_innodb_files);
	mysql_file_register("innodb", all_innodb_files, count);
# endif /* UNIV_PFS_IO */

	count = array_elements(all_innodb_conds);
	mysql_cond_register("innodb", all_innodb_conds, count);
#endif /* HAVE_PSI_INTERFACE */

	bool	create_new_db = false;

	/* Check whether the data files exist. */
	dberr_t	err = srv_sys_space.check_file_spec(&create_new_db, 5U << 20);

	if (err != DB_SUCCESS) {
		DBUG_RETURN(innodb_init_abort());
	}

	err = srv_start(create_new_db);

	if (err != DB_SUCCESS) {
		innodb_shutdown();
		DBUG_RETURN(innodb_init_abort());
	}

	srv_was_started = true;
	innodb_params_adjust();

	innobase_old_blocks_pct = buf_LRU_old_ratio_update(
		innobase_old_blocks_pct, true);

	ibuf_max_size_update(srv_change_buffer_max_size);

	mysql_mutex_init(pending_checkpoint_mutex_key,
			 &pending_checkpoint_mutex,
			 MY_MUTEX_INIT_FAST);
#ifdef MYSQL_DYNAMIC_PLUGIN
	if (innobase_hton != p) {
		innobase_hton = reinterpret_cast<handlerton*>(p);
		*innobase_hton = *innodb_hton_ptr;
	}
#endif /* MYSQL_DYNAMIC_PLUGIN */

	/* Currently, monitor counter information are not persistent. */
	memset(monitor_set_tbl, 0, sizeof monitor_set_tbl);

	memset(innodb_counter_value, 0, sizeof innodb_counter_value);

	/* Do this as late as possible so server is fully starts up,
	since  we might get some initial stats if user choose to turn
	on some counters from start up */
	if (innobase_enable_monitor_counter) {
		innodb_enable_monitor_at_startup(
			innobase_enable_monitor_counter);
	}

	/* Turn on monitor counters that are default on */
	srv_mon_default_on();

	/* Unit Tests */
#ifdef UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR
	unit_test_os_file_get_parent_dir();
#endif /* UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR */

#ifdef UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
	test_make_filepath();
#endif /*UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH */

#ifdef UNIV_ENABLE_DICT_STATS_TEST
	test_dict_stats_all();
#endif /*UNIV_ENABLE_DICT_STATS_TEST */

#ifdef UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT
# ifdef HAVE_UT_CHRONO_T
	test_row_raw_format_int();
# endif /* HAVE_UT_CHRONO_T */
#endif /* UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT */

	DBUG_RETURN(0);
}

/** Shut down the InnoDB storage engine.
@return	0 */
static
int
innobase_end(handlerton*, ha_panic_function)
{
	DBUG_ENTER("innobase_end");

	if (srv_was_started) {
		THD *thd= current_thd;
		if (thd) { // may be UNINSTALL PLUGIN statement
		 	if (trx_t* trx = thd_to_trx(thd)) {
				trx->free();
		 	}
		}


		innodb_shutdown();
		innobase_space_shutdown();

		mysql_mutex_destroy(&pending_checkpoint_mutex);
	}

	DBUG_RETURN(0);
}

/*****************************************************************//**
Commits a transaction in an InnoDB database. */
void
innobase_commit_low(
/*================*/
	trx_t*	trx)	/*!< in: transaction handle */
{
#ifdef WITH_WSREP
	const char* tmp = 0;
	const bool is_wsrep = trx->is_wsrep();
	THD* thd = trx->mysql_thd;
	if (is_wsrep) {
		tmp = thd_proc_info(thd, "innobase_commit_low()");
	}
#endif /* WITH_WSREP */
	if (trx_is_started(trx)) {
		trx_commit_for_mysql(trx);
	} else {
		trx->will_lock = 0;
#ifdef WITH_WSREP
		trx->wsrep = false;
#endif /* WITH_WSREP */
	}

#ifdef WITH_WSREP
	if (is_wsrep) {
		thd_proc_info(thd, tmp);
	}
#endif /* WITH_WSREP */
}

/*****************************************************************//**
Creates an InnoDB transaction struct for the thd if it does not yet have one.
Starts a new InnoDB transaction if a transaction is not yet started. And
assigns a new snapshot for a consistent read if the transaction does not yet
have one.
@return 0 */
static
int
innobase_start_trx_and_assign_read_view(
/*====================================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	THD*		thd)	/*!< in: MySQL thread handle of the user for
				whom the transaction should be committed */
{
	DBUG_ENTER("innobase_start_trx_and_assign_read_view");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* Create a new trx struct for thd, if it does not yet have one */

	trx_t*	trx = check_trx_exists(thd);

	/* The transaction should not be active yet, start it */

	ut_ad(!trx_is_started(trx));

	trx_start_if_not_started_xa(trx, false);

	/* Assign a read view if the transaction does not have it yet.
	Do this only if transaction is using REPEATABLE READ isolation
	level. */
	trx->isolation_level = innobase_map_isolation_level(
		thd_get_trx_isolation(thd));

	if (trx->isolation_level == TRX_ISO_REPEATABLE_READ) {
		trx->read_view.open(trx);
	} else {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: WITH CONSISTENT SNAPSHOT"
				    " was ignored because this phrase"
				    " can only be used with"
				    " REPEATABLE READ isolation level.");
	}

	/* Set the MySQL flag to mark that there is an active transaction */

	innobase_register_trx(hton, current_thd, trx);

	DBUG_RETURN(0);
}

static
void
innobase_commit_ordered_2(
/*======================*/
	trx_t*	trx, 	/*!< in: Innodb transaction */
	THD*	thd)	/*!< in: MySQL thread handle */
{
	DBUG_ENTER("innobase_commit_ordered_2");

	const bool read_only = trx->read_only || trx->id == 0;

	if (!read_only) {
		/* The following call reads the binary log position of
		the transaction being committed.

		Binary logging of other engines is not relevant to
		InnoDB as all InnoDB requires is that committing
		InnoDB transactions appear in the same order in the
		MySQL binary log as they appear in InnoDB logs, which
		is guaranteed by the server.

		If the binary log is not enabled, or the transaction
		is not written to the binary log, the file name will
		be a NULL pointer. */
		thd_binlog_pos(thd, &trx->mysql_log_file_name,
			       &trx->mysql_log_offset);

		/* Don't do write + flush right now. For group commit
		to work we want to do the flush later. */
		trx->flush_log_later = true;
	}

#ifdef WITH_WSREP
	/* If the transaction is not run in 2pc, we must assign wsrep
	XID here in order to get it written in rollback segment. */
	if (trx->is_wsrep()) {
		thd_get_xid(thd, (MYSQL_XID*)trx->xid);
	}
#endif /* WITH_WSREP */

	innobase_commit_low(trx);

	if (!read_only) {
		trx->mysql_log_file_name = NULL;
		trx->flush_log_later = false;
	}

	DBUG_VOID_RETURN;
}

/*****************************************************************//**
Perform the first, fast part of InnoDB commit.

Doing it in this call ensures that we get the same commit order here
as in binlog and any other participating transactional storage engines.

Note that we want to do as little as really needed here, as we run
under a global mutex. The expensive fsync() is done later, in
innobase_commit(), without a lock so group commit can take place.

Note also that this method can be called from a different thread than
the one handling the rest of the transaction. */
static
void
innobase_commit_ordered(
/*====================*/
	handlerton *hton, /*!< in: Innodb handlerton */
	THD*	thd,	/*!< in: MySQL thread handle of the user for whom
			the transaction should be committed */
	bool	all)	/*!< in:	TRUE - commit transaction
				FALSE - the current SQL statement ended */
{
	trx_t*		trx;
	DBUG_ENTER("innobase_commit_ordered");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx = check_trx_exists(thd);

	if (!trx_is_registered_for_2pc(trx) && trx_is_started(trx)) {
		/* We cannot throw error here; instead we will catch this error
		again in innobase_commit() and report it from there. */
		DBUG_VOID_RETURN;
	}

	/* commit_ordered is only called when committing the whole transaction
	(or an SQL statement when autocommit is on). */
	DBUG_ASSERT(all ||
		(!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)));

	innobase_commit_ordered_2(trx, thd);
	trx->active_commit_ordered = true;

	DBUG_VOID_RETURN;
}

/*****************************************************************//**
Commits a transaction in an InnoDB database or marks an SQL statement
ended.
@return 0 or deadlock error if the transaction was aborted by another
	higher priority transaction. */
static
int
innobase_commit(
/*============*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd,		/*!< in: MySQL thread handle of the
					user for whom the transaction should
					be committed */
	bool		commit_trx)	/*!< in: true - commit transaction
					false - the current SQL statement
					ended */
{
	DBUG_ENTER("innobase_commit");
	DBUG_PRINT("enter", ("commit_trx: %d", commit_trx));
	DBUG_ASSERT(hton == innodb_hton_ptr);
	DBUG_PRINT("trans", ("ending transaction"));

	trx_t*	trx = check_trx_exists(thd);

	ut_ad(trx->dict_operation_lock_mode == 0);
	ut_ad(trx->dict_operation == TRX_DICT_OP_NONE);

	/* Transaction is deregistered only in a commit or a rollback. If
	it is deregistered we know there cannot be resources to be freed
	and we could return immediately.  For the time being, we play safe
	and do the cleanup though there should be nothing to clean up. */

	if (!trx_is_registered_for_2pc(trx) && trx_is_started(trx)) {

		sql_print_error("Transaction not registered for MariaDB 2PC,"
				" but transaction is active");
	}

	bool	read_only = trx->read_only || trx->id == 0;
	DBUG_PRINT("info", ("readonly: %d", read_only));

	if (commit_trx
	    || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {

		DBUG_EXECUTE_IF("crash_innodb_before_commit",
				DBUG_SUICIDE(););

		/* Run the fast part of commit if we did not already. */
		if (!trx->active_commit_ordered) {
			innobase_commit_ordered_2(trx, thd);

		}

		/* We were instructed to commit the whole transaction, or
		this is an SQL statement end and autocommit is on */

		/* At this point commit order is fixed and transaction is
		visible to others. So we can wakeup other commits waiting for
		this one, to allow then to group commit with us. */
		thd_wakeup_subsequent_commits(thd, 0);

		/* Now do a write + flush of logs. */
		trx_commit_complete_for_mysql(trx);

		trx_deregister_from_2pc(trx);
	} else {
		/* We just mark the SQL statement ended and do not do a
		transaction commit */

		/* If we had reserved the auto-inc lock for some
		table in this SQL statement we release it now */

		if (!read_only) {
			lock_unlock_table_autoinc(trx);
		}

		/* Store the current undo_no of the transaction so that we
		know where to roll back if we have to roll back the next
		SQL statement */

		trx_mark_sql_stat_end(trx);
	}

	/* Reset the number AUTO-INC rows required */
	trx->n_autoinc_rows = 0;

	/* This is a statement level variable. */
	trx->fts_next_doc_id = 0;

	DBUG_RETURN(0);
}

/*****************************************************************//**
Rolls back a transaction or the latest SQL statement.
@return 0 or error number */
static
int
innobase_rollback(
/*==============*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back */
	bool		rollback_trx)	/*!< in: TRUE - rollback entire
					transaction FALSE - rollback the current
					statement only */
{
	DBUG_ENTER("innobase_rollback");
	DBUG_ASSERT(hton == innodb_hton_ptr);
	DBUG_PRINT("trans", ("aborting transaction"));

	trx_t*	trx = check_trx_exists(thd);

	ut_ad(trx->dict_operation_lock_mode == 0);
	ut_ad(trx->dict_operation == TRX_DICT_OP_NONE);

	/* Reset the number AUTO-INC rows required */

	trx->n_autoinc_rows = 0;

	/* If we had reserved the auto-inc lock for some table (if
	we come here to roll back the latest SQL statement) we
	release it now before a possibly lengthy rollback */
	lock_unlock_table_autoinc(trx);

	/* This is a statement level variable. */

	trx->fts_next_doc_id = 0;

	dberr_t		error;

#ifdef WITH_WSREP
	/* If trx was assigned wsrep XID in prepare phase and the
	trx is being rolled back due to BF abort, clear XID in order
	to avoid writing it to rollback segment out of order. The XID
	will be reassigned when the transaction is replayed. */
	if (trx->state != TRX_STATE_NOT_STARTED && wsrep_is_wsrep_xid(trx->xid)) {
		trx->xid->null();
	}
#endif /* WITH_WSREP */
	if (rollback_trx
	    || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

		error = trx_rollback_for_mysql(trx);

		trx_deregister_from_2pc(trx);
	} else {

		error = trx_rollback_last_sql_stat_for_mysql(trx);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, trx->mysql_thd));
}

/*****************************************************************//**
Rolls back a transaction
@return 0 or error number */
static
int
innobase_rollback_trx(
/*==================*/
	trx_t*	trx)	/*!< in: transaction */
{
	DBUG_ENTER("innobase_rollback_trx");
	DBUG_PRINT("trans", ("aborting transaction"));

	/* If we had reserved the auto-inc lock for some table (if
	we come here to roll back the latest SQL statement) we
	release it now before a possibly lengthy rollback */
	lock_unlock_table_autoinc(trx);
	trx_deregister_from_2pc(trx);

	DBUG_RETURN(convert_error_code_to_mysql(trx_rollback_for_mysql(trx),
						0, trx->mysql_thd));
}


struct pending_checkpoint {
	struct pending_checkpoint *next;
	handlerton *hton;
	void *cookie;
	ib_uint64_t lsn;
};
static struct pending_checkpoint *pending_checkpoint_list;
static struct pending_checkpoint *pending_checkpoint_list_end;

/*****************************************************************//**
Handle a commit checkpoint request from server layer.
We put the request in a queue, so that we can notify upper layer about
checkpoint complete when we have flushed the redo log.
If we have already flushed all relevant redo log, we notify immediately.*/
static
void
innobase_checkpoint_request(
	handlerton *hton,
	void *cookie)
{
	ib_uint64_t			lsn;
	ib_uint64_t			flush_lsn;
	struct pending_checkpoint *	entry;

	/* Do the allocation outside of lock to reduce contention. The normal
	case is that not everything is flushed, so we will need to enqueue. */
	entry = static_cast<struct pending_checkpoint *>
		(my_malloc(PSI_INSTRUMENT_ME, sizeof(*entry), MYF(MY_WME)));
	if (!entry) {
		sql_print_error("Failed to allocate %u bytes."
				" Commit checkpoint will be skipped.",
				static_cast<unsigned>(sizeof(*entry)));
		return;
	}

	entry->next = NULL;
	entry->hton = hton;
	entry->cookie = cookie;

	mysql_mutex_lock(&pending_checkpoint_mutex);
	lsn = log_get_lsn();
	flush_lsn = log_get_flush_lsn();
	if (lsn > flush_lsn) {
		/* Put the request in queue.
		When the log gets flushed past the lsn, we will remove the
		entry from the queue and notify the upper layer. */
		entry->lsn = lsn;
		if (pending_checkpoint_list_end) {
			pending_checkpoint_list_end->next = entry;
			/* There is no need to order the entries in the list
			by lsn. The upper layer can accept notifications in
			any order, and short delays in notifications do not
			significantly impact performance. */
		} else {
			pending_checkpoint_list = entry;
		}
		pending_checkpoint_list_end = entry;
		entry = NULL;
	}
	mysql_mutex_unlock(&pending_checkpoint_mutex);

	if (entry) {
		/* We are already flushed. Notify the checkpoint immediately. */
		commit_checkpoint_notify_ha(entry->hton, entry->cookie);
		my_free(entry);
	}
}

/*****************************************************************//**
Log code calls this whenever log has been written and/or flushed up
to a new position. We use this to notify upper layer of a new commit
checkpoint when necessary.*/
UNIV_INTERN
void
innobase_mysql_log_notify(
/*======================*/
	ib_uint64_t	flush_lsn)	/*!< in: LSN flushed to disk */
{
	struct pending_checkpoint *	pending;
	struct pending_checkpoint *	entry;
	struct pending_checkpoint *	last_ready;

	/* It is safe to do a quick check for NULL first without lock.
	Even if we should race, we will at most skip one checkpoint and
	take the next one, which is harmless. */
	if (!pending_checkpoint_list)
		return;

	mysql_mutex_lock(&pending_checkpoint_mutex);
	pending = pending_checkpoint_list;
	if (!pending)
	{
		mysql_mutex_unlock(&pending_checkpoint_mutex);
		return;
	}

	last_ready = NULL;
	for (entry = pending; entry != NULL; entry = entry -> next)
	{
		/* Notify checkpoints up until the first entry that has not
		been fully flushed to the redo log. Since we do not maintain
		the list ordered, in principle there could be more entries
		later than were also flushed. But there is no harm in
		delaying notifications for those a bit. And in practise, the
		list is unlikely to have more than one element anyway, as we
		flush the redo log at least once every second. */
		if (entry->lsn > flush_lsn)
			break;
		last_ready = entry;
	}

	if (last_ready)
	{
		/* We found some pending checkpoints that are now flushed to
		disk. So remove them from the list. */
		pending_checkpoint_list = entry;
		if (!entry)
			pending_checkpoint_list_end = NULL;
	}

	mysql_mutex_unlock(&pending_checkpoint_mutex);

	if (!last_ready)
		return;

	/* Now that we have released the lock, notify upper layer about all
	commit checkpoints that have now completed. */
	for (;;) {
		entry = pending;
		pending = pending->next;

		commit_checkpoint_notify_ha(entry->hton, entry->cookie);

		my_free(entry);
		if (entry == last_ready)
			break;
	}
}

/*****************************************************************//**
Rolls back a transaction to a savepoint.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_rollback_to_savepoint(
/*===========================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back to savepoint */
	void*		savepoint)	/*!< in: savepoint data */
{

	DBUG_ENTER("innobase_rollback_to_savepoint");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx_t*	trx = check_trx_exists(thd);

	/* TODO: use provided savepoint data area to store savepoint data */

	char	name[64];

	longlong2str(longlong(savepoint), name, 36);

	int64_t	mysql_binlog_cache_pos;

	dberr_t	error = trx_rollback_to_savepoint_for_mysql(
		trx, name, &mysql_binlog_cache_pos);

	if (error == DB_SUCCESS && trx->fts_trx != NULL) {
		fts_savepoint_rollback(trx, name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*****************************************************************//**
Check whether innodb state allows to safely release MDL locks after
rollback to savepoint.
When binlog is on, MDL locks acquired after savepoint unit are not
released if there are any locks held in InnoDB.
@return true if it is safe, false if its not safe. */
static
bool
innobase_rollback_to_savepoint_can_release_mdl(
/*===========================================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd)		/*!< in: handle to the MySQL thread
					of the user whose transaction should
					be rolled back to savepoint */
{
	DBUG_ENTER("innobase_rollback_to_savepoint_can_release_mdl");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx_t*	trx = check_trx_exists(thd);

	/* If transaction has not acquired any locks then it is safe
	to release MDL after rollback to savepoint */
	if (UT_LIST_GET_LEN(trx->lock.trx_locks) == 0) {

		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}

/*****************************************************************//**
Release transaction savepoint name.
@return 0 if success, HA_ERR_NO_SAVEPOINT if no savepoint with the
given name */
static
int
innobase_release_savepoint(
/*=======================*/
	handlerton*	hton,		/*!< in: handlerton for InnoDB */
	THD*		thd,		/*!< in: handle to the MySQL thread
					of the user whose transaction's
					savepoint should be released */
	void*		savepoint)	/*!< in: savepoint data */
{
	dberr_t		error;
	trx_t*		trx;
	char		name[64];

	DBUG_ENTER("innobase_release_savepoint");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	trx = check_trx_exists(thd);

	/* TODO: use provided savepoint data area to store savepoint data */

	longlong2str(longlong(savepoint), name, 36);

	error = trx_release_savepoint_for_mysql(trx, name);

	if (error == DB_SUCCESS && trx->fts_trx != NULL) {
		fts_savepoint_release(trx, name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*****************************************************************//**
Sets a transaction savepoint.
@return always 0, that is, always succeeds */
static
int
innobase_savepoint(
/*===============*/
	handlerton*	hton,	/*!< in: handle to the InnoDB handlerton */
	THD*		thd,	/*!< in: handle to the MySQL thread */
	void*		savepoint)/*!< in: savepoint data */
{
	DBUG_ENTER("innobase_savepoint");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* In the autocommit mode there is no sense to set a savepoint
	(unless we are in sub-statement), so SQL layer ensures that
	this method is never called in such situation.  */

	trx_t*	trx = check_trx_exists(thd);

	/* Cannot happen outside of transaction */
	DBUG_ASSERT(trx_is_registered_for_2pc(trx));

	/* TODO: use provided savepoint data area to store savepoint data */
	char	name[64];

	longlong2str(longlong(savepoint), name, 36);

	dberr_t	error = trx_savepoint_for_mysql(trx, name, 0);

	if (error == DB_SUCCESS && trx->fts_trx != NULL) {
		fts_savepoint_take(trx->fts_trx, name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}


/**
  Frees a possible InnoDB trx object associated with the current THD.

  @param hton  innobase handlerton
  @param thd   server thread descriptor, which resources should be free'd

  @return 0 always
*/
static int innobase_close_connection(handlerton *hton, THD *thd)
{
  DBUG_ASSERT(hton == innodb_hton_ptr);
  if (auto trx= thd_to_trx(thd))
  {
    if (trx->state == TRX_STATE_PREPARED && trx->has_logged_persistent())
    {
      trx_disconnect_prepared(trx);
      return 0;
    }
    innobase_rollback_trx(trx);
    trx->free();
  }
  return 0;
}

UNIV_INTERN void lock_cancel_waiting_and_release(lock_t* lock);

/** Cancel any pending lock request associated with the current THD.
@sa THD::awake() @sa ha_kill_query() */
static void innobase_kill_query(handlerton*, THD *thd, enum thd_kill_levels)
{
  DBUG_ENTER("innobase_kill_query");

  if (trx_t* trx= thd_to_trx(thd))
  {
#ifdef WITH_WSREP
    if (trx->is_wsrep() && wsrep_thd_is_aborting(thd))
      /* if victim has been signaled by BF thread and/or aborting is already
      progressing, following query aborting is not necessary any more.
      Also, BF thread should own trx mutex for the victim. */
      DBUG_VOID_RETURN;
#endif /* WITH_WSREP */
    lock_mutex_enter();
    trx_sys.trx_list.freeze();
    trx_mutex_enter(trx);
    /* It is possible that innobase_close_connection() is concurrently
    being executed on our victim. Even if the trx object is later
    reused for another client connection or a background transaction,
    its trx->mysql_thd will differ from our thd.

    trx_sys.trx_list is thread-safe. It's freezed to 'protect'
    trx_t. However, trx_t::commit_in_memory() changes a trx_t::state
    of autocommit non-locking transactions without any protection.

    At this point, trx may have been reallocated for another client
    connection, or for a background operation. In that case, either
    trx_t::state or trx_t::mysql_thd should not match our expectations. */
    bool cancel= trx->mysql_thd == thd && trx->state == TRX_STATE_ACTIVE &&
      !trx->lock.was_chosen_as_deadlock_victim;
    trx_sys.trx_list.unfreeze();
    if (!cancel);
    else if (lock_t *lock= trx->lock.wait_lock)
      lock_cancel_waiting_and_release(lock);
    lock_mutex_exit();
    trx_mutex_exit(trx);
  }

  DBUG_VOID_RETURN;
}


/*************************************************************************//**
** InnoDB database tables
*****************************************************************************/

/** Get the record format from the data dictionary.
@return one of ROW_TYPE_REDUNDANT, ROW_TYPE_COMPACT,
ROW_TYPE_COMPRESSED, ROW_TYPE_DYNAMIC */

enum row_type
ha_innobase::get_row_type() const
{
	if (m_prebuilt && m_prebuilt->table) {
		const ulint	flags = m_prebuilt->table->flags;

		switch (dict_tf_get_rec_format(flags)) {
		case REC_FORMAT_REDUNDANT:
			return(ROW_TYPE_REDUNDANT);
		case REC_FORMAT_COMPACT:
			return(ROW_TYPE_COMPACT);
		case REC_FORMAT_COMPRESSED:
			return(ROW_TYPE_COMPRESSED);
		case REC_FORMAT_DYNAMIC:
			return(ROW_TYPE_DYNAMIC);
		}
	}
	ut_ad(0);
	return(ROW_TYPE_NOT_USED);
}

/****************************************************************//**
Get the table flags to use for the statement.
@return table flags */

handler::Table_flags
ha_innobase::table_flags() const
/*============================*/
{
	THD*			thd = ha_thd();
	handler::Table_flags	flags = m_int_table_flags;

	/* Need to use tx_isolation here since table flags is (also)
	called before prebuilt is inited. */

	if (thd_tx_isolation(thd) <= ISO_READ_COMMITTED) {
		return(flags);
	}

	return(flags | HA_BINLOG_STMT_CAPABLE);
}

/****************************************************************//**
Returns the table type (storage engine name).
@return table type */

const char*
ha_innobase::table_type() const
/*===========================*/
{
	return(innobase_hton_name);
}

/****************************************************************//**
Returns the index type.
@return index type */

const char*
ha_innobase::index_type(
/*====================*/
	uint	keynr)		/*!< : index number */
{
	dict_index_t*	index = innobase_get_index(keynr);

	if (index && index->type & DICT_FTS) {
		return("FULLTEXT");
	} else if (dict_index_is_spatial(index)) {
		return("SPATIAL");
	} else {
		return("BTREE");
	}
}

/****************************************************************//**
Returns the operations supported for indexes.
@return flags of supported operations */

ulong
ha_innobase::index_flags(
/*=====================*/
	uint	key,
	uint,
	bool) const
{
	if (table_share->key_info[key].algorithm == HA_KEY_ALG_FULLTEXT) {
		return(0);
	}

	/* For spatial index, we don't support descending scan
	and ICP so far. */
	if (table_share->key_info[key].flags & HA_SPATIAL) {
		return HA_READ_NEXT | HA_READ_ORDER| HA_READ_RANGE
			| HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR;
	}

	ulong flags= key == table_share->primary_key
		? HA_CLUSTERED_INDEX : 0;

	flags |= HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER
		| HA_READ_RANGE | HA_KEYREAD_ONLY
		| HA_DO_INDEX_COND_PUSHDOWN
		| HA_DO_RANGE_FILTER_PUSHDOWN;

	return(flags);
}

/****************************************************************//**
Returns the maximum number of keys.
@return MAX_KEY */

uint
ha_innobase::max_supported_keys() const
/*===================================*/
{
	return(MAX_KEY);
}

/****************************************************************//**
Returns the maximum key length.
@return maximum supported key length, in bytes */

uint
ha_innobase::max_supported_key_length() const
/*=========================================*/
{
	/* An InnoDB page must store >= 2 keys; a secondary key record
	must also contain the primary key value.  Therefore, if both
	the primary key and the secondary key are at this maximum length,
	it must be less than 1/4th of the free space on a page including
	record overhead.

	MySQL imposes its own limit to this number; MAX_KEY_LENGTH = 3072.

	For page sizes = 16k, InnoDB historically reported 3500 bytes here,
	But the MySQL limit of 3072 was always used through the handler
	interface.

	Note: Handle 16k and 32k pages the same here since the limits
	are higher than imposed by MySQL. */

	switch (srv_page_size) {
	case 4096:
		/* Hack: allow mysql.innodb_index_stats to be created. */
		/* FIXME: rewrite this API, and in sql_table.cc consider
		that in index-organized tables (such as InnoDB), secondary
		index records will be padded with the PRIMARY KEY, instead
		of some short ROWID or record heap address. */
		return(1173);
	case 8192:
		return(1536);
	default:
		return(3500);
	}
}

/****************************************************************//**
Returns the key map of keys that are usable for scanning.
@return key_map_full */

const key_map*
ha_innobase::keys_to_use_for_scanning()
/*===================================*/
{
	return(&key_map_full);
}

/****************************************************************//**
Ensures that if there's a concurrent inplace ADD INDEX, being-indexed virtual
columns are computed. They are not marked as indexed in the old table, so the
server won't add them to the read_set automatically */
void
ha_innobase::column_bitmaps_signal()
/*================================*/
{
	if (!table->vfield || table->current_lock != F_WRLCK) {
		return;
	}

	dict_index_t*	clust_index = dict_table_get_first_index(m_prebuilt->table);
	uint	num_v = 0;
	for (uint j = 0; j < table->s->virtual_fields; j++) {
		if (table->vfield[j]->stored_in_db()) {
			continue;
		}

		dict_col_t*	col = &m_prebuilt->table->v_cols[num_v].m_col;
		if (col->ord_part ||
		    (dict_index_is_online_ddl(clust_index) &&
		     row_log_col_is_indexed(clust_index, num_v))) {
			table->mark_virtual_column_with_deps(table->vfield[j]);
		}
		num_v++;
	}
}


/****************************************************************//**
Determines if table caching is supported.
@return HA_CACHE_TBL_ASKTRANSACT */

uint8
ha_innobase::table_cache_type()
/*===========================*/
{
	return(HA_CACHE_TBL_ASKTRANSACT);
}

/****************************************************************//**
Determines if the primary key is clustered index.
@return true */

/** Normalizes a table name string.
A normalized name consists of the database name catenated to '/'
and table name. For example: test/mytable.
On Windows, normalization puts both the database name and the
table name always to lower case if "set_lower_case" is set to TRUE.
@param[out]	norm_name	Normalized name, null-terminated.
@param[in]	name		Name to normalize.
@param[in]	set_lower_case	True if we also should fold to lower case. */
void
normalize_table_name_c_low(
/*=======================*/
	char*           norm_name,      /* out: normalized name as a
					null-terminated string */
	const char*     name,           /* in: table name string */
	ibool           set_lower_case) /* in: TRUE if we want to set
					 name to lower case */
{
	char*	name_ptr;
	ulint	name_len;
	char*	db_ptr;
	ulint	db_len;
	char*	ptr;
	ulint	norm_len;

	/* Scan name from the end */

	ptr = strend(name) - 1;

	/* seek to the last path separator */
	while (ptr >= name && *ptr != '\\' && *ptr != '/') {
		ptr--;
	}

	name_ptr = ptr + 1;
	name_len = strlen(name_ptr);

	/* skip any number of path separators */
	while (ptr >= name && (*ptr == '\\' || *ptr == '/')) {
		ptr--;
	}

	DBUG_ASSERT(ptr >= name);

	/* seek to the last but one path separator or one char before
	the beginning of name */
	db_len = 0;
	while (ptr >= name && *ptr != '\\' && *ptr != '/') {
		ptr--;
		db_len++;
	}

	db_ptr = ptr + 1;

	norm_len = db_len + name_len + sizeof "/";
	ut_a(norm_len < FN_REFLEN - 1);

	memcpy(norm_name, db_ptr, db_len);

	norm_name[db_len] = '/';

	/* Copy the name and null-byte. */
	memcpy(norm_name + db_len + 1, name_ptr, name_len + 1);

	if (set_lower_case) {
		innobase_casedn_str(norm_name);
	}
}

create_table_info_t::create_table_info_t(
	THD*		thd,
	const TABLE*	form,
	HA_CREATE_INFO*	create_info,
	char*		table_name,
	char*		remote_path,
	bool		file_per_table,
	trx_t*		trx)
	: m_thd(thd),
	  m_trx(trx),
	  m_form(form),
	  m_default_row_format(innodb_default_row_format),
	  m_create_info(create_info),
	  m_table_name(table_name), m_table(NULL),
	  m_drop_before_rollback(false),
	  m_remote_path(remote_path),
	  m_innodb_file_per_table(file_per_table)
{
}

/** Normalizes a table name string.
A normalized name consists of the database name catenated to '/'
and table name. For example: test/mytable.
On Windows, normalization puts both the database name and the
table name always to lower case if "set_lower_case" is set to TRUE.
@param[out]	norm_name	Normalized name, null-terminated.
@param[in]	name		Name to normalize.
@param[in]	set_lower_case	True if we also should fold to lower case. */
void
create_table_info_t::normalize_table_name_low(
	char*		norm_name,
	const char*	name,
	ibool		set_lower_case)
{
	normalize_table_name_c_low(norm_name, name, set_lower_case);
}

#if !defined(DBUG_OFF)
/*********************************************************************
Test normalize_table_name_low(). */
static
void
test_normalize_table_name_low()
/*===========================*/
{
	char		norm_name[FN_REFLEN];
	const char*	test_data[][2] = {
		/* input, expected result */
		{"./mysqltest/t1", "mysqltest/t1"},
		{"./test/#sql-842b_2", "test/#sql-842b_2"},
		{"./test/#sql-85a3_10", "test/#sql-85a3_10"},
		{"./test/#sql2-842b-2", "test/#sql2-842b-2"},
		{"./test/bug29807", "test/bug29807"},
		{"./test/foo", "test/foo"},
		{"./test/innodb_bug52663", "test/innodb_bug52663"},
		{"./test/t", "test/t"},
		{"./test/t1", "test/t1"},
		{"./test/t10", "test/t10"},
		{"/a/b/db/table", "db/table"},
		{"/a/b/db///////table", "db/table"},
		{"/a/b////db///////table", "db/table"},
		{"/var/tmp/mysqld.1/#sql842b_2_10", "mysqld.1/#sql842b_2_10"},
		{"db/table", "db/table"},
		{"ddd/t", "ddd/t"},
		{"d/ttt", "d/ttt"},
		{"d/t", "d/t"},
		{".\\mysqltest\\t1", "mysqltest/t1"},
		{".\\test\\#sql-842b_2", "test/#sql-842b_2"},
		{".\\test\\#sql-85a3_10", "test/#sql-85a3_10"},
		{".\\test\\#sql2-842b-2", "test/#sql2-842b-2"},
		{".\\test\\bug29807", "test/bug29807"},
		{".\\test\\foo", "test/foo"},
		{".\\test\\innodb_bug52663", "test/innodb_bug52663"},
		{".\\test\\t", "test/t"},
		{".\\test\\t1", "test/t1"},
		{".\\test\\t10", "test/t10"},
		{"C:\\a\\b\\db\\table", "db/table"},
		{"C:\\a\\b\\db\\\\\\\\\\\\\\table", "db/table"},
		{"C:\\a\\b\\\\\\\\db\\\\\\\\\\\\\\table", "db/table"},
		{"C:\\var\\tmp\\mysqld.1\\#sql842b_2_10", "mysqld.1/#sql842b_2_10"},
		{"db\\table", "db/table"},
		{"ddd\\t", "ddd/t"},
		{"d\\ttt", "d/ttt"},
		{"d\\t", "d/t"},
	};

	for (size_t i = 0; i < UT_ARR_SIZE(test_data); i++) {
		printf("test_normalize_table_name_low():"
		       " testing \"%s\", expected \"%s\"... ",
		       test_data[i][0], test_data[i][1]);

		create_table_info_t::normalize_table_name_low(
			norm_name, test_data[i][0], FALSE);

		if (strcmp(norm_name, test_data[i][1]) == 0) {
			printf("ok\n");
		} else {
			printf("got \"%s\"\n", norm_name);
			ut_error;
		}
	}
}

/*********************************************************************
Test ut_format_name(). */
static
void
test_ut_format_name()
/*=================*/
{
	char		buf[NAME_LEN * 3];

	struct {
		const char*	name;
		ulint		buf_size;
		const char*	expected;
	} test_data[] = {
		{"test/t1",	sizeof(buf),	"`test`.`t1`"},
		{"test/t1",	12,		"`test`.`t1`"},
		{"test/t1",	11,		"`test`.`t1"},
		{"test/t1",	10,		"`test`.`t"},
		{"test/t1",	9,		"`test`.`"},
		{"test/t1",	8,		"`test`."},
		{"test/t1",	7,		"`test`"},
		{"test/t1",	6,		"`test"},
		{"test/t1",	5,		"`tes"},
		{"test/t1",	4,		"`te"},
		{"test/t1",	3,		"`t"},
		{"test/t1",	2,		"`"},
		{"test/t1",	1,		""},
		{"test/t1",	0,		"BUF_NOT_CHANGED"},
		{"table",	sizeof(buf),	"`table`"},
		{"ta'le",	sizeof(buf),	"`ta'le`"},
		{"ta\"le",	sizeof(buf),	"`ta\"le`"},
		{"ta`le",	sizeof(buf),	"`ta``le`"},
	};

	for (size_t i = 0; i < UT_ARR_SIZE(test_data); i++) {

		memcpy(buf, "BUF_NOT_CHANGED", strlen("BUF_NOT_CHANGED") + 1);

		char*	ret;

		ret = ut_format_name(test_data[i].name,
				     buf,
				     test_data[i].buf_size);

		ut_a(ret == buf);

		if (strcmp(buf, test_data[i].expected) == 0) {
			ib::info() << "ut_format_name(" << test_data[i].name
				<< ", buf, " << test_data[i].buf_size << "),"
				" expected " << test_data[i].expected
				<< ", OK";
		} else {
			ib::error() << "ut_format_name(" << test_data[i].name
				<< ", buf, " << test_data[i].buf_size << "),"
				" expected " << test_data[i].expected
				<< ", ERROR: got " << buf;
			ut_error;
		}
	}
}
#endif /* !DBUG_OFF */

/** Match index columns between MySQL and InnoDB.
This function checks whether the index column information
is consistent between KEY info from mysql and that from innodb index.
@param[in]	key_info	Index info from mysql
@param[in]	index_info	Index info from InnoDB
@return true if all column types match. */
static
bool
innobase_match_index_columns(
	const KEY*		key_info,
	const dict_index_t*	index_info)
{
	const KEY_PART_INFO*	key_part;
	const KEY_PART_INFO*	key_end;
	const dict_field_t*	innodb_idx_fld;
	const dict_field_t*	innodb_idx_fld_end;

	DBUG_ENTER("innobase_match_index_columns");

	/* Check whether user defined index column count matches */
	if (key_info->user_defined_key_parts !=
		index_info->n_user_defined_cols) {
		DBUG_RETURN(FALSE);
	}

	key_part = key_info->key_part;
	key_end = key_part + key_info->user_defined_key_parts;
	innodb_idx_fld = index_info->fields;
	innodb_idx_fld_end = index_info->fields + index_info->n_fields;

	/* Check each index column's datatype. We do not check
	column name because there exists case that index
	column name got modified in mysql but such change does not
	propagate to InnoDB.
	One hidden assumption here is that the index column sequences
	are matched up between those in mysql and InnoDB. */
	for (; key_part != key_end; ++key_part) {
		unsigned is_unsigned;
		auto mtype = innodb_idx_fld->col->mtype;

		/* Need to translate to InnoDB column type before
		comparison. */
		auto col_type = get_innobase_type_from_mysql_type(
			&is_unsigned, key_part->field);

		/* Ignore InnoDB specific system columns. */
		while (mtype == DATA_SYS) {
			innodb_idx_fld++;

			if (innodb_idx_fld >= innodb_idx_fld_end) {
				DBUG_RETURN(FALSE);
			}
		}

		/* MariaDB-5.5 compatibility */
		if ((key_part->field->real_type() == MYSQL_TYPE_ENUM ||
		     key_part->field->real_type() == MYSQL_TYPE_SET) &&
		    mtype == DATA_FIXBINARY) {
			col_type= DATA_FIXBINARY;
		}

		if (col_type != mtype) {
			/* If the col_type we get from mysql type is a geometry
			data type, we should check if mtype is a legacy type
			from 5.6, either upgraded to DATA_GEOMETRY or not.
			This is indeed not an accurate check, but should be
			safe, since DATA_BLOB would be upgraded once we create
			spatial index on it and we intend to use DATA_GEOMETRY
			for legacy GIS data types which are of var-length. */
			switch (col_type) {
			case DATA_GEOMETRY:
				if (mtype == DATA_BLOB) {
					break;
				}
				/* Fall through */
			default:
				/* Column type mismatches */
				DBUG_RETURN(false);
			}
		}

		innodb_idx_fld++;
	}

	DBUG_RETURN(TRUE);
}

/** Build a template for a base column for a virtual column
@param[in]	table		MySQL TABLE
@param[in]	clust_index	InnoDB clustered index
@param[in]	field		field in MySQL table
@param[in]	col		InnoDB column
@param[in,out]	templ		template to fill
@param[in]	col_no		field index for virtual col
*/
static
void
innobase_vcol_build_templ(
	const TABLE*		table,
	dict_index_t*		clust_index,
	Field*			field,
	const dict_col_t*	col,
	mysql_row_templ_t*	templ,
	ulint			col_no)
{
	templ->col_no = col_no;
	templ->is_virtual = col->is_virtual();

	if (templ->is_virtual) {
		templ->clust_rec_field_no = ULINT_UNDEFINED;
		templ->rec_field_no = col->ind;
	} else {
		templ->clust_rec_field_no = dict_col_get_clust_pos(
						col, clust_index);
		ut_a(templ->clust_rec_field_no != ULINT_UNDEFINED);

		templ->rec_field_no = templ->clust_rec_field_no;
	}

	if (field->real_maybe_null()) {
                templ->mysql_null_byte_offset =
                        field->null_offset();

                templ->mysql_null_bit_mask = (ulint) field->null_bit;
        } else {
                templ->mysql_null_bit_mask = 0;
        }

        templ->mysql_col_offset = static_cast<ulint>(
					get_field_offset(table, field));
	templ->mysql_col_len = static_cast<ulint>(field->pack_length());
        templ->type = col->mtype;
        templ->mysql_type = static_cast<ulint>(field->type());

	if (templ->mysql_type == DATA_MYSQL_TRUE_VARCHAR) {
		templ->mysql_length_bytes = static_cast<ulint>(
			((Field_varstring*) field)->length_bytes);
	}

        templ->charset = dtype_get_charset_coll(col->prtype);
        templ->mbminlen = dict_col_get_mbminlen(col);
        templ->mbmaxlen = dict_col_get_mbmaxlen(col);
        templ->is_unsigned = col->prtype & DATA_UNSIGNED;
}

/** Build template for the virtual columns and their base columns. This
is done when the table first opened.
@param[in]	table		MySQL TABLE
@param[in]	ib_table	InnoDB dict_table_t
@param[in,out]	s_templ		InnoDB template structure
@param[in]	add_v		new virtual columns added along with
				add index call
@param[in]	locked		true if dict_sys mutex is held */
void
innobase_build_v_templ(
	const TABLE*		table,
	const dict_table_t*	ib_table,
	dict_vcol_templ_t*	s_templ,
	const dict_add_v_col_t*	add_v,
	bool			locked)
{
	ulint	ncol = unsigned(ib_table->n_cols) - DATA_N_SYS_COLS;
	ulint	n_v_col = ib_table->n_v_cols;
	bool	marker[REC_MAX_N_FIELDS];

	DBUG_ENTER("innobase_build_v_templ");
	ut_ad(ncol < REC_MAX_N_FIELDS);

	if (add_v != NULL) {
		n_v_col += add_v->n_v_col;
	}

	ut_ad(n_v_col > 0);

	if (!locked) {
		mutex_enter(&dict_sys.mutex);
	}

	if (s_templ->vtempl) {
		if (!locked) {
			mutex_exit(&dict_sys.mutex);
		}
		DBUG_VOID_RETURN;
	}

	memset(marker, 0, sizeof(bool) * ncol);

	s_templ->vtempl = static_cast<mysql_row_templ_t**>(
		ut_zalloc_nokey((ncol + n_v_col)
				* sizeof *s_templ->vtempl));
	s_templ->n_col = ncol;
	s_templ->n_v_col = n_v_col;
	s_templ->rec_len = table->s->reclength;
	s_templ->default_rec = UT_NEW_ARRAY_NOKEY(uchar, s_templ->rec_len);
	memcpy(s_templ->default_rec, table->s->default_values, s_templ->rec_len);

	/* Mark those columns could be base columns */
	for (ulint i = 0; i < ib_table->n_v_cols; i++) {
		const dict_v_col_t*	vcol = dict_table_get_nth_v_col(
							ib_table, i);

		for (ulint j = vcol->num_base; j--; ) {
			marker[vcol->base_col[j]->ind] = true;
		}
	}

	if (add_v) {
		for (ulint i = 0; i < add_v->n_v_col; i++) {
			const dict_v_col_t*	vcol = &add_v->v_col[i];

			for (ulint j = vcol->num_base; j--; ) {
				marker[vcol->base_col[j]->ind] = true;
			}
		}
	}

	ulint	j = 0;
	ulint	z = 0;

	dict_index_t*	clust_index = dict_table_get_first_index(ib_table);

	for (ulint i = 0; i < table->s->fields; i++) {
		Field*  field = table->field[i];

		/* Build template for virtual columns */
		if (!field->stored_in_db()) {
#ifdef UNIV_DEBUG
			const char*	name;

			if (z >= ib_table->n_v_def) {
				name = add_v->v_col_name[z - ib_table->n_v_def];
			} else {
				name = dict_table_get_v_col_name(ib_table, z);
			}

			ut_ad(!my_strcasecmp(system_charset_info, name,
					     field->field_name.str));
#endif
			const dict_v_col_t*	vcol;

			if (z >= ib_table->n_v_def) {
				vcol = &add_v->v_col[z - ib_table->n_v_def];
			} else {
				vcol = dict_table_get_nth_v_col(ib_table, z);
			}

			s_templ->vtempl[z + s_templ->n_col]
				= static_cast<mysql_row_templ_t*>(
					ut_malloc_nokey(
					sizeof *s_templ->vtempl[j]));

			innobase_vcol_build_templ(
				table, clust_index, field,
				&vcol->m_col,
				s_templ->vtempl[z + s_templ->n_col],
				z);
			z++;
			continue;
                }

		ut_ad(j < ncol);

		/* Build template for base columns */
		if (marker[j]) {
			dict_col_t*   col = dict_table_get_nth_col(
						ib_table, j);

			ut_ad(!my_strcasecmp(system_charset_info,
					     dict_table_get_col_name(
						     ib_table, j),
					     field->field_name.str));

			s_templ->vtempl[j] = static_cast<
				mysql_row_templ_t*>(
					ut_malloc_nokey(
					sizeof *s_templ->vtempl[j]));

			innobase_vcol_build_templ(
				table, clust_index, field, col,
				s_templ->vtempl[j], j);
		}

		j++;
	}

	if (!locked) {
		mutex_exit(&dict_sys.mutex);
	}

	s_templ->db_name = table->s->db.str;
	s_templ->tb_name = table->s->table_name.str;
	DBUG_VOID_RETURN;
}

/** Check consistency between .frm indexes and InnoDB indexes.
@param[in]	table	table object formed from .frm
@param[in]	ib_table	InnoDB table definition
@retval	true if not errors were found */
static bool
check_index_consistency(const TABLE* table, const dict_table_t* ib_table)
{
	ulint mysql_num_index = table->s->keys;
	ulint ib_num_index = UT_LIST_GET_LEN(ib_table->indexes);
	bool ret = true;

	/* If there exists inconsistency between MySQL and InnoDB dictionary
	(metadata) information, the number of index defined in MySQL
	could exceed that in InnoDB, return error */
	if (ib_num_index < mysql_num_index) {
		ret = false;
		goto func_exit;
	}

	/* For each index in the mysql key_info array, fetch its
	corresponding InnoDB index pointer into index_mapping
	array. */
	for (ulint count = 0; count < mysql_num_index; count++) {
		const dict_index_t* index = dict_table_get_index_on_name(
			ib_table, table->key_info[count].name.str);

		if (index == NULL) {
			sql_print_error("Cannot find index %s in InnoDB"
					" index dictionary.",
					table->key_info[count].name.str);
			ret = false;
			goto func_exit;
		}

		/* Double check fetched index has the same
		column info as those in mysql key_info. */
		if (!innobase_match_index_columns(&table->key_info[count],
						  index)) {
			sql_print_error("Found index %s whose column info"
					" does not match that of MariaDB.",
					table->key_info[count].name.str);
			ret = false;
			goto func_exit;
		}
	}

func_exit:
	return ret;
}

/********************************************************************//**
Get the upper limit of the MySQL integral and floating-point type.
@return maximum allowed value for the field */
UNIV_INTERN
ulonglong
innobase_get_int_col_max_value(
/*===========================*/
	const Field*	field)	/*!< in: MySQL field */
{
	ulonglong	max_value = 0;

	switch (field->key_type()) {
	/* TINY */
	case HA_KEYTYPE_BINARY:
		max_value = 0xFFULL;
		break;
	case HA_KEYTYPE_INT8:
		max_value = 0x7FULL;
		break;
	/* SHORT */
	case HA_KEYTYPE_USHORT_INT:
		max_value = 0xFFFFULL;
		break;
	case HA_KEYTYPE_SHORT_INT:
		max_value = 0x7FFFULL;
		break;
	/* MEDIUM */
	case HA_KEYTYPE_UINT24:
		max_value = 0xFFFFFFULL;
		break;
	case HA_KEYTYPE_INT24:
		max_value = 0x7FFFFFULL;
		break;
	/* LONG */
	case HA_KEYTYPE_ULONG_INT:
		max_value = 0xFFFFFFFFULL;
		break;
	case HA_KEYTYPE_LONG_INT:
		max_value = 0x7FFFFFFFULL;
		break;
	/* BIG */
	case HA_KEYTYPE_ULONGLONG:
		max_value = 0xFFFFFFFFFFFFFFFFULL;
		break;
	case HA_KEYTYPE_LONGLONG:
		max_value = 0x7FFFFFFFFFFFFFFFULL;
		break;
	case HA_KEYTYPE_FLOAT:
		/* We use the maximum as per IEEE754-2008 standard, 2^24 */
		max_value = 0x1000000ULL;
		break;
	case HA_KEYTYPE_DOUBLE:
		/* We use the maximum as per IEEE754-2008 standard, 2^53 */
		max_value = 0x20000000000000ULL;
		break;
	default:
		ut_error;
	}

	return(max_value);
}

/** Initialize the AUTO_INCREMENT column metadata.

Since a partial table definition for a persistent table can already be
present in the InnoDB dict_sys cache before it is accessed from SQL,
we have to initialize the AUTO_INCREMENT counter on the first
ha_innobase::open().

@param[in,out]	table	persistent table
@param[in]	field	the AUTO_INCREMENT column */
static
void
initialize_auto_increment(dict_table_t* table, const Field* field)
{
	ut_ad(!table->is_temporary());

	const unsigned	col_no = innodb_col_no(field);

	table->autoinc_mutex.lock();

	table->persistent_autoinc = static_cast<uint16_t>(
		dict_table_get_nth_col_pos(table, col_no, NULL) + 1)
		& dict_index_t::MAX_N_FIELDS;

	if (table->autoinc) {
		/* Already initialized. Our caller checked
		table->persistent_autoinc without
		autoinc_mutex protection, and there might be multiple
		ha_innobase::open() executing concurrently. */
	} else if (srv_force_recovery > SRV_FORCE_NO_IBUF_MERGE) {
		/* If the recovery level is set so high that writes
		are disabled we force the AUTOINC counter to 0
		value effectively disabling writes to the table.
		Secondly, we avoid reading the table in case the read
		results in failure due to a corrupted table/index.

		We will not return an error to the client, so that the
		tables can be dumped with minimal hassle.  If an error
		were returned in this case, the first attempt to read
		the table would fail and subsequent SELECTs would succeed. */
	} else if (table->persistent_autoinc) {
		table->autoinc = innobase_next_autoinc(
			btr_read_autoinc_with_fallback(table, col_no),
			1 /* need */,
			1 /* auto_increment_increment */,
			0 /* auto_increment_offset */,
			innobase_get_int_col_max_value(field));
	}

	table->autoinc_mutex.unlock();
}

/** Open an InnoDB table
@param[in]	name	table name
@return	error code
@retval	0	on success */
int
ha_innobase::open(const char* name, int, uint)
{
	char			norm_name[FN_REFLEN];

	DBUG_ENTER("ha_innobase::open");

	normalize_table_name(norm_name, name);

	m_user_thd = NULL;

	/* Will be allocated if it is needed in ::update_row() */
	m_upd_buf = NULL;
	m_upd_buf_size = 0;

	char*	is_part = is_partition(norm_name);
	THD*	thd = ha_thd();
	dict_table_t* ib_table = open_dict_table(name, norm_name, is_part,
						 DICT_ERR_IGNORE_FK_NOKEY);

	DEBUG_SYNC(thd, "ib_open_after_dict_open");

	if (NULL == ib_table) {

		if (is_part) {
			sql_print_error("Failed to open table %s.\n",
					norm_name);
		}
		set_my_errno(ENOENT);

		DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
	}

	size_t n_fields = omits_virtual_cols(*table_share)
		? table_share->stored_fields : table_share->fields;
	size_t n_cols = dict_table_get_n_user_cols(ib_table)
		+ dict_table_get_n_v_cols(ib_table)
		- !!DICT_TF2_FLAG_IS_SET(ib_table, DICT_TF2_FTS_HAS_DOC_ID);

	if (UNIV_UNLIKELY(n_cols != n_fields)) {
		ib::warn() << "Table " << norm_name << " contains "
			<< n_cols << " user"
			" defined columns in InnoDB, but " << n_fields
			<< " columns in MariaDB. Please check"
			" INFORMATION_SCHEMA.INNODB_SYS_COLUMNS and"
			" https://mariadb.com/kb/en/innodb-data-dictionary-troubleshooting/"
			" for how to resolve the issue.";

		/* Mark this table as corrupted, so the drop table
		or force recovery can still use it, but not others. */
		ib_table->file_unreadable = true;
		ib_table->corrupted = true;
		dict_table_close(ib_table, FALSE, FALSE);
		set_my_errno(ENOENT);
		DBUG_RETURN(HA_ERR_CRASHED_ON_USAGE);
	}

	innobase_copy_frm_flags_from_table_share(ib_table, table->s);

	/* No point to init any statistics if tablespace is still encrypted. */
	if (ib_table->is_readable()) {
		dict_stats_init(ib_table);
	} else {
		ib_table->stat_initialized = 1;
	}

	MONITOR_INC(MONITOR_TABLE_OPEN);

	if ((ib_table->flags2 & DICT_TF2_DISCARDED)) {

		ib_senderrf(thd,
			IB_LOG_LEVEL_WARN, ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		/* Allow an open because a proper DISCARD should have set
		all the flags and index root page numbers to FIL_NULL that
		should prevent any DML from running but it should allow DDL
		operations. */
	} else if (!ib_table->is_readable()) {
		const fil_space_t* space = ib_table->space;
		if (!space) {
			ib_senderrf(
				thd, IB_LOG_LEVEL_WARN,
				ER_TABLESPACE_MISSING, norm_name);
		}

		if (!thd_tablespace_op(thd)) {
			set_my_errno(ENOENT);
			int ret_err = HA_ERR_TABLESPACE_MISSING;

			if (space && space->crypt_data
			    && space->crypt_data->is_encrypted()) {
				push_warning_printf(
					thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_DECRYPTION_FAILED,
					"Table %s in file %s is encrypted"
					" but encryption service or"
					" used key_id %u is not available. "
					" Can't continue reading table.",
					table_share->table_name.str,
					space->chain.start->name,
					space->crypt_data->key_id);
				ret_err = HA_ERR_DECRYPTION_FAILED;
			}

			dict_table_close(ib_table, FALSE, FALSE);
			DBUG_RETURN(ret_err);
		}
	}

	m_prebuilt = row_create_prebuilt(ib_table, table->s->reclength);

	m_prebuilt->default_rec = table->s->default_values;
	ut_ad(m_prebuilt->default_rec);

	m_prebuilt->m_mysql_table = table;

	/* Looks like MySQL-3.23 sometimes has primary key number != 0 */
	m_primary_key = table->s->primary_key;

	key_used_on_scan = m_primary_key;

	if (ib_table->n_v_cols) {
		mutex_enter(&dict_sys.mutex);
		if (ib_table->vc_templ == NULL) {
			ib_table->vc_templ = UT_NEW_NOKEY(dict_vcol_templ_t());
			innobase_build_v_templ(
				table, ib_table, ib_table->vc_templ, NULL,
				true);
		}

		mutex_exit(&dict_sys.mutex);
	}

	if (!check_index_consistency(table, ib_table)) {
		sql_print_error("InnoDB indexes are inconsistent with what "
				"defined in .frm for table %s",
				name);
	}

	/* Allocate a buffer for a 'row reference'. A row reference is
	a string of bytes of length ref_length which uniquely specifies
	a row in our table. Note that MySQL may also compare two row
	references for equality by doing a simple memcmp on the strings
	of length ref_length! */
	if (!(m_prebuilt->clust_index_was_generated
	      = dict_index_is_auto_gen_clust(ib_table->indexes.start))) {
		if (m_primary_key >= MAX_KEY) {
			ib_table->dict_frm_mismatch = DICT_FRM_NO_PK;

			/* This mismatch could cause further problems
			if not attended, bring this to the user's attention
			by printing a warning in addition to log a message
			in the errorlog */

			ib_push_frm_error(thd, ib_table, table, 0, true);

			/* If m_primary_key >= MAX_KEY, its (m_primary_key)
			value could be out of bound if continue to index
			into key_info[] array. Find InnoDB primary index,
			and assign its key_length to ref_length.
			In addition, since MySQL indexes are sorted starting
			with primary index, unique index etc., initialize
			ref_length to the first index key length in
			case we fail to find InnoDB cluster index.

			Please note, this will not resolve the primary
			index mismatch problem, other side effects are
			possible if users continue to use the table.
			However, we allow this table to be opened so
			that user can adopt necessary measures for the
			mismatch while still being accessible to the table
			date. */
			if (!table->key_info) {
				ut_ad(!table->s->keys);
				ref_length = 0;
			} else {
				ref_length = table->key_info[0].key_length;
			}

			/* Find corresponding cluster index
			key length in MySQL's key_info[] array */
			for (uint i = 0; i < table->s->keys; i++) {
				dict_index_t*	index;
				index = innobase_get_index(i);
				if (dict_index_is_clust(index)) {
					ref_length =
						 table->key_info[i].key_length;
				}
			}
		} else {
			/* MySQL allocates the buffer for ref.
			key_info->key_length includes space for all key
			columns + one byte for each column that may be
			NULL. ref_length must be as exact as possible to
			save space, because all row reference buffers are
			allocated based on ref_length. */

			ref_length = table->key_info[m_primary_key].key_length;
		}
	} else {
		if (m_primary_key != MAX_KEY) {

			ib_table->dict_frm_mismatch = DICT_NO_PK_FRM_HAS;

			/* This mismatch could cause further problems
			if not attended, bring this to the user attention
			by printing a warning in addition to log a message
			in the errorlog */
			ib_push_frm_error(thd, ib_table, table, 0, true);
		}

		ref_length = DATA_ROW_ID_LEN;

		/* If we automatically created the clustered index, then
		MySQL does not know about it, and MySQL must NOT be aware
		of the index used on scan, to make it avoid checking if we
		update the column of the index. That is why we assert below
		that key_used_on_scan is the undefined value MAX_KEY.
		The column is the row id in the automatical generation case,
		and it will never be updated anyway. */

		if (key_used_on_scan != MAX_KEY) {
			sql_print_warning(
				"Table %s key_used_on_scan is %u even "
				"though there is no primary key inside "
				"InnoDB.", name, key_used_on_scan);
		}
	}

	/* Index block size in InnoDB: used by MySQL in query optimization */
	stats.block_size = static_cast<uint>(srv_page_size);

	if (m_prebuilt->table == NULL
	    || m_prebuilt->table->is_temporary()
	    || m_prebuilt->table->persistent_autoinc
	    || !m_prebuilt->table->is_readable()) {
	} else if (const Field* ai = table->found_next_number_field) {
		initialize_auto_increment(m_prebuilt->table, ai);
	}

	/* Set plugin parser for fulltext index */
	for (uint i = 0; i < table->s->keys; i++) {
		if (table->key_info[i].flags & HA_USES_PARSER) {
			dict_index_t*	index = innobase_get_index(i);
			plugin_ref	parser = table->key_info[i].parser;

			ut_ad(index->type & DICT_FTS);
			index->parser =
				static_cast<st_mysql_ftparser *>(
					plugin_decl(parser)->info);

			DBUG_EXECUTE_IF("fts_instrument_use_default_parser",
				index->parser = &fts_default_parser;);
		}
	}

	if (table && m_prebuilt->table) {
		ut_ad(table->versioned() == m_prebuilt->table->versioned());
	}

	info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST | HA_STATUS_OPEN);
	DBUG_RETURN(0);
}

/** Convert MySQL column number to dict_table_t::cols[] offset.
@param[in]	field	non-virtual column
@return	column number relative to dict_table_t::cols[] */
unsigned
innodb_col_no(const Field* field)
{
	ut_ad(!innobase_is_s_fld(field));
	const TABLE*	table	= field->table;
	unsigned	col_no	= 0;
	ut_ad(field == table->field[field->field_index]);
	for (unsigned i = 0; i < field->field_index; i++) {
		if (table->field[i]->stored_in_db()) {
			col_no++;
		}
	}
	return(col_no);
}

/** Opens dictionary table object using table name. For partition, we need to
try alternative lower/upper case names to support moving data files across
platforms.
@param[in]	table_name	name of the table/partition
@param[in]	norm_name	normalized name of the table/partition
@param[in]	is_partition	if this is a partition of a table
@param[in]	ignore_err	error to ignore for loading dictionary object
@return dictionary table object or NULL if not found */
dict_table_t*
ha_innobase::open_dict_table(
	const char*
#ifdef _WIN32
	table_name
#endif
	,
	const char*		norm_name,
	bool			is_partition,
	dict_err_ignore_t	ignore_err)
{
	DBUG_ENTER("ha_innobase::open_dict_table");
	dict_table_t*	ib_table = dict_table_open_on_name(norm_name, FALSE,
							   TRUE, ignore_err);

	if (NULL == ib_table && is_partition) {
		/* MySQL partition engine hard codes the file name
		separator as "#P#". The text case is fixed even if
		lower_case_table_names is set to 1 or 2. This is true
		for sub-partition names as well. InnoDB always
		normalises file names to lower case on Windows, this
		can potentially cause problems when copying/moving
		tables between platforms.

		1) If boot against an installation from Windows
		platform, then its partition table name could
		be in lower case in system tables. So we will
		need to check lower case name when load table.

		2) If we boot an installation from other case
		sensitive platform in Windows, we might need to
		check the existence of table name without lower
		case in the system table. */
		if (innobase_get_lower_case_table_names() == 1) {
			char	par_case_name[FN_REFLEN];

#ifndef _WIN32
			/* Check for the table using lower
			case name, including the partition
			separator "P" */
			strcpy(par_case_name, norm_name);
			innobase_casedn_str(par_case_name);
#else
			/* On Windows platfrom, check
			whether there exists table name in
			system table whose name is
			not being normalized to lower case */
			create_table_info_t::
				normalize_table_name_low(
					par_case_name,
					table_name, FALSE);
#endif
			ib_table = dict_table_open_on_name(
				par_case_name, FALSE, TRUE,
				ignore_err);
		}

		if (ib_table != NULL) {
#ifndef _WIN32
			sql_print_warning("Partition table %s opened"
					  " after converting to lower"
					  " case. The table may have"
					  " been moved from a case"
					  " in-sensitive file system."
					  " Please recreate table in"
					  " the current file system\n",
					  norm_name);
#else
			sql_print_warning("Partition table %s opened"
					  " after skipping the step to"
					  " lower case the table name."
					  " The table may have been"
					  " moved from a case sensitive"
					  " file system. Please"
					  " recreate table in the"
					  " current file system\n",
					  norm_name);
#endif
		}
	}

	DBUG_RETURN(ib_table);
}

handler*
ha_innobase::clone(
/*===============*/
	const char*	name,		/*!< in: table name */
	MEM_ROOT*	mem_root)	/*!< in: memory context */
{
	DBUG_ENTER("ha_innobase::clone");

	ha_innobase*	new_handler = static_cast<ha_innobase*>(
		handler::clone(m_prebuilt->table->name.m_name, mem_root));

	if (new_handler != NULL) {
		DBUG_ASSERT(new_handler->m_prebuilt != NULL);

		new_handler->m_prebuilt->select_lock_type
			= m_prebuilt->select_lock_type;
	}

	DBUG_RETURN(new_handler);
}


uint
ha_innobase::max_supported_key_part_length() const
/*==============================================*/
{
	/* A table format specific index column length check will be performed
	at ha_innobase::add_index() and row_create_index_for_mysql() */
	return(REC_VERSION_56_MAX_INDEX_COL_LEN);
}

/******************************************************************//**
Closes a handle to an InnoDB table.
@return 0 */

int
ha_innobase::close()
/*================*/
{
	DBUG_ENTER("ha_innobase::close");

	row_prebuilt_free(m_prebuilt, FALSE);

	if (m_upd_buf != NULL) {
		ut_ad(m_upd_buf_size != 0);
		my_free(m_upd_buf);
		m_upd_buf = NULL;
		m_upd_buf_size = 0;
	}

	MONITOR_INC(MONITOR_TABLE_CLOSE);

	DBUG_RETURN(0);
}

/* The following accessor functions should really be inside MySQL code! */

#ifdef WITH_WSREP
UNIV_INTERN
ulint
wsrep_innobase_mysql_sort(
/*======================*/
					/* out: str contains sort string */
	int		mysql_type,	/* in: MySQL type */
	uint		charset_number,	/* in: number of the charset */
	unsigned char*	str,		/* in: data field */
	ulint		str_length,	/* in: data field length,
					not UNIV_SQL_NULL */
	unsigned int	buf_length)	/* in: total str buffer length */

{
	CHARSET_INFO*		charset;
	enum_field_types	mysql_tp;
	ulint			ret_length =	str_length;

	DBUG_ASSERT(str_length != UNIV_SQL_NULL);

	mysql_tp = (enum_field_types) mysql_type;

	switch (mysql_tp) {

	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
	case MYSQL_TYPE_VARCHAR:
	{
		uchar tmp_str[REC_VERSION_56_MAX_INDEX_COL_LEN] = {'\0'};
		ulint tmp_length = REC_VERSION_56_MAX_INDEX_COL_LEN;

		/* Use the charset number to pick the right charset struct for
		the comparison. Since the MySQL function get_charset may be
		slow before Bar removes the mutex operation there, we first
		look at 2 common charsets directly. */

		if (charset_number == default_charset_info->number) {
			charset = default_charset_info;
		} else if (charset_number == my_charset_latin1.number) {
			charset = &my_charset_latin1;
		} else {
			charset = get_charset(charset_number, MYF(MY_WME));

			if (charset == NULL) {
			  sql_print_error("InnoDB needs charset %lu for doing "
					  "a comparison, but MariaDB cannot "
					  "find that charset.",
					  (ulong) charset_number);
				ut_a(0);
			}
		}

		ut_a(str_length <= tmp_length);
		memcpy(tmp_str, str, str_length);

		tmp_length = charset->strnxfrm(str, str_length,
					       uint(str_length), tmp_str,
					       tmp_length, 0);
		DBUG_ASSERT(tmp_length <= str_length);
		if (wsrep_protocol_version < 3) {
			tmp_length = charset->strnxfrm(
				str, str_length,
				uint(str_length), tmp_str, tmp_length, 0);
			DBUG_ASSERT(tmp_length <= str_length);
		} else {
			/* strnxfrm will expand the destination string,
			   protocols < 3 truncated the sorted sring
			   protocols >= 3 gets full sorted sring
			*/
			tmp_length = charset->strnxfrm(
				str, buf_length,
				uint(str_length), tmp_str, str_length, 0);
			DBUG_ASSERT(tmp_length <= buf_length);
			ret_length = tmp_length;
		}

		break;
	}
	case MYSQL_TYPE_DECIMAL :
	case MYSQL_TYPE_TINY :
	case MYSQL_TYPE_SHORT :
	case MYSQL_TYPE_LONG :
	case MYSQL_TYPE_FLOAT :
	case MYSQL_TYPE_DOUBLE :
	case MYSQL_TYPE_NULL :
	case MYSQL_TYPE_TIMESTAMP :
	case MYSQL_TYPE_LONGLONG :
	case MYSQL_TYPE_INT24 :
	case MYSQL_TYPE_DATE :
	case MYSQL_TYPE_TIME :
	case MYSQL_TYPE_DATETIME :
	case MYSQL_TYPE_YEAR :
	case MYSQL_TYPE_NEWDATE :
	case MYSQL_TYPE_NEWDECIMAL :
	case MYSQL_TYPE_ENUM :
	case MYSQL_TYPE_SET :
	case MYSQL_TYPE_GEOMETRY :
		break;
	default:
		break;
	}

	return ret_length;
}
#endif /* WITH_WSREP */

/******************************************************************//**
compare two character string according to their charset. */
int
innobase_fts_text_cmp(
/*==================*/
	const void*	cs,		/*!< in: Character set */
	const void*     p1,		/*!< in: key */
	const void*     p2)		/*!< in: node */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;

	return(ha_compare_text(
		charset, s1->f_str, static_cast<uint>(s1->f_len),
		s2->f_str, static_cast<uint>(s2->f_len), 0));
}

/******************************************************************//**
compare two character string case insensitively according to their charset. */
int
innobase_fts_text_case_cmp(
/*=======================*/
	const void*	cs,		/*!< in: Character set */
	const void*     p1,		/*!< in: key */
	const void*     p2)		/*!< in: node */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;
	ulint			newlen;

	my_casedn_str(charset, (char*) s2->f_str);

	newlen = strlen((const char*) s2->f_str);

	return(ha_compare_text(
		charset, s1->f_str, static_cast<uint>(s1->f_len),
		s2->f_str, static_cast<uint>(newlen), 0));
}

/******************************************************************//**
Get the first character's code position for FTS index partition. */
ulint
innobase_strnxfrm(
/*==============*/
	const CHARSET_INFO*
			cs,		/*!< in: Character set */
	const uchar*	str,		/*!< in: string */
	const ulint	len)		/*!< in: string length */
{
	uchar		mystr[2];
	ulint		value;

	if (!str || len == 0) {
		return(0);
	}

	cs->strnxfrm((uchar*) mystr, 2, str, len);

	value = mach_read_from_2(mystr);

	if (value > 255) {
		value = value / 256;
	}

	return(value);
}

/******************************************************************//**
compare two character string according to their charset. */
int
innobase_fts_text_cmp_prefix(
/*=========================*/
	const void*	cs,		/*!< in: Character set */
	const void*	p1,		/*!< in: prefix key */
	const void*	p2)		/*!< in: value to compare */
{
	const CHARSET_INFO*	charset = (const CHARSET_INFO*) cs;
	const fts_string_t*	s1 = (const fts_string_t*) p1;
	const fts_string_t*	s2 = (const fts_string_t*) p2;
	int			result;

	result = ha_compare_text(
		charset, s2->f_str, static_cast<uint>(s2->f_len),
		s1->f_str, static_cast<uint>(s1->f_len), 1);

	/* We switched s1, s2 position in ha_compare_text. So we need
	to negate the result */
	return(-result);
}

/******************************************************************//**
Makes all characters in a string lower case. */
size_t
innobase_fts_casedn_str(
/*====================*/
	CHARSET_INFO*	cs,	/*!< in: Character set */
	char*		src,	/*!< in: string to put in lower case */
	size_t		src_len,/*!< in: input string length */
	char*		dst,	/*!< in: buffer for result string */
	size_t		dst_len)/*!< in: buffer size */
{
	if (cs->casedn_multiply == 1) {
		memcpy(dst, src, src_len);
		dst[src_len] = 0;
		my_casedn_str(cs, dst);

		return(strlen(dst));
	} else {
		return(cs->casedn(src, src_len, dst, dst_len));
	}
}

#define true_word_char(c, ch) ((c) & (_MY_U | _MY_L | _MY_NMR) || (ch) == '_')

#define misc_word_char(X)       0

/*************************************************************//**
Get the next token from the given string and store it in *token.
It is mostly copied from MyISAM's doc parsing function ft_simple_get_word()
@return length of string processed */
ulint
innobase_mysql_fts_get_token(
/*=========================*/
	CHARSET_INFO*	cs,		/*!< in: Character set */
	const byte*	start,		/*!< in: start of text */
	const byte*	end,		/*!< in: one character past end of
					text */
	fts_string_t*	token)		/*!< out: token's text */
{
	int		mbl;
	const uchar*	doc = start;

	ut_a(cs);

	token->f_n_char = token->f_len = 0;
	token->f_str = NULL;

	for (;;) {

		if (doc >= end) {
			return ulint(doc - start);
		}

		int	ctype;

		mbl = cs->ctype(&ctype, doc, (const uchar*) end);

		if (true_word_char(ctype, *doc)) {
			break;
		}

		doc += mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1);
	}

	ulint	mwc = 0;
	ulint	length = 0;

	token->f_str = const_cast<byte*>(doc);

	while (doc < end) {

		int	ctype;

		mbl = cs->ctype(&ctype, (uchar*) doc, (uchar*) end);
		if (true_word_char(ctype, *doc)) {
			mwc = 0;
		} else if (!misc_word_char(*doc) || mwc) {
			break;
		} else {
			++mwc;
		}

		++length;

		doc += mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1);
	}

	token->f_len = (uint) (doc - token->f_str) - mwc;
	token->f_n_char = length;

	return ulint(doc - start);
}

/** Converts a MySQL type to an InnoDB type. Note that this function returns
the 'mtype' of InnoDB. InnoDB differentiates between MySQL's old <= 4.1
VARCHAR and the new true VARCHAR in >= 5.0.3 by the 'prtype'.
@param[out]	unsigned_flag	DATA_UNSIGNED if an 'unsigned type'; at least
ENUM and SET, and unsigned integer types are 'unsigned types'
@param[in]	f		MySQL Field
@return DATA_BINARY, DATA_VARCHAR, ... */
uint8_t
get_innobase_type_from_mysql_type(unsigned *unsigned_flag, const Field *field)
{
	/* The following asserts try to check that the MySQL type code fits in
	8 bits: this is used in ibuf and also when DATA_NOT_NULL is ORed to
	the type */

	static_assert(MYSQL_TYPE_STRING < 256, "compatibility");
	static_assert(MYSQL_TYPE_VAR_STRING < 256, "compatibility");
	static_assert(MYSQL_TYPE_DOUBLE < 256, "compatibility");
	static_assert(MYSQL_TYPE_FLOAT < 256, "compatibility");
	static_assert(MYSQL_TYPE_DECIMAL < 256, "compatibility");

	if (field->flags & UNSIGNED_FLAG) {

		*unsigned_flag = DATA_UNSIGNED;
	} else {
		*unsigned_flag = 0;
	}

	if (field->real_type() == MYSQL_TYPE_ENUM
		|| field->real_type() == MYSQL_TYPE_SET) {

		/* MySQL has field->type() a string type for these, but the
		data is actually internally stored as an unsigned integer
		code! */

		*unsigned_flag = DATA_UNSIGNED; /* MySQL has its own unsigned
						flag set to zero, even though
						internally this is an unsigned
						integer type */
		return(DATA_INT);
	}

	switch (field->type()) {
		/* NOTE that we only allow string types in DATA_MYSQL and
		DATA_VARMYSQL */
	case MYSQL_TYPE_VAR_STRING:	/* old <= 4.1 VARCHAR */
	case MYSQL_TYPE_VARCHAR:	/* new >= 5.0.3 true VARCHAR */
		if (field->binary()) {
			return(DATA_BINARY);
		} else if (field->charset() == &my_charset_latin1) {
			return(DATA_VARCHAR);
		} else {
			return(DATA_VARMYSQL);
		}
	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_STRING:
		if (field->binary() || field->key_type() == HA_KEYTYPE_BINARY) {
			return(DATA_FIXBINARY);
		} else if (field->charset() == &my_charset_latin1) {
			return(DATA_CHAR);
		} else {
			return(DATA_MYSQL);
		}
	case MYSQL_TYPE_NEWDECIMAL:
		return(DATA_FIXBINARY);
	case MYSQL_TYPE_LONG:
	case MYSQL_TYPE_LONGLONG:
	case MYSQL_TYPE_TINY:
	case MYSQL_TYPE_SHORT:
	case MYSQL_TYPE_INT24:
	case MYSQL_TYPE_DATE:
	case MYSQL_TYPE_YEAR:
	case MYSQL_TYPE_NEWDATE:
		return(DATA_INT);
	case MYSQL_TYPE_TIME:
	case MYSQL_TYPE_DATETIME:
	case MYSQL_TYPE_TIMESTAMP:
		if (field->key_type() == HA_KEYTYPE_BINARY) {
			return(DATA_FIXBINARY);
		} else {
			return(DATA_INT);
		}
	case MYSQL_TYPE_FLOAT:
		return(DATA_FLOAT);
	case MYSQL_TYPE_DOUBLE:
		return(DATA_DOUBLE);
	case MYSQL_TYPE_DECIMAL:
		return(DATA_DECIMAL);
	case MYSQL_TYPE_GEOMETRY:
		return(DATA_GEOMETRY);
	case MYSQL_TYPE_TINY_BLOB:
	case MYSQL_TYPE_MEDIUM_BLOB:
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_LONG_BLOB:
		return(DATA_BLOB);
	case MYSQL_TYPE_NULL:
		/* MySQL currently accepts "NULL" datatype, but will
		reject such datatype in the next release. We will cope
		with it and not trigger assertion failure in 5.1 */
		break;
	default:
		ut_error;
	}

	return(0);
}

/*******************************************************************//**
Reads an unsigned integer value < 64k from 2 bytes, in the little-endian
storage format.
@return value */
static inline
uint
innobase_read_from_2_little_endian(
/*===============================*/
	const uchar*	buf)	/*!< in: from where to read */
{
	return((uint) ((ulint)(buf[0]) + 256 * ((ulint)(buf[1]))));
}

#ifdef WITH_WSREP
/*******************************************************************//**
Stores a key value for a row to a buffer.
@return	key value length as stored in buff */
static
uint16_t
wsrep_store_key_val_for_row(
/*=========================*/
	THD* 		thd,
	TABLE*		table,
	uint		keynr,	/*!< in: key number */
	char*		buff,	/*!< in/out: buffer for the key value (in MySQL
				format) */
	uint		buff_len,/*!< in: buffer length */
	const uchar*	record,
	bool*		key_is_null)/*!< out: full key was null */
{
	KEY*		key_info	= table->key_info + keynr;
	KEY_PART_INFO*	key_part	= key_info->key_part;
	KEY_PART_INFO*	end		= key_part + key_info->user_defined_key_parts;
	char*		buff_start	= buff;
	enum_field_types mysql_type;
	Field*		field;
	ulint buff_space = buff_len;

	DBUG_ENTER("wsrep_store_key_val_for_row");

	memset(buff, 0, buff_len);
	*key_is_null = true;

	for (; key_part != end; key_part++) {

		uchar sorted[REC_VERSION_56_MAX_INDEX_COL_LEN] = {'\0'};
		bool part_is_null = false;

		if (key_part->null_bit) {
			if (buff_space > 0) {
				if (record[key_part->null_offset]
				    & key_part->null_bit) {
					*buff = 1;
					part_is_null = true;
				} else {
					*buff = 0;
				}
				buff++;
				buff_space--;
			} else {
				fprintf (stderr, "WSREP: key truncated: %s\n",
					 wsrep_thd_query(thd));
			}
		}
		if (!part_is_null)  *key_is_null = false;

		field = key_part->field;
		mysql_type = field->type();

		if (mysql_type == MYSQL_TYPE_VARCHAR) {
						/* >= 5.0.3 true VARCHAR */
			ulint		lenlen;
			ulint		len;
			const byte*	data;
			ulint		key_len;
			ulint		true_len;
			const CHARSET_INFO* cs;
			int		error=0;

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len + 2;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;
				continue;
			}
			cs = field->charset();

			lenlen = (ulint)
				(((Field_varstring*)field)->length_bytes);

			data = row_mysql_read_true_varchar(&len,
				(byte*) (record
				+ (ulint)get_field_offset(table, field)),
				lenlen);

			true_len = len;

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) my_well_formed_length(cs,
						(const char *) data,
						(const char *) data + len,
						(uint) (key_len /
						cs->mbmaxlen),
						&error);
			}

			/* In a column prefix index, we may need to truncate
			the stored value: */

			if (true_len > key_len) {
				true_len = key_len;
			}

			memcpy(sorted, data, true_len);
			true_len = wsrep_innobase_mysql_sort(
				mysql_type, cs->number, sorted, true_len,
				REC_VERSION_56_MAX_INDEX_COL_LEN);
			if (wsrep_protocol_version > 1) {
				/* Note that we always reserve the maximum possible
				length of the true VARCHAR in the key value, though
				only len first bytes after the 2 length bytes contain
				actual data. The rest of the space was reset to zero
				in the bzero() call above. */
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
 				memcpy(buff, sorted, true_len);
				buff += true_len;
				buff_space -= true_len;
			} else {
				buff += key_len;
			}
		} else if (mysql_type == MYSQL_TYPE_TINY_BLOB
			|| mysql_type == MYSQL_TYPE_MEDIUM_BLOB
			|| mysql_type == MYSQL_TYPE_BLOB
			|| mysql_type == MYSQL_TYPE_LONG_BLOB
			/* MYSQL_TYPE_GEOMETRY data is treated
			as BLOB data in innodb. */
			|| mysql_type == MYSQL_TYPE_GEOMETRY) {

			const CHARSET_INFO* cs;
			ulint		key_len;
			ulint		true_len;
			int		error=0;
			ulint		blob_len;
			const byte*	blob_data;

			ut_a(key_part->key_part_flag & HA_PART_KEY_SEG);

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len + 2;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;

				continue;
			}

			cs = field->charset();

			blob_data = row_mysql_read_blob_ref(&blob_len,
				(byte*) (record
				+ (ulint)get_field_offset(table, field)),
					(ulint) field->pack_length());

			true_len = blob_len;

			ut_a(get_field_offset(table, field)
				== key_part->offset);

			/* For multi byte character sets we need to calculate
			the true length of the key */

			if (blob_len > 0 && cs->mbmaxlen > 1) {
				true_len = (ulint) my_well_formed_length(cs,
						(const char *) blob_data,
						(const char *) blob_data
							+ blob_len,
						(uint) (key_len /
							cs->mbmaxlen),
						&error);
			}

			/* All indexes on BLOB and TEXT are column prefix
			indexes, and we may need to truncate the data to be
			stored in the key value: */

			if (true_len > key_len) {
				true_len = key_len;
			}

			memcpy(sorted, blob_data, true_len);
			true_len = wsrep_innobase_mysql_sort(
				mysql_type, cs->number, sorted, true_len,
				REC_VERSION_56_MAX_INDEX_COL_LEN);


			/* Note that we always reserve the maximum possible
			length of the BLOB prefix in the key value. */
			if (wsrep_protocol_version > 1) {
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;
			} else {
				buff += key_len;
			}
			memcpy(buff, sorted, true_len);
		} else {
			/* Here we handle all other data types except the
			true VARCHAR, BLOB and TEXT. Note that the column
			value we store may be also in a column prefix
			index. */

			const CHARSET_INFO*	cs = NULL;
			ulint			true_len;
			ulint			key_len;
			const uchar*		src_start;
			int			error=0;
			enum_field_types	real_type;

			key_len = key_part->length;

			if (part_is_null) {
				true_len = key_len;
				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len = buff_space;
				}
				buff       += true_len;
				buff_space -= true_len;

				continue;
			}

			src_start = record + key_part->offset;
			real_type = field->real_type();
			true_len = key_len;

			/* Character set for the field is defined only
			to fields whose type is string and real field
			type is not enum or set. For these fields check
			if character set is multi byte. */

			if (real_type != MYSQL_TYPE_ENUM
				&& real_type != MYSQL_TYPE_SET
				&& ( mysql_type == MYSQL_TYPE_VAR_STRING
					|| mysql_type == MYSQL_TYPE_STRING)) {

				cs = field->charset();

				/* For multi byte character sets we need to
				calculate the true length of the key */

				if (key_len > 0 && cs->mbmaxlen > 1) {

					true_len = (ulint)
						my_well_formed_length(cs,
							(const char *)src_start,
							(const char *)src_start
								+ key_len,
							(uint) (key_len /
								cs->mbmaxlen),
							&error);
				}
				memcpy(sorted, src_start, true_len);
				true_len = wsrep_innobase_mysql_sort(
					mysql_type, cs->number, sorted, true_len,
					REC_VERSION_56_MAX_INDEX_COL_LEN);

				if (true_len > buff_space) {
					fprintf (stderr,
						 "WSREP: key truncated: %s\n",
						 wsrep_thd_query(thd));
					true_len   = buff_space;
				}
				memcpy(buff, sorted, true_len);
			} else {
				memcpy(buff, src_start, true_len);
			}
			buff       += true_len;
			buff_space -= true_len;
		}
	}

	ut_a(buff <= buff_start + buff_len);

	DBUG_RETURN(static_cast<uint16_t>(buff - buff_start));
}
#endif /* WITH_WSREP */
/**************************************************************//**
Determines if a field is needed in a m_prebuilt struct 'template'.
@return field to use, or NULL if the field is not needed */
static
const Field*
build_template_needs_field(
/*=======================*/
	bool		index_contains,	/*!< in:
					dict_index_t::contains_col_or_prefix(
					i) */
	bool		read_just_key,	/*!< in: TRUE when MySQL calls
					ha_innobase::extra with the
					argument HA_EXTRA_KEYREAD; it is enough
					to read just columns defined in
					the index (i.e., no read of the
					clustered index record necessary) */
	bool		fetch_all_in_key,
					/*!< in: true=fetch all fields in
					the index */
	bool		fetch_primary_key_cols,
					/*!< in: true=fetch the
					primary key columns */
	dict_index_t*	index,		/*!< in: InnoDB index to use */
	const TABLE*	table,		/*!< in: MySQL table object */
	ulint		i,		/*!< in: field index in InnoDB table */
	ulint		num_v)		/*!< in: num virtual column so far */
{
	const Field*	field	= table->field[i];

	if (!field->stored_in_db()
	    && ha_innobase::omits_virtual_cols(*table->s)) {
		return NULL;
	}

	if (!index_contains) {
		if (read_just_key) {
			/* If this is a 'key read', we do not need
			columns that are not in the key */

			return(NULL);
		}
	} else if (fetch_all_in_key) {
		/* This field is needed in the query */

		return(field);
	}

	if (bitmap_is_set(table->read_set, static_cast<uint>(i))
	    || bitmap_is_set(table->write_set, static_cast<uint>(i))) {
		/* This field is needed in the query */

		return(field);
	}

	ut_ad(i >= num_v);
	if (fetch_primary_key_cols
	    && dict_table_col_in_clustered_key(index->table, i - num_v)) {
		/* This field is needed in the query */
		return(field);
	}

	/* This field is not needed in the query, skip it */

	return(NULL);
}

/**************************************************************//**
Determines if a field is needed in a m_prebuilt struct 'template'.
@return whether the field is needed for index condition pushdown */
inline
bool
build_template_needs_field_in_icp(
/*==============================*/
	const dict_index_t*	index,	/*!< in: InnoDB index */
	const row_prebuilt_t*	prebuilt,/*!< in: row fetch template */
	bool			contains,/*!< in: whether the index contains
					column i */
	ulint			i,	/*!< in: column number */
	bool			is_virtual)
					/*!< in: a virtual column or not */
{
	ut_ad(contains == index->contains_col_or_prefix(i, is_virtual));

	return(index == prebuilt->index
	       ? contains
	       : prebuilt->index->contains_col_or_prefix(i, is_virtual));
}

/**************************************************************//**
Adds a field to a m_prebuilt struct 'template'.
@return the field template */
static
mysql_row_templ_t*
build_template_field(
/*=================*/
	row_prebuilt_t*	prebuilt,	/*!< in/out: template */
	dict_index_t*	clust_index,	/*!< in: InnoDB clustered index */
	dict_index_t*	index,		/*!< in: InnoDB index to use */
	TABLE*		table,		/*!< in: MySQL table object */
	const Field*	field,		/*!< in: field in MySQL table */
	ulint		i,		/*!< in: field index in InnoDB table */
	ulint		v_no)		/*!< in: field index for virtual col */
{
	mysql_row_templ_t*	templ;
	const dict_col_t*	col;

	ut_ad(clust_index->table == index->table);

	templ = prebuilt->mysql_template + prebuilt->n_template++;
	MEM_UNDEFINED(templ, sizeof *templ);
	templ->rec_field_is_prefix = FALSE;
	templ->rec_prefix_field_no = ULINT_UNDEFINED;
	templ->is_virtual = !field->stored_in_db();

	if (!templ->is_virtual) {
		templ->col_no = i;
		col = dict_table_get_nth_col(index->table, i);
		templ->clust_rec_field_no = dict_col_get_clust_pos(
						col, clust_index);
		/* If clustered index record field is not found, lets print out
		field names and all the rest to understand why field is not found. */
		if (templ->clust_rec_field_no == ULINT_UNDEFINED) {
			const char* tb_col_name = dict_table_get_col_name(clust_index->table, i);
			dict_field_t* field=NULL;
			size_t size = 0;

			for(ulint j=0; j < clust_index->n_user_defined_cols; j++) {
				dict_field_t* ifield = &(clust_index->fields[j]);
				if (ifield && !memcmp(tb_col_name, ifield->name,
						strlen(tb_col_name))) {
					field = ifield;
					break;
				}
			}

			ib::info() << "Looking for field " << i << " name "
				<< (tb_col_name ? tb_col_name : "NULL")
				<< " from table " << clust_index->table->name;


			for(ulint j=0; j < clust_index->n_user_defined_cols; j++) {
				dict_field_t* ifield = &(clust_index->fields[j]);
				ib::info() << "InnoDB Table "
					<< clust_index->table->name
					<< "field " << j << " name "
					<< (ifield ? ifield->name() : "NULL");
			}

			for(ulint j=0; j < table->s->stored_fields; j++) {
				ib::info() << "MySQL table "
					<< table->s->table_name.str
					<< " field " << j << " name "
					<< table->field[j]->field_name.str;
			}

			ib::fatal() << "Clustered record field for column " << i
				<< " not found table n_user_defined "
				<< clust_index->n_user_defined_cols
				<< " index n_user_defined "
				<< clust_index->table->n_cols - DATA_N_SYS_COLS
				<< " InnoDB table "
				<< clust_index->table->name
				<< " field name "
				<< (field ? field->name() : "NULL")
				<< " MySQL table "
				<< table->s->table_name.str
				<< " field name "
				<< (tb_col_name ? tb_col_name : "NULL")
				<< " n_fields "
				<< table->s->stored_fields
				<< " query "
				<< innobase_get_stmt_unsafe(current_thd, &size);
		}

		if (dict_index_is_clust(index)) {
			templ->rec_field_no = templ->clust_rec_field_no;
		} else {
			/* If we're in a secondary index, keep track
			* of the original index position even if this
			* is just a prefix index; we will use this
			* later to avoid a cluster index lookup in
			* some cases.*/

			templ->rec_field_no = dict_index_get_nth_col_pos(index, i,
						&templ->rec_prefix_field_no);
		}
	} else {
		DBUG_ASSERT(!ha_innobase::omits_virtual_cols(*table->s));
		col = &dict_table_get_nth_v_col(index->table, v_no)->m_col;
		templ->clust_rec_field_no = v_no;

		if (dict_index_is_clust(index)) {
			templ->rec_field_no = templ->clust_rec_field_no;
		} else {
			templ->rec_field_no
				= dict_index_get_nth_col_or_prefix_pos(
					index, v_no, FALSE, true,
					&templ->rec_prefix_field_no);
		}
		templ->icp_rec_field_no = ULINT_UNDEFINED;
	}

	if (field->real_maybe_null()) {
		templ->mysql_null_byte_offset =
			field->null_offset();

		templ->mysql_null_bit_mask = (ulint) field->null_bit;
	} else {
		templ->mysql_null_bit_mask = 0;
	}


	templ->mysql_col_offset = (ulint) get_field_offset(table, field);
	templ->mysql_col_len = (ulint) field->pack_length();
	templ->type = col->mtype;
	templ->mysql_type = (ulint) field->type();

	if (templ->mysql_type == DATA_MYSQL_TRUE_VARCHAR) {
		templ->mysql_length_bytes = (ulint)
			(((Field_varstring*) field)->length_bytes);
	} else {
		templ->mysql_length_bytes = 0;
	}

	templ->charset = dtype_get_charset_coll(col->prtype);
	templ->mbminlen = dict_col_get_mbminlen(col);
	templ->mbmaxlen = dict_col_get_mbmaxlen(col);
	templ->is_unsigned = col->prtype & DATA_UNSIGNED;

	if (!dict_index_is_clust(index)
	    && templ->rec_field_no == ULINT_UNDEFINED) {
		prebuilt->need_to_access_clustered = TRUE;

		if (templ->rec_prefix_field_no != ULINT_UNDEFINED) {
			dict_field_t* field = dict_index_get_nth_field(
						index,
						templ->rec_prefix_field_no);
			templ->rec_field_is_prefix = (field->prefix_len != 0);
		}
	}

	/* For spatial index, we need to access cluster index. */
	if (dict_index_is_spatial(index)) {
		prebuilt->need_to_access_clustered = TRUE;
	}

	if (prebuilt->mysql_prefix_len < templ->mysql_col_offset
	    + templ->mysql_col_len) {
		prebuilt->mysql_prefix_len = templ->mysql_col_offset
			+ templ->mysql_col_len;
	}

	if (DATA_LARGE_MTYPE(templ->type)) {
		prebuilt->templ_contains_blob = TRUE;
	}

	return(templ);
}

/**************************************************************//**
Builds a 'template' to the m_prebuilt struct. The template is used in fast
retrieval of just those column values MySQL needs in its processing. */

void
ha_innobase::build_template(
/*========================*/
	bool		whole_row)	/*!< in: true=ROW_MYSQL_WHOLE_ROW,
					false=ROW_MYSQL_REC_FIELDS */
{
	dict_index_t*	index;
	dict_index_t*	clust_index;
	ibool		fetch_all_in_key	= FALSE;
	ibool		fetch_primary_key_cols	= FALSE;

	if (m_prebuilt->select_lock_type == LOCK_X || m_prebuilt->table->no_rollback()) {
		/* We always retrieve the whole clustered index record if we
		use exclusive row level locks, for example, if the read is
		done in an UPDATE statement or if we are using a no rollback
                table */

		whole_row = true;
	} else if (!whole_row) {
		if (m_prebuilt->hint_need_to_fetch_extra_cols
			== ROW_RETRIEVE_ALL_COLS) {

			/* We know we must at least fetch all columns in the
			key, or all columns in the table */

			if (m_prebuilt->read_just_key) {
				/* MySQL has instructed us that it is enough
				to fetch the columns in the key; looks like
				MySQL can set this flag also when there is
				only a prefix of the column in the key: in
				that case we retrieve the whole column from
				the clustered index */

				fetch_all_in_key = TRUE;
			} else {
				whole_row = true;
			}
		} else if (m_prebuilt->hint_need_to_fetch_extra_cols
			== ROW_RETRIEVE_PRIMARY_KEY) {
			/* We must at least fetch all primary key cols. Note
			that if the clustered index was internally generated
			by InnoDB on the row id (no primary key was
			defined), then row_search_for_mysql() will always
			retrieve the row id to a special buffer in the
			m_prebuilt struct. */

			fetch_primary_key_cols = TRUE;
		}
	}

	clust_index = dict_table_get_first_index(m_prebuilt->table);

	index = whole_row ? clust_index : m_prebuilt->index;

	m_prebuilt->versioned_write = table->versioned_write(VERS_TRX_ID);
	m_prebuilt->need_to_access_clustered = (index == clust_index);

	/* Either m_prebuilt->index should be a secondary index, or it
	should be the clustered index. */
	ut_ad(dict_index_is_clust(index) == (index == clust_index));

	/* Below we check column by column if we need to access
	the clustered index. */

	if (pushed_rowid_filter && rowid_filter_is_active) {
		fetch_primary_key_cols = TRUE;
		m_prebuilt->pk_filter = this;
	} else {
		m_prebuilt->pk_filter = NULL;
	}

	const bool skip_virtual = omits_virtual_cols(*table_share);
	const ulint n_fields = table_share->fields;

	if (!m_prebuilt->mysql_template) {
		m_prebuilt->mysql_template = (mysql_row_templ_t*)
			ut_malloc_nokey(n_fields * sizeof(mysql_row_templ_t));
	}

	m_prebuilt->template_type = whole_row
		? ROW_MYSQL_WHOLE_ROW : ROW_MYSQL_REC_FIELDS;
	m_prebuilt->null_bitmap_len = table->s->null_bytes
		& dict_index_t::MAX_N_FIELDS;

	/* Prepare to build m_prebuilt->mysql_template[]. */
	m_prebuilt->templ_contains_blob = FALSE;
	m_prebuilt->mysql_prefix_len = 0;
	m_prebuilt->n_template = 0;
	m_prebuilt->idx_cond_n_cols = 0;

	/* Note that in InnoDB, i is the column number in the table.
	MySQL calls columns 'fields'. */

	ulint num_v = 0;

	if ((active_index != MAX_KEY
	     && active_index == pushed_idx_cond_keyno)
	    || (pushed_rowid_filter && rowid_filter_is_active)) {
		/* Push down an index condition or an end_range check. */
		for (ulint i = 0; i < n_fields; i++) {
			const Field* field = table->field[i];
			const bool is_v = !field->stored_in_db();
			if (is_v && skip_virtual) {
				num_v++;
				continue;
			}
			bool index_contains = index->contains_col_or_prefix(
				is_v ? num_v : i - num_v, is_v);
			if (is_v && index_contains) {
				m_prebuilt->n_template = 0;
				num_v = 0;
				goto no_icp;
			}

			/* Test if an end_range or an index condition
			refers to the field. Note that "index" and
			"index_contains" may refer to the clustered index.
			Index condition pushdown is relative to
			m_prebuilt->index (the index that is being
			looked up first). */

			/* When join_read_always_key() invokes this
			code via handler::ha_index_init() and
			ha_innobase::index_init(), end_range is not
			yet initialized. Because of that, we must
			always check for index_contains, instead of
			the subset
			field->part_of_key.is_set(active_index)
			which would be acceptable if end_range==NULL. */
			if (build_template_needs_field_in_icp(
				    index, m_prebuilt, index_contains,
				    is_v ? num_v : i - num_v, is_v)) {
				if (!whole_row) {
					field = build_template_needs_field(
						index_contains,
						m_prebuilt->read_just_key,
						fetch_all_in_key,
						fetch_primary_key_cols,
						index, table, i, num_v);
					if (!field) {
						if (is_v) {
							num_v++;
						}
						continue;
					}
				}

				ut_ad(!is_v);

				mysql_row_templ_t* templ= build_template_field(
					m_prebuilt, clust_index, index,
					table, field, i - num_v, 0);

				ut_ad(!templ->is_virtual);

				m_prebuilt->idx_cond_n_cols++;
				ut_ad(m_prebuilt->idx_cond_n_cols
				      == m_prebuilt->n_template);

				if (index == m_prebuilt->index) {
					templ->icp_rec_field_no
						= templ->rec_field_no;
				} else {
					templ->icp_rec_field_no
						= dict_index_get_nth_col_pos(
							m_prebuilt->index,
							i - num_v,
							&templ->rec_prefix_field_no);
				}

				if (dict_index_is_clust(m_prebuilt->index)) {
					ut_ad(templ->icp_rec_field_no
					      != ULINT_UNDEFINED);
					/* If the primary key includes
					a column prefix, use it in
					index condition pushdown,
					because the condition is
					evaluated before fetching any
					off-page (externally stored)
					columns. */
					if (templ->icp_rec_field_no
					    < m_prebuilt->index->n_uniq) {
						/* This is a key column;
						all set. */
						continue;
					}
				} else if (templ->icp_rec_field_no
					   != ULINT_UNDEFINED) {
					continue;
				}

				/* This is a column prefix index.
				The column prefix can be used in
				an end_range comparison. */

				templ->icp_rec_field_no
					= dict_index_get_nth_col_or_prefix_pos(
						m_prebuilt->index, i - num_v,
						true, false,
						&templ->rec_prefix_field_no);
				ut_ad(templ->icp_rec_field_no
				      != ULINT_UNDEFINED);

				/* Index condition pushdown can be used on
				all columns of a secondary index, and on
				the PRIMARY KEY columns. On the clustered
				index, it must never be used on other than
				PRIMARY KEY columns, because those columns
				may be stored off-page, and we will not
				fetch externally stored columns before
				checking the index condition. */
				/* TODO: test the above with an assertion
				like this. Note that index conditions are
				currently pushed down as part of the
				"optimizer phase" while end_range is done
				as part of the execution phase. Therefore,
				we were unable to use an accurate condition
				for end_range in the "if" condition above,
				and the following assertion would fail.
				ut_ad(!dict_index_is_clust(m_prebuilt->index)
				      || templ->rec_field_no
				      < m_prebuilt->index->n_uniq);
				*/
			}

			if (is_v) {
				num_v++;
			}
		}

		ut_ad(m_prebuilt->idx_cond_n_cols > 0);
		ut_ad(m_prebuilt->idx_cond_n_cols == m_prebuilt->n_template);

		num_v = 0;

		/* Include the fields that are not needed in index condition
		pushdown. */
		for (ulint i = 0; i < n_fields; i++) {
			const Field*		field = table->field[i];
			const bool is_v = !field->stored_in_db();
			if (is_v && skip_virtual) {
				num_v++;
				continue;
			}

			bool index_contains = index->contains_col_or_prefix(
				is_v ? num_v : i - num_v, is_v);

			if (!build_template_needs_field_in_icp(
				    index, m_prebuilt, index_contains,
				    is_v ? num_v : i - num_v, is_v)) {
				/* Not needed in ICP */
				if (!whole_row) {
					field = build_template_needs_field(
						index_contains,
						m_prebuilt->read_just_key,
						fetch_all_in_key,
						fetch_primary_key_cols,
						index, table, i, num_v);
					if (!field) {
						if (is_v) {
							num_v++;
						}
						continue;
					}
				}

				ut_d(mysql_row_templ_t*	templ =)
				build_template_field(
					m_prebuilt, clust_index, index,
					table, field, i - num_v, num_v);
				ut_ad(templ->is_virtual == (ulint)is_v);

				if (is_v) {
					num_v++;
				}
			}
		}
		if (active_index == pushed_idx_cond_keyno) {
			m_prebuilt->idx_cond = this;
		}
	} else {
no_icp:
		/* No index condition pushdown */
		m_prebuilt->idx_cond = NULL;
		ut_ad(num_v == 0);

		for (ulint i = 0; i < n_fields; i++) {
			const Field*	field = table->field[i];
			const bool is_v = !field->stored_in_db();

			if (whole_row) {
				if (is_v && skip_virtual) {
					num_v++;
					continue;
				}
				/* Even this is whole_row, if the seach is
				on a virtual column, and read_just_key is
				set, and field is not in this index, we
				will not try to fill the value since they
				are not stored in such index nor in the
				cluster index. */
				if (is_v
				    && m_prebuilt->read_just_key
				    && !m_prebuilt->index->contains_col_or_prefix(
					num_v, true))
				{
					/* Turn off ROW_MYSQL_WHOLE_ROW */
					m_prebuilt->template_type =
						 ROW_MYSQL_REC_FIELDS;
					num_v++;
					continue;
				}
			} else {
				if (is_v
				    && (skip_virtual || index->is_primary())) {
					num_v++;
					continue;
				}

				bool contain = index->contains_col_or_prefix(
					is_v ? num_v: i - num_v, is_v);

				field = build_template_needs_field(
					contain,
					m_prebuilt->read_just_key,
					fetch_all_in_key,
					fetch_primary_key_cols,
					index, table, i, num_v);
				if (!field) {
					if (is_v) {
						num_v++;
					}
					continue;
				}
			}

			ut_d(mysql_row_templ_t* templ =)
			build_template_field(
				m_prebuilt, clust_index, index,
				table, field, i - num_v, num_v);
			ut_ad(templ->is_virtual == (ulint)is_v);
			if (is_v) {
				num_v++;
			}
		}
	}

	if (index != clust_index && m_prebuilt->need_to_access_clustered) {
		/* Change rec_field_no's to correspond to the clustered index
		record */
		for (ulint i = 0; i < m_prebuilt->n_template; i++) {
			mysql_row_templ_t*	templ
				= &m_prebuilt->mysql_template[i];

			templ->rec_field_no = templ->clust_rec_field_no;
		}
	}
}

/********************************************************************//**
This special handling is really to overcome the limitations of MySQL's
binlogging. We need to eliminate the non-determinism that will arise in
INSERT ... SELECT type of statements, since MySQL binlog only stores the
min value of the autoinc interval. Once that is fixed we can get rid of
the special lock handling.
@return DB_SUCCESS if all OK else error code */

dberr_t
ha_innobase::innobase_lock_autoinc(void)
/*====================================*/
{
	DBUG_ENTER("ha_innobase::innobase_lock_autoinc");
	dberr_t		error = DB_SUCCESS;

	ut_ad(!srv_read_only_mode);

	switch (innobase_autoinc_lock_mode) {
	case AUTOINC_NO_LOCKING:
		/* Acquire only the AUTOINC mutex. */
		m_prebuilt->table->autoinc_mutex.lock();
		break;

	case AUTOINC_NEW_STYLE_LOCKING:
		/* For simple (single/multi) row INSERTs/REPLACEs and RBR
		events, we fallback to the old style only if another
		transaction has already acquired the AUTOINC lock on
		behalf of a LOAD FILE or INSERT ... SELECT etc. type of
		statement. */
		switch (thd_sql_command(m_user_thd)) {
		case SQLCOM_INSERT:
		case SQLCOM_REPLACE:
		case SQLCOM_END: // RBR event
			/* Acquire the AUTOINC mutex. */
			m_prebuilt->table->autoinc_mutex.lock();
			/* We need to check that another transaction isn't
			already holding the AUTOINC lock on the table. */
			if (!m_prebuilt->table->n_waiting_or_granted_auto_inc_locks) {
				/* Do not fall back to old style locking. */
				DBUG_RETURN(error);
			}
			m_prebuilt->table->autoinc_mutex.unlock();
		}
		/* Use old style locking. */
		/* fall through */
	case AUTOINC_OLD_STYLE_LOCKING:
		DBUG_EXECUTE_IF("die_if_autoinc_old_lock_style_used",
				ut_ad(0););
		error = row_lock_table_autoinc_for_mysql(m_prebuilt);

		if (error == DB_SUCCESS) {

			/* Acquire the AUTOINC mutex. */
			m_prebuilt->table->autoinc_mutex.lock();
		}
		break;

	default:
		ut_error;
	}

	DBUG_RETURN(error);
}

/********************************************************************//**
Store the autoinc value in the table. The autoinc value is only set if
it's greater than the existing autoinc value in the table.
@return DB_SUCCESS if all went well else error code */

dberr_t
ha_innobase::innobase_set_max_autoinc(
/*==================================*/
	ulonglong	auto_inc)	/*!< in: value to store */
{
	dberr_t		error;

	error = innobase_lock_autoinc();

	if (error == DB_SUCCESS) {

		dict_table_autoinc_update_if_greater(m_prebuilt->table, auto_inc);
		m_prebuilt->table->autoinc_mutex.unlock();
	}

	return(error);
}

/** @return whether the table is read-only */
bool ha_innobase::is_read_only() const
{
  ut_ad(m_prebuilt->trx == thd_to_trx(m_user_thd));

  if (high_level_read_only)
  {
    ib_senderrf(m_user_thd, IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
    return true;
  }

  if (!DICT_TF_GET_ZIP_SSIZE(m_prebuilt->table->flags) ||
      !innodb_read_only_compressed)
    return false;

  ib_senderrf(m_user_thd, IB_LOG_LEVEL_WARN, ER_UNSUPPORTED_COMPRESSED_TABLE);
  return true;
}

/********************************************************************//**
Stores a row in an InnoDB database, to the table specified in this
handle.
@return error code */

int
ha_innobase::write_row(
/*===================*/
	const uchar*	record)	/*!< in: a row in MySQL format */
{
	dberr_t		error;
#ifdef WITH_WSREP
	bool		wsrep_auto_inc_inserted= false;
#endif
	int		error_result = 0;
	bool		auto_inc_used = false;

	DBUG_ENTER("ha_innobase::write_row");

	trx_t*		trx = thd_to_trx(m_user_thd);

	/* Validation checks before we commence write_row operation. */
	if (is_read_only()) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	ins_mode_t	vers_set_fields;
	/* Handling of Auto-Increment Columns. */
	if (table->next_number_field && record == table->record[0]) {

		/* Reset the error code before calling
		innobase_get_auto_increment(). */
		m_prebuilt->autoinc_error = DB_SUCCESS;

#ifdef WITH_WSREP
		wsrep_auto_inc_inserted = trx->is_wsrep()
			&& wsrep_drupal_282555_workaround
			&& table->next_number_field->val_int() == 0;
#endif

		if ((error_result = update_auto_increment())) {
			/* We don't want to mask autoinc overflow errors. */

			/* Handle the case where the AUTOINC sub-system
			failed during initialization. */
			if (m_prebuilt->autoinc_error == DB_UNSUPPORTED) {
				error_result = ER_AUTOINC_READ_FAILED;
				/* Set the error message to report too. */
				my_error(ER_AUTOINC_READ_FAILED, MYF(0));
				goto func_exit;
			} else if (m_prebuilt->autoinc_error != DB_SUCCESS) {
				error = m_prebuilt->autoinc_error;
				goto report_error;
			}

			/* MySQL errors are passed straight back. */
			goto func_exit;
		}

		auto_inc_used = true;
	}

	/* Prepare INSERT graph that will be executed for actual INSERT
	(This is a one time operation) */
	if (m_prebuilt->mysql_template == NULL
	    || m_prebuilt->template_type != ROW_MYSQL_WHOLE_ROW) {

		/* Build the template used in converting quickly between
		the two database formats */

		build_template(true);
	}

	vers_set_fields = table->versioned_write(VERS_TRX_ID) ?
		ROW_INS_VERSIONED : ROW_INS_NORMAL;

	/* Execute insert graph that will result in actual insert. */
	error = row_insert_for_mysql((byte*) record, m_prebuilt, vers_set_fields);

	DEBUG_SYNC(m_user_thd, "ib_after_row_insert");

	/* Handling of errors related to auto-increment. */
	if (auto_inc_used) {
		ulonglong	auto_inc;
		ulonglong	col_max_value;

		/* Note the number of rows processed for this statement, used
		by get_auto_increment() to determine the number of AUTO-INC
		values to reserve. This is only useful for a mult-value INSERT
		and is a statement level counter. */
		if (trx->n_autoinc_rows > 0) {
			--trx->n_autoinc_rows;
		}

		/* We need the upper limit of the col type to check for
		whether we update the table autoinc counter or not. */
		col_max_value =
			table->next_number_field->get_max_int_value();

		/* Get the value that MySQL attempted to store in the table.*/
		auto_inc = table->next_number_field->val_uint();

		switch (error) {
		case DB_DUPLICATE_KEY:

			/* A REPLACE command and LOAD DATA INFILE REPLACE
			handle a duplicate key error themselves, but we
			must update the autoinc counter if we are performing
			those statements. */

			switch (thd_sql_command(m_user_thd)) {
			case SQLCOM_LOAD:
				if (!trx->duplicates) {
					break;
				}

			case SQLCOM_REPLACE:
			case SQLCOM_INSERT_SELECT:
			case SQLCOM_REPLACE_SELECT:
				goto set_max_autoinc;

#ifdef WITH_WSREP
			/* workaround for LP bug #355000, retrying the insert */
			case SQLCOM_INSERT:

				WSREP_DEBUG("DUPKEY error for autoinc\n"
				      "THD %ld, value %llu, off %llu inc %llu",
				      thd_get_thread_id(m_user_thd),
				      auto_inc,
				      m_prebuilt->autoinc_offset,
				      m_prebuilt->autoinc_increment);

                               if (wsrep_auto_inc_inserted &&
                                   wsrep_thd_retry_counter(m_user_thd) == 0  &&
				    !thd_test_options(m_user_thd,
						      OPTION_NOT_AUTOCOMMIT |
						      OPTION_BEGIN)) {
					WSREP_DEBUG(
					    "retrying insert: %s",
					    wsrep_thd_query(m_user_thd));
					error= DB_SUCCESS;
					wsrep_thd_self_abort(m_user_thd);
                                        /* jump straight to func exit over
                                         * later wsrep hooks */
                                        goto func_exit;
				}
                                break;
#endif /* WITH_WSREP */

			default:
				break;
			}

			break;

		case DB_SUCCESS:
			/* If the actual value inserted is greater than
			the upper limit of the interval, then we try and
			update the table upper limit. Note: last_value
			will be 0 if get_auto_increment() was not called. */

			if (auto_inc >= m_prebuilt->autoinc_last_value) {
set_max_autoinc:
				/* This should filter out the negative
				values set explicitly by the user. */
				if (auto_inc <= col_max_value) {

					ulonglong	offset;
					ulonglong	increment;
					dberr_t		err;
#ifdef WITH_WSREP
					/* Applier threads which are processing
					ROW events and don't go through server
					level autoinc processing, therefore
					m_prebuilt autoinc values don't get
					properly assigned. Fetch values from
					server side. */
					if (trx->is_wsrep() &&
					    wsrep_thd_is_applying(m_user_thd))
					{
					    wsrep_thd_auto_increment_variables(
						m_user_thd, &offset, &increment);
					}
					else
#endif /* WITH_WSREP */
					{
					    ut_a(m_prebuilt->autoinc_increment > 0);
					    offset = m_prebuilt->autoinc_offset;
					    increment = m_prebuilt->autoinc_increment;
					}
					auto_inc = innobase_next_autoinc(
						auto_inc,
						1, increment, offset,
						col_max_value);

					err = innobase_set_max_autoinc(
						auto_inc);

					if (err != DB_SUCCESS) {
						error = err;
					}
				}
			}
			break;
		default:
			break;
		}
	}

report_error:
	/* Cleanup and exit. */
	if (error == DB_TABLESPACE_DELETED) {
		ib_senderrf(
			trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);
	}

	error_result = convert_error_code_to_mysql(
		error, m_prebuilt->table->flags, m_user_thd);

#ifdef WITH_WSREP
	if (!error_result && trx->is_wsrep()
	    && wsrep_thd_is_local(m_user_thd)
	    && !wsrep_thd_ignore_table(m_user_thd)
	    && !wsrep_consistency_check(m_user_thd)
	    && (thd_sql_command(m_user_thd) != SQLCOM_CREATE_TABLE)
	    && (thd_sql_command(m_user_thd) != SQLCOM_LOAD ||
	        thd_binlog_format(m_user_thd) == BINLOG_FORMAT_ROW)) {
		if (wsrep_append_keys(m_user_thd, WSREP_SERVICE_KEY_EXCLUSIVE,
				      record,
				      NULL)) {
			DBUG_PRINT("wsrep", ("row key failed"));
			error_result = HA_ERR_INTERNAL_ERROR;
			goto func_exit;
		}
	}
#endif /* WITH_WSREP */

	if (error_result == HA_FTS_INVALID_DOCID) {
		my_error(HA_FTS_INVALID_DOCID, MYF(0));
	}

func_exit:
	DBUG_RETURN(error_result);
}

/** Fill the update vector's "old_vrow" field for those non-updated,
but indexed columns. Such columns could stil present in the virtual
index rec fields even if they are not updated (some other fields updated),
so needs to be logged.
@param[in]	prebuilt		InnoDB prebuilt struct
@param[in,out]	vfield			field to filled
@param[in]	o_len			actual column length
@param[in,out]	col			column to be filled
@param[in]	old_mysql_row_col	MySQL old field ptr
@param[in]	col_pack_len		MySQL field col length
@param[in,out]	buf			buffer for a converted integer value
@return used buffer ptr from row_mysql_store_col_in_innobase_format() */
static
byte*
innodb_fill_old_vcol_val(
	row_prebuilt_t*	prebuilt,
	dfield_t*	vfield,
	ulint		o_len,
	dict_col_t*	col,
	const byte*	old_mysql_row_col,
	ulint		col_pack_len,
	byte*		buf)
{
	dict_col_copy_type(
		col, dfield_get_type(vfield));
	if (o_len != UNIV_SQL_NULL) {

		buf = row_mysql_store_col_in_innobase_format(
			vfield,
			buf,
			TRUE,
			old_mysql_row_col,
			col_pack_len,
			dict_table_is_comp(prebuilt->table));
	} else {
		dfield_set_null(vfield);
	}

	return(buf);
}

/** Calculate an update vector corresponding to the changes
between old_row and new_row.
@param[out]	uvect		update vector
@param[in]	old_row		current row in MySQL format
@param[in]	new_row		intended updated row in MySQL format
@param[in]	table		MySQL table handle
@param[in,out]	upd_buff	buffer to use for converted values
@param[in]	buff_len	length of upd_buff
@param[in,out]	prebuilt	InnoDB execution context
@param[out]	auto_inc	updated AUTO_INCREMENT value, or 0 if none
@return DB_SUCCESS or error code */
static
dberr_t
calc_row_difference(
	upd_t*		uvect,
	const uchar*	old_row,
	const uchar*	new_row,
	TABLE*		table,
	uchar*		upd_buff,
	ulint		buff_len,
	row_prebuilt_t*	prebuilt,
	ib_uint64_t&	auto_inc)
{
	uchar*		original_upd_buff = upd_buff;
	Field*		field;
	enum_field_types field_mysql_type;
	ulint		o_len;
	ulint		n_len;
	ulint		col_pack_len;
	const byte*	new_mysql_row_col;
	const byte*	old_mysql_row_col;
	const byte*	o_ptr;
	const byte*	n_ptr;
	byte*		buf;
	upd_field_t*	ufield;
	ulint		col_type;
	ulint		n_changed = 0;
	dfield_t	dfield;
	dict_index_t*	clust_index;
	ibool		changes_fts_column = FALSE;
	ibool		changes_fts_doc_col = FALSE;
	trx_t* const	trx = prebuilt->trx;
	doc_id_t	doc_id = FTS_NULL_DOC_ID;
	uint16_t	num_v = 0;
	const bool skip_virtual = ha_innobase::omits_virtual_cols(*table->s);

	ut_ad(!srv_read_only_mode);

	clust_index = dict_table_get_first_index(prebuilt->table);
	auto_inc = 0;

	/* We use upd_buff to convert changed fields */
	buf = (byte*) upd_buff;

	for (uint i = 0; i < table->s->fields; i++) {
		field = table->field[i];
		const bool is_virtual = !field->stored_in_db();
		if (is_virtual && skip_virtual) {
			num_v++;
			continue;
		}
		dict_col_t* col = is_virtual
			? &prebuilt->table->v_cols[num_v].m_col
			: &prebuilt->table->cols[i - num_v];

		o_ptr = (const byte*) old_row + get_field_offset(table, field);
		n_ptr = (const byte*) new_row + get_field_offset(table, field);

		/* Use new_mysql_row_col and col_pack_len save the values */

		new_mysql_row_col = n_ptr;
		old_mysql_row_col = o_ptr;
		col_pack_len = field->pack_length();

		o_len = col_pack_len;
		n_len = col_pack_len;

		/* We use o_ptr and n_ptr to dig up the actual data for
		comparison. */

		field_mysql_type = field->type();

		col_type = col->mtype;

		switch (col_type) {

		case DATA_BLOB:
		case DATA_GEOMETRY:
			o_ptr = row_mysql_read_blob_ref(&o_len, o_ptr, o_len);
			n_ptr = row_mysql_read_blob_ref(&n_len, n_ptr, n_len);

			break;

		case DATA_VARCHAR:
		case DATA_BINARY:
		case DATA_VARMYSQL:
			if (field_mysql_type == MYSQL_TYPE_VARCHAR) {
				/* This is a >= 5.0.3 type true VARCHAR where
				the real payload data length is stored in
				1 or 2 bytes */

				o_ptr = row_mysql_read_true_varchar(
					&o_len, o_ptr,
					(ulint)
					(((Field_varstring*) field)->length_bytes));

				n_ptr = row_mysql_read_true_varchar(
					&n_len, n_ptr,
					(ulint)
					(((Field_varstring*) field)->length_bytes));
			}

			break;
		default:
			;
		}

		if (field_mysql_type == MYSQL_TYPE_LONGLONG
		    && prebuilt->table->fts
		    && innobase_strcasecmp(
			field->field_name.str, FTS_DOC_ID_COL_NAME) == 0) {
			doc_id = (doc_id_t) mach_read_from_n_little_endian(
				n_ptr, 8);
			if (doc_id == 0) {
				return(DB_FTS_INVALID_DOCID);
			}
		}

		if (field->real_maybe_null()) {
			if (field->is_null_in_record(old_row)) {
				o_len = UNIV_SQL_NULL;
			}

			if (field->is_null_in_record(new_row)) {
				n_len = UNIV_SQL_NULL;
			}
		}

#ifdef UNIV_DEBUG
		bool	online_ord_part = false;
#endif

		if (is_virtual) {
			/* If the virtual column is not indexed,
			we shall ignore it for update */
			if (!col->ord_part) {
				/* Check whether there is a table-rebuilding
				online ALTER TABLE in progress, and this
				virtual column could be newly indexed, thus
				it will be materialized. Then we will have
				to log its update.
				Note, we do not support online dropping virtual
				column while adding new index, nor with
				online alter column order while adding index,
				so the virtual column sequence must not change
				if it is online operation */
				if (dict_index_is_online_ddl(clust_index)
				    && row_log_col_is_indexed(clust_index,
							      num_v)) {
#ifdef UNIV_DEBUG
					online_ord_part = true;
#endif
				} else {
					num_v++;
					continue;
				}
			}

			if (!uvect->old_vrow) {
				uvect->old_vrow = dtuple_create_with_vcol(
					uvect->heap, 0, prebuilt->table->n_v_cols);
			}

			ulint   max_field_len = DICT_MAX_FIELD_LEN_BY_FORMAT(
						prebuilt->table);

			/* for virtual columns, we only materialize
			its index, and index field length would not
			exceed max_field_len. So continue if the
			first max_field_len bytes are matched up */
			if (o_len != UNIV_SQL_NULL
			   && n_len != UNIV_SQL_NULL
			   && o_len >= max_field_len
			   && n_len >= max_field_len
			   && memcmp(o_ptr, n_ptr, max_field_len) == 0) {
				dfield_t*	vfield = dtuple_get_nth_v_field(
					uvect->old_vrow, num_v);
				buf = innodb_fill_old_vcol_val(
					prebuilt, vfield, o_len,
					col, old_mysql_row_col,
					col_pack_len, buf);
			       num_v++;
			       continue;
			}
		}

		if (o_len != n_len || (o_len != 0 && o_len != UNIV_SQL_NULL
				       && 0 != memcmp(o_ptr, n_ptr, o_len))) {
			/* The field has changed */

			ufield = uvect->fields + n_changed;
			MEM_UNDEFINED(ufield, sizeof *ufield);

			/* Let us use a dummy dfield to make the conversion
			from the MySQL column format to the InnoDB format */


			/* If the length of new geometry object is 0, means
			this object is invalid geometry object, we need
			to block it. */
			if (DATA_GEOMETRY_MTYPE(col_type)
			    && o_len != 0 && n_len == 0) {
				return(DB_CANT_CREATE_GEOMETRY_OBJECT);
			}

			if (n_len != UNIV_SQL_NULL) {
				dict_col_copy_type(
					col, dfield_get_type(&dfield));

				buf = row_mysql_store_col_in_innobase_format(
					&dfield,
					(byte*) buf,
					TRUE,
					new_mysql_row_col,
					col_pack_len,
					dict_table_is_comp(prebuilt->table));
				dfield_copy(&ufield->new_val, &dfield);
			} else {
				dict_col_copy_type(
					col, dfield_get_type(&ufield->new_val));
				dfield_set_null(&ufield->new_val);
			}

			ufield->exp = NULL;
			ufield->orig_len = 0;
			if (is_virtual) {
				dfield_t*	vfield = dtuple_get_nth_v_field(
					uvect->old_vrow, num_v);
				upd_fld_set_virtual_col(ufield);
				ufield->field_no = num_v;

				ut_ad(col->ord_part || online_ord_part);
				ufield->old_v_val = static_cast<dfield_t*>(
					mem_heap_alloc(
						uvect->heap,
						sizeof *ufield->old_v_val));

				if (!field->is_null_in_record(old_row)) {
					if (n_len == UNIV_SQL_NULL) {
						dict_col_copy_type(
							col, dfield_get_type(
								&dfield));
					}

					buf = row_mysql_store_col_in_innobase_format(
						&dfield,
						(byte*) buf,
						TRUE,
						old_mysql_row_col,
						col_pack_len,
						dict_table_is_comp(
						prebuilt->table));
					dfield_copy(ufield->old_v_val,
						    &dfield);
					dfield_copy(vfield, &dfield);
				} else {
					dict_col_copy_type(
						col, dfield_get_type(
						ufield->old_v_val));
					dfield_set_null(ufield->old_v_val);
					dfield_set_null(vfield);
				}
				num_v++;
				ut_ad(field != table->found_next_number_field);
			} else {
				ufield->field_no = static_cast<uint16_t>(
					dict_col_get_clust_pos(
						&prebuilt->table->cols
						[i - num_v],
						clust_index));
				ufield->old_v_val = NULL;
				if (field != table->found_next_number_field
				    || dfield_is_null(&ufield->new_val)) {
				} else {
					auto_inc = field->val_uint();
				}
			}
			n_changed++;

			/* If an FTS indexed column was changed by this
			UPDATE then we need to inform the FTS sub-system.

			NOTE: Currently we re-index all FTS indexed columns
			even if only a subset of the FTS indexed columns
			have been updated. That is the reason we are
			checking only once here. Later we will need to
			note which columns have been updated and do
			selective processing. */
			if (prebuilt->table->fts != NULL && !is_virtual) {
				ulint		offset;
				dict_table_t*   innodb_table;

				innodb_table = prebuilt->table;

				if (!changes_fts_column) {
					offset = row_upd_changes_fts_column(
						innodb_table, ufield);

					if (offset != ULINT_UNDEFINED) {
						changes_fts_column = TRUE;
					}
				}

				if (!changes_fts_doc_col) {
					changes_fts_doc_col =
					row_upd_changes_doc_id(
						innodb_table, ufield);
				}
			}
		} else if (is_virtual) {
			dfield_t*	vfield = dtuple_get_nth_v_field(
				uvect->old_vrow, num_v);
			buf = innodb_fill_old_vcol_val(
				prebuilt, vfield, o_len,
				col, old_mysql_row_col,
				col_pack_len, buf);
			ut_ad(col->ord_part || online_ord_part);
			num_v++;
		}
	}

	/* If the update changes a column with an FTS index on it, we
	then add an update column node with a new document id to the
	other changes. We piggy back our changes on the normal UPDATE
	to reduce processing and IO overhead. */
	if (!prebuilt->table->fts) {
		trx->fts_next_doc_id = 0;
	} else if (changes_fts_column || changes_fts_doc_col) {
		dict_table_t*   innodb_table = prebuilt->table;

		ufield = uvect->fields + n_changed;

		if (!DICT_TF2_FLAG_IS_SET(
			innodb_table, DICT_TF2_FTS_HAS_DOC_ID)) {

			/* If Doc ID is managed by user, and if any
			FTS indexed column has been updated, its corresponding
			Doc ID must also be updated. Otherwise, return
			error */
			if (changes_fts_column && !changes_fts_doc_col) {
				ib::warn() << "A new Doc ID must be supplied"
					" while updating FTS indexed columns.";
				return(DB_FTS_INVALID_DOCID);
			}

			/* Doc ID must monotonically increase */
			ut_ad(innodb_table->fts->cache);
			if (doc_id < prebuilt->table->fts->cache->next_doc_id) {

				ib::warn() << "FTS Doc ID must be larger than "
					<< innodb_table->fts->cache->next_doc_id
					- 1  << " for table "
					<< innodb_table->name;

				return(DB_FTS_INVALID_DOCID);
			} else if ((doc_id
				    - prebuilt->table->fts->cache->next_doc_id)
				   >= FTS_DOC_ID_MAX_STEP) {

				ib::warn() << "Doc ID " << doc_id << " is too"
					" big. Its difference with largest"
					" Doc ID used " << prebuilt->table->fts
					->cache->next_doc_id - 1
					<< " cannot exceed or equal to "
					<< FTS_DOC_ID_MAX_STEP;
			}


			trx->fts_next_doc_id = doc_id;
		} else {
			/* If the Doc ID is a hidden column, it can't be
			changed by user */
			ut_ad(!changes_fts_doc_col);

			/* Doc ID column is hidden, a new Doc ID will be
			generated by following fts_update_doc_id() call */
			trx->fts_next_doc_id = 0;
		}

		fts_update_doc_id(
			innodb_table, ufield, &trx->fts_next_doc_id);

		++n_changed;
	} else {
		/* We have a Doc ID column, but none of FTS indexed
		columns are touched, nor the Doc ID column, so set
		fts_next_doc_id to UINT64_UNDEFINED, which means do not
		update the Doc ID column */
		trx->fts_next_doc_id = UINT64_UNDEFINED;
	}

	uvect->n_fields = n_changed;
	uvect->info_bits = 0;

	ut_a(buf <= (byte*) original_upd_buff + buff_len);

	ut_ad(uvect->validate());
	return(DB_SUCCESS);
}

#ifdef WITH_WSREP
static
int
wsrep_calc_row_hash(
/*================*/
	byte*		digest,		/*!< in/out: md5 sum */
	const uchar*	row,		/*!< in: row in MySQL format */
	TABLE*		table,		/*!< in: table in MySQL data
					dictionary */
	row_prebuilt_t*	prebuilt)	/*!< in: InnoDB prebuilt struct */
{
	ulint		len;
	const byte*	ptr;

	void *ctx = alloca(my_md5_context_size());
	my_md5_init(ctx);

	for (uint i = 0; i < table->s->fields; i++) {
		byte null_byte=0;
		byte true_byte=1;

		const Field* field = table->field[i];
		if (!field->stored_in_db()) {
			continue;
		}

		ptr = (const byte*) row + get_field_offset(table, field);
		len = field->pack_length();

		switch (prebuilt->table->cols[i].mtype) {

		case DATA_BLOB:
			ptr = row_mysql_read_blob_ref(&len, ptr, len);

			break;

		case DATA_VARCHAR:
		case DATA_BINARY:
		case DATA_VARMYSQL:
			if (field->type() == MYSQL_TYPE_VARCHAR) {
				/* This is a >= 5.0.3 type true VARCHAR where
				the real payload data length is stored in
				1 or 2 bytes */

				ptr = row_mysql_read_true_varchar(
					&len, ptr,
					(ulint)
					(((Field_varstring*)field)->length_bytes));

			}

			break;
		default:
			;
		}
		/*
		if (field->null_ptr &&
		    field_in_record_is_null(table, field, (char*) row)) {
		*/

		if (field->is_null_in_record(row)) {
			my_md5_input(ctx, &null_byte, 1);
		} else {
			my_md5_input(ctx, &true_byte, 1);
			my_md5_input(ctx, ptr, len);
		}
	}

	my_md5_result(ctx, digest);

	return(0);
}
#endif /* WITH_WSREP */

/**
Updates a row given as a parameter to a new value. Note that we are given
whole rows, not just the fields which are updated: this incurs some
overhead for CPU when we check which fields are actually updated.
TODO: currently InnoDB does not prevent the 'Halloween problem':
in a searched update a single row can get updated several times
if its index columns are updated!
@param[in] old_row	Old row contents in MySQL format
@param[out] new_row	Updated row contents in MySQL format
@return error number or 0 */

int
ha_innobase::update_row(
	const uchar*	old_row,
	const uchar*	new_row)
{
	int		err;

	dberr_t		error;
	trx_t*		trx = thd_to_trx(m_user_thd);

	DBUG_ENTER("ha_innobase::update_row");

	if (is_read_only()) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	} else if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	if (m_upd_buf == NULL) {
		ut_ad(m_upd_buf_size == 0);

		/* Create a buffer for packing the fields of a record. Why
		table->reclength did not work here? Obviously, because char
		fields when packed actually became 1 byte longer, when we also
		stored the string length as the first byte. */

		m_upd_buf_size = table->s->reclength + table->s->max_key_length
			+ MAX_REF_PARTS * 3;

		m_upd_buf = reinterpret_cast<uchar*>(
			my_malloc(PSI_INSTRUMENT_ME,
                                  m_upd_buf_size,
				MYF(MY_WME)));

		if (m_upd_buf == NULL) {
			m_upd_buf_size = 0;
			DBUG_RETURN(HA_ERR_OUT_OF_MEM);
		}
	}

	upd_t*		uvect = row_get_prebuilt_update_vector(m_prebuilt);
	ib_uint64_t	autoinc;

	/* Build an update vector from the modified fields in the rows
	(uses m_upd_buf of the handle) */

	error = calc_row_difference(
		uvect, old_row, new_row, table, m_upd_buf, m_upd_buf_size,
		m_prebuilt, autoinc);

	if (error != DB_SUCCESS) {
		goto func_exit;
	}

	if (!uvect->n_fields) {
		/* This is the same as success, but instructs
		MySQL that the row is not really updated and it
		should not increase the count of updated rows.
		This is fix for http://bugs.mysql.com/29157 */
		DBUG_RETURN(HA_ERR_RECORD_IS_THE_SAME);
	} else {
		const bool vers_set_fields = m_prebuilt->versioned_write
			&& m_prebuilt->upd_node->update->affects_versioned();
		const bool vers_ins_row = vers_set_fields
			&& thd_sql_command(m_user_thd) != SQLCOM_ALTER_TABLE;

		/* This is not a delete */
		m_prebuilt->upd_node->is_delete =
			(vers_set_fields && !vers_ins_row) ||
			(thd_sql_command(m_user_thd) == SQLCOM_DELETE &&
				table->versioned(VERS_TIMESTAMP))
			? VERSIONED_DELETE
			: NO_DELETE;

		error = row_update_for_mysql(m_prebuilt);

		if (error == DB_SUCCESS && vers_ins_row
		    /* Multiple UPDATE of same rows in single transaction create
		       historical rows only once. */
		    && trx->id != table->vers_start_id()) {
			error = row_insert_for_mysql((byte*) old_row,
						     m_prebuilt,
						     ROW_INS_HISTORICAL);
		}
	}

	if (error == DB_SUCCESS && autoinc) {
		/* A value for an AUTO_INCREMENT column
		was specified in the UPDATE statement. */

		ulonglong	offset;
		ulonglong	increment;
#ifdef WITH_WSREP
		/* Applier threads which are processing
		ROW events and don't go through server
		level autoinc processing, therefore
		m_prebuilt autoinc values don't get
		properly assigned. Fetch values from
		server side. */
		if (trx->is_wsrep() && wsrep_thd_is_applying(m_user_thd))
			wsrep_thd_auto_increment_variables(
				m_user_thd, &offset, &increment);
		else
#endif /* WITH_WSREP */
			offset = m_prebuilt->autoinc_offset,
				increment = m_prebuilt->autoinc_increment;

		autoinc = innobase_next_autoinc(
			autoinc, 1, increment, offset,
			table->found_next_number_field->get_max_int_value());

		error = innobase_set_max_autoinc(autoinc);

		if (m_prebuilt->table->persistent_autoinc) {
			/* Update the PAGE_ROOT_AUTO_INC. Yes, we do
			this even if dict_table_t::autoinc already was
			greater than autoinc, because we cannot know
			if any INSERT actually used (and wrote to
			PAGE_ROOT_AUTO_INC) a value bigger than our
			autoinc. */
			btr_write_autoinc(dict_table_get_first_index(
						  m_prebuilt->table),
					  autoinc);
		}
	}

func_exit:
	if (error == DB_FTS_INVALID_DOCID) {
		err = HA_FTS_INVALID_DOCID;
		my_error(HA_FTS_INVALID_DOCID, MYF(0));
	} else {
		err = convert_error_code_to_mysql(
			error, m_prebuilt->table->flags, m_user_thd);
	}

#ifdef WITH_WSREP
	if (error == DB_SUCCESS && trx->is_wsrep()
	    && wsrep_thd_is_local(m_user_thd)
	    && !wsrep_thd_ignore_table(m_user_thd)) {
		DBUG_PRINT("wsrep", ("update row key"));

		if (wsrep_append_keys(m_user_thd,
				      wsrep_protocol_version >= 4
				      ? WSREP_SERVICE_KEY_UPDATE
				      : WSREP_SERVICE_KEY_EXCLUSIVE,
				      old_row, new_row)){
			WSREP_DEBUG("WSREP: UPDATE_ROW_KEY FAILED");
			DBUG_PRINT("wsrep", ("row key failed"));
			DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
		}
	}
#endif /* WITH_WSREP */

	DBUG_RETURN(err);
}

/**********************************************************************//**
Deletes a row given as the parameter.
@return error number or 0 */

int
ha_innobase::delete_row(
/*====================*/
	const uchar*	record)	/*!< in: a row in MySQL format */
{
	dberr_t		error;
	trx_t*		trx = thd_to_trx(m_user_thd);

	DBUG_ENTER("ha_innobase::delete_row");

	if (is_read_only()) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	} else if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	if (!m_prebuilt->upd_node) {
		row_get_prebuilt_update_vector(m_prebuilt);
	}

	/* This is a delete */
	m_prebuilt->upd_node->is_delete = table->versioned_write(VERS_TRX_ID)
		&& table->vers_end_field()->is_max()
		&& trx->id != table->vers_start_id()
		? VERSIONED_DELETE
		: PLAIN_DELETE;

	error = row_update_for_mysql(m_prebuilt);

#ifdef WITH_WSREP
	if (error == DB_SUCCESS && trx->is_wsrep()
	    && wsrep_thd_is_local(m_user_thd)
	    && !wsrep_thd_ignore_table(m_user_thd)) {
		if (wsrep_append_keys(m_user_thd, WSREP_SERVICE_KEY_EXCLUSIVE,
				      record,
				      NULL)) {
			DBUG_PRINT("wsrep", ("delete fail"));
			DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
		}
	}
#endif /* WITH_WSREP */
	DBUG_RETURN(convert_error_code_to_mysql(
			    error, m_prebuilt->table->flags, m_user_thd));
}

/** Delete all rows from the table.
@return error number or 0 */

int
ha_innobase::delete_all_rows()
{
	DBUG_ENTER("ha_innobase::delete_all_rows");
	DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

/**********************************************************************//**
Removes a new lock set on a row, if it was not read optimistically. This can
be called after a row has been read in the processing of an UPDATE or a DELETE
query. */

void
ha_innobase::unlock_row(void)
/*=========================*/
{
	DBUG_ENTER("ha_innobase::unlock_row");

	if (m_prebuilt->select_lock_type == LOCK_NONE) {
		DBUG_VOID_RETURN;
	}

	ut_ad(trx_state_eq(m_prebuilt->trx, TRX_STATE_ACTIVE, true));

	switch (m_prebuilt->row_read_type) {
	case ROW_READ_WITH_LOCKS:
		if (m_prebuilt->trx->isolation_level > TRX_ISO_READ_COMMITTED)
			break;
		/* fall through */
	case ROW_READ_TRY_SEMI_CONSISTENT:
		row_unlock_for_mysql(m_prebuilt, FALSE);
		break;
	case ROW_READ_DID_SEMI_CONSISTENT:
		m_prebuilt->row_read_type = ROW_READ_TRY_SEMI_CONSISTENT;
		break;
	}

	DBUG_VOID_RETURN;
}

/* See handler.h and row0mysql.h for docs on this function. */

bool
ha_innobase::was_semi_consistent_read(void)
/*=======================================*/
{
	return(m_prebuilt->row_read_type == ROW_READ_DID_SEMI_CONSISTENT);
}

/* See handler.h and row0mysql.h for docs on this function. */
void ha_innobase::try_semi_consistent_read(bool yes)
{
	ut_ad(m_prebuilt->trx == thd_to_trx(ha_thd()));
	/* Row read type is set to semi consistent read if this was
	requested by the SQL layer and the transaction isolation level is
	READ UNCOMMITTED or READ COMMITTED. */
	m_prebuilt->row_read_type = yes
		&& m_prebuilt->trx->isolation_level <= TRX_ISO_READ_COMMITTED
		? ROW_READ_TRY_SEMI_CONSISTENT
		: ROW_READ_WITH_LOCKS;
}

/******************************************************************//**
Initializes a handle to use an index.
@return 0 or error number */

int
ha_innobase::index_init(
/*====================*/
	uint		keynr,	/*!< in: key (index) number */
	bool)
{
	DBUG_ENTER("index_init");

	DBUG_RETURN(change_active_index(keynr));
}

/******************************************************************//**
Currently does nothing.
@return 0 */

int
ha_innobase::index_end(void)
/*========================*/
{
	DBUG_ENTER("index_end");

	active_index = MAX_KEY;

	in_range_check_pushed_down = FALSE;

	m_ds_mrr.dsmrr_close();

	DBUG_RETURN(0);
}

/*********************************************************************//**
Converts a search mode flag understood by MySQL to a flag understood
by InnoDB. */
page_cur_mode_t
convert_search_mode_to_innobase(
/*============================*/
	ha_rkey_function	find_flag)
{
	switch (find_flag) {
	case HA_READ_KEY_EXACT:
		/* this does not require the index to be UNIQUE */
	case HA_READ_KEY_OR_NEXT:
		return(PAGE_CUR_GE);
	case HA_READ_AFTER_KEY:
		return(PAGE_CUR_G);
	case HA_READ_BEFORE_KEY:
		return(PAGE_CUR_L);
	case HA_READ_KEY_OR_PREV:
	case HA_READ_PREFIX_LAST:
	case HA_READ_PREFIX_LAST_OR_PREV:
		return(PAGE_CUR_LE);
	case HA_READ_MBR_CONTAIN:
		return(PAGE_CUR_CONTAIN);
	case HA_READ_MBR_INTERSECT:
		return(PAGE_CUR_INTERSECT);
	case HA_READ_MBR_WITHIN:
		return(PAGE_CUR_WITHIN);
	case HA_READ_MBR_DISJOINT:
		return(PAGE_CUR_DISJOINT);
	case HA_READ_MBR_EQUAL:
		return(PAGE_CUR_MBR_EQUAL);
	case HA_READ_PREFIX:
		return(PAGE_CUR_UNSUPP);
	/* do not use "default:" in order to produce a gcc warning:
	enumeration value '...' not handled in switch
	(if -Wswitch or -Wall is used) */
	}

	my_error(ER_CHECK_NOT_IMPLEMENTED, MYF(0), "this functionality");

	return(PAGE_CUR_UNSUPP);
}

/*
   BACKGROUND INFO: HOW A SELECT SQL QUERY IS EXECUTED
   ---------------------------------------------------
The following does not cover all the details, but explains how we determine
the start of a new SQL statement, and what is associated with it.

For each table in the database the MySQL interpreter may have several
table handle instances in use, also in a single SQL query. For each table
handle instance there is an InnoDB  'm_prebuilt' struct which contains most
of the InnoDB data associated with this table handle instance.

  A) if the user has not explicitly set any MySQL table level locks:

  1) MySQL calls ::external_lock to set an 'intention' table level lock on
the table of the handle instance. There we set
m_prebuilt->sql_stat_start = TRUE. The flag sql_stat_start should be set
true if we are taking this table handle instance to use in a new SQL
statement issued by the user. We also increment trx->n_mysql_tables_in_use.

  2) If m_prebuilt->sql_stat_start == TRUE we 'pre-compile' the MySQL search
instructions to m_prebuilt->template of the table handle instance in
::index_read. The template is used to save CPU time in large joins.

  3) In row_search_for_mysql, if m_prebuilt->sql_stat_start is true, we
allocate a new consistent read view for the trx if it does not yet have one,
or in the case of a locking read, set an InnoDB 'intention' table level
lock on the table.

  4) We do the SELECT. MySQL may repeatedly call ::index_read for the
same table handle instance, if it is a join.

  5) When the SELECT ends, MySQL removes its intention table level locks
in ::external_lock. When trx->n_mysql_tables_in_use drops to zero,
 (a) we execute a COMMIT there if the autocommit is on,
 (b) we also release possible 'SQL statement level resources' InnoDB may
have for this SQL statement. The MySQL interpreter does NOT execute
autocommit for pure read transactions, though it should. That is why the
table handler in that case has to execute the COMMIT in ::external_lock.

  B) If the user has explicitly set MySQL table level locks, then MySQL
does NOT call ::external_lock at the start of the statement. To determine
when we are at the start of a new SQL statement we at the start of
::index_read also compare the query id to the latest query id where the
table handle instance was used. If it has changed, we know we are at the
start of a new SQL statement. Since the query id can theoretically
overwrap, we use this test only as a secondary way of determining the
start of a new SQL statement. */


/**********************************************************************//**
Positions an index cursor to the index specified in the handle. Fetches the
row if any.
@return 0, HA_ERR_KEY_NOT_FOUND, or error number */

int
ha_innobase::index_read(
/*====================*/
	uchar*		buf,		/*!< in/out: buffer for the returned
					row */
	const uchar*	key_ptr,	/*!< in: key value; if this is NULL
					we position the cursor at the
					start or end of index; this can
					also contain an InnoDB row id, in
					which case key_len is the InnoDB
					row id length; the key value can
					also be a prefix of a full key value,
					and the last column can be a prefix
					of a full column */
	uint			key_len,/*!< in: key value length */
	enum ha_rkey_function find_flag)/*!< in: search flags from my_base.h */
{
	DBUG_ENTER("index_read");
	DEBUG_SYNC_C("ha_innobase_index_read_begin");

	ut_a(m_prebuilt->trx == thd_to_trx(m_user_thd));
	ut_ad(key_len != 0 || find_flag != HA_READ_KEY_EXACT);

	dict_index_t*	index = m_prebuilt->index;

	if (index == NULL || index->is_corrupted()) {
		m_prebuilt->index_usable = FALSE;
		DBUG_RETURN(HA_ERR_CRASHED);
	}

	if (!m_prebuilt->index_usable) {
		DBUG_RETURN(index->is_corrupted()
			    ? HA_ERR_INDEX_CORRUPT
			    : HA_ERR_TABLE_DEF_CHANGED);
	}

	if (index->type & DICT_FTS) {
		DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
	}

	/* For R-Tree index, we will always place the page lock to
	pages being searched */
	if (dict_index_is_spatial(index)) {
		++m_prebuilt->trx->will_lock;
	}

	/* Note that if the index for which the search template is built is not
	necessarily m_prebuilt->index, but can also be the clustered index */

	if (m_prebuilt->sql_stat_start) {
		build_template(false);
	}

	if (key_ptr != NULL) {
		/* Convert the search key value to InnoDB format into
		m_prebuilt->search_tuple */

		row_sel_convert_mysql_key_to_innobase(
			m_prebuilt->search_tuple,
			m_prebuilt->srch_key_val1,
			m_prebuilt->srch_key_val_len,
			index,
			(byte*) key_ptr,
			(ulint) key_len);

		DBUG_ASSERT(m_prebuilt->search_tuple->n_fields > 0);
	} else {
		/* We position the cursor to the last or the first entry
		in the index */

		dtuple_set_n_fields(m_prebuilt->search_tuple, 0);
	}

	page_cur_mode_t	mode = convert_search_mode_to_innobase(find_flag);

	ulint	match_mode = 0;

	if (find_flag == HA_READ_KEY_EXACT) {

		match_mode = ROW_SEL_EXACT;

	} else if (find_flag == HA_READ_PREFIX_LAST) {

		match_mode = ROW_SEL_EXACT_PREFIX;
	}

	m_last_match_mode = (uint) match_mode;

	dberr_t ret = mode == PAGE_CUR_UNSUPP ? DB_UNSUPPORTED
		: row_search_mvcc(buf, mode, m_prebuilt, match_mode, 0);

	DBUG_EXECUTE_IF("ib_select_query_failure", ret = DB_ERROR;);

	int	error;

	switch (ret) {
	case DB_SUCCESS:
		error = 0;
		table->status = 0;
		if (m_prebuilt->table->is_system_db) {
			srv_stats.n_system_rows_read.add(
				thd_get_thread_id(m_prebuilt->trx->mysql_thd), 1);
		} else {
			srv_stats.n_rows_read.add(
				thd_get_thread_id(m_prebuilt->trx->mysql_thd), 1);
		}
		break;

	case DB_RECORD_NOT_FOUND:
		error = HA_ERR_KEY_NOT_FOUND;
		table->status = STATUS_NOT_FOUND;
		break;

	case DB_END_OF_INDEX:
		error = HA_ERR_KEY_NOT_FOUND;
		table->status = STATUS_NOT_FOUND;
		break;

	case DB_TABLESPACE_DELETED:
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_TABLESPACE_MISSING;
		break;

	case DB_TABLESPACE_NOT_FOUND:

		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_TABLESPACE_MISSING;
		break;

	default:
		error = convert_error_code_to_mysql(
			ret, m_prebuilt->table->flags, m_user_thd);

		table->status = STATUS_NOT_FOUND;
		break;
	}

	DBUG_RETURN(error);
}

/*******************************************************************//**
The following functions works like index_read, but it find the last
row with the current key value or prefix.
@return 0, HA_ERR_KEY_NOT_FOUND, or an error code */

int
ha_innobase::index_read_last(
/*=========================*/
	uchar*		buf,	/*!< out: fetched row */
	const uchar*	key_ptr,/*!< in: key value, or a prefix of a full
				key value */
	uint		key_len)/*!< in: length of the key val or prefix
				in bytes */
{
	return(index_read(buf, key_ptr, key_len, HA_READ_PREFIX_LAST));
}

/********************************************************************//**
Get the index for a handle. Does not change active index.
@return NULL or index instance. */

dict_index_t*
ha_innobase::innobase_get_index(
/*============================*/
	uint		keynr)	/*!< in: use this index; MAX_KEY means always
				clustered index, even if it was internally
				generated by InnoDB */
{
	KEY*		key = NULL;
	dict_table_t*	ib_table = m_prebuilt->table;
	dict_index_t*	index;

	DBUG_ENTER("innobase_get_index");

	if (keynr != MAX_KEY && table->s->keys > 0) {
		key = &table->key_info[keynr];
		index = dict_table_get_index_on_name(ib_table, key->name.str);
	} else {
		index = dict_table_get_first_index(ib_table);
	}

	if (index == NULL) {
		sql_print_error(
			"InnoDB could not find key no %u with name %s"
			" from dict cache for table %s",
			keynr, key ? key->name.str : "NULL",
			ib_table->name.m_name);
	}

	DBUG_RETURN(index);
}

/********************************************************************//**
Changes the active index of a handle.
@return 0 or error code */

int
ha_innobase::change_active_index(
/*=============================*/
	uint	keynr)	/*!< in: use this index; MAX_KEY means always clustered
			index, even if it was internally generated by
			InnoDB */
{
	DBUG_ENTER("change_active_index");

	ut_ad(m_user_thd == ha_thd());
	ut_a(m_prebuilt->trx == thd_to_trx(m_user_thd));

	active_index = keynr;

	m_prebuilt->index = innobase_get_index(keynr);

	if (m_prebuilt->index == NULL) {
		sql_print_warning("InnoDB: change_active_index(%u) failed",
				  keynr);
		m_prebuilt->index_usable = FALSE;
		DBUG_RETURN(1);
	}

	m_prebuilt->index_usable = row_merge_is_index_usable(
		m_prebuilt->trx, m_prebuilt->index);

	if (!m_prebuilt->index_usable) {
		if (m_prebuilt->index->is_corrupted()) {
			char	table_name[MAX_FULL_NAME_LEN + 1];

			innobase_format_name(
				table_name, sizeof table_name,
				m_prebuilt->index->table->name.m_name);

			if (m_prebuilt->index->is_primary()) {
				ut_ad(m_prebuilt->index->table->corrupted);
				push_warning_printf(
					m_user_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_TABLE_CORRUPT,
					"InnoDB: Table %s is corrupted.",
					table_name);
				DBUG_RETURN(ER_TABLE_CORRUPT);
			} else {
				push_warning_printf(
					m_user_thd, Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_INDEX_CORRUPT,
					"InnoDB: Index %s for table %s is"
					" marked as corrupted",
					m_prebuilt->index->name(),
					table_name);
				DBUG_RETURN(HA_ERR_INDEX_CORRUPT);
			}
		} else {
			push_warning_printf(
				m_user_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_ERR_TABLE_DEF_CHANGED,
				"InnoDB: insufficient history for index %u",
				keynr);
		}

		/* The caller seems to ignore this.  Thus, we must check
		this again in row_search_for_mysql(). */
		DBUG_RETURN(convert_error_code_to_mysql(DB_MISSING_HISTORY,
				0, NULL));
	}

	ut_a(m_prebuilt->search_tuple != 0);

	/* Initialization of search_tuple is not needed for FT index
	since FT search returns rank only. In addition engine should
	be able to retrieve FTS_DOC_ID column value if necessary. */
	if (m_prebuilt->index->type & DICT_FTS) {
		for (uint i = 0; i < table->s->fields; i++) {
			if (m_prebuilt->read_just_key
			    && bitmap_is_set(table->read_set, i)
			    && !strcmp(table->s->field[i]->field_name.str,
				       FTS_DOC_ID_COL_NAME)) {
				m_prebuilt->fts_doc_id_in_read_set = true;
				break;
			}
		}
	} else {
		ulint n_fields = dict_index_get_n_unique_in_tree(
			m_prebuilt->index);

		dtuple_set_n_fields(m_prebuilt->search_tuple, n_fields);

		dict_index_copy_types(
			m_prebuilt->search_tuple, m_prebuilt->index,
			n_fields);

		/* If it's FTS query and FTS_DOC_ID exists FTS_DOC_ID field is
		always added to read_set. */
		m_prebuilt->fts_doc_id_in_read_set = m_prebuilt->in_fts_query
			&& m_prebuilt->read_just_key
			&& m_prebuilt->index->contains_col_or_prefix(
				m_prebuilt->table->fts->doc_col, false);
	}

	/* MySQL changes the active index for a handle also during some
	queries, for example SELECT MAX(a), SUM(a) first retrieves the MAX()
	and then calculates the sum. Previously we played safe and used
	the flag ROW_MYSQL_WHOLE_ROW below, but that caused unnecessary
	copying. Starting from MySQL-4.1 we use a more efficient flag here. */

	build_template(false);

	DBUG_RETURN(0);
}

/***********************************************************************//**
Reads the next or previous row from a cursor, which must have previously been
positioned using index_read.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::general_fetch(
/*=======================*/
	uchar*	buf,		/*!< in/out: buffer for next row in MySQL
				format */
	uint	direction,	/*!< in: ROW_SEL_NEXT or ROW_SEL_PREV */
	uint	match_mode)	/*!< in: 0, ROW_SEL_EXACT, or
				ROW_SEL_EXACT_PREFIX */
{
	DBUG_ENTER("general_fetch");

	const trx_t*	trx = m_prebuilt->trx;

	ut_ad(trx == thd_to_trx(m_user_thd));

	if (m_prebuilt->table->is_readable()) {
	} else if (m_prebuilt->table->corrupted) {
		DBUG_RETURN(HA_ERR_CRASHED);
	} else {
		DBUG_RETURN(m_prebuilt->table->space
			    ? HA_ERR_DECRYPTION_FAILED
			    : HA_ERR_NO_SUCH_TABLE);
	}

	int	error;

	switch (dberr_t	ret = row_search_mvcc(buf, PAGE_CUR_UNSUPP, m_prebuilt,
					      match_mode, direction)) {
	case DB_SUCCESS:
		error = 0;
		table->status = 0;
		if (m_prebuilt->table->is_system_db) {
			srv_stats.n_system_rows_read.add(
				thd_get_thread_id(trx->mysql_thd), 1);
		} else {
			srv_stats.n_rows_read.add(
				thd_get_thread_id(trx->mysql_thd), 1);
		}
		break;
	case DB_RECORD_NOT_FOUND:
		error = HA_ERR_END_OF_FILE;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_END_OF_INDEX:
		error = HA_ERR_END_OF_FILE;
		table->status = STATUS_NOT_FOUND;
		break;
	case DB_TABLESPACE_DELETED:
		ib_senderrf(
			trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_TABLESPACE_MISSING;
		break;
	case DB_TABLESPACE_NOT_FOUND:

		ib_senderrf(
			trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);

		table->status = STATUS_NOT_FOUND;
		error = HA_ERR_TABLESPACE_MISSING;
		break;
	default:
		error = convert_error_code_to_mysql(
			ret, m_prebuilt->table->flags, m_user_thd);

		table->status = STATUS_NOT_FOUND;
		break;
	}

	DBUG_RETURN(error);
}

/***********************************************************************//**
Reads the next row from a cursor, which must have previously been
positioned using index_read.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::index_next(
/*====================*/
	uchar*		buf)	/*!< in/out: buffer for next row in MySQL
				format */
{
	return(general_fetch(buf, ROW_SEL_NEXT, 0));
}

/*******************************************************************//**
Reads the next row matching to the key value given as the parameter.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::index_next_same(
/*=========================*/
	uchar*		buf,	/*!< in/out: buffer for the row */
	const uchar*, uint)
{
	return(general_fetch(buf, ROW_SEL_NEXT, m_last_match_mode));
}

/***********************************************************************//**
Reads the previous row from a cursor, which must have previously been
positioned using index_read.
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::index_prev(
/*====================*/
	uchar*	buf)	/*!< in/out: buffer for previous row in MySQL format */
{
	return(general_fetch(buf, ROW_SEL_PREV, 0));
}

/********************************************************************//**
Positions a cursor on the first record in an index and reads the
corresponding row to buf.
@return 0, HA_ERR_END_OF_FILE, or error code */

int
ha_innobase::index_first(
/*=====================*/
	uchar*	buf)	/*!< in/out: buffer for the row */
{
	DBUG_ENTER("index_first");

	int	error = index_read(buf, NULL, 0, HA_READ_AFTER_KEY);

	/* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */

	if (error == HA_ERR_KEY_NOT_FOUND) {
		error = HA_ERR_END_OF_FILE;
	}

	DBUG_RETURN(error);
}

/********************************************************************//**
Positions a cursor on the last record in an index and reads the
corresponding row to buf.
@return 0, HA_ERR_END_OF_FILE, or error code */

int
ha_innobase::index_last(
/*====================*/
	uchar*	buf)	/*!< in/out: buffer for the row */
{
	DBUG_ENTER("index_last");

	int	error = index_read(buf, NULL, 0, HA_READ_BEFORE_KEY);

	/* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */

	if (error == HA_ERR_KEY_NOT_FOUND) {
		error = HA_ERR_END_OF_FILE;
	}

	DBUG_RETURN(error);
}

/****************************************************************//**
Initialize a table scan.
@return 0 or error number */

int
ha_innobase::rnd_init(
/*==================*/
	bool	scan)	/*!< in: true if table/index scan FALSE otherwise */
{
	int		err;

	/* Store the active index value so that we can restore the original
	value after a scan */

	if (m_prebuilt->clust_index_was_generated) {
		err = change_active_index(MAX_KEY);
	} else {
		err = change_active_index(m_primary_key);
	}

	/* Don't use semi-consistent read in random row reads (by position).
	This means we must disable semi_consistent_read if scan is false */

	if (!scan) {
		try_semi_consistent_read(0);
	}

	m_start_of_scan = true;

	return(err);
}

/*****************************************************************//**
Ends a table scan.
@return 0 or error number */

int
ha_innobase::rnd_end(void)
/*======================*/
{
	return(index_end());
}

/*****************************************************************//**
Reads the next row in a table scan (also used to read the FIRST row
in a table scan).
@return 0, HA_ERR_END_OF_FILE, or error number */

int
ha_innobase::rnd_next(
/*==================*/
	uchar*	buf)	/*!< in/out: returns the row in this buffer,
			in MySQL format */
{
	int	error;

	DBUG_ENTER("rnd_next");

	if (m_start_of_scan) {
		error = index_first(buf);

		if (error == HA_ERR_KEY_NOT_FOUND) {
			error = HA_ERR_END_OF_FILE;
		}

		m_start_of_scan = false;
	} else {
		error = general_fetch(buf, ROW_SEL_NEXT, 0);
	}

	DBUG_RETURN(error);
}

/**********************************************************************//**
Fetches a row from the table based on a row reference.
@return 0, HA_ERR_KEY_NOT_FOUND, or error code */

int
ha_innobase::rnd_pos(
/*=================*/
	uchar*	buf,	/*!< in/out: buffer for the row */
	uchar*	pos)	/*!< in: primary key value of the row in the
			MySQL format, or the row id if the clustered
			index was internally generated by InnoDB; the
			length of data in pos has to be ref_length */
{
	DBUG_ENTER("rnd_pos");
	DBUG_DUMP("key", pos, ref_length);

	ut_a(m_prebuilt->trx == thd_to_trx(ha_thd()));

	/* Note that we assume the length of the row reference is fixed
	for the table, and it is == ref_length */

	int	error = index_read(buf, pos, (uint)ref_length, HA_READ_KEY_EXACT);

	if (error != 0) {
		DBUG_PRINT("error", ("Got error: %d", error));
	}

	DBUG_RETURN(error);
}

/**********************************************************************//**
Initialize FT index scan
@return 0 or error number */

int
ha_innobase::ft_init()
/*==================*/
{
	DBUG_ENTER("ft_init");

	trx_t*	trx = check_trx_exists(ha_thd());

	/* FTS queries are not treated as autocommit non-locking selects.
	This is because the FTS implementation can acquire locks behind
	the scenes. This has not been verified but it is safer to treat
	them as regular read only transactions for now. */

	if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	DBUG_RETURN(rnd_init(false));
}

/**********************************************************************//**
Initialize FT index scan
@return FT_INFO structure if successful or NULL */

FT_INFO*
ha_innobase::ft_init_ext(
/*=====================*/
	uint			flags,	/* in: */
	uint			keynr,	/* in: */
	String*			key)	/* in: */
{
	NEW_FT_INFO*		fts_hdl = NULL;
	dict_index_t*		index;
	fts_result_t*		result;
	char			buf_tmp[8192];
	ulint			buf_tmp_used;
	uint			num_errors;
	ulint			query_len = key->length();
	const CHARSET_INFO*	char_set = key->charset();
	const char*		query = key->ptr();

	if (UNIV_UNLIKELY(fts_enable_diag_print)) {
		{
			ib::info	out;
			out << "keynr=" << keynr << ", '";
			out.write(key->ptr(), key->length());
		}

		if (flags & FT_BOOL) {
			ib::info() << "BOOL search";
		} else {
			ib::info() << "NL search";
		}
	}

	/* FIXME: utf32 and utf16 are not compatible with some
	string function used. So to convert them to uft8 before
	we proceed. */
	if (strcmp(char_set->csname, "utf32") == 0
	    || strcmp(char_set->csname, "utf16") == 0) {

		buf_tmp_used = innobase_convert_string(
			buf_tmp, sizeof(buf_tmp) - 1,
			&my_charset_utf8mb3_general_ci,
			query, query_len, (CHARSET_INFO*) char_set,
			&num_errors);

		buf_tmp[buf_tmp_used] = 0;
		query = buf_tmp;
		query_len = buf_tmp_used;
	}

	trx_t*	trx = m_prebuilt->trx;

	/* FTS queries are not treated as autocommit non-locking selects.
	This is because the FTS implementation can acquire locks behind
	the scenes. This has not been verified but it is safer to treat
	them as regular read only transactions for now. */

	if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	dict_table_t*	ft_table = m_prebuilt->table;

	/* Table does not have an FTS index */
	if (!ft_table->fts || ib_vector_is_empty(ft_table->fts->indexes)) {
		my_error(ER_TABLE_HAS_NO_FT, MYF(0));
		return(NULL);
	}

	/* If tablespace is discarded, we should return here */
	if (!ft_table->space) {
		my_error(ER_TABLESPACE_MISSING, MYF(0), table->s->db.str,
			 table->s->table_name.str);
		return(NULL);
	}

	if (keynr == NO_SUCH_KEY) {
		/* FIXME: Investigate the NO_SUCH_KEY usage */
		index = reinterpret_cast<dict_index_t*>
			(ib_vector_getp(ft_table->fts->indexes, 0));
	} else {
		index = innobase_get_index(keynr);
	}

	if (index == NULL || index->type != DICT_FTS) {
		my_error(ER_TABLE_HAS_NO_FT, MYF(0));
		return(NULL);
	}

	if (!(ft_table->fts->added_synced)) {
		fts_init_index(ft_table, FALSE);

		ft_table->fts->added_synced = true;
	}

	const byte*	q = reinterpret_cast<const byte*>(
		const_cast<char*>(query));

	// FIXME: support ft_init_ext_with_hints(), pass LIMIT
	dberr_t	error = fts_query(trx, index, flags, q, query_len, &result);

	if (error != DB_SUCCESS) {
		my_error(convert_error_code_to_mysql(error, 0, NULL), MYF(0));
		return(NULL);
	}

	/* Allocate FTS handler, and instantiate it before return */
	fts_hdl = reinterpret_cast<NEW_FT_INFO*>(
		my_malloc(PSI_INSTRUMENT_ME, sizeof(NEW_FT_INFO), MYF(0)));

	fts_hdl->please = const_cast<_ft_vft*>(&ft_vft_result);
	fts_hdl->could_you = const_cast<_ft_vft_ext*>(&ft_vft_ext_result);
	fts_hdl->ft_prebuilt = m_prebuilt;
	fts_hdl->ft_result = result;

	/* FIXME: Re-evaluate the condition when Bug 14469540 is resolved */
	m_prebuilt->in_fts_query = true;

	return(reinterpret_cast<FT_INFO*>(fts_hdl));
}

/*****************************************************************//**
Set up search tuple for a query through FTS_DOC_ID_INDEX on
supplied Doc ID. This is used by MySQL to retrieve the documents
once the search result (Doc IDs) is available */
static
void
innobase_fts_create_doc_id_key(
/*===========================*/
	dtuple_t*	tuple,		/* in/out: m_prebuilt->search_tuple */
	const dict_index_t*
			index,		/* in: index (FTS_DOC_ID_INDEX) */
	doc_id_t*	doc_id)		/* in/out: doc id to search, value
					could be changed to storage format
					used for search. */
{
	doc_id_t	temp_doc_id;
	dfield_t*	dfield = dtuple_get_nth_field(tuple, 0);

	ut_a(dict_index_get_n_unique(index) == 1);

	dtuple_set_n_fields(tuple, index->n_fields);
	dict_index_copy_types(tuple, index, index->n_fields);

#ifdef UNIV_DEBUG
	/* The unique Doc ID field should be an eight-bytes integer */
	dict_field_t*	field = dict_index_get_nth_field(index, 0);
        ut_a(field->col->mtype == DATA_INT);
	ut_ad(sizeof(*doc_id) == field->fixed_len);
	ut_ad(!strcmp(index->name, FTS_DOC_ID_INDEX_NAME));
#endif /* UNIV_DEBUG */

	/* Convert to storage byte order */
	mach_write_to_8(reinterpret_cast<byte*>(&temp_doc_id), *doc_id);
	*doc_id = temp_doc_id;
	dfield_set_data(dfield, doc_id, sizeof(*doc_id));

        dtuple_set_n_fields_cmp(tuple, 1);

	for (ulint i = 1; i < index->n_fields; i++) {
		dfield = dtuple_get_nth_field(tuple, i);
		dfield_set_null(dfield);
	}
}

/**********************************************************************//**
Fetch next result from the FT result set
@return error code */

int
ha_innobase::ft_read(
/*=================*/
	uchar*		buf)		/*!< in/out: buf contain result row */
{
	row_prebuilt_t*	ft_prebuilt;

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO*>(ft_handler)->ft_prebuilt;

	ut_a(ft_prebuilt == m_prebuilt);

	fts_result_t*	result;

	result = reinterpret_cast<NEW_FT_INFO*>(ft_handler)->ft_result;

	if (result->current == NULL) {
		/* This is the case where the FTS query did not
		contain and matching documents. */
		if (result->rankings_by_id != NULL) {
			/* Now that we have the complete result, we
			need to sort the document ids on their rank
			calculation. */

			fts_query_sort_result_on_rank(result);

			result->current = const_cast<ib_rbt_node_t*>(
				rbt_first(result->rankings_by_rank));
		} else {
			ut_a(result->current == NULL);
		}
	} else {
		result->current = const_cast<ib_rbt_node_t*>(
			rbt_next(result->rankings_by_rank, result->current));
	}

next_record:

	if (result->current != NULL) {
		doc_id_t	search_doc_id;
		dtuple_t*	tuple = m_prebuilt->search_tuple;

		/* If we only need information from result we can return
		   without fetching the table row */
		if (ft_prebuilt->read_just_key) {
#ifdef MYSQL_STORE_FTS_DOC_ID
			if (m_prebuilt->fts_doc_id_in_read_set) {
				fts_ranking_t* ranking;
				ranking = rbt_value(fts_ranking_t,
						    result->current);
				innobase_fts_store_docid(
					table, ranking->doc_id);
			}
#endif
			table->status= 0;
			return(0);
		}

		dict_index_t*	index;

		index = m_prebuilt->table->fts_doc_id_index;

		/* Must find the index */
		ut_a(index != NULL);

		/* Switch to the FTS doc id index */
		m_prebuilt->index = index;

		fts_ranking_t*	ranking = rbt_value(
			fts_ranking_t, result->current);

		search_doc_id = ranking->doc_id;

		/* We pass a pointer of search_doc_id because it will be
		converted to storage byte order used in the search
		tuple. */
		innobase_fts_create_doc_id_key(tuple, index, &search_doc_id);

		int	error;

		switch (dberr_t ret = row_search_for_mysql(buf, PAGE_CUR_GE,
							   m_prebuilt,
							   ROW_SEL_EXACT, 0)) {
		case DB_SUCCESS:
			error = 0;
			table->status = 0;
			break;
		case DB_RECORD_NOT_FOUND:
			result->current = const_cast<ib_rbt_node_t*>(
				rbt_next(result->rankings_by_rank,
					 result->current));

			if (!result->current) {
				/* exhaust the result set, should return
				HA_ERR_END_OF_FILE just like
				ha_innobase::general_fetch() and/or
				ha_innobase::index_first() etc. */
				error = HA_ERR_END_OF_FILE;
				table->status = STATUS_NOT_FOUND;
			} else {
				goto next_record;
			}
			break;
		case DB_END_OF_INDEX:
			error = HA_ERR_END_OF_FILE;
			table->status = STATUS_NOT_FOUND;
			break;
		case DB_TABLESPACE_DELETED:

			ib_senderrf(
				m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_DISCARDED,
				table->s->table_name.str);

			table->status = STATUS_NOT_FOUND;
			error = HA_ERR_TABLESPACE_MISSING;
			break;
		case DB_TABLESPACE_NOT_FOUND:

			ib_senderrf(
				m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
				ER_TABLESPACE_MISSING,
				table->s->table_name.str);

			table->status = STATUS_NOT_FOUND;
			error = HA_ERR_TABLESPACE_MISSING;
			break;
		default:
			error = convert_error_code_to_mysql(
				ret, 0, m_user_thd);

			table->status = STATUS_NOT_FOUND;
			break;
		}

		return(error);
	}

	return(HA_ERR_END_OF_FILE);
}

#ifdef WITH_WSREP
inline
const char*
wsrep_key_type_to_str(Wsrep_service_key_type type)
{
	switch (type) {
	case WSREP_SERVICE_KEY_SHARED:
		return "shared";
	case WSREP_SERVICE_KEY_REFERENCE:
		return "reference";
	case WSREP_SERVICE_KEY_UPDATE:
		return "update";
	case WSREP_SERVICE_KEY_EXCLUSIVE:
		return "exclusive";
	};
	return "unknown";
}

extern dberr_t
wsrep_append_foreign_key(
/*===========================*/
	trx_t*		trx,		/*!< in: trx */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	const rec_t*	rec,		/*!<in: clustered index record */
	dict_index_t*	index,		/*!<in: clustered index */
	ibool		referenced,	/*!<in: is check for referenced table */
	Wsrep_service_key_type	key_type)	/*!< in: access type of this key
					(shared, exclusive, reference...) */
{
	if (!trx->is_wsrep() || !wsrep_thd_is_local(trx->mysql_thd)) {
		return DB_SUCCESS;
	}

	THD* thd = trx->mysql_thd;

	if (!foreign ||
	    (!foreign->referenced_table && !foreign->foreign_table)) {
		WSREP_INFO("FK: %s missing in: %s",
			   (!foreign ? "constraint" :
			    (!foreign->referenced_table ?
			     "referenced table" : "foreign table")),
			   wsrep_thd_query(thd));
		return DB_ERROR;
	}

	ulint rcode = DB_SUCCESS;
	char  cache_key[513] = {'\0'};
	size_t cache_key_len = 0;

	if ( !((referenced) ?
		foreign->referenced_table : foreign->foreign_table)) {
		WSREP_DEBUG("pulling %s table into cache",
			    (referenced) ? "referenced" : "foreign");
		mutex_enter(&dict_sys.mutex);

		if (referenced) {
			foreign->referenced_table =
				dict_table_get_low(
					foreign->referenced_table_name_lookup);
			if (foreign->referenced_table) {
				foreign->referenced_index =
					dict_foreign_find_index(
						foreign->referenced_table, NULL,
						foreign->referenced_col_names,
						foreign->n_fields,
						foreign->foreign_index,
						TRUE, FALSE);
			}
		} else {
	  		foreign->foreign_table =
				dict_table_get_low(
					foreign->foreign_table_name_lookup);

			if (foreign->foreign_table) {
				foreign->foreign_index =
					dict_foreign_find_index(
						foreign->foreign_table, NULL,
						foreign->foreign_col_names,
						foreign->n_fields,
						foreign->referenced_index,
						TRUE, FALSE);
			}
		}
		mutex_exit(&dict_sys.mutex);
	}

	if ( !((referenced) ?
		foreign->referenced_table : foreign->foreign_table)) {
		WSREP_WARN("FK: %s missing in query: %s",
			   (!foreign->referenced_table) ?
			   "referenced table" : "foreign table",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void");
		return DB_ERROR;
	}

	byte  key[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
	ulint len = WSREP_MAX_SUPPORTED_KEY_LENGTH;

	dict_index_t *idx_target = (referenced) ?
		foreign->referenced_index : index;
	dict_index_t *idx = (referenced) ?
		UT_LIST_GET_FIRST(foreign->referenced_table->indexes) :
		UT_LIST_GET_FIRST(foreign->foreign_table->indexes);
	int i = 0;

	while (idx != NULL && idx != idx_target) {
		if (innobase_strcasecmp (idx->name, innobase_index_reserve_name) != 0) {
			i++;
		}
		idx = UT_LIST_GET_NEXT(indexes, idx);
	}

	ut_a(idx);
	key[0] = byte(i);

	rcode = wsrep_rec_get_foreign_key(
		&key[1], &len, rec, index, idx,
		wsrep_protocol_version > 1);

	if (rcode != DB_SUCCESS) {
		WSREP_ERROR(
			"FK key set failed: " ULINTPF
			" (" ULINTPF "%s), index: %s %s, %s",
			rcode, referenced, wsrep_key_type_to_str(key_type),
			(index)       ? index->name() : "void index",
			(index && index->table) ? index->table->name.m_name :
				"void table",
			wsrep_thd_query(thd));
		return DB_ERROR;
	}

	strncpy(cache_key,
		(wsrep_protocol_version > 1) ?
		((referenced) ?
			foreign->referenced_table->name.m_name :
			foreign->foreign_table->name.m_name) :
		foreign->foreign_table->name.m_name, sizeof(cache_key) - 1);
	cache_key_len = strlen(cache_key);

#ifdef WSREP_DEBUG_PRINT
	ulint j;
	fprintf(stderr, "FK parent key, table: %s %s len: %lu ",
		cache_key, wsrep_key_type_to_str(key_type), len+1);
	for (j=0; j<len+1; j++) {
		fprintf(stderr, " %hhX, ", key[j]);
	}
	fprintf(stderr, "\n");
#endif
	char *p = strchr(cache_key, '/');

	if (p) {
		*p = '\0';
	} else {
		WSREP_WARN("unexpected foreign key table %s %s",
			   foreign->referenced_table->name.m_name,
			   foreign->foreign_table->name.m_name);
	}

	wsrep_buf_t wkey_part[3];
        wsrep_key_t wkey = {wkey_part, 3};

	if (!wsrep_prepare_key_for_innodb(
		thd,
		(const uchar*)cache_key,
		cache_key_len +  1,
		(const uchar*)key, len+1,
		wkey_part,
		(size_t*)&wkey.key_parts_num)) {
		WSREP_WARN("key prepare failed for cascaded FK: %s",
			   (wsrep_thd_query(thd)) ?
			    wsrep_thd_query(thd) : "void");
		return DB_ERROR;
	}
	rcode = wsrep_thd_append_key(thd, &wkey, 1, key_type);
	if (rcode) {
		DBUG_PRINT("wsrep", ("row key failed: " ULINTPF, rcode));
		WSREP_ERROR("Appending cascaded fk row key failed: %s, "
			    ULINTPF,
			    (wsrep_thd_query(thd)) ?
			    wsrep_thd_query(thd) : "void", rcode);
		return DB_ERROR;
	}

	return DB_SUCCESS;
}

static int
wsrep_append_key(
/*=============*/
	THD		*thd,
	trx_t 		*trx,
	TABLE_SHARE 	*table_share,
	const char*	key,
	uint16_t        key_len,
	Wsrep_service_key_type	key_type	/*!< in: access type of this key
					(shared, exclusive, semi...) */
)
{
	DBUG_ENTER("wsrep_append_key");
	DBUG_PRINT("enter",
		    ("thd: %lu trx: %lld", thd_get_thread_id(thd),
		    (long long)trx->id));
#ifdef WSREP_DEBUG_PRINT
	fprintf(stderr, "%s conn %lu, trx " TRX_ID_FMT ", keylen %d, key %s.%s\n",
		wsrep_key_type_to_str(key_type),
		thd_get_thread_id(thd), trx->id, key_len,
		table_share->table_name.str, key);
	for (int i=0; i<key_len; i++) {
		fprintf(stderr, "%hhX, ", key[i]);
	}
	fprintf(stderr, "\n");
#endif
	wsrep_buf_t wkey_part[3];
        wsrep_key_t wkey = {wkey_part, 3};

	if (!wsrep_prepare_key_for_innodb(
			thd,
			(const uchar*)table_share->table_cache_key.str,
			table_share->table_cache_key.length,
			(const uchar*)key, key_len,
			wkey_part,
			(size_t*)&wkey.key_parts_num)) {
		WSREP_WARN("key prepare failed for: %s",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void");
		DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
	}

	int rcode = wsrep_thd_append_key(thd, &wkey, 1, key_type);
	if (rcode) {
		DBUG_PRINT("wsrep", ("row key failed: %d", rcode));
		WSREP_WARN("Appending row key failed: %s, %d",
			   (wsrep_thd_query(thd)) ?
			   wsrep_thd_query(thd) : "void", rcode);
		DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
	}

	DBUG_RETURN(0);
}

static bool
referenced_by_foreign_key2(
/*=======================*/
	dict_table_t* table,
	dict_index_t* index)
{
	ut_ad(table != NULL);
	ut_ad(index != NULL);

	const dict_foreign_set* fks = &table->referenced_set;

	for (dict_foreign_set::const_iterator it = fks->begin();
             it != fks->end();
             ++it) {
                dict_foreign_t* foreign = *it;

                if (foreign->referenced_index != index) {
                        continue;
                }
                ut_ad(table == foreign->referenced_table);
                return true;
        }
        return false;
}

int
ha_innobase::wsrep_append_keys(
/*===========================*/
	THD 		*thd,
	Wsrep_service_key_type	key_type,	/*!< in: access type of this row
					operation:
					(shared, exclusive, reference...) */
	const uchar*	record0,	/* in: row in MySQL format */
	const uchar*	record1)	/* in: row in MySQL format */
{
	/* Sanity check: newly inserted records should always be passed with
	   EXCLUSIVE key type, all the rest are expected to carry a pre-image
	 */
	ut_a(record1 != NULL || key_type == WSREP_SERVICE_KEY_EXCLUSIVE);

	int rcode;
	DBUG_ENTER("wsrep_append_keys");

	bool key_appended = false;
	trx_t *trx = thd_to_trx(thd);

#ifdef WSREP_DEBUG_PRINT
	fprintf(stderr, "%s conn %lu, trx " TRX_ID_FMT ", table %s\nSQL: %s\n",
		wsrep_key_type_to_str(key_type),
		thd_get_thread_id(thd), trx->id,
		table_share->table_name.str, wsrep_thd_query(thd));
#endif

	if (table_share && table_share->tmp_table  != NO_TMP_TABLE) {
		WSREP_DEBUG("skipping tmp table DML: THD: %lu tmp: %d SQL: %s",
			    thd_get_thread_id(thd),
			    table_share->tmp_table,
			    (wsrep_thd_query(thd)) ?
			    wsrep_thd_query(thd) : "void");
		DBUG_RETURN(0);
	}

	if (wsrep_protocol_version == 0) {
		char 	keyval[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
		char 	*key 		= &keyval[0];
		bool    is_null;

		auto len = wsrep_store_key_val_for_row(
			thd, table, 0, key, WSREP_MAX_SUPPORTED_KEY_LENGTH,
			record0, &is_null);

		if (!is_null) {
			rcode = wsrep_append_key(
				thd, trx, table_share, keyval,
				len, key_type);

			if (rcode) {
				DBUG_RETURN(rcode);
			}
		} else {
			WSREP_DEBUG("NULL key skipped (proto 0): %s",
				    wsrep_thd_query(thd));
		}
	} else {
		ut_a(table->s->keys <= 256);
		uint i;
                bool hasPK= false;

		for (i=0; i<table->s->keys; ++i) {
			KEY*  key_info	= table->key_info + i;
			if (key_info->flags & HA_NOSAME) {
				hasPK = true;
				break;
			}
		}

		for (i=0; i<table->s->keys; ++i) {
			KEY*  key_info	= table->key_info + i;

			dict_index_t* idx  = innobase_get_index(i);
			dict_table_t* tab  = (idx) ? idx->table : NULL;

			/* keyval[] shall contain an ordinal number at byte 0
			   and the actual key data shall be written at byte 1.
			   Hence the total data length is the key length + 1 */
			char keyval0[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
			char keyval1[WSREP_MAX_SUPPORTED_KEY_LENGTH+1] = {'\0'};
			keyval0[0] = (char)i;
			keyval1[0] = (char)i;
			char* key0 = &keyval0[1];
			char* key1 = &keyval1[1];

			if (!tab) {
				WSREP_WARN("MariaDB-InnoDB key mismatch %s %s",
					   table->s->table_name.str,
					   key_info->name.str);
			}
			/* !hasPK == table with no PK,
			   must append all non-unique keys */
			if (!hasPK || key_info->flags & HA_NOSAME ||
			    ((tab &&
			      referenced_by_foreign_key2(tab, idx)) ||
			     (!tab && referenced_by_foreign_key()))) {

				bool is_null0;
				auto len0 = wsrep_store_key_val_for_row(
					thd, table, i, key0,
					WSREP_MAX_SUPPORTED_KEY_LENGTH,
					record0, &is_null0);

				if (record1) {
					bool is_null1;
					auto len1= wsrep_store_key_val_for_row(
						thd, table, i, key1,
						WSREP_MAX_SUPPORTED_KEY_LENGTH,
						record1, &is_null1);

					if (is_null0 != is_null1 ||
					    len0 != len1 ||
					    memcmp(key0, key1, len0)) {
						/* This key has chaged. If it
						  is unique, this is an exclusive
						  operation -> upgrade key type */
						if (key_info->flags & HA_NOSAME) {
						    key_type = WSREP_SERVICE_KEY_EXCLUSIVE;
						}

						if (!is_null1) {
						    rcode = wsrep_append_key(
							thd, trx, table_share,
							keyval1,
						    /* for len1+1 see keyval1
						     initialization comment */
							uint16_t(len1+1),
							key_type);
						    if (rcode)
							DBUG_RETURN(rcode);
						}
					}
				}

				if (!is_null0) {
					rcode = wsrep_append_key(
						thd, trx, table_share,
						/* for len0+1 see keyval0
						   initialization comment */
						keyval0, uint16_t(len0+1),
						key_type);
					if (rcode)
						DBUG_RETURN(rcode);

					if (key_info->flags & HA_NOSAME  ||
					    key_type == WSREP_SERVICE_KEY_SHARED||
					    key_type == WSREP_SERVICE_KEY_REFERENCE)
						key_appended = true;
				} else {
					WSREP_DEBUG("NULL key skipped: %s",
						    wsrep_thd_query(thd));
				}
			}
		}
	}

	/* if no PK, calculate hash of full row, to be the key value */
	if (!key_appended && wsrep_certify_nonPK) {
		uchar digest[16];
		int rcode;

		wsrep_calc_row_hash(digest, record0, table, m_prebuilt);

		if ((rcode = wsrep_append_key(thd, trx, table_share,
					      (const char*) digest, 16,
					      key_type))) {
			DBUG_RETURN(rcode);
		}

		if (record1) {
			wsrep_calc_row_hash(
				digest, record1, table, m_prebuilt);
			if ((rcode = wsrep_append_key(thd, trx, table_share,
						      (const char*) digest,
						      16, key_type))) {
				DBUG_RETURN(rcode);
			}
		}
		DBUG_RETURN(0);
	}

	DBUG_RETURN(0);
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Stores a reference to the current row to 'ref' field of the handle. Note
that in the case where we have generated the clustered index for the
table, the function parameter is illogical: we MUST ASSUME that 'record'
is the current 'position' of the handle, because if row ref is actually
the row id internally generated in InnoDB, then 'record' does not contain
it. We just guess that the row id must be for the record where the handle
was positioned the last time. */

void
ha_innobase::position(
/*==================*/
	const uchar*	record)	/*!< in: row in MySQL format */
{
	uint		len;

	ut_a(m_prebuilt->trx == thd_to_trx(ha_thd()));

	if (m_prebuilt->clust_index_was_generated) {
		/* No primary key was defined for the table and we
		generated the clustered index from row id: the
		row reference will be the row id, not any key value
		that MySQL knows of */

		len = DATA_ROW_ID_LEN;

		memcpy(ref, m_prebuilt->row_id, len);
	} else {

		/* Copy primary key as the row reference */
		KEY*	key_info = table->key_info + m_primary_key;
		key_copy(ref, (uchar*)record, key_info, key_info->key_length);
		len = key_info->key_length;
	}

	ut_ad(len == ref_length);
}

/*****************************************************************//**
Check whether there exist a column named as "FTS_DOC_ID", which is
reserved for InnoDB FTS Doc ID
@return true if there exist a "FTS_DOC_ID" column */
static
bool
create_table_check_doc_id_col(
/*==========================*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	const TABLE*	form,		/*!< in: information on table
					columns and indexes */
	ulint*		doc_id_col)	/*!< out: Doc ID column number if
					there exist a FTS_DOC_ID column,
					ULINT_UNDEFINED if column is of the
					wrong type/name/size */
{
	for (ulint i = 0; i < form->s->fields; i++) {
		const Field* field = form->field[i];
		if (!field->stored_in_db()) {
			continue;
		}

		unsigned unsigned_type;

		auto col_type = get_innobase_type_from_mysql_type(
			&unsigned_type, field);

		auto col_len = field->pack_length();

		if (innobase_strcasecmp(field->field_name.str,
					FTS_DOC_ID_COL_NAME) == 0) {

			/* Note the name is case sensitive due to
			our internal query parser */
			if (col_type == DATA_INT
			    && !field->real_maybe_null()
			    && col_len == sizeof(doc_id_t)
			    && (strcmp(field->field_name.str,
				      FTS_DOC_ID_COL_NAME) == 0)) {
				*doc_id_col = i;
			} else {
				push_warning_printf(
					trx->mysql_thd,
					Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: FTS_DOC_ID column must be"
					" of BIGINT NOT NULL type, and named"
					" in all capitalized characters");
				my_error(ER_WRONG_COLUMN_NAME, MYF(0),
					 field->field_name.str);
				*doc_id_col = ULINT_UNDEFINED;
			}

			return(true);
		}
	}

	return(false);
}


/** Finds all base columns needed to compute a given generated column.
This is returned as a bitmap, in field->table->tmp_set.
Works for both dict_v_col_t and dict_s_col_t columns.
@param[in]	table		InnoDB table
@param[in]	field		MySQL field
@param[in,out]	col		virtual or stored column */
template <typename T>
void
prepare_vcol_for_base_setup(
/*========================*/
	const dict_table_t*	table,
	const Field*	field,
	T*		col)
{
	ut_ad(col->num_base == 0);
	ut_ad(col->base_col == NULL);

	MY_BITMAP *old_read_set = field->table->read_set;

	field->table->read_set = &field->table->tmp_set;

	bitmap_clear_all(&field->table->tmp_set);
	field->vcol_info->expr->walk(
		&Item::register_field_in_read_map, 1, field->table);
	col->num_base= bitmap_bits_set(&field->table->tmp_set)
		& dict_index_t::MAX_N_FIELDS;
	if (col->num_base != 0) {
		col->base_col = static_cast<dict_col_t**>(mem_heap_zalloc(
					table->heap, col->num_base * sizeof(
						* col->base_col)));
	}
	field->table->read_set= old_read_set;
}


/** Set up base columns for virtual column
@param[in]	table		InnoDB table
@param[in]	field		MySQL field
@param[in,out]	v_col		virtual column */
void
innodb_base_col_setup(
	dict_table_t*	table,
	const Field*	field,
	dict_v_col_t*	v_col)
{
	uint16_t n = 0;

	prepare_vcol_for_base_setup(table, field, v_col);

	for (uint i= 0; i < field->table->s->fields; ++i) {
		const Field* base_field = field->table->field[i];
		if (base_field->stored_in_db()
			&& bitmap_is_set(&field->table->tmp_set, i)) {
			ulint   z;

			for (z = 0; z < table->n_cols; z++) {
				const char* name = dict_table_get_col_name(table, z);
				if (!innobase_strcasecmp(name,
						base_field->field_name.str)) {
					break;
				}
			}

			ut_ad(z != table->n_cols);

			v_col->base_col[n] = dict_table_get_nth_col(table, z);
			ut_ad(v_col->base_col[n]->ind == z);
			n++;
		}
	}
	v_col->num_base= n & dict_index_t::MAX_N_FIELDS;
}

/** Set up base columns for stored column
@param[in]	table	InnoDB table
@param[in]	field	MySQL field
@param[in,out]	s_col	stored column */
void
innodb_base_col_setup_for_stored(
	const dict_table_t*	table,
	const Field*		field,
	dict_s_col_t*		s_col)
{
	ulint	n = 0;

	prepare_vcol_for_base_setup(table, field, s_col);

	for (uint i= 0; i < field->table->s->fields; ++i) {
		const Field* base_field = field->table->field[i];

		if (base_field->stored_in_db()
		    && bitmap_is_set(&field->table->tmp_set, i)) {
			ulint	z;
			for (z = 0; z < table->n_cols; z++) {
				const char* name = dict_table_get_col_name(
						table, z);
				if (!innobase_strcasecmp(
					name, base_field->field_name.str)) {
					break;
				}
			}

			ut_ad(z != table->n_cols);

			s_col->base_col[n] = dict_table_get_nth_col(table, z);
			n++;

			if (n == s_col->num_base) {
				break;
			}
		}
	}
	s_col->num_base= n;
}

/** Create a table definition to an InnoDB database.
@return ER_* level error */
inline MY_ATTRIBUTE((warn_unused_result))
int
create_table_info_t::create_table_def()
{
	dict_table_t*	table;
	ulint		nulls_allowed;
	unsigned	unsigned_type;
	ulint		binary_type;
	ulint		long_true_varchar;
	ulint		charset_no;
	ulint		doc_id_col = 0;
	ibool		has_doc_id_col = FALSE;
	mem_heap_t*	heap;
	ha_table_option_struct *options= m_form->s->option_struct;
	dberr_t		err = DB_SUCCESS;

	DBUG_ENTER("create_table_def");
	DBUG_PRINT("enter", ("table_name: %s", m_table_name));

	DBUG_ASSERT(m_trx->mysql_thd == m_thd);
	DBUG_ASSERT(!m_drop_before_rollback);

	/* MySQL does the name length check. But we do additional check
	on the name length here */
	const size_t	table_name_len = strlen(m_table_name);
	if (table_name_len > MAX_FULL_NAME_LEN) {
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_TABLE_NAME,
			"InnoDB: Table Name or Database Name is too long");

		DBUG_RETURN(ER_TABLE_NAME);
	}

	if (m_table_name[table_name_len - 1] == '/') {
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_TABLE_NAME,
			"InnoDB: Table name is empty");

		DBUG_RETURN(ER_WRONG_TABLE_NAME);
	}

	/* Find out the number of virtual columns. */
	ulint num_v = 0;
	const bool omit_virtual = ha_innobase::omits_virtual_cols(*m_form->s);
	const ulint n_cols = omit_virtual
		? m_form->s->stored_fields : m_form->s->fields;

	if (!omit_virtual) {
		for (ulint i = 0; i < n_cols; i++) {
			num_v += !m_form->field[i]->stored_in_db();
		}
	}

	/* Check whether there already exists a FTS_DOC_ID column */
	if (create_table_check_doc_id_col(m_trx, m_form, &doc_id_col)){

		/* Raise error if the Doc ID column is of wrong type or name */
		if (doc_id_col == ULINT_UNDEFINED) {
			DBUG_RETURN(HA_ERR_GENERIC);
		} else {
			has_doc_id_col = TRUE;
		}
	}

	/* Adjust the number of columns for the FTS hidden field */
	const ulint actual_n_cols = n_cols
		+ (m_flags2 & DICT_TF2_FTS && !has_doc_id_col);

	table = dict_mem_table_create(m_table_name, NULL,
				      actual_n_cols, num_v, m_flags, m_flags2);

	/* Set the hidden doc_id column. */
	if (m_flags2 & DICT_TF2_FTS) {
		table->fts->doc_col = has_doc_id_col
				      ? doc_id_col : n_cols - num_v;
	}

	if (DICT_TF_HAS_DATA_DIR(m_flags)) {
		ut_a(strlen(m_remote_path));

		table->data_dir_path = mem_heap_strdup(
			table->heap, m_remote_path);

	} else {
		table->data_dir_path = NULL;
	}

	heap = mem_heap_create(1000);
	auto _ = make_scope_exit([heap]() { mem_heap_free(heap); });

	ut_d(bool have_vers_start = false);
	ut_d(bool have_vers_end = false);

	for (ulint i = 0, j = 0; j < n_cols; i++) {
		Field*	field = m_form->field[i];
		ulint vers_row = 0;

		if (m_form->versioned()) {
			if (i == m_form->s->vers.start_fieldno) {
				vers_row = DATA_VERS_START;
				ut_d(have_vers_start = true);
			} else if (i == m_form->s->vers.end_fieldno) {
				vers_row = DATA_VERS_END;
				ut_d(have_vers_end = true);
			} else if (!(field->flags
				     & VERS_UPDATE_UNVERSIONED_FLAG)) {
				vers_row = DATA_VERSIONED;
			}
		}

		auto col_type = get_innobase_type_from_mysql_type(
			&unsigned_type, field);

		if (!col_type) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_CANT_CREATE_TABLE,
				"Error creating table '%s' with"
				" column '%s'. Please check its"
				" column type and try to re-create"
				" the table with an appropriate"
				" column type.",
				table->name.m_name, field->field_name.str);
err_col:
			dict_mem_table_free(table);
			ut_ad(trx_state_eq(m_trx, TRX_STATE_NOT_STARTED));
			DBUG_RETURN(HA_ERR_GENERIC);
		}

		nulls_allowed = field->real_maybe_null() ? 0 : DATA_NOT_NULL;
		binary_type = field->binary() ? DATA_BINARY_TYPE : 0;

		charset_no = 0;

		if (dtype_is_string_type(col_type)) {

			charset_no = (ulint) field->charset()->number;

			DBUG_EXECUTE_IF("simulate_max_char_col",
					charset_no = MAX_CHAR_COLL_NUM + 1;
					);

			if (charset_no > MAX_CHAR_COLL_NUM) {
				/* in data0type.h we assume that the
				number fits in one byte in prtype */
				push_warning_printf(
					m_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_CANT_CREATE_TABLE,
					"In InnoDB, charset-collation codes"
					" must be below 256."
					" Unsupported code " ULINTPF ".",
					charset_no);
				dict_mem_table_free(table);

				DBUG_RETURN(ER_CANT_CREATE_TABLE);
			}
		}

		auto col_len = field->pack_length();

		/* The MySQL pack length contains 1 or 2 bytes length field
		for a true VARCHAR. Let us subtract that, so that the InnoDB
		column length in the InnoDB data dictionary is the real
		maximum byte length of the actual data. */

		long_true_varchar = 0;

		if (field->type() == MYSQL_TYPE_VARCHAR) {
			col_len -= ((Field_varstring*) field)->length_bytes;

			if (((Field_varstring*) field)->length_bytes == 2) {
				long_true_varchar = DATA_LONG_TRUE_VARCHAR;
			}
		}

		/* First check whether the column to be added has a
		system reserved name. */
		if (dict_col_name_is_reserved(field->field_name.str)){
			my_error(ER_WRONG_COLUMN_NAME, MYF(0),
				 field->field_name.str);
			goto err_col;
		}

		ulint is_virtual = !field->stored_in_db() ? DATA_VIRTUAL : 0;

		if (!is_virtual) {
			dict_mem_table_add_col(table, heap,
				field->field_name.str, col_type,
				dtype_form_prtype(
					(ulint) field->type()
					| nulls_allowed | unsigned_type
					| binary_type | long_true_varchar
					| vers_row,
					charset_no),
				col_len);
		} else if (!omit_virtual) {
			dict_mem_table_add_v_col(table, heap,
				field->field_name.str, col_type,
				dtype_form_prtype(
					(ulint) field->type()
					| nulls_allowed | unsigned_type
					| binary_type | long_true_varchar
					| vers_row
					| is_virtual,
					charset_no),
				col_len, i, 0);
		}

		if (innobase_is_s_fld(field)) {
			ut_ad(!is_virtual);
			/* Added stored column in m_s_cols list. */
			dict_mem_table_add_s_col(
				table, 0);
		}

		if (is_virtual && omit_virtual) {
			continue;
		}

		j++;
	}

	ut_ad(have_vers_start == have_vers_end);
	ut_ad(table->versioned() == have_vers_start);
	ut_ad(!table->versioned() || table->vers_start != table->vers_end);

	if (num_v) {
		for (ulint i = 0, j = 0; i < n_cols; i++) {
			dict_v_col_t*	v_col;

			const Field* field = m_form->field[i];

			if (field->stored_in_db()) {
				continue;
			}

			v_col = dict_table_get_nth_v_col(table, j);

			j++;

			innodb_base_col_setup(table, field, v_col);
		}
	}

	/** Fill base columns for the stored column present in the list. */
	if (table->s_cols && !table->s_cols->empty()) {
		for (ulint i = 0; i < n_cols; i++) {
			Field*  field = m_form->field[i];

			if (!innobase_is_s_fld(field)) {
				continue;
			}

			dict_s_col_list::iterator       it;
			for (it = table->s_cols->begin();
			     it != table->s_cols->end(); ++it) {
				dict_s_col_t	s_col = *it;

				if (s_col.s_pos == i) {
					innodb_base_col_setup_for_stored(
						table, field, &s_col);
					break;
				}
			}
		}
	}

	/* Add the FTS doc_id hidden column. */
	if (m_flags2 & DICT_TF2_FTS && !has_doc_id_col) {
		fts_add_doc_id_column(table, heap);
	}

	dict_table_add_system_columns(table, heap);

	if (table->is_temporary()) {
		if ((options->encryption == 1
		     && !innodb_encrypt_temporary_tables)
		    || (options->encryption == 2
			&& innodb_encrypt_temporary_tables)) {
			push_warning_printf(m_thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_ILLEGAL_HA_CREATE_OPTION,
					    "Ignoring encryption parameter during "
					    "temporary table creation.");
		}

		m_trx->table_id = table->id
			= dict_sys.get_temporary_table_id();
		ut_ad(dict_tf_get_rec_format(table->flags)
		      != REC_FORMAT_COMPRESSED);
		table->space_id = SRV_TMP_SPACE_ID;
		table->space = fil_system.temp_space;
		table->add_to_cache();
	} else {
		if (err == DB_SUCCESS) {
			err = row_create_table_for_mysql(
				table, m_trx,
				fil_encryption_t(options->encryption),
				uint32_t(options->encryption_key_id));
			m_drop_before_rollback = (err == DB_SUCCESS);
		}

		DBUG_EXECUTE_IF("ib_crash_during_create_for_encryption",
				DBUG_SUICIDE(););
	}

	DBUG_EXECUTE_IF("ib_create_err_tablespace_exist",
			err = DB_TABLESPACE_EXISTS;);

	switch (err) {
	case DB_SUCCESS:
		ut_ad(table);
		m_table = table;
		DBUG_RETURN(0);
	default:
		break;
	case DB_DUPLICATE_KEY:
	case DB_TABLESPACE_EXISTS:
		char display_name[FN_REFLEN];
		char* buf_end = innobase_convert_identifier(
			display_name, sizeof(display_name) - 1,
			m_table_name, strlen(m_table_name),
			m_thd);

		*buf_end = '\0';

		my_error(err == DB_DUPLICATE_KEY
			 ? ER_TABLE_EXISTS_ERROR
			 : ER_TABLESPACE_EXISTS, MYF(0), display_name);
	}

	DBUG_RETURN(convert_error_code_to_mysql(err, m_flags, m_thd));
}

/*****************************************************************//**
Creates an index in an InnoDB database. */
inline
int
create_index(
/*=========*/
	trx_t*		trx,		/*!< in: InnoDB transaction handle */
	const TABLE*	form,		/*!< in: information on table
					columns and indexes */
	dict_table_t*	table,		/*!< in,out: table */
	uint		key_num)	/*!< in: index number */
{
	dict_index_t*	index;
	int		error;
	const KEY*	key;
	ulint*		field_lengths;

	DBUG_ENTER("create_index");

	key = form->key_info + key_num;

	/* Assert that "GEN_CLUST_INDEX" cannot be used as non-primary index */
	ut_a(innobase_strcasecmp(key->name.str, innobase_index_reserve_name) != 0);

	if (key->flags & (HA_SPATIAL | HA_FULLTEXT)) {
		/* Only one of these can be specified at a time. */
		ut_ad(~key->flags & (HA_SPATIAL | HA_FULLTEXT));
		ut_ad(!(key->flags & HA_NOSAME));
		index = dict_mem_index_create(table, key->name.str,
					      (key->flags & HA_SPATIAL)
					      ? DICT_SPATIAL : DICT_FTS,
					      key->user_defined_key_parts);

		for (ulint i = 0; i < key->user_defined_key_parts; i++) {
			const Field* field = key->key_part[i].field;

			/* We do not support special (Fulltext or Spatial)
			index on virtual columns */
			if (!field->stored_in_db()) {
				ut_ad(0);
				DBUG_RETURN(HA_ERR_UNSUPPORTED);
			}

			dict_mem_index_add_field(index, field->field_name.str,
						 0);
		}

		DBUG_RETURN(convert_error_code_to_mysql(
				    row_create_index_for_mysql(
					    index, trx, NULL),
				    table->flags, NULL));
	}

	ulint ind_type = 0;

	if (key_num == form->s->primary_key) {
		ind_type |= DICT_CLUSTERED;
	}

	if (key->flags & HA_NOSAME) {
		ind_type |= DICT_UNIQUE;
	}

	field_lengths = (ulint*) my_malloc(PSI_INSTRUMENT_ME,
		key->user_defined_key_parts * sizeof *
				field_lengths, MYF(MY_FAE));

	/* We pass 0 as the space id, and determine at a lower level the space
	id where to store the table */

	index = dict_mem_index_create(table, key->name.str,
				      ind_type, key->user_defined_key_parts);

	for (ulint i = 0; i < key->user_defined_key_parts; i++) {
		KEY_PART_INFO*	key_part = key->key_part + i;
		ulint		prefix_len;
		unsigned	is_unsigned;


		/* (The flag HA_PART_KEY_SEG denotes in MySQL a
		column prefix field in an index: we only store a
		specified number of first bytes of the column to
		the index field.) The flag does not seem to be
		properly set by MySQL. Let us fall back on testing
		the length of the key part versus the column.
		We first reach to the table's column; if the index is on a
		prefix, key_part->field is not the table's column (it's a
		"fake" field forged in open_table_from_share() with length
		equal to the length of the prefix); so we have to go to
		form->fied. */
		Field*	field= form->field[key_part->field->field_index];
		if (field == NULL)
		  ut_error;

		const char*	field_name = key_part->field->field_name.str;

		auto col_type = get_innobase_type_from_mysql_type(
			&is_unsigned, key_part->field);

		if (DATA_LARGE_MTYPE(col_type)
		    || (key_part->length < field->pack_length()
			&& field->type() != MYSQL_TYPE_VARCHAR)
		    || (field->type() == MYSQL_TYPE_VARCHAR
			&& key_part->length < field->pack_length()
			- ((Field_varstring*) field)->length_bytes)) {

			switch (col_type) {
			default:
				prefix_len = key_part->length;
				break;
			case DATA_INT:
			case DATA_FLOAT:
			case DATA_DOUBLE:
			case DATA_DECIMAL:
				sql_print_error(
					"MariaDB is trying to create a column"
					" prefix index field, on an"
					" inappropriate data type. Table"
					" name %s, column name %s.",
					form->s->table_name.str,
					key_part->field->field_name.str);

				prefix_len = 0;
			}
		} else {
			prefix_len = 0;
		}

		field_lengths[i] = key_part->length;

		if (!key_part->field->stored_in_db()) {
			index->type |= DICT_VIRTUAL;
		}

		dict_mem_index_add_field(index, field_name, prefix_len);
	}

	ut_ad(key->flags & HA_FULLTEXT || !(index->type & DICT_FTS));

	/* Even though we've defined max_supported_key_part_length, we
	still do our own checking using field_lengths to be absolutely
	sure we don't create too long indexes. */
	ulint flags = table->flags;

	error = convert_error_code_to_mysql(
		row_create_index_for_mysql(index, trx, field_lengths),
		flags, NULL);

	my_free(field_lengths);

	DBUG_RETURN(error);
}

/** Return a display name for the row format
@param[in]	row_format	Row Format
@return row format name */
static
const char*
get_row_format_name(
	enum row_type	row_format)
{
	switch (row_format) {
	case ROW_TYPE_COMPACT:
		return("COMPACT");
	case ROW_TYPE_COMPRESSED:
		return("COMPRESSED");
	case ROW_TYPE_DYNAMIC:
		return("DYNAMIC");
	case ROW_TYPE_REDUNDANT:
		return("REDUNDANT");
	case ROW_TYPE_DEFAULT:
		return("DEFAULT");
	case ROW_TYPE_FIXED:
		return("FIXED");
	case ROW_TYPE_PAGE:
	case ROW_TYPE_NOT_USED:
		break;
	}
	return("NOT USED");
}

/** Validate DATA DIRECTORY option.
@return true if valid, false if not. */
bool
create_table_info_t::create_option_data_directory_is_valid()
{
	bool		is_valid = true;

	ut_ad(m_create_info->data_file_name
	      && m_create_info->data_file_name[0] != '\0');

	/* Use DATA DIRECTORY only with file-per-table. */
	if (!m_allow_file_per_table) {
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: DATA DIRECTORY requires"
			" innodb_file_per_table.");
		is_valid = false;
	}

	/* Do not use DATA DIRECTORY with TEMPORARY TABLE. */
	if (m_create_info->tmp_table()) {
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: DATA DIRECTORY cannot be used"
			" for TEMPORARY tables.");
		is_valid = false;
	}

	/* We check for a DATA DIRECTORY mixed with TABLESPACE in
	create_option_tablespace_is_valid(), no need to here. */

	return(is_valid);
}

/** Validate the create options. Check that the options KEY_BLOCK_SIZE,
ROW_FORMAT, DATA DIRECTORY, TEMPORARY are compatible with
each other and other settings.  These CREATE OPTIONS are not validated
here unless innodb_strict_mode is on. With strict mode, this function
will report each problem it finds using a custom message with error
code ER_ILLEGAL_HA_CREATE_OPTION, not its built-in message.
@return NULL if valid, string name of bad option if not. */
const char*
create_table_info_t::create_options_are_invalid()
{
	bool	has_key_block_size = (m_create_info->key_block_size != 0);

	const char*	ret = NULL;
	enum row_type	row_format	= m_create_info->row_type;
	const bool	is_temp 	= m_create_info->tmp_table();

	ut_ad(m_thd != NULL);

	/* If innodb_strict_mode is not set don't do any more validation. */
	if (!THDVAR(m_thd, strict_mode)) {
		return(NULL);
	}

	/* Check if a non-zero KEY_BLOCK_SIZE was specified. */
	if (has_key_block_size) {
		if (is_temp || innodb_read_only_compressed) {
			my_error(ER_UNSUPPORTED_COMPRESSED_TABLE, MYF(0));
			return("KEY_BLOCK_SIZE");
		}

		switch (m_create_info->key_block_size) {
			ulint	kbs_max;
		case 1:
		case 2:
		case 4:
		case 8:
		case 16:
			/* The maximum KEY_BLOCK_SIZE (KBS) is
			UNIV_PAGE_SIZE_MAX. But if srv_page_size is
			smaller than UNIV_PAGE_SIZE_MAX, the maximum
			KBS is also smaller. */
			kbs_max = ut_min(
				1U << (UNIV_PAGE_SSIZE_MAX - 1),
				1U << (PAGE_ZIP_SSIZE_MAX - 1));
			if (m_create_info->key_block_size > kbs_max) {
				push_warning_printf(
					m_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: KEY_BLOCK_SIZE=%ld"
					" cannot be larger than %ld.",
					m_create_info->key_block_size,
					kbs_max);
				ret = "KEY_BLOCK_SIZE";
			}

			/* Valid KEY_BLOCK_SIZE, check its dependencies. */
			if (!m_allow_file_per_table) {
				push_warning(
					m_thd, Sql_condition::WARN_LEVEL_WARN,
					ER_ILLEGAL_HA_CREATE_OPTION,
					"InnoDB: KEY_BLOCK_SIZE requires"
					" innodb_file_per_table.");
				ret = "KEY_BLOCK_SIZE";
			}
			break;
		default:
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: invalid KEY_BLOCK_SIZE = %u."
				" Valid values are [1, 2, 4, 8, 16]",
				(uint) m_create_info->key_block_size);
			ret = "KEY_BLOCK_SIZE";
			break;
		}
	}

	/* Check for a valid InnoDB ROW_FORMAT specifier and
	other incompatibilities. */
	switch (row_format) {
	case ROW_TYPE_COMPRESSED:
		if (is_temp || innodb_read_only_compressed) {
			my_error(ER_UNSUPPORTED_COMPRESSED_TABLE, MYF(0));
			return("ROW_FORMAT");
		}
		if (!m_allow_file_per_table) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=%s requires"
				" innodb_file_per_table.",
				get_row_format_name(row_format));
			ret = "ROW_FORMAT";
		}
		break;
	case ROW_TYPE_DYNAMIC:
	case ROW_TYPE_COMPACT:
	case ROW_TYPE_REDUNDANT:
		if (has_key_block_size) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: cannot specify ROW_FORMAT = %s"
				" with KEY_BLOCK_SIZE.",
				get_row_format_name(row_format));
			ret = "KEY_BLOCK_SIZE";
		}
		break;
	case ROW_TYPE_DEFAULT:
		break;
	case ROW_TYPE_FIXED:
	case ROW_TYPE_PAGE:
	case ROW_TYPE_NOT_USED:
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: invalid ROW_FORMAT specifier.");
		ret = "ROW_TYPE";
		break;
	}

	if (m_create_info->data_file_name
	    && m_create_info->data_file_name[0] != '\0'
	    && !create_option_data_directory_is_valid()) {
		ret = "DATA DIRECTORY";
	}

	/* Do not allow INDEX_DIRECTORY */
	if (m_create_info->index_file_name) {
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: INDEX DIRECTORY is not supported");
		ret = "INDEX DIRECTORY";
	}

	/* Don't support compressed table when page size > 16k. */
	if ((has_key_block_size || row_format == ROW_TYPE_COMPRESSED)
	    && srv_page_size > UNIV_PAGE_SIZE_DEF) {
		push_warning(m_thd, Sql_condition::WARN_LEVEL_WARN,
			     ER_ILLEGAL_HA_CREATE_OPTION,
			     "InnoDB: Cannot create a COMPRESSED table"
			     " when innodb_page_size > 16k.");

		if (has_key_block_size) {
			ret = "KEY_BLOCK_SIZE";
		} else {
			ret = "ROW_TYPE";
		}
	}

	return(ret);
}

/*****************************************************************//**
Check engine specific table options not handled by SQL-parser.
@return	NULL if valid, string if not */
const char*
create_table_info_t::check_table_options()
{
	enum row_type row_format = m_create_info->row_type;
	const ha_table_option_struct *options= m_form->s->option_struct;

	switch (options->encryption) {
	case FIL_ENCRYPTION_OFF:
		if (options->encryption_key_id != FIL_DEFAULT_ENCRYPTION_KEY) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: ENCRYPTED=NO implies"
				" ENCRYPTION_KEY_ID=1");
			compile_time_assert(FIL_DEFAULT_ENCRYPTION_KEY == 1);
		}
		if (srv_encrypt_tables != 2) {
			break;
		}
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_WRONG_CREATE_OPTION,
			"InnoDB: ENCRYPTED=NO cannot be used with"
			" innodb_encrypt_tables=FORCE");
		return "ENCRYPTED";
	case FIL_ENCRYPTION_DEFAULT:
		if (!srv_encrypt_tables) {
			break;
		}
		/* fall through */
	case FIL_ENCRYPTION_ON:
		const uint32_t key_id = uint32_t(options->encryption_key_id);
		if (!encryption_key_id_exists(key_id)) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: ENCRYPTION_KEY_ID %u not available",
				key_id);
			return "ENCRYPTION_KEY_ID";
		}

		/* We do not support encryption for spatial indexes,
		except if innodb_checksum_algorithm=full_crc32.
		Do not allow ENCRYPTED=YES if any SPATIAL INDEX exists. */
		if (options->encryption != FIL_ENCRYPTION_ON
		    || srv_checksum_algorithm
		    >= SRV_CHECKSUM_ALGORITHM_FULL_CRC32) {
			break;
		}
		for (ulint i = 0; i < m_form->s->keys; i++) {
			if (m_form->key_info[i].flags & HA_SPATIAL) {
				push_warning(m_thd,
					     Sql_condition::WARN_LEVEL_WARN,
					     HA_ERR_UNSUPPORTED,
					     "InnoDB: ENCRYPTED=YES is not"
					     " supported for SPATIAL INDEX");
				return "ENCRYPTED";
			}
		}
	}

	if (!m_allow_file_per_table
	    && options->encryption != FIL_ENCRYPTION_DEFAULT) {
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_WRONG_CREATE_OPTION,
			"InnoDB: ENCRYPTED requires innodb_file_per_table");
		return "ENCRYPTED";
 	}

	/* Check page compression requirements */
	if (options->page_compressed) {

		if (row_format == ROW_TYPE_COMPRESSED) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED table can't have"
				" ROW_TYPE=COMPRESSED");
			return "PAGE_COMPRESSED";
		}

		switch (row_format) {
		default:
			break;
		case ROW_TYPE_DEFAULT:
			if (m_default_row_format
			    != DEFAULT_ROW_FORMAT_REDUNDANT) {
				break;
			}
			/* fall through */
		case ROW_TYPE_REDUNDANT:
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED table can't have"
				" ROW_TYPE=REDUNDANT");
			return "PAGE_COMPRESSED";
		}

		if (!m_allow_file_per_table) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED requires"
				" innodb_file_per_table.");
			return "PAGE_COMPRESSED";
		}

		if (m_create_info->key_block_size) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSED table can't have"
				" key_block_size");
			return "PAGE_COMPRESSED";
		}
	}

	/* Check page compression level requirements, some of them are
	already checked above */
	if (options->page_compression_level != 0) {
		if (options->page_compressed == false) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: PAGE_COMPRESSION_LEVEL requires"
				" PAGE_COMPRESSED");
			return "PAGE_COMPRESSION_LEVEL";
		}

		if (options->page_compression_level < 1 || options->page_compression_level > 9) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				HA_WRONG_CREATE_OPTION,
				"InnoDB: invalid PAGE_COMPRESSION_LEVEL = %lu."
				" Valid values are [1, 2, 3, 4, 5, 6, 7, 8, 9]",
				options->page_compression_level);
			return "PAGE_COMPRESSION_LEVEL";
		}
	}

	return NULL;
}

/*****************************************************************//**
Update create_info.  Used in SHOW CREATE TABLE et al. */

void
ha_innobase::update_create_info(
/*============================*/
	HA_CREATE_INFO*	create_info)	/*!< in/out: create info */
{
	if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) {
		info(HA_STATUS_AUTO);
		create_info->auto_increment_value = stats.auto_increment_value;
	}

	if (m_prebuilt->table->is_temporary()) {
		return;
	}

	dict_get_and_save_data_dir_path(m_prebuilt->table, false);

	if (m_prebuilt->table->data_dir_path) {
		create_info->data_file_name = m_prebuilt->table->data_dir_path;
	}
}

/*****************************************************************//**
Initialize the table FTS stopword list
@return TRUE if success */
ibool
innobase_fts_load_stopword(
/*=======================*/
	dict_table_t*	table,	/*!< in: Table has the FTS */
	trx_t*		trx,	/*!< in: transaction */
	THD*		thd)	/*!< in: current thread */
{
  const char *stopword_table= THDVAR(thd, ft_user_stopword_table);
  if (!stopword_table)
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    if (innobase_server_stopword_table)
      stopword_table= thd_strdup(thd, innobase_server_stopword_table);
    mysql_mutex_unlock(&LOCK_global_system_variables);
  }

  return fts_load_stopword(table, trx, stopword_table,
                           THDVAR(thd, ft_enable_stopword), false);
}

/** Parse the table name into normal name and remote path if needed.
@param[in]	name	Table name (db/table or full path).
@return 0 if successful, otherwise, error number */
int
create_table_info_t::parse_table_name(
	const char*
#ifdef _WIN32
	name
#endif
				      )
{
	DBUG_ENTER("parse_table_name");

#ifdef _WIN32
	/* Names passed in from server are in two formats:
	1. <database_name>/<table_name>: for normal table creation
	2. full path: for temp table creation, or DATA DIRECTORY.

	When srv_file_per_table is on and mysqld_embedded is off,
	check for full path pattern, i.e.
	X:\dir\...,		X is a driver letter, or
	\\dir1\dir2\...,	UNC path
	returns error if it is in full path format, but not creating a temp.
	table. Currently InnoDB does not support symbolic link on Windows. */

	if (m_innodb_file_per_table
	    && !mysqld_embedded
	    && !m_create_info->tmp_table()) {

		if ((name[1] == ':')
		    || (name[0] == '\\' && name[1] == '\\')) {
			sql_print_error("Cannot create table %s\n", name);
			DBUG_RETURN(HA_ERR_GENERIC);
		}
	}
#endif

	m_remote_path[0] = '\0';

	/* Make sure DATA DIRECTORY is compatible with other options
	and set the remote path.  In the case of either;
	  CREATE TEMPORARY TABLE ... DATA DIRECTORY={path} ... ;
	  CREATE TABLE ... DATA DIRECTORY={path} TABLESPACE={name}... ;
	we ignore the DATA DIRECTORY. */
	if (m_create_info->data_file_name
	    && m_create_info->data_file_name[0] != '\0') {
		if (!create_option_data_directory_is_valid()) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				WARN_OPTION_IGNORED,
				ER_DEFAULT(WARN_OPTION_IGNORED),
				"DATA DIRECTORY");

			m_flags &= ~DICT_TF_MASK_DATA_DIR;
		} else {
			strncpy(m_remote_path,
				m_create_info->data_file_name,
				FN_REFLEN - 1);
		}
	}

	if (m_create_info->index_file_name) {
		my_error(WARN_OPTION_IGNORED, ME_WARNING,
			"INDEX DIRECTORY");
	}

	DBUG_RETURN(0);
}

/** @return whether innodb_strict_mode is active */
bool ha_innobase::is_innodb_strict_mode(THD *thd)
{
  return THDVAR(thd, strict_mode);
}

/** Determine InnoDB table flags.
If strict_mode=OFF, this will adjust the flags to what should be assumed.
@retval true on success
@retval false on error */
bool create_table_info_t::innobase_table_flags()
{
	DBUG_ENTER("innobase_table_flags");

	const char*	fts_doc_id_index_bad = NULL;
	ulint		zip_ssize = 0;
	enum row_type	row_type;
	rec_format_t	innodb_row_format =
		get_row_format(m_default_row_format);
	const bool	is_temp = m_create_info->tmp_table();
	bool		zip_allowed = !is_temp;

	const ulint	zip_ssize_max =
		ut_min(static_cast<ulint>(UNIV_PAGE_SSIZE_MAX),
		       static_cast<ulint>(PAGE_ZIP_SSIZE_MAX));

	/* Cache the value of innobase_compression_level, in case it is
	modified by another thread while the table is being created. */
	const ulint     default_compression_level = page_zip_level;

	ha_table_option_struct *options= m_form->s->option_struct;

	m_flags = 0;
	m_flags2 = 0;

	/* Check if there are any FTS indexes defined on this table. */
	for (uint i = 0; i < m_form->s->keys; i++) {
		const KEY*	key = &m_form->key_info[i];

		if (key->flags & HA_FULLTEXT) {
			m_flags2 |= DICT_TF2_FTS;

			/* We don't support FTS indexes in temporary
			tables. */
			if (is_temp) {
				my_error(ER_INNODB_NO_FT_TEMP_TABLE, MYF(0));
				DBUG_RETURN(false);
			}

			if (fts_doc_id_index_bad) {
				goto index_bad;
			}
		}

		if (innobase_strcasecmp(key->name.str, FTS_DOC_ID_INDEX_NAME)) {
			continue;
		}

		/* Do a pre-check on FTS DOC ID index */
		if (!(key->flags & HA_NOSAME)
		    || strcmp(key->name.str, FTS_DOC_ID_INDEX_NAME)
		    || strcmp(key->key_part[0].field->field_name.str,
			      FTS_DOC_ID_COL_NAME)) {
			fts_doc_id_index_bad = key->name.str;
		}

		if (fts_doc_id_index_bad && (m_flags2 & DICT_TF2_FTS)) {
index_bad:
			my_error(ER_INNODB_FT_WRONG_DOCID_INDEX, MYF(0),
				 fts_doc_id_index_bad);
			DBUG_RETURN(false);
		}
	}

	if (m_create_info->key_block_size > 0) {
		/* The requested compressed page size (key_block_size)
		is given in kilobytes. If it is a valid number, store
		that value as the number of log2 shifts from 512 in
		zip_ssize. Zero means it is not compressed. */
		ulint	zssize;		/* Zip Shift Size */
		ulint	kbsize;		/* Key Block Size */
		for (zssize = kbsize = 1;
		     zssize <= zip_ssize_max;
		     zssize++, kbsize <<= 1) {
			if (kbsize == m_create_info->key_block_size) {
				zip_ssize = zssize;
				break;
			}
		}

		/* Make sure compressed row format is allowed. */
		if (is_temp) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: KEY_BLOCK_SIZE is ignored"
				" for TEMPORARY TABLE.");
			zip_allowed = false;
		} else if (!m_allow_file_per_table) {
			push_warning(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: KEY_BLOCK_SIZE requires"
				" innodb_file_per_table.");
			zip_allowed = false;
		}

		if (!zip_allowed
		    || zssize > zip_ssize_max) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ignoring KEY_BLOCK_SIZE=%u.",
				(uint) m_create_info->key_block_size);
		}
	}

	row_type = m_create_info->row_type;

	if (zip_ssize && zip_allowed) {
		/* if ROW_FORMAT is set to default,
		automatically change it to COMPRESSED. */
		if (row_type == ROW_TYPE_DEFAULT) {
			row_type = ROW_TYPE_COMPRESSED;
		} else if (row_type != ROW_TYPE_COMPRESSED) {
			/* ROW_FORMAT other than COMPRESSED
			ignores KEY_BLOCK_SIZE.  It does not
			make sense to reject conflicting
			KEY_BLOCK_SIZE and ROW_FORMAT, because
			such combinations can be obtained
			with ALTER TABLE anyway. */
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ignoring KEY_BLOCK_SIZE=%u"
				" unless ROW_FORMAT=COMPRESSED.",
				(uint) m_create_info->key_block_size);
			zip_allowed = false;
		}
	} else {
		/* zip_ssize == 0 means no KEY_BLOCK_SIZE. */
		if (row_type == ROW_TYPE_COMPRESSED && zip_allowed) {
			/* ROW_FORMAT=COMPRESSED without KEY_BLOCK_SIZE
			implies half the maximum KEY_BLOCK_SIZE(*1k) or
			srv_page_size, whichever is less. */
			zip_ssize = zip_ssize_max - 1;
		}
	}

	/* Validate the row format.  Correct it if necessary */

	switch (row_type) {
	case ROW_TYPE_REDUNDANT:
		innodb_row_format = REC_FORMAT_REDUNDANT;
		break;
	case ROW_TYPE_COMPACT:
		innodb_row_format = REC_FORMAT_COMPACT;
		break;
	case ROW_TYPE_COMPRESSED:
		if (is_temp) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=%s is ignored for"
				" TEMPORARY TABLE.",
				get_row_format_name(row_type));
		} else if (!m_allow_file_per_table) {
			push_warning_printf(
				m_thd, Sql_condition::WARN_LEVEL_WARN,
				ER_ILLEGAL_HA_CREATE_OPTION,
				"InnoDB: ROW_FORMAT=COMPRESSED requires"
				" innodb_file_per_table.");
		} else {
			innodb_row_format = REC_FORMAT_COMPRESSED;
			break;
		}
		zip_allowed = false;
		/* Set ROW_FORMAT = COMPACT */
		/* fall through */
	case ROW_TYPE_NOT_USED:
	case ROW_TYPE_FIXED:
	case ROW_TYPE_PAGE:
		push_warning(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			ER_ILLEGAL_HA_CREATE_OPTION,
			"InnoDB: assuming ROW_FORMAT=DYNAMIC.");
		/* fall through */
	case ROW_TYPE_DYNAMIC:
		innodb_row_format = REC_FORMAT_DYNAMIC;
		break;
	case ROW_TYPE_DEFAULT:
		;
	}

	/* Don't support compressed table when page size > 16k. */
	if (zip_allowed && zip_ssize && srv_page_size > UNIV_PAGE_SIZE_DEF) {
		push_warning(m_thd, Sql_condition::WARN_LEVEL_WARN,
			     ER_ILLEGAL_HA_CREATE_OPTION,
			     "InnoDB: Cannot create a COMPRESSED table"
			     " when innodb_page_size > 16k."
			     " Assuming ROW_FORMAT=DYNAMIC.");
		zip_allowed = false;
	}

	ut_ad(!is_temp || !zip_allowed);
	ut_ad(!is_temp || innodb_row_format != REC_FORMAT_COMPRESSED);

	/* Set the table flags */
	if (!zip_allowed) {
		zip_ssize = 0;
	}

	if (is_temp) {
		m_flags2 |= DICT_TF2_TEMPORARY;
	} else if (m_use_file_per_table) {
		m_flags2 |= DICT_TF2_USE_FILE_PER_TABLE;
	}

	/* Set the table flags */
	dict_tf_set(&m_flags, innodb_row_format, zip_ssize,
			m_use_data_dir,
			options->page_compressed,
		    	options->page_compression_level == 0 ?
		        default_compression_level : ulint(options->page_compression_level));

	if (m_form->s->table_type == TABLE_TYPE_SEQUENCE) {
		m_flags |= DICT_TF_MASK_NO_ROLLBACK;
	}

	/* Set the flags2 when create table or alter tables */
	m_flags2 |= DICT_TF2_FTS_AUX_HEX_NAME;
	DBUG_EXECUTE_IF("innodb_test_wrong_fts_aux_table_name",
			m_flags2 &= ~DICT_TF2_FTS_AUX_HEX_NAME;);

	DBUG_RETURN(true);
}

/** Parse MERGE_THRESHOLD value from the string.
@param[in]	thd	connection
@param[in]	str	string which might include 'MERGE_THRESHOLD='
@return	value parsed. 0 means not found or invalid value. */
static
unsigned
innobase_parse_merge_threshold(
	THD*		thd,
	const char*	str)
{
	static const char*	label = "MERGE_THRESHOLD=";
	static const size_t	label_len = strlen(label);
	const char*		pos = str;

	pos = strstr(str, label);

	if (pos == NULL) {
		return(0);
	}

	pos += label_len;

	lint	ret = atoi(pos);

	if (ret > 0 && ret <= 50) {
		return(static_cast<unsigned>(ret));
	}

	push_warning_printf(
		thd, Sql_condition::WARN_LEVEL_WARN,
		ER_ILLEGAL_HA_CREATE_OPTION,
		"InnoDB: Invalid value for MERGE_THRESHOLD in the CREATE TABLE"
		" statement. The value is ignored.");

	return(0);
}

/** Parse hint for table and its indexes, and update the information
in dictionary.
@param[in]	thd		connection
@param[in,out]	table		target table
@param[in]	table_share	table definition */
void
innobase_parse_hint_from_comment(
	THD*			thd,
	dict_table_t*		table,
	const TABLE_SHARE*	table_share)
{
	unsigned merge_threshold_table;
	unsigned merge_threshold_index[MAX_KEY];
	bool	is_found[MAX_KEY];

	if (table_share->comment.str != NULL) {
		merge_threshold_table
			= innobase_parse_merge_threshold(
				thd, table_share->comment.str);
	} else {
		merge_threshold_table = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	}

	if (merge_threshold_table == 0) {
		merge_threshold_table = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	}

	for (uint i = 0; i < table_share->keys; i++) {
		KEY*	key_info = &table_share->key_info[i];

		ut_ad(i < sizeof(merge_threshold_index)
			  / sizeof(merge_threshold_index[0]));

		if (key_info->flags & HA_USES_COMMENT
		    && key_info->comment.str != NULL) {
			merge_threshold_index[i]
				= innobase_parse_merge_threshold(
					thd, key_info->comment.str);
		} else {
			merge_threshold_index[i] = merge_threshold_table;
		}

		if (merge_threshold_index[i] == 0) {
			merge_threshold_index[i] = merge_threshold_table;
		}
	}

	/* update SYS_INDEX table */
	if (!table->is_temporary()) {
		for (uint i = 0; i < table_share->keys; i++) {
			is_found[i] = false;
		}

		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {

			if (dict_index_is_auto_gen_clust(index)) {

				/* GEN_CLUST_INDEX should use
				merge_threshold_table */
				dict_index_set_merge_threshold(
					index, merge_threshold_table);
				continue;
			}

			for (uint i = 0; i < table_share->keys; i++) {
				if (is_found[i]) {
					continue;
				}

				KEY*	key_info = &table_share->key_info[i];

				if (innobase_strcasecmp(
					index->name, key_info->name.str) == 0) {

					dict_index_set_merge_threshold(
						index,
						merge_threshold_index[i]);
					is_found[i] = true;
					break;
				}
			}
		}
	}

	for (uint i = 0; i < table_share->keys; i++) {
		is_found[i] = false;
	}

	/* update in memory */
	for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
	     index != NULL;
	     index = UT_LIST_GET_NEXT(indexes, index)) {

		if (dict_index_is_auto_gen_clust(index)) {

			/* GEN_CLUST_INDEX should use merge_threshold_table */

			/* x-lock index is needed to exclude concurrent
			pessimistic tree operations */
			rw_lock_x_lock(dict_index_get_lock(index));
			index->merge_threshold = merge_threshold_table
				& ((1U << 6) - 1);
			rw_lock_x_unlock(dict_index_get_lock(index));

			continue;
		}

		for (uint i = 0; i < table_share->keys; i++) {
			if (is_found[i]) {
				continue;
			}

			KEY*	key_info = &table_share->key_info[i];

			if (innobase_strcasecmp(
				index->name, key_info->name.str) == 0) {

				/* x-lock index is needed to exclude concurrent
				pessimistic tree operations */
				rw_lock_x_lock(dict_index_get_lock(index));
				index->merge_threshold
					= merge_threshold_index[i]
					& ((1U << 6) - 1);
				rw_lock_x_unlock(dict_index_get_lock(index));
				is_found[i] = true;

				break;
			}
		}
	}
}

/** Set m_use_* flags. */
void
create_table_info_t::set_tablespace_type(
	bool	table_being_altered_is_file_per_table)
{
	/** Allow file_per_table for this table either because:
	1) the setting innodb_file_per_table=on,
	2) the table being altered is currently file_per_table */
	m_allow_file_per_table =
		m_innodb_file_per_table
		|| table_being_altered_is_file_per_table;

	/* Ignore the current innodb-file-per-table setting if we are
	creating a temporary table. */
	m_use_file_per_table = m_allow_file_per_table
		&& !m_create_info->tmp_table();

	/* DATA DIRECTORY must have m_use_file_per_table but cannot be
	used with TEMPORARY tables. */
	m_use_data_dir =
		m_use_file_per_table
		&& (m_create_info->data_file_name != NULL)
		&& (m_create_info->data_file_name[0] != '\0');
}

/** Initialize the create_table_info_t object.
@return error number */
int
create_table_info_t::initialize()
{
	DBUG_ENTER("create_table_info_t::initialize");

	ut_ad(m_thd != NULL);
	ut_ad(m_create_info != NULL);

	if (m_form->s->fields > REC_MAX_N_USER_FIELDS) {
		DBUG_RETURN(HA_ERR_TOO_MANY_FIELDS);
	}

	/* Check for name conflicts (with reserved name) for
	any user indices to be created. */
	if (innobase_index_name_is_reserved(m_thd, m_form->key_info,
					    m_form->s->keys)) {
		DBUG_RETURN(HA_ERR_WRONG_INDEX);
	}

	/* Get the transaction associated with the current thd, or create one
	if not yet created */

	check_trx_exists(m_thd);

	DBUG_RETURN(0);
}


/** Check if a virtual column is part of a fulltext or spatial index. */
bool
create_table_info_t::gcols_in_fulltext_or_spatial()
{
	for (ulint i = 0; i < m_form->s->keys; i++) {
		const KEY*	key = m_form->key_info + i;
		if (!(key->flags & (HA_SPATIAL | HA_FULLTEXT))) {
			continue;
		}
		for (ulint j = 0; j < key->user_defined_key_parts; j++) {
			/* We do not support special (Fulltext or
			Spatial) index on virtual columns */
			if (!key->key_part[j].field->stored_in_db()) {
				my_error(ER_UNSUPPORTED_ACTION_ON_GENERATED_COLUMN, MYF(0));
				return true;
			}
		}
	}
	return false;
}


/** Prepare to create a new table to an InnoDB database.
@param[in]	name	Table name
@return error number */
int create_table_info_t::prepare_create_table(const char* name, bool strict)
{
	DBUG_ENTER("prepare_create_table");

	ut_ad(m_thd != NULL);
	ut_ad(m_create_info != NULL);

	set_tablespace_type(false);

	normalize_table_name(m_table_name, name);

	/* Validate table options not handled by the SQL-parser */
	if (check_table_options()) {
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	/* Validate the create options if innodb_strict_mode is set.
	Do not use the regular message for ER_ILLEGAL_HA_CREATE_OPTION
	because InnoDB might actually support the option, but not under
	the current conditions.  The messages revealing the specific
	problems are reported inside this function. */
	if (strict && create_options_are_invalid()) {
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	/* Create the table flags and flags2 */
	if (!innobase_table_flags()) {
		DBUG_RETURN(HA_WRONG_CREATE_OPTION);
	}

	if (high_level_read_only) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	if (gcols_in_fulltext_or_spatial()) {
		DBUG_RETURN(HA_ERR_UNSUPPORTED);
	}

	for (uint i = 0; i < m_form->s->keys; i++) {
		const size_t max_field_len
		    = DICT_MAX_FIELD_LEN_BY_FORMAT_FLAG(m_flags);
		const KEY& key = m_form->key_info[i];

		if (key.algorithm == HA_KEY_ALG_FULLTEXT) {
			continue;
		}

		if (too_big_key_part_length(max_field_len, key)) {
			DBUG_RETURN(convert_error_code_to_mysql(
			    DB_TOO_BIG_INDEX_COL, m_flags, NULL));
		}
	}

	DBUG_RETURN(parse_table_name(name));
}

/** Push warning message to SQL-layer based on foreign key constraint index
match error.
@param[in]	trx		Current transaction
@param[in]	operation	Operation ("Create" or "Alter")
@param[in]	create_name	Table name as specified in SQL
@param[in]	columns		Foreign key column names array
@param[in]	index_error 	Index error code
@param[in]	err_col	  	Column where error happened
@param[in]	err_index  	Index where error happened
@param[in]	table	  	Table object */
static void
foreign_push_index_error(trx_t* trx, const char* operation,
			 const char* create_name, const char* fk_text,
			 const char** columns, fkerr_t index_error,
			 ulint err_col, dict_index_t* err_index,
			 dict_table_t* table)
{
	switch (index_error) {
	case FK_SUCCESS:
		break;
	case FK_INDEX_NOT_FOUND:
		ib_foreign_warn(trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
				"%s table %s with foreign key %s constraint"
				" failed. There is no index in the referenced"
				" table where the referenced columns appear"
				" as the first columns.",
				operation, create_name, fk_text);
		return;
	case FK_IS_PREFIX_INDEX:
		ib_foreign_warn(
			trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
			"%s table %s with foreign key %s constraint"
			" failed. There is only prefix index in the referenced"
			" table where the referenced columns appear"
			" as the first columns.",
			operation, create_name, fk_text);
		return;
	case FK_COL_NOT_NULL:
		ib_foreign_warn(
			trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
			"%s table %s with foreign key %s constraint"
			" failed. You have defined a SET NULL condition but "
			"column '%s' on index is defined as NOT NULL.",
			operation, create_name, fk_text, columns[err_col]);
		return;
	case FK_COLS_NOT_EQUAL:
		dict_field_t* field;
		const char*   col_name;
		field = dict_index_get_nth_field(err_index, err_col);

		col_name = field->col->is_virtual()
				   ? "(null)"
				   : dict_table_get_col_name(
					   table, dict_col_get_no(field->col));
		ib_foreign_warn(
			trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
			"%s table %s with foreign key %s constraint"
			" failed. Field type or character set for column '%s' "
			"does not mach referenced column '%s'.",
			operation, create_name, fk_text, columns[err_col],
			col_name);
		return;
	}
	DBUG_ASSERT("unknown error" == 0);
}

/** Find column or virtual column in table by its name.
@param[in]	table	Table where column is searched
@param[in]	name	Name to search for
@retval		true	if found
@retval		false	if not found */
static bool
find_col(dict_table_t* table, const char** name)
{
	ulint i;
	for (i = 0; i < dict_table_get_n_cols(table); i++) {

		const char* col_name = dict_table_get_col_name(table, i);

		if (0 == innobase_strcasecmp(col_name, *name)) {
			/* Found */
			strcpy((char*)*name, col_name);
			return true;
		}
	}

	for (i = 0; i < dict_table_get_n_v_cols(table); i++) {

		const char* col_name = dict_table_get_v_col_name(table, i);

		if (0 == innobase_strcasecmp(col_name, *name)) {
			/* Found */
			strcpy((char*)*name, col_name);
			return true;
		}
	}
	return false;
}

/** Foreign key printer for error messages. Prints FK name if it exists or
key part list in the form (col1, col2, col3, ...) */
class key_text
{
	static const size_t MAX_TEXT = 48;
	char		    buf[MAX_TEXT + 1];

public:
	key_text(Key* key)
	{
		char* ptr = buf;
		if (key->name.str) {
			size_t len = std::min(key->name.length, MAX_TEXT - 2);
			*(ptr++)   = '`';
			memcpy(ptr, key->name.str, len);
			ptr	  += len;
			*(ptr++)   = '`';
			*ptr	   = '\0';
			return;
		}
		*(ptr++)  = '(';
		List_iterator_fast<Key_part_spec> it(key->columns);
		while (Key_part_spec* k = it++) {
			/* 3 is etc continuation ("...");
			   2 is comma separator (", ") in case of next exists;
			   1 is terminating ')' */
			if (MAX_TEXT - (size_t)(ptr - buf)
				>= (it.peek() ? 3 + 2 + 1 : 3 + 1)
				+ k->field_name.length) {
				memcpy(ptr, k->field_name.str,
				       k->field_name.length);
				ptr += k->field_name.length;
				if (it.peek()) {
					*(ptr++) = ',';
					*(ptr++) = ' ';
				}
			} else {
				ut_ad((size_t)(ptr - buf) <= MAX_TEXT - 4);
				memcpy(ptr, "...", 3);
				ptr += 3;
				break;
			}
		}
		*(ptr++) = ')';
		*ptr 	 = '\0';
	}
	const char* str() { return buf; }
};

/** Create InnoDB foreign keys from MySQL alter_info. Collect all
dict_foreign_t items into local_fk_set and then add into system table.
@return		DB_SUCCESS or specific error code */
dberr_t
create_table_info_t::create_foreign_keys()
{
	dict_foreign_set      local_fk_set;
	dict_foreign_set_free local_fk_set_free(local_fk_set);
	dberr_t		      error;
	ulint		      number	      = 1;
	static const unsigned MAX_COLS_PER_FK = 500;
	const char*	      column_names[MAX_COLS_PER_FK];
	const char*	      ref_column_names[MAX_COLS_PER_FK];
	char		      create_name[MAX_DATABASE_NAME_LEN + 1 +
					  MAX_TABLE_NAME_LEN + 1];
	dict_index_t*	      index	  = NULL;
	fkerr_t		      index_error = FK_SUCCESS;
	dict_index_t*	      err_index	  = NULL;
	ulint		      err_col;
	const bool	      tmp_table = m_flags2 & DICT_TF2_TEMPORARY;
	const CHARSET_INFO*   cs	= thd_charset(m_thd);
	const char*	      operation = "Create ";
	const char*	      name	= m_table_name;

	enum_sql_command sqlcom = enum_sql_command(thd_sql_command(m_thd));

	if (sqlcom == SQLCOM_ALTER_TABLE) {
		dict_table_t* table_to_alter;
		mem_heap_t*   heap = mem_heap_create(10000);
		ulint	      highest_id_so_far;
		char*	      n = dict_get_referenced_table(
			name, LEX_STRING_WITH_LEN(m_form->s->db),
			LEX_STRING_WITH_LEN(m_form->s->table_name),
			&table_to_alter, heap, cs);

		/* Starting from 4.0.18 and 4.1.2, we generate foreign key id's
		in the format databasename/tablename_ibfk_[number], where
		[number] is local to the table; look for the highest [number]
		for table_to_alter, so that we can assign to new constraints
		higher numbers. */

		/* If we are altering a temporary table, the table name after
		ALTER TABLE does not correspond to the internal table name, and
		table_to_alter is NULL. TODO: should we fix this somehow? */

		if (table_to_alter) {
			n		  = table_to_alter->name.m_name;
			highest_id_so_far = dict_table_get_highest_foreign_id(
				table_to_alter);
		} else {
			highest_id_so_far = 0;
		}

		char* bufend = innobase_convert_name(
			create_name, sizeof create_name, n, strlen(n), m_thd);
		create_name[bufend - create_name] = '\0';
		number				  = highest_id_so_far + 1;
		mem_heap_free(heap);
		operation = "Alter ";
	} else if (strstr(name, "#P#") || strstr(name, "#p#")) {
		/* Partitioned table */
		create_name[0] = '\0';
	} else {
		char* bufend = innobase_convert_name(create_name,
						     sizeof create_name,
						     name,
						     strlen(name), m_thd);
		create_name[bufend - create_name] = '\0';
	}

	Alter_info* alter_info = m_create_info->alter_info;
	ut_ad(alter_info);
	List_iterator_fast<Key> key_it(alter_info->key_list);

	dict_table_t* table = dict_table_get_low(name);
	if (!table) {
		ib_foreign_warn(m_trx, DB_CANNOT_ADD_CONSTRAINT, create_name,
				"%s table %s foreign key constraint"
				" failed. Table not found.",
				operation, create_name);

		return (DB_CANNOT_ADD_CONSTRAINT);
	}

	while (Key* key = key_it++) {
		if (key->type != Key::FOREIGN_KEY)
			continue;

		if (tmp_table) {
			ib_foreign_warn(m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table `%s`.`%s` with foreign key "
					"constraint failed. "
					"Temporary tables can't have "
					"foreign key constraints.",
					operation, m_form->s->db.str,
					m_form->s->table_name.str);

			return (DB_CANNOT_ADD_CONSTRAINT);
		} else if (!*create_name) {
			ut_ad("should be unreachable" == 0);
			return DB_CANNOT_ADD_CONSTRAINT;
		}

		Foreign_key*   fk = static_cast<Foreign_key*>(key);
		Key_part_spec* col;
		bool	       success;

		dict_foreign_t* foreign = dict_mem_foreign_create();
		if (!foreign) {
			return (DB_OUT_OF_MEMORY);
		}

		List_iterator_fast<Key_part_spec> col_it(fk->columns);
		unsigned			  i = 0, j = 0;
		while ((col = col_it++)) {
			column_names[i] = mem_heap_strdupl(
				foreign->heap, col->field_name.str,
				col->field_name.length);
			success = find_col(table, column_names + i);
			if (!success) {
				key_text k(fk);
				ib_foreign_warn(
					m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table %s foreign key %s constraint"
					" failed. Column %s was not found.",
					operation, create_name, k.str(),
					column_names[i]);
				dict_foreign_free(foreign);
				return (DB_CANNOT_ADD_CONSTRAINT);
			}
			++i;
			if (i >= MAX_COLS_PER_FK) {
				key_text k(fk);
				ib_foreign_warn(
					m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table %s foreign key %s constraint"
					" failed. Too many columns: %u (%u "
					"allowed).",
					operation, create_name, k.str(), i,
					MAX_COLS_PER_FK);
				dict_foreign_free(foreign);
				return (DB_CANNOT_ADD_CONSTRAINT);
			}
		}

		index = dict_foreign_find_index(
			table, NULL, column_names, i, NULL, TRUE, FALSE,
			&index_error, &err_col, &err_index);

		if (!index) {
			key_text k(fk);
			foreign_push_index_error(m_trx, operation, create_name,
						 k.str(), column_names,
						 index_error, err_col,
						 err_index, table);
			dict_foreign_free(foreign);
			return (DB_CANNOT_ADD_CONSTRAINT);
		}

		if (fk->constraint_name.str) {
			ulint db_len;

			/* Catenate 'databasename/' to the constraint name
			specified by the user: we conceive the constraint as
			belonging to the same MySQL 'database' as the table
			itself. We store the name to foreign->id. */

			db_len = dict_get_db_name_len(table->name.m_name);

			foreign->id = static_cast<char*>(mem_heap_alloc(
				foreign->heap,
				db_len + fk->constraint_name.length + 2));

			memcpy(foreign->id, table->name.m_name, db_len);
			foreign->id[db_len] = '/';
			strcpy(foreign->id + db_len + 1,
			       fk->constraint_name.str);
		}

		if (foreign->id == NULL) {
			error = dict_create_add_foreign_id(
				&number, table->name.m_name, foreign);
			if (error != DB_SUCCESS) {
				dict_foreign_free(foreign);
				return (error);
			}
		}

		std::pair<dict_foreign_set::iterator, bool> ret
			= local_fk_set.insert(foreign);

		if (!ret.second) {
			/* A duplicate foreign key name has been found */
			dict_foreign_free(foreign);
			return (DB_CANNOT_ADD_CONSTRAINT);
		}

		foreign->foreign_table = table;
		foreign->foreign_table_name
			= mem_heap_strdup(foreign->heap, table->name.m_name);
		if (!foreign->foreign_table_name) {
			return (DB_OUT_OF_MEMORY);
		}

		dict_mem_foreign_table_name_lookup_set(foreign, TRUE);

		foreign->foreign_index = index;
		foreign->n_fields      = i & dict_index_t::MAX_N_FIELDS;

		foreign->foreign_col_names = static_cast<const char**>(
			mem_heap_alloc(foreign->heap, i * sizeof(void*)));
		if (!foreign->foreign_col_names) {
			return (DB_OUT_OF_MEMORY);
		}

		memcpy(foreign->foreign_col_names, column_names,
		       i * sizeof(void*));

		foreign->referenced_table_name = dict_get_referenced_table(
			name, LEX_STRING_WITH_LEN(fk->ref_db),
			LEX_STRING_WITH_LEN(fk->ref_table),
			&foreign->referenced_table, foreign->heap, cs);

		if (!foreign->referenced_table_name) {
			return (DB_OUT_OF_MEMORY);
		}

		if (!foreign->referenced_table && m_trx->check_foreigns) {
			char  buf[MAX_TABLE_NAME_LEN + 1] = "";
			char* bufend;

			bufend = innobase_convert_name(
				buf, MAX_TABLE_NAME_LEN,
				foreign->referenced_table_name,
				strlen(foreign->referenced_table_name), m_thd);
			buf[bufend - buf] = '\0';
			key_text k(fk);
			ib_foreign_warn(m_trx, DB_CANNOT_ADD_CONSTRAINT,
					create_name,
					"%s table %s with foreign key %s "
					"constraint failed. Referenced table "
					"%s not found in the data dictionary.",
					operation, create_name, k.str(), buf);
			return (DB_CANNOT_ADD_CONSTRAINT);
		}

		/* Don't allow foreign keys on partitioned tables yet. */
		if (foreign->referenced_table
		    && dict_table_is_partition(foreign->referenced_table)) {
			/* How could one make a referenced table to be a
			 * partition? */
			ut_ad(0);
			my_error(ER_FEATURE_NOT_SUPPORTED_WITH_PARTITIONING,
				 MYF(0), "FOREIGN KEY");
			return (DB_CANNOT_ADD_CONSTRAINT);
		}

		col_it.init(fk->ref_columns);
		while ((col = col_it++)) {
			ref_column_names[j] = mem_heap_strdupl(
				foreign->heap, col->field_name.str,
				col->field_name.length);
			if (foreign->referenced_table) {
				success = find_col(foreign->referenced_table,
						   ref_column_names + j);
				if (!success) {
					key_text k(fk);
					ib_foreign_warn(
						m_trx,
						DB_CANNOT_ADD_CONSTRAINT,
						create_name,
						"%s table %s foreign key %s "
						"constraint failed. "
						"Column %s was not found.",
						operation, create_name,
						k.str(), ref_column_names[j]);

					return (DB_CANNOT_ADD_CONSTRAINT);
				}
			}
			++j;
		}
		/* See ER_WRONG_FK_DEF in mysql_prepare_create_table() */
		ut_ad(i == j);

		/* Try to find an index which contains the columns as the first
		fields and in the right order, and the types are the same as in
		foreign->foreign_index */

		if (foreign->referenced_table) {
			index = dict_foreign_find_index(
				foreign->referenced_table, NULL,
				ref_column_names, i, foreign->foreign_index,
				TRUE, FALSE, &index_error, &err_col,
				&err_index);

			if (!index) {
				key_text k(fk);
				foreign_push_index_error(
					m_trx, operation, create_name, k.str(),
					column_names, index_error, err_col,
					err_index, foreign->referenced_table);

				return (DB_CANNOT_ADD_CONSTRAINT);
			}
		} else {
			ut_a(m_trx->check_foreigns == FALSE);
			index = NULL;
		}

		foreign->referenced_index = index;
		dict_mem_referenced_table_name_lookup_set(foreign, TRUE);

		foreign->referenced_col_names = static_cast<const char**>(
			mem_heap_alloc(foreign->heap, i * sizeof(void*)));
		if (!foreign->referenced_col_names) {
			return (DB_OUT_OF_MEMORY);
		}

		memcpy(foreign->referenced_col_names, ref_column_names,
		       i * sizeof(void*));

		if (fk->delete_opt == FK_OPTION_SET_NULL
		    || fk->update_opt == FK_OPTION_SET_NULL) {
			for (j = 0; j < foreign->n_fields; j++) {
				if ((dict_index_get_nth_col(
					     foreign->foreign_index, j)
					     ->prtype)
				    & DATA_NOT_NULL) {
					const dict_col_t* col
						= dict_index_get_nth_col(
							foreign->foreign_index,
							j);
					const char* col_name
						= dict_table_get_col_name(
							foreign->foreign_index
								->table,
							dict_col_get_no(col));

					/* It is not sensible to define SET
					NULL
					if the column is not allowed to be
					NULL! */
					key_text k(fk);
					ib_foreign_warn(
						m_trx,
						DB_CANNOT_ADD_CONSTRAINT,
						create_name,
						"%s table %s with foreign key "
						"%s constraint failed. You have"
						" defined a SET NULL condition "
						"but column '%s' is defined as "
						"NOT NULL.",
						operation, create_name,
						k.str(), col_name);

					return (DB_CANNOT_ADD_CONSTRAINT);
				}
			}
		}

		switch (fk->delete_opt) {
		case FK_OPTION_UNDEF:
		case FK_OPTION_RESTRICT:
			break;
		case FK_OPTION_CASCADE:
			foreign->type |= DICT_FOREIGN_ON_DELETE_CASCADE;
			break;
		case FK_OPTION_SET_NULL:
			foreign->type |= DICT_FOREIGN_ON_DELETE_SET_NULL;
			break;
		case FK_OPTION_NO_ACTION:
			foreign->type |= DICT_FOREIGN_ON_DELETE_NO_ACTION;
			break;
		case FK_OPTION_SET_DEFAULT:
			// TODO: MDEV-10393 Foreign keys SET DEFAULT action
			break;
		default:
			ut_ad(0);
			break;
		}

		switch (fk->update_opt) {
		case FK_OPTION_UNDEF:
		case FK_OPTION_RESTRICT:
			break;
		case FK_OPTION_CASCADE:
			foreign->type |= DICT_FOREIGN_ON_UPDATE_CASCADE;
			break;
		case FK_OPTION_SET_NULL:
			foreign->type |= DICT_FOREIGN_ON_UPDATE_SET_NULL;
			break;
		case FK_OPTION_NO_ACTION:
			foreign->type |= DICT_FOREIGN_ON_UPDATE_NO_ACTION;
			break;
		case FK_OPTION_SET_DEFAULT:
			// TODO: MDEV-10393 Foreign keys SET DEFAULT action
			break;
		default:
			ut_ad(0);
			break;
		}
	}

	if (dict_foreigns_has_s_base_col(local_fk_set, table)) {
		return (DB_NO_FK_ON_S_BASE_COL);
	}

	/**********************************************************/
	/* The following call adds the foreign key constraints
	to the data dictionary system tables on disk */
	m_trx->op_info = "adding foreign keys";

	trx_start_if_not_started_xa(m_trx, true);

	trx_set_dict_operation(m_trx, TRX_DICT_OP_TABLE);

	error = dict_create_add_foreigns_to_dictionary(local_fk_set, table,
						       m_trx);

	if (error == DB_SUCCESS) {

		table->foreign_set.insert(local_fk_set.begin(),
					  local_fk_set.end());
		std::for_each(local_fk_set.begin(), local_fk_set.end(),
			      dict_foreign_add_to_referenced_table());
		local_fk_set.clear();

		dict_mem_table_fill_foreign_vcol_set(table);
	}
	return (error);
}

/** Create the internal innodb table.
@param create_fk	whether to add FOREIGN KEY constraints */
int create_table_info_t::create_table(bool create_fk)
{
	int		error;
	int		primary_key_no;
	uint		i;

	DBUG_ENTER("create_table");

	/* Look for a primary key */
	primary_key_no = (m_form->s->primary_key != MAX_KEY ?
			  (int) m_form->s->primary_key : -1);

	/* Our function innobase_get_mysql_key_number_for_index assumes
	the primary key is always number 0, if it exists */
	ut_a(primary_key_no == -1 || primary_key_no == 0);

	error = create_table_def();

	if (error) {
		DBUG_RETURN(error);
	}

	DBUG_ASSERT(m_drop_before_rollback
		    == !(m_flags2 & DICT_TF2_TEMPORARY));

	/* Create the keys */

	if (m_form->s->keys == 0 || primary_key_no == -1) {
		/* Create an index which is used as the clustered index;
		order the rows by their row id which is internally generated
		by InnoDB */
		ulint flags = m_table->flags;
		dict_index_t* index = dict_mem_index_create(
			m_table, innobase_index_reserve_name,
			DICT_CLUSTERED, 0);
		error = convert_error_code_to_mysql(
			row_create_index_for_mysql(index, m_trx, NULL),
			flags, m_thd);
		if (error) {
			DBUG_RETURN(error);
		}
	}

	if (primary_key_no != -1) {
		/* In InnoDB the clustered index must always be created
		first */
		if ((error = create_index(m_trx, m_form, m_table,
					  (uint) primary_key_no))) {
			DBUG_RETURN(error);
		}
	}

	/* Create the ancillary tables that are common to all FTS indexes on
	this table. */
	if (m_flags2 & DICT_TF2_FTS) {
		fts_doc_id_index_enum	ret;

		/* Check whether there already exists FTS_DOC_ID_INDEX */
		ret = innobase_fts_check_doc_id_index_in_def(
			m_form->s->keys, m_form->key_info);

		switch (ret) {
		case FTS_INCORRECT_DOC_ID_INDEX:
			push_warning_printf(m_thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_NAME_FOR_INDEX,
					    " InnoDB: Index name %s is reserved"
					    " for the unique index on"
					    " FTS_DOC_ID column for FTS"
					    " Document ID indexing"
					    " on table %s. Please check"
					    " the index definition to"
					    " make sure it is of correct"
					    " type\n",
					    FTS_DOC_ID_INDEX_NAME,
					    m_table->name.m_name);

			if (m_table->fts) {
				fts_free(m_table);
			}

			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
				 FTS_DOC_ID_INDEX_NAME);
			DBUG_RETURN(-1);
		case FTS_EXIST_DOC_ID_INDEX:
		case FTS_NOT_EXIST_DOC_ID_INDEX:
			break;
		}

		dberr_t	err = fts_create_common_tables(
			m_trx, m_table,
			(ret == FTS_EXIST_DOC_ID_INDEX));

		error = convert_error_code_to_mysql(err, 0, NULL);

		if (error) {
			DBUG_RETURN(error);
		}
	}

	for (i = 0; i < m_form->s->keys; i++) {
		if (i != uint(primary_key_no)
		    && (error = create_index(m_trx, m_form, m_table, i))) {
			DBUG_RETURN(error);
		}
	}

	/* Cache all the FTS indexes on this table in the FTS specific
	structure. They are used for FTS indexed column update handling. */
	if (m_flags2 & DICT_TF2_FTS) {
		fts_t*          fts = m_table->fts;

		ut_a(fts != NULL);

		dict_table_get_all_fts_indexes(m_table, fts->indexes);
	}

	dberr_t err = create_fk ? create_foreign_keys() : DB_SUCCESS;

	if (err == DB_SUCCESS) {
		/* Check that also referencing constraints are ok */
		dict_names_t	fk_tables;
		err = dict_load_foreigns(m_table_name, NULL,
						false, true,
						DICT_ERR_IGNORE_NONE,
						fk_tables);
		while (err == DB_SUCCESS && !fk_tables.empty()) {
			dict_load_table(fk_tables.front(),
					DICT_ERR_IGNORE_NONE);
			fk_tables.pop_front();
		}
	}

	switch (err) {
	case DB_PARENT_NO_INDEX:
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_ERR_CANNOT_ADD_FOREIGN,
			"Create table '%s' with foreign key constraint"
			" failed. There is no index in the referenced"
			" table where the referenced columns appear"
			" as the first columns.\n", m_table_name);
		break;

	case DB_CHILD_NO_INDEX:
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_ERR_CANNOT_ADD_FOREIGN,
			"Create table '%s' with foreign key constraint"
			" failed. There is no index in the referencing"
			" table where referencing columns appear"
			" as the first columns.\n", m_table_name);
		break;
	case DB_NO_FK_ON_S_BASE_COL:
		push_warning_printf(
			m_thd, Sql_condition::WARN_LEVEL_WARN,
			HA_ERR_CANNOT_ADD_FOREIGN,
			"Create table '%s' with foreign key constraint"
			" failed. Cannot add foreign key constraint"
			" placed on the base column of stored"
			" column. \n",
			m_table_name);
	default:
		break;
	}

	if (err != DB_SUCCESS) {
		DBUG_RETURN(convert_error_code_to_mysql(
					err, m_flags, NULL));
	}

	/* In TRUNCATE TABLE, we will merely warn about the maximum
	row size being too large. */
	if (!row_size_is_acceptable(*m_table, create_fk)) {
		DBUG_RETURN(convert_error_code_to_mysql(
			    DB_TOO_BIG_RECORD, m_flags, NULL));
	}

	DBUG_RETURN(0);
}

bool create_table_info_t::row_size_is_acceptable(
  const dict_table_t &table, bool strict) const
{
  for (dict_index_t *index= dict_table_get_first_index(&table); index;
       index= dict_table_get_next_index(index))
    if (!row_size_is_acceptable(*index, strict))
      return false;
  return true;
}

/* FIXME: row size check has some flaws and should be improved */
dict_index_t::record_size_info_t dict_index_t::record_size_info() const
{
  ut_ad(!(type & DICT_FTS));

  /* maximum allowed size of a node pointer record */
  ulint page_ptr_max;
  const bool comp= table->not_redundant();
  /* table->space == NULL after DISCARD TABLESPACE */
  const ulint zip_size= dict_tf_get_zip_size(table->flags);
  record_size_info_t result;

  if (zip_size && zip_size < srv_page_size)
  {
    /* On a ROW_FORMAT=COMPRESSED page, two records must fit in the
    uncompressed page modification log. On compressed pages
    with size.physical() == univ_page_size.physical(),
    this limit will never be reached. */
    ut_ad(comp);
    /* The maximum allowed record size is the size of
    an empty page, minus a byte for recoding the heap
    number in the page modification log.  The maximum
    allowed node pointer size is half that. */
    result.max_leaf_size= page_zip_empty_size(n_fields, zip_size);
    if (result.max_leaf_size)
    {
      result.max_leaf_size--;
    }
    page_ptr_max= result.max_leaf_size / 2;
    /* On a compressed page, there is a two-byte entry in
    the dense page directory for every record.  But there
    is no record header. */
    result.shortest_size= 2;
  }
  else
  {
    /* The maximum allowed record size is half a B-tree
    page(16k for 64k page size).  No additional sparse
    page directory entry will be generated for the first
    few user records. */
    result.max_leaf_size= (comp || srv_page_size < UNIV_PAGE_SIZE_MAX)
                              ? page_get_free_space_of_empty(comp) / 2
                              : REDUNDANT_REC_MAX_DATA_SIZE;

    page_ptr_max= result.max_leaf_size;
    /* Each record has a header. */
    result.shortest_size= comp ? REC_N_NEW_EXTRA_BYTES : REC_N_OLD_EXTRA_BYTES;
  }

  if (comp)
  {
    /* Include the "null" flags in the
    maximum possible record size. */
    result.shortest_size+= UT_BITS_IN_BYTES(n_nullable);
  }
  else
  {
    /* For each column, include a 2-byte offset and a
    "null" flag.  The 1-byte format is only used in short
    records that do not contain externally stored columns.
    Such records could never exceed the page limit, even
    when using the 2-byte format. */
    result.shortest_size+= 2 * n_fields;
  }

  const ulint max_local_len= table->get_overflow_field_local_len();

  /* Compute the maximum possible record size. */
  for (unsigned i= 0; i < n_fields; i++)
  {
    const dict_field_t &f= fields[i];
    const dict_col_t &col= *f.col;

    /* In dtuple_convert_big_rec(), variable-length columns
    that are longer than BTR_EXTERN_LOCAL_STORED_MAX_SIZE
    may be chosen for external storage.

    Fixed-length columns, and all columns of secondary
    index records are always stored inline. */

    /* Determine the maximum length of the index field.
    The field_ext_max_size should be computed as the worst
    case in rec_get_converted_size_comp() for
    REC_STATUS_ORDINARY records. */

    size_t field_max_size= dict_col_get_fixed_size(&col, comp);
    if (field_max_size && f.fixed_len != 0)
    {
      /* dict_index_add_col() should guarantee this */
      ut_ad(!f.prefix_len || f.fixed_len == f.prefix_len);
      /* Fixed lengths are not encoded
      in ROW_FORMAT=COMPACT. */
      goto add_field_size;
    }

    field_max_size= dict_col_get_max_size(&col);

    if (f.prefix_len)
    {
      if (f.prefix_len < field_max_size)
      {
        field_max_size= f.prefix_len;
      }

      /* those conditions were copied from dtuple_convert_big_rec()*/
    }
    else if (field_max_size > max_local_len &&
             field_max_size > BTR_EXTERN_LOCAL_STORED_MAX_SIZE &&
             DATA_BIG_COL(&col) && dict_index_is_clust(this))
    {

      /* In the worst case, we have a locally stored
      column of BTR_EXTERN_LOCAL_STORED_MAX_SIZE bytes.
      The length can be stored in one byte.  If the
      column were stored externally, the lengths in
      the clustered index page would be
      BTR_EXTERN_FIELD_REF_SIZE and 2. */
      field_max_size= max_local_len;
    }

    if (comp)
    {
      /* Add the extra size for ROW_FORMAT=COMPACT.
      For ROW_FORMAT=REDUNDANT, these bytes were
      added to result.shortest_size before this loop. */
      result.shortest_size+= field_max_size < 256 ? 1 : 2;
    }
  add_field_size:
    result.shortest_size+= field_max_size;

    /* Check the size limit on leaf pages. */
    if (result.shortest_size >= result.max_leaf_size)
    {
      result.set_too_big(i);
    }

    /* Check the size limit on non-leaf pages.  Records
    stored in non-leaf B-tree pages consist of the unique
    columns of the record (the key columns of the B-tree)
    and a node pointer field.  When we have processed the
    unique columns, result.shortest_size equals the size of the
    node pointer record minus the node pointer column. */
    if (i + 1 == dict_index_get_n_unique_in_tree(this) &&
        result.shortest_size + REC_NODE_PTR_SIZE >= page_ptr_max)
    {
      result.set_too_big(i);
    }
  }

  return result;
}

/** Issue a warning that the row is too big. */
static void ib_warn_row_too_big(THD *thd, const dict_table_t *table)
{
  /* FIXME: this row size check should be improved */
  /* If prefix is true then a 768-byte prefix is stored
  locally for BLOB fields. Refer to dict_table_get_format() */
  const bool prefix= !dict_table_has_atomic_blobs(table);

  const ulint free_space=
      page_get_free_space_of_empty(table->flags & DICT_TF_COMPACT) / 2;

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN, HA_ERR_TO_BIG_ROW,
      "Row size too large (> " ULINTPF "). Changing some columns to TEXT"
      " or BLOB %smay help. In current row format, BLOB prefix of"
      " %d bytes is stored inline.",
      free_space,
      prefix ? "or using ROW_FORMAT=DYNAMIC or ROW_FORMAT=COMPRESSED " : "",
      prefix ? DICT_MAX_FIXED_COL_LEN : 0);
}

bool create_table_info_t::row_size_is_acceptable(
    const dict_index_t &index, bool strict) const
{
  if ((index.type & DICT_FTS) || index.table->is_system_db)
  {
    /* Ignore system tables check because innodb_table_stats
    maximum row size can not fit on 4k page. */
    return true;
  }

  const bool innodb_strict_mode= THDVAR(m_thd, strict_mode);
  dict_index_t::record_size_info_t info= index.record_size_info();

  if (info.row_is_too_big())
  {
    ut_ad(info.get_overrun_size() != 0);
    ut_ad(info.max_leaf_size != 0);

    const size_t idx= info.get_first_overrun_field_index();
    const dict_field_t *field= dict_index_get_nth_field(&index, idx);

    ut_ad((!field->name) == field->col->is_dropped());
    if (innodb_strict_mode || global_system_variables.log_warnings > 2)
    {
      ib::error_or_warn eow(strict && innodb_strict_mode);
      if (field->name)
        eow << "Cannot add field " << field->name << " in table ";
      else
        eow << "Cannot add an instantly dropped column in table ";
      eow << index.table->name << " because after adding it, the row size is "
          << info.get_overrun_size()
          << " which is greater than maximum allowed size ("
          << info.max_leaf_size << " bytes) for a record on index leaf page.";
    }

    if (strict && innodb_strict_mode)
      return false;

    ib_warn_row_too_big(m_thd, index.table);
  }

  return true;
}

/** Update a new table in an InnoDB database.
@return error number */
int
create_table_info_t::create_table_update_dict()
{
	dict_table_t*	innobase_table;

	DBUG_ENTER("create_table_update_dict");

	innobase_table = dict_table_open_on_name(
		m_table_name, FALSE, FALSE, DICT_ERR_IGNORE_NONE);

	DBUG_ASSERT(innobase_table != 0);
	if (innobase_table->fts != NULL) {
		if (innobase_table->fts_doc_id_index == NULL) {
			innobase_table->fts_doc_id_index
				= dict_table_get_index_on_name(
					innobase_table, FTS_DOC_ID_INDEX_NAME);
			DBUG_ASSERT(innobase_table->fts_doc_id_index != NULL);
		} else {
			DBUG_ASSERT(innobase_table->fts_doc_id_index
				    == dict_table_get_index_on_name(
						innobase_table,
						FTS_DOC_ID_INDEX_NAME));
		}
	}

	DBUG_ASSERT((innobase_table->fts == NULL)
		    == (innobase_table->fts_doc_id_index == NULL));

	innobase_copy_frm_flags_from_create_info(innobase_table, m_create_info);

	dict_stats_update(innobase_table, DICT_STATS_EMPTY_TABLE);

	/* Load server stopword into FTS cache */
	if (m_flags2 & DICT_TF2_FTS) {
		if (!innobase_fts_load_stopword(innobase_table, NULL, m_thd)) {
			dict_table_close(innobase_table, FALSE, FALSE);
			m_trx->free();
			DBUG_RETURN(-1);
		}

		mutex_enter(&dict_sys.mutex);
		fts_optimize_add_table(innobase_table);
		mutex_exit(&dict_sys.mutex);
	}

	if (const Field* ai = m_form->found_next_number_field) {
		ut_ad(ai->stored_in_db());

		ib_uint64_t	autoinc = m_create_info->auto_increment_value;

		if (autoinc == 0) {
			autoinc = 1;
		}

		innobase_table->autoinc_mutex.lock();
		dict_table_autoinc_initialize(innobase_table, autoinc);

		if (innobase_table->is_temporary()) {
			/* AUTO_INCREMENT is not persistent for
			TEMPORARY TABLE. Temporary tables are never
			evicted. Keep the counter in memory only. */
		} else {
			const unsigned	col_no = innodb_col_no(ai);

			innobase_table->persistent_autoinc
				= static_cast<uint16_t>(
					dict_table_get_nth_col_pos(
						innobase_table, col_no, NULL)
					+ 1)
				& dict_index_t::MAX_N_FIELDS;

			/* Persist the "last used" value, which
			typically is AUTO_INCREMENT - 1.
			In btr_create(), the value 0 was already written. */
			if (--autoinc) {
				btr_write_autoinc(
					dict_table_get_first_index(
						innobase_table),
					autoinc);
			}
		}

		innobase_table->autoinc_mutex.unlock();
	}

	innobase_parse_hint_from_comment(m_thd, innobase_table, m_form->s);

	dict_table_close(innobase_table, FALSE, FALSE);
	DBUG_RETURN(0);
}

/** Allocate a new trx. */
void
create_table_info_t::allocate_trx()
{
	m_trx = innobase_trx_allocate(m_thd);

	m_trx->will_lock++;
	m_trx->ddl = true;
}

/** Create a new table to an InnoDB database.
@param[in]	name		Table name, format: "db/table_name".
@param[in]	form		Table format; columns and index information.
@param[in]	create_info	Create info (including create statement string).
@param[in]	file_per_table	whether to create .ibd file
@param[in,out]	trx		dictionary transaction, or NULL to create new
@return	0 if success else error number. */
inline int
ha_innobase::create(
	const char*	name,
	TABLE*		form,
	HA_CREATE_INFO*	create_info,
	bool		file_per_table,
	trx_t*		trx)
{
	int		error;
	char		norm_name[FN_REFLEN];	/* {database}/{tablename} */
	char		remote_path[FN_REFLEN];	/* Absolute path of table */

	DBUG_ENTER("ha_innobase::create");

	DBUG_ASSERT(form->s == table_share);
	DBUG_ASSERT(table_share->table_type == TABLE_TYPE_SEQUENCE
		    || table_share->table_type == TABLE_TYPE_NORMAL);

	create_table_info_t	info(ha_thd(),
				     form,
				     create_info,
				     norm_name,
				     remote_path,
				     file_per_table, trx);

	if ((error = info.initialize())
	    || (error = info.prepare_create_table(name, !trx))) {
		if (trx) {
			trx_rollback_for_mysql(trx);
			row_mysql_unlock_data_dictionary(trx);
		}
		DBUG_RETURN(error);
	}

	const bool own_trx = !trx;

	if (own_trx) {
		info.allocate_trx();
		trx = info.trx();
		/* Latch the InnoDB data dictionary exclusively so that no deadlocks
		or lock waits can happen in it during a table create operation.
		Drop table etc. do this latching in row0mysql.cc. */
		row_mysql_lock_data_dictionary(trx);
		DBUG_ASSERT(trx_state_eq(trx, TRX_STATE_NOT_STARTED));
	}

	if ((error = info.create_table(own_trx))) {
		/* Drop the being-created table before rollback,
		so that rollback can possibly rename back a table
		that could have been renamed before the failed creation. */
		if (info.drop_before_rollback()) {
			trx->error_state = DB_SUCCESS;
			row_drop_table_for_mysql(info.table_name(),
						 trx, SQLCOM_TRUNCATE, true,
						 false);
		}
		trx_rollback_for_mysql(trx);
		row_mysql_unlock_data_dictionary(trx);
		if (own_trx) {
			trx->free();
		}
		DBUG_RETURN(error);
	}

	innobase_commit_low(trx);
	row_mysql_unlock_data_dictionary(trx);

	if (own_trx) {
		trx->free();
	}

	/* Flush the log to reduce probability that the .frm files and
	the InnoDB data dictionary get out-of-sync if the user runs
	with innodb_flush_log_at_trx_commit = 0 */
	log_buffer_flush_to_disk();

	ut_ad(!srv_read_only_mode);

	error = info.create_table_update_dict();

	DBUG_RETURN(error);
}

/** Create a new table to an InnoDB database.
@param[in]	name		Table name, format: "db/table_name".
@param[in]	form		Table format; columns and index information.
@param[in]	create_info	Create info (including create statement string).
@return	0 if success else error number. */
int
ha_innobase::create(
	const char*	name,
	TABLE*		form,
	HA_CREATE_INFO*	create_info)
{
	return create(name, form, create_info, srv_file_per_table);
}

/*****************************************************************//**
Discards or imports an InnoDB tablespace.
@return 0 == success, -1 == error */

int
ha_innobase::discard_or_import_tablespace(
/*======================================*/
	my_bool		discard)	/*!< in: TRUE if discard, else import */
{

	DBUG_ENTER("ha_innobase::discard_or_import_tablespace");

	ut_a(m_prebuilt->trx != NULL);
	ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);
	ut_a(m_prebuilt->trx == thd_to_trx(ha_thd()));

	if (is_read_only()) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	if (m_prebuilt->table->is_temporary()) {
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_CANNOT_DISCARD_TEMPORARY_TABLE);

		DBUG_RETURN(HA_ERR_TABLE_NEEDS_UPGRADE);
	}

	if (m_prebuilt->table->space == fil_system.sys_space) {
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLE_IN_SYSTEM_TABLESPACE,
			m_prebuilt->table->name.m_name);

		DBUG_RETURN(HA_ERR_TABLE_NEEDS_UPGRADE);
	}

	trx_start_if_not_started(m_prebuilt->trx, true);

	/* Obtain an exclusive lock on the table. */
	dberr_t	err = row_mysql_lock_table(
		m_prebuilt->trx, m_prebuilt->table, LOCK_X,
		discard ? "setting table lock for DISCARD TABLESPACE"
			: "setting table lock for IMPORT TABLESPACE");

	if (err != DB_SUCCESS) {
		/* unable to lock the table: do nothing */
	} else if (discard) {

		/* Discarding an already discarded tablespace should be an
		idempotent operation. Also, if the .ibd file is missing the
		user may want to set the DISCARD flag in order to IMPORT
		a new tablespace. */

		if (!m_prebuilt->table->is_readable()) {
			ib_senderrf(
				m_prebuilt->trx->mysql_thd,
				IB_LOG_LEVEL_WARN, ER_TABLESPACE_MISSING,
				m_prebuilt->table->name.m_name);
		}

		err = row_discard_tablespace_for_mysql(
			m_prebuilt->table->name.m_name, m_prebuilt->trx);

	} else if (m_prebuilt->table->is_readable()) {
		/* Commit the transaction in order to
		release the table lock. */
		trx_commit_for_mysql(m_prebuilt->trx);

		ib::error() << "Unable to import tablespace "
			<< m_prebuilt->table->name << " because it already"
			" exists.  Please DISCARD the tablespace"
			" before IMPORT.";
		ib_senderrf(
			m_prebuilt->trx->mysql_thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_EXISTS, m_prebuilt->table->name.m_name);

		DBUG_RETURN(HA_ERR_TABLE_EXIST);
	} else {
		err = row_import_for_mysql(m_prebuilt->table, m_prebuilt);

		if (err == DB_SUCCESS) {

			info(HA_STATUS_TIME
			     | HA_STATUS_CONST
			     | HA_STATUS_VARIABLE
			     | HA_STATUS_AUTO);

			fil_crypt_set_encrypt_tables(srv_encrypt_tables);
		}
	}

	/* Commit the transaction in order to release the table lock. */
	trx_commit_for_mysql(m_prebuilt->trx);

	if (discard || err != DB_SUCCESS) {
		DBUG_RETURN(convert_error_code_to_mysql(
				    err, m_prebuilt->table->flags, NULL));
	}

	/* Evict and reload the table definition in order to invoke
	btr_cur_instant_init(). */
	table_id_t id = m_prebuilt->table->id;
	ut_ad(id);
	mutex_enter(&dict_sys.mutex);
	dict_table_close(m_prebuilt->table, TRUE, FALSE);
	dict_sys.remove(m_prebuilt->table);
	m_prebuilt->table = dict_table_open_on_id(id, TRUE,
						  DICT_TABLE_OP_NORMAL);
	mutex_exit(&dict_sys.mutex);
	if (!m_prebuilt->table) {
		err = DB_TABLE_NOT_FOUND;
	} else {
		if (const Field* ai = table->found_next_number_field) {
			initialize_auto_increment(m_prebuilt->table, ai);
		}
		dict_stats_init(m_prebuilt->table);
	}

	if (dict_stats_is_persistent_enabled(m_prebuilt->table)) {
		dberr_t		ret;

		/* Adjust the persistent statistics. */
		ret = dict_stats_update(m_prebuilt->table,
					DICT_STATS_RECALC_PERSISTENT);

		if (ret != DB_SUCCESS) {
			push_warning_printf(
				ha_thd(),
				Sql_condition::WARN_LEVEL_WARN,
				ER_ALTER_INFO,
				"Error updating stats for table '%s'"
				" after table rebuild: %s",
				m_prebuilt->table->name.m_name,
				ut_strerr(ret));
		}
	}

	DBUG_RETURN(0);
}

/**
   @return 1 if frm file exists
   @return 0 if it doesn't exists
*/

static bool frm_file_exists(const char *path)
{
  char buff[FN_REFLEN];
  strxnmov(buff, FN_REFLEN, path, reg_ext, NullS);
  return !access(buff, F_OK);
}


/**
Drops a table from an InnoDB database. Before calling this function,
MySQL calls innobase_commit to commit the transaction of the current user.
Then the current user cannot have locks set on the table. Drop table
operation inside InnoDB will remove all locks any user has on the table
inside InnoDB.
@param[in]	name	table name
@param[in]	sqlcom	SQLCOM_DROP_DB, SQLCOM_TRUNCATE, ...
@return error number */
inline int ha_innobase::delete_table(const char* name, enum_sql_command sqlcom)
{
	dberr_t	err;
	THD*	thd = ha_thd();
	char	norm_name[FN_REFLEN];

	DBUG_ENTER("ha_innobase::delete_table");

	DBUG_EXECUTE_IF(
		"test_normalize_table_name_low",
		test_normalize_table_name_low();
	);
	DBUG_EXECUTE_IF(
		"test_ut_format_name",
		test_ut_format_name();
	);

	/* Strangely, MySQL passes the table name without the '.frm'
	extension, in contrast to ::create */
	normalize_table_name(norm_name, name);

	if (high_level_read_only) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	trx_t*	parent_trx = check_trx_exists(thd);

	/* Remove the to-be-dropped table from the list of modified tables
	by parent_trx. Otherwise we may end up with an orphaned pointer to
	the table object from parent_trx::mod_tables. This could happen in:
	SET AUTOCOMMIT=0;
	CREATE TABLE t (PRIMARY KEY (a)) ENGINE=INNODB SELECT 1 AS a UNION
	ALL SELECT 1 AS a; */
	trx_mod_tables_t::const_iterator	iter;

	for (iter = parent_trx->mod_tables.begin();
	     iter != parent_trx->mod_tables.end();
	     ++iter) {

		dict_table_t*	table_to_drop = iter->first;

		if (strcmp(norm_name, table_to_drop->name.m_name) == 0) {
			parent_trx->mod_tables.erase(table_to_drop);
			break;
		}
	}

	trx_t*	trx = innobase_trx_allocate(thd);

	ulint	name_len = strlen(name);

	ut_a(name_len < 1000);

	/* Either the transaction is already flagged as a locking transaction
	or it hasn't been started yet. */

	ut_a(!trx_is_started(trx) || trx->will_lock > 0);

	/* We are doing a DDL operation. */
	++trx->will_lock;

	/* Drop the table in InnoDB */

	err = row_drop_table_for_mysql(norm_name, trx, sqlcom);

	if (err == DB_TABLE_NOT_FOUND
	    && innobase_get_lower_case_table_names() == 1) {
		char*	is_part = is_partition(norm_name);

		if (is_part) {
			char	par_case_name[FN_REFLEN];

#ifndef __WIN__
			/* Check for the table using lower
			case name, including the partition
			separator "P" */
			strcpy(par_case_name, norm_name);
			innobase_casedn_str(par_case_name);
#else
			/* On Windows platfrom, check
			whether there exists table name in
			system table whose name is
			not being normalized to lower case */
			normalize_table_name_c_low(
				par_case_name, name, FALSE);
#endif
			err = row_drop_table_for_mysql(
				par_case_name, trx, sqlcom);
		}
	}

	if (err == DB_TABLE_NOT_FOUND &&
            frm_file_exists(name))
        {
		/* Test to drop all tables which matches db/tablename + '#'.
		Only partitions can have '#' as non-first character in
		the table name!

		Temporary table names always start with '#', partitions are
		the only 'tables' that can have '#' after the first character
		and table name must have length > 0. User tables cannot have
		'#' since it would be translated to @0023. Therefor this should
		only match partitions. */
		uint	len = (uint) strlen(norm_name);
		ulint	num_partitions;
		ut_a(len < FN_REFLEN);
		norm_name[len] = '#';
		norm_name[len + 1] = 0;
		err = row_drop_database_for_mysql(norm_name, trx,
			&num_partitions);
		norm_name[len] = 0;
		table_name_t tbl_name(norm_name);
		if (num_partitions == 0 && !tbl_name.is_temporary()) {
			ib::error() << "Table " << tbl_name <<
				" does not exist in the InnoDB"
				" internal data dictionary though MariaDB is"
				" trying to drop it. Have you copied the .frm"
				" file of the table to the MariaDB database"
				" directory from another database? "
				<< TROUBLESHOOTING_MSG;
		}
		if (num_partitions == 0) {
			err = DB_TABLE_NOT_FOUND;
		}
	}

	if (err == DB_TABLE_NOT_FOUND
	    && innobase_get_lower_case_table_names() == 1) {
		char*	is_part = is_partition(norm_name);

		if (is_part != NULL) {
			char	par_case_name[FN_REFLEN];

#ifndef _WIN32
			/* Check for the table using lower
			case name, including the partition
			separator "P" */
			strcpy(par_case_name, norm_name);
			innobase_casedn_str(par_case_name);
#else
			/* On Windows platfrom, check
			whether there exists table name in
			system table whose name is
			not being normalized to lower case */
			create_table_info_t::normalize_table_name_low(
				par_case_name, name, FALSE);
#endif /* _WIN32 */
			err = row_drop_table_for_mysql(
				par_case_name, trx, sqlcom, true);
		}
	}

	ut_ad(!srv_read_only_mode);
	/* Flush the log to reduce probability that the .frm files and
	the InnoDB data dictionary get out-of-sync if the user runs
	with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	innobase_commit_low(trx);

	trx->free();

	DBUG_RETURN(convert_error_code_to_mysql(err, 0, NULL));
}

/** Drop an InnoDB table.
@param[in]	name	table name
@return error number */
int ha_innobase::delete_table(const char* name)
{
	enum_sql_command sqlcom = enum_sql_command(thd_sql_command(ha_thd()));
	/* SQLCOM_TRUNCATE should be passed via ha_innobase::truncate() only.

	On client disconnect, when dropping temporary tables, the
	previous sqlcom would not be overwritten.  In such a case, we
	will have thd_kill_level() != NOT_KILLED, !m_prebuilt can
	hold, and sqlcom could be anything, including TRUNCATE.

	The sqlcom only matters for persistent tables; no persistent
	metadata or FOREIGN KEY metadata is kept for temporary
	tables. Therefore, we relax the assertion. If there is a bug
	that slips through this assertion due to !m_prebuilt, the
	worst impact should be that on DROP TABLE of a persistent
	table, FOREIGN KEY constraints will be ignored and their
	metadata will not be removed. */
	DBUG_ASSERT(sqlcom != SQLCOM_TRUNCATE
		    || (thd_kill_level(ha_thd()) != THD_IS_NOT_KILLED
			&& (!m_prebuilt
			    || m_prebuilt->table->is_temporary())));
	return delete_table(name, sqlcom);
}

/** Remove all tables in the named database inside InnoDB.
@param[in]	hton	handlerton from InnoDB
@param[in]	path	Database path; Inside InnoDB the name of the last
directory in the path is used as the database name.
For example, in 'mysql/data/test' the database name is 'test'. */

static
void
innobase_drop_database(
	handlerton*	hton,
	char*		path)
{
	char*	namebuf;

	/* Get the transaction associated with the current thd, or create one
	if not yet created */

	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (high_level_read_only) {
		return;
	}

	THD*	thd = current_thd;

	ulint	len = 0;
	char*	ptr = strend(path) - 2;

	while (ptr >= path && *ptr != '\\' && *ptr != '/') {
		ptr--;
		len++;
	}

	ptr++;
	namebuf = (char*) my_malloc(PSI_INSTRUMENT_ME, (uint) len + 2, MYF(0));

	memcpy(namebuf, ptr, len);
	namebuf[len] = '/';
	namebuf[len + 1] = '\0';

#ifdef	_WIN32
	innobase_casedn_str(namebuf);
#endif /* _WIN32 */

	trx_t*	trx = innobase_trx_allocate(thd);

	/* Either the transaction is already flagged as a locking transaction
	or it hasn't been started yet. */

	ut_a(!trx_is_started(trx) || trx->will_lock > 0);

	/* We are doing a DDL operation. */
	++trx->will_lock;

	ulint	dummy;

	row_drop_database_for_mysql(namebuf, trx, &dummy);

	my_free(namebuf);

	/* Flush the log to reduce probability that the .frm files and
	the InnoDB data dictionary get out-of-sync if the user runs
	with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	innobase_commit_low(trx);

	trx->free();
}

/** Rename an InnoDB table.
@param[in,out]	trx	InnoDB data dictionary transaction
@param[in]	from	old table name
@param[in]	to	new table name
@param[in]	commit	whether to commit trx
@param[in]	use_fk	whether to parse and enforce FOREIGN KEY constraints
@return DB_SUCCESS or error code */
inline
dberr_t
innobase_rename_table(
	trx_t*		trx,
	const char*	from,
	const char*	to,
	bool		commit,
	bool		use_fk)
{
	dberr_t	error;
	char	norm_to[FN_REFLEN];
	char	norm_from[FN_REFLEN];

	DBUG_ENTER("innobase_rename_table");
	DBUG_ASSERT(trx_get_dict_operation(trx) == TRX_DICT_OP_INDEX
		    || trx_get_dict_operation(trx) == TRX_DICT_OP_TABLE);

	ut_ad(!srv_read_only_mode);

	normalize_table_name(norm_to, to);
	normalize_table_name(norm_from, from);

	DEBUG_SYNC_C("innodb_rename_table_ready");

	trx_start_if_not_started(trx, true);
	ut_ad(trx->will_lock > 0);

	if (commit) {
		/* Serialize data dictionary operations with dictionary mutex:
		no deadlocks can occur then in these operations. */
		row_mysql_lock_data_dictionary(trx);
	}

	error = row_rename_table_for_mysql(norm_from, norm_to, trx, commit,
					   use_fk);

	if (error != DB_SUCCESS) {
		if (error == DB_TABLE_NOT_FOUND
		    && innobase_get_lower_case_table_names() == 1) {
			char*	is_part = is_partition(norm_from);

			if (is_part) {
				char	par_case_name[FN_REFLEN];
#ifndef _WIN32
				/* Check for the table using lower
				case name, including the partition
				separator "P" */
				strcpy(par_case_name, norm_from);
				innobase_casedn_str(par_case_name);
#else
				/* On Windows platfrom, check
				whether there exists table name in
				system table whose name is
				not being normalized to lower case */
				create_table_info_t::normalize_table_name_low(
					par_case_name, from, FALSE);
#endif /* _WIN32 */
				trx_start_if_not_started(trx, true);
				error = row_rename_table_for_mysql(
					par_case_name, norm_to, trx,
					true, false);
			}
		}

		if (error == DB_SUCCESS) {
#ifndef _WIN32
			sql_print_warning("Rename partition table %s"
					  " succeeds after converting to lower"
					  " case. The table may have"
					  " been moved from a case"
					  " in-sensitive file system.\n",
					  norm_from);
#else
			sql_print_warning("Rename partition table %s"
					  " succeeds after skipping the step to"
					  " lower case the table name."
					  " The table may have been"
					  " moved from a case sensitive"
					  " file system.\n",
					  norm_from);
#endif /* _WIN32 */
		}
	}

	if (commit) {
		row_mysql_unlock_data_dictionary(trx);
	}

	/* Flush the log to reduce probability that the .frm
	files and the InnoDB data dictionary get out-of-sync
	if the user runs with innodb_flush_log_at_trx_commit = 0 */

	log_buffer_flush_to_disk();

	DBUG_RETURN(error);
}

/** TRUNCATE TABLE
@return	error code
@retval	0	on success */
int ha_innobase::truncate()
{
	DBUG_ENTER("ha_innobase::truncate");

	update_thd();

	if (is_read_only()) {
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	HA_CREATE_INFO	info;
	mem_heap_t*	heap = mem_heap_create(1000);
	dict_table_t*	ib_table = m_prebuilt->table;
	const auto	update_time = ib_table->update_time;
	const auto	stored_lock = m_prebuilt->stored_select_lock_type;
	info.init();
	update_create_info_from_table(&info, table);

	if (ib_table->is_temporary()) {
		info.options|= HA_LEX_CREATE_TMP_TABLE;
	} else {
		dict_get_and_save_data_dir_path(ib_table, false);
	}

	char* data_file_name = ib_table->data_dir_path;

	if (data_file_name) {
		info.data_file_name = data_file_name
			= mem_heap_strdup(heap, data_file_name);
	}

	const char* temp_name = dict_mem_create_temporary_tablename(
		heap, ib_table->name.m_name, ib_table->id);
	const char* name = mem_heap_strdup(heap, ib_table->name.m_name);
	trx_t*	trx = innobase_trx_allocate(m_user_thd);

	++trx->will_lock;
	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);
	row_mysql_lock_data_dictionary(trx);
	int err = convert_error_code_to_mysql(
		innobase_rename_table(trx, ib_table->name.m_name, temp_name,
				      false, false),
		ib_table->flags, m_user_thd);
	if (err) {
		trx_rollback_for_mysql(trx);
		row_mysql_unlock_data_dictionary(trx);
	} else {
		switch (dict_tf_get_rec_format(ib_table->flags)) {
		case REC_FORMAT_REDUNDANT:
			info.row_type = ROW_TYPE_REDUNDANT;
			break;
		case REC_FORMAT_COMPACT:
			info.row_type = ROW_TYPE_COMPACT;
			break;
		case REC_FORMAT_COMPRESSED:
			info.row_type = ROW_TYPE_COMPRESSED;
			break;
		case REC_FORMAT_DYNAMIC:
			info.row_type = ROW_TYPE_DYNAMIC;
			break;
		}

		err = create(name, table, &info,
			     ib_table->is_temporary()
			     || dict_table_is_file_per_table(ib_table), trx);
	}

	trx->free();

	if (!err) {
		/* Reopen the newly created table, and drop the
		original table that was renamed to temp_name. */

		row_prebuilt_t* prebuilt = m_prebuilt;
		uchar* upd_buf = m_upd_buf;
		ulint upd_buf_size = m_upd_buf_size;
		/* Mimic ha_innobase::close(). */
		m_prebuilt = NULL;
		m_upd_buf = NULL;
		m_upd_buf_size = 0;
		err = open(name, 0, 0);
		if (!err) {
			m_prebuilt->stored_select_lock_type = stored_lock;
			m_prebuilt->table->update_time = update_time;
			row_prebuilt_free(prebuilt, FALSE);
			delete_table(temp_name, SQLCOM_TRUNCATE);
			my_free(upd_buf);
		} else {
			/* Revert to the old table before truncation. */
			m_prebuilt = prebuilt;
			m_upd_buf = upd_buf;
			m_upd_buf_size = upd_buf_size;
		}
	}

	mem_heap_free(heap);
	DBUG_RETURN(err);
}

/*********************************************************************//**
Renames an InnoDB table.
@return 0 or error code */

int
ha_innobase::rename_table(
/*======================*/
	const char*	from,	/*!< in: old name of the table */
	const char*	to)	/*!< in: new name of the table */
{
	THD*	thd = ha_thd();

	DBUG_ENTER("ha_innobase::rename_table");

	if (high_level_read_only) {
		ib_senderrf(thd, IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);
		DBUG_RETURN(HA_ERR_TABLE_READONLY);
	}

	trx_t*	trx = innobase_trx_allocate(thd);

	/* We are doing a DDL operation. */
	++trx->will_lock;
	trx_set_dict_operation(trx, TRX_DICT_OP_INDEX);

	dberr_t	error = innobase_rename_table(trx, from, to, true, true);

	DEBUG_SYNC(thd, "after_innobase_rename_table");

	innobase_commit_low(trx);

	trx->free();

	if (error == DB_SUCCESS) {
		char	norm_from[MAX_FULL_NAME_LEN];
		char	norm_to[MAX_FULL_NAME_LEN];
		char	errstr[512];
		dberr_t	ret;

		normalize_table_name(norm_from, from);
		normalize_table_name(norm_to, to);

		ret = dict_stats_rename_table(norm_from, norm_to,
					      errstr, sizeof(errstr));

		if (ret != DB_SUCCESS) {
			ib::error() << errstr;

			push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
				     ER_LOCK_WAIT_TIMEOUT, errstr);
		}
	}

	/* Add a special case to handle the Duplicated Key error
	and return DB_ERROR instead.
	This is to avoid a possible SIGSEGV error from mysql error
	handling code. Currently, mysql handles the Duplicated Key
	error by re-entering the storage layer and getting dup key
	info by calling get_dup_key(). This operation requires a valid
	table handle ('row_prebuilt_t' structure) which could no
	longer be available in the error handling stage. The suggested
	solution is to report a 'table exists' error message (since
	the dup key error here is due to an existing table whose name
	is the one we are trying to rename to) and return the generic
	error code. */
	if (error == DB_DUPLICATE_KEY) {
		my_error(ER_TABLE_EXISTS_ERROR, MYF(0), to);

		error = DB_ERROR;
	} else if (error == DB_LOCK_WAIT_TIMEOUT) {
		my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0), to);

		error = DB_LOCK_WAIT;
	}

	DBUG_RETURN(convert_error_code_to_mysql(error, 0, NULL));
}

/*********************************************************************//**
Estimates the number of index records in a range.
@return estimated number of rows */

ha_rows
ha_innobase::records_in_range(
/*==========================*/
	uint			keynr,		/*!< in: index number */
	const key_range		*min_key,	/*!< in: start key value of the
						range, may also be 0 */
	const key_range		*max_key,	/*!< in: range end key val, may
						also be 0 */
        page_range              *pages)
{
	KEY*		key;
	dict_index_t*	index;
	dtuple_t*	range_start;
	dtuple_t*	range_end;
	ha_rows		n_rows;
	page_cur_mode_t	mode1;
	page_cur_mode_t	mode2;
	mem_heap_t*	heap;

	DBUG_ENTER("records_in_range");

	ut_a(m_prebuilt->trx == thd_to_trx(ha_thd()));

	m_prebuilt->trx->op_info = "estimating records in index range";

	active_index = keynr;

	key = table->key_info + active_index;

	index = innobase_get_index(keynr);

	/* There exists possibility of not being able to find requested
	index due to inconsistency between MySQL and InoDB dictionary info.
	Necessary message should have been printed in innobase_get_index() */
	if (!m_prebuilt->table->space) {
		n_rows = HA_POS_ERROR;
		goto func_exit;
	}
	if (!index) {
		n_rows = HA_POS_ERROR;
		goto func_exit;
	}
	if (index->is_corrupted()) {
		n_rows = HA_ERR_INDEX_CORRUPT;
		goto func_exit;
	}
	if (!row_merge_is_index_usable(m_prebuilt->trx, index)) {
		n_rows = HA_ERR_TABLE_DEF_CHANGED;
		goto func_exit;
	}

	heap = mem_heap_create(2 * (key->ext_key_parts * sizeof(dfield_t)
				    + sizeof(dtuple_t)));

	range_start = dtuple_create(heap, key->ext_key_parts);
	dict_index_copy_types(range_start, index, key->ext_key_parts);

	range_end = dtuple_create(heap, key->ext_key_parts);
	dict_index_copy_types(range_end, index, key->ext_key_parts);

	row_sel_convert_mysql_key_to_innobase(
		range_start,
		m_prebuilt->srch_key_val1,
		m_prebuilt->srch_key_val_len,
		index,
		(byte*) (min_key ? min_key->key : (const uchar*) 0),
		(ulint) (min_key ? min_key->length : 0));

	DBUG_ASSERT(min_key
		    ? range_start->n_fields > 0
		    : range_start->n_fields == 0);

	row_sel_convert_mysql_key_to_innobase(
		range_end,
		m_prebuilt->srch_key_val2,
		m_prebuilt->srch_key_val_len,
		index,
		(byte*) (max_key ? max_key->key : (const uchar*) 0),
		(ulint) (max_key ? max_key->length : 0));

	DBUG_ASSERT(max_key
		    ? range_end->n_fields > 0
		    : range_end->n_fields == 0);

	mode1 = convert_search_mode_to_innobase(
		min_key ? min_key->flag : HA_READ_KEY_EXACT);

	mode2 = convert_search_mode_to_innobase(
		max_key ? max_key->flag : HA_READ_KEY_EXACT);

	if (mode1 != PAGE_CUR_UNSUPP && mode2 != PAGE_CUR_UNSUPP) {

		if (dict_index_is_spatial(index)) {
			/*Only min_key used in spatial index. */
			n_rows = rtr_estimate_n_rows_in_range(
				index, range_start, mode1);
		} else {
                        btr_pos_t tuple1(range_start, mode1, pages->first_page);
                        btr_pos_t tuple2(range_end,   mode2, pages->last_page);
			n_rows = btr_estimate_n_rows_in_range(
                                 index, &tuple1, &tuple2);
                        pages->first_page= tuple1.page_id.raw();
                        pages->last_page=  tuple2.page_id.raw();
		}
	} else {

		n_rows = HA_POS_ERROR;
	}

	mem_heap_free(heap);

	DBUG_EXECUTE_IF(
		"print_btr_estimate_n_rows_in_range_return_value",
		push_warning_printf(
			ha_thd(), Sql_condition::WARN_LEVEL_WARN,
			ER_NO_DEFAULT,
			"btr_estimate_n_rows_in_range(): %lld",
                        (longlong) n_rows);
	);

func_exit:

	m_prebuilt->trx->op_info = (char*)"";

	/* The MySQL optimizer seems to believe an estimate of 0 rows is
	always accurate and may return the result 'Empty set' based on that.
	The accuracy is not guaranteed, and even if it were, for a locking
	read we should anyway perform the search to set the next-key lock.
	Add 1 to the value to make sure MySQL does not make the assumption! */

	if (n_rows == 0) {
		n_rows = 1;
	}

	DBUG_RETURN((ha_rows) n_rows);
}

/*********************************************************************//**
Gives an UPPER BOUND to the number of rows in a table. This is used in
filesort.cc.
@return upper bound of rows */

ha_rows
ha_innobase::estimate_rows_upper_bound()
/*====================================*/
{
	const dict_index_t*	index;
	ulonglong		estimate;
	ulonglong		local_data_file_length;

	DBUG_ENTER("estimate_rows_upper_bound");

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "calculating upper bound for table rows";

	index = dict_table_get_first_index(m_prebuilt->table);

	ulint	stat_n_leaf_pages = index->stat_n_leaf_pages;

	ut_a(stat_n_leaf_pages > 0);

	local_data_file_length = ulonglong(stat_n_leaf_pages)
		<< srv_page_size_shift;

	/* Calculate a minimum length for a clustered index record and from
	that an upper bound for the number of rows. Since we only calculate
	new statistics in row0mysql.cc when a table has grown by a threshold
	factor, we must add a safety factor 2 in front of the formula below. */

	estimate = 2 * local_data_file_length
		/ dict_index_calc_min_rec_len(index);

	m_prebuilt->trx->op_info = "";

        /* Set num_rows less than MERGEBUFF to simulate the case where we do
        not have enough space to merge the externally sorted file blocks. */
        DBUG_EXECUTE_IF("set_num_rows_lt_MERGEBUFF",
                        estimate = 2;
                        DBUG_SET("-d,set_num_rows_lt_MERGEBUFF");
                       );

	DBUG_RETURN((ha_rows) estimate);
}

/*********************************************************************//**
How many seeks it will take to read through the table. This is to be
comparable to the number returned by records_in_range so that we can
decide if we should scan the table or use keys.
@return estimated time measured in disk seeks */

double
ha_innobase::scan_time()
/*====================*/
{
	/* Since MySQL seems to favor table scans too much over index
	searches, we pretend that a sequential read takes the same time
	as a random disk read, that is, we do not divide the following
	by 10, which would be physically realistic. */

	/* The locking below is disabled for performance reasons. Without
	it we could end up returning uninitialized value to the caller,
	which in the worst case could make some query plan go bogus or
	issue a Valgrind warning. */
	if (m_prebuilt == NULL) {
		/* In case of derived table, Optimizer will try to fetch stat
		for table even before table is create or open. In such
		cases return default value of 1.
		TODO: This will be further improved to return some approximate
		estimate but that would also needs pre-population of stats
		structure. As of now approach is in sync with MyISAM. */
		return(ulonglong2double(stats.data_file_length) / IO_SIZE + 2);
	}

	ulint	stat_clustered_index_size;

	ut_a(m_prebuilt->table->stat_initialized);

	stat_clustered_index_size =
		m_prebuilt->table->stat_clustered_index_size;

	return((double) stat_clustered_index_size);
}

/******************************************************************//**
Calculate the time it takes to read a set of ranges through an index
This enables us to optimise reads for clustered indexes.
@return estimated time measured in disk seeks */

double
ha_innobase::read_time(
/*===================*/
	uint	index,	/*!< in: key number */
	uint	ranges,	/*!< in: how many ranges */
	ha_rows rows)	/*!< in: estimated number of rows in the ranges */
{
	ha_rows total_rows;

	if (index != table->s->primary_key) {
		/* Not clustered */
		return(handler::read_time(index, ranges, rows));
	}

	/* Assume that the read time is proportional to the scan time for all
	rows + at most one seek per range. */

	double	time_for_scan = scan_time();

	if ((total_rows = estimate_rows_upper_bound()) < rows) {

		return(time_for_scan);
	}

	return(ranges + (double) rows / (double) total_rows * time_for_scan);
}

/** Update the system variable with the given value of the InnoDB
buffer pool size.
@param[in]	buf_pool_size	given value of buffer pool size.*/
void
innodb_set_buf_pool_size(ulonglong buf_pool_size)
{
	innobase_buffer_pool_size = buf_pool_size;
}

/*********************************************************************//**
Calculates the key number used inside MySQL for an Innobase index.
@return the key number used inside MySQL */
static
unsigned
innobase_get_mysql_key_number_for_index(
/*====================================*/
	const TABLE*		table,	/*!< in: table in MySQL data
					dictionary */
	dict_table_t*		ib_table,/*!< in: table in InnoDB data
					dictionary */
	const dict_index_t*	index)	/*!< in: index */
{
	const dict_index_t*	ind;
	unsigned int		i;

	/* If index does not belong to the table object of share structure
	(ib_table comes from the share structure) search the index->table
	object instead */
	if (index->table != ib_table) {
		i = 0;
		ind = dict_table_get_first_index(index->table);

		while (index != ind) {
			ind = dict_table_get_next_index(ind);
			i++;
		}

		if (dict_index_is_auto_gen_clust(index)) {
			ut_a(i > 0);
			i--;
		}

		return(i);
	}

	/* Directly find matching index with information from mysql TABLE
	structure and InnoDB dict_index_t list */
	for (i = 0; i < table->s->keys; i++) {
		ind = dict_table_get_index_on_name(
			ib_table, table->key_info[i].name.str);

		if (index == ind) {
			return(i);
		}
	}

	/* Loop through each index of the table and lock them */
	for (ind = dict_table_get_first_index(ib_table);
	     ind != NULL;
	     ind = dict_table_get_next_index(ind)) {
		if (index == ind) {
			/* Temp index is internal to InnoDB, that is
			not present in the MySQL index list, so no
			need to print such mismatch warning. */
			if (index->is_committed()) {
				sql_print_warning(
					"Found index %s in InnoDB index list"
					" but not its MariaDB index number."
					" It could be an InnoDB internal"
					" index.",
					index->name());
			}
			return(~0U);
		}
	}

	ut_error;

	return(~0U);
}

/*********************************************************************//**
Calculate Record Per Key value. Need to exclude the NULL value if
innodb_stats_method is set to "nulls_ignored"
@return estimated record per key value */
rec_per_key_t
innodb_rec_per_key(
/*===============*/
	dict_index_t*	index,		/*!< in: dict_index_t structure */
	ulint		i,		/*!< in: the column we are
					calculating rec per key */
	ha_rows		records)	/*!< in: estimated total records */
{
	rec_per_key_t	rec_per_key;
	ib_uint64_t	n_diff;

	ut_a(index->table->stat_initialized);

	ut_ad(i < dict_index_get_n_unique(index));
	ut_ad(!dict_index_is_spatial(index));

	if (records == 0) {
		/* "Records per key" is meaningless for empty tables.
		Return 1.0 because that is most convenient to the Optimizer. */
		return(1.0);
	}

	n_diff = index->stat_n_diff_key_vals[i];

	if (n_diff == 0) {

		rec_per_key = static_cast<rec_per_key_t>(records);
	} else if (srv_innodb_stats_method == SRV_STATS_NULLS_IGNORED) {
		ib_uint64_t	n_null;
		ib_uint64_t	n_non_null;

		n_non_null = index->stat_n_non_null_key_vals[i];

		/* In theory, index->stat_n_non_null_key_vals[i]
		should always be less than the number of records.
		Since this is statistics value, the value could
		have slight discrepancy. But we will make sure
		the number of null values is not a negative number. */
		if (records < n_non_null) {
			n_null = 0;
		} else {
			n_null = records - n_non_null;
		}

		/* If the number of NULL values is the same as or
		larger than that of the distinct values, we could
		consider that the table consists mostly of NULL value.
		Set rec_per_key to 1. */
		if (n_diff <= n_null) {
			rec_per_key = 1.0;
		} else {
			/* Need to exclude rows with NULL values from
			rec_per_key calculation */
			rec_per_key
				= static_cast<rec_per_key_t>(records - n_null)
				/ static_cast<rec_per_key_t>(n_diff - n_null);
		}
	} else {
		DEBUG_SYNC_C("after_checking_for_0");
		rec_per_key = static_cast<rec_per_key_t>(records)
			/ static_cast<rec_per_key_t>(n_diff);
	}

	if (rec_per_key < 1.0) {
		/* Values below 1.0 are meaningless and must be due to the
		stats being imprecise. */
		rec_per_key = 1.0;
	}

	return(rec_per_key);
}

/** Calculate how many KiB of new data we will be able to insert to the
tablespace without running out of space. Start with a space object that has
been acquired by the caller who holds it for the calculation,
@param[in]	space		tablespace object from fil_space_acquire()
@return available space in KiB */
static uintmax_t
fsp_get_available_space_in_free_extents(const fil_space_t& space)
{
	ulint	size_in_header = space.size_in_header;
	if (size_in_header < FSP_EXTENT_SIZE) {
		return 0;		/* TODO: count free frag pages and
					return a value based on that */
	}

	/* Below we play safe when counting free extents above the free limit:
	some of them will contain extent descriptor pages, and therefore
	will not be free extents */
	ut_ad(size_in_header >= space.free_limit);
	ulint	n_free_up =
		(size_in_header - space.free_limit) / FSP_EXTENT_SIZE;

	const ulint size = space.physical_size();
	if (n_free_up > 0) {
		n_free_up--;
		n_free_up -= n_free_up / (size / FSP_EXTENT_SIZE);
	}

	/* We reserve 1 extent + 0.5 % of the space size to undo logs
	and 1 extent + 0.5 % to cleaning operations; NOTE: this source
	code is duplicated in the function above! */

	ulint	reserve = 2 + ((size_in_header / FSP_EXTENT_SIZE) * 2) / 200;
	ulint	n_free = space.free_len + n_free_up;

	if (reserve > n_free) {
		return(0);
	}

	return(static_cast<uintmax_t>(n_free - reserve)
	       * FSP_EXTENT_SIZE * (size / 1024));
}

/*********************************************************************//**
Returns statistics information of the table to the MySQL interpreter,
in various fields of the handle object.
@return HA_ERR_* error code or 0 */

int
ha_innobase::info_low(
/*==================*/
	uint	flag,	/*!< in: what information is requested */
	bool	is_analyze)
{
	dict_table_t*	ib_table;
	ib_uint64_t	n_rows;
	char		path[FN_REFLEN];
	os_file_stat_t	stat_info;

	DBUG_ENTER("info");

	DEBUG_SYNC_C("ha_innobase_info_low");

	ut_ad(!mutex_own(&dict_sys.mutex));

	/* If we are forcing recovery at a high level, we will suppress
	statistics calculation on tables, because that may crash the
	server if an index is badly corrupted. */

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "returning various info to MariaDB";

	ib_table = m_prebuilt->table;
	DBUG_ASSERT(ib_table->get_ref_count() > 0);

	if (flag & HA_STATUS_TIME) {
		if (is_analyze || innobase_stats_on_metadata) {

			dict_stats_upd_option_t	opt;
			dberr_t			ret;

			m_prebuilt->trx->op_info = "updating table statistics";

			if (dict_stats_is_persistent_enabled(ib_table)) {

				if (is_analyze) {
					opt = DICT_STATS_RECALC_PERSISTENT;
				} else {
					/* This is e.g. 'SHOW INDEXES', fetch
					the persistent stats from disk. */
					opt = DICT_STATS_FETCH_ONLY_IF_NOT_IN_MEMORY;
				}
			} else {
				opt = DICT_STATS_RECALC_TRANSIENT;
			}

			ret = dict_stats_update(ib_table, opt);

			if (ret != DB_SUCCESS) {
				m_prebuilt->trx->op_info = "";
				DBUG_RETURN(HA_ERR_GENERIC);
			}

			m_prebuilt->trx->op_info =
				"returning various info to MariaDB";
		}


		stats.update_time = (ulong) ib_table->update_time;
	}

	DBUG_EXECUTE_IF("dict_sys_mutex_avoid", goto func_exit;);

	if (flag & HA_STATUS_VARIABLE) {

		ulint	stat_clustered_index_size;
		ulint	stat_sum_of_other_index_sizes;

		mutex_enter(&dict_sys.mutex);

		ut_a(ib_table->stat_initialized);

		n_rows = ib_table->stat_n_rows;

		stat_clustered_index_size
			= ib_table->stat_clustered_index_size;

		stat_sum_of_other_index_sizes
			= ib_table->stat_sum_of_other_index_sizes;

		mutex_exit(&dict_sys.mutex);

		/*
		The MySQL optimizer seems to assume in a left join that n_rows
		is an accurate estimate if it is zero. Of course, it is not,
		since we do not have any locks on the rows yet at this phase.
		Since SHOW TABLE STATUS seems to call this function with the
		HA_STATUS_TIME flag set, while the left join optimizer does not
		set that flag, we add one to a zero value if the flag is not
		set. That way SHOW TABLE STATUS will show the best estimate,
		while the optimizer never sees the table empty. */

		if (n_rows == 0 && !(flag & (HA_STATUS_TIME | HA_STATUS_OPEN))) {
			n_rows++;
		}

		/* Fix bug#40386: Not flushing query cache after truncate.
		n_rows can not be 0 unless the table is empty, set to 1
		instead. The original problem of bug#29507 is actually
		fixed in the server code. */
		if (thd_sql_command(m_user_thd) == SQLCOM_TRUNCATE) {

			n_rows = 1;

			/* We need to reset the m_prebuilt value too, otherwise
			checks for values greater than the last value written
			to the table will fail and the autoinc counter will
			not be updated. This will force write_row() into
			attempting an update of the table's AUTOINC counter. */

			m_prebuilt->autoinc_last_value = 0;
		}

		stats.records = (ha_rows) n_rows;
		stats.deleted = 0;
		if (fil_space_t* space = ib_table->space) {
			const ulint size = space->physical_size();
			stats.data_file_length
				= ulonglong(stat_clustered_index_size)
				* size;
			stats.index_file_length
				= ulonglong(stat_sum_of_other_index_sizes)
				* size;
			stats.delete_length = 1024
				* fsp_get_available_space_in_free_extents(
					*space);
		}
		stats.check_time = 0;
		stats.mrr_length_per_rec= (uint)ref_length +  8; // 8 = max(sizeof(void *));

		if (stats.records == 0) {
			stats.mean_rec_length = 0;
		} else {
			stats.mean_rec_length = (ulong)
				(stats.data_file_length / stats.records);
		}
	}

	if (flag & HA_STATUS_CONST) {
		/* Verify the number of index in InnoDB and MySQL
		matches up. If m_prebuilt->clust_index_was_generated
		holds, InnoDB defines GEN_CLUST_INDEX internally */
		ulint	num_innodb_index = UT_LIST_GET_LEN(ib_table->indexes)
			- m_prebuilt->clust_index_was_generated;
		if (table->s->keys < num_innodb_index) {
			/* If there are too many indexes defined
			inside InnoDB, ignore those that are being
			created, because MySQL will only consider
			the fully built indexes here. */

			for (const dict_index_t* index
				     = UT_LIST_GET_FIRST(ib_table->indexes);
			     index != NULL;
			     index = UT_LIST_GET_NEXT(indexes, index)) {

				/* First, online index creation is
				completed inside InnoDB, and then
				MySQL attempts to upgrade the
				meta-data lock so that it can rebuild
				the .frm file. If we get here in that
				time frame, dict_index_is_online_ddl()
				would not hold and the index would
				still not be included in TABLE_SHARE. */
				if (!index->is_committed()) {
					num_innodb_index--;
				}
			}

			if (table->s->keys < num_innodb_index
			    && innobase_fts_check_doc_id_index(
				    ib_table, NULL, NULL)
			    == FTS_EXIST_DOC_ID_INDEX) {
				num_innodb_index--;
			}
		}

		if (table->s->keys != num_innodb_index) {
			ib_table->dict_frm_mismatch = DICT_FRM_INCONSISTENT_KEYS;
			ib_push_frm_error(m_user_thd, ib_table, table, num_innodb_index, true);
		}

		snprintf(path, sizeof(path), "%s/%s%s",
			 mysql_data_home, table->s->normalized_path.str,
			 reg_ext);

		unpack_filename(path,path);

		/* Note that we do not know the access time of the table,
		nor the CHECK TABLE time, nor the UPDATE or INSERT time. */

		if (os_file_get_status(
			    path, &stat_info, false,
			    srv_read_only_mode) == DB_SUCCESS) {
			stats.create_time = (ulong) stat_info.ctime;
		}

		struct Locking {
			Locking() { mutex_enter(&dict_sys.mutex); }
			~Locking() { mutex_exit(&dict_sys.mutex); }
		} locking;

		ut_a(ib_table->stat_initialized);

		for (uint i = 0; i < table->s->keys; i++) {
			ulong	j;

			dict_index_t* index = innobase_get_index(i);

			if (index == NULL) {
				ib_table->dict_frm_mismatch = DICT_FRM_INCONSISTENT_KEYS;
				ib_push_frm_error(m_user_thd, ib_table, table, num_innodb_index, true);
				break;
			}

			KEY*	key = &table->key_info[i];

			for (j = 0; j < key->ext_key_parts; j++) {

				if ((key->flags & HA_FULLTEXT)
				    || (key->flags & HA_SPATIAL)) {

					/* The record per key does not apply to
					FTS or Spatial indexes. */
				/*
					key->rec_per_key[j] = 1;
					key->set_records_per_key(j, 1.0);
				*/
					continue;
				}

				if (j + 1 > index->n_uniq) {
					sql_print_error(
						"Index %s of %s has %u columns"
					        " unique inside InnoDB, but "
						"MySQL is asking statistics for"
					        " %lu columns. Have you mixed "
						"up .frm files from different "
						" installations? %s",
						index->name(),
						ib_table->name.m_name,
						index->n_uniq, j + 1,
						TROUBLESHOOTING_MSG);
					break;
				}

				/* innodb_rec_per_key() will use
				index->stat_n_diff_key_vals[] and the value we
				pass index->table->stat_n_rows. Both are
				calculated by ANALYZE and by the background
				stats gathering thread (which kicks in when too
				much of the table has been changed). In
				addition table->stat_n_rows is adjusted with
				each DML (e.g. ++ on row insert). Those
				adjustments are not MVCC'ed and not even
				reversed on rollback. So,
				index->stat_n_diff_key_vals[] and
				index->table->stat_n_rows could have been
				calculated at different time. This is
				acceptable. */

				ulong	rec_per_key_int = static_cast<ulong>(
					innodb_rec_per_key(index, j,
							   stats.records));

				/* Since MySQL seems to favor table scans
				too much over index searches, we pretend
				index selectivity is 2 times better than
				our estimate: */

				rec_per_key_int = rec_per_key_int / 2;

				if (rec_per_key_int == 0) {
					rec_per_key_int = 1;
				}

				key->rec_per_key[j] = rec_per_key_int;
			}
		}
	}

	if (srv_force_recovery > SRV_FORCE_NO_IBUF_MERGE) {

		goto func_exit;

	} else if (flag & HA_STATUS_ERRKEY) {
		const dict_index_t*	err_index;

		ut_a(m_prebuilt->trx);
		ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);

		err_index = trx_get_error_info(m_prebuilt->trx);

		if (err_index) {
			errkey = innobase_get_mysql_key_number_for_index(
					table, ib_table, err_index);
		} else {
			errkey = (unsigned int) (
				(m_prebuilt->trx->error_key_num
				 == ULINT_UNDEFINED)
					? ~0U
					: m_prebuilt->trx->error_key_num);
		}
	}

	if ((flag & HA_STATUS_AUTO) && table->found_next_number_field) {
		stats.auto_increment_value = innobase_peek_autoinc();
	}

func_exit:
	m_prebuilt->trx->op_info = (char*)"";

	DBUG_RETURN(0);
}

/*********************************************************************//**
Returns statistics information of the table to the MySQL interpreter,
in various fields of the handle object.
@return HA_ERR_* error code or 0 */

int
ha_innobase::info(
/*==============*/
	uint	flag)	/*!< in: what information is requested */
{
	return(info_low(flag, false /* not ANALYZE */));
}

/*
Updates index cardinalities of the table, based on random dives into
each index tree. This does NOT calculate exact statistics on the table.
@return HA_ADMIN_* error code or HA_ADMIN_OK */

int
ha_innobase::analyze(THD*, HA_CHECK_OPT*)
{
	/* Simply call info_low() with all the flags
	and request recalculation of the statistics */
	int	ret = info_low(
		HA_STATUS_TIME | HA_STATUS_CONST | HA_STATUS_VARIABLE,
		true /* this is ANALYZE */);

	if (ret != 0) {
		return(HA_ADMIN_FAILED);
	}

	return(HA_ADMIN_OK);
}

/*****************************************************************//**
Defragment table.
@return	error number */
inline int ha_innobase::defragment_table(const char *name)
{
	char		norm_name[FN_REFLEN];
	dict_table_t*	table = NULL;
	dict_index_t*	index = NULL;
	int		ret = 0;
	dberr_t		err = DB_SUCCESS;

	normalize_table_name(norm_name, name);

	table = dict_table_open_on_name(norm_name, FALSE,
		FALSE, DICT_ERR_IGNORE_FK_NOKEY);

	for (index = dict_table_get_first_index(table); index;
	     index = dict_table_get_next_index(index)) {

		if (index->is_corrupted()) {
			continue;
		}

		if (dict_index_is_spatial(index)) {
			/* Do not try to defragment spatial indexes,
			because doing it properly would require
			appropriate logic around the SSN (split
			sequence number). */
			continue;
		}

		if (index->page == FIL_NULL) {
			/* Do not defragment auxiliary tables related
			to FULLTEXT INDEX. */
			ut_ad(index->type & DICT_FTS);
			continue;
		}

		if (btr_defragment_find_index(index)) {
			// We borrow this error code. When the same index is
			// already in the defragmentation queue, issue another
			// defragmentation only introduces overhead. We return
			// an error here to let the user know this is not
			// necessary. Note that this will fail a query that's
			// trying to defragment a full table if one of the
			// indicies in that table is already in defragmentation.
			// We choose this behavior so user is aware of this
			// rather than silently defragment other indicies of
			// that table.
			ret = ER_SP_ALREADY_EXISTS;
			break;
		}

		os_event_t event = btr_defragment_add_index(index, &err);

		if (err != DB_SUCCESS) {
			push_warning_printf(
				current_thd,
				Sql_condition::WARN_LEVEL_WARN,
				ER_NO_SUCH_TABLE,
				"Table %s is encrypted but encryption service or"
				" used key_id is not available. "
				" Can't continue checking table.",
				index->table->name.m_name);

			ret = convert_error_code_to_mysql(err, 0, current_thd);
			break;
		}

		if (event) {
			while(os_event_wait_time(event, 1000000)) {
				if (thd_killed(current_thd)) {
					btr_defragment_remove_index(index);
					ret = ER_QUERY_INTERRUPTED;
					break;
				}
			}
			os_event_destroy(event);
		}

		if (ret) {
			break;
		}
	}

	dict_table_close(table, FALSE, FALSE);
	return ret;
}

/**********************************************************************//**
This is mapped to "ALTER TABLE tablename ENGINE=InnoDB", which rebuilds
the table in MySQL. */

int
ha_innobase::optimize(
/*==================*/
	THD*		thd,		/*!< in: connection thread handle */
	HA_CHECK_OPT*)
{

	/* FTS-FIXME: Since MySQL doesn't support engine-specific commands,
	we have to hijack some existing command in order to be able to test
	the new admin commands added in InnoDB's FTS support. For now, we
	use MySQL's OPTIMIZE command, normally mapped to ALTER TABLE in
	InnoDB (so it recreates the table anew), and map it to OPTIMIZE.

	This works OK otherwise, but MySQL locks the entire table during
	calls to OPTIMIZE, which is undesirable. */
	bool try_alter = true;

	if (!m_prebuilt->table->is_temporary() && srv_defragment) {
		int err = defragment_table(m_prebuilt->table->name.m_name);

		if (err == 0) {
			try_alter = false;
		} else {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    uint(err),
				"InnoDB: Cannot defragment table %s: returned error code %d\n",
				m_prebuilt->table->name.m_name, err);

			if(err == ER_SP_ALREADY_EXISTS) {
				try_alter = false;
			}
		}
	}

	if (innodb_optimize_fulltext_only) {
		if (m_prebuilt->table->fts && m_prebuilt->table->fts->cache
		    && m_prebuilt->table->space) {
			fts_sync_table(m_prebuilt->table);
			fts_optimize_table(m_prebuilt->table);
		}
		try_alter = false;
	}

	return try_alter ? HA_ADMIN_TRY_ALTER : HA_ADMIN_OK;
}

/*******************************************************************//**
Tries to check that an InnoDB table is not corrupted. If corruption is
noticed, prints to stderr information about it. In case of corruption
may also assert a failure and crash the server.
@return HA_ADMIN_CORRUPT or HA_ADMIN_OK */

int
ha_innobase::check(
/*===============*/
	THD*		thd,		/*!< in: user thread handle */
	HA_CHECK_OPT*	check_opt)	/*!< in: check options */
{
	dict_index_t*	index;
	ulint		n_rows;
	ulint		n_rows_in_table	= ULINT_UNDEFINED;
	bool		is_ok		= true;
	dberr_t		ret;

	DBUG_ENTER("ha_innobase::check");
	DBUG_ASSERT(thd == ha_thd());
	ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);
	ut_a(m_prebuilt->trx == thd_to_trx(thd));

	if (m_prebuilt->mysql_template == NULL) {
		/* Build the template; we will use a dummy template
		in index scans done in checking */

		build_template(true);
	}

	if (!m_prebuilt->table->space) {

		ib_senderrf(
			thd,
			IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_DISCARDED,
			table->s->table_name.str);

		DBUG_RETURN(HA_ADMIN_CORRUPT);

	} else if (!m_prebuilt->table->is_readable() &&
		   !m_prebuilt->table->space) {

		ib_senderrf(
			thd, IB_LOG_LEVEL_ERROR,
			ER_TABLESPACE_MISSING,
			table->s->table_name.str);

		DBUG_RETURN(HA_ADMIN_CORRUPT);
	}

	m_prebuilt->trx->op_info = "checking table";

	if (m_prebuilt->table->corrupted) {
		/* If some previous operation has marked the table as
		corrupted in memory, and has not propagated such to
		clustered index, we will do so here */
		index = dict_table_get_first_index(m_prebuilt->table);

		if (!index->is_corrupted()) {
			dict_set_corrupted(
				index, m_prebuilt->trx, "CHECK TABLE");
		}

		push_warning_printf(m_user_thd,
				    Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_INDEX_CORRUPT,
				    "InnoDB: Index %s is marked as"
				    " corrupted",
				    index->name());

		/* Now that the table is already marked as corrupted,
		there is no need to check any index of this table */
		m_prebuilt->trx->op_info = "";

		DBUG_RETURN(HA_ADMIN_CORRUPT);
	}

	uint old_isolation_level = m_prebuilt->trx->isolation_level;

	/* We must run the index record counts at an isolation level
	>= READ COMMITTED, because a dirty read can see a wrong number
	of records in some index; to play safe, we use always
	REPEATABLE READ here (except when undo logs are unavailable) */
	m_prebuilt->trx->isolation_level = srv_force_recovery
		>= SRV_FORCE_NO_UNDO_LOG_SCAN
		? TRX_ISO_READ_UNCOMMITTED
		: TRX_ISO_REPEATABLE_READ;

	ut_ad(!m_prebuilt->table->corrupted);

	for (index = dict_table_get_first_index(m_prebuilt->table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {
		/* If this is an index being created or dropped, skip */
		if (!index->is_committed()) {
			continue;
		}

		if (!(check_opt->flags & T_QUICK)
		    && !index->is_corrupted()) {

			dberr_t err = btr_validate_index(
					index, m_prebuilt->trx);

			if (err != DB_SUCCESS) {
				is_ok = false;

				if (err == DB_DECRYPTION_FAILED) {
					push_warning_printf(
						thd,
						Sql_condition::WARN_LEVEL_WARN,
						ER_NO_SUCH_TABLE,
						"Table %s is encrypted but encryption service or"
						" used key_id is not available. "
						" Can't continue checking table.",
						index->table->name.m_name);
				} else {
					push_warning_printf(
						thd,
						Sql_condition::WARN_LEVEL_WARN,
						ER_NOT_KEYFILE,
						"InnoDB: The B-tree of"
						" index %s is corrupted.",
						index->name());
				}

				continue;
			}
		}

		/* Instead of invoking change_active_index(), set up
		a dummy template for non-locking reads, disabling
		access to the clustered index. */
		m_prebuilt->index = index;

		m_prebuilt->index_usable = row_merge_is_index_usable(
			m_prebuilt->trx, m_prebuilt->index);

		DBUG_EXECUTE_IF(
			"dict_set_index_corrupted",
			if (!index->is_primary()) {
				m_prebuilt->index_usable = FALSE;
				// row_mysql_lock_data_dictionary(m_prebuilt->trx);
				dict_set_corrupted(index, m_prebuilt->trx, "dict_set_index_corrupted");
				// row_mysql_unlock_data_dictionary(m_prebuilt->trx);
			});

		if (UNIV_UNLIKELY(!m_prebuilt->index_usable)) {
			if (index->is_corrupted()) {
				push_warning_printf(
					m_user_thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_INDEX_CORRUPT,
					"InnoDB: Index %s is marked as"
					" corrupted",
					index->name());
				is_ok = false;
			} else {
				push_warning_printf(
					m_user_thd,
					Sql_condition::WARN_LEVEL_WARN,
					HA_ERR_TABLE_DEF_CHANGED,
					"InnoDB: Insufficient history for"
					" index %s",
					index->name());
			}
			continue;
		}

		m_prebuilt->sql_stat_start = TRUE;
		m_prebuilt->template_type = ROW_MYSQL_DUMMY_TEMPLATE;
		m_prebuilt->n_template = 0;
		m_prebuilt->need_to_access_clustered = FALSE;

		dtuple_set_n_fields(m_prebuilt->search_tuple, 0);

		m_prebuilt->select_lock_type = LOCK_NONE;

		/* Scan this index. */
		if (dict_index_is_spatial(index)) {
			ret = row_count_rtree_recs(m_prebuilt, &n_rows);
		} else {
			ret = row_scan_index_for_mysql(
				m_prebuilt, index, &n_rows);
		}

		DBUG_EXECUTE_IF(
			"dict_set_index_corrupted",
			if (!index->is_primary()) {
				ret = DB_CORRUPTION;
			});

		if (ret == DB_INTERRUPTED || thd_killed(m_user_thd)) {
			/* Do not report error since this could happen
			during shutdown */
			break;
		}
		if (ret != DB_SUCCESS) {
			/* Assume some kind of corruption. */
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NOT_KEYFILE,
				"InnoDB: The B-tree of"
				" index %s is corrupted.",
				index->name());
			is_ok = false;
			dict_set_corrupted(
				index, m_prebuilt->trx, "CHECK TABLE-check index");
		}


		if (index == dict_table_get_first_index(m_prebuilt->table)) {
			n_rows_in_table = n_rows;
		} else if (!(index->type & DICT_FTS)
			   && (n_rows != n_rows_in_table)) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NOT_KEYFILE,
				"InnoDB: Index '%-.200s' contains " ULINTPF
				" entries, should be " ULINTPF ".",
				index->name(), n_rows, n_rows_in_table);
			is_ok = false;
			dict_set_corrupted(
				index, m_prebuilt->trx,
				"CHECK TABLE; Wrong count");
		}
	}

	/* Restore the original isolation level */
	m_prebuilt->trx->isolation_level = old_isolation_level;
#ifdef BTR_CUR_HASH_ADAPT
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	/* We validate the whole adaptive hash index for all tables
	at every CHECK TABLE only when QUICK flag is not present. */

	if (!(check_opt->flags & T_QUICK) && !btr_search_validate()) {
		push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
			     ER_NOT_KEYFILE,
			     "InnoDB: The adaptive hash index is corrupted.");
		is_ok = false;
	}
# endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
	m_prebuilt->trx->op_info = "";

	DBUG_RETURN(is_ok ? HA_ADMIN_OK : HA_ADMIN_CORRUPT);
}

/*************************************************************//**
Adds information about free space in the InnoDB tablespace to a table comment
which is printed out when a user calls SHOW TABLE STATUS. Adds also info on
foreign keys.
@return	table comment + InnoDB free space + info on foreign keys */
UNIV_INTERN
char*
ha_innobase::update_table_comment(
/*==============================*/
	const char*	comment)/*!< in: table comment defined by user */
{
	uint	length = (uint) strlen(comment);
	char*	str=0;
	size_t	flen;
	std::string fk_str;

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	if (length > 64000 - 3) {
		return((char*) comment); /* string too long */
	}

	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "returning table comment";

#define SSTR( x ) reinterpret_cast< std::ostringstream & >(		\
        ( std::ostringstream() << std::dec << x ) ).str()

	if (m_prebuilt->table->space) {
		fk_str.append("InnoDB free: ");
		fk_str.append(SSTR(fsp_get_available_space_in_free_extents(
					   *m_prebuilt->table->space)));
	}

	fk_str.append(dict_print_info_on_foreign_keys(
			FALSE, m_prebuilt->trx,
			m_prebuilt->table));

	flen = fk_str.length();

	if (length + flen + 3 > 64000) {
		flen = 64000 - 3 - length;
	}
	/* allocate buffer for the full string */
	str = (char*) my_malloc(PSI_INSTRUMENT_ME, length + flen + 3, MYF(0));
	if (str) {
		char* pos	= str + length;
		if (length) {
			memcpy(str, comment, length);
			*pos++ = ';';
			*pos++ = ' ';
		}
		memcpy(pos, fk_str.c_str(), flen);
		pos[flen] = 0;
	}

	m_prebuilt->trx->op_info = (char*)"";

	return(str ? str : (char*) comment);
}

/*******************************************************************//**
Gets the foreign key create info for a table stored in InnoDB.
@return own: character string in the form which can be inserted to the
CREATE TABLE statement, MUST be freed with
ha_innobase::free_foreign_key_create_info */

char*
ha_innobase::get_foreign_key_create_info(void)
/*==========================================*/
{
	ut_a(m_prebuilt != NULL);

	/* We do not know if MySQL can call this function before calling
	external_lock(). To be safe, update the thd of the current table
	handle. */

	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "getting info on foreign keys";

	/* Output the data to a temporary string */
	std::string str = dict_print_info_on_foreign_keys(
		TRUE, m_prebuilt->trx,
		m_prebuilt->table);

	m_prebuilt->trx->op_info = "";

	/* Allocate buffer for the string */
	char *fk_str = reinterpret_cast<char*>(
			my_malloc(PSI_INSTRUMENT_ME, str.length() + 1, MYF(0)));

	if (fk_str) {
		memcpy(fk_str, str.c_str(), str.length());
		fk_str[str.length()]='\0';
	}

	return(fk_str);
}


/***********************************************************************//**
Maps a InnoDB foreign key constraint to a equivalent MySQL foreign key info.
@return pointer to foreign key info */
static
FOREIGN_KEY_INFO*
get_foreign_key_info(
/*=================*/
	THD*		thd,	/*!< in: user thread handle */
	dict_foreign_t* foreign)/*!< in: foreign key constraint */
{
	FOREIGN_KEY_INFO	f_key_info;
	FOREIGN_KEY_INFO*	pf_key_info;
	uint			i = 0;
	size_t			len;
	char			tmp_buff[NAME_LEN+1];
	char			name_buff[NAME_LEN+1];
	const char*		ptr;
	LEX_CSTRING*		referenced_key_name;
	LEX_CSTRING*		name = NULL;

	if (dict_table_t::is_temporary_name(foreign->foreign_table_name)) {
 		return NULL;
 	}

	ptr = dict_remove_db_name(foreign->id);
	f_key_info.foreign_id = thd_make_lex_string(
		thd, 0, ptr, strlen(ptr), 1);

	/* Name format: database name, '/', table name, '\0' */

	/* Referenced (parent) database name */
	len = dict_get_db_name_len(foreign->referenced_table_name);
	ut_a(len < sizeof(tmp_buff));
	memcpy(tmp_buff, foreign->referenced_table_name, len);
	tmp_buff[len] = 0;

	len = filename_to_tablename(tmp_buff, name_buff, sizeof(name_buff));
	f_key_info.referenced_db = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	/* Referenced (parent) table name */
	ptr = dict_remove_db_name(foreign->referenced_table_name);
	len = filename_to_tablename(ptr, name_buff, sizeof(name_buff), 1);
	f_key_info.referenced_table = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	/* Dependent (child) database name */
	len = dict_get_db_name_len(foreign->foreign_table_name);
	ut_a(len < sizeof(tmp_buff));
	memcpy(tmp_buff, foreign->foreign_table_name, len);
	tmp_buff[len] = 0;

	len = filename_to_tablename(tmp_buff, name_buff, sizeof(name_buff));
	f_key_info.foreign_db = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	/* Dependent (child) table name */
	ptr = dict_remove_db_name(foreign->foreign_table_name);
	len = filename_to_tablename(ptr, name_buff, sizeof(name_buff), 1);
	f_key_info.foreign_table = thd_make_lex_string(
		thd, 0, name_buff, len, 1);

	do {
		ptr = foreign->foreign_col_names[i];
		name = thd_make_lex_string(thd, name, ptr,
					   strlen(ptr), 1);
		f_key_info.foreign_fields.push_back(name);
		ptr = foreign->referenced_col_names[i];
		name = thd_make_lex_string(thd, name, ptr,
					   strlen(ptr), 1);
		f_key_info.referenced_fields.push_back(name);
	} while (++i < foreign->n_fields);

	if (foreign->type & DICT_FOREIGN_ON_DELETE_CASCADE) {
		f_key_info.delete_method = FK_OPTION_CASCADE;
	} else if (foreign->type & DICT_FOREIGN_ON_DELETE_SET_NULL) {
		f_key_info.delete_method = FK_OPTION_SET_NULL;
	} else if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
		f_key_info.delete_method = FK_OPTION_NO_ACTION;
	} else {
		f_key_info.delete_method = FK_OPTION_RESTRICT;
	}


	if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
		f_key_info.update_method = FK_OPTION_CASCADE;
	} else if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
		f_key_info.update_method = FK_OPTION_SET_NULL;
	} else if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
		f_key_info.update_method = FK_OPTION_NO_ACTION;
	} else {
		f_key_info.update_method = FK_OPTION_RESTRICT;
	}

	/* Load referenced table to update FK referenced key name. */
	if (foreign->referenced_table == NULL) {

		dict_table_t*	ref_table;

		ut_ad(mutex_own(&dict_sys.mutex));
		ref_table = dict_table_open_on_name(
			foreign->referenced_table_name_lookup,
			TRUE, FALSE, DICT_ERR_IGNORE_NONE);

		if (ref_table == NULL) {

			if (!thd_test_options(
				thd, OPTION_NO_FOREIGN_KEY_CHECKS)) {
				ib::info()
					<< "Foreign Key referenced table "
					<< foreign->referenced_table_name
					<< " not found for foreign table "
					<< foreign->foreign_table_name;
 			}
		} else {

			dict_table_close(ref_table, TRUE, FALSE);
		}
	}

	if (foreign->referenced_index
	    && foreign->referenced_index->name != NULL) {
		referenced_key_name = thd_make_lex_string(
			thd,
			f_key_info.referenced_key_name,
			foreign->referenced_index->name,
			strlen(foreign->referenced_index->name),
			1);
	} else {
		referenced_key_name = NULL;
	}

	f_key_info.referenced_key_name = referenced_key_name;

	pf_key_info = (FOREIGN_KEY_INFO*) thd_memdup(thd, &f_key_info,
						      sizeof(FOREIGN_KEY_INFO));

	return(pf_key_info);
}

/*******************************************************************//**
Gets the list of foreign keys in this table.
@return always 0, that is, always succeeds */

int
ha_innobase::get_foreign_key_list(
/*==============================*/
	THD*			thd,		/*!< in: user thread handle */
	List<FOREIGN_KEY_INFO>*	f_key_list)	/*!< out: foreign key list */
{
	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "getting list of foreign keys";

	mutex_enter(&dict_sys.mutex);

	for (dict_foreign_set::iterator it
		= m_prebuilt->table->foreign_set.begin();
	     it != m_prebuilt->table->foreign_set.end();
	     ++it) {

		FOREIGN_KEY_INFO*	pf_key_info;
		dict_foreign_t*		foreign = *it;

		pf_key_info = get_foreign_key_info(thd, foreign);

		if (pf_key_info != NULL) {
			f_key_list->push_back(pf_key_info);
		}
	}

	mutex_exit(&dict_sys.mutex);

	m_prebuilt->trx->op_info = "";

	return(0);
}

/*******************************************************************//**
Gets the set of foreign keys where this table is the referenced table.
@return always 0, that is, always succeeds */

int
ha_innobase::get_parent_foreign_key_list(
/*=====================================*/
	THD*			thd,		/*!< in: user thread handle */
	List<FOREIGN_KEY_INFO>*	f_key_list)	/*!< out: foreign key list */
{
	update_thd(ha_thd());

	m_prebuilt->trx->op_info = "getting list of referencing foreign keys";

	mutex_enter(&dict_sys.mutex);

	for (dict_foreign_set::iterator it
		= m_prebuilt->table->referenced_set.begin();
	     it != m_prebuilt->table->referenced_set.end();
	     ++it) {

		FOREIGN_KEY_INFO*	pf_key_info;
		dict_foreign_t*		foreign = *it;

		pf_key_info = get_foreign_key_info(thd, foreign);

		if (pf_key_info != NULL) {
			f_key_list->push_back(pf_key_info);
		}
	}

	mutex_exit(&dict_sys.mutex);

	m_prebuilt->trx->op_info = "";

	return(0);
}

/** Table list item structure is used to store only the table
and name. It is used by get_cascade_foreign_key_table_list to store
the intermediate result for fetching the table set. */
struct table_list_item {
	/** InnoDB table object */
	const dict_table_t*	table;
	/** Table name */
	const char*		name;
};

/*****************************************************************//**
Checks if ALTER TABLE may change the storage engine of the table.
Changing storage engines is not allowed for tables for which there
are foreign key constraints (parent or child tables).
@return TRUE if can switch engines */

bool
ha_innobase::can_switch_engines(void)
/*=================================*/
{
	DBUG_ENTER("ha_innobase::can_switch_engines");

	update_thd();

	m_prebuilt->trx->op_info =
			"determining if there are foreign key constraints";

	row_mysql_freeze_data_dictionary(m_prebuilt->trx);

	bool	can_switch = m_prebuilt->table->referenced_set.empty()
		&& m_prebuilt->table->foreign_set.empty();

	row_mysql_unfreeze_data_dictionary(m_prebuilt->trx);
	m_prebuilt->trx->op_info = "";

	DBUG_RETURN(can_switch);
}

/*******************************************************************//**
Checks if a table is referenced by a foreign key. The MySQL manual states that
a REPLACE is either equivalent to an INSERT, or DELETE(s) + INSERT. Only a
delete is then allowed internally to resolve a duplicate key conflict in
REPLACE, not an update.
@return > 0 if referenced by a FOREIGN KEY */

uint
ha_innobase::referenced_by_foreign_key(void)
/*========================================*/
{
	if (dict_table_is_referenced_by_foreign_key(m_prebuilt->table)) {

		return(1);
	}

	return(0);
}

/*******************************************************************//**
Frees the foreign key create info for a table stored in InnoDB, if it is
non-NULL. */

void
ha_innobase::free_foreign_key_create_info(
/*======================================*/
	char*	str)	/*!< in, own: create info string to free */
{
	if (str != NULL) {
		my_free(str);
	}
}

/*******************************************************************//**
Tells something additional to the handler about how to do things.
@return 0 or error number */

int
ha_innobase::extra(
/*===============*/
	enum ha_extra_function operation)
			   /*!< in: HA_EXTRA_FLUSH or some other flag */
{
	check_trx_exists(ha_thd());

	/* Warning: since it is not sure that MySQL calls external_lock
	before calling this function, the trx field in m_prebuilt can be
	obsolete! */

	switch (operation) {
	case HA_EXTRA_FLUSH:
		if (m_prebuilt->blob_heap) {
			row_mysql_prebuilt_free_blob_heap(m_prebuilt);
		}
		break;
	case HA_EXTRA_RESET_STATE:
		reset_template();
		thd_to_trx(ha_thd())->duplicates = 0;
		break;
	case HA_EXTRA_NO_KEYREAD:
		m_prebuilt->read_just_key = 0;
		break;
	case HA_EXTRA_KEYREAD:
		m_prebuilt->read_just_key = 1;
		break;
	case HA_EXTRA_KEYREAD_PRESERVE_FIELDS:
		m_prebuilt->keep_other_fields_on_keyread = 1;
		break;

		/* IMPORTANT: m_prebuilt->trx can be obsolete in
		this method, because it is not sure that MySQL
		calls external_lock before this method with the
		parameters below.  We must not invoke update_thd()
		either, because the calling threads may change.
		CAREFUL HERE, OR MEMORY CORRUPTION MAY OCCUR! */
	case HA_EXTRA_INSERT_WITH_UPDATE:
		thd_to_trx(ha_thd())->duplicates |= TRX_DUP_IGNORE;
		break;
	case HA_EXTRA_NO_IGNORE_DUP_KEY:
		thd_to_trx(ha_thd())->duplicates &= ~TRX_DUP_IGNORE;
		break;
	case HA_EXTRA_WRITE_CAN_REPLACE:
		thd_to_trx(ha_thd())->duplicates |= TRX_DUP_REPLACE;
		break;
	case HA_EXTRA_WRITE_CANNOT_REPLACE:
		thd_to_trx(ha_thd())->duplicates &= ~TRX_DUP_REPLACE;
		break;
	case HA_EXTRA_BEGIN_ALTER_COPY:
		m_prebuilt->table->skip_alter_undo = 1;
		if (m_prebuilt->table->is_temporary()
		    || !m_prebuilt->table->versioned_by_id()) {
			break;
		}
		trx_start_if_not_started(m_prebuilt->trx, true);
		m_prebuilt->trx->mod_tables.insert(
			trx_mod_tables_t::value_type(
				const_cast<dict_table_t*>(m_prebuilt->table),
				0))
			.first->second.set_versioned(0);
		break;
	case HA_EXTRA_END_ALTER_COPY:
		m_prebuilt->table->skip_alter_undo = 0;
		break;
	case HA_EXTRA_FAKE_START_STMT:
		trx_register_for_2pc(m_prebuilt->trx);
		m_prebuilt->sql_stat_start = true;
		break;
	default:/* Do nothing */
		;
	}

	return(0);
}

/**
MySQL calls this method at the end of each statement */
int
ha_innobase::reset()
{
	if (m_prebuilt->blob_heap) {
		row_mysql_prebuilt_free_blob_heap(m_prebuilt);
	}

	reset_template();

	m_ds_mrr.dsmrr_close();

	/* TODO: This should really be reset in reset_template() but for now
	it's safer to do it explicitly here. */

	/* This is a statement level counter. */
	m_prebuilt->autoinc_last_value = 0;

	return(0);
}

/******************************************************************//**
MySQL calls this function at the start of each SQL statement inside LOCK
TABLES. Inside LOCK TABLES the ::external_lock method does not work to
mark SQL statement borders. Note also a special case: if a temporary table
is created inside LOCK TABLES, MySQL has not called external_lock() at all
on that table.
MySQL-5.0 also calls this before each statement in an execution of a stored
procedure. To make the execution more deterministic for binlogging, MySQL-5.0
locks all tables involved in a stored procedure with full explicit table
locks (thd_in_lock_tables(thd) holds in store_lock()) before executing the
procedure.
@return 0 or error code */

int
ha_innobase::start_stmt(
/*====================*/
	THD*		thd,	/*!< in: handle to the user thread */
	thr_lock_type	lock_type)
{
	trx_t*		trx = m_prebuilt->trx;

	DBUG_ENTER("ha_innobase::start_stmt");

	update_thd(thd);

	ut_ad(m_prebuilt->table != NULL);

	trx = m_prebuilt->trx;

	/* Reset the AUTOINC statement level counter for multi-row INSERTs. */
	trx->n_autoinc_rows = 0;

	m_prebuilt->sql_stat_start = TRUE;
	m_prebuilt->hint_need_to_fetch_extra_cols = 0;
	reset_template();

	if (m_prebuilt->table->is_temporary()
	    && m_mysql_has_locked
	    && m_prebuilt->select_lock_type == LOCK_NONE) {
		dberr_t error;

		switch (thd_sql_command(thd)) {
		case SQLCOM_INSERT:
		case SQLCOM_UPDATE:
		case SQLCOM_DELETE:
		case SQLCOM_REPLACE:
			init_table_handle_for_HANDLER();
			m_prebuilt->select_lock_type = LOCK_X;
			m_prebuilt->stored_select_lock_type = LOCK_X;
			error = row_lock_table(m_prebuilt);

			if (error != DB_SUCCESS) {
				int	st = convert_error_code_to_mysql(
					error, 0, thd);
				DBUG_RETURN(st);
			}
			break;
		}
	}

	if (!m_mysql_has_locked) {
		/* This handle is for a temporary table created inside
		this same LOCK TABLES; since MySQL does NOT call external_lock
		in this case, we must use x-row locks inside InnoDB to be
		prepared for an update of a row */

		m_prebuilt->select_lock_type = LOCK_X;

	} else if (trx->isolation_level != TRX_ISO_SERIALIZABLE
		   && thd_sql_command(thd) == SQLCOM_SELECT
		   && lock_type == TL_READ) {

		/* For other than temporary tables, we obtain
		no lock for consistent read (plain SELECT). */

		m_prebuilt->select_lock_type = LOCK_NONE;
	} else {
		/* Not a consistent read: restore the
		select_lock_type value. The value of
		stored_select_lock_type was decided in:
		1) ::store_lock(),
		2) ::external_lock(),
		3) ::init_table_handle_for_HANDLER(). */

		ut_a(m_prebuilt->stored_select_lock_type != LOCK_NONE_UNSET);

		m_prebuilt->select_lock_type =
			m_prebuilt->stored_select_lock_type;
	}

	*trx->detailed_error = 0;

	innobase_register_trx(ht, thd, trx);

	if (!trx_is_started(trx)) {
		++trx->will_lock;
	}

	DBUG_RETURN(0);
}

/******************************************************************//**
Maps a MySQL trx isolation level code to the InnoDB isolation level code
@return InnoDB isolation level */
static inline
uint
innobase_map_isolation_level(
/*=========================*/
	enum_tx_isolation	iso)	/*!< in: MySQL isolation level code */
{
	if (UNIV_UNLIKELY(srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN)
	    || UNIV_UNLIKELY(srv_read_only_mode)) {
		return TRX_ISO_READ_UNCOMMITTED;
	}
	switch (iso) {
	case ISO_REPEATABLE_READ:	return(TRX_ISO_REPEATABLE_READ);
	case ISO_READ_COMMITTED:	return(TRX_ISO_READ_COMMITTED);
	case ISO_SERIALIZABLE:		return(TRX_ISO_SERIALIZABLE);
	case ISO_READ_UNCOMMITTED:	return(TRX_ISO_READ_UNCOMMITTED);
	}

	ut_error;

	return(0);
}

/******************************************************************//**
As MySQL will execute an external lock for every new table it uses when it
starts to process an SQL statement (an exception is when MySQL calls
start_stmt for the handle) we can use this function to store the pointer to
the THD in the handle. We will also use this function to communicate
to InnoDB that a new SQL statement has started and that we must store a
savepoint to our transaction handle, so that we are able to roll back
the SQL statement in case of an error.
@return 0 */

int
ha_innobase::external_lock(
/*=======================*/
	THD*	thd,		/*!< in: handle to the user thread */
	int	lock_type)	/*!< in: lock type */
{
	DBUG_ENTER("ha_innobase::external_lock");
	DBUG_PRINT("enter",("lock_type: %d", lock_type));

	update_thd(thd);
	trx_t* trx = m_prebuilt->trx;
	ut_ad(m_prebuilt->table);

	/* Statement based binlogging does not work in isolation level
	READ UNCOMMITTED and READ COMMITTED since the necessary
	locks cannot be taken. In this case, we print an
	informative error message and return with an error.
	Note: decide_logging_format would give the same error message,
	except it cannot give the extra details. */

	if (lock_type == F_WRLCK
	    && !(table_flags() & HA_BINLOG_STMT_CAPABLE)
	    && thd_binlog_format(thd) == BINLOG_FORMAT_STMT
	    && thd_binlog_filter_ok(thd)
	    && thd_sqlcom_can_generate_row_events(thd)) {
		bool skip = false;
#ifdef WITH_WSREP
		skip = trx->is_wsrep() && !wsrep_thd_is_local(thd);
#endif /* WITH_WSREP */
		/* used by test case */
		DBUG_EXECUTE_IF("no_innodb_binlog_errors", skip = true;);

		if (!skip) {
			my_error(ER_BINLOG_STMT_MODE_AND_ROW_ENGINE, MYF(0),
			         " InnoDB is limited to row-logging when"
			         " transaction isolation level is"
			         " READ COMMITTED or READ UNCOMMITTED.");

			DBUG_RETURN(HA_ERR_LOGGING_IMPOSSIBLE);
		}
	}

	/* Check for UPDATEs in read-only mode. */
	if (srv_read_only_mode) {
		switch (thd_sql_command(thd)) {
		case SQLCOM_CREATE_TABLE:
			if (lock_type != F_WRLCK) {
				break;
			}
			/* fall through */
		case SQLCOM_UPDATE:
		case SQLCOM_INSERT:
		case SQLCOM_REPLACE:
		case SQLCOM_DROP_TABLE:
		case SQLCOM_ALTER_TABLE:
		case SQLCOM_OPTIMIZE:
		case SQLCOM_CREATE_INDEX:
		case SQLCOM_DROP_INDEX:
		case SQLCOM_CREATE_SEQUENCE:
		case SQLCOM_DROP_SEQUENCE:
		case SQLCOM_DELETE:
			ib_senderrf(thd, IB_LOG_LEVEL_WARN,
				    ER_READ_ONLY_MODE);
			DBUG_RETURN(HA_ERR_TABLE_READONLY);
		}
	}

	m_prebuilt->sql_stat_start = TRUE;
	m_prebuilt->hint_need_to_fetch_extra_cols = 0;

	reset_template();

	switch (m_prebuilt->table->quiesce) {
	case QUIESCE_START:
		/* Check for FLUSH TABLE t WITH READ LOCK; */
		if (!srv_read_only_mode
		    && thd_sql_command(thd) == SQLCOM_FLUSH
		    && lock_type == F_RDLCK) {

			if (!m_prebuilt->table->space) {
				ib_senderrf(trx->mysql_thd, IB_LOG_LEVEL_ERROR,
					    ER_TABLESPACE_DISCARDED,
					    table->s->table_name.str);

				DBUG_RETURN(HA_ERR_TABLESPACE_MISSING);
			}

			row_quiesce_table_start(m_prebuilt->table, trx);

			/* Use the transaction instance to track UNLOCK
			TABLES. It can be done via START TRANSACTION; too
			implicitly. */

			++trx->flush_tables;
		}
		break;

	case QUIESCE_COMPLETE:
		/* Check for UNLOCK TABLES; implicit or explicit
		or trx interruption. */
		if (trx->flush_tables > 0
		    && (lock_type == F_UNLCK || trx_is_interrupted(trx))) {

			row_quiesce_table_complete(m_prebuilt->table, trx);

			ut_a(trx->flush_tables > 0);
			--trx->flush_tables;
		}

		break;

	case QUIESCE_NONE:
		break;
	}

	if (lock_type == F_WRLCK) {

		/* If this is a SELECT, then it is in UPDATE TABLE ...
		or SELECT ... FOR UPDATE */
		m_prebuilt->select_lock_type = LOCK_X;
		m_prebuilt->stored_select_lock_type = LOCK_X;
	}

	if (lock_type != F_UNLCK) {
		/* MySQL is setting a new table lock */

		*trx->detailed_error = 0;

		innobase_register_trx(ht, thd, trx);

		if (trx->isolation_level == TRX_ISO_SERIALIZABLE
		    && m_prebuilt->select_lock_type == LOCK_NONE
		    && thd_test_options(
			    thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

			/* To get serializable execution, we let InnoDB
			conceptually add 'LOCK IN SHARE MODE' to all SELECTs
			which otherwise would have been consistent reads. An
			exception is consistent reads in the AUTOCOMMIT=1 mode:
			we know that they are read-only transactions, and they
			can be serialized also if performed as consistent
			reads. */

			m_prebuilt->select_lock_type = LOCK_S;
			m_prebuilt->stored_select_lock_type = LOCK_S;
		}

		/* Starting from 4.1.9, no InnoDB table lock is taken in LOCK
		TABLES if AUTOCOMMIT=1. It does not make much sense to acquire
		an InnoDB table lock if it is released immediately at the end
		of LOCK TABLES, and InnoDB's table locks in that case cause
		VERY easily deadlocks.

		We do not set InnoDB table locks if user has not explicitly
		requested a table lock. Note that thd_in_lock_tables(thd)
		can hold in some cases, e.g., at the start of a stored
		procedure call (SQLCOM_CALL). */

		if (m_prebuilt->select_lock_type != LOCK_NONE) {

			if (thd_sql_command(thd) == SQLCOM_LOCK_TABLES
			    && THDVAR(thd, table_locks)
			    && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT)
			    && thd_in_lock_tables(thd)) {

				dberr_t	error = row_lock_table(m_prebuilt);

				if (error != DB_SUCCESS) {

					DBUG_RETURN(
						convert_error_code_to_mysql(
							error, 0, thd));
				}
			}

			trx->mysql_n_tables_locked++;
		}

		trx->n_mysql_tables_in_use++;
		m_mysql_has_locked = true;

		if (!trx_is_started(trx)
		    && (m_prebuilt->select_lock_type != LOCK_NONE
			|| m_prebuilt->stored_select_lock_type != LOCK_NONE)) {

			++trx->will_lock;
		}

		DBUG_RETURN(0);
	} else {
		DEBUG_SYNC_C("ha_innobase_end_statement");
	}

	/* MySQL is releasing a table lock */

	trx->n_mysql_tables_in_use--;
	m_mysql_has_locked = false;

	/* If the MySQL lock count drops to zero we know that the current SQL
	statement has ended */

	if (trx->n_mysql_tables_in_use == 0) {

		trx->mysql_n_tables_locked = 0;
		m_prebuilt->used_in_HANDLER = FALSE;

		if (!thd_test_options(
				thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {

			if (trx_is_started(trx)) {

				innobase_commit(ht, thd, TRUE);
			}

		} else if (trx->isolation_level <= TRX_ISO_READ_COMMITTED) {
			trx->read_view.close();
		}
	}

	if (!trx_is_started(trx)
	    && lock_type != F_UNLCK
	    && (m_prebuilt->select_lock_type != LOCK_NONE
		|| m_prebuilt->stored_select_lock_type != LOCK_NONE)) {

		++trx->will_lock;
	}

	DBUG_RETURN(0);
}

/************************************************************************//**
Here we export InnoDB status variables to MySQL. */
static
void
innodb_export_status()
/*==================*/
{
	if (srv_was_started) {
		srv_export_innodb_status();
	}
}

/************************************************************************//**
Implements the SHOW ENGINE INNODB STATUS command. Sends the output of the
InnoDB Monitor to the client.
@return 0 on success */
static
int
innodb_show_status(
/*===============*/
	handlerton*	hton,	/*!< in: the innodb handlerton */
	THD*		thd,	/*!< in: the MySQL query thread of the caller */
	stat_print_fn*	stat_print)
{
	static const char	truncated_msg[] = "... truncated...\n";
	const long		MAX_STATUS_SIZE = 1048576;
	ulint			trx_list_start = ULINT_UNDEFINED;
	ulint			trx_list_end = ULINT_UNDEFINED;
	bool			ret_val;

	DBUG_ENTER("innodb_show_status");
	DBUG_ASSERT(hton == innodb_hton_ptr);

	/* We don't create the temp files or associated
	mutexes in read-only-mode */

	if (srv_read_only_mode) {
		DBUG_RETURN(0);
	}

	srv_wake_purge_thread_if_not_active();

	/* We let the InnoDB Monitor to output at most MAX_STATUS_SIZE
	bytes of text. */

	char*	str;
	size_t	flen;

	mutex_enter(&srv_monitor_file_mutex);
	rewind(srv_monitor_file);

	srv_printf_innodb_monitor(srv_monitor_file, FALSE,
				  &trx_list_start, &trx_list_end);

	os_file_set_eof(srv_monitor_file);

	flen = size_t(ftell(srv_monitor_file));
	if (ssize_t(flen) < 0) {
		flen = 0;
	}

	size_t	usable_len;

	if (flen > MAX_STATUS_SIZE) {
		usable_len = MAX_STATUS_SIZE;
		srv_truncated_status_writes++;
	} else {
		usable_len = flen;
	}

	/* allocate buffer for the string, and
	read the contents of the temporary file */

	if (!(str = (char*) my_malloc(PSI_INSTRUMENT_ME,
		      usable_len + 1, MYF(0)))) {
		mutex_exit(&srv_monitor_file_mutex);
		DBUG_RETURN(1);
	}

	rewind(srv_monitor_file);

	if (flen < MAX_STATUS_SIZE) {
		/* Display the entire output. */
		flen = fread(str, 1, flen, srv_monitor_file);
	} else if (trx_list_end < flen
		   && trx_list_start < trx_list_end
		   && trx_list_start + flen - trx_list_end
		   < MAX_STATUS_SIZE - sizeof truncated_msg - 1) {

		/* Omit the beginning of the list of active transactions. */
		size_t	len = fread(str, 1, trx_list_start, srv_monitor_file);

		memcpy(str + len, truncated_msg, sizeof truncated_msg - 1);
		len += sizeof truncated_msg - 1;
		usable_len = (MAX_STATUS_SIZE - 1) - len;
		fseek(srv_monitor_file, long(flen - usable_len), SEEK_SET);
		len += fread(str + len, 1, usable_len, srv_monitor_file);
		flen = len;
	} else {
		/* Omit the end of the output. */
		flen = fread(str, 1, MAX_STATUS_SIZE - 1, srv_monitor_file);
	}

	mutex_exit(&srv_monitor_file_mutex);

	ret_val= stat_print(
		thd, innobase_hton_name,
		static_cast<uint>(strlen(innobase_hton_name)),
		STRING_WITH_LEN(""), str, static_cast<uint>(flen));

	my_free(str);

	DBUG_RETURN(ret_val);
}

/** Callback for collecting mutex statistics */
struct ShowStatus {

	/** For tracking the mutex metrics */
	struct Value {

		/** Constructor
		@param[in]	name		Name of the mutex
		@param[in]	spins		Number of spins
		@param[in]	os_waits	OS waits so far
		@param[in]	calls		Number of calls to enter() */
		Value(const char*	name,
		      ulint		spins,
		      uint64_t		waits,
		      uint64_t		calls)
			:
			m_name(name),
			m_spins(spins),
			m_waits(waits),
			m_calls(calls)
		{
			/* No op */
		}

		/** Mutex name */
		std::string		m_name;

		/** Spins so far */
		ulint			m_spins;

		/** Waits so far */
		uint64_t		m_waits;

		/** Number of calls so far */
		uint64_t		m_calls;
	};

	/** Order by m_waits, in descending order. */
	struct OrderByWaits: public std::binary_function<Value, Value, bool>
	{
		/** @return true if rhs < lhs */
		bool operator()(
			const Value& lhs,
			const Value& rhs) const
			UNIV_NOTHROW
		{
			return(rhs.m_waits < lhs.m_waits);
		}
	};

	typedef std::vector<Value, ut_allocator<Value> > Values;

	/** Collect the individual latch counts */
	struct GetCount {
		typedef latch_meta_t::CounterType::Count Count;

		/** Constructor
		@param[in]	name		Latch name
		@param[in,out]	values		Put the values here */
		GetCount(
			const char*	name,
			Values*		values)
			UNIV_NOTHROW
			:
			m_name(name),
			m_values(values)
		{
			/* No op */
		}

		/** Collect the latch metrics. Ignore entries where the
		spins and waits are zero.
		@param[in]	count		The latch metrics */
		void operator()(Count* count) const UNIV_NOTHROW
		{
			if (count->m_spins > 0 || count->m_waits > 0) {

				m_values->push_back(Value(
					m_name,
					count->m_spins,
					count->m_waits,
					count->m_calls));
			}
		}

		/** The latch name */
		const char*	m_name;

		/** For collecting the active mutex stats. */
		Values*		m_values;
	};

	/** Constructor */
	ShowStatus() { }

	/** Callback for collecting the stats
	@param[in]	latch_meta		Latch meta data
	@return always returns true */
	bool operator()(latch_meta_t& latch_meta)
		UNIV_NOTHROW
	{
		latch_meta.get_counter()->iterate(
			GetCount(latch_meta.get_name(), &m_values));

		return(true);
	}

	/** Implements the SHOW MUTEX STATUS command, for mutexes.
	The table structure is like so: Engine | Mutex Name | Status
	We store the metrics  in the "Status" column as:

		spins=N,waits=N,calls=N"

	The user has to parse the dataunfortunately
	@param[in,out]	thd		the MySQL query thread of the caller
	@param[in,out]	stat_print	function for printing statistics
	@return true on success. */
	bool to_string(
		THD*		thd,
		stat_print_fn*	stat_print)
		UNIV_NOTHROW;

	/** For collecting the active mutex stats. */
	Values		m_values;
};

/** Implements the SHOW MUTEX STATUS command, for mutexes.
The table structure is like so: Engine | Mutex Name | Status
We store the metrics  in the "Status" column as:

	spins=N,waits=N,calls=N"

The user has to parse the dataunfortunately
@param[in,out]	thd		the MySQL query thread of the caller
@param[in,out]	stat_print	function for printing statistics
@return true on success. */
bool
ShowStatus::to_string(
	THD*		thd,
	stat_print_fn*	stat_print)
	UNIV_NOTHROW
{
	uint		hton_name_len = (uint) strlen(innobase_hton_name);

	std::sort(m_values.begin(), m_values.end(), OrderByWaits());

	Values::iterator	end = m_values.end();

	for (Values::iterator it = m_values.begin(); it != end; ++it) {

		int	name_len;
		char	name_buf[IO_SIZE];

		name_len = snprintf(
			name_buf, sizeof(name_buf), "%s", it->m_name.c_str());

		int	status_len;
		char	status_buf[IO_SIZE];

		status_len = snprintf(
			status_buf, sizeof(status_buf),
			"spins=%lu,waits=%lu,calls=%llu",
			static_cast<ulong>(it->m_spins),
			static_cast<long>(it->m_waits),
			(ulonglong) it->m_calls);

		if (stat_print(thd, innobase_hton_name,
			       hton_name_len,
			       name_buf, static_cast<uint>(name_len),
			       status_buf, static_cast<uint>(status_len))) {

			return(false);
		}
	}

	return(true);
}

/** Implements the SHOW MUTEX STATUS command, for mutexes.
@param[in,out]	hton		the innodb handlerton
@param[in,out]	thd		the MySQL query thread of the caller
@param[in,out]	stat_print	function for printing statistics
@return 0 on success. */
static
int
innodb_show_mutex_status(
	handlerton*
#ifdef DBUG_ASSERT_EXISTS
	hton
#endif
	,
	THD*		thd,
	stat_print_fn*	stat_print)
{
	DBUG_ENTER("innodb_show_mutex_status");

	ShowStatus	collector;

	DBUG_ASSERT(hton == innodb_hton_ptr);

	mutex_monitor.iterate(collector);

	if (!collector.to_string(thd, stat_print)) {
		DBUG_RETURN(1);
	}

	DBUG_RETURN(0);
}

/** Implements the SHOW MUTEX STATUS command.
@param[in,out]	hton		the innodb handlerton
@param[in,out]	thd		the MySQL query thread of the caller
@param[in,out]	stat_print	function for printing statistics
@return 0 on success. */
static
int
innodb_show_rwlock_status(
	handlerton*
#ifdef DBUG_ASSERT_EXISTS
	hton
#endif
	,
	THD*		thd,
	stat_print_fn*	stat_print)
{
	DBUG_ENTER("innodb_show_rwlock_status");

	const rw_lock_t* block_rwlock= nullptr;
	ulint		block_rwlock_oswait_count = 0;
	uint		hton_name_len = (uint) strlen(innobase_hton_name);

	DBUG_ASSERT(hton == innodb_hton_ptr);

	mutex_enter(&rw_lock_list_mutex);

	for (const rw_lock_t& rw_lock : rw_lock_list) {

		if (rw_lock.count_os_wait == 0) {
			continue;
		}

		int		buf1len;
		char		buf1[IO_SIZE];

		if (rw_lock.is_block_lock) {

			block_rwlock = &rw_lock;
			block_rwlock_oswait_count += rw_lock.count_os_wait;

			continue;
		}

		buf1len = snprintf(
			buf1, sizeof buf1, "rwlock: %s:%u",
			innobase_basename(rw_lock.cfile_name),
			rw_lock.cline);

		int		buf2len;
		char		buf2[IO_SIZE];

		buf2len = snprintf(
			buf2, sizeof buf2, "waits=%u",
			rw_lock.count_os_wait);

		if (stat_print(thd, innobase_hton_name,
			       hton_name_len,
			       buf1, static_cast<uint>(buf1len),
			       buf2, static_cast<uint>(buf2len))) {

			mutex_exit(&rw_lock_list_mutex);

			DBUG_RETURN(1);
		}
	}

	if (block_rwlock != NULL) {

		int		buf1len;
		char		buf1[IO_SIZE];

		buf1len = snprintf(
			buf1, sizeof buf1, "sum rwlock: %s:%u",
			innobase_basename(block_rwlock->cfile_name),
			block_rwlock->cline);

		int		buf2len;
		char		buf2[IO_SIZE];

		buf2len = snprintf(
			buf2, sizeof buf2, "waits=" ULINTPF,
			block_rwlock_oswait_count);

		if (stat_print(thd, innobase_hton_name,
			       hton_name_len,
			       buf1, static_cast<uint>(buf1len),
			       buf2, static_cast<uint>(buf2len))) {

			mutex_exit(&rw_lock_list_mutex);

			DBUG_RETURN(1);
		}
	}

	mutex_exit(&rw_lock_list_mutex);

	DBUG_RETURN(0);
}

/** Implements the SHOW MUTEX STATUS command.
@param[in,out]	hton		the innodb handlerton
@param[in,out]	thd		the MySQL query thread of the caller
@param[in,out]	stat_print	function for printing statistics
@return 0 on success. */
static
int
innodb_show_latch_status(
	handlerton*	hton,
	THD*		thd,
	stat_print_fn*	stat_print)
{
	int	ret = innodb_show_mutex_status(hton, thd, stat_print);

	if (ret != 0) {
		return(ret);
	}

	return(innodb_show_rwlock_status(hton, thd, stat_print));
}

/************************************************************************//**
Return 0 on success and non-zero on failure. Note: the bool return type
seems to be abused here, should be an int. */
static
bool
innobase_show_status(
/*=================*/
	handlerton*		hton,	/*!< in: the innodb handlerton */
	THD*			thd,	/*!< in: the MySQL query thread
					of the caller */
	stat_print_fn*		stat_print,
	enum ha_stat_type	stat_type)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	switch (stat_type) {
	case HA_ENGINE_STATUS:
		/* Non-zero return value means there was an error. */
		return(innodb_show_status(hton, thd, stat_print) != 0);

	case HA_ENGINE_MUTEX:
		return(innodb_show_latch_status(hton, thd, stat_print) != 0);

	case HA_ENGINE_LOGS:
		/* Not handled */
		break;
	}

	/* Success */
	return(false);
}
/*********************************************************************//**
Returns number of THR_LOCK locks used for one instance of InnoDB table.
InnoDB no longer relies on THR_LOCK locks so 0 value is returned.
Instead of THR_LOCK locks InnoDB relies on combination of metadata locks
(e.g. for LOCK TABLES and DDL) and its own locking subsystem.
Note that even though this method returns 0, SQL-layer still calls
::store_lock(), ::start_stmt() and ::external_lock() methods for InnoDB
tables. */

uint
ha_innobase::lock_count(void) const
/*===============================*/
{
	return 0;
}

/*****************************************************************//**
Supposed to convert a MySQL table lock stored in the 'lock' field of the
handle to a proper type before storing pointer to the lock into an array
of pointers.
In practice, since InnoDB no longer relies on THR_LOCK locks and its
lock_count() method returns 0 it just informs storage engine about type
of THR_LOCK which SQL-layer would have acquired for this specific statement
on this specific table.
MySQL also calls this if it wants to reset some table locks to a not-locked
state during the processing of an SQL query. An example is that during a
SELECT the read lock is released early on the 'const' tables where we only
fetch one row. MySQL does not call this when it releases all locks at the
end of an SQL statement.
@return pointer to the current element in the 'to' array. */

THR_LOCK_DATA**
ha_innobase::store_lock(
/*====================*/
	THD*			thd,		/*!< in: user thread handle */
	THR_LOCK_DATA**		to,		/*!< in: pointer to the current
						element in an array of pointers
						to lock structs;
						only used as return value */
	thr_lock_type		lock_type)	/*!< in: lock type to store in
						'lock'; this may also be
						TL_IGNORE */
{
	/* Note that trx in this function is NOT necessarily m_prebuilt->trx
	because we call update_thd() later, in ::external_lock()! Failure to
	understand this caused a serious memory corruption bug in 5.1.11. */

	trx_t*	trx = check_trx_exists(thd);

	/* NOTE: MySQL can call this function with lock 'type' TL_IGNORE!
	Be careful to ignore TL_IGNORE if we are going to do something with
	only 'real' locks! */

	/* If no MySQL table is in use, we need to set the isolation level
	of the transaction. */

	if (lock_type != TL_IGNORE
	    && trx->n_mysql_tables_in_use == 0) {
		trx->isolation_level = innobase_map_isolation_level(
			(enum_tx_isolation) thd_tx_isolation(thd));

		if (trx->isolation_level <= TRX_ISO_READ_COMMITTED) {

			/* At low transaction isolation levels we let
			each consistent read set its own snapshot */
			trx->read_view.close();
		}
	}

	DBUG_ASSERT(EQ_CURRENT_THD(thd));
	const bool in_lock_tables = thd_in_lock_tables(thd);
	const int sql_command = thd_sql_command(thd);

	if (srv_read_only_mode
	    && (sql_command == SQLCOM_UPDATE
		|| sql_command == SQLCOM_INSERT
		|| sql_command == SQLCOM_REPLACE
		|| sql_command == SQLCOM_DROP_TABLE
		|| sql_command == SQLCOM_ALTER_TABLE
		|| sql_command == SQLCOM_OPTIMIZE
		|| (sql_command == SQLCOM_CREATE_TABLE
		    && (lock_type >= TL_WRITE_CONCURRENT_INSERT
			 && lock_type <= TL_WRITE))
		|| sql_command == SQLCOM_CREATE_INDEX
		|| sql_command == SQLCOM_DROP_INDEX
		|| sql_command == SQLCOM_CREATE_SEQUENCE
		|| sql_command == SQLCOM_DROP_SEQUENCE
		|| sql_command == SQLCOM_DELETE)) {

		ib_senderrf(trx->mysql_thd,
			    IB_LOG_LEVEL_WARN, ER_READ_ONLY_MODE);

	} else if (sql_command == SQLCOM_FLUSH
		   && lock_type == TL_READ_NO_INSERT) {

		/* Check for FLUSH TABLES ... WITH READ LOCK */

		/* Note: This call can fail, but there is no way to return
		the error to the caller. We simply ignore it for now here
		and push the error code to the caller where the error is
		detected in the function. */

		dberr_t	err = row_quiesce_set_state(
			m_prebuilt->table, QUIESCE_START, trx);

		ut_a(err == DB_SUCCESS || err == DB_UNSUPPORTED);

		if (trx->isolation_level == TRX_ISO_SERIALIZABLE) {
			m_prebuilt->select_lock_type = LOCK_S;
			m_prebuilt->stored_select_lock_type = LOCK_S;
		} else {
			m_prebuilt->select_lock_type = LOCK_NONE;
			m_prebuilt->stored_select_lock_type = LOCK_NONE;
		}

	/* Check for DROP TABLE */
	} else if (sql_command == SQLCOM_DROP_TABLE ||
                   sql_command == SQLCOM_DROP_SEQUENCE) {

		/* MySQL calls this function in DROP TABLE though this table
		handle may belong to another thd that is running a query. Let
		us in that case skip any changes to the m_prebuilt struct. */

	/* Check for LOCK TABLE t1,...,tn WITH SHARED LOCKS */
	} else if ((lock_type == TL_READ && in_lock_tables)
		   || (lock_type == TL_READ_HIGH_PRIORITY && in_lock_tables)
		   || lock_type == TL_READ_WITH_SHARED_LOCKS
		   || lock_type == TL_READ_NO_INSERT
		   || (lock_type != TL_IGNORE
		       && sql_command != SQLCOM_SELECT)) {

		/* The OR cases above are in this order:
		1) MySQL is doing LOCK TABLES ... READ LOCAL, or we
		are processing a stored procedure or function, or
		2) (we do not know when TL_READ_HIGH_PRIORITY is used), or
		3) this is a SELECT ... IN SHARE MODE, or
		4) we are doing a complex SQL statement like
		INSERT INTO ... SELECT ... and the logical logging (MySQL
		binlog) requires the use of a locking read, or
		MySQL is doing LOCK TABLES ... READ.
		5) we let InnoDB do locking reads for all SQL statements that
		are not simple SELECTs; note that select_lock_type in this
		case may get strengthened in ::external_lock() to LOCK_X.
		Note that we MUST use a locking read in all data modifying
		SQL statements, because otherwise the execution would not be
		serializable, and also the results from the update could be
		unexpected if an obsolete consistent read view would be
		used. */

		/* Use consistent read for checksum table */

		if (sql_command == SQLCOM_CHECKSUM
		    || sql_command == SQLCOM_CREATE_SEQUENCE
		    || (sql_command == SQLCOM_ANALYZE && lock_type == TL_READ)
		    || (trx->isolation_level <= TRX_ISO_READ_COMMITTED
			&& (lock_type == TL_READ
			    || lock_type == TL_READ_NO_INSERT)
			&& (sql_command == SQLCOM_INSERT_SELECT
			    || sql_command == SQLCOM_REPLACE_SELECT
			    || sql_command == SQLCOM_UPDATE
			    || sql_command == SQLCOM_CREATE_SEQUENCE
			    || sql_command == SQLCOM_CREATE_TABLE))) {

			/* If the transaction isolation level is
			READ UNCOMMITTED or READ COMMITTED and we are executing
			INSERT INTO...SELECT or REPLACE INTO...SELECT
			or UPDATE ... = (SELECT ...) or CREATE  ...
			SELECT... without FOR UPDATE or IN SHARE
			MODE in select, then we use consistent read
			for select. */

			m_prebuilt->select_lock_type = LOCK_NONE;
			m_prebuilt->stored_select_lock_type = LOCK_NONE;
		} else {
			m_prebuilt->select_lock_type = LOCK_S;
			m_prebuilt->stored_select_lock_type = LOCK_S;
		}

	} else if (lock_type != TL_IGNORE) {

		/* We set possible LOCK_X value in external_lock, not yet
		here even if this would be SELECT ... FOR UPDATE */

		m_prebuilt->select_lock_type = LOCK_NONE;
		m_prebuilt->stored_select_lock_type = LOCK_NONE;
	}

	if (!trx_is_started(trx)
	    && (m_prebuilt->select_lock_type != LOCK_NONE
	        || m_prebuilt->stored_select_lock_type != LOCK_NONE)) {

		++trx->will_lock;
	}

	return(to);
}

/*********************************************************************//**
Read the next autoinc value. Acquire the relevant locks before reading
the AUTOINC value. If SUCCESS then the table AUTOINC mutex will be locked
on return and all relevant locks acquired.
@return DB_SUCCESS or error code */

dberr_t
ha_innobase::innobase_get_autoinc(
/*==============================*/
	ulonglong*	value)		/*!< out: autoinc value */
{
	*value = 0;

	m_prebuilt->autoinc_error = innobase_lock_autoinc();

	if (m_prebuilt->autoinc_error == DB_SUCCESS) {

		/* Determine the first value of the interval */
		*value = dict_table_autoinc_read(m_prebuilt->table);

		/* It should have been initialized during open. */
		if (*value == 0) {
			m_prebuilt->autoinc_error = DB_UNSUPPORTED;
			m_prebuilt->table->autoinc_mutex.unlock();
		}
	}

	return(m_prebuilt->autoinc_error);
}

/*******************************************************************//**
This function reads the global auto-inc counter. It doesn't use the
AUTOINC lock even if the lock mode is set to TRADITIONAL.
@return the autoinc value */

ulonglong
ha_innobase::innobase_peek_autoinc(void)
/*====================================*/
{
	ulonglong	auto_inc;
	dict_table_t*	innodb_table;

	ut_a(m_prebuilt != NULL);
	ut_a(m_prebuilt->table != NULL);

	innodb_table = m_prebuilt->table;

	innodb_table->autoinc_mutex.lock();

	auto_inc = dict_table_autoinc_read(innodb_table);

	if (auto_inc == 0) {
		ib::info() << "AUTOINC next value generation is disabled for"
			" '" << innodb_table->name << "'";
	}

	innodb_table->autoinc_mutex.unlock();

	return(auto_inc);
}

/*********************************************************************//**
Returns the value of the auto-inc counter in *first_value and ~0 on failure. */

void
ha_innobase::get_auto_increment(
/*============================*/
	ulonglong	offset,			/*!< in: table autoinc offset */
	ulonglong	increment,		/*!< in: table autoinc
						increment */
	ulonglong	nb_desired_values,	/*!< in: number of values
						reqd */
	ulonglong*	first_value,		/*!< out: the autoinc value */
	ulonglong*	nb_reserved_values)	/*!< out: count of reserved
						values */
{
	trx_t*		trx;
	dberr_t		error;
	ulonglong	autoinc = 0;

	/* Prepare m_prebuilt->trx in the table handle */
	update_thd(ha_thd());

	error = innobase_get_autoinc(&autoinc);

	if (error != DB_SUCCESS) {
		*first_value = (~(ulonglong) 0);
		return;
	}

	/* This is a hack, since nb_desired_values seems to be accurate only
	for the first call to get_auto_increment() for multi-row INSERT and
	meaningless for other statements e.g, LOAD etc. Subsequent calls to
	this method for the same statement results in different values which
	don't make sense. Therefore we store the value the first time we are
	called and count down from that as rows are written (see write_row()).
	*/

	trx = m_prebuilt->trx;

	/* Note: We can't rely on *first_value since some MySQL engines,
	in particular the partition engine, don't initialize it to 0 when
	invoking this method. So we are not sure if it's guaranteed to
	be 0 or not. */

	/* We need the upper limit of the col type to check for
	whether we update the table autoinc counter or not. */
	ulonglong col_max_value =
			table->next_number_field->get_max_int_value();

	/** The following logic is needed to avoid duplicate key error
	for autoincrement column.

	(1) InnoDB gives the current autoincrement value with respect
	to increment and offset value.

	(2) Basically it does compute_next_insert_id() logic inside InnoDB
	to avoid the current auto increment value changed by handler layer.

	(3) It is restricted only for insert operations. */

	if (increment > 1 && thd_sql_command(m_user_thd) != SQLCOM_ALTER_TABLE
	    && autoinc < col_max_value) {

		ulonglong prev_auto_inc = autoinc;

		autoinc = ((autoinc - 1) + increment - offset)/ increment;

		autoinc = autoinc * increment + offset;

		/* If autoinc exceeds the col_max_value then reset
		to old autoinc value. Because in case of non-strict
		sql mode, boundary value is not considered as error. */

		if (autoinc >= col_max_value) {
			autoinc = prev_auto_inc;
		}

		ut_ad(autoinc > 0);
	}

	/* Called for the first time ? */
	if (trx->n_autoinc_rows == 0) {

		trx->n_autoinc_rows = (ulint) nb_desired_values;

		/* It's possible for nb_desired_values to be 0:
		e.g., INSERT INTO T1(C) SELECT C FROM T2; */
		if (nb_desired_values == 0) {

			trx->n_autoinc_rows = 1;
		}

		set_if_bigger(*first_value, autoinc);
	/* Not in the middle of a mult-row INSERT. */
	} else if (m_prebuilt->autoinc_last_value == 0) {
		set_if_bigger(*first_value, autoinc);
	}

	if (*first_value > col_max_value) {
		/* Out of range number. Let handler::update_auto_increment()
		take care of this */
		m_prebuilt->autoinc_last_value = 0;
		m_prebuilt->table->autoinc_mutex.unlock();
		*nb_reserved_values= 0;
		return;
	}

	*nb_reserved_values = trx->n_autoinc_rows;

	/* With old style AUTOINC locking we only update the table's
	AUTOINC counter after attempting to insert the row. */
	if (innobase_autoinc_lock_mode != AUTOINC_OLD_STYLE_LOCKING) {
		ulonglong	current;
		ulonglong	next_value;

		current = *first_value;

		/* Compute the last value in the interval */
		next_value = innobase_next_autoinc(
			current, *nb_reserved_values, increment, offset,
			col_max_value);

		m_prebuilt->autoinc_last_value = next_value;

		if (m_prebuilt->autoinc_last_value < *first_value) {
			*first_value = (~(ulonglong) 0);
		} else {
			/* Update the table autoinc variable */
			dict_table_autoinc_update_if_greater(
				m_prebuilt->table,
				m_prebuilt->autoinc_last_value);
		}
	} else {
		/* This will force write_row() into attempting an update
		of the table's AUTOINC counter. */
		m_prebuilt->autoinc_last_value = 0;
	}

	/* The increment to be used to increase the AUTOINC value, we use
	this in write_row() and update_row() to increase the autoinc counter
	for columns that are filled by the user. We need the offset and
	the increment. */
	m_prebuilt->autoinc_offset = offset;
	m_prebuilt->autoinc_increment = increment;

	m_prebuilt->table->autoinc_mutex.unlock();
}

/*******************************************************************//**
See comment in handler.cc */

bool
ha_innobase::get_error_message(
/*===========================*/
	int	error,
	String*	buf)
{
	trx_t*	trx = check_trx_exists(ha_thd());

	if (error == HA_ERR_DECRYPTION_FAILED) {
		const char *msg = "Table encrypted but decryption failed. This could be because correct encryption management plugin is not loaded, used encryption key is not available or encryption method does not match.";
		buf->copy(msg, (uint)strlen(msg), system_charset_info);
	} else {
		buf->copy(trx->detailed_error, (uint) strlen(trx->detailed_error),
			system_charset_info);
	}

	return(FALSE);
}

/** Retrieves the names of the table and the key for which there was a
duplicate entry in the case of HA_ERR_FOREIGN_DUPLICATE_KEY.

If any of the names is not available, then this method will return
false and will not change any of child_table_name or child_key_name.

@param[out] child_table_name Table name
@param[in] child_table_name_len Table name buffer size
@param[out] child_key_name Key name
@param[in] child_key_name_len Key name buffer size

@retval true table and key names were available and were written into the
corresponding out parameters.
@retval false table and key names were not available, the out parameters
were not touched. */
bool
ha_innobase::get_foreign_dup_key(
/*=============================*/
	char*	child_table_name,
	uint	child_table_name_len,
	char*	child_key_name,
	uint	child_key_name_len)
{
	const dict_index_t*	err_index;

	ut_a(m_prebuilt->trx != NULL);
	ut_a(m_prebuilt->trx->magic_n == TRX_MAGIC_N);

	err_index = trx_get_error_info(m_prebuilt->trx);

	if (err_index == NULL) {
		return(false);
	}
	/* else */

	/* copy table name (and convert from filename-safe encoding to
	system_charset_info) */
	char*	p = strchr(err_index->table->name.m_name, '/');

	/* strip ".../" prefix if any */
	if (p != NULL) {
		p++;
	} else {
		p = err_index->table->name.m_name;
	}

	size_t	len;

	len = filename_to_tablename(p, child_table_name, child_table_name_len);

	child_table_name[len] = '\0';

	/* copy index name */
	snprintf(child_key_name, child_key_name_len, "%s",
		    err_index->name());

	return(true);
}

/*******************************************************************//**
Compares two 'refs'. A 'ref' is the (internal) primary key value of the row.
If there is no explicitly declared non-null unique key or a primary key, then
InnoDB internally uses the row id as the primary key.
@return < 0 if ref1 < ref2, 0 if equal, else > 0 */

int
ha_innobase::cmp_ref(
/*=================*/
	const uchar*	ref1,	/*!< in: an (internal) primary key value in the
				MySQL key value format */
	const uchar*	ref2)	/*!< in: an (internal) primary key value in the
				MySQL key value format */
{
	enum_field_types mysql_type;
	Field*		field;
	KEY_PART_INFO*	key_part;
	KEY_PART_INFO*	key_part_end;
	uint		len1;
	uint		len2;
	int		result;

	if (m_prebuilt->clust_index_was_generated) {
		/* The 'ref' is an InnoDB row id */

		return(memcmp(ref1, ref2, DATA_ROW_ID_LEN));
	}

	/* Do a type-aware comparison of primary key fields. PK fields
	are always NOT NULL, so no checks for NULL are performed. */

	key_part = table->key_info[table->s->primary_key].key_part;

	key_part_end = key_part
		+ table->key_info[table->s->primary_key].user_defined_key_parts;

	for (; key_part != key_part_end; ++key_part) {
		field = key_part->field;
		mysql_type = field->type();

		if (mysql_type == MYSQL_TYPE_TINY_BLOB
			|| mysql_type == MYSQL_TYPE_MEDIUM_BLOB
			|| mysql_type == MYSQL_TYPE_BLOB
			|| mysql_type == MYSQL_TYPE_LONG_BLOB) {

			/* In the MySQL key value format, a column prefix of
			a BLOB is preceded by a 2-byte length field */

			len1 = innobase_read_from_2_little_endian(ref1);
			len2 = innobase_read_from_2_little_endian(ref2);

			result = ((Field_blob*) field)->cmp(
				ref1 + 2, len1, ref2 + 2, len2);
		} else {
			result = field->key_cmp(ref1, ref2);
		}

		if (result) {

			return(result);
		}

		ref1 += key_part->store_length;
		ref2 += key_part->store_length;
	}

	return(0);
}

/*******************************************************************//**
Ask InnoDB if a query to a table can be cached.
@return TRUE if query caching of the table is permitted */

my_bool
ha_innobase::register_query_cache_table(
/*====================================*/
	THD*		thd,		/*!< in: user thread handle */
	const char*	table_key,	/*!< in: normalized path to the
					table */
	uint		key_length,	/*!< in: length of the normalized
					path to the table */
	qc_engine_callback*
			call_back,	/*!< out: pointer to function for
					checking if query caching
					is permitted */
	ulonglong	*engine_data)	/*!< in/out: data to call_back */
{
	*engine_data = 0;
	*call_back = innobase_query_caching_of_table_permitted;

	return(innobase_query_caching_of_table_permitted(
			thd, table_key,
			static_cast<uint>(key_length),
			engine_data));
}

/******************************************************************//**
This function is used to find the storage length in bytes of the first n
characters for prefix indexes using a multibyte character set. The function
finds charset information and returns length of prefix_len characters in the
index field in bytes.
@return number of bytes occupied by the first n characters */
ulint
innobase_get_at_most_n_mbchars(
/*===========================*/
	ulint charset_id,	/*!< in: character set id */
	ulint prefix_len,	/*!< in: prefix length in bytes of the index
				(this has to be divided by mbmaxlen to get the
				number of CHARACTERS n in the prefix) */
	ulint data_len,		/*!< in: length of the string in bytes */
	const char* str)	/*!< in: character string */
{
	ulint char_length;	/*!< character length in bytes */
	ulint n_chars;		/*!< number of characters in prefix */
	CHARSET_INFO* charset;	/*!< charset used in the field */

	charset = get_charset((uint) charset_id, MYF(MY_WME));

	ut_ad(charset);
	ut_ad(charset->mbmaxlen);

	/* Calculate how many characters at most the prefix index contains */

	n_chars = prefix_len / charset->mbmaxlen;

	/* If the charset is multi-byte, then we must find the length of the
	first at most n chars in the string. If the string contains less
	characters than n, then we return the length to the end of the last
	character. */

	if (charset->mbmaxlen > 1) {
		/* charpos() returns the byte length of the first n_chars
		characters, or a value bigger than the length of str, if
		there were not enough full characters in str.

		Why does the code below work:
		Suppose that we are looking for n UTF-8 characters.

		1) If the string is long enough, then the prefix contains at
		least n complete UTF-8 characters + maybe some extra
		characters + an incomplete UTF-8 character. No problem in
		this case. The function returns the pointer to the
		end of the nth character.

		2) If the string is not long enough, then the string contains
		the complete value of a column, that is, only complete UTF-8
		characters, and we can store in the column prefix index the
		whole string. */

		char_length= charset->charpos(str, str + data_len, n_chars);
		if (char_length > data_len) {
			char_length = data_len;
		}
	} else if (data_len < prefix_len) {

		char_length = data_len;

	} else {

		char_length = prefix_len;
	}

	return(char_length);
}

/*******************************************************************//**
This function is used to prepare an X/Open XA distributed transaction.
@return 0 or error number */
static
int
innobase_xa_prepare(
/*================*/
	handlerton*	hton,		/*!< in: InnoDB handlerton */
	THD*		thd,		/*!< in: handle to the MySQL thread of
					the user whose XA transaction should
					be prepared */
	bool		prepare_trx)	/*!< in: true - prepare transaction
					false - the current SQL statement
					ended */
{
	trx_t*		trx = check_trx_exists(thd);

	DBUG_ASSERT(hton == innodb_hton_ptr);

	thd_get_xid(thd, (MYSQL_XID*) trx->xid);

	if (!trx_is_registered_for_2pc(trx) && trx_is_started(trx)) {

		sql_print_error("Transaction not registered for MariaDB 2PC,"
				" but transaction is active");
	}

	if (prepare_trx
	    || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {

		/* We were instructed to prepare the whole transaction, or
		this is an SQL statement end and autocommit is on */

		ut_ad(trx_is_registered_for_2pc(trx));

		trx_prepare_for_mysql(trx);
	} else {
		/* We just mark the SQL statement ended and do not do a
		transaction prepare */

		/* If we had reserved the auto-inc lock for some
		table in this SQL statement we release it now */

		lock_unlock_table_autoinc(trx);

		/* Store the current undo_no of the transaction so that we
		know where to roll back if we have to roll back the next
		SQL statement */

		trx_mark_sql_stat_end(trx);
	}

	if (thd_sql_command(thd) != SQLCOM_XA_PREPARE
	    && (prepare_trx
		|| !thd_test_options(
			thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {

		/* For mysqlbackup to work the order of transactions in binlog
		and InnoDB must be the same. Consider the situation

		  thread1> prepare; write to binlog; ...
			  <context switch>
		  thread2> prepare; write to binlog; commit
		  thread1>			     ... commit

		The server guarantees that writes to the binary log
		and commits are in the same order, so we do not have
		to handle this case. */
	}

	return(0);
}

/*******************************************************************//**
This function is used to recover X/Open XA distributed transactions.
@return number of prepared transactions stored in xid_list */
static
int
innobase_xa_recover(
/*================*/
	handlerton*	hton,	/*!< in: InnoDB handlerton */
	XID*		xid_list,/*!< in/out: prepared transactions */
	uint		len)	/*!< in: number of slots in xid_list */
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (len == 0 || xid_list == NULL) {

		return(0);
	}

	return(trx_recover_for_mysql(xid_list, len));
}

/*******************************************************************//**
This function is used to commit one X/Open XA distributed transaction
which is in the prepared state
@return 0 or error number */
static
int
innobase_commit_by_xid(
/*===================*/
	handlerton*	hton,
	XID*		xid)	/*!< in: X/Open XA transaction identification */
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	DBUG_EXECUTE_IF("innobase_xa_fail",
			return XAER_RMFAIL;);

	if (high_level_read_only) {
		return(XAER_RMFAIL);
	}

	if (trx_t* trx = trx_get_trx_by_xid(xid)) {
		/* use cases are: disconnected xa, slave xa, recovery */
		innobase_commit_low(trx);
		ut_ad(trx->mysql_thd == NULL);
		trx_deregister_from_2pc(trx);
		ut_ad(!trx->will_lock);    /* trx cache requirement */
		trx->free();

		return(XA_OK);
	} else {
		return(XAER_NOTA);
	}
}

/** This function is used to rollback one X/Open XA distributed transaction
which is in the prepared state

@param[in] hton InnoDB handlerton
@param[in] xid X/Open XA transaction identification

@return 0 or error number */
int innobase_rollback_by_xid(handlerton* hton, XID* xid)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	DBUG_EXECUTE_IF("innobase_xa_fail",
			return XAER_RMFAIL;);

	if (high_level_read_only) {
		return(XAER_RMFAIL);
	}

	if (trx_t* trx = trx_get_trx_by_xid(xid)) {
#ifdef WITH_WSREP
		/* If a wsrep transaction is being rolled back during
		the recovery, we must clear the xid in order to avoid
		writing serialisation history for rolled back transaction. */
		if (wsrep_is_wsrep_xid(trx->xid)) {
			trx->xid->null();
		}
#endif /* WITH_WSREP */
		int ret = innobase_rollback_trx(trx);
		ut_ad(!trx->will_lock);
		trx->free();

		return(ret);
	} else {
		return(XAER_NOTA);
	}
}

bool
ha_innobase::check_if_incompatible_data(
/*====================================*/
	HA_CREATE_INFO*	info,
	uint		table_changes)
{
	ha_table_option_struct *param_old, *param_new;

	/* Cache engine specific options */
	param_new = info->option_struct;
	param_old = table->s->option_struct;

	innobase_copy_frm_flags_from_create_info(m_prebuilt->table, info);

	if (table_changes != IS_EQUAL_YES) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Check that auto_increment value was not changed */
	if ((info->used_fields & HA_CREATE_USED_AUTO)
	    && info->auto_increment_value != 0) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Check that row format didn't change */
	if ((info->used_fields & HA_CREATE_USED_ROW_FORMAT)
	    && info->row_type != get_row_type()) {

		return(COMPATIBLE_DATA_NO);
	}

	/* Specifying KEY_BLOCK_SIZE requests a rebuild of the table. */
	if (info->used_fields & HA_CREATE_USED_KEY_BLOCK_SIZE) {
		return(COMPATIBLE_DATA_NO);
	}

	/* Changes on engine specific table options requests a rebuild of the table. */
	if (param_new->page_compressed != param_old->page_compressed ||
	    param_new->page_compression_level != param_old->page_compression_level)
        {
		return(COMPATIBLE_DATA_NO);
	}

	return(COMPATIBLE_DATA_YES);
}

/****************************************************************//**
Update the system variable innodb_io_capacity_max using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_io_capacity_max_update(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);

	if (in_val < srv_io_capacity) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity_max %lu"
			" lower than innodb_io_capacity %lu.",
			in_val, srv_io_capacity);

		srv_io_capacity = in_val;

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity to %lu",
				    srv_io_capacity);
	}

	srv_max_io_capacity = in_val;
}

/****************************************************************//**
Update the system variable innodb_io_capacity using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_io_capacity_update(
/*======================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	in_val = *static_cast<const ulong*>(save);

	if (in_val > srv_max_io_capacity) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_io_capacity to %lu"
				    " higher than innodb_io_capacity_max %lu",
				    in_val, srv_max_io_capacity);

		srv_max_io_capacity = in_val * 2;

		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_max_io_capacity to %lu",
				    srv_max_io_capacity);
	}

	srv_io_capacity = in_val;
}

/****************************************************************//**
Update the system variable innodb_max_dirty_pages_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_max_dirty_pages_pct_update(
/*==============================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	double	in_val = *static_cast<const double*>(save);
	if (in_val < srv_max_dirty_pages_pct_lwm) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_max_dirty_pages_pct cannot be"
				    " set lower than"
				    " innodb_max_dirty_pages_pct_lwm.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Lowering"
				    " innodb_max_dirty_page_pct_lwm to %lf",
				    in_val);

		srv_max_dirty_pages_pct_lwm = in_val;
	}

	srv_max_buf_pool_modified_pct = in_val;
}

/****************************************************************//**
Update the system variable innodb_max_dirty_pages_pct_lwm using the
"saved" value. This function is registered as a callback with MySQL. */
static
void
innodb_max_dirty_pages_pct_lwm_update(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	double	in_val = *static_cast<const double*>(save);
	if (in_val > srv_max_buf_pool_modified_pct) {
		in_val = srv_max_buf_pool_modified_pct;
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_max_dirty_pages_pct_lwm"
				    " cannot be set higher than"
				    " innodb_max_dirty_pages_pct.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_max_dirty_page_pct_lwm"
				    " to %lf",
				    in_val);
	}

	srv_max_dirty_pages_pct_lwm = in_val;
}

/*************************************************************//**
Don't allow to set innodb_fast_shutdown=0 if purge threads are
already down.
@return 0 if innodb_fast_shutdown can be set */
static
int
fast_shutdown_validate(
/*=============================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	if (check_sysvar_int(thd, var, save, value)) {
		return(1);
	}

	uint new_val = *reinterpret_cast<uint*>(save);

	if (srv_fast_shutdown && !new_val
	    && !srv_read_only_mode && abort_loop) {
		return(1);
	}

	return(0);
}

/*************************************************************//**
Check whether valid argument given to innobase_*_stopword_table.
This function is registered as a callback with MySQL.
@return 0 for valid stopword table */
static
int
innodb_stopword_table_validate(
/*===========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	stopword_table_name;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	trx_t*		trx;

	ut_a(save != NULL);
	ut_a(value != NULL);

	stopword_table_name = value->val_str(value, buff, &len);

	trx = check_trx_exists(thd);

	row_mysql_lock_data_dictionary(trx);

	/* Validate the stopword table's (if supplied) existence and
	of the right format */
	int ret = stopword_table_name && !fts_valid_stopword_table(
		stopword_table_name);

	row_mysql_unlock_data_dictionary(trx);

	if (!ret) {
		if (stopword_table_name == buff) {
			ut_ad(static_cast<size_t>(len) < sizeof buff);
			stopword_table_name = thd_strmake(thd,
							  stopword_table_name,
							  len);
		}

		*static_cast<const char**>(save) = stopword_table_name;
	}

	return(ret);
}

extern void buf_resize_start();

/** Update the system variable innodb_buffer_pool_size using the "saved"
value. This function is registered as a callback with MySQL.
@param[in]	save	immediate result from check function */
static
void
innodb_buffer_pool_size_update(THD*,st_mysql_sys_var*,void*, const void* save)
{
        longlong	in_val = *static_cast<const longlong*>(save);

	snprintf(export_vars.innodb_buffer_pool_resize_status,
	        sizeof(export_vars.innodb_buffer_pool_resize_status),
		"Requested to resize buffer pool.");

	buf_resize_start();

	ib::info() << export_vars.innodb_buffer_pool_resize_status
		<< " (new size: " << in_val << " bytes)";
}

/** The latest assigned innodb_ft_aux_table name */
static char* innodb_ft_aux_table;

/** Update innodb_ft_aux_table_id on SET GLOBAL innodb_ft_aux_table.
@param[in,out]	thd	connection
@param[out]	save	new value of innodb_ft_aux_table
@param[in]	value	user-specified value */
static int innodb_ft_aux_table_validate(THD *thd, st_mysql_sys_var*,
					void* save, st_mysql_value* value)
{
	char buf[STRING_BUFFER_USUAL_SIZE];
	int len = sizeof buf;

	if (const char* table_name = value->val_str(value, buf, &len)) {
		if (dict_table_t* table = dict_table_open_on_name(
			    table_name, FALSE, TRUE, DICT_ERR_IGNORE_NONE)) {
			const table_id_t id = dict_table_has_fts_index(table)
				? table->id : 0;
			dict_table_close(table, FALSE, FALSE);
			if (id) {
				innodb_ft_aux_table_id = id;
				if (table_name == buf) {
					ut_ad(static_cast<size_t>(len)
					      < sizeof buf);
					table_name = thd_strmake(thd,
								 table_name,
								 len);
				}


				*static_cast<const char**>(save) = table_name;
				return 0;
			}
		}

		return 1;
	} else {
		*static_cast<char**>(save) = NULL;
		innodb_ft_aux_table_id = 0;
		return 0;
	}
}

#ifdef BTR_CUR_HASH_ADAPT
/****************************************************************//**
Update the system variable innodb_adaptive_hash_index using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_adaptive_hash_index_update(THD*, st_mysql_sys_var*, void*,
				  const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	if (*(my_bool*) save) {
		btr_search_enable();
	} else {
		btr_search_disable();
	}
	mysql_mutex_lock(&LOCK_global_system_variables);
}
#endif /* BTR_CUR_HASH_ADAPT */

/****************************************************************//**
Update the system variable innodb_cmp_per_index using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_cmp_per_index_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
	/* Reset the stats whenever we enable the table
	INFORMATION_SCHEMA.innodb_cmp_per_index. */
	if (!srv_cmp_per_index_enabled && *(my_bool*) save) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		page_zip_reset_stat_per_index();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}

	srv_cmp_per_index_enabled = !!(*(my_bool*) save);
}

/****************************************************************//**
Update the system variable innodb_old_blocks_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_old_blocks_pct_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	uint ratio = buf_LRU_old_ratio_update(*static_cast<const uint*>(save),
					      true);
	mysql_mutex_lock(&LOCK_global_system_variables);
	innobase_old_blocks_pct = ratio;
}

/****************************************************************//**
Update the system variable innodb_old_blocks_pct using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_change_buffer_max_size_update(THD*, st_mysql_sys_var*, void*,
				     const void* save)
{
	srv_change_buffer_max_size = *static_cast<const uint*>(save);
	mysql_mutex_unlock(&LOCK_global_system_variables);
	ibuf_max_size_update(srv_change_buffer_max_size);
	mysql_mutex_lock(&LOCK_global_system_variables);
}

#ifdef UNIV_DEBUG
static uint srv_fil_make_page_dirty_debug = 0;
static uint srv_saved_page_number_debug;

/****************************************************************//**
Make the first page of given user tablespace dirty. */
static
void
innodb_make_page_dirty(THD*, st_mysql_sys_var*, void*, const void* save)
{
	mtr_t		mtr;
	uint		space_id = *static_cast<const uint*>(save);
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_space_t*	space = fil_space_t::get(space_id);

	if (space == NULL) {
func_exit_no_space:
		mysql_mutex_lock(&LOCK_global_system_variables);
		return;
	}

	if (srv_saved_page_number_debug >= space->size) {
func_exit:
		space->release();
		goto func_exit_no_space;
	}

	mtr.start();
	mtr.set_named_space(space);

	buf_block_t*	block = buf_page_get(
		page_id_t(space_id, srv_saved_page_number_debug),
		space->zip_size(), RW_X_LATCH, &mtr);

	if (block != NULL) {
		ib::info() << "Dirtying page: " << block->page.id();
		mtr.write<1,mtr_t::FORCED>(*block,
					   block->frame + FIL_PAGE_SPACE_ID,
					   block->frame[FIL_PAGE_SPACE_ID]);
	}
	mtr.commit();
	log_write_up_to(mtr.commit_lsn(), true);
	goto func_exit;
}
#endif // UNIV_DEBUG

/****************************************************************//**
Update the monitor counter according to the "set_option",  turn
on/off or reset specified monitor counter. */
static
void
innodb_monitor_set_option(
/*======================*/
	const monitor_info_t* monitor_info,/*!< in: monitor info for the monitor
					to set */
	mon_option_t	set_option)	/*!< in: Turn on/off reset the
					counter */
{
	monitor_id_t	monitor_id = monitor_info->monitor_id;

	/* If module type is MONITOR_GROUP_MODULE, it cannot be
	turned on/off individually. It should never use this
	function to set options */
	ut_a(!(monitor_info->monitor_type & MONITOR_GROUP_MODULE));

	switch (set_option) {
	case MONITOR_TURN_ON:
		MONITOR_ON(monitor_id);
		MONITOR_INIT(monitor_id);
		MONITOR_SET_START(monitor_id);

		/* If the monitor to be turned on uses
		exisitng monitor counter (status variable),
		make special processing to remember existing
		counter value. */
		if (monitor_info->monitor_type & MONITOR_EXISTING) {
			srv_mon_process_existing_counter(
				monitor_id, MONITOR_TURN_ON);
		}

		if (MONITOR_IS_ON(MONITOR_LATCHES)) {

			mutex_monitor.enable();
		}
		break;

	case MONITOR_TURN_OFF:
		if (monitor_info->monitor_type & MONITOR_EXISTING) {
			srv_mon_process_existing_counter(
				monitor_id, MONITOR_TURN_OFF);
		}

		MONITOR_OFF(monitor_id);
		MONITOR_SET_OFF(monitor_id);

		if (!MONITOR_IS_ON(MONITOR_LATCHES)) {

			mutex_monitor.disable();
		}
		break;

	case MONITOR_RESET_VALUE:
		srv_mon_reset(monitor_id);

		if (monitor_id == (MONITOR_LATCHES)) {

			mutex_monitor.reset();
		}
		break;

	case MONITOR_RESET_ALL_VALUE:
		srv_mon_reset_all(monitor_id);
		mutex_monitor.reset();
		break;

	default:
		ut_error;
	}
}

/****************************************************************//**
Find matching InnoDB monitor counters and update their status
according to the "set_option",  turn on/off or reset specified
monitor counter. */
static
void
innodb_monitor_update_wildcard(
/*===========================*/
	const char*	name,		/*!< in: monitor name to match */
	mon_option_t	set_option)	/*!< in: the set option, whether
					to turn on/off or reset the counter */
{
	ut_a(name);

	for (ulint use = 0; use < NUM_MONITOR; use++) {
		ulint		type;
		monitor_id_t	monitor_id = static_cast<monitor_id_t>(use);
		monitor_info_t*	monitor_info;

		if (!innobase_wildcasecmp(
			srv_mon_get_name(monitor_id), name)) {
			monitor_info = srv_mon_get_info(monitor_id);

			type = monitor_info->monitor_type;

			/* If the monitor counter is of MONITOR_MODULE
			type, skip it. Except for those also marked with
			MONITOR_GROUP_MODULE flag, which can be turned
			on only as a module. */
			if (!(type & MONITOR_MODULE)
			     && !(type & MONITOR_GROUP_MODULE)) {
				innodb_monitor_set_option(monitor_info,
							  set_option);
			}

			/* Need to special handle counters marked with
			MONITOR_GROUP_MODULE, turn on the whole module if
			any one of it comes here. Currently, only
			"module_buf_page" is marked with MONITOR_GROUP_MODULE */
			if (type & MONITOR_GROUP_MODULE) {
				if ((monitor_id >= MONITOR_MODULE_BUF_PAGE)
				     && (monitor_id < MONITOR_MODULE_OS)) {
					if (set_option == MONITOR_TURN_ON
					    && MONITOR_IS_ON(
						MONITOR_MODULE_BUF_PAGE)) {
						continue;
					}

					srv_mon_set_module_control(
						MONITOR_MODULE_BUF_PAGE,
						set_option);
				} else {
					/* If new monitor is added with
					MONITOR_GROUP_MODULE, it needs
					to be added here. */
					ut_ad(0);
				}
			}
		}
	}
}

/*************************************************************//**
Given a configuration variable name, find corresponding monitor counter
and return its monitor ID if found.
@return monitor ID if found, MONITOR_NO_MATCH if there is no match */
static
ulint
innodb_monitor_id_by_name_get(
/*==========================*/
	const char*	name)	/*!< in: monitor counter namer */
{
	ut_a(name);

	/* Search for wild character '%' in the name, if
	found, we treat it as a wildcard match. We do not search for
	single character wildcard '_' since our monitor names already contain
	such character. To avoid confusion, we request user must include
	at least one '%' character to activate the wildcard search. */
	if (strchr(name, '%')) {
		return(MONITOR_WILDCARD_MATCH);
	}

	/* Not wildcard match, check for an exact match */
	for (ulint i = 0; i < NUM_MONITOR; i++) {
		if (!innobase_strcasecmp(
			name, srv_mon_get_name(static_cast<monitor_id_t>(i)))) {
			return(i);
		}
	}

	return(MONITOR_NO_MATCH);
}
/*************************************************************//**
Validate that the passed in monitor name matches at least one
monitor counter name with wildcard compare.
@return TRUE if at least one monitor name matches */
static
ibool
innodb_monitor_validate_wildcard_name(
/*==================================*/
	const char*	name)	/*!< in: monitor counter namer */
{
	for (ulint i = 0; i < NUM_MONITOR; i++) {
		if (!innobase_wildcasecmp(
			srv_mon_get_name(static_cast<monitor_id_t>(i)), name)) {
			return(TRUE);
		}
	}

	return(FALSE);
}
/*************************************************************//**
Validate the passed in monitor name, find and save the
corresponding monitor name in the function parameter "save".
@return 0 if monitor name is valid */
static
int
innodb_monitor_valid_byname(
/*========================*/
	void*			save,	/*!< out: immediate result
					for update function */
	const char*		name)	/*!< in: incoming monitor name */
{
	ulint		use;
	monitor_info_t*	monitor_info;

	if (!name) {
		return(1);
	}

	use = innodb_monitor_id_by_name_get(name);

	/* No monitor name matches, nor it is wildcard match */
	if (use == MONITOR_NO_MATCH) {
		return(1);
	}

	if (use < NUM_MONITOR) {
		monitor_info = srv_mon_get_info((monitor_id_t) use);

		/* If the monitor counter is marked with
		MONITOR_GROUP_MODULE flag, then this counter
		cannot be turned on/off individually, instead
		it shall be turned on/off as a group using
		its module name */
		if ((monitor_info->monitor_type & MONITOR_GROUP_MODULE)
		    && (!(monitor_info->monitor_type & MONITOR_MODULE))) {
			sql_print_warning(
				"Monitor counter '%s' cannot"
				" be turned on/off individually."
				" Please use its module name"
				" to turn on/off the counters"
				" in the module as a group.\n",
				name);

			return(1);
		}

	} else {
		ut_a(use == MONITOR_WILDCARD_MATCH);

		/* For wildcard match, if there is not a single monitor
		counter name that matches, treat it as an invalid
		value for the system configuration variables */
		if (!innodb_monitor_validate_wildcard_name(name)) {
			return(1);
		}
	}

	/* Save the configure name for innodb_monitor_update() */
	*static_cast<const char**>(save) = name;

	return(0);
}
/*************************************************************//**
Validate passed-in "value" is a valid monitor counter name.
This function is registered as a callback with MySQL.
@return 0 for valid name */
static
int
innodb_monitor_validate(
/*====================*/
	THD*, st_mysql_sys_var*,
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	const char*	name;
	char*		monitor_name;
	char		buff[STRING_BUFFER_USUAL_SIZE];
	int		len = sizeof(buff);
	int		ret;

	ut_a(save != NULL);
	ut_a(value != NULL);

	name = value->val_str(value, buff, &len);

	/* monitor_name could point to memory from MySQL
	or buff[]. Always dup the name to memory allocated
	by InnoDB, so we can access it in another callback
	function innodb_monitor_update() and free it appropriately */
	if (name) {
		monitor_name = my_strdup(PSI_INSTRUMENT_ME,
                                         name, MYF(0));
	} else {
		return(1);
	}

	ret = innodb_monitor_valid_byname(save, monitor_name);

	if (ret) {
		/* Validation failed */
		my_free(monitor_name);
	} else {
		/* monitor_name will be freed in separate callback function
		innodb_monitor_update(). Assert "save" point to
		the "monitor_name" variable */
		ut_ad(*static_cast<char**>(save) == monitor_name);
	}

	return(ret);
}

/****************************************************************//**
Update the system variable innodb_enable(disable/reset/reset_all)_monitor
according to the "set_option" and turn on/off or reset specified monitor
counter. */
static
void
innodb_monitor_update(
/*==================*/
	THD*			thd,		/*!< in: thread handle */
	void*			var_ptr,	/*!< out: where the
						formal string goes */
	const void*		save,		/*!< in: immediate result
						from check function */
	mon_option_t		set_option,	/*!< in: the set option,
						whether to turn on/off or
						reset the counter */
	ibool			free_mem)	/*!< in: whether we will
						need to free the memory */
{
	monitor_info_t*	monitor_info;
	ulint		monitor_id;
	ulint		err_monitor = 0;
	const char*	name;

	ut_a(save != NULL);

	name = *static_cast<const char*const*>(save);

	if (!name) {
		monitor_id = MONITOR_DEFAULT_START;
	} else {
		monitor_id = innodb_monitor_id_by_name_get(name);

		/* Double check we have a valid monitor ID */
		if (monitor_id == MONITOR_NO_MATCH) {
			return;
		}
	}

	if (monitor_id == MONITOR_DEFAULT_START) {
		/* If user set the variable to "default", we will
		print a message and make this set operation a "noop".
		The check is being made here is because "set default"
		does not go through validation function */
		if (thd) {
			push_warning_printf(
				thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_DEFAULT,
				"Default value is not defined for"
				" this set option. Please specify"
				" correct counter or module name.");
		} else {
			sql_print_error(
				"Default value is not defined for"
				" this set option. Please specify"
				" correct counter or module name.\n");
		}

		if (var_ptr) {
			*(const char**) var_ptr = NULL;
		}
	} else if (monitor_id == MONITOR_WILDCARD_MATCH) {
		innodb_monitor_update_wildcard(name, set_option);
	} else {
		monitor_info = srv_mon_get_info(
			static_cast<monitor_id_t>(monitor_id));

		ut_a(monitor_info);

		/* If monitor is already truned on, someone could already
		collect monitor data, exit and ask user to turn off the
		monitor before turn it on again. */
		if (set_option == MONITOR_TURN_ON
		    && MONITOR_IS_ON(monitor_id)) {
			err_monitor = monitor_id;
			goto exit;
		}

		if (var_ptr) {
			*(const char**) var_ptr = monitor_info->monitor_name;
		}

		/* Depending on the monitor name is for a module or
		a counter, process counters in the whole module or
		individual counter. */
		if (monitor_info->monitor_type & MONITOR_MODULE) {
			srv_mon_set_module_control(
				static_cast<monitor_id_t>(monitor_id),
				set_option);
		} else {
			innodb_monitor_set_option(monitor_info, set_option);
		}
	}
exit:
	/* Only if we are trying to turn on a monitor that already
	been turned on, we will set err_monitor. Print related
	information */
	if (err_monitor) {
		sql_print_warning("InnoDB: Monitor %s is already enabled.",
				  srv_mon_get_name((monitor_id_t) err_monitor));
	}

	if (free_mem && name) {
		my_free((void*) name);
	}

	return;
}

/** Validate SET GLOBAL innodb_buffer_pool_filename.
On Windows, file names with colon (:) are not allowed.
@param thd   connection
@param save  &srv_buf_dump_filename
@param value new value to be validated
@return	0 for valid name */
static int innodb_srv_buf_dump_filename_validate(THD *thd, st_mysql_sys_var*,
						 void *save,
						 st_mysql_value *value)
{
  char buff[OS_FILE_MAX_PATH];
  int len= sizeof buff;

  if (const char *buf_name= value->val_str(value, buff, &len))
  {
#ifdef _WIN32
    if (!is_filename_allowed(buf_name, len, FALSE))
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
			  ER_WRONG_ARGUMENTS,
			  "InnoDB: innodb_buffer_pool_filename "
			  "cannot have colon (:) in the file name.");
      return 1;
    }
#endif /* _WIN32 */
    if (buf_name == buff)
    {
      ut_ad(static_cast<size_t>(len) < sizeof buff);
      buf_name= thd_strmake(thd, buf_name, len);
    }

    *static_cast<const char**>(save)= buf_name;
    return 0;
  }

  return 1;
}

#ifdef UNIV_DEBUG
static char* srv_buffer_pool_evict;

/****************************************************************//**
Evict all uncompressed pages of compressed tables from the buffer pool.
Keep the compressed pages in the buffer pool.
@return whether all uncompressed pages were evicted */
static bool innodb_buffer_pool_evict_uncompressed()
{
	bool	all_evicted = true;

	mysql_mutex_lock(&buf_pool.mutex);

	for (buf_block_t* block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
	     block != NULL; ) {
		buf_block_t*	prev_block = UT_LIST_GET_PREV(unzip_LRU, block);
		ut_ad(block->page.state() == BUF_BLOCK_FILE_PAGE);
		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);

		if (!buf_LRU_free_page(&block->page, false)) {
			all_evicted = false;
			block = prev_block;
		} else {
			/* Because buf_LRU_free_page() may release
			and reacquire buf_pool.mutex, prev_block
			may be invalid. */
			block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
		}
	}

	mysql_mutex_unlock(&buf_pool.mutex);
	return(all_evicted);
}

/****************************************************************//**
Called on SET GLOBAL innodb_buffer_pool_evict=...
Handles some values specially, to evict pages from the buffer pool.
SET GLOBAL innodb_buffer_pool_evict='uncompressed'
evicts all uncompressed page frames of compressed tablespaces. */
static
void
innodb_buffer_pool_evict_update(THD*, st_mysql_sys_var*, void*,
				const void* save)
{
	if (const char* op = *static_cast<const char*const*>(save)) {
		if (!strcmp(op, "uncompressed")) {
			mysql_mutex_unlock(&LOCK_global_system_variables);
			for (uint tries = 0; tries < 10000; tries++) {
				if (innodb_buffer_pool_evict_uncompressed()) {
					mysql_mutex_lock(
						&LOCK_global_system_variables);
					return;
				}

				os_thread_sleep(10000);
			}

			/* We failed to evict all uncompressed pages. */
			ut_ad(0);
		}
	}
}
#endif /* UNIV_DEBUG */

/****************************************************************//**
Update the system variable innodb_monitor_enable and enable
specified monitor counter.
This function is registered as a callback with MySQL. */
static
void
innodb_enable_monitor_update(
/*=========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_TURN_ON, TRUE);
}

/****************************************************************//**
Update the system variable innodb_monitor_disable and turn
off specified monitor counter. */
static
void
innodb_disable_monitor_update(
/*==========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_TURN_OFF, TRUE);
}

/****************************************************************//**
Update the system variable innodb_monitor_reset and reset
specified monitor counter(s).
This function is registered as a callback with MySQL. */
static
void
innodb_reset_monitor_update(
/*========================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_RESET_VALUE, TRUE);
}

/****************************************************************//**
Update the system variable innodb_monitor_reset_all and reset
all value related monitor counter.
This function is registered as a callback with MySQL. */
static
void
innodb_reset_all_monitor_update(
/*============================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*,
	void*				var_ptr,/*!< out: where the
						formal string goes */
	const void*			save)	/*!< in: immediate result
						from check function */
{
	innodb_monitor_update(thd, var_ptr, save, MONITOR_RESET_ALL_VALUE,
			      TRUE);
}

static
void
innodb_defragment_frequency_update(THD*, st_mysql_sys_var*, void*,
				   const void* save)
{
	srv_defragment_frequency = (*static_cast<const uint*>(save));
	srv_defragment_interval = 1000000000ULL / srv_defragment_frequency;
}

static inline char *my_strtok_r(char *str, const char *delim, char **saveptr)
{
#if defined _WIN32
	return strtok_s(str, delim, saveptr);
#else
	return strtok_r(str, delim, saveptr);
#endif
}

/****************************************************************//**
Parse and enable InnoDB monitor counters during server startup.
User can list the monitor counters/groups to be enable by specifying
"loose-innodb_monitor_enable=monitor_name1;monitor_name2..."
in server configuration file or at the command line. The string
separate could be ";", "," or empty space. */
static
void
innodb_enable_monitor_at_startup(
/*=============================*/
	char*	str)	/*!< in/out: monitor counter enable list */
{
	static const char*	sep = " ;,";
	char*			last;

	ut_a(str);

	/* Walk through the string, and separate each monitor counter
	and/or counter group name, and calling innodb_monitor_update()
	if successfully updated. Please note that the "str" would be
	changed by strtok_r() as it walks through it. */
	for (char* option = my_strtok_r(str, sep, &last);
	     option;
	     option = my_strtok_r(NULL, sep, &last)) {
		char*	option_name;
		if (!innodb_monitor_valid_byname(&option_name, option)) {
			innodb_monitor_update(NULL, NULL, &option,
					      MONITOR_TURN_ON, FALSE);
		} else {
			sql_print_warning("Invalid monitor counter"
					  " name: '%s'", option);
		}
	}
}

/****************************************************************//**
Callback function for accessing the InnoDB variables from MySQL:
SHOW VARIABLES. */
static int show_innodb_vars(THD*, SHOW_VAR* var, char*)
{
	innodb_export_status();
	var->type = SHOW_ARRAY;
	var->value = (char*) &innodb_status_variables;
	//var->scope = SHOW_SCOPE_GLOBAL;

	return(0);
}

/****************************************************************//**
This function checks each index name for a table against reserved
system default primary index name 'GEN_CLUST_INDEX'. If a name
matches, this function pushes an warning message to the client,
and returns true.
@return true if the index name matches the reserved name */
bool
innobase_index_name_is_reserved(
/*============================*/
	THD*		thd,		/*!< in/out: MySQL connection */
	const KEY*	key_info,	/*!< in: Indexes to be created */
	ulint		num_of_keys)	/*!< in: Number of indexes to
					be created. */
{
	const KEY*	key;
	uint		key_num;	/* index number */

	for (key_num = 0; key_num < num_of_keys; key_num++) {
		key = &key_info[key_num];

		if (innobase_strcasecmp(key->name.str,
					innobase_index_reserve_name) == 0) {
			/* Push warning to mysql */
			push_warning_printf(thd,
					    Sql_condition::WARN_LEVEL_WARN,
					    ER_WRONG_NAME_FOR_INDEX,
					    "Cannot Create Index with name"
					    " '%s'. The name is reserved"
					    " for the system default primary"
					    " index.",
					    innobase_index_reserve_name);

			my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0),
				 innobase_index_reserve_name);

			return(true);
		}
	}

	return(false);
}

/** Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_retrieve_ranking(
	FT_INFO*	fts_hdl)
{
	fts_result_t*	result;
	row_prebuilt_t*	ft_prebuilt;

	result = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_result;

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_prebuilt;

	fts_ranking_t*  ranking = rbt_value(fts_ranking_t, result->current);
	ft_prebuilt->fts_doc_id= ranking->doc_id;

	return(ranking->rank);
}

/** Free the memory for the FTS handler
@param[in,out]	fts_hdl	FTS handler */
static
void
innobase_fts_close_ranking(
	FT_INFO*	fts_hdl)
{
	fts_result_t*	result;

	result = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_result;

	fts_query_free_result(result);

	my_free((uchar*) fts_hdl);
}

/** Find and Retrieve the FTS Relevance Ranking result for doc with doc_id
of m_prebuilt->fts_doc_id
@param[in,out]	fts_hdl	FTS handler
@return the relevance ranking value */
static
float
innobase_fts_find_ranking(FT_INFO* fts_hdl, uchar*, uint)
{
	fts_result_t*	result;
	row_prebuilt_t*	ft_prebuilt;

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_prebuilt;
	result = reinterpret_cast<NEW_FT_INFO*>(fts_hdl)->ft_result;

	/* Retrieve the ranking value for doc_id with value of
	m_prebuilt->fts_doc_id */
	return(fts_retrieve_ranking(result, ft_prebuilt->fts_doc_id));
}

#ifdef UNIV_DEBUG
static my_bool	innodb_background_drop_list_empty = TRUE;
static my_bool	innodb_log_checkpoint_now = TRUE;
static my_bool	innodb_buf_flush_list_now = TRUE;
static uint	innodb_merge_threshold_set_all_debug
	= DICT_INDEX_MERGE_THRESHOLD_DEFAULT;

/** Wait for the background drop list to become empty. */
static
void
wait_background_drop_list_empty(THD*, st_mysql_sys_var*, void*, const void*)
{
	row_wait_for_background_drop_list_empty();
}

/****************************************************************//**
Force innodb to checkpoint. */
static
void
checkpoint_now_set(THD*, st_mysql_sys_var*, void*, const void* save)
{
	if (*(my_bool*) save) {
		mysql_mutex_unlock(&LOCK_global_system_variables);

		while (log_sys.last_checkpoint_lsn
		       + SIZE_OF_FILE_CHECKPOINT
		       < log_sys.get_lsn()) {
			log_make_checkpoint();
			log_sys.log.flush();
		}

		dberr_t err = fil_write_flushed_lsn(log_sys.get_lsn());

		if (err != DB_SUCCESS) {
			ib::warn() << "Checkpoint set failed " << err;
		}

		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

/****************************************************************//**
Force a dirty pages flush now. */
static
void
buf_flush_list_now_set(THD*, st_mysql_sys_var*, void*, const void* save)
{
	if (*(my_bool*) save) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		buf_flush_sync();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

/** Override current MERGE_THRESHOLD setting for all indexes at dictionary
now.
@param[in]	save	immediate result from check function */
static
void
innodb_merge_threshold_set_all_debug_update(THD*, st_mysql_sys_var*, void*,
					    const void* save)
{
	innodb_merge_threshold_set_all_debug
		= (*static_cast<const uint*>(save));
	dict_set_merge_threshold_all_debug(
		innodb_merge_threshold_set_all_debug);
}
#endif /* UNIV_DEBUG */

/** Find and Retrieve the FTS doc_id for the current result row
@param[in,out]	fts_hdl	FTS handler
@return the document ID */
static
ulonglong
innobase_fts_retrieve_docid(
	FT_INFO_EXT*	fts_hdl)
{
	fts_result_t*	result;
	row_prebuilt_t* ft_prebuilt;

	ft_prebuilt = reinterpret_cast<NEW_FT_INFO *>(fts_hdl)->ft_prebuilt;
	result = reinterpret_cast<NEW_FT_INFO *>(fts_hdl)->ft_result;

	if (ft_prebuilt->read_just_key) {

		fts_ranking_t* ranking =
			rbt_value(fts_ranking_t, result->current);

		return(ranking->doc_id);
	}

	return(ft_prebuilt->fts_doc_id);
}

/* These variables are never read by InnoDB or changed. They are a kind of
dummies that are needed by the MySQL infrastructure to call
buffer_pool_dump_now(), buffer_pool_load_now() and buffer_pool_load_abort()
by the user by doing:
  SET GLOBAL innodb_buffer_pool_dump_now=ON;
  SET GLOBAL innodb_buffer_pool_load_now=ON;
  SET GLOBAL innodb_buffer_pool_load_abort=ON;
Their values are read by MySQL and displayed to the user when the variables
are queried, e.g.:
  SELECT @@innodb_buffer_pool_dump_now;
  SELECT @@innodb_buffer_pool_load_now;
  SELECT @@innodb_buffer_pool_load_abort; */
static my_bool	innodb_buffer_pool_dump_now = FALSE;
static my_bool	innodb_buffer_pool_load_now = FALSE;
static my_bool	innodb_buffer_pool_load_abort = FALSE;

/****************************************************************//**
Trigger a dump of the buffer pool if innodb_buffer_pool_dump_now is set
to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_dump_now(
/*=================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		buf_dump_start();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

/****************************************************************//**
Trigger a load of the buffer pool if innodb_buffer_pool_load_now is set
to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_load_now(
/*=================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		buf_load_start();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

/****************************************************************//**
Abort a load of the buffer pool if innodb_buffer_pool_load_abort
is set to ON. This function is registered as a callback with MySQL. */
static
void
buffer_pool_load_abort(
/*===================*/
	THD*				thd	/*!< in: thread handle */
					MY_ATTRIBUTE((unused)),
	struct st_mysql_sys_var*	var	/*!< in: pointer to system
						variable */
					MY_ATTRIBUTE((unused)),
	void*				var_ptr	/*!< out: where the formal
						string goes */
					MY_ATTRIBUTE((unused)),
	const void*			save)	/*!< in: immediate result from
						check function */
{
	if (*(my_bool*) save && !srv_read_only_mode) {
		mysql_mutex_unlock(&LOCK_global_system_variables);
		buf_load_abort();
		mysql_mutex_lock(&LOCK_global_system_variables);
	}
}

/****************************************************************//**
Update the system variable innodb_log_write_ahead_size using the "saved"
value. This function is registered as a callback with MySQL. */
static
void
innodb_log_write_ahead_size_update(
/*===============================*/
	THD*				thd,	/*!< in: thread handle */
	st_mysql_sys_var*, void*,
	const void*			save)	/*!< in: immediate result
						from check function */
{
	ulong	val = OS_FILE_LOG_BLOCK_SIZE;
	ulong	in_val = *static_cast<const ulong*>(save);

	while (val < in_val) {
		val = val * 2;
	}

	if (val > srv_page_size) {
		val = srv_page_size;
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_log_write_ahead_size cannot"
				    " be set higher than innodb_page_size.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_log_write_ahead_size"
				    " to %lu",
				    srv_page_size);
	} else if (val != in_val) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_log_write_ahead_size should be"
				    " set 2^n value and larger than 512.");
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Setting innodb_log_write_ahead_size"
				    " to %lu",
				    val);
	}

	srv_log_write_ahead_size = val;
}

/** Update innodb_status_output or innodb_status_output_locks,
which control InnoDB "status monitor" output to the error log.
@param[out]	var	current value
@param[in]	save	to-be-assigned value */
static
void
innodb_status_output_update(THD*,st_mysql_sys_var*,void*var,const void*save)
{
  if (srv_monitor_timer)
  {
    *static_cast<my_bool*>(var)= *static_cast<const my_bool*>(save);
    mysql_mutex_unlock(&LOCK_global_system_variables);
    /* Wakeup server monitor. */
    srv_monitor_timer_schedule_now();
    mysql_mutex_lock(&LOCK_global_system_variables);
  }
}

/** Update the system variable innodb_encryption_threads.
@param[in]	save	to-be-assigned value */
static
void
innodb_encryption_threads_update(THD*,st_mysql_sys_var*,void*,const void*save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_thread_cnt(*static_cast<const uint*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Update the system variable innodb_encryption_rotate_key_age.
@param[in]	save	to-be-assigned value */
static
void
innodb_encryption_rotate_key_age_update(THD*, st_mysql_sys_var*, void*,
					const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_rotate_key_age(*static_cast<const uint*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Update the system variable innodb_encryption_rotation_iops.
@param[in]	save	to-be-assigned value */
static
void
innodb_encryption_rotation_iops_update(THD*, st_mysql_sys_var*, void*,
				       const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_rotation_iops(*static_cast<const uint*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

/** Update the system variable innodb_encrypt_tables.
@param[in]	save	to-be-assigned value */
static
void
innodb_encrypt_tables_update(THD*, st_mysql_sys_var*, void*, const void* save)
{
	mysql_mutex_unlock(&LOCK_global_system_variables);
	fil_crypt_set_encrypt_tables(*static_cast<const ulong*>(save));
	mysql_mutex_lock(&LOCK_global_system_variables);
}

static SHOW_VAR innodb_status_variables_export[]= {
	{"Innodb", (char*) &show_innodb_vars, SHOW_FUNC},
	{NullS, NullS, SHOW_LONG}
};

static struct st_mysql_storage_engine innobase_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

#ifdef WITH_WSREP

/** This function is used to kill one transaction.

This transaction was open on this node (not-yet-committed), and a
conflicting writeset from some other node that was being applied
caused a locking conflict.  First committed (from other node)
wins, thus open transaction is rolled back.  BF stands for
brute-force: any transaction can get aborted by galera any time
it is necessary.

This conflict can happen only when the replicated writeset (from
other node) is being applied, not when it’s waiting in the queue.
If our local transaction reached its COMMIT and this conflicting
writeset was in the queue, then it should fail the local
certification test instead.

A brute force abort is only triggered by a locking conflict
between a writeset being applied by an applier thread (slave thread)
and an open transaction on the node, not by a Galera writeset
comparison as in the local certification failure.

@param[in]	bf_thd		Brute force (BF) thread
@param[in,out]	victim_trx	Vimtim trx to be killed
@param[in]	signal		Should victim be signaled */
int wsrep_innobase_kill_one_trx(THD *bf_thd, trx_t *victim_trx, bool signal)
{
	ut_ad(bf_thd);
	ut_ad(victim_trx);
	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(victim_trx));

	DBUG_ENTER("wsrep_innobase_kill_one_trx");

	THD *thd= victim_trx->mysql_thd;
	ut_ad(thd);
	/* Note that bf_trx might not exist here e.g. on MDL conflict
	case (test: galera_concurrent_ctas). Similarly, BF thread
	could be also acquiring MDL-lock causing victim to be
	aborted. However, we have not yet called innobase_trx_init()
	for BF transaction (test: galera_many_columns)*/
	trx_t* bf_trx= thd_to_trx(bf_thd);
	DBUG_ASSERT(wsrep_on(bf_thd));

	wsrep_thd_LOCK(thd);

	WSREP_LOG_CONFLICT(bf_thd, thd, TRUE);

	WSREP_DEBUG("Aborter %s trx_id: " TRX_ID_FMT " thread: %ld "
		"seqno: %lld client_state: %s client_mode: %s transaction_mode: %s "
		"query: %s",
		wsrep_thd_is_BF(bf_thd, false) ? "BF" : "normal",
		bf_trx ? bf_trx->id : TRX_ID_MAX,
		thd_get_thread_id(bf_thd),
		wsrep_thd_trx_seqno(bf_thd),
		wsrep_thd_client_state_str(bf_thd),
		wsrep_thd_client_mode_str(bf_thd),
		wsrep_thd_transaction_state_str(bf_thd),
		wsrep_thd_query(bf_thd));

	WSREP_DEBUG("Victim %s trx_id: " TRX_ID_FMT " thread: %ld "
		"seqno: %lld client_state: %s  client_mode: %s transaction_mode: %s "
		"query: %s",
		wsrep_thd_is_BF(thd, false) ? "BF" : "normal",
		victim_trx->id,
		thd_get_thread_id(thd),
		wsrep_thd_trx_seqno(thd),
		wsrep_thd_client_state_str(thd),
		wsrep_thd_client_mode_str(thd),
		wsrep_thd_transaction_state_str(thd),
		wsrep_thd_query(thd));

	/* Mark transaction as a victim for Galera abort */
	victim_trx->lock.was_chosen_as_wsrep_victim= true;
	if (wsrep_thd_set_wsrep_aborter(bf_thd, thd))
	{
	  WSREP_DEBUG("innodb kill transaction skipped due to wsrep_aborter set");
	  wsrep_thd_UNLOCK(thd);
	  DBUG_RETURN(0);
	}

	/* Note that we need to release this as it will be acquired
	below in wsrep-lib */
	wsrep_thd_UNLOCK(thd);
	DEBUG_SYNC(bf_thd, "before_wsrep_thd_abort");

	if (wsrep_thd_bf_abort(bf_thd, thd, signal))
	{
		lock_t*  wait_lock = victim_trx->lock.wait_lock;
		if (wait_lock) {
			DBUG_ASSERT(victim_trx->is_wsrep());
			WSREP_DEBUG("victim has wait flag: %lu",
				    thd_get_thread_id(thd));

			WSREP_DEBUG("canceling wait lock");
			victim_trx->lock.was_chosen_as_deadlock_victim= TRUE;
			lock_cancel_waiting_and_release(wait_lock);
		}
	}

	DBUG_RETURN(0);
}

/** This function forces the victim transaction to abort. Aborting the
  transaction does NOT end it, it still has to be rolled back.

  @param bf_thd       brute force THD asking for the abort
  @param victim_thd   victim THD to be aborted

  @return 0 victim was aborted
  @return -1 victim thread was aborted (no transaction)
*/
static
int
wsrep_abort_transaction(
	handlerton*,
	THD *bf_thd,
	THD *victim_thd,
	my_bool signal)
{
	DBUG_ENTER("wsrep_innobase_abort_thd");
	ut_ad(bf_thd);
	ut_ad(victim_thd);

	trx_t* victim_trx	= thd_to_trx(victim_thd);

	WSREP_DEBUG("abort transaction: BF: %s victim: %s victim conf: %s",
			wsrep_thd_query(bf_thd),
			wsrep_thd_query(victim_thd),
			wsrep_thd_transaction_state_str(victim_thd));

	if (victim_trx) {
		lock_mutex_enter();
		trx_mutex_enter(victim_trx);
		int rcode= wsrep_innobase_kill_one_trx(bf_thd,
						       victim_trx, signal);
		trx_mutex_exit(victim_trx);
		lock_mutex_exit();
		DBUG_RETURN(rcode);
	} else {
		wsrep_thd_bf_abort(bf_thd, victim_thd, signal);
	}

	DBUG_RETURN(-1);
}

static
int
innobase_wsrep_set_checkpoint(
/*==========================*/
	handlerton* hton,
	const XID* xid)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);

	if (wsrep_is_wsrep_xid(xid)) {

		trx_rseg_update_wsrep_checkpoint(xid);
		innobase_flush_logs(hton, false);
		return 0;
	} else {
		return 1;
	}
}

static
int
innobase_wsrep_get_checkpoint(
/*==========================*/
	handlerton* hton,
	XID* xid)
{
	DBUG_ASSERT(hton == innodb_hton_ptr);
        trx_rseg_read_wsrep_checkpoint(*xid);
        return 0;
}
#endif /* WITH_WSREP */

/* plugin options */

static MYSQL_SYSVAR_ENUM(checksum_algorithm, srv_checksum_algorithm,
  PLUGIN_VAR_RQCMDARG,
  "The algorithm InnoDB uses for page checksumming. Possible values are"
  " FULL_CRC32"
    " for new files, always use CRC-32C; for old, see CRC32 below;"
  " STRICT_FULL_CRC32"
    " for new files, always use CRC-32C; for old, see STRICT_CRC32 below;"
  " CRC32"
    " write crc32, allow any of the other checksums to match when reading;"
  " STRICT_CRC32"
    " write crc32, do not allow other algorithms to match when reading;"
  " INNODB"
    " write a software calculated checksum, allow any other checksums"
    " to match when reading;"
  " STRICT_INNODB"
    " write a software calculated checksum, do not allow other algorithms"
    " to match when reading;"
  " NONE"
    " write a constant magic number, do not do any checksum verification"
    " when reading;"
  " STRICT_NONE"
    " write a constant magic number, do not allow values other than that"
    " magic number when reading;"
  " Files updated when this option is set to crc32 or strict_crc32 will"
  " not be readable by MariaDB versions older than 10.0.4;"
  " new files created with full_crc32 are readable by MariaDB 10.4.3+",
  NULL, NULL, SRV_CHECKSUM_ALGORITHM_FULL_CRC32,
  &innodb_checksum_algorithm_typelib);

static MYSQL_SYSVAR_STR(data_home_dir, innobase_data_home_dir,
  PLUGIN_VAR_READONLY,
  "The common part for InnoDB table spaces.",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_BOOL(doublewrite, srv_use_doublewrite_buf,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Enable InnoDB doublewrite buffer (enabled by default)."
  " Disable with --skip-innodb-doublewrite.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(use_atomic_writes, innobase_use_atomic_writes,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Enable atomic writes, instead of using the doublewrite buffer, for files "
  "on devices that supports atomic writes. "
  "To use this option one must use "
  "innodb_file_per_table=1, innodb_flush_method=O_DIRECT. "
  "This option only works on Linux with either FusionIO cards using "
  "the directFS filesystem or with Shannon cards using any file system.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(stats_include_delete_marked,
  srv_stats_include_delete_marked,
  PLUGIN_VAR_OPCMDARG,
  "Include delete marked records when calculating persistent statistics",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ENUM(instant_alter_column_allowed,
			 innodb_instant_alter_column_allowed,
  PLUGIN_VAR_RQCMDARG,
  "File format constraint for ALTER TABLE", NULL, NULL, 2/*add_drop_reorder*/,
  &innodb_instant_alter_column_allowed_typelib);

static MYSQL_SYSVAR_ULONG(io_capacity, srv_io_capacity,
  PLUGIN_VAR_RQCMDARG,
  "Number of IOPs the server can do. Tunes the background IO rate",
  NULL, innodb_io_capacity_update, 200, 100, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(io_capacity_max, srv_max_io_capacity,
  PLUGIN_VAR_RQCMDARG,
  "Limit to which innodb_io_capacity can be inflated.",
  NULL, innodb_io_capacity_max_update,
  SRV_MAX_IO_CAPACITY_DUMMY_DEFAULT, 100,
  SRV_MAX_IO_CAPACITY_LIMIT, 0);

static MYSQL_SYSVAR_ULONG(idle_flush_pct,
  srv_idle_flush_pct,
  PLUGIN_VAR_RQCMDARG,
  "Up to what percentage of dirty pages should be flushed when innodb "
  "finds it has spare resources to do so.",
  NULL, NULL, 100, 0, 100, 0);

#ifdef UNIV_DEBUG
static MYSQL_SYSVAR_BOOL(background_drop_list_empty,
  innodb_background_drop_list_empty,
  PLUGIN_VAR_OPCMDARG,
  "Wait for the background drop list to become empty",
  NULL, wait_background_drop_list_empty, FALSE);

static MYSQL_SYSVAR_BOOL(log_checkpoint_now, innodb_log_checkpoint_now,
  PLUGIN_VAR_OPCMDARG,
  "Force checkpoint now",
  NULL, checkpoint_now_set, FALSE);

static MYSQL_SYSVAR_BOOL(buf_flush_list_now, innodb_buf_flush_list_now,
  PLUGIN_VAR_OPCMDARG,
  "Force dirty page flush now",
  NULL, buf_flush_list_now_set, FALSE);

static MYSQL_SYSVAR_UINT(merge_threshold_set_all_debug,
  innodb_merge_threshold_set_all_debug,
  PLUGIN_VAR_RQCMDARG,
  "Override current MERGE_THRESHOLD setting for all indexes at dictionary"
  " cache by the specified value dynamically, at the time.",
  NULL, innodb_merge_threshold_set_all_debug_update,
  DICT_INDEX_MERGE_THRESHOLD_DEFAULT, 1, 50, 0);
#endif /* UNIV_DEBUG */

static MYSQL_SYSVAR_ULONG(purge_batch_size, srv_purge_batch_size,
  PLUGIN_VAR_OPCMDARG,
  "Number of UNDO log pages to purge in one batch from the history list.",
  NULL, NULL,
  300,			/* Default setting */
  1,			/* Minimum value */
  5000, 0);		/* Maximum value */

static MYSQL_SYSVAR_UINT(purge_threads, srv_n_purge_threads,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Number of tasks for purging transaction history",
  NULL, NULL, 4, 1, innodb_purge_threads_MAX, 0);

static MYSQL_SYSVAR_ULONG(sync_array_size, srv_sync_array_size,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Size of the mutex/lock wait array.",
  NULL, NULL,
  1,			/* Default setting */
  1,			/* Minimum value */
  1024, 0);		/* Maximum value */

static MYSQL_SYSVAR_UINT(fast_shutdown, srv_fast_shutdown,
  PLUGIN_VAR_OPCMDARG,
  "Speeds up the shutdown process of the InnoDB storage engine. Possible"
  " values are 0, 1 (faster), 2 (crash-like), 3 (fastest clean).",
  fast_shutdown_validate, NULL, 1, 0, 3, 0);

static MYSQL_SYSVAR_BOOL(file_per_table, srv_file_per_table,
  PLUGIN_VAR_NOCMDARG,
  "Stores each InnoDB table to an .ibd file in the database dir.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_STR(ft_server_stopword_table, innobase_server_stopword_table,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "The user supplied stopword table name.",
  innodb_stopword_table_validate,
  NULL,
  NULL);

static MYSQL_SYSVAR_UINT(flush_log_at_timeout, srv_flush_log_at_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Write and flush logs every (n) second.",
  NULL, NULL, 1, 0, 2700, 0);

static MYSQL_SYSVAR_ULONG(flush_log_at_trx_commit, srv_flush_log_at_trx_commit,
  PLUGIN_VAR_OPCMDARG,
  "Controls the durability/speed trade-off for commits."
  " Set to 0 (write and flush redo log to disk only once per second),"
  " 1 (flush to disk at each commit),"
  " 2 (write to log at commit but flush to disk only once per second)"
  " or 3 (flush to disk at prepare and at commit, slower and usually redundant)."
  " 1 and 3 guarantees that after a crash, committed transactions will"
  " not be lost and will be consistent with the binlog and other transactional"
  " engines. 2 can get inconsistent and lose transactions if there is a"
  " power failure or kernel crash but not if mysqld crashes. 0 has no"
  " guarantees in case of crash. 0 and 2 can be faster than 1 or 3.",
  NULL, NULL, 1, 0, 3, 0);

static MYSQL_SYSVAR_ENUM(flush_method, innodb_flush_method,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "With which method to flush data.",
  NULL, NULL, IF_WIN(SRV_ALL_O_DIRECT_FSYNC, SRV_FSYNC),
  &innodb_flush_method_typelib);

static MYSQL_SYSVAR_BOOL(force_load_corrupted, srv_load_corrupted,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Force InnoDB to load metadata of corrupted table.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(log_group_home_dir, srv_log_group_home_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to InnoDB log files.", NULL, NULL, NULL);

static MYSQL_SYSVAR_DOUBLE(max_dirty_pages_pct, srv_max_buf_pool_modified_pct,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of dirty pages allowed in bufferpool.",
  NULL, innodb_max_dirty_pages_pct_update, 90.0, 0, 99.999, 0);

static MYSQL_SYSVAR_DOUBLE(max_dirty_pages_pct_lwm,
  srv_max_dirty_pages_pct_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of dirty pages at which flushing kicks in.",
  NULL, innodb_max_dirty_pages_pct_lwm_update, 0, 0, 99.999, 0);

static MYSQL_SYSVAR_DOUBLE(adaptive_flushing_lwm,
  srv_adaptive_flushing_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of log capacity below which no adaptive flushing happens.",
  NULL, NULL, 10.0, 0.0, 70.0, 0);

static MYSQL_SYSVAR_BOOL(adaptive_flushing, srv_adaptive_flushing,
  PLUGIN_VAR_NOCMDARG,
  "Attempt flushing dirty pages to avoid IO bursts at checkpoints.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(flush_sync, srv_flush_sync,
  PLUGIN_VAR_NOCMDARG,
  "Allow IO bursts at the checkpoints ignoring io_capacity setting.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONG(flushing_avg_loops,
  srv_flushing_avg_loops,
  PLUGIN_VAR_RQCMDARG,
  "Number of iterations over which the background flushing is averaged.",
  NULL, NULL, 30, 1, 1000, 0);

static MYSQL_SYSVAR_ULONG(max_purge_lag, srv_max_purge_lag,
  PLUGIN_VAR_RQCMDARG,
  "Desired maximum length of the purge queue (0 = no limit)",
  NULL, NULL, 0, 0, ~0UL, 0);

static MYSQL_SYSVAR_ULONG(max_purge_lag_delay, srv_max_purge_lag_delay,
   PLUGIN_VAR_RQCMDARG,
   "Maximum delay of user threads in micro-seconds",
   NULL, NULL,
   0L,			/* Default seting */
   0L,			/* Minimum value */
   10000000UL, 0);	/* Maximum value */

static MYSQL_SYSVAR_UINT(max_purge_lag_wait, innodb_max_purge_lag_wait,
  PLUGIN_VAR_RQCMDARG,
  "Wait until History list length is below the specified limit",
  NULL, innodb_max_purge_lag_wait_update, UINT_MAX, 0, UINT_MAX, 0);

static MYSQL_SYSVAR_BOOL(rollback_on_timeout, innobase_rollback_on_timeout,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Roll back the complete transaction on lock wait timeout, for 4.x compatibility (disabled by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(status_file, innobase_create_status_file,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_NOSYSVAR,
  "Enable SHOW ENGINE INNODB STATUS output in the innodb_status.<pid> file",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(stats_on_metadata, innobase_stats_on_metadata,
  PLUGIN_VAR_OPCMDARG,
  "Enable statistics gathering for metadata commands such as"
  " SHOW TABLE STATUS for tables that use transient statistics (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONGLONG(stats_transient_sample_pages,
  srv_stats_transient_sample_pages,
  PLUGIN_VAR_RQCMDARG,
  "The number of leaf index pages to sample when calculating transient"
  " statistics (if persistent statistics are not used, default 8)",
  NULL, NULL, 8, 1, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(stats_persistent, srv_stats_persistent,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB persistent statistics enabled for all tables unless overridden"
  " at table level",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(stats_auto_recalc, srv_stats_auto_recalc,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB automatic recalculation of persistent statistics enabled for all"
  " tables unless overridden at table level (automatic recalculation is only"
  " done when InnoDB decides that the table has changed too much and needs a"
  " new statistics)",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONGLONG(stats_persistent_sample_pages,
  srv_stats_persistent_sample_pages,
  PLUGIN_VAR_RQCMDARG,
  "The number of leaf index pages to sample when calculating persistent"
  " statistics (by ANALYZE, default 20)",
  NULL, NULL, 20, 1, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(stats_modified_counter, srv_stats_modified_counter,
  PLUGIN_VAR_RQCMDARG,
  "The number of rows modified before we calculate new statistics (default 0 = current limits)",
  NULL, NULL, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(stats_traditional, srv_stats_sample_traditional,
  PLUGIN_VAR_RQCMDARG,
  "Enable traditional statistic calculation based on number of configured pages (default true)",
  NULL, NULL, TRUE);

#ifdef BTR_CUR_HASH_ADAPT
static MYSQL_SYSVAR_BOOL(adaptive_hash_index, btr_search_enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable InnoDB adaptive hash index (disabled by default).",
  NULL, innodb_adaptive_hash_index_update, false);

/** Number of distinct partitions of AHI.
Each partition is protected by its own latch and so we have parts number
of latches protecting complete search system. */
static MYSQL_SYSVAR_ULONG(adaptive_hash_index_parts, btr_ahi_parts,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Number of InnoDB Adaptive Hash Index Partitions (default 8)",
  NULL, NULL, 8, 1, 512, 0);
#endif /* BTR_CUR_HASH_ADAPT */

static MYSQL_SYSVAR_UINT(compression_level, page_zip_level,
  PLUGIN_VAR_RQCMDARG,
  "Compression level used for zlib compression.  0 is no compression"
  ", 1 is fastest, 9 is best compression and default is 6.",
  NULL, NULL, DEFAULT_COMPRESSION_LEVEL, 0, 9, 0);

static MYSQL_SYSVAR_UINT(autoextend_increment,
  sys_tablespace_auto_extend_increment,
  PLUGIN_VAR_RQCMDARG,
  "Data file autoextend increment in megabytes",
  NULL, NULL, 64, 1, 1000, 0);

/** Validate the requested buffer pool size.  Also, reserve the necessary
memory needed for buffer pool resize.
@param[in]	thd	thread handle
@param[in]	var	pointer to system variable
@param[out]	save	immediate result for update function
@param[in]	value	incoming string
@return 0 on success, 1 on failure.
*/
static
int
innodb_buffer_pool_size_validate(
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				save,
	struct st_mysql_value*		value);

static MYSQL_SYSVAR_ULONGLONG(buffer_pool_size, innobase_buffer_pool_size,
  PLUGIN_VAR_RQCMDARG,
  "The size of the memory buffer InnoDB uses to cache data and indexes of its tables.",
  innodb_buffer_pool_size_validate,
  innodb_buffer_pool_size_update,
  srv_buf_pool_def_size,
  srv_buf_pool_min_size,
  LLONG_MAX, 1024*1024L);

static MYSQL_SYSVAR_ULONG(buffer_pool_chunk_size, srv_buf_pool_chunk_unit,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Size of a single memory chunk"
  " for resizing buffer pool. Online buffer pool resizing happens"
  " at this granularity. 0 means disable resizing buffer pool.",
  NULL, NULL,
  128 * 1024 * 1024, 1024 * 1024, LONG_MAX, 1024 * 1024);

static MYSQL_SYSVAR_STR(buffer_pool_filename, srv_buf_dump_filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Filename to/from which to dump/load the InnoDB buffer pool",
  innodb_srv_buf_dump_filename_validate, NULL, SRV_BUF_DUMP_FILENAME_DEFAULT);

static MYSQL_SYSVAR_BOOL(buffer_pool_dump_now, innodb_buffer_pool_dump_now,
  PLUGIN_VAR_RQCMDARG,
  "Trigger an immediate dump of the buffer pool into a file named @@innodb_buffer_pool_filename",
  NULL, buffer_pool_dump_now, FALSE);

static MYSQL_SYSVAR_BOOL(buffer_pool_dump_at_shutdown, srv_buffer_pool_dump_at_shutdown,
  PLUGIN_VAR_RQCMDARG,
  "Dump the buffer pool into a file named @@innodb_buffer_pool_filename",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_ULONG(buffer_pool_dump_pct, srv_buf_pool_dump_pct,
  PLUGIN_VAR_RQCMDARG,
  "Dump only the hottest N% of each buffer pool, defaults to 25",
  NULL, NULL, 25, 1, 100, 0);

#ifdef UNIV_DEBUG
/* Added to test the innodb_buffer_pool_load_incomplete status variable. */
static MYSQL_SYSVAR_ULONG(buffer_pool_load_pages_abort, srv_buf_pool_load_pages_abort,
  PLUGIN_VAR_RQCMDARG,
  "Number of pages during a buffer pool load to process before signaling innodb_buffer_pool_load_abort=1",
  NULL, NULL, LONG_MAX, 1, LONG_MAX, 0);

static MYSQL_SYSVAR_STR(buffer_pool_evict, srv_buffer_pool_evict,
  PLUGIN_VAR_RQCMDARG,
  "Evict pages from the buffer pool",
  NULL, innodb_buffer_pool_evict_update, "");
#endif /* UNIV_DEBUG */

static MYSQL_SYSVAR_BOOL(buffer_pool_load_now, innodb_buffer_pool_load_now,
  PLUGIN_VAR_RQCMDARG,
  "Trigger an immediate load of the buffer pool from a file named @@innodb_buffer_pool_filename",
  NULL, buffer_pool_load_now, FALSE);

static MYSQL_SYSVAR_BOOL(buffer_pool_load_abort, innodb_buffer_pool_load_abort,
  PLUGIN_VAR_RQCMDARG,
  "Abort a currently running load of the buffer pool",
  NULL, buffer_pool_load_abort, FALSE);

/* there is no point in changing this during runtime, thus readonly */
static MYSQL_SYSVAR_BOOL(buffer_pool_load_at_startup, srv_buffer_pool_load_at_startup,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Load the buffer pool from a file named @@innodb_buffer_pool_filename",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(defragment, srv_defragment,
  PLUGIN_VAR_RQCMDARG,
  "Enable/disable InnoDB defragmentation (default FALSE). When set to FALSE, all existing "
  "defragmentation will be paused. And new defragmentation command will fail."
  "Paused defragmentation commands will resume when this variable is set to "
  "true again.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_UINT(defragment_n_pages, srv_defragment_n_pages,
  PLUGIN_VAR_RQCMDARG,
  "Number of pages considered at once when merging multiple pages to "
  "defragment",
  NULL, NULL, 7, 2, 32, 0);

static MYSQL_SYSVAR_UINT(defragment_stats_accuracy,
  srv_defragment_stats_accuracy,
  PLUGIN_VAR_RQCMDARG,
  "How many defragment stats changes there are before the stats "
  "are written to persistent storage. Set to 0 meaning disable "
  "defragment stats tracking.",
  NULL, NULL, 0, 0, ~0U, 0);

static MYSQL_SYSVAR_UINT(defragment_fill_factor_n_recs,
  srv_defragment_fill_factor_n_recs,
  PLUGIN_VAR_RQCMDARG,
  "How many records of space defragmentation should leave on the page. "
  "This variable, together with innodb_defragment_fill_factor, is introduced "
  "so defragmentation won't pack the page too full and cause page split on "
  "the next insert on every page. The variable indicating more defragmentation"
  " gain is the one effective.",
  NULL, NULL, 20, 1, 100, 0);

static MYSQL_SYSVAR_DOUBLE(defragment_fill_factor, srv_defragment_fill_factor,
  PLUGIN_VAR_RQCMDARG,
  "A number between [0.7, 1] that tells defragmentation how full it should "
  "fill a page. Default is 0.9. Number below 0.7 won't make much sense."
  "This variable, together with innodb_defragment_fill_factor_n_recs, is "
  "introduced so defragmentation won't pack the page too full and cause "
  "page split on the next insert on every page. The variable indicating more "
  "defragmentation gain is the one effective.",
  NULL, NULL, 0.9, 0.7, 1, 0);

static MYSQL_SYSVAR_UINT(defragment_frequency, srv_defragment_frequency,
  PLUGIN_VAR_RQCMDARG,
  "Do not defragment a single index more than this number of time per second."
  "This controls the number of time defragmentation thread can request X_LOCK "
  "on an index. Defragmentation thread will check whether "
  "1/defragment_frequency (s) has passed since it worked on this index last "
  "time, and put the index back to the queue if not enough time has passed. "
  "The actual frequency can only be lower than this given number.",
  NULL, innodb_defragment_frequency_update,
  SRV_DEFRAGMENT_FREQUENCY_DEFAULT, 1, 1000, 0);


static MYSQL_SYSVAR_ULONG(lru_scan_depth, srv_LRU_scan_depth,
  PLUGIN_VAR_RQCMDARG,
  "How deep to scan LRU to keep it clean",
  NULL, NULL, 1536, 100, ~0UL, 0);

static MYSQL_SYSVAR_SIZE_T(lru_flush_size, innodb_lru_flush_size,
  PLUGIN_VAR_RQCMDARG,
  "How many pages to flush on LRU eviction",
  NULL, NULL, 32, 1, SIZE_T_MAX, 0);

static MYSQL_SYSVAR_ULONG(flush_neighbors, srv_flush_neighbors,
  PLUGIN_VAR_OPCMDARG,
  "Set to 0 (don't flush neighbors from buffer pool),"
  " 1 (flush contiguous neighbors from buffer pool)"
  " or 2 (flush neighbors from buffer pool),"
  " when flushing a block",
  NULL, NULL, 1, 0, 2, 0);

static MYSQL_SYSVAR_BOOL(deadlock_detect, innobase_deadlock_detect,
  PLUGIN_VAR_NOCMDARG,
  "Enable/disable InnoDB deadlock detector (default ON)."
  " if set to OFF, deadlock detection is skipped,"
  " and we rely on innodb_lock_wait_timeout in case of deadlock.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_UINT(fill_factor, innobase_fill_factor,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of B-tree page filled during bulk insert",
  NULL, NULL, 100, 10, 100, 0);

static MYSQL_SYSVAR_BOOL(ft_enable_diag_print, fts_enable_diag_print,
  PLUGIN_VAR_OPCMDARG,
  "Whether to enable additional FTS diagnostic printout ",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(disable_sort_file_cache, srv_disable_sort_file_cache,
  PLUGIN_VAR_OPCMDARG,
  "Whether to disable OS system file cache for sort I/O",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(ft_aux_table, innodb_ft_aux_table,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "FTS internal auxiliary table to be checked",
  innodb_ft_aux_table_validate, NULL, NULL);

static MYSQL_SYSVAR_ULONG(ft_cache_size, fts_max_cache_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search cache size in bytes",
  NULL, NULL, 8000000, 1600000, 80000000, 0);

static MYSQL_SYSVAR_ULONG(ft_total_cache_size, fts_max_total_cache_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Total memory allocated for InnoDB Fulltext Search cache",
  NULL, NULL, 640000000, 32000000, 1600000000, 0);

static MYSQL_SYSVAR_SIZE_T(ft_result_cache_limit, fts_result_cache_limit,
  PLUGIN_VAR_RQCMDARG,
  "InnoDB Fulltext search query result cache limit in bytes",
  NULL, NULL, 2000000000L, 1000000L, SIZE_T_MAX, 0);

static MYSQL_SYSVAR_ULONG(ft_min_token_size, fts_min_token_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search minimum token size in characters",
  NULL, NULL, 3, 0, 16, 0);

static MYSQL_SYSVAR_ULONG(ft_max_token_size, fts_max_token_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search maximum token size in characters",
  NULL, NULL, FTS_MAX_WORD_LEN_IN_CHAR, 10, FTS_MAX_WORD_LEN_IN_CHAR, 0);

static MYSQL_SYSVAR_ULONG(ft_num_word_optimize, fts_num_word_optimize,
  PLUGIN_VAR_OPCMDARG,
  "InnoDB Fulltext search number of words to optimize for each optimize table call ",
  NULL, NULL, 2000, 1000, 10000, 0);

static MYSQL_SYSVAR_ULONG(ft_sort_pll_degree, fts_sort_pll_degree,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB Fulltext search parallel sort degree, will round up to nearest power of 2 number",
  NULL, NULL, 2, 1, 16, 0);

static MYSQL_SYSVAR_ULONG(sort_buffer_size, srv_sort_buf_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Memory buffer size for index creation",
  NULL, NULL, 1048576, 65536, 64<<20, 0);

static MYSQL_SYSVAR_ULONGLONG(online_alter_log_max_size, srv_online_max_size,
  PLUGIN_VAR_RQCMDARG,
  "Maximum modification log file size for online index creation",
  NULL, NULL, 128<<20, 65536, ~0ULL, 0);

static MYSQL_SYSVAR_BOOL(optimize_fulltext_only, innodb_optimize_fulltext_only,
  PLUGIN_VAR_NOCMDARG,
  "Only optimize the Fulltext index of the table",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(read_io_threads, srv_n_read_io_threads,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of background read I/O threads in InnoDB.",
  NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ULONG(write_io_threads, srv_n_write_io_threads,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of background write I/O threads in InnoDB.",
  NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ULONG(force_recovery, srv_force_recovery,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Helps to save your data in case the disk image of the database becomes corrupt. Value 5 can return bogus data, and 6 can permanently corrupt data.",
  NULL, NULL, 0, 0, 6, 0);

static MYSQL_SYSVAR_ULONG(page_size, srv_page_size,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Page size to use for all InnoDB tablespaces.",
  NULL, NULL, UNIV_PAGE_SIZE_DEF,
  UNIV_PAGE_SIZE_MIN, UNIV_PAGE_SIZE_MAX, 0);

static MYSQL_SYSVAR_ULONG(log_buffer_size, srv_log_buffer_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "The size of the buffer which InnoDB uses to write log to the log files on disk.",
  NULL, NULL, 16L << 20, 256L << 10, LONG_MAX, 1024);

static MYSQL_SYSVAR_ULONGLONG(log_file_size, srv_log_file_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Size of each log file in a log group.",
  NULL, NULL, 96 << 20, 1 << 20, std::numeric_limits<ulonglong>::max(),
  UNIV_PAGE_SIZE_MAX);

static MYSQL_SYSVAR_ULONG(log_write_ahead_size, srv_log_write_ahead_size,
  PLUGIN_VAR_RQCMDARG,
  "Redo log write ahead unit size to avoid read-on-write,"
  " it should match the OS cache block IO size",
  NULL, innodb_log_write_ahead_size_update,
  8*1024L, OS_FILE_LOG_BLOCK_SIZE, UNIV_PAGE_SIZE_DEF, OS_FILE_LOG_BLOCK_SIZE);

static MYSQL_SYSVAR_UINT(old_blocks_pct, innobase_old_blocks_pct,
  PLUGIN_VAR_RQCMDARG,
  "Percentage of the buffer pool to reserve for 'old' blocks.",
  NULL, innodb_old_blocks_pct_update, 100 * 3 / 8, 5, 95, 0);

static MYSQL_SYSVAR_UINT(old_blocks_time, buf_LRU_old_threshold_ms,
  PLUGIN_VAR_RQCMDARG,
  "Move blocks to the 'new' end of the buffer pool if the first access"
  " was at least this many milliseconds ago."
  " The timeout is disabled if 0.",
  NULL, NULL, 1000, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_ULONG(open_files, innobase_open_files,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "How many files at the maximum InnoDB keeps open at the same time.",
  NULL, NULL, 0, 0, LONG_MAX, 0);

static MYSQL_SYSVAR_ULONG(sync_spin_loops, srv_n_spin_wait_rounds,
  PLUGIN_VAR_RQCMDARG,
  "Count of spin-loop rounds in InnoDB mutexes (30 by default)",
  NULL, NULL, 30L, 0L, ~0UL, 0);

static MYSQL_SYSVAR_UINT(spin_wait_delay, srv_spin_wait_delay,
  PLUGIN_VAR_OPCMDARG,
  "Maximum delay between polling for a spin lock (4 by default)",
  NULL, NULL, 4, 0, 6000, 0);

static MYSQL_SYSVAR_BOOL(prefix_index_cluster_optimization,
  srv_prefix_index_cluster_optimization,
  PLUGIN_VAR_OPCMDARG,
  "Enable prefix optimization to sometimes avoid cluster index lookups.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(data_file_path, innobase_data_file_path,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to individual files and their sizes.",
  NULL, NULL, "ibdata1:12M:autoextend");

static MYSQL_SYSVAR_STR(temp_data_file_path, innobase_temp_data_file_path,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path to files and their sizes making temp-tablespace.",
  NULL, NULL, "ibtmp1:12M:autoextend");

static MYSQL_SYSVAR_STR(undo_directory, srv_undo_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Directory where undo tablespace files live, this path can be absolute.",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_ULONG(undo_tablespaces, srv_undo_tablespaces,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of undo tablespaces to use.",
  NULL, NULL,
  0L,			/* Default seting */
  0L,			/* Minimum value */
  TRX_SYS_MAX_UNDO_SPACES, 0); /* Maximum value */

static MYSQL_SYSVAR_ULONGLONG(max_undo_log_size, srv_max_undo_log_size,
  PLUGIN_VAR_OPCMDARG,
  "Desired maximum UNDO tablespace size in bytes",
  NULL, NULL,
  10 << 20, 10 << 20,
  1ULL << (32 + UNIV_PAGE_SIZE_SHIFT_MAX), 0);

static MYSQL_SYSVAR_ULONG(purge_rseg_truncate_frequency,
  srv_purge_rseg_truncate_frequency,
  PLUGIN_VAR_OPCMDARG,
  "Dictates rate at which UNDO records are purged. Value N means"
  " purge rollback segment(s) on every Nth iteration of purge invocation",
  NULL, NULL, 128, 1, 128, 0);

static MYSQL_SYSVAR_BOOL(undo_log_truncate, srv_undo_log_truncate,
  PLUGIN_VAR_OPCMDARG,
  "Enable or Disable Truncate of UNDO tablespace.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_LONG(autoinc_lock_mode, innobase_autoinc_lock_mode,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "The AUTOINC lock modes supported by InnoDB:"
  " 0 => Old style AUTOINC locking (for backward compatibility);"
  " 1 => New style AUTOINC locking;"
  " 2 => No AUTOINC locking (unsafe for SBR)",
  NULL, NULL,
  AUTOINC_NEW_STYLE_LOCKING,	/* Default setting */
  AUTOINC_OLD_STYLE_LOCKING,	/* Minimum value */
  AUTOINC_NO_LOCKING, 0);	/* Maximum value */

static MYSQL_SYSVAR_STR(version, innodb_version_str,
  PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_READONLY,
  "InnoDB version", NULL, NULL, INNODB_VERSION_STR);

static MYSQL_SYSVAR_BOOL(use_native_aio, srv_use_native_aio,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Use native AIO if supported on this platform.",
  NULL, NULL, TRUE);

#ifdef HAVE_LIBNUMA
static MYSQL_SYSVAR_BOOL(numa_interleave, srv_numa_interleave,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Use NUMA interleave memory policy to allocate InnoDB buffer pool.",
  NULL, NULL, FALSE);
#endif /* HAVE_LIBNUMA */

static MYSQL_SYSVAR_ENUM(change_buffering, innodb_change_buffering,
  PLUGIN_VAR_RQCMDARG,
  "Buffer changes to secondary indexes.",
  NULL, NULL, IBUF_USE_ALL, &innodb_change_buffering_typelib);

static MYSQL_SYSVAR_UINT(change_buffer_max_size,
  srv_change_buffer_max_size,
  PLUGIN_VAR_RQCMDARG,
  "Maximum on-disk size of change buffer in terms of percentage"
  " of the buffer pool.",
  NULL, innodb_change_buffer_max_size_update,
  CHANGE_BUFFER_DEFAULT_SIZE, 0, 50, 0);

static MYSQL_SYSVAR_ENUM(stats_method, srv_innodb_stats_method,
   PLUGIN_VAR_RQCMDARG,
  "Specifies how InnoDB index statistics collection code should"
  " treat NULLs. Possible values are NULLS_EQUAL (default),"
  " NULLS_UNEQUAL and NULLS_IGNORED",
   NULL, NULL, SRV_STATS_NULLS_EQUAL, &innodb_stats_method_typelib);

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
static MYSQL_SYSVAR_BOOL(change_buffer_dump, ibuf_dump,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Dump the change buffer at startup.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_UINT(change_buffering_debug, ibuf_debug,
  PLUGIN_VAR_RQCMDARG,
  "Debug flags for InnoDB change buffering (0=none, 1=try to buffer)",
  NULL, NULL, 0, 0, 1, 0);
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

static MYSQL_SYSVAR_ULONG(buf_dump_status_frequency, srv_buf_dump_status_frequency,
  PLUGIN_VAR_RQCMDARG,
  "A number between [0, 100] that tells how oftern buffer pool dump status "
  "in percentages should be printed. E.g. 10 means that buffer pool dump "
  "status is printed when every 10% of number of buffer pool pages are "
  "dumped. Default is 0 (only start and end status is printed).",
  NULL, NULL, 0, 0, 100, 0);

#ifdef WITH_INNODB_DISALLOW_WRITES
/*******************************************************
 *    innobase_disallow_writes variable definition     *
 *******************************************************/
 
/* Must always init to FALSE. */
static my_bool	innobase_disallow_writes	= FALSE;

/**************************************************************************
An "update" method for innobase_disallow_writes variable. */
static
void
innobase_disallow_writes_update(THD*, st_mysql_sys_var*,
				void* var_ptr, const void* save)
{
	const my_bool val = *static_cast<const my_bool*>(save);
	*static_cast<my_bool*>(var_ptr) = val;
	ut_a(srv_allow_writes_event);
	mysql_mutex_unlock(&LOCK_global_system_variables);
	if (val) {
		os_event_reset(srv_allow_writes_event);
	} else {
		os_event_set(srv_allow_writes_event);
	}
	mysql_mutex_lock(&LOCK_global_system_variables);
}

static MYSQL_SYSVAR_BOOL(disallow_writes, innobase_disallow_writes,
  PLUGIN_VAR_NOCMDOPT,
  "Tell InnoDB to stop any writes to disk",
  NULL, innobase_disallow_writes_update, FALSE);
#endif /* WITH_INNODB_DISALLOW_WRITES */

static MYSQL_SYSVAR_BOOL(random_read_ahead, srv_random_read_ahead,
  PLUGIN_VAR_NOCMDARG,
  "Whether to use read ahead for random access within an extent.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(read_ahead_threshold, srv_read_ahead_threshold,
  PLUGIN_VAR_RQCMDARG,
  "Number of pages that must be accessed sequentially for InnoDB to"
  " trigger a readahead.",
  NULL, NULL, 56, 0, 64, 0);

static MYSQL_SYSVAR_STR(monitor_enable, innobase_enable_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Turn on a monitor counter",
  innodb_monitor_validate,
  innodb_enable_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_disable, innobase_disable_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Turn off a monitor counter",
  innodb_monitor_validate,
  innodb_disable_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_reset, innobase_reset_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Reset a monitor counter",
  innodb_monitor_validate,
  innodb_reset_monitor_update, NULL);

static MYSQL_SYSVAR_STR(monitor_reset_all, innobase_reset_all_monitor_counter,
  PLUGIN_VAR_RQCMDARG,
  "Reset all values for a monitor counter",
  innodb_monitor_validate,
  innodb_reset_all_monitor_update, NULL);

static MYSQL_SYSVAR_BOOL(status_output, srv_print_innodb_monitor,
  PLUGIN_VAR_OPCMDARG, "Enable InnoDB monitor output to the error log.",
  NULL, innodb_status_output_update, FALSE);

static MYSQL_SYSVAR_BOOL(status_output_locks, srv_print_innodb_lock_monitor,
  PLUGIN_VAR_OPCMDARG, "Enable InnoDB lock monitor output to the error log."
  " Requires innodb_status_output=ON.",
  NULL, innodb_status_output_update, FALSE);

static MYSQL_SYSVAR_BOOL(print_all_deadlocks, srv_print_all_deadlocks,
  PLUGIN_VAR_OPCMDARG,
  "Print all deadlocks to MariaDB error log (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_ULONG(compression_failure_threshold_pct,
  zip_failure_threshold_pct, PLUGIN_VAR_OPCMDARG,
  "If the compression failure rate of a table is greater than this number"
  " more padding is added to the pages to reduce the failures. A value of"
  " zero implies no padding",
  NULL, NULL, 5, 0, 100, 0);

static MYSQL_SYSVAR_ULONG(compression_pad_pct_max,
  zip_pad_max, PLUGIN_VAR_OPCMDARG,
  "Percentage of empty space on a data page that can be reserved"
  " to make the page compressible.",
  NULL, NULL, 50, 0, 75, 0);

static MYSQL_SYSVAR_BOOL(read_only, srv_read_only_mode,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Start InnoDB in read only mode (off by default)",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(read_only_compressed, innodb_read_only_compressed,
  PLUGIN_VAR_OPCMDARG,
  "Make ROW_FORMAT=COMPRESSED tables read-only (ON by default)",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(cmp_per_index_enabled, srv_cmp_per_index_enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable INFORMATION_SCHEMA.innodb_cmp_per_index,"
  " may have negative impact on performance (off by default)",
  NULL, innodb_cmp_per_index_update, FALSE);

static MYSQL_SYSVAR_ENUM(default_row_format, innodb_default_row_format,
  PLUGIN_VAR_RQCMDARG,
  "The default ROW FORMAT for all innodb tables created without explicit"
  " ROW_FORMAT. Possible values are REDUNDANT, COMPACT, and DYNAMIC."
  " The ROW_FORMAT value COMPRESSED is not allowed",
  NULL, NULL, DEFAULT_ROW_FORMAT_DYNAMIC,
  &innodb_default_row_format_typelib);

#ifdef UNIV_DEBUG
static MYSQL_SYSVAR_UINT(trx_rseg_n_slots_debug, trx_rseg_n_slots_debug,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_NOCMDOPT,
  "Debug flags for InnoDB to limit TRX_RSEG_N_SLOTS for trx_rsegf_undo_find_free()",
  NULL, NULL, 0, 0, 1024, 0);

static MYSQL_SYSVAR_UINT(limit_optimistic_insert_debug,
  btr_cur_limit_optimistic_insert_debug, PLUGIN_VAR_RQCMDARG,
  "Artificially limit the number of records per B-tree page (0=unlimited).",
  NULL, NULL, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_BOOL(trx_purge_view_update_only_debug,
  srv_purge_view_update_only_debug, PLUGIN_VAR_NOCMDOPT,
  "Pause actual purging any delete-marked records, but merely update the purge view."
  " It is to create artificially the situation the purge view have been updated"
  " but the each purges were not done yet.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(evict_tables_on_commit_debug,
  innodb_evict_tables_on_commit_debug, PLUGIN_VAR_OPCMDARG,
  "On transaction commit, try to evict tables from the data dictionary cache.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_UINT(data_file_size_debug,
  srv_sys_space_size_debug,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "InnoDB system tablespace size to be set in recovery.",
  NULL, NULL, 0, 0, 256U << 20, 0);

static MYSQL_SYSVAR_UINT(fil_make_page_dirty_debug,
  srv_fil_make_page_dirty_debug, PLUGIN_VAR_OPCMDARG,
  "Make the first page of the given tablespace dirty.",
  NULL, innodb_make_page_dirty, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_UINT(saved_page_number_debug,
  srv_saved_page_number_debug, PLUGIN_VAR_OPCMDARG,
  "An InnoDB page number.",
  NULL, NULL, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_BOOL(disable_resize_buffer_pool_debug,
  buf_disable_resize_buffer_pool_debug, PLUGIN_VAR_NOCMDARG,
  "Disable resizing buffer pool to make assertion code not expensive.",
  NULL, NULL, TRUE);

static MYSQL_SYSVAR_BOOL(page_cleaner_disabled_debug,
  innodb_page_cleaner_disabled_debug, PLUGIN_VAR_OPCMDARG,
  "Disable page cleaner",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(sync_debug, srv_sync_debug,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Enable the sync debug checks",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(dict_stats_disabled_debug,
  innodb_dict_stats_disabled_debug,
  PLUGIN_VAR_OPCMDARG,
  "Disable dict_stats thread",
  NULL, dict_stats_disabled_debug_update, FALSE);

static MYSQL_SYSVAR_BOOL(master_thread_disabled_debug,
  srv_master_thread_disabled_debug,
  PLUGIN_VAR_OPCMDARG,
  "Disable master thread",
  NULL, srv_master_thread_disabled_debug_update, FALSE);

static MYSQL_SYSVAR_UINT(simulate_comp_failures, srv_simulate_comp_failures,
  PLUGIN_VAR_NOCMDARG,
  "Simulate compression failures.",
  NULL, NULL, 0, 0, 99, 0);
#endif /* UNIV_DEBUG */

static MYSQL_SYSVAR_BOOL(force_primary_key,
  srv_force_primary_key,
  PLUGIN_VAR_OPCMDARG,
  "Do not allow to create table without primary key (off by default)",
  NULL, NULL, FALSE);

static const char *page_compression_algorithms[]= { "none", "zlib", "lz4", "lzo", "lzma", "bzip2", "snappy", 0 };
static TYPELIB page_compression_algorithms_typelib=
{
  array_elements(page_compression_algorithms) - 1, 0,
  page_compression_algorithms, 0
};
static MYSQL_SYSVAR_ENUM(compression_algorithm, innodb_compression_algorithm,
  PLUGIN_VAR_OPCMDARG,
  "Compression algorithm used on page compression. One of: none, zlib, lz4, lzo, lzma, bzip2, or snappy",
  innodb_compression_algorithm_validate, NULL,
  /* We use here the largest number of supported compression method to
  enable all those methods that are available. Availability of compression
  method is verified on innodb_compression_algorithm_validate function. */
  PAGE_ZLIB_ALGORITHM,
  &page_compression_algorithms_typelib);

static MYSQL_SYSVAR_ULONG(fatal_semaphore_wait_threshold, srv_fatal_semaphore_wait_threshold,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Maximum number of seconds that semaphore times out in InnoDB.",
  NULL, NULL,
  DEFAULT_SRV_FATAL_SEMAPHORE_TIMEOUT, /* Default setting */
  1, /* Minimum setting */
  UINT_MAX32, /* Maximum setting */
  0);

static const char* srv_encrypt_tables_names[] = { "OFF", "ON", "FORCE", 0 };
static TYPELIB srv_encrypt_tables_typelib = {
	array_elements(srv_encrypt_tables_names)-1, 0, srv_encrypt_tables_names,
	NULL
};
static MYSQL_SYSVAR_ENUM(encrypt_tables, srv_encrypt_tables,
			 PLUGIN_VAR_OPCMDARG,
			 "Enable encryption for tables. "
			 "Don't forget to enable --innodb-encrypt-log too",
			 innodb_encrypt_tables_validate,
			 innodb_encrypt_tables_update,
			 0,
			 &srv_encrypt_tables_typelib);

static MYSQL_SYSVAR_UINT(encryption_threads, srv_n_fil_crypt_threads,
			 PLUGIN_VAR_RQCMDARG,
			 "Number of threads performing background key rotation ",
			 NULL,
			 innodb_encryption_threads_update,
			 0, 0, 255, 0);

static MYSQL_SYSVAR_UINT(encryption_rotate_key_age,
			 srv_fil_crypt_rotate_key_age,
			 PLUGIN_VAR_RQCMDARG,
			 "Key rotation - re-encrypt in background "
                         "all pages that were encrypted with a key that "
                         "many (or more) versions behind. Value 0 indicates "
			 "that key rotation is disabled.",
			 NULL,
			 innodb_encryption_rotate_key_age_update,
			 1, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_UINT(encryption_rotation_iops, srv_n_fil_crypt_iops,
			 PLUGIN_VAR_RQCMDARG,
			 "Use this many iops for background key rotation",
			 NULL,
			 innodb_encryption_rotation_iops_update,
			 srv_n_fil_crypt_iops, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_BOOL(encrypt_log, srv_encrypt_log,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Enable redo log encryption",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(immediate_scrub_data_uncompressed,
			 srv_immediate_scrub_data_uncompressed,
			 0,
			 "Enable scrubbing of data",
			 NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(encrypt_temporary_tables, innodb_encrypt_temporary_tables,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "Enrypt the temporary table data.",
  NULL, NULL, false);

static struct st_mysql_sys_var* innobase_system_variables[]= {
  MYSQL_SYSVAR(autoextend_increment),
  MYSQL_SYSVAR(buffer_pool_size),
  MYSQL_SYSVAR(buffer_pool_chunk_size),
  MYSQL_SYSVAR(buffer_pool_filename),
  MYSQL_SYSVAR(buffer_pool_dump_now),
  MYSQL_SYSVAR(buffer_pool_dump_at_shutdown),
  MYSQL_SYSVAR(buffer_pool_dump_pct),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(buffer_pool_evict),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(buffer_pool_load_now),
  MYSQL_SYSVAR(buffer_pool_load_abort),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(buffer_pool_load_pages_abort),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(buffer_pool_load_at_startup),
  MYSQL_SYSVAR(defragment),
  MYSQL_SYSVAR(defragment_n_pages),
  MYSQL_SYSVAR(defragment_stats_accuracy),
  MYSQL_SYSVAR(defragment_fill_factor),
  MYSQL_SYSVAR(defragment_fill_factor_n_recs),
  MYSQL_SYSVAR(defragment_frequency),
  MYSQL_SYSVAR(lru_scan_depth),
  MYSQL_SYSVAR(lru_flush_size),
  MYSQL_SYSVAR(flush_neighbors),
  MYSQL_SYSVAR(checksum_algorithm),
  MYSQL_SYSVAR(compression_level),
  MYSQL_SYSVAR(data_file_path),
  MYSQL_SYSVAR(temp_data_file_path),
  MYSQL_SYSVAR(data_home_dir),
  MYSQL_SYSVAR(doublewrite),
  MYSQL_SYSVAR(stats_include_delete_marked),
  MYSQL_SYSVAR(use_atomic_writes),
  MYSQL_SYSVAR(fast_shutdown),
  MYSQL_SYSVAR(read_io_threads),
  MYSQL_SYSVAR(write_io_threads),
  MYSQL_SYSVAR(file_per_table),
  MYSQL_SYSVAR(flush_log_at_timeout),
  MYSQL_SYSVAR(flush_log_at_trx_commit),
  MYSQL_SYSVAR(flush_method),
  MYSQL_SYSVAR(force_recovery),
  MYSQL_SYSVAR(fill_factor),
  MYSQL_SYSVAR(ft_cache_size),
  MYSQL_SYSVAR(ft_total_cache_size),
  MYSQL_SYSVAR(ft_result_cache_limit),
  MYSQL_SYSVAR(ft_enable_stopword),
  MYSQL_SYSVAR(ft_max_token_size),
  MYSQL_SYSVAR(ft_min_token_size),
  MYSQL_SYSVAR(ft_num_word_optimize),
  MYSQL_SYSVAR(ft_sort_pll_degree),
  MYSQL_SYSVAR(force_load_corrupted),
  MYSQL_SYSVAR(lock_wait_timeout),
  MYSQL_SYSVAR(deadlock_detect),
  MYSQL_SYSVAR(page_size),
  MYSQL_SYSVAR(log_buffer_size),
  MYSQL_SYSVAR(log_file_size),
  MYSQL_SYSVAR(log_write_ahead_size),
  MYSQL_SYSVAR(log_group_home_dir),
  MYSQL_SYSVAR(max_dirty_pages_pct),
  MYSQL_SYSVAR(max_dirty_pages_pct_lwm),
  MYSQL_SYSVAR(adaptive_flushing_lwm),
  MYSQL_SYSVAR(adaptive_flushing),
  MYSQL_SYSVAR(flush_sync),
  MYSQL_SYSVAR(flushing_avg_loops),
  MYSQL_SYSVAR(max_purge_lag),
  MYSQL_SYSVAR(max_purge_lag_delay),
  MYSQL_SYSVAR(max_purge_lag_wait),
  MYSQL_SYSVAR(old_blocks_pct),
  MYSQL_SYSVAR(old_blocks_time),
  MYSQL_SYSVAR(open_files),
  MYSQL_SYSVAR(optimize_fulltext_only),
  MYSQL_SYSVAR(rollback_on_timeout),
  MYSQL_SYSVAR(ft_aux_table),
  MYSQL_SYSVAR(ft_enable_diag_print),
  MYSQL_SYSVAR(ft_server_stopword_table),
  MYSQL_SYSVAR(ft_user_stopword_table),
  MYSQL_SYSVAR(disable_sort_file_cache),
  MYSQL_SYSVAR(stats_on_metadata),
  MYSQL_SYSVAR(stats_transient_sample_pages),
  MYSQL_SYSVAR(stats_persistent),
  MYSQL_SYSVAR(stats_persistent_sample_pages),
  MYSQL_SYSVAR(stats_auto_recalc),
  MYSQL_SYSVAR(stats_modified_counter),
  MYSQL_SYSVAR(stats_traditional),
#ifdef BTR_CUR_HASH_ADAPT
  MYSQL_SYSVAR(adaptive_hash_index),
  MYSQL_SYSVAR(adaptive_hash_index_parts),
#endif /* BTR_CUR_HASH_ADAPT */
  MYSQL_SYSVAR(stats_method),
  MYSQL_SYSVAR(status_file),
  MYSQL_SYSVAR(strict_mode),
  MYSQL_SYSVAR(sort_buffer_size),
  MYSQL_SYSVAR(online_alter_log_max_size),
  MYSQL_SYSVAR(sync_spin_loops),
  MYSQL_SYSVAR(spin_wait_delay),
  MYSQL_SYSVAR(table_locks),
  MYSQL_SYSVAR(prefix_index_cluster_optimization),
  MYSQL_SYSVAR(tmpdir),
  MYSQL_SYSVAR(autoinc_lock_mode),
  MYSQL_SYSVAR(version),
  MYSQL_SYSVAR(use_native_aio),
#ifdef HAVE_LIBNUMA
  MYSQL_SYSVAR(numa_interleave),
#endif /* HAVE_LIBNUMA */
  MYSQL_SYSVAR(change_buffering),
  MYSQL_SYSVAR(change_buffer_max_size),
#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
  MYSQL_SYSVAR(change_buffer_dump),
  MYSQL_SYSVAR(change_buffering_debug),
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
#ifdef WITH_INNODB_DISALLOW_WRITES
  MYSQL_SYSVAR(disallow_writes),
#endif /* WITH_INNODB_DISALLOW_WRITES */
  MYSQL_SYSVAR(random_read_ahead),
  MYSQL_SYSVAR(read_ahead_threshold),
  MYSQL_SYSVAR(read_only),
  MYSQL_SYSVAR(read_only_compressed),
  MYSQL_SYSVAR(instant_alter_column_allowed),
  MYSQL_SYSVAR(io_capacity),
  MYSQL_SYSVAR(io_capacity_max),
  MYSQL_SYSVAR(idle_flush_pct),
  MYSQL_SYSVAR(monitor_enable),
  MYSQL_SYSVAR(monitor_disable),
  MYSQL_SYSVAR(monitor_reset),
  MYSQL_SYSVAR(monitor_reset_all),
  MYSQL_SYSVAR(purge_threads),
  MYSQL_SYSVAR(purge_batch_size),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(background_drop_list_empty),
  MYSQL_SYSVAR(log_checkpoint_now),
  MYSQL_SYSVAR(buf_flush_list_now),
  MYSQL_SYSVAR(merge_threshold_set_all_debug),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(status_output),
  MYSQL_SYSVAR(status_output_locks),
  MYSQL_SYSVAR(print_all_deadlocks),
  MYSQL_SYSVAR(cmp_per_index_enabled),
  MYSQL_SYSVAR(max_undo_log_size),
  MYSQL_SYSVAR(purge_rseg_truncate_frequency),
  MYSQL_SYSVAR(undo_log_truncate),
  MYSQL_SYSVAR(undo_directory),
  MYSQL_SYSVAR(undo_tablespaces),
  MYSQL_SYSVAR(sync_array_size),
  MYSQL_SYSVAR(compression_failure_threshold_pct),
  MYSQL_SYSVAR(compression_pad_pct_max),
  MYSQL_SYSVAR(default_row_format),
#ifdef UNIV_DEBUG
  MYSQL_SYSVAR(simulate_comp_failures),
  MYSQL_SYSVAR(trx_rseg_n_slots_debug),
  MYSQL_SYSVAR(limit_optimistic_insert_debug),
  MYSQL_SYSVAR(trx_purge_view_update_only_debug),
  MYSQL_SYSVAR(evict_tables_on_commit_debug),
  MYSQL_SYSVAR(data_file_size_debug),
  MYSQL_SYSVAR(fil_make_page_dirty_debug),
  MYSQL_SYSVAR(saved_page_number_debug),
  MYSQL_SYSVAR(disable_resize_buffer_pool_debug),
  MYSQL_SYSVAR(page_cleaner_disabled_debug),
  MYSQL_SYSVAR(dict_stats_disabled_debug),
  MYSQL_SYSVAR(master_thread_disabled_debug),
  MYSQL_SYSVAR(sync_debug),
#endif /* UNIV_DEBUG */
  MYSQL_SYSVAR(force_primary_key),
  MYSQL_SYSVAR(fatal_semaphore_wait_threshold),
  /* Table page compression feature */
  MYSQL_SYSVAR(compression_default),
  MYSQL_SYSVAR(compression_algorithm),
  /* Encryption feature */
  MYSQL_SYSVAR(encrypt_tables),
  MYSQL_SYSVAR(encryption_threads),
  MYSQL_SYSVAR(encryption_rotate_key_age),
  MYSQL_SYSVAR(encryption_rotation_iops),
  MYSQL_SYSVAR(encrypt_log),
  MYSQL_SYSVAR(default_encryption_key_id),
  MYSQL_SYSVAR(immediate_scrub_data_uncompressed),
  MYSQL_SYSVAR(buf_dump_status_frequency),
  MYSQL_SYSVAR(background_thread),
  MYSQL_SYSVAR(encrypt_temporary_tables),

  NULL
};

maria_declare_plugin(innobase)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &innobase_storage_engine,
  innobase_hton_name,
  plugin_author,
  "Supports transactions, row-level locking, foreign keys and encryption for tables",
  PLUGIN_LICENSE_GPL,
  innodb_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  INNODB_VERSION_SHORT,
  innodb_status_variables_export,/* status variables             */
  innobase_system_variables, /* system variables */
  INNODB_VERSION_STR,         /* string version */
  MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
},
i_s_innodb_trx,
i_s_innodb_locks,
i_s_innodb_lock_waits,
i_s_innodb_cmp,
i_s_innodb_cmp_reset,
i_s_innodb_cmpmem,
i_s_innodb_cmpmem_reset,
i_s_innodb_cmp_per_index,
i_s_innodb_cmp_per_index_reset,
i_s_innodb_buffer_page,
i_s_innodb_buffer_page_lru,
i_s_innodb_buffer_stats,
i_s_innodb_metrics,
i_s_innodb_ft_default_stopword,
i_s_innodb_ft_deleted,
i_s_innodb_ft_being_deleted,
i_s_innodb_ft_config,
i_s_innodb_ft_index_cache,
i_s_innodb_ft_index_table,
i_s_innodb_sys_tables,
i_s_innodb_sys_tablestats,
i_s_innodb_sys_indexes,
i_s_innodb_sys_columns,
i_s_innodb_sys_fields,
i_s_innodb_sys_foreign,
i_s_innodb_sys_foreign_cols,
i_s_innodb_sys_tablespaces,
i_s_innodb_sys_virtual,
i_s_innodb_mutexes,
i_s_innodb_sys_semaphore_waits,
i_s_innodb_tablespaces_encryption
maria_declare_plugin_end;

/** @brief Adjust some InnoDB startup parameters based on file contents
or innodb_page_size. */
static
void
innodb_params_adjust()
{
	MYSQL_SYSVAR_NAME(max_undo_log_size).max_val
		= 1ULL << (32U + srv_page_size_shift);
	MYSQL_SYSVAR_NAME(max_undo_log_size).min_val
		= MYSQL_SYSVAR_NAME(max_undo_log_size).def_val
		= ulonglong(SRV_UNDO_TABLESPACE_SIZE_IN_PAGES)
		<< srv_page_size_shift;
	MYSQL_SYSVAR_NAME(max_undo_log_size).max_val
		= 1ULL << (32U + srv_page_size_shift);
}

/****************************************************************************
 * DS-MRR implementation
 ***************************************************************************/

/**
Multi Range Read interface, DS-MRR calls */
int
ha_innobase::multi_range_read_init(
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint		mode,
	HANDLER_BUFFER*	buf)
{
	return(m_ds_mrr.dsmrr_init(this, seq, seq_init_param,
				 n_ranges, mode, buf));
}

int
ha_innobase::multi_range_read_next(
	range_id_t*		range_info)
{
	return(m_ds_mrr.dsmrr_next(range_info));
}

ha_rows
ha_innobase::multi_range_read_info_const(
	uint		keyno,
	RANGE_SEQ_IF*	seq,
	void*		seq_init_param,
	uint		n_ranges,
	uint*		bufsz,
	uint*		flags,
	Cost_estimate*	cost)
{
	/* See comments in ha_myisam::multi_range_read_info_const */
	m_ds_mrr.init(this, table);

	if (m_prebuilt->select_lock_type != LOCK_NONE) {
		*flags |= HA_MRR_USE_DEFAULT_IMPL;
	}

	ha_rows res= m_ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges,
			bufsz, flags, cost);
	return res;
}

ha_rows
ha_innobase::multi_range_read_info(
	uint		keyno,
	uint		n_ranges,
	uint		keys,
	uint		key_parts,
	uint*		bufsz,
	uint*		flags,
	Cost_estimate*	cost)
{
	m_ds_mrr.init(this, table);
	ha_rows res= m_ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz,
					flags, cost);
	return res;
}

int
ha_innobase::multi_range_read_explain_info(
	uint mrr_mode,
	char *str,
	size_t size)
{
	return m_ds_mrr.dsmrr_explain_info(mrr_mode, str, size);
}

/** Find or open a table handle for the virtual column template
@param[in]	thd	thread handle
@param[in,out]	table	InnoDB table whose virtual column template
			is to be updated
@return table handle
@retval NULL if the table is dropped, unaccessible or corrupted
for purge thread */
static TABLE* innodb_find_table_for_vc(THD* thd, dict_table_t* table)
{
	TABLE *mysql_table;
	const bool  bg_thread = THDVAR(thd, background_thread);

	if (bg_thread) {
		if ((mysql_table = get_purge_table(thd))) {
			return mysql_table;
		}
	} else {
		if (table->vc_templ->mysql_table_query_id
		    == thd_get_query_id(thd)) {
			return table->vc_templ->mysql_table;
		}
	}

	char	db_buf[NAME_LEN + 1];
	char	tbl_buf[NAME_LEN + 1];
	ulint	db_buf_len, tbl_buf_len;

	if (!table->parse_name(db_buf, tbl_buf, &db_buf_len, &tbl_buf_len)) {
		return NULL;
	}

	if (bg_thread) {
		return open_purge_table(thd, db_buf, db_buf_len,
					tbl_buf, tbl_buf_len);
	}

	mysql_table = find_fk_open_table(thd, db_buf, db_buf_len,
					 tbl_buf, tbl_buf_len);
	table->vc_templ->mysql_table = mysql_table;
	table->vc_templ->mysql_table_query_id = thd_get_query_id(thd);
	return mysql_table;
}

/** Get the computed value by supplying the base column values.
@param[in,out]	table		table whose virtual column
				template to be built */
TABLE* innobase_init_vc_templ(dict_table_t* table)
{
	if (table->vc_templ != NULL) {
		return NULL;
	}
	DBUG_ENTER("innobase_init_vc_templ");

	table->vc_templ = UT_NEW_NOKEY(dict_vcol_templ_t());

	TABLE	*mysql_table= innodb_find_table_for_vc(current_thd, table);

	ut_ad(mysql_table);
	if (!mysql_table) {
		DBUG_RETURN(NULL);
	}

	mutex_enter(&dict_sys.mutex);
	innobase_build_v_templ(mysql_table, table, table->vc_templ, NULL, true);
	mutex_exit(&dict_sys.mutex);
	DBUG_RETURN(mysql_table);
}

/** Change dbname and table name in table->vc_templ.
@param[in,out]	table	the table whose virtual column template
dbname and tbname to be renamed. */
void
innobase_rename_vc_templ(
	dict_table_t*	table)
{
	char	dbname[MAX_DATABASE_NAME_LEN + 1];
	char	tbname[MAX_DATABASE_NAME_LEN + 1];
	char*	name = table->name.m_name;
	ulint	dbnamelen = dict_get_db_name_len(name);
	ulint	tbnamelen = strlen(name) - dbnamelen - 1;
	char	t_dbname[MAX_DATABASE_NAME_LEN + 1];
	char	t_tbname[MAX_TABLE_NAME_LEN + 1];

	strncpy(dbname, name, dbnamelen);
	dbname[dbnamelen] = 0;
	strncpy(tbname, name + dbnamelen + 1, tbnamelen);
	tbname[tbnamelen] =0;

	/* For partition table, remove the partition name and use the
	"main" table name to build the template */
	char*	is_part = is_partition(tbname);

	if (is_part != NULL) {
		*is_part = '\0';
		tbnamelen = ulint(is_part - tbname);
	}

	dbnamelen = filename_to_tablename(dbname, t_dbname,
					  MAX_DATABASE_NAME_LEN + 1);
	tbnamelen = filename_to_tablename(tbname, t_tbname,
					  MAX_TABLE_NAME_LEN + 1);

	table->vc_templ->db_name = t_dbname;
	table->vc_templ->tb_name = t_tbname;
}

/** Get the updated parent field value from the update vector for the
given col_no.
@param[in]	foreign		foreign key information
@param[in]	update		updated parent vector.
@param[in]	col_no		base column position of the child table to check
@return updated field from the parent update vector, else NULL */
static
dfield_t*
innobase_get_field_from_update_vector(
	dict_foreign_t*	foreign,
	upd_t*		update,
	ulint		col_no)
{
	dict_table_t*	parent_table = foreign->referenced_table;
	dict_index_t*	parent_index = foreign->referenced_index;
	ulint		parent_field_no;
	ulint		parent_col_no;
	ulint		prefix_col_no;

	for (ulint i = 0; i < foreign->n_fields; i++) {
		if (dict_index_get_nth_col_no(foreign->foreign_index, i)
		    != col_no) {
			continue;
		}

		parent_col_no = dict_index_get_nth_col_no(parent_index, i);
		parent_field_no = dict_table_get_nth_col_pos(
			parent_table, parent_col_no, &prefix_col_no);

		for (ulint j = 0; j < update->n_fields; j++) {
			upd_field_t*	parent_ufield
				= &update->fields[j];

			if (parent_ufield->field_no == parent_field_no) {
				return(&parent_ufield->new_val);
			}
		}
	}

	return (NULL);
}


/**
   Allocate a heap and record for calculating virtual fields
   Used mainly for virtual fields in indexes

@param[in]      thd             MariaDB THD
@param[in]      index           Index in use
@param[out]     heap            Heap that holds temporary row
@param[in,out]  table           MariaDB table
@param[out]     record	        Pointer to allocated MariaDB record
@param[out]     storage	        Internal storage for blobs etc

@retval		true on success
@retval		false on malloc failure or failed to open the maria table
		for purge thread.
*/

bool innobase_allocate_row_for_vcol(THD *thd, dict_index_t *index,
                                    mem_heap_t **heap, TABLE **table,
                                    VCOL_STORAGE *storage)
{
  TABLE *maria_table;
  String *blob_value_storage;
  if (!*table)
    *table = innodb_find_table_for_vc(thd, index->table);

  /* For purge thread, there is a possiblity that table could have
     dropped, corrupted or unaccessible. */
  if (!*table)
    return false;
  maria_table = *table;
  if (!*heap && !(*heap = mem_heap_create(srv_page_size)))
    return false;

  uchar *record = static_cast<byte *>(mem_heap_alloc(*heap,
                                                    maria_table->s->reclength));

  size_t len = maria_table->s->virtual_not_stored_blob_fields * sizeof(String);
  blob_value_storage = static_cast<String *>(mem_heap_alloc(*heap, len));

  if (!record || !blob_value_storage)
    return false;

  storage->maria_table = maria_table;
  storage->innobase_record = record;
  storage->maria_record = maria_table->field[0]->record_ptr();
  storage->blob_value_storage = blob_value_storage;

  maria_table->move_fields(maria_table->field, record, storage->maria_record);
  maria_table->remember_blob_values(blob_value_storage);

  return true;
}


/** Free memory allocated by innobase_allocate_row_for_vcol() */

void innobase_free_row_for_vcol(VCOL_STORAGE *storage)
{
	TABLE *maria_table= storage->maria_table;
	maria_table->move_fields(maria_table->field, storage->maria_record,
                                 storage->innobase_record);
        maria_table->restore_blob_values(storage->blob_value_storage);
}


void innobase_report_computed_value_failed(dtuple_t *row)
{
  ib::error() << "Compute virtual column values failed for "
              << rec_printer(row).str();
}


/** Get the computed value by supplying the base column values.
@param[in,out]	row		the data row
@param[in]	col		virtual column
@param[in]	index		index
@param[in,out]	local_heap	heap memory for processing large data etc.
@param[in,out]	heap		memory heap that copies the actual index row
@param[in]	ifield		index field
@param[in]	thd		MySQL thread handle
@param[in,out]	mysql_table	mysql table object
@param[in]	old_table	during ALTER TABLE, this is the old table
				or NULL.
@param[in]	parent_update	update vector for the parent row
@param[in]	foreign		foreign key information
@return the field filled with computed value, or NULL if just want
to store the value in passed in "my_rec" */
dfield_t*
innobase_get_computed_value(
	dtuple_t*		row,
	const dict_v_col_t*	col,
	const dict_index_t*	index,
	mem_heap_t**		local_heap,
	mem_heap_t*		heap,
	const dict_field_t*	ifield,
	THD*			thd,
	TABLE*			mysql_table,
	byte*			mysql_rec,
	const dict_table_t*	old_table,
	upd_t*			parent_update,
	dict_foreign_t*		foreign)
{
	byte		rec_buf2[REC_VERSION_56_MAX_INDEX_COL_LEN];
	byte*		buf;
	dfield_t*	field;
	ulint		len;

	const ulint zip_size = old_table
		? old_table->space->zip_size()
		: dict_tf_get_zip_size(index->table->flags);

	ulint		ret = 0;

	ut_ad(index->table->vc_templ);
	ut_ad(thd != NULL);
	ut_ad(mysql_table);

	DBUG_ENTER("innobase_get_computed_value");
	const mysql_row_templ_t*
			vctempl =  index->table->vc_templ->vtempl[
				index->table->vc_templ->n_col + col->v_pos];

	if (!heap || index->table->vc_templ->rec_len
		     >= REC_VERSION_56_MAX_INDEX_COL_LEN) {
		if (*local_heap == NULL) {
			*local_heap = mem_heap_create(srv_page_size);
		}

		buf = static_cast<byte*>(mem_heap_alloc(
				*local_heap, index->table->vc_templ->rec_len));
	} else {
		buf = rec_buf2;
	}

	for (ulint i = 0; i < unsigned{col->num_base}; i++) {
		dict_col_t*			base_col = col->base_col[i];
		const dfield_t*			row_field = NULL;
		ulint				col_no = base_col->ind;
		const mysql_row_templ_t*	templ
			= index->table->vc_templ->vtempl[col_no];
		const byte*			data;

		if (parent_update != NULL) {
			/** Get the updated field from update vector
			of the parent table. */
			row_field = innobase_get_field_from_update_vector(
					foreign, parent_update, col_no);
		}

		if (row_field == NULL) {
			row_field = dtuple_get_nth_field(row, col_no);
		}

		data = static_cast<const byte*>(row_field->data);
		len = row_field->len;

		if (row_field->ext) {
			if (*local_heap == NULL) {
				*local_heap = mem_heap_create(srv_page_size);
			}

			data = btr_copy_externally_stored_field(
				&len, data, zip_size,
				dfield_get_len(row_field), *local_heap);
		}

		if (len == UNIV_SQL_NULL) {
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion" /* GCC 5 may need this here */
#endif
                        mysql_rec[templ->mysql_null_byte_offset]
                                |= (byte) templ->mysql_null_bit_mask;
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif
                        memcpy(mysql_rec + templ->mysql_col_offset,
                               static_cast<const byte*>(
					index->table->vc_templ->default_rec
					+ templ->mysql_col_offset),
                               templ->mysql_col_len);
                } else {

			row_sel_field_store_in_mysql_format(
				mysql_rec + templ->mysql_col_offset,
				templ, index, templ->clust_rec_field_no,
				(const byte*)data, len);

			if (templ->mysql_null_bit_mask) {
				/* It is a nullable column with a
				non-NULL value */
				mysql_rec[templ->mysql_null_byte_offset]
					&= static_cast<byte>(
						~templ->mysql_null_bit_mask);
			}
		}
	}

	field = dtuple_get_nth_v_field(row, col->v_pos);

	my_bitmap_map*	old_write_set = dbug_tmp_use_all_columns(mysql_table, mysql_table->write_set);
	my_bitmap_map*	old_read_set = dbug_tmp_use_all_columns(mysql_table, mysql_table->read_set);
	ret = mysql_table->update_virtual_field(mysql_table->field[col->m_col.ind]);
	dbug_tmp_restore_column_map(mysql_table->read_set, old_read_set);
	dbug_tmp_restore_column_map(mysql_table->write_set, old_write_set);

	if (ret != 0) {
		DBUG_RETURN(NULL);
	}

	if (vctempl->mysql_null_bit_mask
	    && (mysql_rec[vctempl->mysql_null_byte_offset]
	        & vctempl->mysql_null_bit_mask)) {
		dfield_set_null(field);
		field->type.prtype |= DATA_VIRTUAL;
		DBUG_RETURN(field);
	}

	row_mysql_store_col_in_innobase_format(
		field, buf,
		TRUE, mysql_rec + vctempl->mysql_col_offset,
		vctempl->mysql_col_len, dict_table_is_comp(index->table));
	field->type.prtype |= DATA_VIRTUAL;

	ulint	max_prefix = col->m_col.max_prefix;

	if (max_prefix && ifield
	    && (ifield->prefix_len == 0
	        || ifield->prefix_len > col->m_col.max_prefix)) {
		max_prefix = ifield->prefix_len;
	}

	/* If this is a prefix index, we only need a portion of the field */
	if (max_prefix) {
		len = dtype_get_at_most_n_mbchars(
			col->m_col.prtype,
			col->m_col.mbminlen, col->m_col.mbmaxlen,
			max_prefix,
			field->len,
			static_cast<char*>(dfield_get_data(field)));
		dfield_set_len(field, len);
	}

	if (heap) {
		dfield_dup(field, heap);
	}

	DBUG_RETURN(field);
}


/** Attempt to push down an index condition.
@param[in] keyno MySQL key number
@param[in] idx_cond Index condition to be checked
@return Part of idx_cond which the handler will not evaluate */

class Item*
ha_innobase::idx_cond_push(
	uint		keyno,
	class Item*	idx_cond)
{
	DBUG_ENTER("ha_innobase::idx_cond_push");
	DBUG_ASSERT(keyno != MAX_KEY);
	DBUG_ASSERT(idx_cond != NULL);

	/* We can only evaluate the condition if all columns are stored.*/
	dict_index_t* idx  = innobase_get_index(keyno);
	if (idx && dict_index_has_virtual(idx)) {
		DBUG_RETURN(idx_cond);
	}

	pushed_idx_cond = idx_cond;
	pushed_idx_cond_keyno = keyno;
	in_range_check_pushed_down = TRUE;
	/* We will evaluate the condition entirely */
	DBUG_RETURN(NULL);
}


/** Push a primary key filter.
@param[in]	pk_filter	filter against which primary keys
				are to be checked
@retval	false if pushed (always) */
bool ha_innobase::rowid_filter_push(Rowid_filter* pk_filter)
{
	DBUG_ENTER("ha_innobase::rowid_filter_push");
	DBUG_ASSERT(pk_filter != NULL);
	pushed_rowid_filter= pk_filter;
	DBUG_RETURN(false);
}

static bool is_part_of_a_key_prefix(const Field_longstr *field)
{
  const TABLE_SHARE *s= field->table->s;

  for (uint i= 0; i < s->keys; i++)
  {
    const KEY &key= s->key_info[i];
    for (uint j= 0; j < key.user_defined_key_parts; j++)
    {
      const KEY_PART_INFO &info= key.key_part[j];
      // When field is a part of some key, a key part and field will have the
      // same length. And their length will be different when only some prefix
      // of a field is used as a key part. That's what we're looking for here.
      if (info.field->field_index == field->field_index &&
          info.length != field->field_length)
      {
        DBUG_ASSERT(info.length < field->field_length);
        return true;
      }
    }
  }

  return false;
}

static bool
is_part_of_a_primary_key(const Field* field)
{
	const TABLE_SHARE* s = field->table->s;

	return s->primary_key != MAX_KEY
	       && field->part_of_key.is_set(s->primary_key);
}

bool ha_innobase::can_convert_string(const Field_string *field,
                                     const Column_definition &new_type) const
{
  DBUG_ASSERT(!field->compression_method());
  if (new_type.type_handler() != field->type_handler())
    return false;

  if (new_type.char_length != field->char_length())
    return false;

  const Charset field_cs(field->charset());

  if (new_type.length != field->max_display_length() &&
      (!m_prebuilt->table->not_redundant() ||
       field_cs.mbminlen() == field_cs.mbmaxlen()))
    return false;

  if (new_type.charset != field->charset())
  {
    if (!field_cs.encoding_allows_reinterpret_as(new_type.charset))
      return false;

    if (!field_cs.eq_collation_specific_names(new_type.charset))
      return !is_part_of_a_primary_key(field);

    // Fully indexed case works instantly like
    // Compare_keys::EqualButKeyPartLength. But prefix case isn't implemented.
    if (is_part_of_a_key_prefix(field))
	    return false;

    return true;
  }

  return true;
}

static bool
supports_enlarging(const dict_table_t* table, const Field_varstring* field,
		   const Column_definition& new_type)
{
	return field->field_length <= 127 || new_type.length <= 255
	       || field->field_length > 255 || !table->not_redundant();
}

bool ha_innobase::can_convert_varstring(
    const Field_varstring *field, const Column_definition &new_type) const
{
  if (new_type.length < field->field_length)
    return false;

  if (new_type.char_length < field->char_length())
    return false;

  if (!new_type.compression_method() != !field->compression_method())
    return false;

  if (new_type.type_handler() != field->type_handler())
    return false;

  if (new_type.charset != field->charset())
  {
    if (!supports_enlarging(m_prebuilt->table, field, new_type))
      return false;

    Charset field_cs(field->charset());
    if (!field_cs.encoding_allows_reinterpret_as(new_type.charset))
      return false;

    if (!field_cs.eq_collation_specific_names(new_type.charset))
      return !is_part_of_a_primary_key(field);

    // Fully indexed case works instantly like
    // Compare_keys::EqualButKeyPartLength. But prefix case isn't implemented.
    if (is_part_of_a_key_prefix(field))
      return false;

    return true;
  }

  if (new_type.length != field->field_length)
  {
    if (!supports_enlarging(m_prebuilt->table, field, new_type))
      return false;

    return true;
  }

  return true;
}

static bool is_part_of_a_key(const Field_blob *field)
{
  const TABLE_SHARE *s= field->table->s;

  for (uint i= 0; i < s->keys; i++)
  {
    const KEY &key= s->key_info[i];
    for (uint j= 0; j < key.user_defined_key_parts; j++)
    {
      const KEY_PART_INFO &info= key.key_part[j];
      if (info.field->field_index == field->field_index)
        return true;
    }
  }

  return false;
}

bool ha_innobase::can_convert_blob(const Field_blob *field,
                                   const Column_definition &new_type) const
{
  if (new_type.type_handler() != field->type_handler())
    return false;

  if (!new_type.compression_method() != !field->compression_method())
    return false;

  if (new_type.pack_length != field->pack_length())
    return false;

  if (new_type.charset != field->charset())
  {
    Charset field_cs(field->charset());
    if (!field_cs.encoding_allows_reinterpret_as(new_type.charset))
      return false;

    if (!field_cs.eq_collation_specific_names(new_type.charset))
      return !is_part_of_a_key(field);

    // Fully indexed case works instantly like
    // Compare_keys::EqualButKeyPartLength. But prefix case isn't implemented.
    if (is_part_of_a_key_prefix(field))
      return false;

    return true;
  }

  return true;
}

Compare_keys ha_innobase::compare_key_parts(
    const Field &old_field, const Column_definition &new_field,
    const KEY_PART_INFO &old_part, const KEY_PART_INFO &new_part) const
{
  const bool is_equal= old_field.is_equal(new_field);
  const CHARSET_INFO *old_cs= old_field.charset();
  const CHARSET_INFO *new_cs= new_field.charset;

  if (!is_equal)
  {
    if (!old_field.can_be_converted_by_engine(new_field))
      return Compare_keys::NotEqual;

    if (!Charset(old_cs).eq_collation_specific_names(new_cs))
      return Compare_keys::NotEqual;
  }

  if (old_part.length / old_cs->mbmaxlen != new_part.length / new_cs->mbmaxlen)
  {
    if (old_part.length != old_field.field_length)
      return Compare_keys::NotEqual;

    if (old_part.length >= new_part.length)
      return Compare_keys::NotEqual;

    return Compare_keys::EqualButKeyPartLength;
  }

  return Compare_keys::Equal;
}

/******************************************************************//**
Use this when the args are passed to the format string from
errmsg-utf8.txt directly as is.

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_condition_level level,
	uint code, const char *format, ...);
*/
void
ib_senderrf(
/*========*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	...)				/*!< Args */
{
	va_list		args;
	const char*	format = my_get_err_msg(code);

	/* If the caller wants to push a message to the client then
	the caller must pass a valid session handle. */

	ut_a(thd != 0);

	/* The error code must exist in the errmsg-utf8.txt file. */
	ut_a(format != 0);

	va_start(args, code);

	myf l;

	switch (level) {
	case IB_LOG_LEVEL_INFO:
		l = ME_NOTE;
		break;
	case IB_LOG_LEVEL_WARN:
		l = ME_WARNING;
		break;
	default:
		l = 0;
		break;
	}

	my_printv_error(code, format, MYF(l), args);

	va_end(args);

	if (level == IB_LOG_LEVEL_FATAL) {
		ut_error;
	}
}

/******************************************************************//**
Use this when the args are first converted to a formatted string and then
passed to the format string from errmsg-utf8.txt. The error message format
must be: "Some string ... %s".

Push a warning message to the client, it is a wrapper around:

void push_warning_printf(
	THD *thd, Sql_condition::enum_condition_level level,
	uint code, const char *format, ...);
*/
void
ib_errf(
/*====*/
	THD*		thd,		/*!< in/out: session */
	ib_log_level_t	level,		/*!< in: warning level */
	ib_uint32_t	code,		/*!< MySQL error code */
	const char*	format,		/*!< printf format */
	...)				/*!< Args */
{
	char*		str = NULL;
	va_list         args;

	/* If the caller wants to push a message to the client then
	the caller must pass a valid session handle. */

	ut_a(thd != 0);
	ut_a(format != 0);

	va_start(args, format);

#ifdef _WIN32
	int		size = _vscprintf(format, args) + 1;
	if (size > 0) {
		str = static_cast<char*>(malloc(size));
	}
	if (str == NULL) {
		va_end(args);
		return;	/* Watch for Out-Of-Memory */
	}
	str[size - 1] = 0x0;
	vsnprintf(str, size, format, args);
#elif HAVE_VASPRINTF
	if (vasprintf(&str, format, args) == -1) {
		/* In case of failure use a fixed length string */
		str = static_cast<char*>(malloc(BUFSIZ));
		vsnprintf(str, BUFSIZ, format, args);
	}
#else
	/* Use a fixed length string. */
	str = static_cast<char*>(malloc(BUFSIZ));
	if (str == NULL) {
		va_end(args);
		return;	/* Watch for Out-Of-Memory */
	}
	vsnprintf(str, BUFSIZ, format, args);
#endif /* _WIN32 */

	ib_senderrf(thd, level, code, str);

	va_end(args);
	free(str);
}

/* Keep the first 16 characters as-is, since the url is sometimes used
as an offset from this.*/
const char*	TROUBLESHOOTING_MSG =
	"Please refer to https://mariadb.com/kb/en/innodb-troubleshooting/"
	" for how to resolve the issue.";

const char*	TROUBLESHOOT_DATADICT_MSG =
	"Please refer to https://mariadb.com/kb/en/innodb-data-dictionary-troubleshooting/"
	" for how to resolve the issue.";

const char*	BUG_REPORT_MSG =
	"Submit a detailed bug report to https://jira.mariadb.org/";

const char*	FORCE_RECOVERY_MSG =
	"Please refer to "
	"https://mariadb.com/kb/en/library/innodb-recovery-modes/"
	" for information about forcing recovery.";

const char*	OPERATING_SYSTEM_ERROR_MSG =
	"Some operating system error numbers are described at"
	" https://mariadb.com/kb/en/library/operating-system-error-codes/";

const char*	FOREIGN_KEY_CONSTRAINTS_MSG =
	"Please refer to https://mariadb.com/kb/en/library/foreign-keys/"
	" for correct foreign key definition.";

const char*	SET_TRANSACTION_MSG =
	"Please refer to https://mariadb.com/kb/en/library/set-transaction/";

const char*	INNODB_PARAMETERS_MSG =
	"Please refer to https://mariadb.com/kb/en/library/innodb-system-variables/";

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset.
@return result string length, as returned by strconvert() */
uint
innobase_convert_to_filename_charset(
/*=================================*/
	char*		to,	/* out: converted identifier */
	const char*	from,	/* in: identifier to convert */
	ulint		len)	/* in: length of 'to', in bytes */
{
	uint		errors;
	CHARSET_INFO*	cs_to = &my_charset_filename;
	CHARSET_INFO*	cs_from = system_charset_info;

	return(static_cast<uint>(strconvert(
				cs_from, from, uint(strlen(from)),
				cs_to, to, static_cast<uint>(len), &errors)));
}

/**********************************************************************
Converts an identifier from my_charset_filename to UTF-8 charset.
@return result string length, as returned by strconvert() */
uint
innobase_convert_to_system_charset(
/*===============================*/
	char*		to,	/* out: converted identifier */
	const char*	from,	/* in: identifier to convert */
	ulint		len,	/* in: length of 'to', in bytes */
	uint*		errors)	/* out: error return */
{
	CHARSET_INFO*	cs1 = &my_charset_filename;
	CHARSET_INFO*	cs2 = system_charset_info;

	return(static_cast<uint>(strconvert(
				cs1, from, static_cast<uint>(strlen(from)),
				cs2, to, static_cast<uint>(len), errors)));
}

/** Validate the requested buffer pool size.  Also, reserve the necessary
memory needed for buffer pool resize.
@param[in]	thd	thread handle
@param[out]	save	immediate result for update function
@param[in]	value	incoming string
@return 0 on success, 1 on failure.
*/
static
int
innodb_buffer_pool_size_validate(
	THD*				thd,
	st_mysql_sys_var*,
	void*				save,
	struct st_mysql_value*		value)
{
	longlong	intbuf;
	value->val_int(value, &intbuf);

	if (!srv_was_started) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "Cannot update innodb_buffer_pool_size,"
				    " because InnoDB is not started.");
		return(1);
	}

#ifdef UNIV_DEBUG
	if (buf_disable_resize_buffer_pool_debug == TRUE) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
			ER_WRONG_ARGUMENTS,
			"Cannot update innodb_buffer_pool_size,"
			" because innodb_disable_resize_buffer_pool_debug"
			" is set.");
		ib::warn() << "Cannot update innodb_buffer_pool_size,"
			" because innodb_disable_resize_buffer_pool_debug"
			" is set.";
		return(1);
	}
#endif /* UNIV_DEBUG */


	mysql_mutex_lock(&buf_pool.mutex);

	if (srv_buf_pool_old_size != srv_buf_pool_size) {
		mysql_mutex_unlock(&buf_pool.mutex);
		my_printf_error(ER_WRONG_ARGUMENTS,
			"Another buffer pool resize is already in progress.", MYF(0));
		return(1);
	}

	ulint	requested_buf_pool_size = buf_pool_size_align(ulint(intbuf));

	*static_cast<ulonglong*>(save) = requested_buf_pool_size;

	if (srv_buf_pool_size == ulint(intbuf)) {
		mysql_mutex_unlock(&buf_pool.mutex);
		/* nothing to do */
		return(0);
	}

	if (srv_buf_pool_size == requested_buf_pool_size) {
		mysql_mutex_unlock(&buf_pool.mutex);
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_WRONG_ARGUMENTS,
				    "innodb_buffer_pool_size must be at least"
				    " innodb_buffer_pool_chunk_size=%lu",
				    srv_buf_pool_chunk_unit);
		/* nothing to do */
		return(0);
	}

	srv_buf_pool_size = requested_buf_pool_size;
	mysql_mutex_unlock(&buf_pool.mutex);

	if (intbuf != static_cast<longlong>(requested_buf_pool_size)) {
		char	buf[64];
		int	len = 64;
		value->val_str(value, buf, &len);
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    ER_TRUNCATED_WRONG_VALUE,
				    "Truncated incorrect %-.32s value: '%-.128s'",
				    mysql_sysvar_buffer_pool_size.name,
				    value->val_str(value, buf, &len));
	}

	return(0);
}

/*************************************************************//**
Check for a valid value of innobase_compression_algorithm.
@return	0 for valid innodb_compression_algorithm. */
static
int
innodb_compression_algorithm_validate(
/*==================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	ulong		compression_algorithm;
	DBUG_ENTER("innobase_compression_algorithm_validate");

	if (check_sysvar_enum(thd, var, save, value)) {
		DBUG_RETURN(1);
	}

	compression_algorithm = *reinterpret_cast<ulong*>(save);
	(void)compression_algorithm;

#ifndef HAVE_LZ4
	if (compression_algorithm == PAGE_LZ4_ALGORITHM) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				    "InnoDB: liblz4 is not installed. \n",
				    compression_algorithm);
		DBUG_RETURN(1);
	}
#endif

#ifndef HAVE_LZO
	if (compression_algorithm == PAGE_LZO_ALGORITHM) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				    "InnoDB: liblzo is not installed. \n",
				    compression_algorithm);
		DBUG_RETURN(1);
	}
#endif

#ifndef HAVE_LZMA
	if (compression_algorithm == PAGE_LZMA_ALGORITHM) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				    "InnoDB: liblzma is not installed. \n",
				    compression_algorithm);
		DBUG_RETURN(1);
	}
#endif

#ifndef HAVE_BZIP2
	if (compression_algorithm == PAGE_BZIP2_ALGORITHM) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				    "InnoDB: libbz2 is not installed. \n",
				    compression_algorithm);
		DBUG_RETURN(1);
	}
#endif

#ifndef HAVE_SNAPPY
	if (compression_algorithm == PAGE_SNAPPY_ALGORITHM) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: innodb_compression_algorithm = %lu unsupported.\n"
				    "InnoDB: libsnappy is not installed. \n",
				    compression_algorithm);
		DBUG_RETURN(1);
	}
#endif
	DBUG_RETURN(0);
}

static
int
innodb_encrypt_tables_validate(
/*=================================*/
	THD*				thd,	/*!< in: thread handle */
	struct st_mysql_sys_var*	var,	/*!< in: pointer to system
						variable */
	void*				save,	/*!< out: immediate result
						for update function */
	struct st_mysql_value*		value)	/*!< in: incoming string */
{
	if (check_sysvar_enum(thd, var, save, value)) {
		return 1;
	}

	ulong encrypt_tables = *(ulong*)save;

	if (encrypt_tables
	    && !encryption_key_id_exists(FIL_DEFAULT_ENCRYPTION_KEY)) {
		push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				    HA_ERR_UNSUPPORTED,
				    "InnoDB: cannot enable encryption, "
		                    "encryption plugin is not available");
		return 1;
	}

	return 0;
}

static void innodb_remember_check_sysvar_funcs()
{
	/* remember build-in sysvar check functions */
	ut_ad((MYSQL_SYSVAR_NAME(checksum_algorithm).flags & 0x1FF) == PLUGIN_VAR_ENUM);
	check_sysvar_enum = MYSQL_SYSVAR_NAME(checksum_algorithm).check;

	ut_ad((MYSQL_SYSVAR_NAME(flush_log_at_timeout).flags & 15) == PLUGIN_VAR_INT);
	check_sysvar_int = MYSQL_SYSVAR_NAME(flush_log_at_timeout).check;
}

static const size_t MAX_BUF_SIZE = 4 * 1024;

/********************************************************************//**
Helper function to push warnings from InnoDB internals to SQL-layer. */
UNIV_INTERN
void
ib_push_warning(
	trx_t*		trx,	/*!< in: trx */
	dberr_t		error,	/*!< in: error code to push as warning */
	const char	*format,/*!< in: warning message */
	...)
{
	if (trx && trx->mysql_thd) {
		THD *thd = (THD *)trx->mysql_thd;
		va_list args;
		char *buf;

		va_start(args, format);
		buf = (char *)my_malloc(PSI_INSTRUMENT_ME, MAX_BUF_SIZE, MYF(MY_WME));
		vsprintf(buf,format, args);

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			uint(convert_error_code_to_mysql(error, 0, thd)), buf);
		my_free(buf);
		va_end(args);
	}
}

/********************************************************************//**
Helper function to push warnings from InnoDB internals to SQL-layer. */
UNIV_INTERN
void
ib_push_warning(
	void*		ithd,	/*!< in: thd */
	dberr_t		error,	/*!< in: error code to push as warning */
	const char	*format,/*!< in: warning message */
	...)
{
	va_list args;
	THD *thd = (THD *)ithd;
	char *buf;

	if (ithd == NULL) {
		thd = current_thd;
	}

	if (thd) {
		va_start(args, format);
		buf = (char *)my_malloc(PSI_INSTRUMENT_ME, MAX_BUF_SIZE, MYF(MY_WME));
		vsprintf(buf,format, args);

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			uint(convert_error_code_to_mysql(error, 0, thd)), buf);
		my_free(buf);
		va_end(args);
	}
}

/** Helper function to push warnings from InnoDB internals to SQL-layer.
@param[in]	trx
@param[in]	error		Error code to push as warning
@param[in]	table_name	Table name
@param[in]	format		Warning message
@param[in]	...		Message arguments */
UNIV_INTERN
void
ib_foreign_warn(trx_t*	    trx,   /*!< in: trx */
		dberr_t	    error, /*!< in: error code to push as warning */
		const char* table_name,
		const char* format, /*!< in: warning message */
		...)
{
	va_list		    args;
	char*		    buf;
	static FILE*	    ef		 = dict_foreign_err_file;
	static const size_t MAX_BUF_SIZE = 4 * 1024;
	buf = (char*)my_malloc(PSI_INSTRUMENT_ME, MAX_BUF_SIZE, MYF(MY_WME));
	if (!buf) {
		return;
	}

	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);

	mutex_enter(&dict_foreign_err_mutex);
	rewind(ef);
	ut_print_timestamp(ef);
	fprintf(ef, " Error in foreign key constraint of table %s:\n",
		table_name);
	fputs(buf, ef);
	mutex_exit(&dict_foreign_err_mutex);

	if (trx && trx->mysql_thd) {
		THD* thd = (THD*)trx->mysql_thd;

		push_warning_printf(
			thd, Sql_condition::WARN_LEVEL_WARN,
			uint(convert_error_code_to_mysql(error, 0, thd)), buf);
	}

	my_free(buf);
}

/********************************************************************//**
Helper function to push frm mismatch error to error log and
if needed to sql-layer. */
UNIV_INTERN
void
ib_push_frm_error(
/*==============*/
	THD*		thd,		/*!< in: MySQL thd */
	dict_table_t*	ib_table,	/*!< in: InnoDB table */
	TABLE*		table,		/*!< in: MySQL table */
	ulint		n_keys,		/*!< in: InnoDB #keys */
	bool		push_warning)	/*!< in: print warning ? */
{
	switch (ib_table->dict_frm_mismatch) {
	case DICT_FRM_NO_PK:
		sql_print_error("Table %s has a primary key in "
			"InnoDB data dictionary, but not "
			"in MariaDB!"
			" Have you mixed up "
			".frm files from different "
			"installations? See "
			"https://mariadb.com/kb/en/innodb-troubleshooting/\n",
			ib_table->name.m_name);

		if (push_warning) {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_SUCH_INDEX,
				"InnoDB: Table %s has a "
				"primary key in InnoDB data "
				"dictionary, but not in "
				"MariaDB!", ib_table->name.m_name);
		}
		break;
	case DICT_NO_PK_FRM_HAS:
		sql_print_error(
				"Table %s has no primary key in InnoDB data "
				"dictionary, but has one in MariaDB! If you "
				"created the table with a MariaDB version < "
				"3.23.54 and did not define a primary key, "
				"but defined a unique key with all non-NULL "
				"columns, then MariaDB internally treats that "
				"key as the primary key. You can fix this "
				"error by dump + DROP + CREATE + reimport "
				"of the table.", ib_table->name.m_name);

		if (push_warning) {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_SUCH_INDEX,
				"InnoDB: Table %s has no "
				"primary key in InnoDB data "
				"dictionary, but has one in "
				"MariaDB!",
				ib_table->name.m_name);
		}
		break;

	case DICT_FRM_INCONSISTENT_KEYS:
		sql_print_error("InnoDB: Table %s contains " ULINTPF " "
			"indexes inside InnoDB, which "
			"is different from the number of "
			"indexes %u defined in the MariaDB "
			" Have you mixed up "
			".frm files from different "
			"installations? See "
			"https://mariadb.com/kb/en/innodb-troubleshooting/\n",
			ib_table->name.m_name, n_keys,
			table->s->keys);

		if (push_warning) {
			push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
				ER_NO_SUCH_INDEX,
				"InnoDB: Table %s contains " ULINTPF " "
				"indexes inside InnoDB, which "
				"is different from the number of "
				"indexes %u defined in the MariaDB ",
                                ib_table->name.m_name, n_keys,
				table->s->keys);
		}
		break;

	case DICT_FRM_CONSISTENT:
	default:
		sql_print_error("InnoDB: Table %s is consistent "
			"on InnoDB data dictionary and MariaDB "
			" FRM file.",
			ib_table->name.m_name);
		ut_error;
		break;
	}
}

/** Writes 8 bytes to nth tuple field
@param[in]	tuple	where to write
@param[in]	nth	index in tuple
@param[in]	data	what to write
@param[in]	buf	field data buffer */
static void set_tuple_col_8(dtuple_t *tuple, int col, uint64_t data, byte *buf)
{
  dfield_t *dfield= dtuple_get_nth_field(tuple, col);
  ut_ad(dfield->type.len == 8);
  if (dfield->len == UNIV_SQL_NULL)
  {
    dfield_set_data(dfield, buf, 8);
  }
  ut_ad(dfield->len == dfield->type.len && dfield->data);
  mach_write_to_8(dfield->data, data);
}

void ins_node_t::vers_update_end(row_prebuilt_t *prebuilt, bool history_row)
{
  ut_ad(prebuilt->ins_node == this);
  trx_t *trx= prebuilt->trx;
#ifndef DBUG_OFF
  ut_ad(table->vers_start != table->vers_end);
  const mysql_row_templ_t *t= prebuilt->get_template_by_col(table->vers_end);
  ut_ad(t);
  ut_ad(t->mysql_col_len == 8);
#endif

  if (history_row)
  {
    set_tuple_col_8(row, table->vers_end, trx->id, vers_end_buf);
  }
  else /* ROW_INS_VERSIONED */
  {
    set_tuple_col_8(row, table->vers_end, TRX_ID_MAX, vers_end_buf);
#ifndef DBUG_OFF
    t= prebuilt->get_template_by_col(table->vers_start);
    ut_ad(t);
    ut_ad(t->mysql_col_len == 8);
#endif
    set_tuple_col_8(row, table->vers_start, trx->id, vers_start_buf);
  }
  dict_index_t *clust_index= dict_table_get_first_index(table);
  THD *thd= trx->mysql_thd;
  TABLE *mysql_table= prebuilt->m_mysql_table;
  mem_heap_t *local_heap= NULL;
  for (ulint col_no= 0; col_no < dict_table_get_n_v_cols(table); col_no++)
  {

    const dict_v_col_t *v_col= dict_table_get_nth_v_col(table, col_no);
    for (ulint i= 0; i < unsigned(v_col->num_base); i++)
    {
      dict_col_t *base_col= v_col->base_col[i];
      if (base_col->ind == table->vers_end)
      {
        innobase_get_computed_value(row, v_col, clust_index, &local_heap,
                                    table->heap, NULL, thd, mysql_table,
                                    mysql_table->record[0], NULL, NULL, NULL);
      }
    }
  }
  if (local_heap)
  {
    mem_heap_free(local_heap);
  }
}
