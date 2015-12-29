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

// Create a lot of dirty nodes, kick off a checkpoint, and close the environment.
// Measure the time it takes to close the environment since we are speeding up that
// function.

#include "test.h"
#include <toku_time.h>

// Insert max_rows key/val pairs into the db
static void do_inserts(DB_ENV *env, DB *db, uint64_t max_rows, size_t val_size) {
    char val_data[val_size]; memset(val_data, 0, val_size);
    int r;
    DB_TXN *txn = nullptr;
    r = env->txn_begin(env, nullptr, &txn, 0);
    CKERR(r);

    for (uint64_t i = 1; i <= max_rows; i++) {
        // pick a sequential key but it does not matter for this test.
        uint64_t k[2] = {
            htonl(i), random64(),
        };
        DBT key = { .data = k, .size = sizeof k };
        DBT val = { .data = val_data, .size = (uint32_t) val_size };
        r = db->put(db, txn, &key, &val, 0);
        CKERR(r);

        if ((i % 1000) == 0) {
            if (verbose)
                fprintf(stderr, "put %" PRIu64 "\n", i);
            r = txn->commit(txn, 0);
            CKERR(r);
            r = env->txn_begin(env, nullptr, &txn, 0);
            CKERR(r);
        }
    }

    r = txn->commit(txn, 0);
    CKERR(r);
}

// Create a cache with a lot of dirty nodes, kick off a checkpoint, and measure the time to
// close the environment.
static void big_shutdown(void) {
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

    do_inserts(env, db, 1000000, 1024);

    // kick the checkpoint thread
    if (verbose)
        fprintf(stderr, "env->checkpointing_set_period\n");
    r = env->checkpointing_set_period(env, 2);
    CKERR(r);
    sleep(3);

    if (verbose)
        fprintf(stderr, "db->close\n");
    r = db->close(db, 0);
    CKERR(r);

    // measure the shutdown time
    uint64_t tstart = toku_current_time_microsec();
    if (verbose)
        fprintf(stderr, "env->close\n");
    r = env->close(env, 0);
    CKERR(r);
    uint64_t tend = toku_current_time_microsec();
    if (verbose)
        fprintf(stderr, "env->close complete %" PRIu64 " sec\n", (tend - tstart)/1000000);
}

int test_main (int argc, char *const argv[]) {
    default_parse_args(argc, argv);

    // init the env directory
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    // run the test
    big_shutdown();

    return 0;
}
