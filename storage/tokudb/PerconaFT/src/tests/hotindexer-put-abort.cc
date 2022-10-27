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
#include "ydb.h"
#include "toku_pthread.h"

// this test reproduces the rollback log corruption that occurs when hot indexing runs concurrent with a long abort
// the concurrent operation occurs when the abort periodically releases the ydb lock which allows the hot indexer
// to run.  the hot indexer erroneously append to the rollback log that is in the process of being aborted.

static int
put_callback(DB *dest_db, DB *src_db, DBT_ARRAY *dest_keys, DBT_ARRAY *dest_vals, const DBT *src_key, const DBT *src_val) {
    toku_dbt_array_resize(dest_keys, 1);
    toku_dbt_array_resize(dest_vals, 1);
    DBT *dest_key = &dest_keys->dbts[0];
    DBT *dest_val = &dest_vals->dbts[0];

    (void) dest_db; (void) src_db; (void) dest_keys; (void) dest_vals; (void) src_key; (void) src_val;
    lazy_assert(src_db != NULL && dest_db != NULL);

    if (dest_key->flags == DB_DBT_REALLOC) {
        dest_key->data = toku_realloc(dest_key->data, src_val->size);
        memcpy(dest_key->data, src_val->data, src_val->size);
        dest_key->size = src_val->size;
    }
    dest_val->size = 0;
    
    return 0;
}

struct indexer_arg {
    DB_ENV *env;
    DB *src_db;
    int n_dest_db;
    DB **dest_db;
};

static void *
indexer_thread(void *arg) {
    struct indexer_arg *indexer_arg = (struct indexer_arg *) arg;
    DB_ENV *env = indexer_arg->env;
    int r;
    
    DB_TXN *indexer_txn = NULL;
    r = env->txn_begin(env, NULL, &indexer_txn, 0); assert_zero(r);
        
    DB_INDEXER *indexer = NULL;
    r = env->create_indexer(env, indexer_txn, &indexer, indexer_arg->src_db, indexer_arg->n_dest_db, indexer_arg->dest_db, NULL, 0); assert_zero(r);
    
    r = indexer->build(indexer); assert_zero(r);
        
    r = indexer->close(indexer); assert_zero(r);
        
    r = indexer_txn->commit(indexer_txn, 0); assert_zero(r);

    return arg;
}

static void
verify_empty(DB_ENV *env, DB *db) {
    int r;
    
    DB_TXN *txn = NULL;
    r = env->txn_begin(env, NULL, &txn, 0); assert_zero(r);

    DBC *cursor = NULL;
    r = db->cursor(db, txn, &cursor, 0); assert_zero(r);

    DBT key, val;
    r = cursor->c_get(cursor, dbt_init(&key, 0, 0), dbt_init(&val, 0, 0), DB_NEXT);
    assert(r == DB_NOTFOUND);
    
    r = cursor->c_close(cursor); assert_zero(r);

    r = txn->commit(txn, 0); assert_zero(r);
}

static void
run_test(void) {
    int r;
    DB_ENV *env = NULL;
    r = db_env_create(&env, 0); assert_zero(r);

    r = env->set_generate_row_callback_for_put(env, put_callback); assert_zero(r);

    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB *src_db = NULL;
    r = db_create(&src_db, env, 0); assert_zero(r);
    r = src_db->open(src_db, NULL, "0.tdb", NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB *dest_db = NULL;
    r = db_create(&dest_db, env, 0); assert_zero(r);
    r = dest_db->open(dest_db, NULL, "1.tdb", NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB_TXN *txn = NULL;
    r = env->txn_begin(env, NULL, &txn, 0); assert_zero(r);

    // insert some
    for (int i = 0; i < 246723; i++) {
        int k = htonl(i);
        int v = i;
        DBT key; dbt_init(&key, &k, sizeof k);
        DBT val; dbt_init(&val, &v, sizeof v);
        r = src_db->put(src_db, txn, &key, &val, 0); assert_zero(r);
    }

    // run the indexer
    struct indexer_arg indexer_arg = {env, src_db, 1, &dest_db};
    toku_pthread_t pid;
    r = toku_pthread_create(
        toku_uninstrumented, &pid, nullptr, indexer_thread, &indexer_arg);
    assert_zero(r);

    r = txn->abort(txn);
    assert_zero(r);

    void *ret;
    r = toku_pthread_join(pid, &ret); assert_zero(r);

    verify_empty(env, src_db);
    verify_empty(env, dest_db);

    r = src_db->close(src_db, 0); assert_zero(r);

    r = dest_db->close(dest_db, 0); assert_zero(r);

    r = env->close(env, 0); assert_zero(r);
}

int
test_main(int argc, char * const argv[]) {
    int r;

    // parse_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        char * const arg = argv[i];
        if (strcmp(arg, "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(arg, "-q") == 0) {
            verbose = 0;
            continue;
        }
    }

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    run_test();

    return 0;
}

