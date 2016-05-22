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

// Test dirty upgrade.
// Generate a rollback log that requires recovery.

#include "test.h"

// Insert max_rows key/val pairs into the db
static void do_inserts(DB_TXN *txn, DB *db, uint64_t max_rows, size_t val_size) {
    char val_data[val_size]; memset(val_data, 0, val_size);
    int r;

    for (uint64_t i = 0; i < max_rows; i++) {
        // pick a sequential key but it does not matter for this test.
        uint64_t k[2] = {
            htonl(i), random64(),
        };

        DBT key = { .data = k, .size = sizeof k };
        DBT val = { .data = val_data, .size = (uint32_t) val_size };
        r = db->put(db, txn, &key, &val, 0);
        CKERR(r);
    }
}

static void run_test(uint64_t num_rows, size_t val_size, bool do_crash) {
    int r;

    DB_ENV *env = nullptr;
    r = db_env_create(&env, 0);
    CKERR(r);
    r = env->set_cachesize(env, 8, 0, 1);
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME,
                  DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE,
                  S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    DB *db = nullptr;
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->open(db, nullptr, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    r = env->txn_checkpoint(env, 0, 0, 0);
    CKERR(r);

    DB_TXN *txn = nullptr;
    r = env->txn_begin(env, nullptr, &txn, 0);
    CKERR(r);

    do_inserts(txn, db, num_rows, val_size);

    r = env->txn_checkpoint(env, 0, 0, 0);
    CKERR(r);

    r = txn->commit(txn, 0);
    CKERR(r);

    if (do_crash)
        assert(0); // crash on purpose

    r = db->close(db, 0);
    CKERR(r);

    r = env->close(env, 0);
    CKERR(r);
}

static void do_verify(DB_ENV *env, DB *db, uint64_t num_rows, size_t val_size UU()) {
    int r;
    DB_TXN *txn = nullptr;
    r = env->txn_begin(env, nullptr, &txn, 0);
    CKERR(r);

    DBC *c = nullptr;
    r = db->cursor(db, txn, &c, 0);
    CKERR(r);

    uint64_t i = 0;
    while (1) {
        DBT key = {};
        DBT val = {};
        r = c->c_get(c, &key, &val, DB_NEXT);
        if (r == DB_NOTFOUND)
            break;
        CKERR(r);
        assert(key.size == 16);
        uint64_t k[2];
        memcpy(k, key.data, key.size);
        assert(htonl(k[0]) == i);
        assert(val.size == val_size);
        i++;
    }
    assert(i == num_rows);

    r = c->c_close(c);
    CKERR(r);

    r = txn->commit(txn, 0);
    CKERR(r);
}

static void run_recover(uint64_t num_rows, size_t val_size) {
    int r;

    DB_ENV *env = nullptr;
    r = db_env_create(&env, 0);
    CKERR(r);
    r = env->set_cachesize(env, 8, 0, 1);
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME,
                  DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE | DB_RECOVER,
                  S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    DB *db = nullptr;
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->open(db, nullptr, "foo.db", 0, DB_BTREE, 0, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    do_verify(env, db, num_rows, val_size);

    r = db->close(db, 0);
    CKERR(r);

    r = env->close(env, 0);
    CKERR(r);
}

int test_main (int argc, char *const argv[]) {
    bool do_test = false;
    bool do_recover = false;
    bool do_crash = true;
    uint64_t num_rows = 1;
    size_t val_size = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            if (verbose > 0) verbose--;
            continue;
        }
        if (strcmp(argv[i], "--test") == 0) {
            do_test = true;
            continue;
        }
        if (strcmp(argv[i], "--recover") == 0) {
            do_recover = true;
            continue;
        }
        if (strcmp(argv[i], "--crash") == 0 && i+1 < argc) {
            do_crash = atoi(argv[++i]);
            continue;
        }
    }
    if (do_test) {
        // init the env directory
        toku_os_recursive_delete(TOKU_TEST_FILENAME);
        int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
        CKERR(r);
        run_test(num_rows, val_size, do_crash);
    }
    if (do_recover) {
        run_recover(num_rows, val_size);
    }

    return 0;
}
