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

// test that update broadcast does nothing if the table is empty

#include "test.h"

const int envflags = DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE;

DB_ENV *env;

static int update_fun(DB *UU(db),
                      const DBT *UU(key),
                      const DBT *UU(old_val), const DBT *UU(extra),
                      void (*set_val)(const DBT *new_val,
                                         void *set_extra),
                      void *set_extra) {
  set_val(extra,set_extra);
  return 0;
}

static void setup (void) {
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    { int chk_r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(chk_r); }
    { int chk_r = db_env_create(&env, 0); CKERR(chk_r); }
    env->set_errfile(env, stderr);
    env->set_update(env, update_fun);
    { int chk_r = env->open(env, TOKU_TEST_FILENAME, envflags, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(chk_r); }
}

static void cleanup (void) {
    { int chk_r = env->close(env, 0); CKERR(chk_r); }
}

static int do_updates(DB_TXN *txn, DB *db, uint32_t flags) {
  DBT key, val;
  uint32_t k = 101;
  uint32_t v = 10101;
  dbt_init(&key, &k, sizeof(k));
  dbt_init(&val, &v, sizeof(v));

  int r = db->update(db, txn, &key, &val, flags); CKERR(r);
    return r;
}

static void run_test(bool prelock, bool commit) {
    DB *db;
    uint32_t update_flags = 0;
    setup();

    IN_TXN_COMMIT(env, NULL, txn_1, 0, {
            { int chk_r = db_create(&db, env, 0); CKERR(chk_r); }
            { int chk_r = db->open(db, txn_1, "foo.db", NULL, DB_BTREE, DB_CREATE, 0666); CKERR(chk_r); }
        });
    if (prelock) {
        IN_TXN_COMMIT(env, NULL, txn_2, 0, {
                { int chk_r = db->pre_acquire_table_lock(db, txn_2); CKERR(chk_r); }
        });
    }

    if (commit) {
        IN_TXN_COMMIT(env, NULL, txn_2, 0, {
                { int chk_r = do_updates(txn_2, db, update_flags); CKERR(chk_r); }
        });
        DBC *cursor = NULL;
        DBT key, val;
        memset(&key, 0, sizeof(key));
        memset(&val, 0, sizeof(val));

        IN_TXN_COMMIT(env, NULL, txn_3, 0, {
                { int chk_r = db->cursor(db, txn_3, &cursor, 0); CKERR(chk_r); }
                { int chk_r = cursor->c_get(cursor, &key, &val, DB_NEXT); CKERR(chk_r); }
            assert(key.size == sizeof(uint32_t));
            assert(val.size == sizeof(uint32_t));
            assert(*(uint32_t *)(key.data) == 101);
            assert(*(uint32_t *)(val.data) == 10101);
            { int chk_r = cursor->c_close(cursor); CKERR(chk_r); }
        });
    }
    else {
        IN_TXN_ABORT(env, NULL, txn_2, 0, {
                { int chk_r = do_updates(txn_2, db, update_flags); CKERR(chk_r); }
        });
        DBC *cursor = NULL;
        DBT key, val;
        memset(&key, 0, sizeof(key));
        memset(&val, 0, sizeof(val));

        IN_TXN_COMMIT(env, NULL, txn_3, 0, {
                { int chk_r = db->cursor(db, txn_3, &cursor, 0); CKERR(chk_r); }
                { int chk_r = cursor->c_get(cursor, &key, &val, DB_NEXT); CKERR2(chk_r, DB_NOTFOUND); }
                { int chk_r = cursor->c_close(cursor); CKERR(chk_r); }
        });
    }
    { int chk_r = db->close(db, 0); CKERR(chk_r); }
    cleanup();
}

int test_main(int argc, char * const argv[]) {
    parse_args(argc, argv);
    run_test(true,true);
    run_test(false,true);
    run_test(true,false);
    run_test(false,false);

    return 0;
}
