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

#ifndef _TOKUDB_STATUS_H
#define _TOKUDB_STATUS_H

// These are keys that will be used for retrieving metadata in status.tokudb
// To get the version, one looks up the value associated with key hatoku_version
// in status.tokudb
typedef ulonglong HA_METADATA_KEY;
#define hatoku_old_version 0
#define hatoku_capabilities 1
#define hatoku_max_ai 2 //maximum auto increment value found so far
#define hatoku_ai_create_value 3
#define hatoku_key_name 4
#define hatoku_frm_data 5
#define hatoku_new_version 6
#define hatoku_cardinality 7

// use a very small pagesize for the status dictionary
#define status_dict_pagesize 1024

namespace tokudb {
namespace metadata {

// get the value for a given key in the status dictionary.
// copy the value to the supplied buffer.
// returns 0 if successful.
int read(
    DB* status_db,
    DB_TXN* txn,
    HA_METADATA_KEY k,
    void* p,
    size_t s,
    size_t* sp) {

    DBT key = {};
    key.data = &k;
    key.size = sizeof(k);
    DBT val = {};
    val.data = p;
    val.ulen = (uint32_t)s;
    val.flags = DB_DBT_USERMEM;
    int error = status_db->get(status_db, txn, &key, &val, 0);
    if (error == 0) {
        *sp = val.size;
    }
    return error;
}

// get the value for a given key in the status dictionary.
// put the value in a realloced buffer.
// returns 0 if successful.
int read_realloc(
    DB* status_db,
    DB_TXN* txn,
    HA_METADATA_KEY k,
    void** pp,
    size_t* sp) {

    DBT key = {};
    key.data = &k;
    key.size = sizeof(k);
    DBT val = {};
    val.data = *pp;
    val.size = (uint32_t)*sp;
    val.flags = DB_DBT_REALLOC;
    int error = status_db->get(status_db, txn, &key, &val, 0);
    if (error == 0) {
        *pp = val.data;
        *sp = val.size;
    }
    return error;
}

// write a key value pair into the status dictionary,
// overwriting the previous value if any.
// auto create a txn if necessary.
// returns 0 if successful.
int write_low(
    DB* status_db,
    void* key_data,
    uint key_size,
    void* val_data,
    uint val_size,
    DB_TXN *txn) {

    DBT key = {};
    key.data = key_data;
    key.size = key_size;
    DBT value = {};
    value.data = val_data;
    value.size = val_size;
    int error = status_db->put(status_db, txn, &key, &value, 0);
    return error;
}

// write a key value pair into the status dictionary,
// overwriting the previous value if any.
// the key must be a HA_METADATA_KEY.
// returns 0 if successful.
int write(
    DB* status_db,
    HA_METADATA_KEY curr_key_data,
    void* val,
    size_t val_size,
    DB_TXN* txn) {

    return
        tokudb::metadata::write_low(
            status_db,
            &curr_key_data,
            sizeof(curr_key_data),
            val,
            val_size,
            txn);
}

// remove a key from the status dictionary.
// auto create a txn if necessary.
// returns 0 if successful.
int remove_low(
    DB* status_db,
    void* key_data,
    uint key_size,
    DB_TXN* txn) {

    DBT key = {};
    key.data = key_data;
    key.size = key_size;
    int error = status_db->del(status_db, txn, &key, DB_DELETE_ANY);
    return error;
}

// remove a key from the status dictionary.
// the key must be a HA_METADATA_KEY
// returns 0 if successful.
int remove(
    DB* status_db,
    HA_METADATA_KEY curr_key_data,
    DB_TXN* txn) {
    return
        tokudb::metadata::remove_low(
            status_db,
            &curr_key_data,
            sizeof(curr_key_data),
            txn);
}

int close(DB** status_db_ptr) {
    int error = 0;
    DB* status_db = *status_db_ptr;
    if (status_db) {
        error = status_db->close(status_db, 0);
        if (error == 0)
            *status_db_ptr = NULL;
    }
    return error;
}

int create(
    DB_ENV* env,
    DB** status_db_ptr,
    const char* name,
    DB_TXN* txn) {

    int error;
    DB *status_db = NULL;

    error = db_create(&status_db, env, 0);
    if (error == 0) {
        error = status_db->set_pagesize(status_db, status_dict_pagesize);
    }
    if (error == 0) {
        error =
            status_db->open(
                status_db,
                txn,
                name,
                NULL,
                DB_BTREE, DB_CREATE | DB_EXCL,
                S_IWUSR);
    }
    if (error == 0) {
        *status_db_ptr = status_db;
    } else {
        int r = tokudb::metadata::close(&status_db);
        assert_always(r == 0);
    }
    return error;
}

int open(
    DB_ENV* env,
    DB** status_db_ptr,
    const char* name,
    DB_TXN* txn) {

    int error = 0;
    DB* status_db = NULL;
    error = db_create(&status_db, env, 0);
    if (error == 0) {
        error =
            status_db->open(
                status_db,
                txn,
                name,
                NULL,
                DB_BTREE,
                DB_THREAD,
                S_IWUSR);
    }
    if (error == 0) {
        uint32_t pagesize = 0;
        error = status_db->get_pagesize(status_db, &pagesize);
        if (error == 0 && pagesize > status_dict_pagesize) {
            error =
                status_db->change_pagesize(status_db, status_dict_pagesize);
        }
    }
    if (error == 0) {
        *status_db_ptr = status_db;
    } else {
        int r = tokudb::metadata::close(&status_db);
        assert_always(r == 0);
    }
    return error;
}

int strip_frm_data(DB_ENV* env) {
    int r;
    DB_TXN* txn = NULL;

    fprintf(stderr, "TokuDB strip_frm_data : Beginning stripping process.\n");

    r = db_env->txn_begin(env, NULL, &txn, 0);
    assert_always(r == 0);

    DBC* c = NULL;
    r = env->get_cursor_for_directory(env, txn, &c);
    assert_always(r == 0);

    DBT key = { };
    key.flags = DB_DBT_REALLOC;

    DBT val = { };
    val.flags = DB_DBT_REALLOC;
    while (1) {
        r = c->c_get(c, &key, &val, DB_NEXT);
        if (r == DB_NOTFOUND)
            break;
        const char* dname = (const char*) key.data;
        const char* iname = (const char*) val.data;
        assert_always(r == 0);

        if (strstr(iname, "_status_")) {
            fprintf(
                stderr,
                "TokuDB strip_frm_data : stripping from dname=%s iname=%s\n",
                dname,
                iname);

            DB* status_db;
            r = tokudb::metadata::open(db_env, &status_db, dname, txn);
            if (r != 0) {
                fprintf(
                    stderr,
                    "TokuDB strip_frm_data : unable to open status file %s, "
                    "error = %d\n",
                    dname,
                    r);
                continue;
            }

            // GOL : this is a godawful hack. The inventors of this did not
            // think it would be a good idea to use some kind of magic
            // identifier k/v pair so that you can in fact tell a proper status
            // file from any other file that might have the string _status_ in
            // it. Out in ha_tokudb::create, when the status file is initially
            // created, it is immediately populated with:
            //    uint hatoku_new_version=HA_TOKU_VERSION=4 and
            //    uint hatoku_capabilities=HA_TOKU_CAP=0
            // Since I can't count on the fact that these values are/were
            // _always_ 4 and 0, I can count on the fact that they _must_ be
            // there and the _must_ be sizeof(uint). That will at least give us
            // a much better idea that these are in fact status files.
            void* p = NULL;
            size_t sz;
            r =
                tokudb::metadata::read_realloc(
                    status_db,
                    txn,
                    hatoku_new_version,
                    &p,
                    &sz);
            if (r != 0) {
                fprintf(
                    stderr,
                    "TokuDB strip_frm_data : does not look like a real TokuDB "
                    "status file, new_verion is missing, leaving alone %s \n",
                    dname);

                r = tokudb::metadata::close(&status_db);
                assert_always(r == 0);
                continue;
            } else if (sz != sizeof(uint)) {
                fprintf(
                    stderr,
                    "TokuDB strip_frm_data : does not look like a real TokuDB "
                    "status file, new_verion is the wrong size, "
                    "leaving alone %s \n",
                    dname);

                tokudb::memory::free(p);
                r = tokudb::metadata::close(&status_db);
                assert_always(r == 0);
                continue;
            }
            tokudb::memory::free(p);
            p = NULL;

            r =
                tokudb::metadata::read_realloc(
                    status_db,
                    txn,
                    hatoku_capabilities,
                    &p,
                    &sz);
            if (r != 0) {
                fprintf(
                    stderr,
                    "TokuDB strip_frm_data : does not look like a real TokuDB "
                    "status file, capabilities is missing, leaving alone %s \n",
                    dname);

                r = tokudb::metadata::close(&status_db);
                assert_always(r == 0);
                continue;
            } else if (sz != sizeof(uint)) {
                fprintf(
                    stderr,
                    "TokuDB strip_frm_data : does not look like a real TokuDB "
                    "status file, capabilities is the wrong size, "
                    "leaving alone %s \n",
                    dname);

                tokudb::memory::free(p);
                r = tokudb::metadata::close(&status_db);
                assert_always(r == 0);
                continue;
            }
            tokudb::memory::free(p);

            // OK, st this point, it is probably a status file, not 100% but
            // it looks like it :(
            r = tokudb::metadata::remove(status_db, hatoku_frm_data, txn);
            if (r != 0) {
                fprintf(
                    stderr,
                    "TokuDB strip_frm_data : unable to find/strip frm data "
                    "from status file %s, error = %d\n",
                    dname,
                    r);
            }

            r = tokudb::metadata::close(&status_db);
            assert_always(r == 0);
        }
    }
    tokudb::memory::free(key.data);
    tokudb::memory::free(val.data);

    fprintf(
        stderr,
        "TokuDB strip_frm_data : Stripping process complete, beginning "
        "commit, this may take some time.\n");

    r = c->c_close(c);
    assert_always(r == 0);

    r = txn->commit(txn, 0);
    assert_always(r == 0);

    fprintf(
        stderr,
        "TokuDB strip_frm_data : Commit complete, resuming server init "
        "process.");

    return 0;
}

} // namespace metadata
} // namespace tokudb
#endif
