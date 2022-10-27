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



static void
db_put (DB *db, int k, int v) {
    DB_TXN * const null_txn = 0;
    DBT key, val;
    int r = db->put(db, null_txn, dbt_init(&key, &k, sizeof k), dbt_init(&val, &v, sizeof v), 0);
    assert(r == 0);
}

static int
cursor_get (DBC *cursor, unsigned int *k, unsigned int *v, int op) {
    DBT key, val;
    int r = cursor->c_get(cursor, dbt_init_malloc(&key), dbt_init_malloc(&val), op);
    if (r == 0) {
        assert(key.size == sizeof *k); memcpy(k, key.data, key.size);
        assert(val.size == sizeof *v); memcpy(v, val.data, val.size);
    }
    if (key.data) toku_free(key.data);
    if (val.data) toku_free(val.data);
    return r;
}

static void
test_cursor_sticky (int n, int dup_mode) {
    if (verbose) printf("test_cursor_sticky:%d %d\n", n, dup_mode);

    DB_TXN * const null_txn = 0;
    const char * const fname = "test_cursor_sticky.ft_handle";
    int r;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    /* create the dup database file */
    DB_ENV *env;
    r = db_env_create(&env, 0); assert(r == 0);
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE+DB_PRIVATE+DB_INIT_MPOOL, 0); assert(r == 0);

    DB *db;
    r = db_create(&db, env, 0); assert(r == 0);
    r = db->set_flags(db, dup_mode); assert(r == 0);
    r = db->set_pagesize(db, 4096); assert(r == 0);
    r = db->open(db, null_txn, fname, "main", DB_BTREE, DB_CREATE, 0666); assert(r == 0);

    int i;
    unsigned int k, v;
    for (i=0; i<n; i++) {
        db_put(db, htonl(i), htonl(i));
    } 

    /* walk the tree */
    DBC *cursor;
    r = db->cursor(db, 0, &cursor, 0); assert(r == 0);
    for (i=0; i<n; i++) {
        // GCC 4.8 complains about these being maybe uninitialized.
        // TODO(leif): figure out why and fix it.
        k = 0; v = 0;
        r = cursor_get(cursor, &k, &v, DB_NEXT); assert(r == 0);
        assert(k == htonl(i)); assert(v == htonl(i));
    }

    r = cursor_get(cursor, &k, &v, DB_NEXT); assert(r == DB_NOTFOUND);

    r = cursor_get(cursor, &k, &v, DB_CURRENT); assert(r == 0); assert(k == htonl(n-1)); assert(v == htonl(n-1));

    r = cursor->c_close(cursor); assert(r == 0);

    r = db->close(db, 0); assert(r == 0);
    r = env->close(env, 0); assert(r == 0);
}


int
test_main(int argc, char *const argv[]) {
    int i;

    // setvbuf(stdout, NULL, _IONBF, 0);
    parse_args(argc, argv);
  
    for (i=1; i<65537; i *= 2) {
        test_cursor_sticky(i, 0);
    }
    return 0;
}
