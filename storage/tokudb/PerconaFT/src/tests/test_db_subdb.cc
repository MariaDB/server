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

// TOKU_TEST_FILENAME is defined in the Makefile

int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    DB_ENV * env = 0;
    DB *db;
    DB_TXN * const null_txn = 0;
    const char * const fname = "test.db";
    int r;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);

    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); assert(r==0);

    r=db_env_create(&env, 0);   assert(r==0);
    // Note: without DB_INIT_MPOOL the BDB library will fail on db->open().
    r=env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_PRIVATE|DB_CREATE|DB_INIT_LOG|DB_INIT_TXN, S_IRWXU+S_IRWXG+S_IRWXO); assert(r==0);

    r = db_create(&db, env, 0);
    CKERR(r);

    r = db->open(db, null_txn, fname, "main", DB_BTREE, DB_CREATE, 0666);
    CKERR(r);
    
    r = db->close(db, 0);
    CKERR(r);

#if 0    
    const char * const fname2 = "test2.db";
    // This sequence segfaults in BDB 4.3.29
    // See what happens if we open a database with a subdb, when the file has only the main db.
    r = db->open(db, null_txn, fname2, 0, DB_BTREE, DB_CREATE, 0666);
    CKERR(r);
    r = db->close(db,0);
    CKERR(r);
    r = db->open(db, null_txn, fname2, "main", DB_BTREE, 0, 0666);
    CKERR(r);
    r = db->close(db, 0);
    CKERR(r);
#endif

    r = env->close(env, 0);
    CKERR(r);

    return 0;
}
