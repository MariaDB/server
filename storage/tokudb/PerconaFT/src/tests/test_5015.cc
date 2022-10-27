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


#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>

#include <unistd.h>
#include <db.h>
#include <errno.h>

// TOKU_TEST_FILENAME is defined in the Makefile

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);

    DB_TXN * const null_txn = 0;
    const char * const fname = "test.already.exists.ft_handle";
    int r;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); assert(r==0);

    DB_ENV *env;
    r = db_env_create(&env, 0); assert(r == 0);
    r = env->open(env, TOKU_TEST_FILENAME, DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL | DB_INIT_TXN | DB_INIT_LOCK | DB_INIT_LOG, 0); assert(r == 0);

    DB *db;
    r = db_create(&db, env, 0);
    CKERR(r);
    db->set_errfile(db,0); // Turn off those annoying errors
    r = db->open(db, null_txn, fname, "main", DB_BTREE, DB_CREATE, 0666);
    CKERR(r);
    DB_TXN* parent_txn = NULL;
    DB_TXN* child_txn = NULL;
    r = env->txn_begin(env, 0, &parent_txn, 0);
    CKERR(r);
    r = env->txn_begin(env, parent_txn, &child_txn, 0);
    CKERR(r);
    DBT key,val;
    r = db->put(db, child_txn, dbt_init(&key, "a", 2), dbt_init(&val, "a", 2), 0);       
    CKERR(r);
    uint8_t gid[DB_GID_SIZE];
    memset(gid, 0, DB_GID_SIZE);
    gid[0]='a';
    r = child_txn->prepare(child_txn, gid, 0);
    CKERR(r);

    r = env->txn_checkpoint(env, 0, 0, 0);
    CKERR(r);

    r = child_txn->commit(child_txn, 0);
    CKERR(r);
    r = parent_txn->commit(parent_txn, 0);
    CKERR(r);

    r = db->close(db, 0);                                                    CKERR(r);
    r = env->close(env, 0);                                                  CKERR(r);
    return 0;
}
