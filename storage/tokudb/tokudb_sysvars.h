/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
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

#ifndef _TOKUDB_SYSVARS_H
#define _TOKUDB_SYSVARS_H

#if defined(TOKU_INCLUDE_OPTION_STRUCTS) && TOKU_INCLUDE_OPTION_STRUCTS
struct ha_table_option_struct {
    uint row_format;
};

struct ha_index_option_struct {
    bool clustering;
};

static inline bool key_is_clustering(const KEY *key) {
    return (key->flags & HA_CLUSTERING) || (key->option_struct && key->option_struct->clustering);
}

#else

static inline bool key_is_clustering(const KEY *key) {
    return key->flags & HA_CLUSTERING;
}
#endif  // defined(TOKU_INCLUDE_OPTION_STRUCTS) && TOKU_INCLUDE_OPTION_STRUCTS

namespace tokudb {
namespace sysvars {

enum analyze_mode_t {
    TOKUDB_ANALYZE_STANDARD = 0,
    TOKUDB_ANALYZE_RECOUNT_ROWS = 1,
    TOKUDB_ANALYZE_CANCEL = 2
};

enum empty_scan_mode_t {
    TOKUDB_EMPTY_SCAN_DISABLED = 0,
    TOKUDB_EMPTY_SCAN_LR = 1,
    TOKUDB_EMPTY_SCAN_RL = 2,
};

enum row_format_t {
    SRV_ROW_FORMAT_UNCOMPRESSED = 0,
    SRV_ROW_FORMAT_ZLIB = 1,
    SRV_ROW_FORMAT_SNAPPY = 2,
    SRV_ROW_FORMAT_QUICKLZ = 3,
    SRV_ROW_FORMAT_LZMA = 4,
    SRV_ROW_FORMAT_FAST = 5,
    SRV_ROW_FORMAT_SMALL = 6,
    SRV_ROW_FORMAT_DEFAULT = 7
};

#define DEFAULT_TOKUDB_CLEANER_ITERATIONS 5
#define DEFAULT_TOKUDB_CLEANER_PERIOD 1
#define DEFAULT_TOKUDB_KILLED_TIME 4000     // milliseconds
#define DEFAULT_TOKUDB_LOCK_TIMEOUT 4000    // milliseconds


// globals
extern ulonglong    cache_size;
extern uint         cachetable_pool_threads;
extern int          cardinality_scale_percent;
extern my_bool      checkpoint_on_flush_logs;
extern uint         checkpoint_pool_threads;
extern uint         checkpointing_period;
extern ulong        cleaner_iterations;
extern ulong        cleaner_period;
extern uint         client_pool_threads;
extern my_bool      compress_buffers_before_eviction;
extern char*        data_dir;
extern ulong        debug;
extern my_bool      directio;
extern my_bool      enable_partial_eviction;
extern int          fs_reserve_percent;
extern uint         fsync_log_period;
extern char*        log_dir;
extern ulonglong    max_lock_memory;
extern uint         read_status_frequency;
extern my_bool      strip_frm_data;
extern char*        tmp_dir;
extern uint         write_status_frequency;
extern my_bool      dir_per_db;
extern char*        version;

#if defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) && \
    TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL
extern char*        gdb_path;
extern my_bool      gdb_on_fatal;
#endif  // defined(TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL) &&
        // TOKU_INCLUDE_HANDLERTON_HANDLE_FATAL_SIGNAL

extern my_bool         check_jemalloc;

#if defined(TOKUDB_DEBUG)
// used to control background job manager
extern my_bool      debug_pause_background_job_manager;
#endif // defined(TOKUDB_DEBUG)

// session/thread
my_bool     alter_print_error(THD* thd);
double      analyze_delete_fraction(THD* thd);
my_bool     analyze_in_background(THD* thd);
analyze_mode_t analyze_mode(THD* thd);
ulonglong   analyze_throttle(THD* thd);
ulonglong   analyze_time(THD* thd);
ulonglong   auto_analyze(THD* thd);
uint        block_size(THD* thd);
my_bool     bulk_fetch(THD* thd);
my_bool     commit_sync(THD* thd);
my_bool     create_index_online(THD* thd);
my_bool     disable_hot_alter(THD* thd);
my_bool     disable_prefetching(THD* thd);
my_bool     disable_slow_alter(THD* thd);
my_bool     enable_fast_update(THD* thd);
my_bool     enable_fast_upsert(THD* thd);
empty_scan_mode_t empty_scan(THD* thd);
uint        fanout(THD* thd);
my_bool     hide_default_row_format(THD* thd);
ulonglong   killed_time(THD* thd);
my_bool     load_save_space(THD* thd);
char*       last_lock_timeout(THD* thd);
void        set_last_lock_timeout(THD* thd, char* last);
ulonglong   loader_memory_size(THD* thd);
ulonglong   lock_timeout(THD* thd);
uint        lock_timeout_debug(THD* thd);
double      optimize_index_fraction(THD* thd);
const char* optimize_index_name(THD* thd);
ulonglong   optimize_throttle(THD* thd);
uint        pk_insert_mode(THD* thd);
void        set_pk_insert_mode(THD* thd, uint mode);
my_bool     prelock_empty(THD* thd);
uint        read_block_size(THD* thd);
uint        read_buf_size(THD* thd);
row_format_t row_format(THD *thd);
#if defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
my_bool     rpl_check_readonly(THD* thd);
my_bool     rpl_lookup_rows(THD* thd);
ulonglong   rpl_lookup_rows_delay(THD* thd);
my_bool     rpl_unique_checks(THD* thd);
ulonglong   rpl_unique_checks_delay(THD* thd);
#endif // defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
my_bool     support_xa(THD* thd);

extern st_mysql_sys_var* system_variables[];

#if TOKU_INCLUDE_OPTION_STRUCTS
extern ha_create_table_option tokudb_table_options[];
extern ha_create_table_option tokudb_index_options[];
#endif

} // namespace sysvars
} // namespace tokudb

#endif // _TOKUDB_SYSVARS_H
