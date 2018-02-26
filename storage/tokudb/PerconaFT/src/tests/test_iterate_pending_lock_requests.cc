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

#include <portability/toku_pthread.h>

static DB_ENV *env;
static DB *db;
static DB_TXN *txn1, *txn2, *txn3;
static const char *dname = "iterate_pending_requests_dname";
static const int magic_key = 100;
static int iterate_callback_called;
toku_pthread_t thread1, thread2;

// Verify the state of the world
static int iterate_callback(DB *_db, uint64_t requesting_txnid,
                            const DBT *left_key, const DBT *right_key,
                            uint64_t blocking_txnid, uint64_t start_time, void *extra) {
    iterate_callback_called++;
    invariant(extra == nullptr);
    invariant(strcmp(_db->get_dname(_db), db->get_dname(db)) == 0);
    invariant(start_time > 0);
    invariant(*reinterpret_cast<int *>(left_key->data) == magic_key);
    invariant(*reinterpret_cast<int *>(right_key->data) == magic_key);
    invariant(blocking_txnid == txn1->id64(txn1));
    invariant(requesting_txnid == txn2->id64(txn2) || requesting_txnid == txn3->id64(txn3));
    return 0;
}

static void acquire_lock(DB_TXN *txn, int key) {
    int val = 0;
    DBT k, v;
    dbt_init(&k, &key, sizeof(int));
    dbt_init(&v, &val, sizeof(int));
    (void) db->put(db, txn, &k, &v, 0);
}

struct acquire_lock_extra {
    acquire_lock_extra(DB_TXN *x, int k) :
        txn(x), key(k) {
    }
    DB_TXN *txn;
    int key;
};

static void *acquire_lock_thread(void *arg) {
    acquire_lock_extra *info = reinterpret_cast<acquire_lock_extra *>(arg);
    acquire_lock(info->txn, info->key);
    return NULL;
}

int test_main(int UU(argc), char *const UU(argv[])) {
    int r;
    const int env_flags = DB_INIT_MPOOL | DB_CREATE | DB_THREAD |
        DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN | DB_PRIVATE;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, 0755); CKERR(r);

    r = db_env_create(&env, 0); CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, env_flags, 0755); CKERR(r);
    r = env->set_lock_timeout(env, 4000, nullptr);

    r = db_create(&db, env, 0); CKERR(r);
    r = db->open(db, NULL, dname, NULL, DB_BTREE, DB_CREATE, 0777); CKERR(r);

    r = env->txn_begin(env, NULL, &txn1, DB_SERIALIZABLE); CKERR(r);
    r = env->txn_begin(env, NULL, &txn2, DB_SERIALIZABLE); CKERR(r);
    r = env->txn_begin(env, NULL, &txn3, DB_SERIALIZABLE); CKERR(r);

    // Extremely simple test. Get lock [0, 0] on txn1, then asynchronously
    // attempt to get that lock in txn2 and txn3. The iterate callback
    // verifies that two waiters exist for [0, 0] and that txn1 is
    // the blocking txn.

    acquire_lock(txn1, magic_key);

    acquire_lock_extra e1(txn2, magic_key);
    r = toku_pthread_create(
        toku_uninstrumented, &thread1, nullptr, acquire_lock_thread, &e1);
    CKERR(r);
    acquire_lock_extra e2(txn3, magic_key);
    r = toku_pthread_create(
        toku_uninstrumented, &thread2, nullptr, acquire_lock_thread, &e2);
    CKERR(r);

    sleep(1);
    r = env->iterate_pending_lock_requests(env, iterate_callback, NULL);
    CKERR(r);
    invariant(iterate_callback_called == 2);

    void *v;
    r = toku_pthread_join(thread1, &v); CKERR(r);
    r = toku_pthread_join(thread2, &v); CKERR(r);

    r = txn1->commit(txn1, 0); CKERR(r);
    r = txn2->commit(txn2, 0); CKERR(r);
    r = txn3->commit(txn3, 0); CKERR(r);

    r = db->close(db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
    return 0;
}
