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

#define _CRT_SECURE_NO_DEPRECATE
#include <test.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <toku_stdint.h>
#include <unistd.h>
#include <toku_assert.h>
#include "toku_os.h"

int test_main(int argc, char *const argv[]) {
    int verbose = 0;
    int limit = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
            continue;
        }
        if (strcmp(argv[i], "--timeit") == 0) {
            limit = 100000;
            continue;
        }
    }

    int r;

#if 0
    r = toku_get_filesystem_sizes(NULL, NULL, NULL, NULL);
    assert(r == EFAULT);
#endif

    r = toku_get_filesystem_sizes(".", NULL, NULL, NULL);
    assert(r == 0);

    uint64_t free_size = 0, avail_size = 0, total_size = 0;
    for (int i = 0; i < limit; i++) {
        r = toku_get_filesystem_sizes(".", &avail_size, &free_size, &total_size);
        assert(r == 0);
        assert(avail_size <= free_size && free_size <= total_size);
    }
    if (verbose) {
        printf("avail=%" PRIu64 "\n", avail_size);
        printf("free=%" PRIu64 "\n", free_size);
        printf("total=%" PRIu64 "\n", total_size);
    }

    return 0;
}
