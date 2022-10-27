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

#ident \
    "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "test.h"
// to verify the DB_LOCKING_READ works to lock the read rows for snapshot
// isolaton.
// we create a db, then init a read transaction with repeatable-read isolation
// and
// locking read flag, then we start another transaction to grab the write lock.
// DB_LOCKING_READ is defined here to just make the before and after tests work
// (before
// test did not have DB_LOCKING_READ flag).
#if !defined(DB_LOCKING_READ)
#define DB_LOCKING_READ 0
#endif
static int prelock_range(DBC *cursor, int left, int right) {
    DBT key_left;
    dbt_init(&key_left, &left, sizeof left);
    DBT key_right;
    dbt_init(&key_right, &right, sizeof right);
    int r = cursor->c_set_bounds(cursor, &key_left, &key_right, true, 0);
    return r;
}

static void test_read_write_range(DB_ENV *env,
                                  DB *db,
                                  uint32_t iso_flags,
                                  int expect_r) {
    int r;

    DB_TXN *txn_a = NULL;
    r = env->txn_begin(env, NULL, &txn_a, iso_flags);
    assert_zero(r);
    DB_TXN *txn_b = NULL;
    r = env->txn_begin(env, NULL, &txn_b, iso_flags);
    assert_zero(r);

    DBC *cursor_a = NULL;
    r = db->cursor(db, txn_a, &cursor_a, DB_LOCKING_READ);
    assert_zero(r);
    DBC *cursor_b = NULL;
    r = db->cursor(db, txn_b, &cursor_b, DB_RMW);
    assert_zero(r);

    r = prelock_range(cursor_a, htonl(10), htonl(100));
    assert_zero(r);
    r = prelock_range(cursor_b, htonl(50), htonl(200));
    assert(r == expect_r);

    r = cursor_a->c_close(cursor_a);
    assert_zero(r);
    r = cursor_b->c_close(cursor_b);
    assert_zero(r);

    r = txn_a->commit(txn_a, 0);
    assert_zero(r);
    r = txn_b->commit(txn_b, 0);
    assert_zero(r);
}

static void test_read_write_point(DB_ENV *env,
                                  DB *db,
                                  uint32_t iso_flags,
                                  int expect_r) {
    int r;

    DB_TXN *txn1 = NULL;
    r = env->txn_begin(env, NULL, &txn1, iso_flags);
    assert_zero(r);

    DB_TXN *txn2 = NULL;
    r = env->txn_begin(env, NULL, &txn2, iso_flags);
    assert_zero(r);

    DBC *c1 = NULL;
    r = db->cursor(db, txn1, &c1, DB_LOCKING_READ);
    assert_zero(r);

    DBC *c2 = NULL;
    r = db->cursor(db, txn2, &c2, DB_RMW);
    assert_zero(r);

    int k = htonl(42);
    DBT key;
    dbt_init(&key, &k, sizeof k);
    DBT val;
    memset(&val, 0, sizeof val);
    r = c1->c_get(c1, &key, &val, DB_SET);
    assert_zero(r);

    r = c2->c_get(c2, &key, &val, DB_SET);
    assert(r == expect_r);

    r = c1->c_close(c1);
    assert_zero(r);
    r = c2->c_close(c2);
    assert_zero(r);

    r = txn1->commit(txn1, 0);
    assert_zero(r);
    r = txn2->commit(txn2, 0);
    assert_zero(r);
}

int test_main(int argc, char *const argv[]) {
    int r;

    const char *env_dir = TOKU_TEST_FILENAME;
    const char *db_filename = "lockingreadtest";

    parse_args(argc, argv);

    char rm_cmd[strlen(env_dir) + strlen("rm -rf ") + 1];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", env_dir);
    r = system(rm_cmd);
    assert_zero(r);

    r = toku_os_mkdir(env_dir, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    assert_zero(r);

    DB_ENV *env = NULL;
    r = db_env_create(&env, 0);
    assert_zero(r);
    int env_open_flags = DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL | DB_INIT_TXN |
                         DB_INIT_LOCK | DB_INIT_LOG;
    r = env->open(
        env, env_dir, env_open_flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    assert_zero(r);

    // create the db
    DB *db = NULL;
    r = db_create(&db, env, 0);
    assert_zero(r);
    DB_TXN *create_txn = NULL;
    r = env->txn_begin(env, NULL, &create_txn, 0);
    assert_zero(r);
    r = db->open(db,
                 create_txn,
                 db_filename,
                 NULL,
                 DB_BTREE,
                 DB_CREATE,
                 S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    assert_zero(r);
    r = create_txn->commit(create_txn, 0);
    assert_zero(r);

    // add a record

    DB_TXN *write_txn = NULL;
    r = env->txn_begin(env, NULL, &write_txn, 0);
    assert_zero(r);

    int k = htonl(42);
    int v = 42;
    DBT key;
    dbt_init(&key, &k, sizeof k);
    DBT val;
    dbt_init(&val, &v, sizeof v);
    r = db->put(db, write_txn, &key, &val, DB_NOOVERWRITE);
    assert_zero(r);
    r = write_txn->commit(write_txn, 0);
    assert_zero(r);

    test_read_write_range(env, db, DB_TXN_SNAPSHOT, DB_LOCK_NOTGRANTED);
    test_read_write_point(env, db, DB_TXN_SNAPSHOT, DB_LOCK_NOTGRANTED);

    r = db->close(db, 0);
    assert_zero(r);

    r = env->close(env, 0);
    assert_zero(r);
    return 0;
}
