/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "test.h"

// Verify that a commit of a prepared txn in recovery retains the rows that it inserted.
// A checkpoint is taken after the rows are inserted and before and the txn prepare.

const int test_nrows = 1000000;

static void create_foo(DB_ENV *env, DB_TXN *txn) {
    int r;
    DB *db = nullptr;
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->open(db, txn, "foo.db", 0, DB_BTREE, DB_CREATE,  S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);
    r = db->close(db, 0);
    CKERR(r);
}

static void populate_foo(DB_ENV *env, DB_TXN *txn) {
    int r;
    DB *db = nullptr;
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->open(db, txn, "foo.db", 0, DB_BTREE, 0, 0);
    CKERR(r);

    for (int i = 0; i < test_nrows; i++) {
        int k = htonl(i);
        DBT key = { .data = &k, .size = sizeof k }; DBT val = { .data = &i, .size = sizeof i };
        r = db->put(db, txn, &key, &val, 0);
        CKERR(r);
    }

    r = db->close(db, 0);
    CKERR(r);
}

static void check_foo(DB_ENV *env, DB_TXN *txn) {
    int r;
    DB *db;
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->open(db, txn, "foo.db", 0, DB_BTREE, 0, 0);
    CKERR(r);

    DBC *c = nullptr;
    r = db->cursor(db, txn, &c, 0);
    CKERR(r);

    DBT key = {}; key.flags = DB_DBT_REALLOC;
    DBT val = {}; val.flags = DB_DBT_REALLOC;
    int i;
    for (i = 0; 1; i++) {
        r = c->c_get(c, &key, &val, DB_NEXT);
        if (r != 0)
            break;
        int k, v;
        assert(key.size == sizeof k);
        memcpy(&k, key.data, key.size);
        assert(k == (int) htonl(i));
        assert(val.size == sizeof v);
        memcpy(&v, val.data, val.size);
        assert(v == i);
    }
    assert(i == test_nrows);
    toku_free(key.data);
    toku_free(val.data);
    
    r = c->c_close(c);
    CKERR(r);
    
    r = db->close(db, 0);
    CKERR(r);
}

static void create_prepared_txn(void) {
    int r;

    DB_ENV *env = nullptr;
    r = db_env_create(&env, 0);
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, 
                  DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, 
                  S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    DB_TXN *txn = nullptr;
    r = env->txn_begin(env, nullptr, &txn, 0);
    CKERR(r);

    create_foo(env, txn);
    
    r = txn->commit(txn, 0);
    CKERR(r);

    r = env->txn_begin(env, nullptr, &txn, 0);
    CKERR(r);

    populate_foo(env, txn);

    r = env->txn_checkpoint(env, 0, 0, 0);
    CKERR(r);

    TOKU_XA_XID xid = { 0x1234, 8, 9 };
    for (int i = 0; i < 8+9; i++) {
        xid.data[i] = i;
    }
    r = txn->xa_prepare(txn, &xid, 0);
    CKERR(r);

    // discard the txn so that we can close the env and run xa recovery later
    r = txn->discard(txn, 0);
    CKERR(r);

    r = env->close(env, TOKUFT_DIRTY_SHUTDOWN);
    CKERR(r);
}

static void run_xa_recovery(void) {
    int r;

    DB_ENV *env;
    r = db_env_create(&env, 0);
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, 
                  DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE | DB_RECOVER,
                  S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    // get prepared xid
    long count;
    TOKU_XA_XID xid;
    r = env->txn_xa_recover(env, &xid, 1, &count, DB_FIRST);
    CKERR(r);

    // commit it
    DB_TXN *txn = nullptr;
    r = env->get_txn_from_xid(env, &xid, &txn);
    CKERR(r);
    r = txn->commit(txn, 0);
    CKERR(r);

    r = env->txn_begin(env, nullptr, &txn, 0);
    CKERR(r);

    check_foo(env, txn);

    r = txn->commit(txn, 0);
    CKERR(r);

    r = env->close(env, 0);
    CKERR(r);
}

int test_main (int argc, char *const argv[]) {
    default_parse_args(argc, argv);

    // init the env directory
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);   
    CKERR(r);

    // run the test
    create_prepared_txn();
    run_xa_recovery();

    return 0;
}
