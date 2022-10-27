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

// use strace to very that the toku_fsync_directory function works

#include <stdlib.h>
#include <string.h>
#include "test.h"
#include <portability/toku_path.h>
#include <limits.h>

static int verbose = 0;

int test_main(int argc, char *const argv[]) {
    int r;

    for (int i=1; i<argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            if (verbose < 0) verbose = 0;
            verbose++;
            continue;
        } else if (strcmp(argv[i], "-q") == 0) {
            verbose = 0;
            continue;
        } else {
            exit(1);
        }
    }

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO);
    CKERR(r);
    char buf[TOKU_PATH_MAX + 1];
    r = toku_os_mkdir(toku_path_join(buf, 2, TOKU_TEST_FILENAME, "test"), S_IRWXU+S_IRWXG+S_IRWXO); CKERR(r);
    r = toku_fsync_directory(""); CKERR(r);
    r = toku_fsync_directory("."); CKERR(r);
    r = toku_fsync_directory(toku_path_join(buf, 3, TOKU_TEST_FILENAME, "test", "a")); CKERR(r);
    r = toku_fsync_directory(toku_path_join(buf, 4, ".", TOKU_TEST_FILENAME, "test", "a")); CKERR(r);
    r = toku_fsync_directory("/tmp/x"); CKERR(r);

    return 0;
}
