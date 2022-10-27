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

/* Simple test of logging.  Can I start PerconaFT with logging enabled? */

#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <db.h>


// TOKU_TEST_FILENAME is defined in the Makefile

DB_ENV *env;
DB *db;

int
test_main (int UU(argc), char UU(*const argv[])) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);                                                           assert(r==0);
    r=db_env_create(&env, 0);                                                     CKERR(r);
    r=env->open(env, TOKU_TEST_FILENAME, DB_PRIVATE|DB_INIT_MPOOL|DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO);                         CKERR(r);
    r=db_create(&db, env, 0);                                                     CKERR(r);
    r=db->open(db, NULL, "doesnotexist.db", "testdb", DB_BTREE, 0, 0666);         assert(r==ENOENT);
    r=db->open(db, NULL, "doesnotexist.db", "testdb", DB_BTREE, DB_CREATE, 0666); CKERR(r);
    r=db->close(db, 0);                                                           CKERR(r);
    r=env->close(env, 0);                                                         CKERR(r);
    return 0;
}
