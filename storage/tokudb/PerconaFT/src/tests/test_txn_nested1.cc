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
#include <memory.h>
#include <sys/stat.h>
#include <db.h>
#include <ft/txn/xids.h>
#define MAX_NEST MAX_NESTED_TRANSACTIONS


/*********************
 *
 * Purpose of this test is to exercise nested transactions in a basic way:
 * Create MAX nested transactions, inserting a value at each level, verify:
 * 
 * for i = 1 to MAX
 *  - txnid = begin()
 *  - txns[i] = txnid
 *  - insert, query
 *
 * for i = 1 to MAX
 *  - txnid = txns[MAX - i - 1]
 *  - commit or abort(txnid), query
 *
 */

static DB *db;
static DB_ENV *env;

static void
setup_db (void) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    r = db_env_create(&env, 0); CKERR(r);
    r = env->set_default_bt_compare(env, int_dbt_cmp); CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL | DB_INIT_LOG | DB_INIT_LOCK | DB_INIT_TXN | DB_PRIVATE | DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); 
    CKERR(r);

    {
        DB_TXN *txn = 0;
        r = env->txn_begin(env, 0, &txn, 0); CKERR(r);

        r = db_create(&db, env, 0); CKERR(r);
        r = db->open(db, txn, "test.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
        r = txn->commit(txn, 0); CKERR(r);
    }
}


static void
close_db (void) {
    int r;
    r=db->close(db, 0); CKERR(r);
    r=env->close(env, 0); CKERR(r);
}

static void
test_txn_nesting (int depth) {
    int r;
    if (verbose) { fprintf(stderr, "%s (%s):%d [depth = %d]\n", __FILE__, __FUNCTION__, __LINE__, depth); fflush(stderr); }

    DBT key, val, observed_val;
    dbt_init(&observed_val, NULL, 0);
    int i;

    DB_TXN * txns[depth];
    DB_TXN * parent = NULL;

    int vals[depth];

    int mykey = 42;
    dbt_init(&key, &mykey, sizeof mykey);
    

    for (i = 0; i < depth; i++){
	DB_TXN * this_txn;

	if (verbose)
	    printf("Begin txn at level %d\n", i);
	vals[i] = i;
	dbt_init(&val, &vals[i], sizeof i);
	r = env->txn_begin(env, parent, &this_txn, 0);   CKERR(r);
	txns[i] = this_txn;
	parent = this_txn;  // will be parent in next iteration
	r = db->put(db, this_txn, &key, &val, 0);          CKERR(r);

        r = db->get(db, this_txn, &key, &observed_val, 0); CKERR(r);
	assert(int_dbt_cmp(db, &val, &observed_val) == 0);
    }

    int which_val = depth-1;
    for (i = depth-1; i >= 0; i--) {
        //Query, verify the correct value is stored.
        //Close (abort/commit) innermost transaction

        if (verbose)
            printf("Commit txn at level %d\n", i);

        dbt_init(&observed_val, NULL, 0);
        r = db->get(db, txns[i], &key, &observed_val, 0); CKERR(r);
	dbt_init(&val, &vals[which_val], sizeof i);
	assert(int_dbt_cmp(db, &val, &observed_val) == 0);

	if (i % 2) {
	    r = txns[i]->commit(txns[i], DB_TXN_NOSYNC);   CKERR(r);
	    //which_val does not change (it gets promoted)
	}
	else {
	    r = txns[i]->abort(txns[i]); CKERR(r);
	    which_val = i - 1;
	}
        txns[i] = NULL;
    }
    //Query, verify the correct value is stored.
    r = db->get(db, NULL, &key, &observed_val, 0);
    if (which_val == -1) CKERR2(r, DB_NOTFOUND);
    else {
        CKERR(r);
	dbt_init(&val, &vals[which_val], sizeof i);
        assert(int_dbt_cmp(db, &val, &observed_val) == 0);
    }
}

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    setup_db();
    test_txn_nesting(MAX_NEST);
    close_db();
    return 0;
}
