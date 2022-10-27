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

#include <memory.h>
#include <db.h>

#include <errno.h>
#include <sys/stat.h>


// TOKU_TEST_FILENAME is defined in the Makefile

static DB *db;
static DB_TXN* txns[(int)256];
static DB_ENV* dbenv;
static DBC*    cursors[(int)256];

static void
put(bool success, char txn, int _key, int _data) {
    assert(txns[(int)txn]);

    int r;
    DBT key;
    DBT data;
    
    r = db->put(db, txns[(int)txn],
                    dbt_init(&key, &_key, sizeof(int)),
                    dbt_init(&data, &_data, sizeof(int)),
                    0);

    if (success)    CKERR(r);
    else            CKERR2s(r, DB_LOCK_DEADLOCK, DB_LOCK_NOTGRANTED);
}

static void
init_txn (char name, uint32_t flags) {
    int r;
    assert(!txns[(int)name]);
    r = dbenv->txn_begin(dbenv, NULL, &txns[(int)name], DB_TXN_NOWAIT | flags);
        CKERR(r);
    assert(txns[(int)name]);
}

static void
init_dbc (char name) {
    int r;

    assert(!cursors[(int)name] && txns[(int)name]);
    r = db->cursor(db, txns[(int)name], &cursors[(int)name], 0);
        CKERR(r);
    assert(cursors[(int)name]);
}

static void
commit_txn (char name) {
    int r;
    assert(txns[(int)name] && !cursors[(int)name]);

    r = txns[(int)name]->commit(txns[(int)name], 0);
        CKERR(r);
    txns[(int)name] = NULL;
}


static void
close_dbc (char name) {
    int r;

    assert(cursors[(int)name]);
    r = cursors[(int)name]->c_close(cursors[(int)name]);
        CKERR(r);
    cursors[(int)name] = NULL;
}

static void
early_commit (char name) {
    assert(cursors[(int)name] && txns[(int)name]);
    close_dbc(name);
    commit_txn(name);
}

static void
setup_dbs (void) {
    int r;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    dbenv   = NULL;
    db      = NULL;
    /* Open/create primary */
    r = db_env_create(&dbenv, 0);
        CKERR(r);
    r = dbenv->set_default_bt_compare(dbenv, int_dbt_cmp);
        CKERR(r);
    uint32_t env_txn_flags  = DB_INIT_TXN | DB_INIT_LOCK;
    uint32_t env_open_flags = DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL;
	r = dbenv->open(dbenv, TOKU_TEST_FILENAME, env_open_flags | env_txn_flags, 0600);
        CKERR(r);
    
    r = db_create(&db, dbenv, 0);
        CKERR(r);

    char a;
    for (a = 'a'; a <= 'z'; a++) init_txn(a, 0);
    for (a = '0'; a <= '9'; a++) init_txn(a, DB_READ_UNCOMMITTED);
    init_txn('\0', 0);
    r = db->open(db, txns[(int)'\0'], "foobar.db", NULL, DB_BTREE, DB_CREATE | DB_READ_UNCOMMITTED, 0600);
        CKERR(r);
    commit_txn('\0');
    for (a = 'a'; a <= 'z'; a++) init_dbc(a);
    for (a = '0'; a <= '9'; a++) init_dbc(a);
}

static void
close_dbs(void) {
    char a;
    for (a = 'a'; a <= 'z'; a++) {
        if (cursors[(int)a]) close_dbc(a);
        if (txns[(int)a])    commit_txn(a);
    }
    for (a = '0'; a <= '9'; a++) {
        if (cursors[(int)a]) close_dbc(a);
        if (txns[(int)a])    commit_txn(a);
    }

    int r;
    r = db->close(db, 0);
        CKERR(r);
    db      = NULL;
    r = dbenv->close(dbenv, 0);
        CKERR(r);
    dbenv   = NULL;
}


static void
table_scan(char txn, bool success) {
    int r;
    DBT key;
    DBT data;

    assert(txns[(int)txn] && cursors[(int)txn]);
    r = cursors[(int)txn]->c_get(cursors[(int)txn],
                                 dbt_init(&key,  0, 0),
                                 dbt_init(&data, 0, 0),
                                 DB_FIRST);
    while (r==0) {
        r = cursors[(int)txn]->c_get(cursors[(int)txn],
                                     dbt_init(&key,  0, 0),
                                     dbt_init(&data, 0, 0),
                                     DB_NEXT);
    }
#ifdef BLOCKING_ROW_LOCKS_READS_NOT_SHARED
    if (success) invariant(r == DB_NOTFOUND || r == DB_LOCK_NOTGRANTED || r == DB_LOCK_DEADLOCK);
    else         CKERR2s(r, DB_LOCK_NOTGRANTED, DB_LOCK_DEADLOCK);
#else
    if (success) CKERR2(r, DB_NOTFOUND);
    else         CKERR2s(r, DB_LOCK_NOTGRANTED, DB_LOCK_DEADLOCK);
#endif
}

static void
table_prelock(char txn, bool success) {
    int r;
    r = db->pre_acquire_table_lock(db,  txns[(int)txn]);
    if (success) CKERR(r);
    else         CKERR2s(r, DB_LOCK_NOTGRANTED, DB_LOCK_DEADLOCK);
}

static void
test (void) {
    char txn;
    /* ********************************************************************** */
    setup_dbs();
    close_dbs();
    /* ********************************************************************** */
    setup_dbs();
    table_scan('0', true);
    table_prelock('a', true);
    put(true, 'a', 0, 0);
    for (txn = 'b'; txn<'z'; txn++) {
        table_scan(txn, false);
    }
    for (txn = '0'; txn<'9'; txn++) {
        table_scan(txn, true);
    }
    early_commit('a');
    for (txn = 'b'; txn<'z'; txn++) {
        table_scan(txn, true);
    }
    for (txn = '0'; txn<'9'; txn++) {
        table_scan(txn, true);
    }
    close_dbs();
    /* ********************************************************************** */
}


int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    test();
    return 0;
}
