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

//Verify aborting transactions works properly when transaction 
//starts with an empty db and a table lock.

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <db.h>
#include <memory.h>
#include <stdio.h>


// TOKU_TEST_FILENAME is defined in the Makefile

DB_ENV *env;
DB *db;
DB_TXN *null_txn = NULL;
DB_TXN *txn;
DB_TXN *childtxn;
uint32_t find_num;

static void
init(void) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_env_create(&env, 0); CKERR(r);
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_PRIVATE|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);
    r=db->open(db, null_txn, "foo.db", 0, DB_BTREE, DB_CREATE|DB_EXCL, S_IRWXU|S_IRWXG|S_IRWXO);
    CKERR(r);
    r=db->close(db, 0); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);
    r=db->open(db, null_txn, "foo.db", 0, DB_BTREE, 0, S_IRWXU|S_IRWXG|S_IRWXO);
    CKERR(r);
    r=env->txn_begin(env, 0, &txn, 0); CKERR(r);
    r=db->pre_acquire_table_lock(db, txn); CKERR(r);
    r=env->txn_begin(env, txn, &childtxn, 0); CKERR(r);
}

static void
tear_down(void) {
    int r;
    r = db->close(db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
}

static void
abort_childtxn(void) {
    find_num = 0;
    int r;
    r = txn->abort(childtxn); CKERR(r);
    r = txn->commit(txn, 0); CKERR(r);
    childtxn = NULL;
    txn = NULL;
}

static void
abort_both(void) {
    find_num = 0;
    int r;
    r = txn->abort(childtxn); CKERR(r);
    r = txn->abort(txn); CKERR(r);
    childtxn = NULL;
    txn = NULL;
}

static void
abort_parent(void) {
    int r = txn->abort(txn); CKERR(r);
}

static void
abort_txn(int type) {
         if (type==0) abort_parent();
    else if (type==1) abort_childtxn();
    else if (type==2) abort_both();
    else assert(false);

    find_num = 0;
    childtxn = NULL;
    txn = NULL;
}

static void
put(uint32_t k, uint32_t v) {
    int r;
    DBT key,val;
    static uint32_t kvec[128];
    static uint32_t vvec[128];

    kvec[0] = k;
    vvec[0] = v;
    dbt_init(&key, &kvec[0], sizeof(kvec));
    dbt_init(&val, &vvec[0], sizeof(vvec));
    r = db->put(db, childtxn ? childtxn : txn, &key, &val, 0); CKERR(r);
}

static void
test_insert_and_abort(uint32_t num_to_insert, int abort_type) {
    if (verbose>1) printf("\t" __FILE__ ": insert+abort(%u,%d)\n", num_to_insert, abort_type);
    find_num = 0;
    
    uint32_t k;
    uint32_t v;

    uint32_t i;
    for (i=0; i < num_to_insert; i++) {
        k = htonl(i);
        v = htonl(i+num_to_insert);
        put(k, v);
    }
    abort_txn(abort_type);
}

static void
test_insert_and_abort_and_insert(uint32_t num_to_insert, int abort_type) {
    if (verbose>1) printf("\t" __FILE__ ": insert+abort+insert(%u,%d)\n", num_to_insert, abort_type);
    test_insert_and_abort(num_to_insert, abort_type); 
    find_num = num_to_insert / 2;
    uint32_t k, v;
    uint32_t i;
    for (i=0; i < find_num; i++) {
        k = htonl(i);
        v = htonl(i+5);
        put(k, v);
    }
}

#define bit0 (1<<0)
#define bit1 (1<<1)

static int
do_nothing(DBT const *UU(a), DBT  const *UU(b), void *UU(c)) {
    return 0;
}

static void
verify_and_tear_down(int close_first) {
    int r;
    {
        char *filename;
        {
            DBT dname;
            DBT iname;
            dbt_init(&dname, "foo.db", sizeof("foo.db"));
            dbt_init(&iname, NULL, 0);
            iname.flags |= DB_DBT_MALLOC;
            r = env->get_iname(env, &dname, &iname);
            CKERR(r);
            CAST_FROM_VOIDP(filename, iname.data);
            assert(filename);
        }
        toku_struct_stat statbuf;
        char fullfile[TOKU_PATH_MAX + 1];
        r = toku_stat(toku_path_join(fullfile, 2, TOKU_TEST_FILENAME, filename),
                      &statbuf,
                      toku_uninstrumented);
        assert(r == 0);
        toku_free(filename);
    }
    if (close_first) {
        r=db->close(db, 0); CKERR(r);
        r=db_create(&db, env, 0); CKERR(r);
        r=db->open(db, null_txn, "foo.db", 0, DB_BTREE, 0, S_IRWXU|S_IRWXG|S_IRWXO);
        CKERR(r);
    }
    DBC *cursor;
    r=env->txn_begin(env, 0, &txn, 0); CKERR(r);
    r = db->cursor(db, txn, &cursor, 0); CKERR(r);
    uint32_t found = 0;
    do {
        r = cursor->c_getf_next(cursor, 0, do_nothing, NULL);
        if (r==0) found++;
    } while (r==0);
    CKERR2(r, DB_NOTFOUND);
    cursor->c_close(cursor);
    txn->commit(txn, 0);
    assert(found==find_num);
    tear_down();
}

static void
runtests(int abort_type) {
    if (verbose) printf("\t" __FILE__ ": runtests(%d)\n", abort_type);
    int close_first;
    for (close_first = 0; close_first < 2; close_first++) {
        init();
        abort_txn(abort_type);
        verify_and_tear_down(close_first);
        uint32_t n;
        for (n = 1; n < 1<<10; n*=2) {
            init();
            test_insert_and_abort(n, abort_type);
            verify_and_tear_down(close_first);

            init();
            test_insert_and_abort_and_insert(n, abort_type);
            verify_and_tear_down(close_first);
        }
    }
}

int
test_main (int argc, char *const argv[]) {
    parse_args(argc, argv);
    int abort_type;
    for (abort_type = 0; abort_type<3; abort_type++) {
        runtests(abort_type);
    }
    return 0;
}

