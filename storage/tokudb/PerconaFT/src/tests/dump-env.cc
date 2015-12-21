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
#include <db.h>
#include <sys/stat.h>

static DB_ENV *env;
static DB *db;
DB_TXN *txn;


static void
setup (void) {
    int r;
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    if (r != 0) {
        CKERR2(errno, EEXIST);
    }

    r=db_env_create(&env, 0); CKERR(r);
    r=env->set_redzone(env, 0); CKERR(r);
    r=env->set_default_bt_compare(env, int_dbt_cmp); CKERR(r);
    env->set_errfile(env, stderr);
    
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_MPOOL|DB_INIT_TXN|DB_CREATE|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=db_create(&db, env, 0); CKERR(r);

    r=env->txn_begin(env, 0, &txn, 0); assert(r==0);
    r=db->open(db, txn, "foo.db", 0, DB_BTREE, DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r=txn->commit(txn, 0);    assert(r==0);
}

static void
test_shutdown (void) {
    int r;
    r= db->close(db, 0); CKERR(r);
    r= env->close(env, 0); CKERR(r);
}

static void
doit(void) {
    int r;

    DBC *dbc;
    r=env->txn_begin(env, 0, &txn, 0); CKERR(r);
    r = env->get_cursor_for_persistent_environment(env, txn, &dbc); CKERR(r);
    DBT key;
    DBT val;
    dbt_init_realloc(&key);
    dbt_init_realloc(&val);

    while ((r = dbc->c_get(dbc, &key, &val, DB_NEXT)) == 0) {
        if (verbose) {
            printf("ENTRY\n\tKEY [%.*s]",
                    key.size,
                    (char*)key.data);
            if (val.size == sizeof(uint32_t)) {
                //assume integer
                printf("\n\tVAL [%" PRIu32"]\n",
                        toku_dtoh32(*(uint32_t*)val.data));
            } else if (val.size == sizeof(uint64_t)) {
                //assume 64 bit integer
                printf("\n\tVAL [%" PRIu64"]\n",
                        toku_dtoh64(*(uint64_t*)val.data));
            } else {
                printf("\n\tVAL [%.*s]\n",
                        val.size,
                        (char*)val.data);
            }
        }
    }
    CKERR2(r, DB_NOTFOUND);
    r = dbc->c_close(dbc);
    CKERR(r);
    r = txn->commit(txn, 0);
    CKERR(r);

    toku_free(key.data);
    toku_free(val.data);
}

int
test_main (int argc, char * const argv[]) {
    parse_args(argc, argv);

    setup();
    doit();
    test_shutdown();

    return 0;
}

