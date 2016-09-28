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
test_txn_abort (int n) {
    if (verbose>1) printf("%s %s:%d\n", __FILE__, __FUNCTION__, n);

    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    DB_ENV *env;
    r = db_env_create(&env, 0); assert(r == 0);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL + DB_INIT_LOG + DB_INIT_LOCK + DB_INIT_TXN + DB_PRIVATE + DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); 
    if (r != 0) printf("%s:%d:%d:%s\n", __FILE__, __LINE__, r, db_strerror(r));
    assert(r == 0);

    DB_TXN *txn = 0;
    r = env->txn_begin(env, 0, &txn, 0); assert(r == 0);

    DB *db;
    r = db_create(&db, env, 0); assert(r == 0);
    r = db->open(db, txn, "test.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); assert(r == 0);
    r = txn->commit(txn, 0); assert(r == 0);

    r = env->txn_begin(env, 0, &txn, 0); assert(r == 0);
    int i;
    for (i=0; i<n; i++) {
        DBT key, val;
	int i2=htonl(i*2);
	if (verbose>2) printf("put %d\n", i*2);
        r = db->put(db, txn, dbt_init(&key, &i2, sizeof i2), dbt_init(&val, &i, sizeof i), 0); 
        if (r != 0) printf("%s:%d:%d:%s\n", __FILE__, __LINE__, r, db_strerror(r));
        assert(r == 0);
    }
    r = txn->commit(txn, 0);
    
    r = env->txn_begin(env, 0, &txn, 0); assert(r == 0);
    for (i=0; i<n; i++) {
        DBT key;
	int i2=htonl(i*2);
	if (verbose>2) printf("del %d\n", i*2);
        r = db->del(db, txn, dbt_init(&key, &i2, sizeof i2), 0);
        if (r != 0) printf("%s:%d:%d:%s\n", __FILE__, __LINE__, r, db_strerror(r));
        assert(r == 0);
    }
    r = txn->abort(txn); 
    if (r != 0) printf("%s:%d:abort:%d\n", __FILE__, __LINE__, r);
    assert(r == 0);
    /* walk the db, even numbers should be there */
    r = env->txn_begin(env, 0, &txn, 0); assert(r == 0);
    DBC *cursor;
    r = db->cursor(db, txn, &cursor, 0); assert(r == 0);
    DBT key; memset(&key, 0, sizeof key);
    DBT val; memset(&val, 0, sizeof val);
    for (i=0; 1; i++) {
	r = cursor->c_get(cursor, &key, &val, DB_NEXT);
	if (r!=0) break;
	if (verbose>2) printf("%u present\n", (uint32_t)ntohl(*(int*)key.data));
	assert(key.size==4);
	assert(ntohl(*(int*)key.data)==(unsigned int)(2*i));
    }
    assert(i==n);
    r = cursor->c_close(cursor); assert(r == 0);
    r = txn->commit(txn, 0);

    r = db->close(db, 0); assert(r == 0);
    r = env->close(env, 0); assert(r == 0);
}

int
test_main(int argc, char *const argv[]) {
    int i;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbose++;
            continue;
        }
    }
    if (verbose>0) printf("%s", __FILE__);
    if (verbose>1) printf("\n");
    for (i=1; i<100; i++) 
        test_txn_abort(i);
    if (verbose>1) printf("%s OK\n", __FILE__);
    if (verbose>0) printf(" OK\n");
    return 0;
}
