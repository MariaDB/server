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

/* Test for #3522.    Demonstrate that with DB_TRYAGAIN a cursor can stall.
 * Strategy: Create a tree (with relatively small nodes so things happen quickly, and relatively large compared to the cache).
 *  In a single transaction: Delete everything, and then do a DB_FIRST.
 *  Make the test terminate by capturing the calls to pread(). */

#include "test.h"
#include <portability/toku_atomic.h>

static DB_ENV *env;
static DB *db;
const int N = 1000;

const int n_preads_limit = 1000;
long n_preads = 0;

static ssize_t my_pread (int fd, void *buf, size_t count, off_t offset) {
    long n_read_so_far = toku_sync_fetch_and_add(&n_preads, 1);
    if (n_read_so_far > n_preads_limit) {
	if (verbose) fprintf(stderr, "Apparent infinite loop detected\n");
	abort();
    }
    return pread(fd, buf, count, offset);
}

static void
insert(int i, DB_TXN *txn)
{
    char hello[30], there[30];
    snprintf(hello, sizeof(hello), "hello%d", i);
    snprintf(there, sizeof(there), "there%d", i);
    DBT key, val;
    int r=db->put(db, txn,
		  dbt_init(&key, hello, strlen(hello)+1),
		  dbt_init(&val, there, strlen(there)+1),
		  0);
    CKERR(r);
}

static void op_delete (int i, DB_TXN *x) {
    char hello[30];
    DBT key;
    if (verbose>1) printf("op_delete %d\n", i);
    snprintf(hello, sizeof(hello), "hello%d", i);
    int r = db->del(db, x,
		    dbt_init(&key,  hello, strlen(hello)+1),
		    0);
    CKERR(r);
}

static void
setup (void) {
    db_env_set_func_pread(my_pread);
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    r = db_env_create(&env, 0);                                                       CKERR(r);
    r = env->set_redzone(env, 0);                                                     CKERR(r);
    r = env->set_cachesize(env, 0, 128*1024, 1);                                      CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r = db_create(&db, env, 0);                                                       CKERR(r);
    r = db->set_pagesize(db, 4096);                                                   CKERR(r);
    {
	DB_TXN *txn;
	r = env->txn_begin(env, 0, &txn, 0);                                              CKERR(r);
	r = db->open(db, txn, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
	r = txn->commit(txn, 0);                                                          CKERR(r);
    }
    {
	DB_TXN *txn;
	r = env->txn_begin(env, 0, &txn, 0);                                              CKERR(r);
	for (int i=0; i<N; i++) insert(i, txn);
	r = txn->commit(txn, 0);                                                          CKERR(r);
    }
}

static void finish (void) {
    int r;
    r = db->close(db, 0);                                                             CKERR(r);
    r = env->close(env, 0);                                                           CKERR(r);
}


int did_nothing = 0;

static int
do_nothing(DBT const *UU(a), DBT  const *UU(b), void *UU(c)) {
    did_nothing++;
    return 0;
}
static void run_del_next (void) {
    DB_TXN *txn;
    DBC *cursor;
    int r;
    r = env->txn_begin(env, 0, &txn, 0);                                              CKERR(r);
    for (int i=0; i<N; i++) op_delete(i, txn);

    r = db->cursor(db, txn, &cursor, 0);                                              CKERR(r);
    if (verbose) printf("read_next\n");
    n_preads = 0;
    r = cursor->c_getf_next(cursor, 0, do_nothing, NULL);                             CKERR2(r, DB_NOTFOUND);
    assert(did_nothing==0);
    if (verbose) printf("n_preads=%ld\n", n_preads);
    r = cursor->c_close(cursor);                                                      CKERR(r);
    r = txn->commit(txn, 0);                                                          CKERR(r);
}

static void run_del_prev (void) {
    DB_TXN *txn;
    DBC *cursor;
    int r;
    r = env->txn_begin(env, 0, &txn, 0);                                              CKERR(r);
    for (int i=0; i<N; i++) op_delete(i, txn);

    r = db->cursor(db, txn, &cursor, 0);                                              CKERR(r);
    if (verbose) printf("read_prev\n");
    n_preads = 0;
    r = cursor->c_getf_prev(cursor, 0, do_nothing, NULL);                             CKERR2(r, DB_NOTFOUND);
    assert(did_nothing==0);
    if (verbose) printf("n_preads=%ld\n", n_preads);
    r = cursor->c_close(cursor);                                                      CKERR(r);
    r = txn->commit(txn, 0);                                                          CKERR(r);
}

static void run_test (void) {
    setup();
    run_del_next();
    finish();

    setup();
    run_del_prev();
    finish();
}
int test_main (int argc, char*const argv[]) {
    parse_args(argc, argv);
    run_test();
    printf("n_preads=%ld\n", n_preads);
    return 0;
}


