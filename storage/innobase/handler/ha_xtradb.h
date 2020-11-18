/*****************************************************************************

Copyright (c) 2000, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.
Copyright (c) 2009, Percona Inc.

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

/** @file ha_xtradb.h */

#ifndef HA_XTRADB_H
#define HA_XTRADB_H

static
void
innodb_print_deprecation(const char* param);

/* XtraDB compatibility system variables. Note that default value and
minimum value can be different compared to original to detect has user
really used the parameter or not. */

static my_bool innodb_buffer_pool_populate;
#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
static ulong srv_cleaner_max_lru_time;
static ulong srv_cleaner_max_flush_time;
static ulong srv_cleaner_flush_chunk_size;
static ulong srv_cleaner_lru_chunk_size;
static ulong srv_cleaner_free_list_lwm;
static my_bool srv_cleaner_eviction_factor;
#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */
static ulong srv_pass_corrupt_table;
static ulong srv_empty_free_list_algorithm;
static ulong innobase_file_io_threads;
static ulong srv_foreground_preflush;
static longlong srv_kill_idle_transaction;
static my_bool srv_fake_changes_locks;
static my_bool	innobase_log_archive;
static char*	innobase_log_arch_dir			= NULL;
static ulong srv_log_arch_expire_sec;
static ulong innobase_log_block_size;
static ulong srv_log_checksum_algorithm;
static ulonglong srv_max_bitmap_file_size;
static ulonglong srv_max_changed_pages;
static ulong innobase_mirrored_log_groups;
#ifdef UNIV_LINUX
static ulong srv_sched_priority_cleaner;
#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
static my_bool srv_cleaner_thread_priority;
static my_bool srv_io_thread_priority;
static my_bool srv_master_thread_priority;
static my_bool srv_purge_thread_priority;
static ulong srv_sched_priority_io;
static ulong srv_sched_priority_master;
static ulong srv_sched_priority_purge;
#endif /* UNIV_DEBUG || UNIV_PERF_DEBUG */
#endif /* UNIV_LINUX */
static ulong srv_cleaner_lsn_age_factor;
static ulong srv_show_locks_held;
static ulong srv_show_verbose_locks;
static my_bool srv_track_changed_pages;
static my_bool innodb_track_redo_log_now;
static my_bool srv_use_global_flush_log_at_trx_commit;
static my_bool srv_use_stacktrace;


static const char innodb_deprecated_msg[]= "Using %s is deprecated and the"
		" parameter may be removed in future releases."
		" Ignoning the parameter.";


#ifdef BTR_CUR_HASH_ADAPT
/* it is just alias for innodb_adaptive_hash_index_parts */
/** Number of distinct partitions of AHI.
Each partition is protected by its own latch and so we have parts number
of latches protecting complete search system. */
static MYSQL_SYSVAR_ULONG(adaptive_hash_index_partitions, btr_ahi_parts,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "It is an alias for innodb_adaptive_hash_index_parts; "
  "only exists to allow easier upgrade from earlier XtraDB versions.",
  NULL, NULL, 8, 1, 512, 0);
#endif /* BTR_CUR_HASH_ADAPT */

static MYSQL_SYSVAR_BOOL(buffer_pool_populate, innodb_buffer_pool_populate,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, NULL, FALSE);

#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
static
void
set_cleaner_max_lru_time(THD*thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_cleaner_max_lru_time");
}
/* Original default 1000 */
static MYSQL_SYSVAR_ULONG(cleaner_max_lru_time, srv_cleaner_max_lru_time,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, set_cleaner_max_lru_time, 0, 0, ~0UL, 0);

static
void
set_cleaner_max_flush_time(THD*thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_cleaner_max_flush_time");
}
/* Original default 1000 */
static MYSQL_SYSVAR_ULONG(cleaner_max_flush_time, srv_cleaner_max_flush_time,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_cleaner_max_flush_time, 0, 0, ~0UL, 0);

static
void
set_cleaner_flush_chunk_size(THD*thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_cleaner_flush_chunk_size");
}
/* Original default 100 */
static MYSQL_SYSVAR_ULONG(cleaner_flush_chunk_size,
  srv_cleaner_flush_chunk_size,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_cleaner_flush_chunk_size, 0, 0, ~0UL, 0);

static
void
set_cleaner_lru_chunk_size(THD*thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_cleaner_lru_chunk_size");
}
/* Original default 100 */
static MYSQL_SYSVAR_ULONG(cleaner_lru_chunk_size,
  srv_cleaner_lru_chunk_size,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_cleaner_lru_chunk_size, 0, 0, ~0UL, 0);

static
void
set_cleaner_free_list_lwm(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_cleaner_free_list_lwm");
}
/* Original default 10 */
static MYSQL_SYSVAR_ULONG(cleaner_free_list_lwm, srv_cleaner_free_list_lwm,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_cleaner_free_list_lwm, 0, 0, 100, 0);

static
void
set_cleaner_eviction_factor(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_cleaner_eviction_factor");
}
static MYSQL_SYSVAR_BOOL(cleaner_eviction_factor, srv_cleaner_eviction_factor,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_cleaner_eviction_factor, FALSE);

#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */

/* Added new default DEPRECATED */
/** Possible values for system variable "innodb_cleaner_lsn_age_factor".  */
static const char* innodb_cleaner_lsn_age_factor_names[] = {
	"LEGACY",
	"HIGH_CHECKPOINT",
	"DEPRECATED",
	NullS
};

/** Enumeration for innodb_cleaner_lsn_age_factor.  */
static TYPELIB innodb_cleaner_lsn_age_factor_typelib = {
	array_elements(innodb_cleaner_lsn_age_factor_names) - 1,
	"innodb_cleaner_lsn_age_factor_typelib",
	innodb_cleaner_lsn_age_factor_names,
	NULL
};

/** Alternatives for srv_cleaner_lsn_age_factor, set through
innodb_cleaner_lsn_age_factor variable  */
enum srv_cleaner_lsn_age_factor_t {
	SRV_CLEANER_LSN_AGE_FACTOR_LEGACY,	/*!< Original Oracle MySQL 5.6
						formula */
	SRV_CLEANER_LSN_AGE_FACTOR_HIGH_CHECKPOINT,
						/*!< Percona Server 5.6 formula
						that returns lower values than
					        legacy option for low
					        checkpoint ages, and higher
					        values for high ages.  This has
					        the effect of stabilizing the
						checkpoint age higher.  */
	SRV_CLEANER_LSN_AGE_FACTOR_DEPRECATED	/*!< Deprecated, do not use */
};

/** Alternatives for srv_foreground_preflush, set through
innodb_foreground_preflush variable  */
enum srv_foreground_preflush_t {
	SRV_FOREGROUND_PREFLUSH_SYNC_PREFLUSH,	/*!< Original Oracle MySQL 5.6
						behavior of performing a sync
						flush list flush  */
	SRV_FOREGROUND_PREFLUSH_EXP_BACKOFF,	/*!< Exponential backoff wait
						for the page cleaner to flush
						for us  */
	SRV_FOREGROUND_PREFLUSH_DEPRECATED	/*!< Deprecated, do not use */
};

/** Alternatives for srv_empty_free_list_algorithm, set through
innodb_empty_free_list_algorithm variable  */
enum srv_empty_free_list_t {
	SRV_EMPTY_FREE_LIST_LEGACY,	/*!< Original Oracle MySQL 5.6
				        algorithm */
	SRV_EMPTY_FREE_LIST_BACKOFF,	/*!< Percona Server 5.6 algorithm that
					loops in a progressive backoff until a
					free page is produced by the cleaner
					thread */
	SRV_EMPTY_FREE_LIST_DEPRECATED	/*!< Deprecated, do not use */
};

#define SRV_CHECKSUM_ALGORITHM_DEPRECATED 6

static
void
set_cleaner_lsn_age_factor(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_cleaner_lsn_age_factor");
}
static MYSQL_SYSVAR_ENUM(cleaner_lsn_age_factor,
  srv_cleaner_lsn_age_factor,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_cleaner_lsn_age_factor, SRV_CLEANER_LSN_AGE_FACTOR_DEPRECATED,
  &innodb_cleaner_lsn_age_factor_typelib);

/* Added new default drepcated, 3 */
const char *corrupt_table_action_names[]=
{
  "assert", /* 0 */
  "warn", /* 1 */
  "salvage", /* 2 */
  "deprecated", /* 3 */
  NullS
};

TYPELIB corrupt_table_action_typelib=
{
  array_elements(corrupt_table_action_names) - 1, "corrupt_table_action_typelib",
  corrupt_table_action_names, NULL
};

static
void
set_corrupt_table_action(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_corrupt_table_action");
}
static	MYSQL_SYSVAR_ENUM(corrupt_table_action, srv_pass_corrupt_table,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_corrupt_table_action, 3, &corrupt_table_action_typelib);

/* Added new default DEPRECATED */
/** Possible values for system variable "innodb_empty_free_list_algorithm".  */
static const char* innodb_empty_free_list_algorithm_names[] = {
	"LEGACY",
	"BACKOFF",
	"DEPRECATED",
	NullS
};

/** Enumeration for innodb_empty_free_list_algorithm.  */
static TYPELIB innodb_empty_free_list_algorithm_typelib = {
	array_elements(innodb_empty_free_list_algorithm_names) - 1,
	"innodb_empty_free_list_algorithm_typelib",
	innodb_empty_free_list_algorithm_names,
	NULL
};

static
void
set_empty_free_list_algorithm(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_empty_free_list_algorithm");
}
static MYSQL_SYSVAR_ENUM(empty_free_list_algorithm,
  srv_empty_free_list_algorithm,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_empty_free_list_algorithm, SRV_EMPTY_FREE_LIST_DEPRECATED,
  &innodb_empty_free_list_algorithm_typelib);

static
void
set_fake_changes(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_fake_changes");
}
static MYSQL_THDVAR_BOOL(fake_changes, PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_fake_changes, FALSE);

/* Original default, min 4. */
static MYSQL_SYSVAR_ULONG(file_io_threads, innobase_file_io_threads,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, NULL, 0, 0, 64, 0);

/** Possible values for system variable "innodb_foreground_preflush".  */
static const char* innodb_foreground_preflush_names[] = {
	"SYNC_PREFLUSH",
	"EXPONENTIAL_BACKOFF",
	"DEPRECATED",
	NullS
};

/* Enumeration for innodb_foreground_preflush.  */
static TYPELIB innodb_foreground_preflush_typelib = {
	array_elements(innodb_foreground_preflush_names) - 1,
	"innodb_foreground_preflush_typelib",
	innodb_foreground_preflush_names,
	NULL
};

static
void
set_foreground_preflush(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_foreground_preflush");
}
static MYSQL_SYSVAR_ENUM(foreground_preflush, srv_foreground_preflush,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_foreground_preflush, SRV_FOREGROUND_PREFLUSH_DEPRECATED,
  &innodb_foreground_preflush_typelib);

#ifdef EXTENDED_FOR_KILLIDLE
#define kill_idle_help_text "If non-zero value, the idle session with transaction which is idle over the value in seconds is killed by InnoDB."
#else
#define kill_idle_help_text "No effect for this build."
#endif
static
void
set_kill_idle_transaction(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_kill_idle_transaction");
}
static MYSQL_SYSVAR_LONGLONG(kill_idle_transaction, srv_kill_idle_transaction,
  PLUGIN_VAR_RQCMDARG, kill_idle_help_text,
  NULL, &set_kill_idle_transaction, 0, 0, LONG_MAX, 0);

static
void
set_locking_fake_changes(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_locking_fake_changes");
}
/* Original default: TRUE */
static MYSQL_SYSVAR_BOOL(locking_fake_changes, srv_fake_changes_locks,
  PLUGIN_VAR_NOCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_locking_fake_changes, FALSE);

static MYSQL_SYSVAR_STR(log_arch_dir, innobase_log_arch_dir,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, NULL, NULL);

static
void
set_log_archive(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_log_archive");
}
static MYSQL_SYSVAR_BOOL(log_archive, innobase_log_archive,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_log_archive, FALSE);

static
void
set_log_arch_expire_sec(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_log_arch_expire_sec");
}
static MYSQL_SYSVAR_ULONG(log_arch_expire_sec,
  srv_log_arch_expire_sec, PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_log_arch_expire_sec, 0, 0, ~0UL, 0);

/* Original default, min 512 */
static MYSQL_SYSVAR_ULONG(log_block_size, innobase_log_block_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, NULL, 0, 0,
  (1 << UNIV_PAGE_SIZE_SHIFT_MAX), 0);

/* Added new default deprecated */
/** Possible values for system variables "innodb_checksum_algorithm" and
"innodb_log_checksum_algorithm". */
static const char* innodb_checksum_algorithm_names2[] = {
	"CRC32",
	"STRICT_CRC32",
	"INNODB",
	"STRICT_INNODB",
	"NONE",
	"STRICT_NONE",
	"DEPRECATED",
	NullS
};

/** Used to define an enumerate type of the system variables
innodb_checksum_algorithm and innodb_log_checksum_algorithm. */
static TYPELIB innodb_checksum_algorithm_typelib2 = {
	array_elements(innodb_checksum_algorithm_names2) - 1,
	"innodb_checksum_algorithm_typelib2",
	innodb_checksum_algorithm_names2,
	NULL
};
static
void
set_log_checksum_algorithm(THD* thd, st_mysql_sys_var*, void*, const void* save)
{
	push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_log_checksum_algorithm");
}
static MYSQL_SYSVAR_ENUM(log_checksum_algorithm, srv_log_checksum_algorithm,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and translated to innodb_log_checksums (NONE to OFF, "
  "everything else to ON); only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_log_checksum_algorithm, SRV_CHECKSUM_ALGORITHM_DEPRECATED,
  &innodb_checksum_algorithm_typelib2);

static
void
set_max_bitmap_file_size(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_max_bitmap_file_size");
}
/* Original default 100M, min 4K */
static MYSQL_SYSVAR_ULONGLONG(max_bitmap_file_size, srv_max_bitmap_file_size,
    PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
    NULL, &set_max_bitmap_file_size, 0, 0, ULONGLONG_MAX, 0);

static
void
set_max_changed_pages(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_max_changed_pages");
}
/* Original default 1000000 */
static MYSQL_SYSVAR_ULONGLONG(max_changed_pages, srv_max_changed_pages,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_max_changed_pages, 0, 0, ~0ULL, 0);

/* Note that the default and minimum values are set to 0 to
detect if the option is passed and print deprecation message */
static MYSQL_SYSVAR_ULONG(mirrored_log_groups, innobase_mirrored_log_groups,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, NULL, 0, 0, 10, 0);

#ifdef UNIV_LINUX

static
void
set_sched_priority_cleaner(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_sched_priority_cleaner");
}
/* Original default 19 */
static MYSQL_SYSVAR_ULONG(sched_priority_cleaner, srv_sched_priority_cleaner,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_sched_priority_cleaner, 0, 0, 39, 0);

#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
static
void
set_priority_cleaner(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_priority_cleaner");
}
static MYSQL_SYSVAR_BOOL(priority_cleaner, srv_cleaner_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_priority_cleaner, FALSE);

static
void
set_priority_io(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_priority_io");
}
static MYSQL_SYSVAR_BOOL(priority_io, srv_io_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
   NULL, &set_priority_io, FALSE);

static
void
set_priority_master(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_priority_master");
}
static MYSQL_SYSVAR_BOOL(priority_master, srv_master_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
   NULL, &set_priority_master, FALSE);

static
void
set_priority_purge(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_priority_purge");
}
static MYSQL_SYSVAR_BOOL(priority_purge, srv_purge_thread_priority,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_priority_purge, FALSE);

static
void
set_sched_priority_io(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_sched_priority_io");
}
/* Original default 19 */
static MYSQL_SYSVAR_ULONG(sched_priority_io, srv_sched_priority_io,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_sched_priority_io, 0, 0, 39, 0);

static
void
set_sched_priority_master(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_sched_priority_master");
}
/* Original default 19 */
static MYSQL_SYSVAR_ULONG(sched_priority_master, srv_sched_priority_master,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_sched_priority_master, 0, 0, 39, 0);

static
void
set_sched_priority_purge(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_sched_priority_purge");
}
/* Original default 19 */
static MYSQL_SYSVAR_ULONG(sched_priority_purge, srv_sched_priority_purge,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_sched_priority_purge, 0, 0, 39, 0);
#endif /* UNIV_DEBUG || UNIV_PERF_DEBUG */
#endif /* UNIV_LINUX */

static
void
set_show_locks_held(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_show_locks_held");
}
/* TODO: Implement */
static MYSQL_SYSVAR_ULONG(show_locks_held, srv_show_locks_held,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_show_locks_held, 0, 0, 1000, 0);

static
void
set_show_verbose_locks(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_show_verbose_locks");
}
/* TODO: Implement */
static MYSQL_SYSVAR_ULONG(show_verbose_locks, srv_show_verbose_locks,
  PLUGIN_VAR_RQCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_show_verbose_locks, 0, 0, 1, 0);

static MYSQL_SYSVAR_BOOL(track_changed_pages, srv_track_changed_pages,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, NULL, FALSE);

static
void
set_track_redo_log_now(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_track_redo_log_now");
}
static MYSQL_SYSVAR_BOOL(track_redo_log_now,
  innodb_track_redo_log_now,
  PLUGIN_VAR_OPCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_track_redo_log_now, FALSE);

static
void
set_use_global_flush_log_at_trx_commit(THD* thd, st_mysql_sys_var*, void*, const void*)
{
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARN_DEPRECATED_SYNTAX,
                            innodb_deprecated_msg,
                            "innodb_use_global_flush_log_at_trx_commit");
}
static MYSQL_SYSVAR_BOOL(use_global_flush_log_at_trx_commit, srv_use_global_flush_log_at_trx_commit,
  PLUGIN_VAR_NOCMDARG,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, &set_use_global_flush_log_at_trx_commit, FALSE);

static MYSQL_SYSVAR_BOOL(use_stacktrace, srv_use_stacktrace,
  PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
  "Deprecated and ignored; only exists to allow easier upgrade from "
  "earlier XtraDB versions.",
  NULL, NULL, FALSE);

/** Print deprecation message for a given system variable.
@param[in]	param		System parameter name */
static
void
innodb_print_deprecation(const char* param)
{
	ib::warn() << "Using " << param << " is deprecated and the"
		" parameter may be removed in future releases."
		" Ignoning the parameter.";
}

/** Check if user has used xtradb extended system variable that
is not currently supported by innodb or marked as deprecated. */
static
void
innodb_check_deprecated(void)
{
	if (innodb_buffer_pool_populate) {
		innodb_print_deprecation("innodb-buffer-pool-populate");
	}

#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
        if (srv_cleaner_max_lru_time) {
		innodb_print_deprecation("innodb-cleaner-max-lru-time");
	}

        if (srv_cleaner_max_flush_time) {
		innodb_print_deprecation("innodb-cleaner-max-flush-time");
	}

        if (srv_cleaner_flush_chunk_size) {
		innodb_print_deprecation("innodb-cleaner-flush-chunk-size");
	}

        if (srv_cleaner_lru_chunk_size) {
		innodb_print_deprecation("innodb-cleaner-lru_chunk_size");
	}
        if (srv_cleaner_free_list_lwm) {
		innodb_print_deprecation("innodb-cleaner-free-list-lwm");
	}

        if (srv_cleaner_eviction_factor) {
		innodb_print_deprecation("innodb-cleaner-eviction-factor");
	}

#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */

	if (srv_cleaner_lsn_age_factor != SRV_CLEANER_LSN_AGE_FACTOR_DEPRECATED) {
		innodb_print_deprecation("innodb-cleaner-lsn-age-factor");
	}

	if (srv_pass_corrupt_table != 3) {
		innodb_print_deprecation("innodb-pass-corrupt-table");
	}

	if (srv_empty_free_list_algorithm != SRV_EMPTY_FREE_LIST_DEPRECATED) {
		innodb_print_deprecation("innodb-empty-free-list-algorithm");
	}

	if (THDVAR((THD*) NULL, fake_changes)) {
		innodb_print_deprecation("innodb-fake-changes");
	}

	if (innobase_file_io_threads) {
		innodb_print_deprecation("innodb-file-io-threads");
	}

	if (srv_foreground_preflush != SRV_FOREGROUND_PREFLUSH_DEPRECATED) {
		innodb_print_deprecation("innodb-foreground-preflush");
	}

	if (srv_kill_idle_transaction != 0) {
		innodb_print_deprecation("innodb-kill-idle-transaction");
	}

	if (srv_fake_changes_locks) {
		innodb_print_deprecation("innodb-fake-changes-locks");
	}

	if (innobase_log_arch_dir) {
		innodb_print_deprecation("innodb-log-arch-dir");
	}

	if (innobase_log_archive) {
		innodb_print_deprecation("innodb-log-archive");
	}

	if (srv_log_arch_expire_sec) {
		innodb_print_deprecation("innodb-log-arch-expire-sec");
	}

	if (innobase_log_block_size) {
		innodb_print_deprecation("innodb-log-block-size");
	}

	if (srv_log_checksum_algorithm != SRV_CHECKSUM_ALGORITHM_DEPRECATED) {
		innodb_print_deprecation("innodb-log-checksum-algorithm");
	}

	if (srv_max_changed_pages) {
		innodb_print_deprecation("innodb-max-changed-pages");
	}

	if (innobase_mirrored_log_groups) {
		innodb_print_deprecation("innodb-mirrored-log-groups");
	}

#ifdef UNIV_LINUX
	if (srv_sched_priority_cleaner) {
		innodb_print_deprecation("innodb-sched-priority-cleaner");
	}

#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
	if (srv_cleaner_thread_priority) {
		innodb_print_deprecation("innodb-cleaner-thread-priority");
	}

	if (srv_io_thread_priority) {
		innodb_print_deprecation("innodb-io-thread-priority");
	}

	if (srv_master_thread_priority) {
		innodb_print_deprecation("inodb-master-thread-priority");
	}

	if (srv_purge_thread_priority) {
		innodb_print_deprecation("inodb-purge-thread-priority");
	}

	if (srv_sched_priority_io) {
		innodb_print_deprecation("innodb-sched-priority-io");
	}

	if (srv_sched_priority_master) {
		innodb_print_deprecation("innodb-sched-priority-master");
	}

	if (srv_sched_priority_purge) {
		innodb_print_deprecation("innodb-sched-priority-purge");
	}
#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */
#endif /* UNIV_LINUX */

	if (srv_track_changed_pages) {
		innodb_print_deprecation("innodb-track-changed-pages");
	}

	if (innodb_track_redo_log_now) {
		innodb_print_deprecation("innodb-track-redo-log-now");
	}

	if (srv_use_global_flush_log_at_trx_commit) {
		innodb_print_deprecation("innodb-use-global-flush-log-at-trx-commit");
	}

	if (srv_use_stacktrace) {
		innodb_print_deprecation("innodb-use-stacktrace");
	}

        if (srv_max_bitmap_file_size) {
		innodb_print_deprecation("innodb-max-bitmap-file-size");
	}

        if (srv_show_locks_held) {
		innodb_print_deprecation("innodb-show-locks-held");
	}

        if (srv_show_verbose_locks) {
		innodb_print_deprecation("innodb-show-verbose-locks");
	}
}

#endif /* HA_XTRADB_H */

#ifdef HA_XTRADB_SYSVARS
  /* XtraDB compatibility system variables */
#ifdef BTR_CUR_HASH_ADAPT
  MYSQL_SYSVAR(adaptive_hash_index_partitions),
#endif /* BTR_CUR_HASH_ADAPT */
  MYSQL_SYSVAR(buffer_pool_populate),
#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
  MYSQL_SYSVAR(cleaner_eviction_factor),
  MYSQL_SYSVAR(cleaner_flush_chunk_size),
  MYSQL_SYSVAR(cleaner_free_list_lwm),
  MYSQL_SYSVAR(cleaner_lru_chunk_size),
  MYSQL_SYSVAR(cleaner_max_lru_time),
  MYSQL_SYSVAR(cleaner_max_flush_time),
#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */
  MYSQL_SYSVAR(cleaner_lsn_age_factor),
  MYSQL_SYSVAR(corrupt_table_action),
  MYSQL_SYSVAR(empty_free_list_algorithm),
  MYSQL_SYSVAR(fake_changes),
  MYSQL_SYSVAR(file_io_threads),
  MYSQL_SYSVAR(foreground_preflush),
  MYSQL_SYSVAR(kill_idle_transaction),
  MYSQL_SYSVAR(locking_fake_changes),
  MYSQL_SYSVAR(log_arch_dir),
  MYSQL_SYSVAR(log_archive),
  MYSQL_SYSVAR(log_arch_expire_sec),
  MYSQL_SYSVAR(log_block_size),
  MYSQL_SYSVAR(log_checksum_algorithm),
  MYSQL_SYSVAR(max_bitmap_file_size),
  MYSQL_SYSVAR(max_changed_pages),
  MYSQL_SYSVAR(mirrored_log_groups),
#ifdef UNIV_LINUX
  MYSQL_SYSVAR(sched_priority_cleaner),
#endif
#if defined UNIV_DEBUG || defined UNIV_PERF_DEBUG
#ifdef UNIV_LINUX
  MYSQL_SYSVAR(priority_cleaner),
  MYSQL_SYSVAR(priority_io),
  MYSQL_SYSVAR(priority_master),
  MYSQL_SYSVAR(priority_purge),
  MYSQL_SYSVAR(sched_priority_io),
  MYSQL_SYSVAR(sched_priority_master),
  MYSQL_SYSVAR(sched_priority_purge),
#endif /* UNIV_LINUX */
#endif /* defined UNIV_DEBUG || defined UNIV_PERF_DEBUG */
  MYSQL_SYSVAR(show_locks_held),
  MYSQL_SYSVAR(show_verbose_locks),
  MYSQL_SYSVAR(track_changed_pages),
  MYSQL_SYSVAR(track_redo_log_now),
  MYSQL_SYSVAR(use_global_flush_log_at_trx_commit),
  MYSQL_SYSVAR(use_stacktrace),

#endif /* HA_XTRADB_SYSVARS */
