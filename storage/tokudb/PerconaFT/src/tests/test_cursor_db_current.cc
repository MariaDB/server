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
#include <stdlib.h>

#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <memory.h>
#include <db.h>

static void
db_put (DB *db, int k, int v) {
    DB_TXN * const null_txn = 0;
    DBT key, val;
    int r = db->put(db, null_txn, dbt_init(&key, &k, sizeof k), dbt_init(&val, &v, sizeof v), 0);
    CKERR(r);
}

static void
test_cursor_current (void) {
    if (verbose) printf("test_cursor_current\n");

    DB_TXN * const null_txn = 0;
    const char * const fname = "test.cursor.current.ft_handle";
    int r;

    DB_ENV *env;
    r = db_env_create(&env, 0); assert(r == 0);
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE+DB_PRIVATE+DB_INIT_MPOOL, 0); assert(r == 0);

    DB *db;
    r = db_create(&db, env, 0); CKERR(r);
    db->set_errfile(db,0); // Turn off those annoying errors
    r = db->open(db, null_txn, fname, "main", DB_BTREE, DB_CREATE, 0666); CKERR(r);

    int k = 42, v = 42000;
    db_put(db, k, v);
    db_put(db, 43, 2000);
 
    DBC *cursor;

    r = db->cursor(db, null_txn, &cursor, 0); CKERR(r);

    DBT key, data; int kk, vv;

    r = cursor->c_get(cursor, dbt_init_malloc(&key), dbt_init_malloc(&data), DB_CURRENT);
    assert(r == EINVAL);

    r = cursor->c_get(cursor, dbt_init_malloc(&key), dbt_init_malloc(&data), DB_FIRST);
    CKERR(r);
    assert(key.size == sizeof kk);
    memcpy(&kk, key.data, sizeof kk);
    assert(kk == k);
    assert(data.size == sizeof vv);
    memcpy(&vv, data.data, data.size);
    assert(vv == v);
    toku_free(key.data); toku_free(data.data);

    r = cursor->c_get(cursor, dbt_init_malloc(&key), dbt_init_malloc(&data), DB_CURRENT);
    CKERR(r);
    assert(key.size == sizeof kk);
    memcpy(&kk, key.data, sizeof kk);
    assert(kk == k);
    assert(data.size == sizeof vv);
    memcpy(&vv, data.data, data.size);
    assert(vv == v);
    r = db->del(db, null_txn, &key, DB_DELETE_ANY);
    toku_free(key.data); toku_free(data.data);

    r = cursor->c_get(cursor, dbt_init_malloc(&key), dbt_init_malloc(&data), DB_CURRENT);
    CKERR2(r,DB_KEYEMPTY);

    r = cursor->c_get(cursor, dbt_init_malloc(&key), dbt_init_malloc(&data), DB_CURRENT);
    CKERR2(r,DB_KEYEMPTY);

    r = cursor->c_close(cursor); CKERR(r);

    r = db->close(db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
}

static void
db_get (DB *db, int k, int UU(v), int expectr) {
    DBT key, val;
    int r = db->get(db, 0, dbt_init(&key, &k, sizeof k), dbt_init_malloc(&val), 0);
    assert(r == expectr);
}

static void
test_reopen (void) {
    if (verbose) printf("test_reopen\n");

    DB_TXN * const null_txn = 0;
    const char * const fname = "test.cursor.current.ft_handle";
    int r;

    DB_ENV *env;
    r = db_env_create(&env, 0); assert(r == 0);
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE+DB_PRIVATE+DB_INIT_MPOOL, 0); assert(r == 0);

    DB *db;
    r = db_create(&db, env, 0); CKERR(r);
    db->set_errfile(db,0); // Turn off those annoying errors
    r = db->open(db, null_txn, fname, "main", DB_BTREE, 0, 0666); CKERR(r);

    db_get(db, 1, 1, DB_NOTFOUND);

    r = db->close(db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
}

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
  
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    test_cursor_current();
    test_reopen();

    return 0;
}
