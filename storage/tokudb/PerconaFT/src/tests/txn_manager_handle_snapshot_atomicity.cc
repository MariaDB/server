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

//In response to the read-commit crash bug in the sysbench, this test is created to test
//the atomicity of the txn manager when handling the child txn snapshot.
//The test is supposed to fail before the read-commit-fix.

#include "test.h"
#include "toku_pthread.h"
#include "ydb.h"
struct test_sync {
    int state;
    toku_mutex_t lock;
    toku_cond_t cv;
};

static void test_sync_init(struct test_sync *UU(sync)) {
#if TOKU_DEBUG_TXN_SYNC
    sync->state = 0;
    toku_mutex_init(toku_uninstrumented, &sync->lock, nullptr);
    toku_cond_init(toku_uninstrumented, &sync->cv, nullptr);
#endif
}

static void test_sync_destroy(struct test_sync *UU(sync)) {
#if TOKU_DEBUG_TXN_SYNC
    toku_mutex_destroy(&sync->lock);
    toku_cond_destroy(&sync->cv);
#endif
}

static void test_sync_sleep(struct test_sync *UU(sync), int UU(new_state)) {
#if TOKU_DEBUG_TXN_SYNC
    toku_mutex_lock(&sync->lock);
    while (sync->state != new_state) {
        toku_cond_wait(&sync->cv, &sync->lock);
    }
    toku_mutex_unlock(&sync->lock);
#endif
}

static void test_sync_next_state(struct test_sync *UU(sync)) {
#if TOKU_DEBUG_TXN_SYNC
    toku_mutex_lock(&sync->lock);
    sync->state++;
    toku_cond_broadcast(&sync->cv);
    toku_mutex_unlock(&sync->lock);
#endif
}


struct start_txn_arg {
    DB_ENV *env;
    DB *db;
    DB_TXN * parent;
};

static struct test_sync sync_s;

static void test_callback(pthread_t self_tid, void * extra) {
    pthread_t **p = (pthread_t **) extra;
    pthread_t tid_1 = *p[0];
    pthread_t tid_2 = *p[1];
    assert(pthread_equal(self_tid, tid_2));
    printf("%s: the thread[%" PRIu64 "] is going to wait...\n", __func__, reinterpret_cast<uint64_t>(tid_1));
    test_sync_next_state(&sync_s);
    sleep(3);
    //test_sync_sleep(&sync_s,3);
    //using test_sync_sleep/test_sync_next_state pair can sync threads better, however
    //after the fix, this might cause a deadlock. just simply use sleep to do a proof-
    //of-concept test. 
    printf("%s: the thread[%" PRIu64 "] is resuming...\n", __func__, reinterpret_cast<uint64_t>(tid_1));
    return;
}

static void * start_txn2(void * extra) {
    struct start_txn_arg * args = (struct start_txn_arg *) extra;
    DB_ENV * env = args -> env;
    DB * db = args->db;
    DB_TXN * parent = args->parent;
    test_sync_sleep(&sync_s, 1);
    printf("start %s [thread %" PRIu64 "]\n", __func__, reinterpret_cast<uint64_t>(pthread_self()));
    DB_TXN *txn;
    int r = env->txn_begin(env, parent, &txn,  DB_READ_COMMITTED);
    assert(r == 0);
    //do some random things...
    DBT key, data;
    dbt_init(&key, "hello", 6);
    dbt_init(&data, "world", 6);
    db->put(db, txn, &key, &data, 0);
    db->get(db, txn, &key, &data, 0);
   
    r = txn->commit(txn, 0);
    assert(r == 0);
    printf("%s done[thread %" PRIu64 "]\n", __func__, reinterpret_cast<uint64_t>(pthread_self()));
    return extra;
}

static void * start_txn1(void * extra) {
    struct start_txn_arg * args = (struct start_txn_arg *) extra;
    DB_ENV * env = args -> env;
    DB * db = args->db;
    printf("start %s: [thread %" PRIu64 "]\n", __func__, reinterpret_cast<uint64_t>(pthread_self()));
    DB_TXN *txn;
    int r = env->txn_begin(env, NULL, &txn,  DB_READ_COMMITTED);
    assert(r == 0);
    printf("%s: txn began by [thread %" PRIu64 "], will wait\n", __func__, reinterpret_cast<uint64_t>(pthread_self()));
    test_sync_next_state(&sync_s);
    test_sync_sleep(&sync_s,2);
    printf("%s: [thread %" PRIu64 "] resumed\n", __func__, reinterpret_cast<uint64_t>(pthread_self()));
    //do some random things...
    DBT key, data;
    dbt_init(&key, "hello", 6);
    dbt_init(&data, "world", 6);
    db->put(db, txn, &key, &data, 0);
    db->get(db, txn, &key, &data, 0);
    r = txn->commit(txn, 0);
    assert(r == 0);
    printf("%s: done[thread %" PRIu64 "]\n", __func__, reinterpret_cast<uint64_t>(pthread_self()));
    //test_sync_next_state(&sync_s);
    return extra;
}

int test_main (int UU(argc), char * const UU(argv[])) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);

    DB_ENV *env;
    r = db_env_create(&env, 0);
    assert(r == 0);

    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);

    DB *db = NULL;
    r = db_create(&db, env, 0);
    assert(r == 0);

    r = db->open(db, NULL, "testit", NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);

    DB_TXN * parent = NULL;
    r = env->txn_begin(env, 0, &parent, DB_READ_COMMITTED);
    assert(r == 0);

    ZERO_STRUCT(sync_s);
    test_sync_init(&sync_s);

    pthread_t tid_1 = 0;
    pthread_t tid_2 = 0;
    pthread_t* callback_extra[2] = {&tid_1, &tid_2};
    toku_set_test_txn_sync_callback(test_callback, callback_extra);

    struct start_txn_arg args = {env, db, parent};

    r = pthread_create(&tid_1, NULL, start_txn1, &args);
    assert(r==0);

    r= pthread_create(&tid_2, NULL, start_txn2, &args);
    assert(r==0);

     void * ret; 
    r = pthread_join(tid_1, &ret);
    assert(r == 0);
    r = pthread_join(tid_2, &ret);
    assert(r == 0);

    r = parent->commit(parent, 0);
    assert(r ==0);

    test_sync_destroy(&sync_s);
    r = db->close(db, 0);
    assert(r == 0);

    r = env->close(env, 0);
    assert(r == 0);

    return 0;
}

