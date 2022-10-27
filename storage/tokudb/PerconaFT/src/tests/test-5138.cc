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

// test that with full optimizations including the "last IPO pass" and
// static linking doesn't break lzma

#include "test.h"

int test_main(int argc, char * const argv[]) {
    int r;
    parse_args(argc, argv);
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    DB_ENV *env;
    r = db_env_create(&env, 0);
    CKERR(r);
    env->set_errfile(env, stderr);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD|DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);

    DB *db;
    DB_TXN *txn = NULL;
    r = env->txn_begin(env, NULL, &txn, 0);
    CKERR(r);
    r = db_create(&db, env, 0);
    CKERR(r);
    r = db->set_compression_method(db, TOKU_LZMA_METHOD);
    CKERR(r);
    r = db->open(db, txn, "foo.db", NULL, DB_BTREE, DB_CREATE, 0666);
    CKERR(r);

    DBT key, val;
    unsigned int i;
    DBT *keyp = dbt_init(&key, &i, sizeof(i));
    DBT *valp = dbt_init(&val, &i, sizeof(i));
    for (i = 0; i < 1000; ++i) {
        r = db->put(db, txn, keyp, valp, 0);
        CKERR(r);
    }

    r = txn->commit(txn, 0);
    CKERR(r);

    r = db->close(db, 0);
    CKERR(r);
    r = env->close(env, 0);
    CKERR(r);

    return 0;
}
