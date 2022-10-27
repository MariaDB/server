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
#include <sys/resource.h>

// create 200 databases and close them.  set the open file limit to 100 and try to open all of them.
// eventually, the locktree can not clone fractal tree, and the db open fails.

int test_main (int argc __attribute__((__unused__)), char *const argv[] __attribute__((__unused__))) {
    int r;

    const int N = 200;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);

    DB_ENV *env;
    r = db_env_create(&env, 0);
    assert(r == 0);

    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);

    DB **dbs = new DB *[N];
    for (int i = 0; i < N; i++) {
        dbs[i] = NULL;
    }
    for (int i = 0; i < N; i++) {
        r = db_create(&dbs[i], env, 0);
        assert(r == 0);

        char dbname[32]; sprintf(dbname, "%d.test", i);
        r = dbs[i]->open(dbs[i], NULL, dbname, NULL, DB_BTREE, DB_AUTO_COMMIT+DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO);
        assert(r == 0);
    }

    for (int i = 0; i < N; i++) {
        if (dbs[i]) {
            r = dbs[i]->close(dbs[i], 0);
            assert(r == 0);
        }
    }

    struct rlimit nofile_limit = { N/2, N/2 };
    r = setrlimit(RLIMIT_NOFILE, &nofile_limit);
    // assert(r == 0); // valgrind does not like this
    if (r != 0) {
        printf("warning: set nofile limit to %d failed %d %s\n", N, errno, strerror(errno));
    }

    for (int i = 0; i < N; i++) {
        dbs[i] = NULL;
    }
    bool emfile_happened = false; // should happen since there are less than N unused file descriptors
    for (int i = 0; i < N; i++) {
        r = db_create(&dbs[i], env, 0);
        assert(r == 0);

        char dbname[32]; sprintf(dbname, "%d.test", i);
        r = dbs[i]->open(dbs[i], NULL, dbname, NULL, DB_BTREE, DB_AUTO_COMMIT, S_IRWXU+S_IRWXG+S_IRWXO);
        if (r == EMFILE) {
            emfile_happened = true;
            break;
        }
    }
    assert(emfile_happened);
    for (int i = 0; i < N; i++) {
        if (dbs[i]) {
            r = dbs[i]->close(dbs[i], 0);
            assert(r == 0);
        }
    }

    r = env->close(env, 0);
    assert(r == 0);

    delete [] dbs;

    return 0;
}
