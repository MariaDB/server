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
#include "PerconaFT/src/ydb.h"

#define TOKU_METADB_NAME "tokudb_meta"

#if defined(HAVE_PSI_MUTEX_INTERFACE)
//static pfs_key_t tokudb_map_mutex_key;

static PSI_mutex_info all_tokudb_mutexes[] = {
    //{&tokudb_map_mutex_key, "tokudb_map_mutex", 0},
    {&ha_tokudb_mutex_key, "ha_tokudb_mutex", 0},
};

static PSI_rwlock_info all_tokudb_rwlocks[] = {
    {&num_DBs_lock_key, "num_DBs_lock", 0},
};
#endif /* HAVE_PSI_MUTEX_INTERFACE */

typedef struct savepoint_info {
    DB_TXN* txn;
    tokudb_trx_data* trx;
    bool in_sub_stmt;
} *SP_INFO, SP_INFO_T;

static handler* tokudb_create_handler(
    handlerton* hton,
    TABLE_SHARE* table,
    MEM_ROOT* mem_root);

static void tokudb_print_error(
    const DB_ENV* db_env,
    const char* db_errpfx,
    const char* buffer);
static void tokudb_cleanup_log_files(void);
static int tokudb_end(handlerton* hton, ha_panic_function type);
static bool tokudb_flush_logs(handlerton* hton);
static bool tokudb_show_status(
    handlerton* hton,
    THD* thd,
    stat_print_fn* print,
    enum ha_stat_type);
#if defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) && \
    TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
static void tokudb_handle_fatal_signal(handlerton* hton, THD* thd, int sig);
#endif  // defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) &&
        // TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
static int tokudb_close_connection(handlerton* hton, THD* thd);
static void tokudb_kill_connection(handlerton *hton, THD *thd, enum thd_kill_levels level);
static int tokudb_commit(handlerton* hton, THD* thd, bool all);
static int tokudb_rollback(handlerton* hton, THD* thd, bool all);
#if defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA
static int tokudb_xa_prepare(handlerton* hton, THD* thd, bool all);
static int tokudb_xa_recover(handlerton* hton, XID* xid_list, uint len);
static int tokudb_commit_by_xid(handlerton* hton, XID* xid);
static int tokudb_rollback_by_xid(handlerton* hton, XID* xid);
#endif  // defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA

static int tokudb_rollback_to_savepoint(
    handlerton* hton,
    THD* thd,
    void* savepoint);
static int tokudb_savepoint(handlerton* hton, THD* thd, void* savepoint);
static int tokudb_release_savepoint(
    handlerton* hton,
    THD* thd,
    void* savepoint);
#if 100000 <= MYSQL_VERSION_ID
static int tokudb_discover_table(handlerton *hton, THD* thd, TABLE_SHARE *ts);
static int tokudb_discover_table_existence(
    handlerton* hton,
    const char* db,
    const char* name);
#endif
#if defined(TOKU_INCLUDE_DISCOVER_FRM) && TOKU_INCLUDE_DISCOVER_FRM
static int tokudb_discover(
    handlerton* hton,
    THD* thd,
    const char* db,
    const char* name,
    uchar** frmblob,
    size_t* frmlen);
static int tokudb_discover2(
    handlerton* hton,
    THD* thd,
    const char* db,
    const char* name,
    bool translate_name,
    uchar** frmblob,
    size_t* frmlen);
static int tokudb_discover3(
    handlerton* hton,
    THD* thd,
    const char* db,
    const char* name,
    char* path,
    uchar** frmblob,
    size_t* frmlen);
#endif  // defined(TOKU_INCLUDE_DISCOVER_FRM) && TOKU_INCLUDE_DISCOVER_FRM
handlerton* tokudb_hton;

const char* ha_tokudb_ext = ".tokudb";
DB_ENV* db_env;

//static tokudb::thread::mutex_t tokudb_map_mutex;
#if defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG
static TREE tokudb_map;
struct tokudb_map_pair {
    THD* thd;
    char *last_lock_timeout;
};
#if 50500 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50599
static int tokudb_map_pair_cmp(void *custom_arg, const void *a, const void *b) {
#else
static int tokudb_map_pair_cmp(
    TOKUDB_UNUSED(const void* custom_arg),
    const void* a,
    const void* b) {
#endif

    const struct tokudb_map_pair *a_key = (const struct tokudb_map_pair *) a;
    const struct tokudb_map_pair *b_key = (const struct tokudb_map_pair *) b;
    if (a_key->thd < b_key->thd)
        return -1;
    else if (a_key->thd > b_key->thd)
        return +1;
    else
        return 0;
};
#endif  // defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG

static PARTITIONED_COUNTER tokudb_primary_key_bytes_inserted;
void toku_hton_update_primary_key_bytes_inserted(uint64_t row_size) {
    increment_partitioned_counter(tokudb_primary_key_bytes_inserted, row_size);
}

static void tokudb_lock_timeout_callback(
    DB* db,
    uint64_t requesting_txnid,
    const DBT* left_key,
    const DBT* right_key,
    uint64_t blocking_txnid);

static void tokudb_lock_wait_needed_callback(
    void* arg,
    uint64_t requesting_txnid,
    uint64_t blocking_txnid);

#define ASSERT_MSGLEN 1024

void toku_hton_assert_fail(
    const char* expr_as_string,
    const char* fun,
    const char* file,
    int line,
    int caller_errno) {

    char msg[ASSERT_MSGLEN];
    if (db_env) {
        snprintf(msg, ASSERT_MSGLEN, "Handlerton: %s ", expr_as_string);
        db_env->crash(db_env, msg, fun, file, line,caller_errno);
    } else {
        snprintf(
            msg,
            ASSERT_MSGLEN,
            "Handlerton assertion failed, no env, %s, %d, %s, %s (errno=%d)\n",
            file,
            line,
            fun,
            expr_as_string,
            caller_errno);
        perror(msg);
        fflush(stderr);
    }
    abort();
}

//my_bool tokudb_shared_data = false;
static uint32_t tokudb_init_flags = 
    DB_CREATE | DB_THREAD | DB_PRIVATE | 
    DB_INIT_LOCK | 
    DB_INIT_MPOOL |
    DB_INIT_TXN | 
    DB_INIT_LOG |
    DB_RECOVER;
static uint32_t tokudb_env_flags = 0;
// static uint32_t tokudb_lock_type = DB_LOCK_DEFAULT;
// static ulong tokudb_log_buffer_size = 0;
// static ulong tokudb_log_file_size = 0;
static char* tokudb_home;
// static long tokudb_lock_scan_time = 0;
// static ulong tokudb_region_size = 0;
// static ulong tokudb_cache_parts = 1;
const char* tokudb_hton_name = "TokuDB";

#if defined(_WIN32)
extern "C" {
#include "ydb.h"
}
#endif

// A flag set if the handlerton is in an initialized, usable state,
// plus a reader-write lock to protect it without serializing reads.
// Since we don't have static initializers for the opaque rwlock type,
// use constructor and destructor functions to create and destroy
// the lock before and after main(), respectively.
int tokudb_hton_initialized;

// tokudb_hton_initialized_lock can not be instrumented as it must be
// initialized before mysql_mutex_register() call to protect
// some globals from race condition.
tokudb::thread::rwlock_t tokudb_hton_initialized_lock;

static SHOW_VAR *toku_global_status_variables = NULL;
static uint64_t toku_global_status_max_rows;
static TOKU_ENGINE_STATUS_ROW_S* toku_global_status_rows = NULL;

static void handle_ydb_error(int error) {
    switch (error) {
    case TOKUDB_HUGE_PAGES_ENABLED:
        sql_print_error("************************************************************");
        sql_print_error("                                                            ");
        sql_print_error("                        @@@@@@@@@@@                         ");
        sql_print_error("                      @@'         '@@                       ");
        sql_print_error("                     @@    _     _  @@                      ");
        sql_print_error("                     |    (.)   (.)  |                      ");
        sql_print_error("                     |             ` |                      ");
        sql_print_error("                     |        >    ' |                      ");
        sql_print_error("                     |     .----.    |                      ");
        sql_print_error("                     ..   |.----.|  ..                      ");
        sql_print_error("                      ..  '      ' ..                       ");
        sql_print_error("                        .._______,.                         ");
        sql_print_error("                                                            ");
        sql_print_error("%s will not run with transparent huge pages enabled.        ", tokudb_hton_name);
        sql_print_error("Please disable them to continue.                            ");
        sql_print_error("(echo never > /sys/kernel/mm/transparent_hugepage/enabled)  ");
        sql_print_error("                                                            ");
        sql_print_error("************************************************************");
        break;
    case TOKUDB_UPGRADE_FAILURE:
        sql_print_error(
            "%s upgrade failed. A clean shutdown of the previous version is "
            "required.",
            tokudb_hton_name);
        break;
    default:
        sql_print_error("%s unknown error %d", tokudb_hton_name, error);
        break;
    }
}

static int tokudb_set_product_name(void) {
    size_t n = strlen(tokudb_hton_name);
    char tokudb_product_name[n+1];
    memset(tokudb_product_name, 0, sizeof tokudb_product_name);
    for (size_t i = 0; i < n; i++)
        tokudb_product_name[i] = tolower(tokudb_hton_name[i]);
    int r = db_env_set_toku_product_name(tokudb_product_name);
    return r;
}

static int tokudb_init_func(void *p) {
    TOKUDB_DBUG_ENTER("%p", p);

    int r = toku_ydb_init();
    assert(r==0);

    // 3938: lock the handlerton's initialized status flag for writing
    rwlock_t_lock_write(tokudb_hton_initialized_lock);

#ifdef HAVE_PSI_INTERFACE
    /* Register TokuDB mutex keys with MySQL performance schema */
    int count;

    count = array_elements(all_tokudb_mutexes);
    mysql_mutex_register("tokudb", all_tokudb_mutexes, count);

    count = array_elements(all_tokudb_rwlocks);
    mysql_rwlock_register("tokudb", all_tokudb_rwlocks, count);

    //tokudb_map_mutex.reinit(tokudb_map_mutex_key);
#endif /* HAVE_PSI_INTERFACE */

    db_env = NULL;
    tokudb_hton = (handlerton*)p;

    if (tokudb::sysvars::check_jemalloc) {
        typedef int (*mallctl_type)(
            const char*,
            void*,
            size_t*,
            void*,
            size_t);
        mallctl_type mallctl_func;
        mallctl_func= (mallctl_type)dlsym(RTLD_DEFAULT, "mallctl");
        if (!mallctl_func) {
            sql_print_error(
                "%s is not initialized because jemalloc is not loaded",
                tokudb_hton_name);
            goto error;
        }
        char *ver;
        size_t len = sizeof(ver);
        mallctl_func("version", &ver, &len, NULL, 0);
        /* jemalloc 2.2.5 crashes mysql-test */
        if (strcmp(ver, "2.3.") < 0) {
            sql_print_error(
                "%s is not initialized because jemalloc is older than 2.3.0",
                tokudb_hton_name);
            goto error;
        }
    }

    r = tokudb_set_product_name();
    if (r) {
        sql_print_error(
            "%s can not set product name error %d",
            tokudb_hton_name,
            r);
        goto error;
    }

    TOKUDB_SHARE::static_init();
    tokudb::background::initialize();

    tokudb_hton->state = SHOW_OPTION_YES;
    // tokudb_hton->flags= HTON_CAN_RECREATE;  // QQQ this came from skeleton
    tokudb_hton->flags = HTON_CLOSE_CURSORS_AT_COMMIT | HTON_SUPPORTS_EXTENDED_KEYS;

#if defined(TOKU_INCLUDE_EXTENDED_KEYS) && TOKU_INCLUDE_EXTENDED_KEYS
#if defined(HTON_SUPPORTS_EXTENDED_KEYS)
    tokudb_hton->flags |= HTON_SUPPORTS_EXTENDED_KEYS;
#endif
#if defined(HTON_EXTENDED_KEYS)
    tokudb_hton->flags |= HTON_EXTENDED_KEYS;
#endif
#endif
#if defined(HTON_SUPPORTS_CLUSTERED_KEYS)
    tokudb_hton->flags |= HTON_SUPPORTS_CLUSTERED_KEYS;
#endif

#if defined(TOKU_USE_DB_TYPE_TOKUDB) && TOKU_USE_DB_TYPE_TOKUDB
    tokudb_hton->db_type = DB_TYPE_TOKUDB;
#elif defined(TOKU_USE_DB_TYPE_UNKNOWN) && TOKU_USE_DB_TYPE_UNKNOWN
    tokudb_hton->db_type = DB_TYPE_UNKNOWN;
#else
#error
#endif

    tokudb_hton->create = tokudb_create_handler;
    tokudb_hton->close_connection = tokudb_close_connection;
    tokudb_hton->kill_query = tokudb_kill_connection;

    tokudb_hton->savepoint_offset = sizeof(SP_INFO_T);
    tokudb_hton->savepoint_set = tokudb_savepoint;
    tokudb_hton->savepoint_rollback = tokudb_rollback_to_savepoint;
    tokudb_hton->savepoint_release = tokudb_release_savepoint;

#if 100000 <= MYSQL_VERSION_ID
    tokudb_hton->discover_table = tokudb_discover_table;
    tokudb_hton->discover_table_existence = tokudb_discover_table_existence;
#else
#if defined(TOKU_INCLUDE_DISCOVER_FRM) && TOKU_INCLUDE_DISCOVER_FRM
    tokudb_hton->discover = tokudb_discover;
#if defined(MYSQL_HANDLERTON_INCLUDE_DISCOVER2)
    tokudb_hton->discover2 = tokudb_discover2;
#endif  // MYSQL_HANDLERTON_INCLUDE_DISCOVER2
#endif  // defined(TOKU_INCLUDE_DISCOVER_FRM) && TOKU_INCLUDE_DISCOVER_FRM
#endif  // 100000 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 100099
    tokudb_hton->commit = tokudb_commit;
    tokudb_hton->rollback = tokudb_rollback;
#if defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA
    tokudb_hton->prepare = tokudb_xa_prepare;
    tokudb_hton->recover = tokudb_xa_recover;
    tokudb_hton->commit_by_xid = tokudb_commit_by_xid;
    tokudb_hton->rollback_by_xid = tokudb_rollback_by_xid;
#endif  // defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA

    tokudb_hton->panic = tokudb_end;
    tokudb_hton->flush_logs = tokudb_flush_logs;
    tokudb_hton->show_status = tokudb_show_status;
#if defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) && \
    TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
    tokudb_hton->handle_fatal_signal = tokudb_handle_fatal_signal;
#endif  // defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) &&
        // TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL

#if defined(TOKU_INCLUDE_OPTION_STRUCTS) && TOKU_INCLUDE_OPTION_STRUCTS
    tokudb_hton->table_options = tokudb::sysvars::tokudb_table_options;
    tokudb_hton->index_options = tokudb::sysvars::tokudb_index_options;
#endif  // defined(TOKU_INCLUDE_OPTION_STRUCTS) && TOKU_INCLUDE_OPTION_STRUCTS

    if (!tokudb_home)
        tokudb_home = mysql_real_data_home;
    DBUG_PRINT("info", ("tokudb_home: %s", tokudb_home));

    if ((r = db_env_create(&db_env, 0))) {
        DBUG_PRINT("info", ("db_env_create %d\n", r));
        handle_ydb_error(r);
        goto error;
    }

    DBUG_PRINT("info", ("tokudb_env_flags: 0x%x\n", tokudb_env_flags));
    r = db_env->set_flags(db_env, tokudb_env_flags, 1);
    if (r) { // QQQ
        TOKUDB_TRACE_FOR_FLAGS(
            TOKUDB_DEBUG_INIT,
            "WARNING: flags=%x r=%d",
            tokudb_env_flags,
            r);
        // goto error;
    }

    // config error handling
    db_env->set_errcall(db_env, tokudb_print_error);
    db_env->set_errpfx(db_env, tokudb_hton_name);

    // Handle deprecated options
    if (tokudb::sysvars::pk_insert_mode(NULL) != 1) {
        TOKUDB_TRACE("Using tokudb_pk_insert_mode is deprecated and the "
            "parameter may be removed in future releases. "
            "tokudb_pk_insert_mode=0 is now forbidden. "
            "See documentation and release notes for details");
        if (tokudb::sysvars::pk_insert_mode(NULL) < 1)
           tokudb::sysvars::set_pk_insert_mode(NULL, 1);
    }

    //
    // set default comparison functions
    //
    r = db_env->set_default_bt_compare(db_env, tokudb_cmp_dbt_key);
    if (r) {
        DBUG_PRINT("info", ("set_default_bt_compare%d\n", r));
        goto error; 
    }

    {
        char* tmp_dir = tokudb::sysvars::tmp_dir;
        char* data_dir = tokudb::sysvars::data_dir;
        if (data_dir == 0) {
            data_dir = mysql_data_home;
        }
        if (tmp_dir == 0) {
            tmp_dir = data_dir;
        }
        DBUG_PRINT("info", ("tokudb_data_dir: %s\n", data_dir));
        db_env->set_data_dir(db_env, data_dir);
        DBUG_PRINT("info", ("tokudb_tmp_dir: %s\n", tmp_dir));
        db_env->set_tmp_dir(db_env, tmp_dir);
    }

    if (tokudb::sysvars::log_dir) {
        DBUG_PRINT("info", ("tokudb_log_dir: %s\n", tokudb::sysvars::log_dir));
        db_env->set_lg_dir(db_env, tokudb::sysvars::log_dir);
    }

    // config the cache table size to min(1/2 of physical memory, 1/8 of the
    // process address space)
    if (tokudb::sysvars::cache_size == 0) {
        uint64_t physmem, maxdata;
        physmem = toku_os_get_phys_memory_size();
        tokudb::sysvars::cache_size = physmem / 2;
        r = toku_os_get_max_process_data_size(&maxdata);
        if (r == 0) {
            if (tokudb::sysvars::cache_size > maxdata / 8)
                tokudb::sysvars::cache_size = maxdata / 8;
        }
    }
    if (tokudb::sysvars::cache_size) {
        DBUG_PRINT(
            "info",
            ("tokudb_cache_size: %lld\n", tokudb::sysvars::cache_size));
        r = db_env->set_cachesize(
            db_env,
            (uint32_t)(tokudb::sysvars::cache_size >> 30),
            (uint32_t)(tokudb::sysvars::cache_size  %
                (1024L * 1024L * 1024L)), 1);
        if (r) {
            DBUG_PRINT("info", ("set_cachesize %d\n", r));
            goto error; 
        }
    }
    if (tokudb::sysvars::max_lock_memory == 0) {
        tokudb::sysvars::max_lock_memory = tokudb::sysvars::cache_size/8;
    }
    if (tokudb::sysvars::max_lock_memory) {
        DBUG_PRINT(
            "info",
            ("tokudb_max_lock_memory: %lld\n",
            tokudb::sysvars::max_lock_memory));
        r = db_env->set_lk_max_memory(
            db_env,
            tokudb::sysvars::max_lock_memory);
        if (r) {
            DBUG_PRINT("info", ("set_lk_max_memory %d\n", r));
            goto error; 
        }
    }
    
    uint32_t gbytes, bytes; int parts;
    r = db_env->get_cachesize(db_env, &gbytes, &bytes, &parts);
    TOKUDB_TRACE_FOR_FLAGS(
        TOKUDB_DEBUG_INIT,
        "tokudb_cache_size=%lld r=%d",
        ((unsigned long long) gbytes << 30) + bytes,
        r);

    r = db_env->set_client_pool_threads(
        db_env,
        tokudb::sysvars::client_pool_threads);
    if (r) {
        DBUG_PRINT("info", ("set_client_pool_threads %d\n", r));
        goto error;
    }

    r = db_env->set_cachetable_pool_threads(
        db_env,
        tokudb::sysvars::cachetable_pool_threads);
    if (r) {
        DBUG_PRINT("info", ("set_cachetable_pool_threads %d\n", r));
        goto error;
    }

    r = db_env->set_checkpoint_pool_threads(
        db_env,
        tokudb::sysvars::checkpoint_pool_threads);
    if (r) {
        DBUG_PRINT("info", ("set_checkpoint_pool_threads %d\n", r));
        goto error;
    }

    if (db_env->set_redzone) {
        r = db_env->set_redzone(db_env, tokudb::sysvars::fs_reserve_percent);
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_INIT, "set_redzone r=%d", r);
    }
    TOKUDB_TRACE_FOR_FLAGS(
        TOKUDB_DEBUG_INIT,
        "env open:flags=%x",
        tokudb_init_flags);

    r = db_env->set_generate_row_callback_for_put(db_env, generate_row_for_put);
    assert_always(r == 0);

    r = db_env->set_generate_row_callback_for_del(db_env, generate_row_for_del);
    assert_always(r == 0);

    db_env->set_update(db_env, tokudb_update_fun);

    db_env_set_direct_io(tokudb::sysvars::directio);

    db_env_set_compress_buffers_before_eviction(
        tokudb::sysvars::compress_buffers_before_eviction);

    db_env->change_fsync_log_period(db_env, tokudb::sysvars::fsync_log_period);

    db_env->set_lock_timeout_callback(db_env, tokudb_lock_timeout_callback);
    db_env->set_dir_per_db(db_env, tokudb::sysvars::dir_per_db);
    db_env->set_lock_wait_callback(db_env, tokudb_lock_wait_needed_callback);

    db_env->set_loader_memory_size(
        db_env,
        tokudb_get_loader_memory_size_callback);

    db_env->set_check_thp(db_env, tokudb::sysvars::check_jemalloc);

    r = db_env->open(
        db_env,
        tokudb_home,
        tokudb_init_flags,
        S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);

    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_INIT, "env opened:return=%d", r);

    if (r) {
        DBUG_PRINT("info", ("env->open %d", r));
        handle_ydb_error(r);
        goto error;
    }

    r = db_env->checkpointing_set_period(
        db_env,
        tokudb::sysvars::checkpointing_period);
    assert_always(r == 0);

    r = db_env->cleaner_set_period(db_env, tokudb::sysvars::cleaner_period);
    assert_always(r == 0);

    r = db_env->cleaner_set_iterations(
        db_env,
        tokudb::sysvars::cleaner_iterations);
    assert_always(r == 0);

    r = db_env->set_lock_timeout(
        db_env,
        DEFAULT_TOKUDB_LOCK_TIMEOUT,
        tokudb_get_lock_wait_time_callback);
    assert_always(r == 0);

    r = db_env->evictor_set_enable_partial_eviction(
        db_env,
        tokudb::sysvars::enable_partial_eviction);
    assert_always(r == 0);

    db_env->set_killed_callback(
        db_env,
        DEFAULT_TOKUDB_KILLED_TIME,
        tokudb_get_killed_time_callback,
        tokudb_killed_callback);

    r = db_env->get_engine_status_num_rows(
        db_env,
        &toku_global_status_max_rows);
    assert_always(r == 0);

    {
        const myf mem_flags =
            MY_FAE|MY_WME|MY_ZEROFILL|MY_ALLOW_ZERO_PTR|MY_FREE_ON_ERROR;
        toku_global_status_variables =
            (SHOW_VAR*)tokudb::memory::malloc(
                sizeof(*toku_global_status_variables) *
                    toku_global_status_max_rows,
                mem_flags);
        toku_global_status_rows =
            (TOKU_ENGINE_STATUS_ROW_S*)tokudb::memory::malloc(
                sizeof(*toku_global_status_rows)*
                    toku_global_status_max_rows,
                mem_flags);
    }

    tokudb_primary_key_bytes_inserted = create_partitioned_counter();

#if defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG
    init_tree(&tokudb_map, 0, 0, 0, tokudb_map_pair_cmp, true, NULL, NULL);
#endif  // defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG

    if (tokudb::sysvars::strip_frm_data) {
        r = tokudb::metadata::strip_frm_data(db_env);
        if (r) {
            DBUG_PRINT("info", ("env->open %d", r));
            handle_ydb_error(r);
            goto error;
        }
    }

    //3938: succeeded, set the init status flag and unlock
    tokudb_hton_initialized = 1;
    tokudb_hton_initialized_lock.unlock();
    DBUG_RETURN(false);

error:
    if (db_env) {
        int rr= db_env->close(db_env, 0);
        assert_always(rr==0);
        db_env = 0;
    }

    // 3938: failed to initialized, drop the flag and lock
    tokudb_hton_initialized = 0;
    tokudb_hton_initialized_lock.unlock();
    DBUG_RETURN(true);
}

static int tokudb_done_func(TOKUDB_UNUSED(void* p)) {
    TOKUDB_DBUG_ENTER("");
    tokudb::memory::free(toku_global_status_variables);
    toku_global_status_variables = NULL;
    tokudb::memory::free(toku_global_status_rows);
    toku_global_status_rows = NULL;
    toku_ydb_destroy();
    TOKUDB_DBUG_RETURN(0);
}

static handler* tokudb_create_handler(
    handlerton* hton,
    TABLE_SHARE* table,
    MEM_ROOT* mem_root) {
    return new(mem_root) ha_tokudb(hton, table);
}

int tokudb_end(TOKUDB_UNUSED(handlerton* hton),
               TOKUDB_UNUSED(ha_panic_function type)) {
    TOKUDB_DBUG_ENTER("");
    int error = 0;
    
    // 3938: if we finalize the storage engine plugin, it is no longer
    // initialized. grab a writer lock for the duration of the
    // call, so we can drop the flag and destroy the mutexes
    // in isolation.
    rwlock_t_lock_write(tokudb_hton_initialized_lock);
    assert_always(tokudb_hton_initialized);

    tokudb::background::destroy();
    TOKUDB_SHARE::static_destroy();

    if (db_env) {
        if (tokudb_init_flags & DB_INIT_LOG)
            tokudb_cleanup_log_files();

        // count the total number of prepared txn's that we discard
        long total_prepared = 0;
#if defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "begin XA cleanup");
        while (1) {
            // get xid's 
            const long n_xid = 1;
            TOKU_XA_XID xids[n_xid];
            long n_prepared = 0;
            error = db_env->txn_xa_recover(
                db_env,
                xids,
                n_xid,
                &n_prepared,
                total_prepared == 0 ? DB_FIRST : DB_NEXT);
            assert_always(error == 0);
            if (n_prepared == 0) 
                break;
            // discard xid's
            for (long i = 0; i < n_xid; i++) {
                DB_TXN *txn = NULL;
                error = db_env->get_txn_from_xid(db_env, &xids[i], &txn);
                assert_always(error == 0);
                error = txn->discard(txn, 0);
                assert_always(error == 0);
            }
            total_prepared += n_prepared;
        }
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "end XA cleanup");
#endif  // defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA
        error = db_env->close(
            db_env,
            total_prepared > 0 ? TOKUFT_DIRTY_SHUTDOWN : 0);
#if defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA
        if (error != 0 && total_prepared > 0) {
            sql_print_error(
                "%s: %ld prepared txns still live, please shutdown, error %d",
                tokudb_hton_name,
                total_prepared,
                error);
        } else
#endif  // defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA
        assert_always(error == 0);
        db_env = NULL;
    }

    if (tokudb_primary_key_bytes_inserted) {
        destroy_partitioned_counter(tokudb_primary_key_bytes_inserted);
        tokudb_primary_key_bytes_inserted = NULL;
    }

#if defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG
    delete_tree(&tokudb_map);
#endif  // defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG

    // 3938: drop the initialized flag and unlock
    tokudb_hton_initialized = 0;
    tokudb_hton_initialized_lock.unlock();

    TOKUDB_DBUG_RETURN(error);
}

static int tokudb_close_connection(TOKUDB_UNUSED(handlerton* hton), THD* thd) {
    int error = 0;
    tokudb_trx_data* trx = (tokudb_trx_data*)thd_get_ha_data(thd, tokudb_hton);
    if (trx && trx->checkpoint_lock_taken) {
        error = db_env->checkpointing_resume(db_env);
    }
    tokudb::memory::free(trx);
#if defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG
    mutex_t_lock(tokudb_map_mutex);
    struct tokudb_map_pair key = {thd, NULL};
    struct tokudb_map_pair* found_key =
        (struct tokudb_map_pair*)tree_search(&tokudb_map, &key, NULL);

    if (found_key) {
        tokudb::memory::free(found_key->last_lock_timeout);
        tree_delete(&tokudb_map, found_key, sizeof(*found_key), NULL);
    }
    mutex_t_unlock(tokudb_map_mutex);
#endif  // defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG
    return error;
}

void tokudb_kill_connection(TOKUDB_UNUSED(handlerton *hton), THD *thd,
                            TOKUDB_UNUSED(enum thd_kill_levels level)) {
    TOKUDB_DBUG_ENTER("");
    db_env->kill_waiter(db_env, thd);
    DBUG_VOID_RETURN;
}

bool tokudb_flush_logs(TOKUDB_UNUSED(handlerton* hton)) {
    TOKUDB_DBUG_ENTER("");
    int error;
    bool result = 0;

    if (tokudb::sysvars::checkpoint_on_flush_logs) {
        //
        // take the checkpoint
        //
        error = db_env->txn_checkpoint(db_env, 0, 0, 0);
        if (error) {
            my_error(ER_ERROR_DURING_CHECKPOINT, MYF(0), error);
            result = 1;
            goto exit;
        }
    }
    else {
        error = db_env->log_flush(db_env, NULL);
        assert_always(error == 0);
    }

    result = 0;
exit:
    TOKUDB_DBUG_RETURN(result);
}


typedef struct txn_progress_info {
    char status[200];
    THD* thd;
} *TXN_PROGRESS_INFO;

static void txn_progress_func(TOKU_TXN_PROGRESS progress, void* extra) {
    TXN_PROGRESS_INFO progress_info = (TXN_PROGRESS_INFO)extra;
    int r = sprintf(
        progress_info->status,
            "%sprocessing %s of transaction, %" PRId64 " out of %" PRId64,
            progress->stalled_on_checkpoint ? "Writing committed changes to disk, " : "",
            progress->is_commit ? "commit" : "abort",
            progress->entries_processed,
            progress->entries_total);
    assert_always(r >= 0);
    thd_proc_info(progress_info->thd, progress_info->status);
}

static void commit_txn_with_progress(DB_TXN* txn, uint32_t flags, THD* thd) {
    const char *orig_proc_info = tokudb_thd_get_proc_info(thd);
    struct txn_progress_info info;
    info.thd = thd;
    int r = txn->commit_with_progress(txn, flags, txn_progress_func, &info);
    if (r != 0) {
        sql_print_error(
            "%s: tried committing transaction %p and got error code %d",
            tokudb_hton_name,
            txn,
            r);
    }
    assert_always(r == 0);
    thd_proc_info(thd, orig_proc_info);
}

static void abort_txn_with_progress(DB_TXN* txn, THD* thd) {
    const char *orig_proc_info = tokudb_thd_get_proc_info(thd);
    struct txn_progress_info info;
    info.thd = thd;
    int r = txn->abort_with_progress(txn, txn_progress_func, &info);
    if (r != 0) {
        sql_print_error(
            "%s: tried aborting transaction %p and got error code %d",
            tokudb_hton_name,
            txn,
            r);
    }
    assert_always(r == 0);
    thd_proc_info(thd, orig_proc_info);
}

static void tokudb_cleanup_handlers(tokudb_trx_data *trx, DB_TXN *txn) {
    LIST *e;
    while ((e = trx->handlers)) {
        trx->handlers = list_delete(trx->handlers, e);
        ha_tokudb *handler = (ha_tokudb *) e->data;
        handler->cleanup_txn(txn);
    }
}

#if MYSQL_VERSION_ID >= 50600
extern "C" enum durability_properties thd_get_durability_property(
    const MYSQL_THD thd);
#endif

// Determine if an fsync is used when a transaction is committed.  
static bool tokudb_sync_on_commit(THD* thd, DB_TXN* txn) {
#if MYSQL_VERSION_ID >= 50600
    // Check the client durability property which is set during 2PC
    if (thd_get_durability_property(thd) == HA_IGNORE_DURABILITY)
        return false;
#endif
#if defined(MARIADB_BASE_VERSION)
    // Check is the txn is prepared and the binlog is open
    if (txn->is_prepared(txn) && mysql_bin_log.is_open())
        return false;
#endif
    if (tokudb::sysvars::fsync_log_period > 0)
        return false;
    return tokudb::sysvars::commit_sync(thd) != 0;
}

static int tokudb_commit(handlerton * hton, THD * thd, bool all) {
    TOKUDB_DBUG_ENTER("%u", all);
    DBUG_PRINT("trans", ("ending transaction %s", all ? "all" : "stmt"));
    tokudb_trx_data *trx = (tokudb_trx_data *) thd_get_ha_data(thd, hton);
    DB_TXN **txn = all ? &trx->all : &trx->stmt;
    DB_TXN *this_txn = *txn;
    if (this_txn) {
        uint32_t syncflag =
            tokudb_sync_on_commit(thd, this_txn) ? 0 : DB_TXN_NOSYNC;
        TOKUDB_TRACE_FOR_FLAGS(
            TOKUDB_DEBUG_TXN,
            "commit trx %u txn %p %" PRIu64 " syncflag %u",
            all,
            this_txn, this_txn->id64(this_txn),
            syncflag);
        // test hook to induce a crash on a debug build
        DBUG_EXECUTE_IF("tokudb_crash_commit_before", DBUG_SUICIDE(););
        tokudb_cleanup_handlers(trx, this_txn);
        commit_txn_with_progress(this_txn, syncflag, thd);
        // test hook to induce a crash on a debug build
        DBUG_EXECUTE_IF("tokudb_crash_commit_after", DBUG_SUICIDE(););
        *txn = NULL;
        trx->sub_sp_level = NULL;
        if (this_txn == trx->sp_level || trx->all == NULL) {
            trx->sp_level = NULL;
        }
    } else {
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_TXN, "nothing to commit %d", all);
    }
    reset_stmt_progress(&trx->stmt_progress);
    TOKUDB_DBUG_RETURN(0);
}

static int tokudb_rollback(handlerton * hton, THD * thd, bool all) {
    TOKUDB_DBUG_ENTER("%u", all);
    DBUG_PRINT("trans", ("aborting transaction %s", all ? "all" : "stmt"));
    tokudb_trx_data *trx = (tokudb_trx_data *) thd_get_ha_data(thd, hton);
    DB_TXN **txn = all ? &trx->all : &trx->stmt;
    DB_TXN *this_txn = *txn;
    if (this_txn) {
        TOKUDB_TRACE_FOR_FLAGS(
            TOKUDB_DEBUG_TXN,
            "rollback %u txn %p %" PRIu64,
            all,
            this_txn, this_txn->id64(this_txn));
        tokudb_cleanup_handlers(trx, this_txn);
        abort_txn_with_progress(this_txn, thd);
        *txn = NULL;
        trx->sub_sp_level = NULL;
        if (this_txn == trx->sp_level || trx->all == NULL) {
            trx->sp_level = NULL;
        }
    } else {
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_TXN, "abort0");
    }
    reset_stmt_progress(&trx->stmt_progress);
    TOKUDB_DBUG_RETURN(0);
}

#if defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA
static bool tokudb_sync_on_prepare(void) {
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "enter");
    // skip sync of log if fsync log period > 0
    if (tokudb::sysvars::fsync_log_period > 0) {
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit");
        return false;
    } else {
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit");
        return true;
    }
}   

static int tokudb_xa_prepare(handlerton* hton, THD* thd, bool all) {
    TOKUDB_DBUG_ENTER("%u", all);
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "enter");
    int r = 0;

    // if tokudb_support_xa is disable, just return
    if (!tokudb::sysvars::support_xa(thd)) {
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit %d", r);
        TOKUDB_DBUG_RETURN(r);
    }

    DBUG_PRINT("trans", ("preparing transaction %s", all ? "all" : "stmt"));
    tokudb_trx_data *trx = (tokudb_trx_data *) thd_get_ha_data(thd, hton);
    DB_TXN* txn = all ? trx->all : trx->stmt;
    if (txn) {
        uint32_t syncflag = tokudb_sync_on_prepare() ? 0 : DB_TXN_NOSYNC;
        TOKUDB_TRACE_FOR_FLAGS(
            TOKUDB_DEBUG_XA,
            "doing txn prepare:%d:%p %" PRIu64,
            all,
            txn, txn->id64(txn));
        // a TOKU_XA_XID is identical to a MYSQL_XID
        TOKU_XA_XID thd_xid;
        thd_get_xid(thd, (MYSQL_XID*) &thd_xid);
        // test hook to induce a crash on a debug build
        DBUG_EXECUTE_IF("tokudb_crash_prepare_before", DBUG_SUICIDE(););
        r = txn->xa_prepare(txn, &thd_xid, syncflag);
        // test hook to induce a crash on a debug build
        DBUG_EXECUTE_IF("tokudb_crash_prepare_after", DBUG_SUICIDE(););

        // XA log entries can be interleaved in the binlog since XA prepare on the master
        // flushes to the binlog.  There can be log entries from different clients pushed
        // into the binlog before XA commit is executed on the master.  Therefore, the slave
        // thread must be able to juggle multiple XA transactions.  Tokudb does this by
        // zapping the client transaction context on the slave when executing the XA prepare
        // and expecting to process XA commit with commit_by_xid (which supplies the XID so
        // that the transaction can be looked up and committed).
        if (r == 0 && all && thd->slave_thread) {
            TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "zap txn context %u", thd_sql_command(thd));
            if (thd_sql_command(thd) == SQLCOM_XA_PREPARE) {
                trx->all = NULL;
                trx->sub_sp_level = NULL;
                trx->sp_level = NULL;
            }
        }
    } else {
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "nothing to prepare %d", all);
    }
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit %d", r);
    TOKUDB_DBUG_RETURN(r);
}

static int tokudb_xa_recover(TOKUDB_UNUSED(handlerton* hton),
                             XID* xid_list,
                             uint len) {
    TOKUDB_DBUG_ENTER("");
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "enter");
    int r = 0;
    if (len == 0 || xid_list == NULL) {
        TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit %d", 0);
        TOKUDB_DBUG_RETURN(0);
    }
    long num_returned = 0;
    r = db_env->txn_xa_recover(
        db_env,
        (TOKU_XA_XID*)xid_list,
        len,
        &num_returned,
        DB_NEXT);
    assert_always(r == 0);
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit %ld", num_returned);
    TOKUDB_DBUG_RETURN((int)num_returned);
}

static int tokudb_commit_by_xid(TOKUDB_UNUSED(handlerton* hton), XID* xid) {
    TOKUDB_DBUG_ENTER("");
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "enter");
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "xid %p", xid);
    int r = 0;
    DB_TXN* txn = NULL;
    TOKU_XA_XID* toku_xid = (TOKU_XA_XID*)xid;

    r = db_env->get_txn_from_xid(db_env, toku_xid, &txn);
    if (r) { goto cleanup; }

    r = txn->commit(txn, 0);
    if (r) { goto cleanup; }

    r = 0;
cleanup:
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit %d", r);
    TOKUDB_DBUG_RETURN(r);
}

static int tokudb_rollback_by_xid(TOKUDB_UNUSED(handlerton* hton), XID* xid) {
    TOKUDB_DBUG_ENTER("");
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "enter");
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "xid %p", xid);
    int r = 0;
    DB_TXN* txn = NULL;
    TOKU_XA_XID* toku_xid = (TOKU_XA_XID*)xid;

    r = db_env->get_txn_from_xid(db_env, toku_xid, &txn);
    if (r) { goto cleanup; }

    r = txn->abort(txn);
    if (r) { goto cleanup; }

    r = 0;
cleanup:
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_XA, "exit %d", r);
    TOKUDB_DBUG_RETURN(r);
}

#endif  // defined(TOKU_INCLUDE_XA) && TOKU_INCLUDE_XA

static int tokudb_savepoint(handlerton * hton, THD * thd, void *savepoint) {
    TOKUDB_DBUG_ENTER("%p", savepoint);
    int error;
    SP_INFO save_info = (SP_INFO)savepoint;
    tokudb_trx_data *trx = (tokudb_trx_data *) thd_get_ha_data(thd, hton);
    if (thd->in_sub_stmt) {
        assert_always(trx->stmt);
        error = txn_begin(
            db_env,
            trx->sub_sp_level,
            &(save_info->txn),
            DB_INHERIT_ISOLATION,
            thd);
        if (error) {
            goto cleanup;
        }
        trx->sub_sp_level = save_info->txn;
        save_info->in_sub_stmt = true;
    } else {
        error = txn_begin(
            db_env,
            trx->sp_level,
            &(save_info->txn),
            DB_INHERIT_ISOLATION,
            thd);
        if (error) {
            goto cleanup;
        }
        trx->sp_level = save_info->txn;
        save_info->in_sub_stmt = false;
    }
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_TXN, "begin txn %p", save_info->txn);
    save_info->trx = trx;
    error = 0;
cleanup:
    TOKUDB_DBUG_RETURN(error);
}

static int tokudb_rollback_to_savepoint(
    handlerton* hton,
    THD* thd,
    void* savepoint) {

    TOKUDB_DBUG_ENTER("%p", savepoint);
    int error;
    SP_INFO save_info = (SP_INFO)savepoint;
    DB_TXN* parent = NULL;
    DB_TXN* txn_to_rollback = save_info->txn;

    tokudb_trx_data* trx = (tokudb_trx_data*)thd_get_ha_data(thd, hton);
    parent = txn_to_rollback->parent;
    TOKUDB_TRACE_FOR_FLAGS(
        TOKUDB_DEBUG_TXN,
        "rollback txn %p",
        txn_to_rollback);
    if (!(error = txn_to_rollback->abort(txn_to_rollback))) {
        if (save_info->in_sub_stmt) {
            trx->sub_sp_level = parent;
        }
        else {
            trx->sp_level = parent;
        }
        error = tokudb_savepoint(hton, thd, savepoint);
    }
    TOKUDB_DBUG_RETURN(error);
}

static int tokudb_release_savepoint(
    handlerton* hton,
    THD* thd,
    void* savepoint) {

    TOKUDB_DBUG_ENTER("%p", savepoint);
    int error = 0;
    SP_INFO save_info = (SP_INFO)savepoint;
    DB_TXN* parent = NULL;
    DB_TXN* txn_to_commit = save_info->txn;

    tokudb_trx_data *trx = (tokudb_trx_data *) thd_get_ha_data(thd, hton);
    parent = txn_to_commit->parent;
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_TXN, "commit txn %p", txn_to_commit);
    DB_TXN *child = txn_to_commit->get_child(txn_to_commit);
    if (child == NULL && !(error = txn_to_commit->commit(txn_to_commit, 0))) {
        if (save_info->in_sub_stmt) {
            trx->sub_sp_level = parent;
        }
        else {
            trx->sp_level = parent;
        }
    }
    save_info->txn = NULL;
    TOKUDB_DBUG_RETURN(error);
}

#if 100000 <= MYSQL_VERSION_ID
static int tokudb_discover_table(handlerton *hton, THD* thd, TABLE_SHARE *ts) {
    uchar *frmblob = 0;
    size_t frmlen;
    int res= tokudb_discover3(
        hton,
        thd,
        ts->db.str,
        ts->table_name.str,
        ts->normalized_path.str,
        &frmblob,
        &frmlen);
    if (!res)
        res= ts->init_from_binary_frm_image(thd, true, frmblob, frmlen);
    
    my_free(frmblob);
    // discover_table should returns HA_ERR_NO_SUCH_TABLE for "not exists"
    return res == ENOENT ? HA_ERR_NO_SUCH_TABLE : res;
}

static int tokudb_discover_table_existence(
    handlerton* hton,
    const char* db,
    const char* name) {

    uchar *frmblob = 0;
    size_t frmlen;
    int res= tokudb_discover(hton, current_thd, db, name, &frmblob, &frmlen);
    my_free(frmblob);
    return res != ENOENT;
}
#endif // 100000 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 100099

#if defined(TOKU_INCLUDE_DISCOVER_FRM) && TOKU_INCLUDE_DISCOVER_FRM
static int tokudb_discover(
    handlerton* hton,
    THD* thd,
    const char* db,
    const char* name,
    uchar** frmblob,
    size_t* frmlen) {

    return tokudb_discover2(hton, thd, db, name, true, frmblob, frmlen);
}

static int tokudb_discover2(
    handlerton* hton,
    THD* thd,
    const char* db,
    const char* name,
    bool translate_name,
    uchar** frmblob,
    size_t*frmlen) {

    char path[FN_REFLEN + 1];
    build_table_filename(
        path,
        sizeof(path) - 1,
        db,
        name,
        "",
        translate_name ? 0 : FN_IS_TMP);
    return tokudb_discover3(hton, thd, db, name, path, frmblob, frmlen);
}

static int tokudb_discover3(TOKUDB_UNUSED(handlerton* hton),
                            THD* thd,
                            const char* db,
                            const char* name,
                            char* path,
                            uchar** frmblob,
                            size_t* frmlen) {
    TOKUDB_DBUG_ENTER("%s %s %s", db, name, path);
    int error;
    DB* status_db = NULL;
    DB_TXN* txn = NULL;
    HA_METADATA_KEY curr_key = hatoku_frm_data;
    DBT key = {};
    DBT value = {};
    bool do_commit = false;

#if 100000 <= MYSQL_VERSION_ID
    tokudb_trx_data* trx = (tokudb_trx_data*)thd_get_ha_data(thd, tokudb_hton);
    if (thd_sql_command(thd) == SQLCOM_CREATE_TABLE &&
        trx &&
        trx->sub_sp_level) {
        do_commit = false;
        txn = trx->sub_sp_level;
    } else {
        error = txn_begin(db_env, 0, &txn, 0, thd);
        if (error) { goto cleanup; }
        do_commit = true;
    }
#else
    error = txn_begin(db_env, 0, &txn, 0, thd);
    if (error) { goto cleanup; }
    do_commit = true;
#endif

    error = open_status_dictionary(&status_db, path, txn);
    if (error) { goto cleanup; }

    key.data = &curr_key;
    key.size = sizeof(curr_key);

    error = status_db->getf_set(
        status_db, 
        txn,
        0,
        &key, 
        smart_dbt_callback_verify_frm,
        &value);
    if (error) {
        goto cleanup;
    }

    *frmblob = (uchar *)value.data;
    *frmlen = value.size;

    error = 0;
cleanup:
    if (status_db) {
        status_db->close(status_db,0);
    }
    if (do_commit && txn) {
        commit_txn(txn, 0);
    }
    TOKUDB_DBUG_RETURN(error);
}
#endif  // defined(TOKU_INCLUDE_DISCOVER_FRM) && TOKU_INCLUDE_DISCOVER_FRM


#define STATPRINT(legend, val) if (legend != NULL && val != NULL) \
    stat_print(thd, \
        tokudb_hton_name, \
        strlen(tokudb_hton_name), \
        legend, \
        strlen(legend), \
        val, \
        strlen(val))

extern sys_var* intern_find_sys_var(
    const char* str,
    uint length,
    bool no_error);

static bool tokudb_show_engine_status(THD * thd, stat_print_fn * stat_print) {
    TOKUDB_DBUG_ENTER("");
    int error;
    uint64_t panic;
    const int panic_string_len = 1024;
    char panic_string[panic_string_len] = {'\0'};
    uint64_t num_rows;
    uint64_t max_rows;
    fs_redzone_state redzone_state;
    const int bufsiz = 1024;
    char buf[bufsiz];

#if MYSQL_VERSION_ID < 50500
    {
        sys_var* version = intern_find_sys_var("version", 0, false);
        snprintf(
            buf,
            bufsiz,
            "%s",
            version->value_ptr(thd,
            (enum_var_type)0,
            (LEX_STRING*)NULL));
        STATPRINT("Version", buf);
    }
#endif
    error = db_env->get_engine_status_num_rows (db_env, &max_rows);
    TOKU_ENGINE_STATUS_ROW_S mystat[max_rows];
    error = db_env->get_engine_status(
        db_env,
        mystat,
        max_rows,
        &num_rows,
        &redzone_state,
        &panic,
        panic_string,
        panic_string_len,
        TOKU_ENGINE_STATUS);

    if (strlen(panic_string)) {
        STATPRINT("Environment panic string", panic_string);
    }
    if (error == 0) {
        if (panic) {
            snprintf(buf, bufsiz, "%" PRIu64, panic);
            STATPRINT("Environment panic", buf);
        }
        
        if(redzone_state == FS_BLOCKED) {
            STATPRINT(
                "*** URGENT WARNING ***", "FILE SYSTEM IS COMPLETELY FULL");
            snprintf(buf, bufsiz, "FILE SYSTEM IS COMPLETELY FULL");
        } else if (redzone_state == FS_GREEN) {
            snprintf(
                buf,
                bufsiz,
                "more than %d percent of total file system space",
                2 * tokudb::sysvars::fs_reserve_percent);
        } else if (redzone_state == FS_YELLOW) {
            snprintf(
                buf,
                bufsiz,
                "*** WARNING *** FILE SYSTEM IS GETTING FULL (less than %d "
                "percent free)",
                2 * tokudb::sysvars::fs_reserve_percent);
        } else if (redzone_state == FS_RED){
            snprintf(
                buf,
                bufsiz,
                "*** WARNING *** FILE SYSTEM IS GETTING VERY FULL (less than "
                "%d percent free): INSERTS ARE PROHIBITED",
                tokudb::sysvars::fs_reserve_percent);
        } else {
            snprintf(
                buf,
                bufsiz,
                "information unavailable, unknown redzone state %d",
                redzone_state);
        }
        STATPRINT ("disk free space", buf);

        for (uint64_t row = 0; row < num_rows; row++) {
            switch (mystat[row].type) {
            case FS_STATE:
                snprintf(buf, bufsiz, "%" PRIu64 "", mystat[row].value.num);
                break;
            case UINT64:
                snprintf(buf, bufsiz, "%" PRIu64 "", mystat[row].value.num);
                break;
            case CHARSTR:
                snprintf(buf, bufsiz, "%s", mystat[row].value.str);
                break;
            case UNIXTIME: {
                time_t t = mystat[row].value.num;
                char tbuf[26];
                snprintf(buf, bufsiz, "%.24s", ctime_r(&t, tbuf));
                break;
            }
            case TOKUTIME: {
                double t = tokutime_to_seconds(mystat[row].value.num);
                snprintf(buf, bufsiz, "%.6f", t);
                break;
            }
            case PARCOUNT: {
                uint64_t v = read_partitioned_counter(
                    mystat[row].value.parcount);
                snprintf(buf, bufsiz, "%" PRIu64, v);
                break;
            }
            case DOUBLE:
                snprintf(buf, bufsiz, "%.6f", mystat[row].value.dnum);
                break;
            default:
                snprintf(
                    buf,
                    bufsiz,
                    "UNKNOWN STATUS TYPE: %d",
                    mystat[row].type);
                break;
            }
            STATPRINT(mystat[row].legend, buf);
        }
        uint64_t bytes_inserted = read_partitioned_counter(
            tokudb_primary_key_bytes_inserted);
        snprintf(buf, bufsiz, "%" PRIu64, bytes_inserted);
        STATPRINT("handlerton: primary key bytes inserted", buf);
    }  
    if (error) { my_errno = error; }
    TOKUDB_DBUG_RETURN(error);
}

void tokudb_checkpoint_lock(THD * thd) {
    int error;
    const char *old_proc_info;
    tokudb_trx_data* trx = (tokudb_trx_data*)thd_get_ha_data(thd, tokudb_hton);
    if (!trx) {
        error = create_tokudb_trx_data_instance(&trx);
        //
        // can only fail due to memory allocation, so ok to assert
        //
        assert_always(!error);
        thd_set_ha_data(thd, tokudb_hton, trx);
    }
    
    if (trx->checkpoint_lock_taken) {
        goto cleanup;
    }
    //
    // This can only fail if environment is not created, which is not possible
    // in handlerton
    //
    old_proc_info = tokudb_thd_get_proc_info(thd);
    thd_proc_info(thd, "Trying to grab checkpointing lock.");
    error = db_env->checkpointing_postpone(db_env);
    assert_always(!error);
    thd_proc_info(thd, old_proc_info);

    trx->checkpoint_lock_taken = true;
cleanup:
    return;
}

void tokudb_checkpoint_unlock(THD * thd) {
    int error;
    const char *old_proc_info;
    tokudb_trx_data* trx = (tokudb_trx_data*)thd_get_ha_data(thd, tokudb_hton);
    if (!trx) {
        error = 0;
        goto  cleanup;
    }
    if (!trx->checkpoint_lock_taken) {
        error = 0;
        goto  cleanup;
    }
    //
    // at this point, we know the checkpoint lock has been taken
    //
    old_proc_info = tokudb_thd_get_proc_info(thd);
    thd_proc_info(thd, "Trying to release checkpointing lock.");
    error = db_env->checkpointing_resume(db_env);
    assert_always(!error);
    thd_proc_info(thd, old_proc_info);

    trx->checkpoint_lock_taken = false;
    
cleanup:
    return;
}

static bool tokudb_show_status(
    TOKUDB_UNUSED(handlerton* hton),
    THD* thd,
    stat_print_fn* stat_print,
    enum ha_stat_type stat_type) {

    switch (stat_type) {
    case HA_ENGINE_STATUS:
        return tokudb_show_engine_status(thd, stat_print);
        break;
    default:
        break;
    }
    return false;
}

#if defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) && \
    TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
static void tokudb_handle_fatal_signal(
    TOKUDB_UNUSED(handlerton* hton),
    TOKUDB_UNUSD(THD* thd),
    int sig) {

    if (tokudb_gdb_on_fatal) {
        db_env_try_gdb_stack_trace(tokudb_gdb_path);
    }
}
#endif  // defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) &&
        // TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL

static void tokudb_print_error(TOKUDB_UNUSED(const DB_ENV* db_env),
                               const char* db_errpfx,
                               const char* buffer) {
    sql_print_error("%s: %s", db_errpfx, buffer);
}

static void tokudb_cleanup_log_files(void) {
    TOKUDB_DBUG_ENTER("");
    char **names;
    int error;

    if ((error = db_env->txn_checkpoint(db_env, 0, 0, 0)))
        my_error(ER_ERROR_DURING_CHECKPOINT, MYF(0), error);

    if ((error = db_env->log_archive(db_env, &names, 0)) != 0) {
        DBUG_PRINT("error", ("log_archive failed (error %d)", error));
        db_env->err(db_env, error, "log_archive");
        DBUG_VOID_RETURN;
    }

    if (names) {
        char **np;
        for (np = names; *np; ++np) {
#if 1
            if (TOKUDB_UNLIKELY(tokudb::sysvars::debug))
                TOKUDB_TRACE("cleanup:%s", *np);
#else
            my_delete(*np, MYF(MY_WME));
#endif
        }

        free(names);
    }

    DBUG_VOID_RETURN;
}

// Split ./database/table-dictionary into database, table and dictionary strings
void tokudb_split_dname(
    const char* dname,
    String& database_name,
    String& table_name,
    String& dictionary_name) {

    const char *splitter = strchr(dname, '/');
    if (splitter) {
        const char *database_ptr = splitter+1;
        const char *table_ptr = strchr(database_ptr, '/');
        if (table_ptr) {
            database_name.append(database_ptr, table_ptr - database_ptr);
            table_ptr += 1;
            const char *dictionary_ptr = strchr(table_ptr, '-');
            if (dictionary_ptr) {
                table_name.append(table_ptr, dictionary_ptr - table_ptr);
                dictionary_ptr += 1;
                dictionary_name.append(dictionary_ptr);
            } else {
                table_name.append(table_ptr);
            }
        } else {
            database_name.append(database_ptr);
        }
    }
}

struct st_mysql_storage_engine tokudb_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION
};

#if defined(TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING) && \
    TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING
struct tokudb_search_txn_extra {
    bool match_found;
    uint64_t match_txn_id;
    uint64_t match_client_id;
};

static int tokudb_search_txn_callback(
    DB_TXN* txn,
    iterate_row_locks_callback iterate_locks,
    void* locks_extra,
    void* extra) {

    uint64_t txn_id = txn->id64(txn);
    uint64_t client_id;
    void *client_extra;
    txn->get_client_id(txn, &client_id, &client_extra);
    struct tokudb_search_txn_extra* e =
        reinterpret_cast<struct tokudb_search_txn_extra*>(extra);
    if (e->match_txn_id == txn_id) {
        e->match_found = true;
        e->match_client_id = client_id;
        return 1;
    }
    return 0;
}

static bool tokudb_txn_id_to_client_id(
    THD* thd,
    uint64_t blocking_txnid,
    uint64_t* blocking_client_id) {

    struct tokudb_search_txn_extra e = {
        false,
        blocking_txnid,
        0
    };
    db_env->iterate_live_transactions(db_env, tokudb_search_txn_callback, &e);
    if (e.match_found) {
        *blocking_client_id = e.match_client_id;
    }
    return e.match_found;
}
#endif  // defined(TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING) &&
        // TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING

static void tokudb_pretty_key(
    const DBT* key,
    const char* default_key,
    String* out) {

    if (key->data == NULL) {
        out->append(default_key);
    } else {
        bool do_hexdump = true;
        if (do_hexdump) {
            // hexdump the key
            const unsigned char* data =
                reinterpret_cast<const unsigned char*>(key->data);
            for (size_t i = 0; i < key->size; i++) {
                char str[3];
                snprintf(str, sizeof str, "%2.2x", data[i]);
                out->append(str);
            }
        }
    }
}

void tokudb_pretty_left_key(const DBT* key, String* out) {
    tokudb_pretty_key(key, "-infinity", out);
}

void tokudb_pretty_right_key(const DBT* key, String* out) {
    tokudb_pretty_key(key, "+infinity", out);
}

const char* tokudb_get_index_name(DB* db) {
    if (db != NULL) {
        return db->get_dname(db);
    } else {
        return "$ydb_internal";
    }
}

static int tokudb_equal_key(const DBT *left_key, const DBT *right_key) {
    if (left_key->data == NULL || right_key->data == NULL ||
        left_key->size != right_key->size)
        return 0;
    else
        return memcmp(left_key->data, right_key->data, left_key->size) == 0;
}

static void tokudb_lock_timeout_callback(
    DB* db,
    uint64_t requesting_txnid,
    const DBT* left_key,
    const DBT* right_key,
    uint64_t blocking_txnid) {

    THD* thd = current_thd;
    if (!thd)
        return;
    ulong lock_timeout_debug = tokudb::sysvars::lock_timeout_debug(thd);
    if (lock_timeout_debug != 0) {
        // generate a JSON document with the lock timeout info
        String log_str;
        log_str.append("{");
        uint64_t mysql_thread_id = thd->thread_id;
        log_str.append("\"mysql_thread_id\":");
        log_str.append_ulonglong(mysql_thread_id);
        log_str.append(", \"dbname\":");
        log_str.append("\"");
        log_str.append(tokudb_get_index_name(db));
        log_str.append("\"");
        log_str.append(", \"requesting_txnid\":");
        log_str.append_ulonglong(requesting_txnid);
        log_str.append(", \"blocking_txnid\":");
        log_str.append_ulonglong(blocking_txnid);
        if (tokudb_equal_key(left_key, right_key)) {
            String key_str;
            tokudb_pretty_key(left_key, "?", &key_str);
            log_str.append(", \"key\":");
            log_str.append("\"");
            log_str.append(key_str);
            log_str.append("\"");
        } else {
            String left_str;
            tokudb_pretty_left_key(left_key, &left_str);
            log_str.append(", \"key_left\":");
            log_str.append("\"");
            log_str.append(left_str);
            log_str.append("\"");
            String right_str;
            tokudb_pretty_right_key(right_key, &right_str);
            log_str.append(", \"key_right\":");
            log_str.append("\"");
            log_str.append(right_str);
            log_str.append("\"");
        }
        log_str.append("}");
        // set last_lock_timeout
        if (lock_timeout_debug & 1) {
            char* old_lock_timeout = tokudb::sysvars::last_lock_timeout(thd);
            char* new_lock_timeout =
                tokudb::memory::strdup(log_str.c_ptr(), MY_FAE);
            tokudb::sysvars::set_last_lock_timeout(thd, new_lock_timeout);
#if defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG
            mutex_t_lock(tokudb_map_mutex);
            struct tokudb_map_pair old_key = {thd, old_lock_timeout};
            tree_delete(&tokudb_map, &old_key, sizeof old_key, NULL);
            struct tokudb_map_pair new_key = {thd, new_lock_timeout};
            tree_insert(&tokudb_map, &new_key, sizeof new_key, NULL);
            mutex_t_unlock(tokudb_map_mutex);
#endif  // defined(TOKU_THDVAR_MEMALLOC_BUG) && TOKU_THDVAR_MEMALLOC_BUG
            tokudb::memory::free(old_lock_timeout);
        }
        // dump to stderr
        if (lock_timeout_debug & 2) {
            sql_print_error(
                "%s: lock timeout %s",
                tokudb_hton_name,
                log_str.c_ptr());
            LEX_STRING *qs = thd_query_string(thd);
            sql_print_error(
                "%s: requesting_thread_id:%" PRIu64 " q:%.*s",
                tokudb_hton_name,
                mysql_thread_id,
                (int)qs->length,
                qs->str);
#if defined(TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING) && \
    TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING
            uint64_t blocking_thread_id = 0;
            if (tokudb_txn_id_to_client_id(
                    thd,
                    blocking_txnid,
                    &blocking_thread_id)) {

                String blocking_qs;
                if (get_thread_query_string(
                        blocking_thread_id,
                        blocking_qs) == 0) {

                    sql_print_error(
                        "%s: blocking_thread_id:%" PRIu64 " q:%.*s",
                        tokudb_hton_name,
                        blocking_thread_id,
                        blocking_qs.length(),
                        blocking_qs.c_ptr());
                }
            }
#endif  // defined(TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING) &&
        // TOKU_INCLUDE_LOCK_TIMEOUT_QUERY_STRING
        }
    }
}

extern "C" int thd_rpl_deadlock_check(MYSQL_THD thd, MYSQL_THD other_thd);

struct tokudb_search_txn_thd {
    bool match_found;
    uint64_t match_txn_id;
    THD *match_client_thd;
};

static int tokudb_search_txn_thd_callback(
    DB_TXN* txn,
    iterate_row_locks_callback iterate_locks,
    void* locks_extra,
    void* extra) {

    uint64_t txn_id = txn->id64(txn);
    uint64_t client_id;
    void *client_extra;
    txn->get_client_id(txn, &client_id, &client_extra);
    struct tokudb_search_txn_thd* e =
        reinterpret_cast<struct tokudb_search_txn_thd*>(extra);
    if (e->match_txn_id == txn_id) {
        e->match_found = true;
        e->match_client_thd = reinterpret_cast<THD *>(client_extra);
        return 1;
    }
    return 0;
}

static bool tokudb_txn_id_to_thd(
    uint64_t txnid,
    THD **out_thd) {

    struct tokudb_search_txn_thd e = {
        false,
        txnid,
        0
    };
    db_env->iterate_live_transactions(db_env, tokudb_search_txn_thd_callback, &e);
    if (e.match_found) {
        *out_thd = e.match_client_thd;
    }
    return e.match_found;
}

static void tokudb_lock_wait_needed_callback(
    void *arg,
    uint64_t requesting_txnid,
    uint64_t blocking_txnid) {

    THD *requesting_thd;
    THD *blocking_thd;
    if (tokudb_txn_id_to_thd(requesting_txnid, &requesting_thd) &&
        tokudb_txn_id_to_thd(blocking_txnid, &blocking_thd)) {
        thd_rpl_deadlock_check (requesting_thd, blocking_thd);
    }
}

// Retrieves variables for information_schema.global_status.
// Names (columnname) are automatically converted to upper case,
// and prefixed with "TOKUDB_"
static int show_tokudb_vars(TOKUDB_UNUSED(THD* thd),
                            SHOW_VAR* var,
                            TOKUDB_UNUSED(char* buff)) {
    TOKUDB_DBUG_ENTER("");

    int error;
    uint64_t panic;
    const int panic_string_len = 1024;
    char panic_string[panic_string_len] = {'\0'};
    fs_redzone_state redzone_state;

    uint64_t num_rows;
    error = db_env->get_engine_status(
        db_env,
        toku_global_status_rows,
        toku_global_status_max_rows,
        &num_rows,
        &redzone_state,
        &panic,
        panic_string,
        panic_string_len,
        TOKU_GLOBAL_STATUS);
    //TODO: Maybe do something with the panic output?
    if (error == 0) {
        assert_always(num_rows <= toku_global_status_max_rows);
        //TODO: Maybe enable some of the items here: (copied from engine status

        //TODO: (optionally) add redzone state, panic, panic string, etc.
        //Right now it's being ignored.

        for (uint64_t row = 0; row < num_rows; row++) {
            SHOW_VAR &status_var = toku_global_status_variables[row];
            TOKU_ENGINE_STATUS_ROW_S &status_row = toku_global_status_rows[row];

            status_var.name = status_row.columnname;
            switch (status_row.type) {
            case FS_STATE:
            case UINT64:
                status_var.type = SHOW_LONGLONG;
                status_var.value = (char*)&status_row.value.num;
                break;
            case CHARSTR:
                status_var.type = SHOW_CHAR;
                status_var.value = (char*)status_row.value.str;
                break;
            case UNIXTIME: {
                status_var.type = SHOW_CHAR;
                time_t t = status_row.value.num;
                char tbuf[26];
                // Reuse the memory in status_row. (It belongs to us).
                snprintf(
                    status_row.value.datebuf,
                    sizeof(status_row.value.datebuf),
                    "%.24s",
                    ctime_r(&t, tbuf));
                status_var.value = (char*)&status_row.value.datebuf[0];
                break;
            }
            case TOKUTIME:
                status_var.type = SHOW_DOUBLE;
                // Reuse the memory in status_row. (It belongs to us).
                status_row.value.dnum = tokutime_to_seconds(status_row.value.num);
                status_var.value = (char*)&status_row.value.dnum;
                break;
            case PARCOUNT: {
                status_var.type = SHOW_LONGLONG;
                uint64_t v = read_partitioned_counter(status_row.value.parcount);
                // Reuse the memory in status_row. (It belongs to us).
                status_row.value.num = v;
                status_var.value = (char*)&status_row.value.num;
                break;
            }
            case DOUBLE:
                status_var.type = SHOW_DOUBLE;
                status_var.value = (char*) &status_row.value.dnum;
                break;
            default:
                status_var.type = SHOW_CHAR;
                // Reuse the memory in status_row.datebuf. (It belongs to us).
                // UNKNOWN TYPE: %d fits in 26 bytes (sizeof datebuf) for any integer.
                snprintf(
                    status_row.value.datebuf,
                    sizeof(status_row.value.datebuf),
                    "UNKNOWN TYPE: %d",
                    status_row.type);
                status_var.value = (char*)&status_row.value.datebuf[0];
                break;
            }
        }
        // Sentinel value at end.
        toku_global_status_variables[num_rows].type = SHOW_LONG;
        toku_global_status_variables[num_rows].value = (char*)NullS;
        toku_global_status_variables[num_rows].name = (char*)NullS;

        var->type= SHOW_ARRAY;
        var->value= (char *) toku_global_status_variables;
    }
    if (error) { my_errno = error; }
    TOKUDB_DBUG_RETURN(error);
}

static SHOW_VAR toku_global_status_variables_export[]= {
    {"Tokudb", (char*)&show_tokudb_vars, SHOW_FUNC},
    {NullS, NullS, SHOW_LONG}
};

#ifdef MARIA_PLUGIN_INTERFACE_VERSION
maria_declare_plugin(tokudb) 
#else
mysql_declare_plugin(tokudb) 
#endif
    {
        MYSQL_STORAGE_ENGINE_PLUGIN,
        &tokudb_storage_engine,
        tokudb_hton_name,
        "Percona",
        "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
        PLUGIN_LICENSE_GPL,
        tokudb_init_func,          /* plugin init */
        tokudb_done_func,          /* plugin deinit */
        TOKUDB_PLUGIN_VERSION,
        toku_global_status_variables_export,  /* status variables */
        tokudb::sysvars::system_variables,   /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
        tokudb::sysvars::version,
        MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
        NULL,                      /* config options */
        0,                         /* flags */
#endif
    },
    tokudb::information_schema::trx,
    tokudb::information_schema::lock_waits,
    tokudb::information_schema::locks,
    tokudb::information_schema::file_map,
    tokudb::information_schema::fractal_tree_info,
    tokudb::information_schema::fractal_tree_block_map,
    tokudb::information_schema::background_job_status
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
maria_declare_plugin_end;
#else
mysql_declare_plugin_end;
#endif
