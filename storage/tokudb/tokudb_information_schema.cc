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
#include "tokudb_information_schema.h"
#include "sql_time.h"
#include "tokudb_background.h"


namespace tokudb {
namespace information_schema {

static void field_store_time_t(Field* field, time_t time) {
    MYSQL_TIME  my_time;
    struct tm tm_time;

    if (time) {
        localtime_r(&time, &tm_time);
        localtime_to_TIME(&my_time, &tm_time);
        my_time.time_type = MYSQL_TIMESTAMP_DATETIME;
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
        field->store_time(&my_time);
#else
        field->store_time(&my_time, MYSQL_TIMESTAMP_DATETIME);
#endif
        field->set_notnull();
    } else {
        field->set_null();
    }
}

st_mysql_information_schema trx_information_schema = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

ST_FIELD_INFO trx_field_info[] = {
    {"trx_id", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"trx_mysql_thread_id", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"trx_time", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

struct trx_extra_t {
    THD *thd;
    TABLE *table;
};

int trx_callback(DB_TXN* txn,
                 TOKUDB_UNUSED(iterate_row_locks_callback iterate_locks),
                 TOKUDB_UNUSED(void* locks_extra),
                 void* extra) {
    uint64_t txn_id = txn->id64(txn);
    uint64_t client_id;
    txn->get_client_id(txn, &client_id, NULL);
    uint64_t start_time = txn->get_start_time(txn);
    trx_extra_t* e = reinterpret_cast<struct trx_extra_t*>(extra);
    THD* thd = e->thd;
    TABLE* table = e->table;
    table->field[0]->store(txn_id, false);
    table->field[1]->store(client_id, false);
    uint64_t tnow = (uint64_t) ::time(NULL);
    table->field[2]->store(tnow >= start_time ? tnow - start_time : 0, false);
    int error = schema_table_store_record(thd, table);
    if (!error && thd_kill_level(thd))
        error = ER_QUERY_INTERRUPTED;
    return error;
}

#if MYSQL_VERSION_ID >= 50600
int trx_fill_table(THD* thd, TABLE_LIST* tables, TOKUDB_UNUSED(Item* cond)) {
#else
int trx_fill_table(THD* thd, TABLE_LIST* tables, TOKUDB_UNUSED(COND* cond)) {
#endif
    TOKUDB_DBUG_ENTER("");
    int error;

    rwlock_t_lock_read(tokudb_hton_initialized_lock);

    if (!tokudb_hton_initialized) {
        error = ER_PLUGIN_IS_NOT_LOADED;
        my_error(error, MYF(0), tokudb_hton_name);
    } else {
        trx_extra_t e = { thd, tables->table };
        error = db_env->iterate_live_transactions(db_env, trx_callback, &e);
        if (error)
            my_error(ER_GET_ERRNO, MYF(0), error, tokudb_hton_name);
    }

    tokudb_hton_initialized_lock.unlock();
    TOKUDB_DBUG_RETURN(error);
}

int trx_init(void* p) {
    ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE*) p;
    schema->fields_info = trx_field_info;
    schema->fill_table = trx_fill_table;
    return 0;
}

int trx_done(TOKUDB_UNUSED(void* p)) {
    return 0;
}

st_mysql_plugin trx = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &trx_information_schema,
    "TokuDB_trx",
    "Percona",
    "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    trx_init,                   /* plugin init */
    trx_done,                   /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,
    NULL,                      /* status variables */
    NULL,                      /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
    tokudb::sysvars::version,
    MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
    NULL,                      /* config options */
    0,                         /* flags */
#endif
};



st_mysql_information_schema lock_waits_information_schema = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

ST_FIELD_INFO lock_waits_field_info[] = {
    {"requesting_trx_id", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"blocking_trx_id", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"lock_waits_dname", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"lock_waits_key_left", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"lock_waits_key_right", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"lock_waits_start_time", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"lock_waits_table_schema", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"lock_waits_table_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"lock_waits_table_dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

struct lock_waits_extra_t {
    THD* thd;
    TABLE* table;
};

int lock_waits_callback(
    DB* db,
    uint64_t requesting_txnid,
    const DBT* left_key,
    const DBT* right_key,
    uint64_t blocking_txnid,
    uint64_t start_time,
    void *extra) {

    lock_waits_extra_t* e =
        reinterpret_cast<struct lock_waits_extra_t*>(extra);
    THD* thd = e->thd;
    TABLE* table = e->table;
    table->field[0]->store(requesting_txnid, false);
    table->field[1]->store(blocking_txnid, false);
    const char* dname = tokudb_get_index_name(db);
    size_t dname_length = strlen(dname);
    table->field[2]->store(dname, dname_length, system_charset_info);
    String left_str;
    tokudb_pretty_left_key(left_key, &left_str);
    table->field[3]->store(
        left_str.ptr(),
        left_str.length(),
        system_charset_info);
    String right_str;
    tokudb_pretty_right_key(right_key, &right_str);
    table->field[4]->store(
        right_str.ptr(),
        right_str.length(),
        system_charset_info);
    table->field[5]->store(start_time, false);

    String database_name, table_name, dictionary_name;
    tokudb_split_dname(dname, database_name, table_name, dictionary_name);
    table->field[6]->store(
        database_name.c_ptr(),
        database_name.length(),
        system_charset_info);
    table->field[7]->store(
        table_name.c_ptr(),
        table_name.length(),
        system_charset_info);
    table->field[8]->store(
        dictionary_name.c_ptr(),
        dictionary_name.length(),
        system_charset_info);

    int error = schema_table_store_record(thd, table);

    if (!error && thd_kill_level(thd))
        error = ER_QUERY_INTERRUPTED;

    return error;
}

#if MYSQL_VERSION_ID >= 50600
int lock_waits_fill_table(THD* thd,
                          TABLE_LIST* tables,
                          TOKUDB_UNUSED(Item* cond)) {
#else
int lock_waits_fill_table(THD* thd,
                          TABLE_LIST* tables,
                          TOKUDB_UNUSED(COND* cond)) {
#endif
    TOKUDB_DBUG_ENTER("");
    int error;

    rwlock_t_lock_read(tokudb_hton_initialized_lock);

    if (!tokudb_hton_initialized) {
        error = ER_PLUGIN_IS_NOT_LOADED;
        my_error(error, MYF(0), tokudb_hton_name);
    } else {
        lock_waits_extra_t e = { thd, tables->table };
        error = db_env->iterate_pending_lock_requests(
            db_env,
            lock_waits_callback,
            &e);
        if (error)
            my_error(ER_GET_ERRNO, MYF(0), error, tokudb_hton_name);
    }

    tokudb_hton_initialized_lock.unlock();
    TOKUDB_DBUG_RETURN(error);
}

int lock_waits_init(void* p) {
    ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
    schema->fields_info = lock_waits_field_info;
    schema->fill_table = lock_waits_fill_table;
    return 0;
}

int lock_waits_done(TOKUDB_UNUSED(void *p)) {
    return 0;
}

st_mysql_plugin lock_waits = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &lock_waits_information_schema,
    "TokuDB_lock_waits",
    "Percona",
    "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    lock_waits_init,            /* plugin init */
    lock_waits_done,            /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,
    NULL,                       /* status variables */
    NULL,                       /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
    tokudb::sysvars::version,
    MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
    NULL,                       /* config options */
    0,                          /* flags */
#endif
};



st_mysql_information_schema locks_information_schema = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

 ST_FIELD_INFO locks_field_info[] = {
    {"locks_trx_id", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"locks_mysql_thread_id", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"locks_dname", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"locks_key_left", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"locks_key_right", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"locks_table_schema", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"locks_table_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"locks_table_dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

struct locks_extra_t {
    THD* thd;
    TABLE* table;
};

int locks_callback(
    DB_TXN* txn,
    iterate_row_locks_callback iterate_locks,
    void* locks_extra,
    void* extra) {

    uint64_t txn_id = txn->id64(txn);
    uint64_t client_id;
    txn->get_client_id(txn, &client_id, NULL);
    locks_extra_t* e = reinterpret_cast<struct locks_extra_t*>(extra);
    THD* thd = e->thd;
    TABLE* table = e->table;
    int error = 0;
    DB* db;
    DBT left_key, right_key;
    while (error == 0 &&
           iterate_locks(&db, &left_key, &right_key, locks_extra) == 0) {
        table->field[0]->store(txn_id, false);
        table->field[1]->store(client_id, false);

        const char* dname = tokudb_get_index_name(db);
        size_t dname_length = strlen(dname);
        table->field[2]->store(dname, dname_length, system_charset_info);

        String left_str;
        tokudb_pretty_left_key(&left_key, &left_str);
        table->field[3]->store(
            left_str.ptr(),
            left_str.length(),
            system_charset_info);

        String right_str;
        tokudb_pretty_right_key(&right_key, &right_str);
        table->field[4]->store(
            right_str.ptr(),
            right_str.length(),
            system_charset_info);

        String database_name, table_name, dictionary_name;
        tokudb_split_dname(dname, database_name, table_name, dictionary_name);
        table->field[5]->store(
            database_name.c_ptr(),
            database_name.length(),
            system_charset_info);
        table->field[6]->store(
            table_name.c_ptr(),
            table_name.length(),
            system_charset_info);
        table->field[7]->store(
            dictionary_name.c_ptr(),
            dictionary_name.length(),
            system_charset_info);

        error = schema_table_store_record(thd, table);

        if (!error && thd_kill_level(thd))
            error = ER_QUERY_INTERRUPTED;
    }
    return error;
}

#if MYSQL_VERSION_ID >= 50600
int locks_fill_table(THD* thd, TABLE_LIST* tables, TOKUDB_UNUSED(Item* cond)) {
#else
int locks_fill_table(THD* thd, TABLE_LIST* tables, TOKUDB_UNUSED(COND* cond)) {
#endif
    TOKUDB_DBUG_ENTER("");
    int error;

    rwlock_t_lock_read(tokudb_hton_initialized_lock);

    if (!tokudb_hton_initialized) {
        error = ER_PLUGIN_IS_NOT_LOADED;
        my_error(error, MYF(0), tokudb_hton_name);
    } else {
        locks_extra_t e = { thd, tables->table };
        error = db_env->iterate_live_transactions(db_env, locks_callback, &e);
        if (error)
            my_error(ER_GET_ERRNO, MYF(0), error, tokudb_hton_name);
    }

    tokudb_hton_initialized_lock.unlock();
    TOKUDB_DBUG_RETURN(error);
}

int locks_init(void* p) {
    ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
    schema->fields_info = locks_field_info;
    schema->fill_table = locks_fill_table;
    return 0;
}

int locks_done(TOKUDB_UNUSED(void* p)) {
    return 0;
}

st_mysql_plugin locks = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &locks_information_schema,
    "TokuDB_locks",
    "Percona",
    "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    locks_init,                 /* plugin init */
    locks_done,                 /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,
    NULL,                       /* status variables */
    NULL,                       /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
    tokudb::sysvars::version,
    MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
    NULL,                       /* config options */
    0,                         /* flags */
#endif
};



st_mysql_information_schema file_map_information_schema = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

ST_FIELD_INFO file_map_field_info[] = {
    {"dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"internal_file_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_schema", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

int report_file_map(TABLE* table, THD* thd) {
    int error;
    DB_TXN* txn = NULL;
    DBC* tmp_cursor = NULL;
    DBT curr_key;
    DBT curr_val;
    memset(&curr_key, 0, sizeof curr_key);
    memset(&curr_val, 0, sizeof curr_val);
    error = txn_begin(db_env, 0, &txn, DB_READ_UNCOMMITTED, thd);
    if (error) {
        goto cleanup;
    }
    error = db_env->get_cursor_for_directory(db_env, txn, &tmp_cursor);
    if (error) {
        goto cleanup;
    }
    while (error == 0) {
        error = tmp_cursor->c_get(tmp_cursor, &curr_key, &curr_val, DB_NEXT);
        if (!error) {
            // We store the NULL terminator in the directory so it's included
            // in the size.
            // See #5789
            // Recalculate and check just to be safe.
            const char *dname = (const char *) curr_key.data;
            size_t dname_len = strlen(dname);
            assert(dname_len == curr_key.size - 1);
            table->field[0]->store(dname, dname_len, system_charset_info);

            const char *iname = (const char *) curr_val.data;
            size_t iname_len = strlen(iname);
            assert(iname_len == curr_val.size - 1);
            table->field[1]->store(iname, iname_len, system_charset_info);

            // split the dname
            String database_name, table_name, dictionary_name;
            tokudb_split_dname(
                dname,
                database_name,
                table_name,
                dictionary_name);
            table->field[2]->store(
                database_name.c_ptr(),
                database_name.length(),
                system_charset_info);
            table->field[3]->store(
                table_name.c_ptr(),
                table_name.length(),
                system_charset_info);
            table->field[4]->store(
                dictionary_name.c_ptr(),
                dictionary_name.length(),
                system_charset_info);

            error = schema_table_store_record(thd, table);
        }
        if (!error && thd_kill_level(thd))
            error = ER_QUERY_INTERRUPTED;
    }
    if (error == DB_NOTFOUND) {
        error = 0;
    }
cleanup:
    if (tmp_cursor) {
        int r = tmp_cursor->c_close(tmp_cursor);
        assert(r == 0);
    }
    if (txn) {
        commit_txn(txn, 0);
    }
    return error;
}

#if MYSQL_VERSION_ID >= 50600
int file_map_fill_table(THD* thd,
                        TABLE_LIST* tables,
                        TOKUDB_UNUSED(Item* cond)) {
#else
int file_map_fill_table(THD* thd,
                        TABLE_LIST* tables,
                        TOKUDB_UNUSED(COND* cond)) {
#endif
    TOKUDB_DBUG_ENTER("");
    int error;
    TABLE* table = tables->table;

    rwlock_t_lock_read(tokudb_hton_initialized_lock);

    if (!tokudb_hton_initialized) {
        error = ER_PLUGIN_IS_NOT_LOADED;
        my_error(error, MYF(0), tokudb_hton_name);
    } else {
        error = report_file_map(table, thd);
        if (error)
            my_error(ER_GET_ERRNO, MYF(0), error, tokudb_hton_name);
    }

    tokudb_hton_initialized_lock.unlock();
    TOKUDB_DBUG_RETURN(error);
}

int file_map_init(void* p) {
    ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
    schema->fields_info = file_map_field_info;
    schema->fill_table = file_map_fill_table;
    return 0;
}

int file_map_done(TOKUDB_UNUSED(void* p)) {
    return 0;
}

st_mysql_plugin file_map = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &file_map_information_schema,
    "TokuDB_file_map",
    "Percona",
    "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    file_map_init,              /* plugin init */
    file_map_done,              /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,
    NULL,                       /* status variables */
    NULL,                       /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
    tokudb::sysvars::version,
    MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
    NULL,                       /* config options */
    0,                          /* flags */
#endif
};



st_mysql_information_schema fractal_tree_info_information_schema = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

ST_FIELD_INFO fractal_tree_info_field_info[] = {
    {"dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"internal_file_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"bt_num_blocks_allocated", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"bt_num_blocks_in_use", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"bt_size_allocated", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"bt_size_in_use", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_schema", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

int report_fractal_tree_info_for_db(
    const DBT* dname,
    const DBT* iname,
    TABLE* table,
    THD* thd) {

    int error;
    uint64_t bt_num_blocks_allocated;
    uint64_t bt_num_blocks_in_use;
    uint64_t bt_size_allocated;
    uint64_t bt_size_in_use;

    DB *db = NULL;
    error = db_create(&db, db_env, 0);
    if (error) {
        goto exit;
    }
    error = db->open(db, NULL, (char *)dname->data, NULL, DB_BTREE, 0, 0666);
    if (error) {
        goto exit;
    }
    error = db->get_fractal_tree_info64(
        db,
        &bt_num_blocks_allocated,
        &bt_num_blocks_in_use,
        &bt_size_allocated,
        &bt_size_in_use);
    if (error) {
        goto exit;
    }

    // We store the NULL terminator in the directory so it's included in the
    // size.
    // See #5789
    // Recalculate and check just to be safe.
    {
        size_t dname_len = strlen((const char*)dname->data);
        assert(dname_len == dname->size - 1);
        table->field[0]->store(
            (char*)dname->data,
            dname_len,
            system_charset_info);
        size_t iname_len = strlen((const char*)iname->data);
        assert(iname_len == iname->size - 1);
        table->field[1]->store(
            (char*)iname->data,
            iname_len,
            system_charset_info);
    }
    table->field[2]->store(bt_num_blocks_allocated, false);
    table->field[3]->store(bt_num_blocks_in_use, false);
    table->field[4]->store(bt_size_allocated, false);
    table->field[5]->store(bt_size_in_use, false);

    // split the dname
    {
        String database_name, table_name, dictionary_name;
        tokudb_split_dname(
            (const char*)dname->data,
            database_name,
            table_name,
            dictionary_name);
        table->field[6]->store(
            database_name.c_ptr(),
            database_name.length(),
            system_charset_info);
        table->field[7]->store(
            table_name.c_ptr(),
            table_name.length(),
            system_charset_info);
        table->field[8]->store(
            dictionary_name.c_ptr(),
            dictionary_name.length(),
            system_charset_info);
    }
    error = schema_table_store_record(thd, table);

exit:
    if (db) {
        int close_error = db->close(db, 0);
        if (error == 0)
            error = close_error;
    }
    return error;
}

int report_fractal_tree_info(TABLE* table, THD* thd) {
    int error;
    DB_TXN* txn = NULL;
    DBC* tmp_cursor = NULL;
    DBT curr_key;
    DBT curr_val;
    memset(&curr_key, 0, sizeof curr_key);
    memset(&curr_val, 0, sizeof curr_val);
    error = txn_begin(db_env, 0, &txn, DB_READ_UNCOMMITTED, thd);
    if (error) {
        goto cleanup;
    }
    error = db_env->get_cursor_for_directory(db_env, txn, &tmp_cursor);
    if (error) {
        goto cleanup;
    }
    while (error == 0) {
        error = tmp_cursor->c_get(tmp_cursor, &curr_key, &curr_val, DB_NEXT);
        if (!error) {
            error = report_fractal_tree_info_for_db(
                &curr_key,
                &curr_val,
                table,
                thd);
            if (error)
                error = 0; // ignore read uncommitted errors
        }
        if (!error && thd_kill_level(thd))
            error = ER_QUERY_INTERRUPTED;
    }
    if (error == DB_NOTFOUND) {
        error = 0;
    }
cleanup:
    if (tmp_cursor) {
        int r = tmp_cursor->c_close(tmp_cursor);
        assert(r == 0);
    }
    if (txn) {
        commit_txn(txn, 0);
    }
    return error;
}

#if MYSQL_VERSION_ID >= 50600
int fractal_tree_info_fill_table(THD* thd,
                                 TABLE_LIST* tables,
                                 TOKUDB_UNUSED(Item* cond)) {
#else
int fractal_tree_info_fill_table(THD* thd,
                                 TABLE_LIST* tables,
                                 TOKUDB_UNUSED(COND* cond)) {
#endif
    TOKUDB_DBUG_ENTER("");
    int error;
    TABLE* table = tables->table;

    // 3938: Get a read lock on the status flag, since we must
    // read it before safely proceeding
    rwlock_t_lock_read(tokudb_hton_initialized_lock);

    if (!tokudb_hton_initialized) {
        error = ER_PLUGIN_IS_NOT_LOADED;
        my_error(error, MYF(0), tokudb_hton_name);
    } else {
        error = report_fractal_tree_info(table, thd);
        if (error)
            my_error(ER_GET_ERRNO, MYF(0), error, tokudb_hton_name);
    }

    //3938: unlock the status flag lock
    tokudb_hton_initialized_lock.unlock();
    TOKUDB_DBUG_RETURN(error);
}

int fractal_tree_info_init(void* p) {
    ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
    schema->fields_info = fractal_tree_info_field_info;
    schema->fill_table = fractal_tree_info_fill_table;
    return 0;
}

int fractal_tree_info_done(TOKUDB_UNUSED(void* p)) {
    return 0;
}

st_mysql_plugin fractal_tree_info = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &fractal_tree_info_information_schema,
    "TokuDB_fractal_tree_info",
    "Percona",
    "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    fractal_tree_info_init,         /* plugin init */
    fractal_tree_info_done,         /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,
    NULL,                           /* status variables */
    NULL,                           /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
    tokudb::sysvars::version,
    MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
    NULL,                           /* config options */
    0,                              /* flags */
#endif
};



st_mysql_information_schema fractal_tree_block_map_information_schema = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

ST_FIELD_INFO fractal_tree_block_map_field_info[] = {
    {"dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"internal_file_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"checkpoint_count", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"blocknum", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"offset", 0, MYSQL_TYPE_LONGLONG, 0, MY_I_S_MAYBE_NULL, NULL, SKIP_OPEN_TABLE },
    {"size", 0, MYSQL_TYPE_LONGLONG, 0, MY_I_S_MAYBE_NULL, NULL, SKIP_OPEN_TABLE },
    {"table_schema", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_dictionary_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

struct report_fractal_tree_block_map_iterator_extra_t {
    int64_t num_rows;
    int64_t i;
    uint64_t* checkpoint_counts;
    int64_t* blocknums;
    int64_t* diskoffs;
    int64_t* sizes;
};

// This iterator is called while holding the blocktable lock.
// We should be as quick as possible.
// We don't want to do one call to get the number of rows, release the
// blocktable lock, and then do another call to get all the rows because
// the number of rows may change if we don't hold the lock.
// As a compromise, we'll do some mallocs inside the lock on the first call,
// but everything else should be fast.
int report_fractal_tree_block_map_iterator(
    uint64_t checkpoint_count,
    int64_t num_rows,
    int64_t blocknum,
    int64_t diskoff,
    int64_t size,
    void* iter_extra) {

    struct report_fractal_tree_block_map_iterator_extra_t* e =
        static_cast<struct report_fractal_tree_block_map_iterator_extra_t*>(iter_extra);

    assert(num_rows > 0);
    if (e->num_rows == 0) {
        e->checkpoint_counts =
            (uint64_t*)tokudb::memory::malloc(
                num_rows * (sizeof *e->checkpoint_counts),
                MYF(MY_WME|MY_ZEROFILL|MY_FAE));
        e->blocknums =
            (int64_t*)tokudb::memory::malloc(
                num_rows * (sizeof *e->blocknums),
                MYF(MY_WME|MY_ZEROFILL|MY_FAE));
        e->diskoffs =
            (int64_t*)tokudb::memory::malloc(
                num_rows * (sizeof *e->diskoffs),
                MYF(MY_WME|MY_ZEROFILL|MY_FAE));
        e->sizes =
            (int64_t*)tokudb::memory::malloc(
                num_rows * (sizeof *e->sizes),
                MYF(MY_WME|MY_ZEROFILL|MY_FAE));
        e->num_rows = num_rows;
    }

    e->checkpoint_counts[e->i] = checkpoint_count;
    e->blocknums[e->i] = blocknum;
    e->diskoffs[e->i] = diskoff;
    e->sizes[e->i] = size;
    ++(e->i);

    return 0;
}

int report_fractal_tree_block_map_for_db(
    const DBT* dname,
    const DBT* iname,
    TABLE* table,
    THD* thd) {

    int error;
    DB* db;
    // avoid struct initializers so that we can compile with older gcc versions
    report_fractal_tree_block_map_iterator_extra_t e = {};

    error = db_create(&db, db_env, 0);
    if (error) {
        goto exit;
    }
    error = db->open(db, NULL, (char *)dname->data, NULL, DB_BTREE, 0, 0666);
    if (error) {
        goto exit;
    }
    error = db->iterate_fractal_tree_block_map(
        db,
        report_fractal_tree_block_map_iterator,
        &e);
    {
        int close_error = db->close(db, 0);
        if (!error) {
            error = close_error;
        }
    }
    if (error) {
        goto exit;
    }

    // If not, we should have gotten an error and skipped this section of code
    assert(e.i == e.num_rows);
    for (int64_t i = 0; error == 0 && i < e.num_rows; ++i) {
        // We store the NULL terminator in the directory so it's included in the size.
        // See #5789
        // Recalculate and check just to be safe.
        size_t dname_len = strlen((const char*)dname->data);
        assert(dname_len == dname->size - 1);
        table->field[0]->store(
            (char*)dname->data,
            dname_len,
            system_charset_info);

        size_t iname_len = strlen((const char*)iname->data);
        assert(iname_len == iname->size - 1);
        table->field[1]->store(
            (char*)iname->data,
            iname_len,
            system_charset_info);

        table->field[2]->store(e.checkpoint_counts[i], false);
        table->field[3]->store(e.blocknums[i], false);
        static const int64_t freelist_null = -1;
        static const int64_t diskoff_unused = -2;
        if (e.diskoffs[i] == diskoff_unused || e.diskoffs[i] == freelist_null) {
            table->field[4]->set_null();
        } else {
            table->field[4]->set_notnull();
            table->field[4]->store(e.diskoffs[i], false);
        }
        static const int64_t size_is_free = -1;
        if (e.sizes[i] == size_is_free) {
            table->field[5]->set_null();
        } else {
            table->field[5]->set_notnull();
            table->field[5]->store(e.sizes[i], false);
        }

        // split the dname
        String database_name, table_name, dictionary_name;
        tokudb_split_dname(
            (const char*)dname->data,
            database_name,
            table_name,
            dictionary_name);
        table->field[6]->store(
            database_name.c_ptr(),
            database_name.length(),
            system_charset_info);
        table->field[7]->store(
            table_name.c_ptr(),
            table_name.length(),
            system_charset_info);
        table->field[8]->store(
            dictionary_name.c_ptr(),
            dictionary_name.length(),
            system_charset_info);

        error = schema_table_store_record(thd, table);
    }

exit:
    if (e.checkpoint_counts != NULL) {
        tokudb::memory::free(e.checkpoint_counts);
        e.checkpoint_counts = NULL;
    }
    if (e.blocknums != NULL) {
        tokudb::memory::free(e.blocknums);
        e.blocknums = NULL;
    }
    if (e.diskoffs != NULL) {
        tokudb::memory::free(e.diskoffs);
        e.diskoffs = NULL;
    }
    if (e.sizes != NULL) {
        tokudb::memory::free(e.sizes);
        e.sizes = NULL;
    }
    return error;
}

int report_fractal_tree_block_map(TABLE* table, THD* thd) {
    int error;
    DB_TXN* txn = NULL;
    DBC* tmp_cursor = NULL;
    DBT curr_key;
    DBT curr_val;
    memset(&curr_key, 0, sizeof curr_key);
    memset(&curr_val, 0, sizeof curr_val);
    error = txn_begin(db_env, 0, &txn, DB_READ_UNCOMMITTED, thd);
    if (error) {
        goto cleanup;
    }
    error = db_env->get_cursor_for_directory(db_env, txn, &tmp_cursor);
    if (error) {
        goto cleanup;
    }
    while (error == 0) {
        error = tmp_cursor->c_get(tmp_cursor, &curr_key, &curr_val, DB_NEXT);
        if (!error) {
            error = report_fractal_tree_block_map_for_db(
                &curr_key,
                &curr_val,
                table,
                thd);
        }
        if (!error && thd_kill_level(thd))
            error = ER_QUERY_INTERRUPTED;
    }
    if (error == DB_NOTFOUND) {
        error = 0;
    }
cleanup:
    if (tmp_cursor) {
        int r = tmp_cursor->c_close(tmp_cursor);
        assert(r == 0);
    }
    if (txn) {
        commit_txn(txn, 0);
    }
    return error;
}

#if MYSQL_VERSION_ID >= 50600
int fractal_tree_block_map_fill_table(
    THD* thd,
    TABLE_LIST* tables,
    TOKUDB_UNUSED(Item* cond)) {
#else
int fractal_tree_block_map_fill_table(
    THD* thd,
    TABLE_LIST* tables,
    TOKUDB_UNUSED(COND* cond)) {
#endif
    TOKUDB_DBUG_ENTER("");
    int error;
    TABLE* table = tables->table;

    // 3938: Get a read lock on the status flag, since we must
    // read it before safely proceeding
    rwlock_t_lock_read(tokudb_hton_initialized_lock);

    if (!tokudb_hton_initialized) {
        error = ER_PLUGIN_IS_NOT_LOADED;
        my_error(error, MYF(0), tokudb_hton_name);
    } else {
        error = report_fractal_tree_block_map(table, thd);
        if (error)
            my_error(ER_GET_ERRNO, MYF(0), error, tokudb_hton_name);
    }

    //3938: unlock the status flag lock
    tokudb_hton_initialized_lock.unlock();
    TOKUDB_DBUG_RETURN(error);
}

int fractal_tree_block_map_init(void* p) {
    ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
    schema->fields_info = fractal_tree_block_map_field_info;
    schema->fill_table = fractal_tree_block_map_fill_table;
    return 0;
}

int fractal_tree_block_map_done(TOKUDB_UNUSED(void *p)) {
    return 0;
}

st_mysql_plugin fractal_tree_block_map = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &fractal_tree_block_map_information_schema,
    "TokuDB_fractal_tree_block_map",
    "Percona",
    "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    fractal_tree_block_map_init,     /* plugin init */
    fractal_tree_block_map_done,     /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,
    NULL,                      /* status variables */
    NULL,                      /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
    tokudb::sysvars::version,
    MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
    NULL,                      /* config options */
    0,                         /* flags */
#endif
};


st_mysql_information_schema background_job_status_information_schema = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

ST_FIELD_INFO background_job_status_field_info[] = {
    {"id", 0, MYSQL_TYPE_LONGLONG, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"database_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"table_name", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"job_type", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"job_params", 256, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"scheduler", 32, MYSQL_TYPE_STRING, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"scheduled_time", 0, MYSQL_TYPE_DATETIME, 0, 0, NULL, SKIP_OPEN_TABLE },
    {"started_time", 0, MYSQL_TYPE_DATETIME, 0, MY_I_S_MAYBE_NULL, NULL, SKIP_OPEN_TABLE },
    {"status", 1024, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL, NULL, SKIP_OPEN_TABLE },
    {NULL, 0, MYSQL_TYPE_NULL, 0, 0, NULL, SKIP_OPEN_TABLE}
};

struct background_job_status_extra {
    THD* thd;
    TABLE* table;
};

void background_job_status_callback(
    tokudb::background::job_manager_t::job_t* job,
    void* extra) {

    background_job_status_extra* e =
        reinterpret_cast<background_job_status_extra*>(extra);

    THD* thd = e->thd;
    TABLE* table = e->table;
    const char* tmp = NULL;

    table->field[0]->store(job->id(), false);

    tmp = job->database();
    table->field[1]->store(tmp, strlen(tmp),  system_charset_info);

    tmp = job->table();
    table->field[2]->store(tmp, strlen(tmp),  system_charset_info);

    tmp = job->type();
    table->field[3]->store(tmp, strlen(tmp),  system_charset_info);

    tmp = job->parameters();
    table->field[4]->store(tmp, strlen(tmp),  system_charset_info);

    if (job->user_scheduled())
        table->field[5]->store("USER", strlen("USER"), system_charset_info);
    else
        table->field[5]->store("AUTO", strlen("AUTO"), system_charset_info);

    field_store_time_t(table->field[6], job->scheduled_time());
    field_store_time_t(table->field[7], job->started_time());

    tmp = job->status();
    if (tmp && tmp[0] != '\0') {
        table->field[8]->store(tmp, strlen(tmp), system_charset_info);
        table->field[8]->set_notnull();
    } else {
        table->field[8]->store(NULL, 0, system_charset_info);
        table->field[8]->set_null();
    }

    schema_table_store_record(thd, table);
}

int report_background_job_status(TABLE *table, THD *thd) {
    int error = 0;
    background_job_status_extra extra = {
        thd,
        table
    };
    tokudb::background::_job_manager->iterate_jobs(
        background_job_status_callback,
        &extra);
    return error;
}

#if MYSQL_VERSION_ID >= 50600
int background_job_status_fill_table(THD *thd, TABLE_LIST *tables, TOKUDB_UNUSED(Item *cond)) {
#else
int background_job_status_fill_table(THD *thd, TABLE_LIST *tables, TOKUDB_UNUSED(COND *cond)) {
#endif
    TOKUDB_DBUG_ENTER("");
    int error;
    TABLE* table = tables->table;

    rwlock_t_lock_read(tokudb_hton_initialized_lock);

    if (!tokudb_hton_initialized) {
        error = ER_PLUGIN_IS_NOT_LOADED;
        my_error(error, MYF(0), tokudb_hton_name);
    } else {
        error = report_background_job_status(table, thd);
        if (error)
            my_error(ER_GET_ERRNO, MYF(0), error, tokudb_hton_name);
    }

    tokudb_hton_initialized_lock.unlock();
    TOKUDB_DBUG_RETURN(error);
}

int background_job_status_init(void* p) {
    ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
    schema->fields_info = background_job_status_field_info;
    schema->fill_table = background_job_status_fill_table;
    return 0;
}

int background_job_status_done(TOKUDB_UNUSED(void* p)) {
    return 0;
}

st_mysql_plugin background_job_status = {
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &background_job_status_information_schema,
    "TokuDB_background_job_status",
    "Percona",
    "Percona TokuDB Storage Engine with Fractal Tree(tm) Technology",
    PLUGIN_LICENSE_GPL,
    background_job_status_init,     /* plugin init */
    background_job_status_done,     /* plugin deinit */
    TOKUDB_PLUGIN_VERSION,
    NULL,                      /* status variables */
    NULL,                      /* system variables */
#ifdef MARIA_PLUGIN_INTERFACE_VERSION
    tokudb::sysvars::version,
    MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
#else
    NULL,                      /* config options */
    0,                         /* flags */
#endif
};

} // namespace information_schema
} // namespace tokudb
