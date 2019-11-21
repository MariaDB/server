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

#ifndef _HA_TOKUDB_H
#define _HA_TOKUDB_H

#include "hatoku_hton.h"
#include "hatoku_cmp.h"
#include "tokudb_background.h"

#define HA_TOKU_ORIG_VERSION 4
#define HA_TOKU_VERSION 4
//
// no capabilities yet
//
#define HA_TOKU_CAP 0

class ha_tokudb;

typedef struct loader_context {
    THD* thd;
    char write_status_msg[1024];
    ha_tokudb* ha;
} *LOADER_CONTEXT;

//
// This class stores table information that is to be shared
// among all ha_tokudb objects.
// There is one instance per table, shared among handlers.
// Some of the variables here are the DB* pointers to indexes,
// and auto increment information.
//
// When the last user releases it's reference on the share,
// it closes all of its database handles and releases all info
// The share instance stays around though so some data can be transiently
// kept across open-close-open-close cycles. These data will be explicitly
// noted below.
//
class TOKUDB_SHARE {
public:
    enum share_state_t {
        CLOSED = 0,
        OPENED = 1,
        ERROR = 2
    };

    // one time, start up init
    static void static_init();

    // one time, shutdown destroy
    static void static_destroy();

    // retuns a locked, properly reference counted share
    // callers must check to ensure share is in correct state for callers use
    // and unlock the share.
    // if create_new is set, a new "CLOSED" share will be created if one
    // doesn't exist, otherwise will return NULL if an existing is not found.
    static TOKUDB_SHARE* get_share(
        const char* table_name,
        THR_LOCK_DATA* data,
        bool create_new);

    // removes a share entirely from the pool, call to rename/deleta a table
    // caller must hold ddl_mutex on this share and the share MUST have
    // exactly 0 _use_count
    static void drop_share(TOKUDB_SHARE* share);

    // returns state string for logging/reporting
    static const char* get_state_string(share_state_t state);

    void* operator new(size_t sz);
    void operator delete(void* p);

    TOKUDB_SHARE();

    // increases the ref count and waits for any currently executing state
    // transition to complete
    // returns current state and leaves share locked
    // callers must check to ensure share is in correct state for callers use
    // and unlock the share.
    share_state_t addref();

    // decreases the ref count and potentially closes the share
    // caller must not have ownership of mutex, will lock and release
    int release();

    // returns the current use count
    // no locking requirements
    inline int use_count() const;

    // locks the share
    inline void lock() const;

    // unlocks the share
    inline void unlock() const;

    // returns the current state of the share
    // no locking requirements
    inline share_state_t state() const;

    // sets the state of the share
    // caller must hold mutex on this share
    inline void set_state(share_state_t state);

    // returns the full MySQL table name of the table ex:
    // ./database/table
    // no locking requirements
    inline const char* full_table_name() const;

    // returns the strlen of the full table name
    // no locking requirements
    inline uint full_table_name_length() const;

    // returns the parsed database name this table resides in
    // no locking requirements
    inline const char* database_name() const;

    // returns the strlen of the database name
    // no locking requirements
    inline uint database_name_length() const;

    // returns the parsed table name of this table
    // no locking requirements
    inline const char* table_name() const;

    // returns the strlen of the the table name
    // no locking requirements
    inline uint table_name_length() const;

    // sets the estimated number of rows in the table
    // should be called only during share initialization and info call
    // caller must hold mutex on this share unless specified by 'locked'
    inline void set_row_count(uint64_t rows, bool locked);

    // updates tracked row count and ongoing table change delta tracking
    // called from any ha_tokudb operation that inserts/modifies/deletes rows
    // may spawn background analysis if enabled, allowed and threshold hit
    // caller must not have ownership of mutex, will lock and release
    void update_row_count(
        THD* thd,
        uint64_t added,
        uint64_t deleted,
        uint64_t updated);

    // returns the current row count estimate
    // no locking requirements
    inline ha_rows row_count() const;

    // initializes cardinality statistics, takes ownership of incoming buffer
    // caller must hold mutex on this share
    inline void init_cardinality_counts(
        uint32_t rec_per_keys,
        uint64_t* rec_per_key);

    // update the cardinality statistics. number of records must match
    // caller must hold mutex on this share
    inline void update_cardinality_counts(
        uint32_t rec_per_keys,
        const uint64_t* rec_per_key);

    // disallow any auto analysis from taking place
    // caller must hold mutex on this share
    inline void disallow_auto_analysis();

    // allow any auto analysis to take place
    // pass in true for 'reset_deltas' to reset delta counting to 0
    // caller must hold mutex on this share
    inline void allow_auto_analysis(bool reset_deltas);

    // cancels all background jobs for this share
    // no locking requirements
    inline void cancel_background_jobs() const;

    // copies cardinality statistics into TABLE counter set
    // caller must not have ownership of mutex, will lock and release
    void set_cardinality_counts_in_table(TABLE* table);

    // performs table analysis on underlying indices and produces estimated
    // cardinality statistics.
    // on success updates cardinality counts in status database and this share
    // MUST pass a valid THD to access session variables.
    // MAY pass txn. If txn is passed, assumes an explicit user scheduled
    // ANALYZE and not an auto ANALYZE resulting from delta threshold
    // uses session variables:
    //  tokudb_analyze_in_background, tokudb_analyze_throttle,
    //  tokudb_analyze_time, and tokudb_analyze_delete_fraction
    // caller must hold mutex on this share
    int analyze_standard(THD* thd, DB_TXN* txn);

    // performs table scan and updates the internal FT logical row count value
    // on success also updates share row count estimate.
    // MUST pass a valid THD to access session variables.
    // MAY pass txn. If txn is passed, assumes an explicit user scheduled
    // uses session variables:
    //  tokudb_analyze_in_background, and tokudb_analyze_throttle
    // caller must not have ownership of mutex, will lock and release
    int analyze_recount_rows(THD* thd, DB_TXN* txn);

public:
    //*********************************
    // Destroyed and recreated on open-close-open
    ulonglong auto_ident;
    ulonglong last_auto_increment, auto_inc_create_value;

    // estimate on number of rows added in the process of a locked tables
    // this is so we can better estimate row count during a lock table
    ha_rows rows_from_locked_table;
    DB* status_block;

    // DB that is indexed on the primary key
    DB* file;

    // array of all DB's that make up table, includes DB that
    // is indexed on the primary key, add 1 in case primary
    // key is hidden
    DB* key_file[MAX_KEY + 1];
    uint status, version, capabilities;
    uint ref_length;

    // whether table has an auto increment column
    bool has_auto_inc;

    // index of auto increment column in table->field, if auto_inc exists
    uint ai_field_index;

    // whether the primary key has a string
    bool pk_has_string;

    KEY_AND_COL_INFO kc_info;

    // key info copied from TABLE_SHARE, used by background jobs that have no
    // access to a handler instance
    uint _keys;
    uint _max_key_parts;
    struct key_descriptor_t {
        uint _parts;
        bool _is_unique;
        char* _name;
    };
    key_descriptor_t* _key_descriptors;

    // we want the following optimization for bulk loads, if the table is empty, 
    // attempt to grab a table lock. emptiness check can be expensive, 
    // so we try it once for a table. After that, we keep this variable around 
    // to tell us to not try it again.
    bool try_table_lock;

    bool has_unique_keys;
    bool replace_into_fast;
    tokudb::thread::rwlock_t _num_DBs_lock;
    uint32_t num_DBs;

private:
    static std::unordered_map<std::string, TOKUDB_SHARE*> _open_tables;
    static tokudb::thread::mutex_t* _open_tables_mutex;

    //*********************************
    // Spans open-close-open
    mutable tokudb::thread::mutex_t _mutex;
    mutable tokudb::thread::mutex_t _ddl_mutex;
    uint _use_count;

    share_state_t _state;

    ulonglong _row_delta_activity;
    bool _allow_auto_analysis;

    String _full_table_name;
    String _database_name;
    String _table_name;

    //*********************************
    // Destroyed and recreated on open-close-open
    THR_LOCK _thr_lock;

    // estimate on number of rows in table
    ha_rows _rows;

    // cardinality counts
    uint32_t _rec_per_keys;
    uint64_t* _rec_per_key;

    void init(const char* table_name);
    void destroy();
};
inline int TOKUDB_SHARE::use_count() const {
    return _use_count;
}
inline void TOKUDB_SHARE::lock() const {
    TOKUDB_SHARE_DBUG_ENTER("file[%s]:state[%s]:use_count[%d]",
                            _full_table_name.ptr(),
                            get_state_string(_state),
                            _use_count);
    mutex_t_lock(_mutex);
    TOKUDB_SHARE_DBUG_VOID_RETURN();
}
inline void TOKUDB_SHARE::unlock() const {
    TOKUDB_SHARE_DBUG_ENTER("file[%s]:state[%s]:use_count[%d]",
                            _full_table_name.ptr(),
                            get_state_string(_state),
                            _use_count);
    mutex_t_unlock(_mutex);
    TOKUDB_SHARE_DBUG_VOID_RETURN();
}
inline TOKUDB_SHARE::share_state_t TOKUDB_SHARE::state() const {
    return _state;
}
inline void TOKUDB_SHARE::set_state(TOKUDB_SHARE::share_state_t state) {
    TOKUDB_SHARE_DBUG_ENTER("file[%s]:state[%s]:use_count[%d]:new_state[%s]",
        _full_table_name.ptr(),
        get_state_string(_state),
        _use_count,
        get_state_string(state));

    assert_debug(_mutex.is_owned_by_me());
    _state = state;
    TOKUDB_SHARE_DBUG_VOID_RETURN();
}
inline const char* TOKUDB_SHARE::full_table_name() const {
    return _full_table_name.ptr();
}
inline uint TOKUDB_SHARE::full_table_name_length() const {
    return _full_table_name.length();
}
inline const char* TOKUDB_SHARE::database_name() const {
    return _database_name.ptr();
}
inline uint TOKUDB_SHARE::database_name_length() const {
    return _database_name.length();
}
inline const char* TOKUDB_SHARE::table_name() const {
    return _table_name.ptr();
}
inline uint TOKUDB_SHARE::table_name_length() const {
    return _table_name.length();
}
inline void TOKUDB_SHARE::set_row_count(uint64_t rows, bool locked) {
    TOKUDB_SHARE_DBUG_ENTER("file[%s]:state[%s]:use_count[%d]:rows[%" PRIu64 "]:locked[%d]",
        _full_table_name.ptr(),
        get_state_string(_state),
        _use_count,
        rows,
        locked);

    if (!locked) {
        lock();
    } else {
        assert_debug(_mutex.is_owned_by_me());
    }
    if (_rows && rows == 0)
        _row_delta_activity = 0;

    _rows = rows;
    if (!locked) {
        unlock();
    }
    TOKUDB_SHARE_DBUG_VOID_RETURN();
}
inline ha_rows TOKUDB_SHARE::row_count() const {
    return _rows;
}
inline void TOKUDB_SHARE::init_cardinality_counts(
    uint32_t rec_per_keys,
    uint64_t* rec_per_key) {

    assert_debug(_mutex.is_owned_by_me());
    // can not change number of keys live
    assert_always(_rec_per_key == nullptr);
    assert_always(_rec_per_keys == 0);
    _rec_per_keys = rec_per_keys;
    _rec_per_key = rec_per_key;
}
inline void TOKUDB_SHARE::update_cardinality_counts(
    uint32_t rec_per_keys,
    const uint64_t* rec_per_key) {

    assert_debug(_mutex.is_owned_by_me());
    // can not change number of keys live
    assert_always(rec_per_keys == _rec_per_keys);
    assert_always(rec_per_key != NULL);
    memcpy(_rec_per_key, rec_per_key, _rec_per_keys * sizeof(uint64_t));
}
inline void TOKUDB_SHARE::disallow_auto_analysis() {
    assert_debug(_mutex.is_owned_by_me());
    _allow_auto_analysis = false;
}
inline void TOKUDB_SHARE::allow_auto_analysis(bool reset_deltas) {
    assert_debug(_mutex.is_owned_by_me());
    _allow_auto_analysis = true;
    if (reset_deltas)
        _row_delta_activity = 0;
}
inline void TOKUDB_SHARE::cancel_background_jobs() const {
    tokudb::background::_job_manager->cancel_job(full_table_name());
}



typedef struct st_filter_key_part_info {
    uint offset;
    uint part_index;
} FILTER_KEY_PART_INFO;

typedef enum {
    lock_read = 0,
    lock_write
} TABLE_LOCK_TYPE;

// the number of rows bulk fetched in one callback grows exponentially
// with the bulk fetch iteration, so the max iteration is the max number
// of shifts we can perform on a 64 bit integer.
#define HA_TOKU_BULK_FETCH_ITERATION_MAX 63

class ha_tokudb : public handler {
private:
    THR_LOCK_DATA lock;         ///< MySQL lock
    TOKUDB_SHARE *share;        ///< Shared lock info

#ifdef MARIADB_BASE_VERSION
    // MariaDB version of MRR
    DsMrr_impl ds_mrr;
#elif 50600 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50699
    // MySQL version of MRR
    DsMrr_impl ds_mrr;
#endif

    // For ICP. Cache our own copies
    Item* toku_pushed_idx_cond;
    uint toku_pushed_idx_cond_keyno;  /* The index which the above condition is for */
    bool icp_went_out_of_range;

    //
    // last key returned by ha_tokudb's cursor
    //
    DBT last_key;
    //
    // pointer used for multi_alloc of key_buff, key_buff2, primary_key_buff
    //
    void *alloc_ptr;
    //
    // buffer used to temporarily store a "packed row" 
    // data pointer of a DBT will end up pointing to this
    // see pack_row for usage
    //
    uchar *rec_buff;
    //
    // number of bytes allocated in rec_buff
    //
    ulong alloced_rec_buff_length;
    //
    // same as above two, but for updates
    //
    uchar *rec_update_buff;
    ulong alloced_update_rec_buff_length;
    uint32_t max_key_length;

    uchar* range_query_buff; // range query buffer
    uint32_t size_range_query_buff; // size of the allocated range query buffer
    uint32_t bytes_used_in_range_query_buff; // number of bytes used in the range query buffer
    uint32_t curr_range_query_buff_offset; // current offset into the range query buffer for queries to read
    uint64_t bulk_fetch_iteration;
    uint64_t rows_fetched_using_bulk_fetch;
    bool doing_bulk_fetch;
    bool maybe_index_scan;

    //
    // buffer used to temporarily store a "packed key" 
    // data pointer of a DBT will end up pointing to this
    //
    uchar *key_buff; 
    //
    // buffer used to temporarily store a "packed key" 
    // data pointer of a DBT will end up pointing to this
    // This is used in functions that require the packing
    // of more than one key
    //
    uchar *key_buff2; 
    uchar *key_buff3; 
    uchar *key_buff4;
    //
    // buffer used to temporarily store a "packed key" 
    // data pointer of a DBT will end up pointing to this
    // currently this is only used for a primary key in
    // the function update_row, hence the name. It 
    // does not carry any state throughout the class.
    //
    uchar *primary_key_buff;

    //
    // ranges of prelocked area, used to know how much to bulk fetch
    //
    uchar *prelocked_left_range; 
    uint32_t prelocked_left_range_size;
    uchar *prelocked_right_range; 
    uint32_t prelocked_right_range_size;


    //
    // individual DBTs for each index
    //
    DBT_ARRAY mult_key_dbt_array[2*(MAX_KEY + 1)];
    DBT_ARRAY mult_rec_dbt_array[MAX_KEY + 1];
    uint32_t mult_put_flags[MAX_KEY + 1];
    uint32_t mult_del_flags[MAX_KEY + 1];
    uint32_t mult_dbt_flags[MAX_KEY + 1];
    

    //
    // when unpacking blobs, we need to store it in a temporary
    // buffer that will persist because MySQL just gets a pointer to the 
    // blob data, a pointer we need to ensure is valid until the next
    // query
    //
    uchar* blob_buff;
    uint32_t num_blob_bytes;

    bool unpack_entire_row;

    //
    // buffers (and their sizes) that will hold the indexes
    // of fields that need to be read for a query
    //
    uint32_t* fixed_cols_for_query;
    uint32_t num_fixed_cols_for_query;
    uint32_t* var_cols_for_query;
    uint32_t num_var_cols_for_query;
    bool read_blobs;
    bool read_key;

    //
    // transaction used by ha_tokudb's cursor
    //
    DB_TXN *transaction;

    // external_lock will set this true for read operations that will be closely followed by write operations.
    bool use_write_locks; // use write locks for reads

    //
    // instance of cursor being used for init_xxx and rnd_xxx functions
    //
    DBC *cursor;
    uint32_t cursor_flags; // flags for cursor
    //
    // flags that are returned in table_flags()
    //
    ulonglong int_table_flags;
    // 
    // count on the number of rows that gets changed, such as when write_row occurs
    // this is meant to help keep estimate on number of elements in DB
    // 
    ulonglong added_rows;
    ulonglong deleted_rows;
    ulonglong updated_rows;


    uint last_dup_key;
    //
    // if set to 0, then the primary key is not hidden
    // if non-zero (not necessarily 1), primary key is hidden
    //
    uint hidden_primary_key;
    bool key_read, using_ignore;
    bool using_ignore_no_key;

    //
    // After a cursor encounters an error, the cursor will be unusable
    // In case MySQL attempts to do a cursor operation (such as rnd_next
    // or index_prev), we will gracefully return this error instead of crashing
    //
    int last_cursor_error;

    //
    // For instances where we successfully prelock a range or a table,
    // we set this to true so that successive cursor calls can know
    // know to limit the locking overhead in a call to the fractal tree
    //
    bool range_lock_grabbed;
    bool range_lock_grabbed_null;

    //
    // For bulk inserts, we want option of not updating auto inc
    // until all inserts are done. By default, is false
    //
    bool delay_updating_ai_metadata; // if true, don't update auto-increment metadata until bulk load completes
    bool ai_metadata_update_required; // if true, autoincrement metadata must be updated 

    //
    // buffer for updating the status of long insert, delete, and update
    // statements. Right now, the the messages are 
    // "[inserted|updated|deleted] about %llu rows",
    // so a buffer of 200 is good enough.
    //
    char write_status_msg[200]; //buffer of 200 should be a good upper bound.
    struct loader_context lc;

    DB_LOADER* loader;
    bool abort_loader;
    int loader_error;

    bool num_DBs_locked_in_bulk;
    uint32_t lock_count;
    
    bool fix_rec_buff_for_blob(ulong length);
    bool fix_rec_update_buff_for_blob(ulong length);
    uchar current_ident[TOKUDB_HIDDEN_PRIMARY_KEY_LENGTH];

    ulong max_row_length(const uchar * buf);
    int pack_row_in_buff(
        DBT * row, 
        const uchar* record,
        uint index,
        uchar* row_buff
        );
    int pack_row(
        DBT * row, 
        const uchar* record,
        uint index
        );
    int pack_old_row_for_update(
        DBT * row, 
        const uchar* record,
        uint index
        );
    uint32_t place_key_into_mysql_buff(KEY* key_info, uchar * record, uchar* data);
    void unpack_key(uchar * record, DBT const *key, uint index);
    uint32_t place_key_into_dbt_buff(KEY* key_info, uchar * buff, const uchar * record, bool* has_null, int key_length);
    DBT* create_dbt_key_from_key(DBT * key, KEY* key_info, uchar * buff, const uchar * record, bool* has_null, bool dont_pack_pk, int key_length, uint8_t inf_byte);
    DBT *create_dbt_key_from_table(DBT * key, uint keynr, uchar * buff, const uchar * record, bool* has_null, int key_length = MAX_KEY_LENGTH);
    DBT* create_dbt_key_for_lookup(DBT * key, KEY* key_info, uchar * buff, const uchar * record, bool* has_null, int key_length = MAX_KEY_LENGTH);
    DBT *pack_key(DBT * key, uint keynr, uchar * buff, const uchar * key_ptr, uint key_length, int8_t inf_byte);
#if defined(TOKU_INCLUDE_EXTENDED_KEYS) && TOKU_INCLUDE_EXTENDED_KEYS
    DBT *pack_ext_key(DBT * key, uint keynr, uchar * buff, const uchar * key_ptr, uint key_length, int8_t inf_byte);
#endif  // defined(TOKU_INCLUDE_EXTENDED_KEYS) && TOKU_INCLUDE_EXTENDED_KEYS
    bool key_changed(uint keynr, const uchar * old_row, const uchar * new_row);
    int handle_cursor_error(int error, int err_to_return);
    DBT *get_pos(DBT * to, uchar * pos);
 
    int open_main_dictionary(const char* name, bool is_read_only, DB_TXN* txn);
    int open_secondary_dictionary(DB** ptr, KEY* key_info, const char* name, bool is_read_only, DB_TXN* txn);
    int acquire_table_lock (DB_TXN* trans, TABLE_LOCK_TYPE lt);
    int estimate_num_rows(DB* db, uint64_t* num_rows, DB_TXN* txn);
    bool has_auto_increment_flag(uint* index);

#if defined(TOKU_INCLUDE_WRITE_FRM_DATA) && TOKU_INCLUDE_WRITE_FRM_DATA
    int write_frm_data(DB* db, DB_TXN* txn, const char* frm_name);
    int verify_frm_data(const char* frm_name, DB_TXN* trans);
    int remove_frm_data(DB *db, DB_TXN *txn);
#endif  // defined(TOKU_INCLUDE_WRITE_FRM_DATA) && TOKU_INCLUDE_WRITE_FRM_DATA

    int write_to_status(DB* db, HA_METADATA_KEY curr_key_data, void* data, uint size, DB_TXN* txn);
    int remove_from_status(DB* db, HA_METADATA_KEY curr_key_data, DB_TXN* txn);

    int write_metadata(DB* db, void* key, uint key_size, void* data, uint data_size, DB_TXN* txn);
    int remove_metadata(DB* db, void* key_data, uint key_size, DB_TXN* transaction);

    int update_max_auto_inc(DB* db, ulonglong val);
    int remove_key_name_from_status(DB* status_block, char* key_name, DB_TXN* txn);
    int write_key_name_to_status(DB* status_block, char* key_name, DB_TXN* txn);
    int write_auto_inc_create(DB* db, ulonglong val, DB_TXN* txn);
    void init_auto_increment();
    bool can_replace_into_be_fast(TABLE_SHARE* table_share, KEY_AND_COL_INFO* kc_info, uint pk);
    int initialize_share(const char* name, int mode);

    void set_query_columns(uint keynr);
    int prelock_range (const key_range *start_key, const key_range *end_key);
    int create_txn(THD* thd, tokudb_trx_data* trx);
    bool may_table_be_empty(DB_TXN *txn);
    int delete_or_rename_table (const char* from_name, const char* to_name, bool is_delete);
    int delete_or_rename_dictionary( const char* from_name, const char* to_name, const char* index_name, bool is_key, DB_TXN* txn, bool is_delete);
    int truncate_dictionary( uint keynr, DB_TXN* txn );
    int create_secondary_dictionary(
        const char* name, 
        TABLE* form, 
        KEY* key_info, 
        DB_TXN* txn, 
        KEY_AND_COL_INFO* kc_info, 
        uint32_t keynr, 
        bool is_hot_index,
        toku_compression_method compression_method
        );
    int create_main_dictionary(const char* name, TABLE* form, DB_TXN* txn, KEY_AND_COL_INFO* kc_info, toku_compression_method compression_method);
    void trace_create_table_info(TABLE* form);
    int is_index_unique(bool* is_unique, DB_TXN* txn, DB* db, KEY* key_info, int lock_flags);
    int is_val_unique(bool* is_unique, uchar* record, KEY* key_info, uint dict_index, DB_TXN* txn);
    int do_uniqueness_checks(uchar* record, DB_TXN* txn, THD* thd);
    void set_main_dict_put_flags(THD* thd, bool opt_eligible, uint32_t* put_flags);
    int insert_row_to_main_dictionary(DBT* pk_key, DBT* pk_val, DB_TXN* txn);
    int insert_rows_to_dictionaries_mult(DBT* pk_key, DBT* pk_val, DB_TXN* txn, THD* thd);
    void test_row_packing(uchar* record, DBT* pk_key, DBT* pk_val);
    uint32_t fill_row_mutator(
        uchar* buf, 
        uint32_t* dropped_columns, 
        uint32_t num_dropped_columns,
        TABLE* altered_table,
        KEY_AND_COL_INFO* altered_kc_info,
        uint32_t keynr,
        bool is_add
        );

    // 0 <= active_index < table_share->keys || active_index == MAX_KEY
    // tokudb_active_index = active_index if active_index < table_share->keys, else tokudb_active_index = primary_key = table_share->keys
    uint tokudb_active_index;
 
public:
    ha_tokudb(handlerton * hton, TABLE_SHARE * table_arg);
    ~ha_tokudb();

    const char *table_type() const;
    const char *index_type(uint inx);
    const char **bas_ext() const;

    //
    // Returns a bit mask of capabilities of storage engine. Capabilities 
    // defined in sql/handler.h
    //
    ulonglong table_flags() const;
    
    ulong index_flags(uint inx, uint part, bool all_parts) const;

    //
    // Returns limit on the number of keys imposed by tokudb.
    //
    uint max_supported_keys() const {
        return MAX_KEY;
    } 

    uint extra_rec_buf_length() const {
        return TOKUDB_HIDDEN_PRIMARY_KEY_LENGTH;
    } 
    ha_rows estimate_rows_upper_bound();

    //
    // Returns the limit on the key length imposed by tokudb.
    //
    uint max_supported_key_length() const {
        return UINT_MAX32;
    } 

    //
    // Returns limit on key part length imposed by tokudb.
    //
    uint max_supported_key_part_length() const {
        return UINT_MAX32;
    } 
    const key_map *keys_to_use_for_scanning() {
        return &key_map_full;
    }

    double scan_time();

    double read_time(uint index, uint ranges, ha_rows rows);
    
    // Defined in mariadb
    double keyread_time(uint index, uint ranges, ha_rows rows);

    // Defined in mysql 5.6
    double index_only_read_time(uint keynr, double records);

    int open(const char *name, int mode, uint test_if_locked);
    int close();
    void update_create_info(HA_CREATE_INFO* create_info);
    int create(const char *name, TABLE * form, HA_CREATE_INFO * create_info);
    int delete_table(const char *name);
    int rename_table(const char *from, const char *to);
    int optimize(THD * thd, HA_CHECK_OPT * check_opt);
    int analyze(THD * thd, HA_CHECK_OPT * check_opt);
    int write_row(uchar * buf);
    int update_row(const uchar * old_data, uchar * new_data);
    int delete_row(const uchar * buf);
#if MYSQL_VERSION_ID >= 100000
    void start_bulk_insert(ha_rows rows, uint flags);
#else
    void start_bulk_insert(ha_rows rows);
#endif
    static int bulk_insert_poll(void* extra, float progress);
    static void loader_add_index_err(DB* db,
                                     int i,
                                     int err,
                                     DBT* key,
                                     DBT* val,
                                     void* error_extra);
    static void loader_dup(DB* db,
                           int i,
                           int err,
                           DBT* key,
                           DBT* val,
                           void* error_extra);
    int end_bulk_insert();
    int end_bulk_insert(bool abort);

    int prepare_index_scan();
    int prepare_index_key_scan( const uchar * key, uint key_len );
    int prepare_range_scan( const key_range *start_key, const key_range *end_key);
    void column_bitmaps_signal();
    int index_init(uint index, bool sorted);
    int index_end();
    int index_next_same(uchar * buf, const uchar * key, uint keylen); 
    int index_read(uchar * buf, const uchar * key, uint key_len, enum ha_rkey_function find_flag);
    int index_read_last(uchar * buf, const uchar * key, uint key_len);
    int index_next(uchar * buf);
    int index_prev(uchar * buf);
    int index_first(uchar * buf);
    int index_last(uchar * buf);

    bool has_gap_locks() const { return true; }

    int rnd_init(bool scan);
    int rnd_end();
    int rnd_next(uchar * buf);
    int rnd_pos(uchar * buf, uchar * pos);

    int read_range_first(const key_range *start_key,
                                 const key_range *end_key,
                                 bool eq_range, bool sorted);
    int read_range_next();


    void position(const uchar * record);
    int info(uint);
    int extra(enum ha_extra_function operation);
    int reset();
    int external_lock(THD * thd, int lock_type);
    int start_stmt(THD * thd, thr_lock_type lock_type);

    ha_rows records_in_range(uint inx, key_range * min_key, key_range * max_key);

    uint32_t get_cursor_isolation_flags(enum thr_lock_type lock_type, THD* thd);
    THR_LOCK_DATA **store_lock(THD * thd, THR_LOCK_DATA ** to, enum thr_lock_type lock_type);

    int get_status(DB_TXN* trans);
    void init_hidden_prim_key_info(DB_TXN *txn);
    inline void get_auto_primary_key(uchar * to) {
        share->lock();
        share->auto_ident++;
        hpk_num_to_char(to, share->auto_ident);
        share->unlock();
    }
    virtual void get_auto_increment(
        ulonglong offset,
        ulonglong increment,
        ulonglong nb_desired_values,
        ulonglong* first_value,
        ulonglong* nb_reserved_values);
    bool is_optimize_blocking();
    bool is_auto_inc_singleton();
    void print_error(int error, myf errflag);
    uint8 table_cache_type() {
        return HA_CACHE_TBL_TRANSACT;
    }
    bool primary_key_is_clustered() {
        return true;
    }
    int cmp_ref(const uchar * ref1, const uchar * ref2);
    bool check_if_incompatible_data(HA_CREATE_INFO * info, uint table_changes);

#ifdef MARIADB_BASE_VERSION

// MariaDB MRR introduced in 5.5, API changed in MariaDB 10.0
#if MYSQL_VERSION_ID >= 100000
#define COST_VECT Cost_estimate
#endif

    int multi_range_read_init(RANGE_SEQ_IF* seq,
                              void* seq_init_param,
                              uint n_ranges, uint mode,
                              HANDLER_BUFFER *buf);
    int multi_range_read_next(range_id_t *range_info);
    ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                        void *seq_init_param, 
                                        uint n_ranges, uint *bufsz,
                                        uint *flags, COST_VECT *cost);
    ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                  uint key_parts, uint *bufsz, 
                                  uint *flags, COST_VECT *cost);
    int multi_range_read_explain_info(uint mrr_mode, char *str, size_t size);

#else

// MySQL  MRR introduced in 5.6
#if 50600 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50699
    int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                              uint n_ranges, uint mode, HANDLER_BUFFER *buf);
    int multi_range_read_next(char **range_info);
    ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                        void *seq_init_param, 
                                        uint n_ranges, uint *bufsz,
                                        uint *flags, Cost_estimate *cost);
    ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                  uint *bufsz, uint *flags, Cost_estimate *cost);
#endif

#endif

    Item* idx_cond_push(uint keyno, class Item* idx_cond);
    void cancel_pushed_idx_cond();

#if defined(TOKU_INCLUDE_ALTER_56) && TOKU_INCLUDE_ALTER_56
 public:
    enum_alter_inplace_result check_if_supported_inplace_alter(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    bool prepare_inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    bool inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    bool commit_inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info, bool commit);
 private:
    int alter_table_add_index(Alter_inplace_info* ha_alter_info);
    int alter_table_drop_index(Alter_inplace_info* ha_alter_info);
    int alter_table_add_or_drop_column(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    int alter_table_expand_varchar_offsets(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    int alter_table_expand_columns(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    int alter_table_expand_one_column(TABLE *altered_table, Alter_inplace_info *ha_alter_info, int expand_field_num);
    int alter_table_expand_blobs(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    void print_alter_info(TABLE *altered_table, Alter_inplace_info *ha_alter_info);
    int setup_kc_info(TABLE *altered_table, KEY_AND_COL_INFO *kc_info);
    int new_row_descriptor(TABLE* altered_table,
                           Alter_inplace_info* ha_alter_info,
                           uint32_t idx,
                           DBT* row_descriptor);

 public:
#endif  // defined(TOKU_INCLUDE_ALTER_56) && TOKU_INCLUDE_ALTER_56
#if defined(TOKU_INCLUDE_ALTER_55) && TOKU_INCLUDE_ALTER_55
public:
    // Returns true of the 5.6 inplace alter table interface is used.
    bool try_hot_alter_table();

    // Used by the partition storage engine to provide new frm data for the table.
    int new_alter_table_frm_data(const uchar *frm_data, size_t frm_len);
#endif  // defined(TOKU_INCLUDE_ALTER_55) && TOKU_INCLUDE_ALTER_55

 private:
  int tokudb_add_index(TABLE* table_arg,
                       KEY* key_info,
                       uint num_of_keys,
                       DB_TXN* txn,
                       bool* inc_num_DBs,
                       bool* modified_DB);
  static int tokudb_add_index_poll(void *extra, float progress);
  void restore_add_index(TABLE* table_arg,
                         uint num_of_keys,
                         bool incremented_numDBs,
                         bool modified_DBs);
  int drop_indexes(uint* key_num, uint num_of_keys, KEY* key_info, DB_TXN* txn);
  void restore_drop_indexes(uint* key_num, uint num_of_keys);

 public:
    // delete all rows from the table
    // effect: all dictionaries, including the main and indexes, should be empty
    int discard_or_import_tablespace(my_bool discard);
    int truncate();
    int delete_all_rows();
    void extract_hidden_primary_key(uint keynr, DBT const *found_key);
    void read_key_only(uchar * buf, uint keynr, DBT const *found_key);
    int read_row_callback (uchar * buf, uint keynr, DBT const *row, DBT const *found_key);
    int read_primary_key(uchar * buf, uint keynr, DBT const *row, DBT const *found_key);
    int unpack_blobs(
        uchar* record,
        const uchar* from_tokudb_blob,
        uint32_t num_blob_bytes,
        bool check_bitmap
        );
    int unpack_row(
        uchar* record, 
        DBT const *row, 
        DBT const *key,
        uint index
        );

    int prefix_cmp_dbts( uint keynr, const DBT* first_key, const DBT* second_key) {
        return tokudb_prefix_cmp_dbt_key(share->key_file[keynr], first_key, second_key);
    }

    void track_progress(THD* thd);
    void set_loader_error(int err);
    void set_dup_value_for_pk(DBT* key);


    //
    // index into key_file that holds DB* that is indexed on
    // the primary_key. this->key_file[primary_index] == this->file
    //
    uint primary_key;

    int check(THD *thd, HA_CHECK_OPT *check_opt);

    int fill_range_query_buf(
        bool need_val, 
        DBT const* key,
        DBT const* row,
        int direction,
        THD* thd,
        uchar* buf,
        DBT* key_to_compare);

#if defined(TOKU_INCLUDE_ROW_TYPE_COMPRESSION) && \
    TOKU_INCLUDE_ROW_TYPE_COMPRESSION
    enum row_type get_row_type() const;
#endif  // defined(TOKU_INCLUDE_ROW_TYPE_COMPRESSION) &&
        // TOKU_INCLUDE_ROW_TYPE_COMPRESSION
private:
    int read_full_row(uchar * buf);
    int __close();
    int get_next(uchar* buf, int direction, DBT* key_to_compare, bool do_key_read);
    int read_data_from_range_query_buff(uchar* buf, bool need_val, bool do_key_read);
    // for ICP, only in MariaDB and MySQL 5.6
    enum icp_result toku_handler_index_cond_check(Item* pushed_idx_cond);
    void invalidate_bulk_fetch();
    void invalidate_icp();
    int delete_all_rows_internal();
    void close_dsmrr();
    void reset_dsmrr();
    
#if defined(TOKU_INCLUDE_WRITE_FRM_DATA) && TOKU_INCLUDE_WRITE_FRM_DATA
    int write_frm_data(const uchar *frm_data, size_t frm_len);
#endif  // defined(TOKU_INCLUDE_WRITE_FRM_DATA) && TOKU_INCLUDE_WRITE_FRM_DATA

private:
#if defined(TOKU_INCLUDE_UPSERT) && TOKU_INCLUDE_UPSERT
    MY_NODISCARD int fast_update(THD *thd,
                                 List<Item> &update_fields,
                                 List<Item> &update_values,
                                 Item *conds);
    MY_NODISCARD bool check_fast_update(THD *thd,
                                        List<Item> &update_fields,
                                        List<Item> &update_values,
                                        Item *conds);
    MY_NODISCARD int send_update_message(List<Item> &update_fields,
                                         List<Item> &update_values,
                                         Item *conds,
                                         DB_TXN *txn);
    MY_NODISCARD int upsert(THD *thd,
                            List<Item> &update_fields,
                            List<Item> &update_values);
    MY_NODISCARD bool check_upsert(THD *thd,
                                   List<Item> &update_fields,
                                   List<Item> &update_values);
    MY_NODISCARD int send_upsert_message(List<Item> &update_fields,
                                         List<Item> &update_values,
                                         DB_TXN *txn);
#endif  // defined(TOKU_INCLUDE_UPSERT) && TOKU_INCLUDE_UPSERT

public:
    // mysql sometimes retires a txn before a cursor that references the txn is closed.
    // for example, commit is sometimes called before index_end.  the following methods
    // put the handler on a list of handlers that get cleaned up when the txn is retired.
    void cleanup_txn(DB_TXN *txn);
private:
    LIST trx_handler_list;
    void add_to_trx_handler_list();
    void remove_from_trx_handler_list();

private:
    int do_optimize(THD *thd);
    int map_to_handler_error(int error);

#if defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
public:
    void rpl_before_write_rows();
    void rpl_after_write_rows();
    void rpl_before_delete_rows();
    void rpl_after_delete_rows();
    void rpl_before_update_rows();
    void rpl_after_update_rows();
    bool rpl_lookup_rows();
private:
    bool in_rpl_write_rows;
    bool in_rpl_delete_rows;
    bool in_rpl_update_rows;
#endif // defined(TOKU_INCLUDE_RFR) && TOKU_INCLUDE_RFR
};

#endif // _HA_TOKUDB_H

