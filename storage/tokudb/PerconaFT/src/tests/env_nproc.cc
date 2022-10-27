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
#include <sys/resource.h>

static void env_open_close(void) {
    int r;

    DB_ENV *env = NULL;
    r = db_env_create(&env, 0);
    assert(r == 0);
    env->set_errfile(env, stderr);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_LOCK+DB_INIT_MPOOL+DB_INIT_TXN+DB_INIT_LOG + DB_CREATE + DB_PRIVATE, S_IRWXU+S_IRWXG+S_IRWXO);
    if (r != 0) {
        fprintf(stderr, "%s:%u r=%d\n", __FILE__, __LINE__, r);
    }
    r = env->close(env, 0);
    assert(r == 0);
}

int test_main (int argc, char * const argv[]) {
    int r;
    int limit = 1;

    // parse_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            if (verbose > 0) verbose--;
            continue;
        }
        limit = atoi(argv[i]);
        continue;
    }

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    struct rlimit nproc_rlimit;
    r = getrlimit(RLIMIT_NPROC, &nproc_rlimit);
    assert(r == 0);

    nproc_rlimit.rlim_cur = limit;
    r = setrlimit(RLIMIT_NPROC, &nproc_rlimit);
    assert(r == 0);

    env_open_close();

    return 0;
}
