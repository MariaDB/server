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

#include <string>

#include "test.h"

static int compare_strings_case_insensitive(DB *db, const DBT *a, const DBT *b) {
    invariant_notnull(db);
    return strcasecmp(reinterpret_cast<char *>(a->data),
                      reinterpret_cast<char *>(b->data));
}

static void test_equal_keys_with_different_bytes(void) {
    int r;

    DB_ENV *env;
    r = db_env_create(&env, 0); CKERR(r);
    r = env->set_default_bt_compare(env, compare_strings_case_insensitive);
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE+DB_PRIVATE+DB_INIT_MPOOL+DB_INIT_TXN, 0); CKERR(r);

    DB *db;
    r = db_create(&db, env, 0); CKERR(r);
    r = db->open(db, NULL, "db", NULL, DB_BTREE, DB_CREATE, 0666); CKERR(r);

    DBT key;

    // put 'key'
    dbt_init(&key, "key", sizeof("key"));
    r = db->put(db, NULL, &key, &key, 0); CKERR(r);

    // del 'KEY' - should match 'key'
    dbt_init(&key, "KEY", sizeof("KEY"));
    r = db->del(db, NULL, &key, 0); CKERR(r);

    DBT val;
    char val_buf[10];
    dbt_init(&val, val_buf, sizeof(val_buf));

    // search should fail for 'key'
    dbt_init(&key, "key", sizeof("key"));
    r = db->get(db, NULL, &key, &val, 0); CKERR2(r, DB_NOTFOUND);

    // search should fail for 'KEY'
    dbt_init(&key, "KEY", sizeof("KEY"));
    r = db->get(db, NULL, &key, &val, 0); CKERR2(r, DB_NOTFOUND);

    r = db->close(db, 0); CKERR(r);
    r = env->close(env, 0); CKERR(r);
}

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);

    test_equal_keys_with_different_bytes();

    return 0;
}
