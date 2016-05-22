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


static void test_insert_many_gc(void) {
    int r;

    DB_ENV *env;
    r = db_env_create(&env, 0); CKERR(r);
    r = env->set_cachesize(env, 1, 0, 1); CKERR(r); // 1gb cache so this test fits in memory
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE+DB_PRIVATE+DB_INIT_MPOOL+DB_INIT_TXN, 0); CKERR(r);

    DB *db;
    r = db_create(&db, env, 0); CKERR(r);
    r = db->open(db, NULL, "db", NULL, DB_BTREE, DB_CREATE, 0666); CKERR(r);

    const int val_size = 1 * 1024 * 1024;

    // Begin a snapshot transaction, which should prevent simple garbage collection
    // from being effective. Only full garbage collection can prevent many inserts
    // into a single leaf node from growing out of control.
    DB_TXN *snapshot_txn;
    r = env->txn_begin(env, NULL, &snapshot_txn, DB_TXN_SNAPSHOT); CKERR(r);

    DBT key;
    int k = 0;
    dbt_init(&key, &k, sizeof(k));

    DBT val;
    char *XMALLOC_N(val_size, val_buf);
    memset(val_buf, 0, val_size);
    dbt_init(&val, val_buf, val_size);

    // Keep overwriting the same row over and over.
    const int N = 75;
    for (int i = 0; i < N; i++) {
        r = db->put(db, NULL, &key, &val, 0); CKERR(r);
    }

    // Full garbage collection should have prevented the leaf node
    // from having an MVCC stack of size 'N'. At the time of this
    // writing, we run full GC on leaf-inject when the leaf is
    // 32mb or larger. A good invariant is that the max LE size
    // never grew larger than 35mb and that the max commited xr stack
    // length never exceeded 35
    const uint64_t le_max_memsize = get_engine_status_val(env, "LE_MAX_MEMSIZE");
    const uint64_t le_max_committed_xr = get_engine_status_val(env, "LE_MAX_COMMITTED_XR");
    invariant(le_max_memsize <= 35 * 1024 * 1024);
    invariant(le_max_committed_xr <= 35);

    r = snapshot_txn->commit(snapshot_txn, 0); CKERR(r);

    toku_free(val_buf);
    r = db->close(db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
}

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);

    test_insert_many_gc();

    return 0;
}
