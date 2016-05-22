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

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <memory.h>
#include <errno.h>
#include <sys/stat.h>
#include <db.h>


static void
test_env_open_flags (int env_open_flags, int expectr) {
    if (verbose) printf("test_env_open_flags:%d\n", env_open_flags);

    DB_ENV *env;
    int r;

    r = db_env_create(&env, 0);
    assert(r == 0);
    env->set_errfile(env, 0);

    r = env->open(env, TOKU_TEST_FILENAME, env_open_flags, 0644);
    if (r != expectr && verbose) printf("env open flags=%x expectr=%d r=%d\n", env_open_flags, expectr, r);

    r = env->close(env, 0);
    assert(r == 0);
}

int
test_main(int argc, char *const argv[]) {

    parse_args(argc, argv);
  
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);

    char tracefile[TOKU_PATH_MAX+1];
    toku_set_trace_file(toku_path_join(tracefile, 2, TOKU_TEST_FILENAME, "trace.tktrace"));

    /* test flags */
    test_env_open_flags(0, ENOENT);
    // This one segfaults in BDB 4.6.21
    test_env_open_flags(DB_PRIVATE, ENOENT);
    test_env_open_flags(DB_PRIVATE+DB_CREATE, 0);
    test_env_open_flags(DB_PRIVATE+DB_CREATE+DB_INIT_MPOOL, 0);
    test_env_open_flags(DB_PRIVATE+DB_RECOVER, EINVAL);
    test_env_open_flags(DB_PRIVATE+DB_CREATE+DB_INIT_MPOOL+DB_RECOVER, EINVAL);

    toku_close_trace_file();

    return 0;
}
