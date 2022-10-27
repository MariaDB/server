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

#include <toku_portability.h>
#include <memory.h>
#include <toku_portability.h>
#include <db.h>

#include <errno.h>
#include <sys/stat.h>

#include "test.h"

// TOKU_TEST_FILENAME is defined in the Makefile
#define FNAME       "foo.tokudb"
const char *name = NULL;

#define NUM         8
#define MAX_LENGTH  (1<<16)

DB_ENV *env;

DB *db;
DB_TXN *null_txn;

static void
open_db(void) {
    int r = db_create(&db, env, 0);
    CKERR(r);
    r = db->open(db, null_txn, FNAME, name, DB_BTREE, DB_CREATE, 0666);
    CKERR(r);
}

static void
delete_db(void) {
    int r = env->dbremove(env, NULL, FNAME, name, 0); CKERR(r);
}

static void
close_db(void) {
    int r;
    r = db->close(db, 0);
    CKERR(r);
}

static void
setup_data(void) {
    int r = db_env_create(&env, 0);                                           CKERR(r);
    const int envflags = DB_CREATE|DB_INIT_MPOOL|DB_INIT_TXN|DB_INIT_LOCK |DB_THREAD |DB_PRIVATE;
    r = env->open(env, TOKU_TEST_FILENAME, envflags, S_IRWXU+S_IRWXG+S_IRWXO);        CKERR(r);
}

static void
runtest(void) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r=toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); assert(r==0);
    setup_data();

    name = "foo";
    open_db();
    close_db();
    delete_db();

    name = "foo1";
    open_db();
    close_db();
    name = "foo2";
    open_db();
    close_db();
    name = "foo1";
    delete_db();
    name = "foo2";
    delete_db();

    name = "foo1";
    open_db();
    close_db();
    name = "foo2";
    open_db();
    close_db();
    name = "foo2";
    delete_db();
    name = "foo1";
    delete_db();

    env->close(env, 0);
}


int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);

    runtest();
    return 0;
}                                        

