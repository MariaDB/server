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
    r = db->set_pagesize(db, 10000);
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

    // now we change the pagesize. In 6.1.0, this would eventually cause a crash
    r = db->change_pagesize(db, 1024);
    CKERR(r);

    r = env->txn_begin(env, 0, &txn, 0);
    CKERR(r);
    for (uint64_t i = 0; i < 10000; i++) {
        DBT key, val;
        uint64_t k = 10000+i;
        uint64_t v = i;
        dbt_init(&key, &k, sizeof k);
        dbt_init(&val, &v, sizeof v);
        db->put(db, txn, &key, &val, DB_PRELOCKED_WRITE); // adding DB_PRELOCKED_WRITE just to make the test go faster
    }
    r = txn->commit(txn, 0);
    CKERR(r);

    r = db->close(db, 0);
    CKERR(r);

    r = env->close(env, 0);
    assert(r == 0);

    return 0;
}
