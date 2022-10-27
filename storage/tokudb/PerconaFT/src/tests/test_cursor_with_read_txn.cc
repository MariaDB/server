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

int test_main(int argc, char * const argv[])
{
    int r;
    DB * db;
    DB_ENV * env;
    (void) argc;
    (void) argv;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, 0755); { int chk_r = r; CKERR(chk_r); }

    // set things up
    r = db_env_create(&env, 0); 
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, 0755); 
    CKERR(r);
    r = db_create(&db, env, 0); 
    CKERR(r);
    r = db->open(db, NULL, "foo.db", NULL, DB_BTREE, DB_CREATE, 0644); 
    CKERR(r);


    DB_TXN* txn = NULL;
    r = env->txn_begin(env, 0, &txn, DB_TXN_SNAPSHOT);
    CKERR(r);

    int k = 1;
    int v = 10;
    DBT key, val;
    r = db->put(
        db, 
        txn, 
        dbt_init(&key, &k, sizeof k), 
        dbt_init(&val, &v, sizeof v), 
        0
        );
    CKERR(r);
    k = 2;
    v = 20;
    r = db->put(
        db, 
        txn, 
        dbt_init(&key, &k, sizeof k), 
        dbt_init(&val, &v, sizeof v), 
        0
        );
    CKERR(r);
    r = txn->commit(txn, 0);
    CKERR(r);
    
    r = env->txn_begin(env, 0, &txn, DB_TXN_SNAPSHOT | DB_TXN_READ_ONLY);
    CKERR(r);
    DBC* cursor = NULL;
    r = db->cursor(db, txn, &cursor, 0);
    CKERR(r);
    DBT key1, val1;
    memset(&key1, 0, sizeof key1);
    memset(&val1, 0, sizeof val1);
    r = cursor->c_get(cursor, &key1, &val1, DB_FIRST);
    CKERR(r);
    invariant(key1.size == sizeof(int));
    invariant(*(int *)key1.data == 1);
    invariant(val1.size == sizeof(int));
    invariant(*(int *)val1.data == 10);

    r = cursor->c_get(cursor, &key1, &val1, DB_NEXT);
    CKERR(r);
    invariant(key1.size == sizeof(int));
    invariant(*(int *)key1.data == 2);
    invariant(val1.size == sizeof(int));
    invariant(*(int *)val1.data == 20);

    r = cursor->c_close(cursor);
    CKERR(r);
    r = txn->commit(txn, 0);
    CKERR(r);    

    // clean things up
    r = db->close(db, 0); 
    CKERR(r);
    r = env->close(env, 0); 
    CKERR(r);

    return 0;
}
