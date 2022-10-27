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

/**
 * Test that read committed always isolation works.
 *
 * Read committed means 'always read the outermost committed value'. This is less isolated
 * than 'read committed', which MySQl defines as 'snapshot isolation per sub-statement (child txn)'
 */

#include <portability/toku_random.h>

#include "test.h"

static void test_simple_committed_read(DB_ENV *env) {
    int r;
    DB *db;
    r = db_create(&db, env, 0); CKERR(r);
    r = db->open(db, NULL, "db", NULL, DB_BTREE, DB_CREATE, 0644); CKERR(r);

    char valbuf[64];
    DBT john, christian, val;
    dbt_init(&john, "john", sizeof("john"));
    dbt_init(&christian, "christian", sizeof("christian"));
    dbt_init(&val, valbuf, sizeof(valbuf));

    // start with just john
    r = db->put(db, NULL, &john, &john, 0); CKERR(r);

    // begin an outer txn with read-committed-always isolation
    DB_TXN *outer_txn;
    r = env->txn_begin(env, NULL, &outer_txn, DB_READ_COMMITTED_ALWAYS); CKERR(r);

    // outer txn sees john
    r = db->get(db, outer_txn, &john, &val, 0); CKERR(r);

    // outer txn does not yet see christian
    r = db->get(db, outer_txn, &christian, &val, 0); CKERR2(r, DB_NOTFOUND);

    // insert christian in another txn (NULL means generate an auto-commit txn)
    r = db->put(db, NULL, &christian, &christian, 0); CKERR(r);

    // outer txn does not see christian, because it is provisional
    // and our copied snapshot says it is not committed
    r = db->get(db, outer_txn, &christian, &val, 0); CKERR2(r, DB_NOTFOUND);

    // insert christian in another txn (again), thereby autocommitting last put
    r = db->put(db, NULL, &christian, &christian, 0); CKERR(r);

    // outer txn sees christian, because we now have a committed version
    r = db->get(db, outer_txn, &christian, &val, 0); CKERR(r);

    // delete john in another txn
    r = db->del(db, NULL, &john, 0); CKERR(r);

    // outer txn no longer sees john
    r = db->get(db, outer_txn, &john, &val, 0); CKERR2(r, DB_NOTFOUND);

    r = outer_txn->commit(outer_txn, 0); CKERR(r);

    r = db->close(db, 0); CKERR(r);
    r = env->dbremove(env, NULL, "db", NULL, 0); CKERR(r);
}

int test_main(int argc, char * const argv[]) {
    default_parse_args(argc, argv);

    int r;
    const int envflags = DB_INIT_MPOOL | DB_CREATE | DB_THREAD |
                         DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN | DB_PRIVATE;

    // startup
    DB_ENV *env;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, 0755); CKERR(r);
    r = db_env_create(&env, 0); CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, envflags, 0755);

    test_simple_committed_read(env);

    // cleanup
    r = env->close(env, 0); CKERR(r);

    return 0;
}

