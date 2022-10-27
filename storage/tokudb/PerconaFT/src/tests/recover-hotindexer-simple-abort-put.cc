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

static int
put_callback(DB *dest_db, DB *src_db, DBT_ARRAY *dest_keys, DBT_ARRAY *dest_vals, const DBT *src_key, const DBT *src_val) {
    toku_dbt_array_resize(dest_keys, 1);
    toku_dbt_array_resize(dest_vals, 1);
    DBT *dest_key = &dest_keys->dbts[0];
    DBT *dest_val = &dest_vals->dbts[0];
    (void) dest_db; (void) src_db; (void) dest_key; (void) dest_val; (void) src_key; (void) src_val;

    lazy_assert(src_db != NULL && dest_db != NULL);

    if (dest_key->flags == DB_DBT_REALLOC) {
        toku_free(dest_key->data);
    }
    dest_key->flags = DB_DBT_REALLOC;
    dest_key->data = toku_xmemdup(src_val->data, src_val->size);
    dest_key->size = src_val->size;
    dest_val->size = 0;
    
    return 0;
}

int envflags = DB_INIT_MPOOL|DB_CREATE|DB_THREAD|DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE;

static void
run_test(void) {
    int r;
    DB_ENV *env = NULL;
    r = db_env_create(&env, 0); assert_zero(r);

    r = env->set_generate_row_callback_for_put(env, put_callback); assert_zero(r);

    { int chk_r = env->open(env, TOKU_TEST_FILENAME, envflags, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(chk_r); }

    DB *src_db = NULL;
    r = db_create(&src_db, env, 0); assert_zero(r);
    r = src_db->open(src_db, NULL, "0.tdb", NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB *dest_db = NULL;
    r = db_create(&dest_db, env, 0); assert_zero(r);
    r = dest_db->open(dest_db, NULL, "1.tdb", NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB_TXN* index_txn = NULL;
    r = env->txn_begin(env, NULL, &index_txn , 0); assert_zero(r);
    DB_TXN* put_txn = NULL;
    r = env->txn_begin(env, NULL, &put_txn , 0); assert_zero(r);

    DBT key,data;
    r = src_db->put(
        src_db, 
        put_txn,
        dbt_init(&key,  "hello", 6),
        dbt_init(&data, "there", 6),
        0
        );

    DB_INDEXER *indexer = NULL;
    r = env->create_indexer(env, index_txn, &indexer, src_db, 1, &dest_db, NULL, 0); assert_zero(r);
    r = indexer->build(indexer); assert_zero(r);        
    r = indexer->close(indexer); assert_zero(r);
    r = index_txn->abort(index_txn); assert_zero(r);

    r = env->txn_checkpoint(env, 0, 0, 0);
    assert_zero(r);

    toku_hard_crash_on_purpose();
}

static void
run_recover(void) {
    DB_ENV *env;
    { int chk_r = db_env_create(&env, 0); CKERR(chk_r); }
    env->set_errfile(env, stderr);
    { int chk_r = env->open(env, TOKU_TEST_FILENAME, envflags|DB_RECOVER, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(chk_r); }
    { int chk_r = env->close(env, 0); CKERR(chk_r); }
}

int
test_main(int argc, char * const argv[]) {
    bool do_test = false;
    bool do_recover = false;

    for (int i = 1; i < argc; i++) {
        char * const arg = argv[i];
        if (strcmp(arg, "--test") == 0) {
            do_test = true;
            continue;
        }
        if (strcmp(arg, "--recover") == 0) {
            do_recover = true;
            continue;
        }
    }

    if (do_test) {
        int r;
        toku_os_recursive_delete(TOKU_TEST_FILENAME);
        r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);
        run_test();
    }
    if (do_recover) {
        run_recover();
    }

    return 0;
}

