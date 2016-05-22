/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: expandtab:ts=8:sw=4:softtabstop=4:
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

#include <db.h>


int num_interrupts_called;
static bool interrupt(void* extra UU(), uint64_t rows UU()) {
    num_interrupts_called++;
    return false;
}

static bool interrupt_true(void* extra UU(), uint64_t rows UU()) {
    num_interrupts_called++;
    return true;
}


int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    DB_ENV *env;
    DB *db;
    int r;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);       assert(r==0);
    r=db_env_create(&env, 0); assert(r==0);
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL | DB_INIT_TXN | DB_CREATE | DB_PRIVATE | DB_INIT_LOG, S_IRWXU+S_IRWXG+S_IRWXO); assert(r==0);

    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->set_readpagesize(db, 1024);
    CKERR(r);
    r = db->set_pagesize(db, 1024*10);
    CKERR(r);

    const char * const fname = "test.change_pagesize";
    r = db->open(db, NULL, fname, "main", DB_BTREE, DB_CREATE, 0666);
    CKERR(r);
    DB_TXN* txn;
    r = env->txn_begin(env, 0, &txn, 0);
    CKERR(r);
    for (uint64_t i = 0; i < 10000; i++) {
        DBT key, val;
        uint64_t k = i;
        uint64_t v = i;
        dbt_init(&key, &k, sizeof k);
        dbt_init(&val, &v, sizeof v);
        db->put(db, txn, &key, &val, DB_PRELOCKED_WRITE); // adding DB_PRELOCKED_WRITE just to make the test go faster
    }
    r = txn->commit(txn, 0);
    CKERR(r);

    // create a snapshot txn so that when we delete the elements
    // we just inserted, that they do not get garbage collected away
    DB_TXN* snapshot_txn;
    r = env->txn_begin(env, 0, &snapshot_txn, DB_TXN_SNAPSHOT);
    CKERR(r);

    DB_TXN* delete_txn;
    r = env->txn_begin(env, 0, &delete_txn, DB_TXN_SNAPSHOT);
    CKERR(r);

    for (uint64_t i = 0; i < 10000; i++) {
        DBT key;
        uint64_t k = i;
        dbt_init(&key, &k, sizeof k);
        db->del(db, delete_txn, &key, DB_PRELOCKED_WRITE | DB_DELETE_ANY); // adding DB_PRELOCKED_WRITE just to make the test go faster
    }
    r = delete_txn->commit(delete_txn, 0);
    CKERR(r);

    // to make more than one basement node in the dictionary's leaf nodes
    r = env->txn_checkpoint(env, 0, 0, 0);
    CKERR(r);

    // create a txn that should see an empty dictionary
    DB_TXN* test_txn;
    r = env->txn_begin(env, 0, &test_txn, DB_TXN_SNAPSHOT);
    CKERR(r);
    DBC* cursor = NULL;
    r = db->cursor(db, test_txn, &cursor, 0);
    cursor->c_set_check_interrupt_callback(cursor, interrupt, NULL);
    DBT key, val;
    r = cursor->c_get(cursor, &key, &val, DB_NEXT);
    CKERR2(r, DB_NOTFOUND);
    assert(num_interrupts_called > 1);
    num_interrupts_called = 0;
    cursor->c_set_check_interrupt_callback(cursor, interrupt_true, NULL);
    r = cursor->c_get(cursor, &key, &val, DB_NEXT);
    CKERR2(r, TOKUDB_INTERRUPTED);
    assert(num_interrupts_called == 1);

    r = cursor->c_close(cursor);
    CKERR(r);    
    r = test_txn->commit(test_txn, 0);
    CKERR(r);

    
    r = snapshot_txn->commit(snapshot_txn, 0);
    CKERR(r);


    r = db->close(db, 0);
    CKERR(r);

    r = env->close(env, 0);
    assert(r == 0);

    return 0;
}
