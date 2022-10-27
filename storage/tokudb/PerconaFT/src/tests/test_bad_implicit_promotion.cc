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

// Test the following scenario:
// Begin A
// A deletes key K
// A aborts
// Begin B
// B deletes key K-1
// B deletes key K
// B deletes key K+1
// B commits
// Begin C
// C queries K, should read K (not the delete!).
//
// An incorrect mvcc implementation would 'implicitly' promote
// A's delete to committed, based on the fact that the oldest
// referenced xid at the time of injection for key k-1 and k+1
// is greater than A's xid.

static void test_insert_bad_implicit_promotion(void) {
    int r;

    DB_ENV *env;
    r = db_env_create(&env, 0); CKERR(r);
    r = env->set_cachesize(env, 1, 0, 1); CKERR(r); // 1gb cache so this test fits in memory
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE+DB_PRIVATE+DB_INIT_MPOOL+DB_INIT_TXN, 0); CKERR(r);

    DB *db;
    r = db_create(&db, env, 0); CKERR(r);
    r = db->set_pagesize(db, 4096); CKERR(r);
    r = db->open(db, NULL, "db", NULL, DB_BTREE, DB_CREATE, 0666); CKERR(r);

    const int val_size = 512;

    DBT key;
    DBT val;
    char *XMALLOC_N(val_size, val_buf);
    memset(val_buf, 'x', val_size);
    dbt_init(&val, val_buf, val_size);

    // Insert rows [0, N]
    const int N = 1000;
    for (int i = 0; i < N; i++) {
        int k = toku_htonl(i);
        dbt_init(&key, &k, sizeof(k));
        r = db->put(db, NULL, &key, &val, 0); CKERR(r);
    }

    int key_500 = toku_htonl(500);
    int key_499 = toku_htonl(499);
    int key_501 = toku_htonl(501);
    // sanity check our keys
    r = db->get(db, NULL, dbt_init(&key, &key_500, sizeof(key_500)), &val, 0); CKERR(r);
    r = db->get(db, NULL, dbt_init(&key, &key_500, sizeof(key_499)), &val, 0); CKERR(r);
    r = db->get(db, NULL, dbt_init(&key, &key_500, sizeof(key_501)), &val, 0); CKERR(r);

    // Abort a delete for key 500
    DB_TXN *txn_A;
    r = env->txn_begin(env, NULL, &txn_A, DB_SERIALIZABLE); CKERR(r);
    dbt_init(&key, &key_500, sizeof(key_500));
    r = db->del(db, txn_A, &key, DB_DELETE_ANY); CKERR(r);
    r = txn_A->abort(txn_A); CKERR(r);

    // Commit two deletes on keys 499 and 501. This should inject
    // at least one message in the same buffer that has the delete/abort
    // messages for key 500.
    DB_TXN *txn_B;
    r = env->txn_begin(env, NULL, &txn_B, DB_SERIALIZABLE); CKERR(r);
    dbt_init(&key, &key_499, sizeof(key_499));
    r = db->del(db, txn_B, &key, DB_DELETE_ANY); CKERR(r);
    dbt_init(&key, &key_501, sizeof(key_501));
    r = db->del(db, txn_B, &key, DB_DELETE_ANY); CKERR(r);
    r = txn_B->commit(txn_B, 0); CKERR(r);

    // No transactions are live - so when we create txn C, the oldest
    // referenced xid will be txn C. If our implicit promotion logic is
    // wrong, we will use txn C's xid to promote the delete on key 500
    // before the abort message hits it, and C's query will return nothing.
    DB_TXN *txn_C;
    dbt_init(&key, &key_500, sizeof(key_500));
    r = env->txn_begin(env, NULL, &txn_C, DB_TXN_SNAPSHOT); CKERR(r);
    r = db->get(db, txn_C, &key, &val, 0); CKERR(r);
    r = txn_C->commit(txn_C, 0); CKERR(r);

    toku_free(val_buf);
    r = db->close(db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
}

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);

    test_insert_bad_implicit_promotion();

    return 0;
}
