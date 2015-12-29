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

int test_main(int argc, char * const argv[])
{
    int r;
    DB * db;
    DB_ENV * env;
    (void) argc;
    (void) argv;

    const char *db_env_dir = TOKU_TEST_FILENAME;
    char rm_cmd[strlen(db_env_dir) + strlen("rm -rf ") + 1];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", db_env_dir);

    r = system(rm_cmd); { int chk_r = r; CKERR(chk_r); }
    r = toku_os_mkdir(db_env_dir, 0755); { int chk_r = r; CKERR(chk_r); }

    // set things up
    r = db_env_create(&env, 0); 
    CKERR(r);
    r = env->open(env, db_env_dir, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, 0755); 
    CKERR(r);
    r = db_create(&db, env, 0); 
    CKERR(r);
    r = db->open(db, NULL, "foo.db", NULL, DB_BTREE, DB_CREATE, 0644); 
    CKERR(r);


    DB_TXN* txn1 = NULL;
    DB_TXN* txn2 = NULL;
    r = env->txn_begin(env, 0, &txn1, DB_TXN_READ_ONLY);
    CKERR(r);
    r = env->txn_begin(env, 0, &txn2, DB_TXN_READ_ONLY);
    CKERR(r);

    
    r=db->pre_acquire_table_lock(db, txn1); CKERR(r);
    r=db->pre_acquire_table_lock(db, txn2); CKERR2(r, DB_LOCK_NOTGRANTED);

    r = txn1->commit(txn1, 0);
    CKERR(r);
    r = txn2->commit(txn2, 0);
    CKERR(r);    

    // clean things up
    r = db->close(db, 0); 
    CKERR(r);
    r = env->close(env, 0); 
    CKERR(r);

    return 0;
}
