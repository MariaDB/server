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
#include <fcntl.h>
#include <sys/resource.h>

// try to open the environment with a small number of unused file descriptors

int test_main (int argc __attribute__((__unused__)), char *const argv[] __attribute__((__unused__))) {
    int r;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);

    DB_ENV *env;
    r = db_env_create(&env, 0);
    assert(r == 0);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(r == 0);
    r = env->close(env, 0);
    assert(r == 0);

    struct rlimit nofile_limit;
    r = getrlimit(RLIMIT_NOFILE, &nofile_limit);
    assert(r == 0);
    const int N = 100;
    nofile_limit.rlim_cur = N;
    r = setrlimit(RLIMIT_NOFILE, &nofile_limit);
    assert(r == 0);

    // compute the number of unused file descriptors    
    int fds[N];
    for (int i = 0; i < N; i++) {
        fds[i] = -1;
    }
    int unused = 0;
    for (int i = 0; i < N; i++, unused++) {
        fds[i] = open("/dev/null", O_RDONLY);
        if (fds[i] == -1)
            break;
    }
    for (int i = 0; i < N; i++) {
        if (fds[i] != -1) {
            close(fds[i]);
        }
    }

    // try to open the environment with a constrained number of unused file descriptors. the env open should return an error rather than crash.
    for (int n = N - unused; n < N; n++) {
        nofile_limit.rlim_cur = n;
        r = setrlimit(RLIMIT_NOFILE, &nofile_limit);
        assert(r == 0);
        
        r = db_env_create(&env, 0);
        assert(r == 0);
        r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD|DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
        if (r == 0) {
            r = env->close(env, 0);
            assert(r == 0);
            break;
        }
        assert(r == EMFILE);
        r = env->close(env, 0);
        assert(r == 0);
    }

    return 0;
}
