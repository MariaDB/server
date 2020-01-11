/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "hatoku_hton.h"
#include "sql_acl.h"
#include "tokudb_dir_cmd.h"
#include "sql_parse.h"

namespace tokudb {
namespace sysvars {

//******************************************************************************
// global variables
//******************************************************************************
#ifdef TOKUDB_VERSION
#define tokudb_stringify_2(x) #x
#define tokudb_stringify(x) tokudb_stringify_2(x)
#define TOKUDB_VERSION_STR tokudb_stringify(TOKUDB_VERSION)
#else
#define TOKUDB_VERSION_STR NULL
#endif


ulonglong   cache_size = 0;
uint        cachetable_pool_threads = 0;
int         cardinality_scale_percent = 0;
my_bool     checkpoint_on_flush_logs = FALSE;
uint        checkpoint_pool_threads = 0;
uint        checkpointing_period = 0;
ulong       cleaner_iterations = 0;
ulong       cleaner_period = 0;
uint        client_pool_threads = 0;
my_bool     compress_buffers_before_eviction = TRUE;
char*       data_dir = NULL;
ulong       debug = 0;
#if defined(TOKUDB_DEBUG) && TOKUDB_DEBUG
// used to control background job manager
my_bool     debug_pause_background_job_manager = FALSE;
#endif  // defined(TOKUDB_DEBUG) && TOKUDB_DEBUG
my_bool     directio = FALSE;
my_bool     enable_partial_eviction = TRUE;
int         fs_reserve_percent = 0;
uint        fsync_log_period = 0;
char*       log_dir = NULL;
ulonglong   max_lock_memory = 0;
uint        read_status_frequency = 0;
my_bool     strip_frm_data = FALSE;
char*       tmp_dir = NULL;
uint        write_status_frequency = 0;
my_bool     dir_per_db = FALSE;
char*       version = (char*) TOKUDB_VERSION_STR;

// file system reserve as a percentage of total disk space
#if defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) && \
    TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
char*       gdb_path = NULL;
my_bool     gdb_on_fatal = FALSE;
#endif  // defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) &&
        // TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL

my_bool        check_jemalloc = TRUE;

static MYSQL_SYSVAR_ULONGLONG(
    cache_size,
    cache_size,
    PLUGIN_VAR_READONLY,
    "cache table size",
    NULL,
    NULL,
    0,
    0,
    ~0ULL,
    0);

static MYSQL_SYSVAR_UINT(
    cachetable_pool_threads,
    cachetable_pool_threads,
    PLUGIN_VAR_READONLY,
    "cachetable ops thread pool size",
    NULL,
    NULL,
    0,
    0,
    1024,
    0);

static MYSQL_SYSVAR_INT(
    cardinality_scale_percent,
    cardinality_scale_percent,
    0,
    "index cardinality scale percentage",
    NULL,
    NULL,
    50,
    0,
    100,
    0);

static MYSQL_SYSVAR_BOOL(
    checkpoint_on_flush_logs,
    checkpoint_on_flush_logs,
    0,
    "checkpoint on flush logs",
    NULL,
    NULL,
    FALSE);

static MYSQL_SYSVAR_UINT(
    checkpoint_pool_threads,
    checkpoint_pool_threads,
    PLUGIN_VAR_READONLY,
    "checkpoint ops thread pool size",
    NULL,
    NULL,
    0,
    0,
    1024,
    0);

static void checkpointing_period_update(
    TOKUDB_UNUSED(THD* thd),
    TOKUDB_UNUSED(st_mysql_sys_var* sys_var),
    void* var,
    const void* save) {
    uint* cp = (uint*)var;
    *cp = *(const uint*)save;
    int r = db_env->checkpointing_set_period(db_env, *cp);
    assert(r == 0);
}

static MYSQL_SYSVAR_UINT(
    checkpointing_period,
    checkpointing_period,
    0,
    "checkpointing period",
    NULL,
    checkpointing_period_update,
    60,
    0,
    ~0U,
    0);

static void cleaner_iterations_update(TOKUDB_UNUSED(THD* thd),
                                      TOKUDB_UNUSED(st_mysql_sys_var* sys_var),
                                      void* var,
                                      const void* save) {
    ulong* ci = (ulong*)var;
    *ci = *(const ulong*)save;
    int r = db_env->cleaner_set_iterations(db_env, *ci);
    assert(r == 0);
}

static MYSQL_SYSVAR_ULONG(
    cleaner_iterations,
    cleaner_iterations,
    0,
    "cleaner_iterations",
    NULL,
    cleaner_iterations_update,
    DEFAULT_TOKUDB_CLEANER_ITERATIONS,
    0,
    ~0UL,
    0);

static void cleaner_period_update(TOKUDB_UNUSED(THD* thd),
                                  TOKUDB_UNUSED(st_mysql_sys_var* sys_var),
                                  void* var,
                                  const void* save) {
    ulong* cp = (ulong*)var;
    *cp = *(const ulong*)save;
    int r = db_env->cleaner_set_period(db_env, *cp);
    assert(r == 0);
}

static MYSQL_SYSVAR_ULONG(
    cleaner_period,
    cleaner_period,
    0,
    "cleaner_period",
    NULL,
    cleaner_period_update,
    DEFAULT_TOKUDB_CLEANER_PERIOD,
    0,
    ~0UL,
    0);

static MYSQL_SYSVAR_UINT(
    client_pool_threads,
    client_pool_threads,
    PLUGIN_VAR_READONLY,
    "client ops thread pool size",
    NULL,
    NULL,
    0,
    0,
    1024,
    0);

static MYSQL_SYSVAR_BOOL(
    compress_buffers_before_eviction,
    compress_buffers_before_eviction,
    PLUGIN_VAR_READONLY,
    "enable in-memory buffer compression before partial eviction",
    NULL,
    NULL,
    TRUE);

static MYSQL_SYSVAR_STR(
    data_dir,
    data_dir,
    PLUGIN_VAR_READONLY,
    "data directory",
    NULL,
    NULL,
    NULL);

static MYSQL_SYSVAR_ULONG(
    debug,
    debug,
    0,
    "plugin debug mask",
    NULL,
    NULL,
    0,
    0,
    ~0UL,
    0);

#if defined(TOKUDB_DEBUG) && TOKUDB_DEBUG
static MYSQL_SYSVAR_BOOL(
    debug_pause_background_job_manager,
    debug_pause_background_job_manager,
    0,
    "debug : pause the background job manager",
    NULL,
    NULL,
    FALSE);
#endif  // defined(TOKUDB_DEBUG) && TOKUDB_DEBUG

static MYSQL_SYSVAR_BOOL(
    directio,
    directio,
    PLUGIN_VAR_READONLY, "enable direct i/o ",
    NULL,
    NULL,
    FALSE);

static void enable_partial_eviction_update(
    TOKUDB_UNUSED(THD* thd),
    TOKUDB_UNUSED(st_mysql_sys_var* sys_var),
    void* var,
    const void* save) {
    my_bool* epe = (my_bool*)var;
    *epe = *(const my_bool*)save;
    int r = db_env->evictor_set_enable_partial_eviction(db_env, *epe);
    assert(r == 0);
}

static MYSQL_SYSVAR_BOOL(
    enable_partial_eviction,
    enable_partial_eviction,
    0,
    "enable partial node eviction",
    NULL,
    enable_partial_eviction_update,
    TRUE);

static MYSQL_SYSVAR_INT(
    fs_reserve_percent,
    fs_reserve_percent,
    PLUGIN_VAR_READONLY,
    "file system space reserve (percent free required)",
    NULL,
    NULL,
    5,
    0,
    100,
    0);

static void fsync_log_period_update(TOKUDB_UNUSED(THD* thd),
                                    TOKUDB_UNUSED(st_mysql_sys_var* sys_var),
                                    void* var,
                                    const void* save) {
    uint* flp = (uint*)var;
    *flp = *(const uint*)save;
    db_env->change_fsync_log_period(db_env, *flp);
}

static MYSQL_SYSVAR_UINT(
    fsync_log_period,
    fsync_log_period,
    0,
    "fsync log period",
    NULL,
    fsync_log_period_update,
    0,
    0,
    ~0U,
    0);

static MYSQL_SYSVAR_STR(
    log_dir,
    log_dir,
    PLUGIN_VAR_READONLY,
    "log directory",
    NULL,
    NULL,
    NULL);

static MYSQL_SYSVAR_ULONGLONG(
    max_lock_memory,
    max_lock_memory,
    PLUGIN_VAR_READONLY,
    "max memory for locks",
    NULL,
    NULL,
    0,
    0,
    ~0ULL,
    0);

static MYSQL_SYSVAR_UINT(
    read_status_frequency,
    read_status_frequency,
    0,
    "frequency that show processlist updates status of reads",
    NULL,
    NULL,
    10000,
    0,
    ~0U,
    0);

static MYSQL_SYSVAR_BOOL(
    strip_frm_data,
    strip_frm_data,
    PLUGIN_VAR_READONLY,
    "strip .frm data from metadata file(s)",
    NULL,
    NULL,
    FALSE);

static MYSQL_SYSVAR_STR(
    tmp_dir,
    tmp_dir,
    PLUGIN_VAR_READONLY,
    "directory to use for temporary files",
    NULL,
    NULL,
    NULL);

static MYSQL_SYSVAR_STR(
    version,
    version,
    PLUGIN_VAR_READONLY,
    "plugin version",
    NULL,
    NULL,
    NULL);

static MYSQL_SYSVAR_UINT(
    write_status_frequency,
    write_status_frequency,
    0,
    "frequency that show processlist updates status of writes",
    NULL,
    NULL,
    1000,
    0,
    ~0U,
    0);

static void tokudb_dir_per_db_update(
    TOKUDB_UNUSED(THD* thd),
    TOKUDB_UNUSED(struct st_mysql_sys_var* sys_var),
    void* var,
    const void* save) {
    my_bool *value = (my_bool *) var;
    *value = *(const my_bool *) save;
    db_env->set_dir_per_db(db_env, *value);
}

static MYSQL_SYSVAR_BOOL(dir_per_db, dir_per_db,
    0, "TokuDB store ft files in db directories",
    NULL, tokudb_dir_per_db_update, FALSE);

#if defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) && \
    TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
static MYSQL_SYSVAR_STR(
    gdb_path,
    gdb_path,
    PLUGIN_VAR_READONLY|PLUGIN_VAR_RQCMDARG,
    "path to gdb for extra debug info on fatal signal",
    NULL,
    NULL,
    "/usr/bin/gdb");

static MYSQL_SYSVAR_BOOL(
    gdb_on_fatal,
    gdb_on_fatal,
    0,
    "enable gdb debug info on fatal signal",
    NULL,
    NULL,
    true);
#endif  // defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) &&
        // TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL

static MYSQL_SYSVAR_BOOL(
    check_jemalloc,
    check_jemalloc,
    PLUGIN_VAR_READONLY|PLUGIN_VAR_RQCMDARG,
    "check if jemalloc is linked and transparent huge pages are disabled",
    NULL,
    NULL,
    TRUE);


//******************************************************************************
// session variables
//******************************************************************************
static MYSQL_THDVAR_BOOL(
    alter_print_error,
    0,
    "print errors for alter table operations",
    NULL,
    NULL,
    false);

static MYSQL_THDVAR_DOUBLE(
    analyze_delete_fraction,
    0,
    "fraction of rows allowed to be deleted",
    NULL,
    NULL,
    1.0,
    0,
    1.0,
    1);

static MYSQL_THDVAR_BOOL(
    analyze_in_background,
    0,
    "dispatch ANALYZE TABLE to background job.",
    NULL,
    NULL,
    false);

const char* srv_analyze_mode_names[] = {
    "TOKUDB_ANALYZE_STANDARD",
    "TOKUDB_ANALYZE_RECOUNT_ROWS",
    "TOKUDB_ANALYZE_CANCEL",
    NullS
};

static TYPELIB tokudb_analyze_mode_typelib = {
    array_elements(srv_analyze_mode_names) - 1,
    "tokudb_analyze_mode_typelib",
    srv_analyze_mode_names,
    NULL
};

static MYSQL_THDVAR_ENUM(analyze_mode,
    PLUGIN_VAR_RQCMDARG,
    "Controls the function of ANALYZE TABLE. Possible values are: "
    "TOKUDB_ANALYZE_STANDARD perform standard table analysis (default); "
    "TOKUDB_ANALYZE_RECOUNT_ROWS perform logical recount of table rows;"
    "TOKUDB_ANALYZE_CANCEL terminate and cancel all scheduled background jobs "
    "for a table",
    NULL,
    NULL,
    TOKUDB_ANALYZE_STANDARD,
    &tokudb_analyze_mode_typelib);

static MYSQL_THDVAR_ULONGLONG(
    analyze_throttle,
    0,
    "analyze throttle (keys)",
    NULL,
    NULL,
    0,
    0,
    ~0U,
    1);

static MYSQL_THDVAR_UINT(
    analyze_time,
    0,
    "analyze time (seconds)",
    NULL,
    NULL,
    5,
    0,
    ~0U,
    1);

static MYSQL_THDVAR_ULONGLONG(
    auto_analyze,
    0,
    "auto analyze threshold (percent)",
    NULL,
    NULL,
    0,
    0,
    ~0U,
    1);

static MYSQL_THDVAR_UINT(
    block_size,
    0,
    "fractal tree block size",
    NULL,
    NULL,
    4<<20,
    4096,
    ~0U,
    1);

static MYSQL_THDVAR_BOOL(
    bulk_fetch,
    PLUGIN_VAR_THDLOCAL,
    "enable bulk fetch",
    NULL,
    NULL,
    true);

static void checkpoint_lock_update(TOKUDB_UNUSED(THD* thd),
                                   TOKUDB_UNUSED(st_mysql_sys_var* var),
                                   void* var_ptr,
                                   const void* save) {
    my_bool* val = (my_bool*)var_ptr;
    *val= *(my_bool*)save ? true : false;
    if (*val) {
        tokudb_checkpoint_lock(thd);
    } else {
        tokudb_checkpoint_unlock(thd);
    }
}

static MYSQL_THDVAR_BOOL(
    checkpoint_lock,
    0,
    "checkpoint lock",
    NULL,
    checkpoint_lock_update,
    false);

static MYSQL_THDVAR_BOOL(
    commit_sync,
    PLUGIN_VAR_THDLOCAL,
    "sync on txn commit",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_BOOL(
    create_index_online,
    0,
    "if on, create index done online",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_BOOL(
    disable_hot_alter,
    0,
    "if on, hot alter table is disabled",
    NULL,
    NULL,
    false);

static MYSQL_THDVAR_BOOL(
    disable_prefetching,
    0,
    "if on, prefetching disabled",
    NULL,
    NULL,
    false);

static MYSQL_THDVAR_BOOL(
    disable_slow_alter,
    0,
    "if on, alter tables that require copy are disabled",
    NULL,
    NULL,
    false);

static const char *tokudb_empty_scan_names[] = {
    "disabled",
    "lr",
    "rl",
    NullS
};

static TYPELIB tokudb_empty_scan_typelib = {
    array_elements(tokudb_empty_scan_names) - 1,
    "tokudb_empty_scan_typelib",
    tokudb_empty_scan_names,
    NULL
};

static MYSQL_THDVAR_ENUM(
    empty_scan,
    PLUGIN_VAR_OPCMDARG,
    "algorithm to check if the table is empty when opened",
    NULL,
    NULL,
    TOKUDB_EMPTY_SCAN_RL,
    &tokudb_empty_scan_typelib);

static MYSQL_THDVAR_UINT(
    fanout,
    0,
    "fractal tree fanout",
    NULL,
    NULL,
    16,
    2,
    16*1024,
    1);

static MYSQL_THDVAR_BOOL(
    hide_default_row_format,
    0,
    "hide the default row format",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_ULONGLONG(
    killed_time,
    0,
    "killed time",
    NULL,
    NULL,
    DEFAULT_TOKUDB_KILLED_TIME,
    0,
    ~0ULL,
    1);

static MYSQL_THDVAR_STR(last_lock_timeout,
                        PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_NOCMDOPT |
                            PLUGIN_VAR_READONLY,
                        "last lock timeout",
                        NULL,
                        NULL,
                        NULL);

static MYSQL_THDVAR_BOOL(
    load_save_space,
    0,
    "compress intermediate bulk loader files to save space",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_ULONGLONG(
    loader_memory_size,
    0,
    "loader memory size",
    NULL,
    NULL,
    100*1000*1000,
    0,
    ~0ULL,
    1);

static MYSQL_THDVAR_ULONGLONG(
    lock_timeout,
    0,
    "lock timeout",
    NULL,
    NULL,
    DEFAULT_TOKUDB_LOCK_TIMEOUT,
    0,
    ~0ULL,
    1);

static MYSQL_THDVAR_UINT(
    lock_timeout_debug,
    0,
    "lock timeout debug",
    NULL,
    NULL,
    1,
    0,
    ~0U,
    1);

static MYSQL_THDVAR_DOUBLE(
    optimize_index_fraction,
    0,
    "optimize index fraction (default 1.0 all)",
    NULL,
    NULL,
    1.0,
    0,
    1.0,
    1);

static MYSQL_THDVAR_STR(
    optimize_index_name,
    PLUGIN_VAR_THDLOCAL + PLUGIN_VAR_MEMALLOC,
    "optimize index name (default all indexes)",
    NULL,
    NULL,
    NULL);

static MYSQL_THDVAR_ULONGLONG(
    optimize_throttle,
    0,
    "optimize throttle (default no throttle)",
    NULL,
    NULL,
    0,
    0,
    ~0ULL,
    1);

static const char* deprecated_tokudb_pk_insert_mode =
    "Using tokudb_pk_insert_mode is deprecated and the "
    "parameter may be removed in future releases.";
static const char* deprecated_tokudb_pk_insert_mode_zero =
    "Using tokudb_pk_insert_mode=0 is deprecated and the "
    "parameter may be removed in future releases. "
    "Only tokudb_pk_insert_mode=1|2 is allowed."
    "Resettig the value to 1.";

static void pk_insert_mode_update(
    THD* thd,
    st_mysql_sys_var* var,
    void* var_ptr,
    const void* save) {
    const uint* new_pk_insert_mode = static_cast<const uint*>(save);
    uint* pk_insert_mode = static_cast<uint*>(var_ptr);
    if (*new_pk_insert_mode == 0) {
        push_warning(
            thd,
            Sql_condition::WARN_LEVEL_WARN,
            HA_ERR_WRONG_COMMAND,
            deprecated_tokudb_pk_insert_mode_zero);
        *pk_insert_mode = 1;
    } else {
        push_warning(
            thd,
            Sql_condition::WARN_LEVEL_WARN,
            HA_ERR_WRONG_COMMAND,
            deprecated_tokudb_pk_insert_mode);
        *pk_insert_mode = *new_pk_insert_mode;
    }
}

static MYSQL_THDVAR_UINT(
    pk_insert_mode,
    0,
    "set the primary key insert mode",
    NULL,
    pk_insert_mode_update,
    1,
    0,
    2,
    1);

static MYSQL_THDVAR_BOOL(
    prelock_empty,
    0,
    "prelock empty table",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_UINT(
    read_block_size,
    0,
    "fractal tree read block size",
    NULL,
    NULL,
    64*1024,
    4096,
    ~0U,
    1);

static MYSQL_THDVAR_UINT(
    read_buf_size,
    0,
    "range query read buffer size",
    NULL,
    NULL,
    128*1024,
    0,
    1*1024*1024,
    1);

static const char *tokudb_row_format_names[] = {
    "tokudb_uncompressed",
    "tokudb_zlib",
    "tokudb_snappy",
    "tokudb_quicklz",
    "tokudb_lzma",
    "tokudb_fast",
    "tokudb_small",
    "tokudb_default",
    NullS
};

static TYPELIB tokudb_row_format_typelib = {
    array_elements(tokudb_row_format_names) - 1,
    "tokudb_row_format_typelib",
    tokudb_row_format_names,
    NULL
};

static MYSQL_THDVAR_ENUM(
    row_format,
    PLUGIN_VAR_OPCMDARG,
    "Specifies the compression method for a table created during this session. "
    "Possible values are TOKUDB_UNCOMPRESSED, TOKUDB_ZLIB, TOKUDB_SNAPPY, "
    "TOKUDB_QUICKLZ, TOKUDB_LZMA, TOKUDB_FAST, TOKUDB_SMALL and TOKUDB_DEFAULT",
    NULL,
    NULL,
    SRV_ROW_FORMAT_ZLIB,
    &tokudb_row_format_typelib);

#if defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
static MYSQL_THDVAR_BOOL(
    rpl_check_readonly,
    PLUGIN_VAR_THDLOCAL,
    "check if the slave is read only",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_BOOL(
    rpl_lookup_rows,
    PLUGIN_VAR_THDLOCAL,
    "lookup a row on rpl slave",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_ULONGLONG(
    rpl_lookup_rows_delay,
    PLUGIN_VAR_THDLOCAL,
    "time in milliseconds to add to lookups on replication slave",
    NULL,
    NULL,
    0,
    0,
    ~0ULL,
    1);

static MYSQL_THDVAR_BOOL(
    rpl_unique_checks,
    PLUGIN_VAR_THDLOCAL,
    "enable unique checks on replication slave",
    NULL,
    NULL,
    true);

static MYSQL_THDVAR_ULONGLONG(
    rpl_unique_checks_delay,
    PLUGIN_VAR_THDLOCAL,
    "time in milliseconds to add to unique checks test on replication slave",
    NULL,
    NULL,
    0,
    0,
    ~0ULL,
    1);
#endif // defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR

static MYSQL_THDVAR_BOOL(
    enable_fast_update,
    PLUGIN_VAR_THDLOCAL,
    "disable slow update",
    NULL,
    NULL,
    false);

static MYSQL_THDVAR_BOOL(
    enable_fast_upsert,
    PLUGIN_VAR_THDLOCAL,
    "disable slow upsert",
    NULL,
    NULL,
    false);

#if TOKU_INCLUDE_XA
static MYSQL_THDVAR_BOOL(
    support_xa,
    PLUGIN_VAR_OPCMDARG,
    "Enable TokuDB support for the XA two-phase commit",
    NULL,
    NULL,
    true);
#endif

//******************************************************************************
// all system variables
//******************************************************************************
st_mysql_sys_var* system_variables[] = {
    // global vars
    MYSQL_SYSVAR(cache_size),
    MYSQL_SYSVAR(checkpoint_on_flush_logs),
    MYSQL_SYSVAR(cachetable_pool_threads),
    MYSQL_SYSVAR(cardinality_scale_percent),
    MYSQL_SYSVAR(checkpoint_pool_threads),
    MYSQL_SYSVAR(checkpointing_period),
    MYSQL_SYSVAR(cleaner_iterations),
    MYSQL_SYSVAR(cleaner_period),
    MYSQL_SYSVAR(client_pool_threads),
    MYSQL_SYSVAR(compress_buffers_before_eviction),
    MYSQL_SYSVAR(data_dir),
    MYSQL_SYSVAR(debug),
    MYSQL_SYSVAR(directio),
    MYSQL_SYSVAR(enable_partial_eviction),
    MYSQL_SYSVAR(fs_reserve_percent),
    MYSQL_SYSVAR(fsync_log_period),
    MYSQL_SYSVAR(log_dir),
    MYSQL_SYSVAR(max_lock_memory),
    MYSQL_SYSVAR(read_status_frequency),
    MYSQL_SYSVAR(strip_frm_data),
    MYSQL_SYSVAR(tmp_dir),
    MYSQL_SYSVAR(version),
    MYSQL_SYSVAR(write_status_frequency),
    MYSQL_SYSVAR(dir_per_db),
#if defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) && \
    TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
    MYSQL_SYSVAR(gdb_path),
    MYSQL_SYSVAR(gdb_on_fatal),
#endif  // defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) &&
        // TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL

    MYSQL_SYSVAR(check_jemalloc),

    // session vars
    MYSQL_SYSVAR(alter_print_error),
    MYSQL_SYSVAR(analyze_delete_fraction),
    MYSQL_SYSVAR(analyze_in_background),
    MYSQL_SYSVAR(analyze_mode),
    MYSQL_SYSVAR(analyze_throttle),
    MYSQL_SYSVAR(analyze_time),
    MYSQL_SYSVAR(auto_analyze),
    MYSQL_SYSVAR(block_size),
    MYSQL_SYSVAR(bulk_fetch),
    MYSQL_SYSVAR(checkpoint_lock),
    MYSQL_SYSVAR(commit_sync),
    MYSQL_SYSVAR(create_index_online),
    MYSQL_SYSVAR(disable_hot_alter),
    MYSQL_SYSVAR(disable_prefetching),
    MYSQL_SYSVAR(disable_slow_alter),
    MYSQL_SYSVAR(empty_scan),
    MYSQL_SYSVAR(fanout),
    MYSQL_SYSVAR(hide_default_row_format),
    MYSQL_SYSVAR(killed_time),
    MYSQL_SYSVAR(last_lock_timeout),
    MYSQL_SYSVAR(load_save_space),
    MYSQL_SYSVAR(loader_memory_size),
    MYSQL_SYSVAR(lock_timeout),
    MYSQL_SYSVAR(lock_timeout_debug),
    MYSQL_SYSVAR(optimize_index_fraction),
    MYSQL_SYSVAR(optimize_index_name),
    MYSQL_SYSVAR(optimize_throttle),
    MYSQL_SYSVAR(pk_insert_mode),
    MYSQL_SYSVAR(prelock_empty),
    MYSQL_SYSVAR(read_block_size),
    MYSQL_SYSVAR(read_buf_size),
    MYSQL_SYSVAR(row_format),
#if defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
    MYSQL_SYSVAR(rpl_check_readonly),
    MYSQL_SYSVAR(rpl_lookup_rows),
    MYSQL_SYSVAR(rpl_lookup_rows_delay),
    MYSQL_SYSVAR(rpl_unique_checks),
    MYSQL_SYSVAR(rpl_unique_checks_delay),
#endif // defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
    MYSQL_SYSVAR(enable_fast_update),
    MYSQL_SYSVAR(enable_fast_upsert),
#if TOKU_INCLUDE_XA
    MYSQL_SYSVAR(support_xa),
#endif

#if defined(TOKUDB_DEBUG) && TOKUDB_DEBUG
   MYSQL_SYSVAR(debug_pause_background_job_manager),
#endif // TOKUDB_DEBUG

    NULL
};

my_bool alter_print_error(THD* thd) {
    return (THDVAR(thd, alter_print_error) != 0);
}
double analyze_delete_fraction(THD* thd) {
    return THDVAR(thd, analyze_delete_fraction);
}
my_bool analyze_in_background(THD* thd) {
    return (THDVAR(thd, analyze_in_background) != 0);
}
analyze_mode_t analyze_mode(THD* thd) {
    return (analyze_mode_t ) THDVAR(thd, analyze_mode);
}
ulonglong analyze_throttle(THD* thd) {
    return THDVAR(thd, analyze_throttle);
}
ulonglong analyze_time(THD* thd) {
    return THDVAR(thd, analyze_time);
}
ulonglong auto_analyze(THD* thd) {
    return THDVAR(thd, auto_analyze);
}
my_bool bulk_fetch(THD* thd) {
    return (THDVAR(thd, bulk_fetch) != 0);
}
uint block_size(THD* thd) {
    return THDVAR(thd, block_size);
}
my_bool commit_sync(THD* thd) {
    return (THDVAR(thd, commit_sync) != 0);
}
my_bool create_index_online(THD* thd) {
    return (THDVAR(thd, create_index_online) != 0);
}
my_bool disable_hot_alter(THD* thd) {
    return (THDVAR(thd, disable_hot_alter) != 0);
}
my_bool disable_prefetching(THD* thd) {
    return (THDVAR(thd, disable_prefetching) != 0);
}
my_bool disable_slow_alter(THD* thd) {
    return (THDVAR(thd, disable_slow_alter) != 0);
}
#if defined(TOKU_INCLUDE_UPSERT) && TOKU_INCLUDE_UPSERT
my_bool enable_fast_update(THD* thd) {
    return (THDVAR(thd, enable_fast_update) != 0);
}
my_bool enable_fast_upsert(THD* thd) {
    return (THDVAR(thd, enable_fast_upsert) != 0);
}
#endif  // defined(TOKU_INCLUDE_UPSERT) && TOKU_INCLUDE_UPSERT
empty_scan_mode_t empty_scan(THD* thd) {
    return (empty_scan_mode_t)THDVAR(thd, empty_scan);
}
uint fanout(THD* thd) {
    return THDVAR(thd, fanout);
}
my_bool hide_default_row_format(THD* thd) {
    return (THDVAR(thd, hide_default_row_format) != 0);
}
ulonglong killed_time(THD* thd) {
    return THDVAR(thd, killed_time);
}
char* last_lock_timeout(THD* thd) {
    return THDVAR(thd, last_lock_timeout);
}
void set_last_lock_timeout(THD* thd, char* last) {
    THDVAR(thd, last_lock_timeout) = last;
}
my_bool load_save_space(THD* thd) {
    return (THDVAR(thd, load_save_space) != 0);
}
ulonglong loader_memory_size(THD* thd) {
    return THDVAR(thd, loader_memory_size);
}
ulonglong lock_timeout(THD* thd) {
    return THDVAR(thd, lock_timeout);
}
uint lock_timeout_debug(THD* thd) {
    return THDVAR(thd, lock_timeout_debug);
}
double optimize_index_fraction(THD* thd) {
    return THDVAR(thd, optimize_index_fraction);
}
const char* optimize_index_name(THD* thd) {
    return THDVAR(thd, optimize_index_name);
}
ulonglong optimize_throttle(THD* thd) {
    return THDVAR(thd, optimize_throttle);
}
uint pk_insert_mode(THD* thd) {
    return THDVAR(thd, pk_insert_mode);
}
void set_pk_insert_mode(THD* thd, uint mode) {
    THDVAR(thd, pk_insert_mode) = mode;
}
my_bool prelock_empty(THD* thd) {
    return (THDVAR(thd, prelock_empty) != 0);
}
uint read_block_size(THD* thd) {
    return THDVAR(thd, read_block_size);
}
uint read_buf_size(THD* thd) {
    return THDVAR(thd, read_buf_size);
}
row_format_t row_format(THD *thd) {
    return (row_format_t) THDVAR(thd, row_format);
}
#if defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
my_bool rpl_check_readonly(THD* thd) {
    return (THDVAR(thd, rpl_check_readonly) != 0);
}
my_bool rpl_lookup_rows(THD* thd) {
    return (THDVAR(thd, rpl_lookup_rows) != 0);
}
ulonglong rpl_lookup_rows_delay(THD* thd) {
    return THDVAR(thd, rpl_lookup_rows_delay);
}
my_bool rpl_unique_checks(THD* thd) {
    return (THDVAR(thd, rpl_unique_checks) != 0);
}
ulonglong rpl_unique_checks_delay(THD* thd) {
    return THDVAR(thd, rpl_unique_checks_delay);
}
#endif // defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
my_bool support_xa(THD* thd) {
    return (THDVAR(thd, support_xa) != 0);
}

#if defined(TOKU_INCLUDE_OPTION_STRUCTS) && TOKU_INCLUDE_OPTION_STRUCTS
ha_create_table_option tokudb_table_options[] = {
    HA_TOPTION_SYSVAR("compression", row_format, row_format),
    HA_TOPTION_END
};

ha_create_table_option tokudb_index_options[] = {
    HA_IOPTION_BOOL("clustering", clustering, 0),
    HA_IOPTION_END
};
#endif  // defined(TOKU_INCLUDE_OPTION_STRUCTS) && TOKU_INCLUDE_OPTION_STRUCTS

} // namespace sysvars
} // namespace tokudb
