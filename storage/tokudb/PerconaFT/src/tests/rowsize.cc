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

static DB_ENV *env = NULL;
static DB *db = NULL;
static const char *envdir = TOKU_TEST_FILENAME;

static void setup_env (void) {
    const int len = strlen(envdir)+100;
    char cmd[len];
    snprintf(cmd, len, "rm -rf %s", envdir);
    {int r = system(cmd);                                                                                                               CKERR(r); }
    {int r = toku_os_mkdir(envdir, S_IRWXU+S_IRWXG+S_IRWXO);                                                                            CKERR(r); }
    {int r = db_env_create(&env, 0);                                                                                                    CKERR(r); }
    //env->set_errfile(env, stderr);
    CKERR(env->set_redzone(env, 0));
    { int r = env->open(env, envdir, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r); }
    { int r = db_create(&db, env, 0);                                                                                                   CKERR(r); }
    { int r = db->open(db, NULL, "foo.db", 0, DB_BTREE, DB_CREATE | DB_AUTO_COMMIT, S_IRWXU+S_IRWXG+S_IRWXO);                           CKERR(r); }
}

static void shutdown_env (void) {
    { int r = db->close(db, 0);   CKERR(r); }
    { int r = env->close(env, 0); CKERR(r); }
}

static void put (const char *keystring, int size, bool should_work) {
    DBT k, v;
    dbt_init(&k, keystring, 1+strlen(keystring));
    dbt_init(&v, toku_xcalloc(size, 1), size);
    static DB_TXN *txn = NULL;
    { int r = env->txn_begin(env, 0, &txn, 0); CKERR(r); }
    {
	int r = db->put(db, NULL, &k, &v, 0);
	if (should_work) {
	    CKERR(r);
	} else {
	    assert(r!=0);
	}
    }
    { int r = txn->commit(txn, 0); CKERR(r); }
    toku_free(v.data);
}

int test_main (int argc, char *const argv[]) {
    if (0) parse_args(argc, argv);
    setup_env();
    if (0) put("foo", 32, true);
    put("foo", 32*1024*1024, true);
    put("bar", 32*1024*1024+1, false);
    shutdown_env();
    
    return 0;
}
