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

/* DB_CURRENT */

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <db.h>


// TOKU_TEST_FILENAME is defined in the Makefile

DB_ENV *env;
DB *db;
DB_TXN* null_txn = NULL;

int
test_main (int UU(argc), char UU(*const argv[])) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);    CKERR(r);
    r=db_env_create(&env, 0); CKERR(r);
    r=env->open(env, TOKU_TEST_FILENAME, DB_PRIVATE|DB_INIT_MPOOL|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);
    r = db->open(db, null_txn, "foo.db", "main", DB_BTREE, DB_CREATE, 0666); CKERR(r);
    DBC *cursor;
    r = db->cursor(db, null_txn, &cursor, 0); CKERR(r);
    DBT key, val;
    DBT ckey, cval;
    int k1 = 1, v1=7;
    enum foo { blob = 1 };
    int k2 = 2;
    int v2 = 8;
    r = db->put(db, null_txn, dbt_init(&key, &k1, sizeof(k1)), dbt_init(&val, &v1, sizeof(v1)), 0);
        CKERR(r);
    r = db->put(db, null_txn, dbt_init(&key, &k2, sizeof(k2)), dbt_init(&val, &v2, sizeof(v2)), 0);
        CKERR(r);

    r = cursor->c_get(cursor, dbt_init(&ckey, NULL, 0), dbt_init(&cval, NULL, 0), DB_LAST); 
        CKERR(r);
    //Copies a static pointer into val.
    r = db->get(db, null_txn, dbt_init(&key, &k1, sizeof(k1)), dbt_init(&val, NULL, 0), 0);
        CKERR(r);
    assert(val.data != &v1);
    assert(*(int*)val.data == v1);

    r = cursor->c_get(cursor, dbt_init(&ckey, NULL, 0), dbt_init(&cval, NULL, 0), DB_LAST); 
        CKERR(r);

    //Does not corrupt it.
    assert(val.data != &v1);
    assert(*(int*)val.data == v1);

    r = cursor->c_get(cursor, &ckey, &cval, DB_CURRENT); 
        CKERR(r);

    assert(*(int*)val.data == v1); // Will bring up valgrind error.


    r = db->del(db, null_txn, &ckey, DB_DELETE_ANY); assert(r == 0);
        CKERR(r);

    assert(*(int*)val.data == v1); // Will bring up valgrind error.

    r = cursor->c_close(cursor);
        CKERR(r);
    r=db->close(db, 0);       CKERR(r);
    r=env->close(env, 0);     CKERR(r);
    return 0;
}

