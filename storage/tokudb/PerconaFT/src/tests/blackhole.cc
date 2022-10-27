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

// Test that a db ignores insert messages in blackhole mode

#include "test.h"
#include <util/dbt.h>

static DB *db;
static DB *blackhole_db;
static DB_ENV *env;

static int num_inserts = 10000;

static void fill_dbt(DBT *dbt, void *data, size_t size) {
    dbt->data = data;
    dbt->size = dbt->ulen = size;
    dbt->flags = DB_DBT_USERMEM;
}

static void setup (bool use_txns) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r = db_env_create(&env, 0); CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, 0, 0);
    int txnflags = use_txns ? (DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN) : 0;
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE|DB_PRIVATE|txnflags, 0777);

    // create a regular db and a blackhole db
    r = db_create(&db, env, 0); CKERR(r);
    r = db_create(&blackhole_db, env, 0); CKERR(r);
    r = db->open(db, NULL, "test.db", 0, DB_BTREE,
            DB_CREATE,
            S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);
    r = blackhole_db->open(blackhole_db, NULL, "blackhole.db", 0, DB_BTREE, 
            DB_CREATE | DB_BLACKHOLE,
            S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);
}

static void cleanup (void) {
    int r;
    r = db->close(db, 0); CKERR(r);
    r = blackhole_db->close(blackhole_db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
}

static void test_blackhole(void) {
    int r = 0;

    for (int i = 0; i < num_inserts; i++) {
        int k = random();
        int v = k + 100;
        DBT key, value;
        fill_dbt(&key, &k, sizeof k); 
        fill_dbt(&value, &v, sizeof v); 

        // put a random key into the regular db.
        r = db->put(db, NULL, &key, &value, 0);
        assert(r == 0);

        // put that key into the blackhole db.
        r = blackhole_db->put(blackhole_db, NULL, &key, &value, 0);
        assert(r == 0);

        // we should be able to find this key in the regular db
        int get_v;
        DBT get_value;
        fill_dbt(&get_value, &get_v, sizeof get_v);
        r = db->get(db, NULL, &key, &get_value, 0);
        assert(r == 0);
        assert(*(int *)get_value.data == v);
        assert(get_value.size == sizeof v);

        // we shouldn't be able to get it back from the blackhole
        r = blackhole_db->get(blackhole_db, NULL, &key, &get_value, 0);
        assert(r == DB_NOTFOUND);
    }
}

int test_main (int argc __attribute__((__unused__)), char *const argv[] __attribute__((__unused__))) {
    // without txns
    setup(false);
    test_blackhole();
    cleanup();

    // with txns
    setup(true);
    test_blackhole();
    cleanup();
    return 0;
}
