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
#include <stdio.h>

#include <sys/stat.h>
#include <db.h>

// Tests that the logical row counts are correct and not subject to variance
// due to normal insert/delete messages within the tree with the few exceptions
// of 1) rollback messages not yet applied; 2) inserts messages turned to
// updates on apply; and 3) missing leafentries on delete messages on apply.

static DB_TXN* const null_txn = 0;
static const uint64_t num_records = 4*1024;

#define CHECK_NUM_ROWS(_expected, _stats) assert(_stats.bt_ndata == _expected)

static DB* create_db(const char* fname, DB_ENV* env) {
    int r;
    DB* db;

    r = db_create(&db, env, 0);
    assert(r == 0);
    db->set_errfile(db, stderr);

    r = db->set_pagesize(db, 8192);
    assert(r == 0);

    r = db->set_readpagesize(db, 1024);
    assert(r == 0);

    r = db->set_fanout(db, 4);
    assert(r == 0);

    r = db->set_compression_method(db, TOKU_NO_COMPRESSION);
    assert(r == 0);

    r = db->open(db, null_txn, fname, "main", DB_BTREE, DB_CREATE,
                 0666);
    assert(r == 0);

    return db;
}
static void add_records(DB* db, DB_TXN* txn, uint64_t start_id, uint64_t num) {
    int r;
    for (uint64_t i = 0, j=start_id; i < num; i++,j++) {
        char key[100], val[256];
        DBT k,v;
        snprintf(key, 100, "%08" PRIu64, j);
        snprintf(val, 256, "%*s", 200, key);
        r =
            db->put(
                db,
                txn,
                dbt_init(&k, key, 1+strlen(key)),
                dbt_init(&v, val, 1+strlen(val)),
                0);
        assert(r == 0);
    }
}
static void delete_records(
    DB* db,
    DB_TXN* txn,
    uint64_t start_id,
    uint64_t num) {

    int r;
    for (uint64_t i = 0, j=start_id; i < num; i++,j++) {
        char key[100];
        DBT k;
        snprintf(key, 100, "%08" PRIu64, j);
        r =
            db->del(
                db,
                txn,
                dbt_init(&k, key, 1+strlen(key)),
                0);
        assert(r == 0);
    }
}
static void full_optimize(DB* db) {
    int r;
    uint64_t loops_run = 0;

    r = db->optimize(db);
    assert(r == 0);

    r = db->hot_optimize(db, NULL, NULL, NULL, NULL, &loops_run);
    assert(r == 0);
}
static void test_insert_commit(DB_ENV* env) {
    int r;
    DB* db;
    DB_TXN* txn;
    DB_BTREE_STAT64 stats;

    db = create_db(__FUNCTION__, env);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    add_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf("%s : before commit %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    r = txn->commit(txn, 0); 
    assert(r == 0);
    
    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf("%s : after commit %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    db->close(db, 0);
}
static void test_insert_delete_commit(DB_ENV* env) {
    int r;
    DB* db;
    DB_TXN* txn;
    DB_BTREE_STAT64 stats;

    db = create_db(__FUNCTION__, env);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    add_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf("%s : before delete %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    delete_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf("%s : after delete %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    r = txn->commit(txn, 0);
    assert(r == 0);
    
    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf("%s : after commit %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    db->close(db, 0);
}
static void test_insert_commit_delete_commit(DB_ENV* env) {
    int r;
    DB* db;
    DB_TXN* txn;
    DB_BTREE_STAT64 stats;

    db = create_db(__FUNCTION__, env);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    add_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf(
            "%s : before insert commit %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    r = txn->commit(txn, 0);
    assert(r == 0);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf(
            "%s : after insert commit %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    delete_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf("%s : after delete %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    r = txn->commit(txn, 0);
    assert(r == 0);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf(
            "%s : after delete commit %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    db->close(db, 0);
}
static void test_insert_rollback(DB_ENV* env) {
    int r;
    DB* db;
    DB_TXN* txn;
    DB_BTREE_STAT64 stats;

    db = create_db(__FUNCTION__, env);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    add_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf("%s : before rollback %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    r = txn->abort(txn);
    assert(r == 0);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    // CAN NOT TEST stats HERE AS THEY ARE SOMEWHAT NON_DETERMINISTIC UNTIL
    // optimize + hot_optimize HAVE BEEN RUN DUE TO THE FACT THAT ROLLBACK
    // MESSAGES ARE "IN-FLIGHT" IN THE TREE AND MUST BE APPLIED IN ORDER TO
    // CORRECT THE RUNNING LOGICAL COUNT
    if (verbose)
        printf("%s : after rollback %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    full_optimize(db);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf(
            "%s : after rollback optimize %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    db->close(db, 0);
}
static void test_insert_delete_rollback(DB_ENV* env) {
    int r;
    DB* db;
    DB_TXN* txn;
    DB_BTREE_STAT64 stats;

    db = create_db(__FUNCTION__, env);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    add_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf("%s : before delete %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    delete_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf("%s : after delete %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    r = txn->abort(txn); 
    assert(r == 0);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf("%s : after commit %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    db->close(db, 0);
}
static void test_insert_commit_delete_rollback(DB_ENV* env) {
    int r;
    DB* db;
    DB_TXN* txn;
    DB_BTREE_STAT64 stats;

    db = create_db(__FUNCTION__, env);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    add_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf(
            "%s : before insert commit %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    r = txn->commit(txn, 0);
    assert(r == 0);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf(
            "%s : after insert commit %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    delete_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(0, stats);
    if (verbose)
        printf("%s : after delete %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    r = txn->abort(txn); 
    assert(r == 0);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    // CAN NOT TEST stats HERE AS THEY ARE SOMEWHAT NON_DETERMINISTIC UNTIL
    // optimize + hot_optimize HAVE BEEN RUN DUE TO THE FACT THAT ROLLBACK
    // MESSAGES ARE "IN-FLIGHT" IN THE TREE AND MUST BE APPLIED IN ORDER TO
    // CORRECT THE RUNNING LOGICAL COUNT
    if (verbose)
        printf(
            "%s : after delete rollback %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    full_optimize(db);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf(
            "%s : after delete rollback optimize %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    db->close(db, 0);
}

static int test_recount_insert_commit_progress(
    uint64_t count,
    uint64_t deleted,
    void*) {

    if (verbose)
        printf(
            "%s : count[%" PRIu64 "] deleted[%" PRIu64 "]\n",
            __FUNCTION__,
            count,
            deleted);
    return 0;
}
static int test_recount_cancel_progress(uint64_t, uint64_t, void*) {
    return 1;
}

static void test_recount_insert_commit(DB_ENV* env) {
    int r;
    DB* db;
    DB_TXN* txn;
    DB_BTREE_STAT64 stats;

    db = create_db(__FUNCTION__, env);

    r = env->txn_begin(env, null_txn, &txn, 0);
    assert(r == 0);

    add_records(db, txn, 0, num_records);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf(
            "%s : before commit %" PRIu64 " rows\n",
            __FUNCTION__,
            stats.bt_ndata);

    r = txn->commit(txn, 0);
    assert(r == 0);

    r = db->stat64(db, null_txn, &stats);
    assert(r == 0);

    CHECK_NUM_ROWS(num_records, stats);
    if (verbose)
        printf("%s : after commit %" PRIu64 " rows\n", __FUNCTION__, stats.bt_ndata);

    // test that recount counted correct # of rows
    r = db->recount_rows(db, test_recount_insert_commit_progress, NULL);
    assert(r == 0);
    CHECK_NUM_ROWS(num_records, stats);

    // test that recount callback cancel returns
    r = db->recount_rows(db, test_recount_cancel_progress, NULL);
    assert(r == 1);
    CHECK_NUM_ROWS(num_records, stats);

    db->close(db, 0);
}
int test_main(int UU(argc), char UU(*const argv[])) {
    int r;
    DB_ENV* env;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU + S_IRWXG + S_IRWXO);

    r = db_env_create(&env, 0);
    assert(r == 0);

    r =
        env->open(
            env,
            TOKU_TEST_FILENAME,
            DB_INIT_MPOOL + DB_INIT_LOG + DB_INIT_TXN + DB_PRIVATE + DB_CREATE,
            S_IRWXU + S_IRWXG + S_IRWXO);
    assert(r == 0);

    test_insert_commit(env);
    test_insert_delete_commit(env);
    test_insert_commit_delete_commit(env);
    test_insert_rollback(env);
    test_insert_delete_rollback(env);
    test_insert_commit_delete_rollback(env);
    test_recount_insert_commit(env);

    r = env->close(env, 0);
    assert(r == 0);

    return 0;
}
