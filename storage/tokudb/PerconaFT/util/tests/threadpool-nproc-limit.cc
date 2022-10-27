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

// this test verifies that the toku thread pool is resilient when hitting the nproc limit.

#include <util/threadpool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>

int verbose = 0;

static int usage(void) {
    fprintf(stderr, "[-q] [-v] [--verbose] (%d)\n", verbose);
    return 1;
}

static void *f(void *arg) {
    return arg;
}

static int dotest(int the_limit) {
    if (verbose)
        fprintf(stderr, "%s:%u %d\n", __FILE__, __LINE__, the_limit);
    int r;
    struct toku_thread_pool *pool = nullptr;
    r = toku_thread_pool_create(&pool, 10);
    assert(r == 0 && pool != nullptr);

    struct rlimit current_nproc_limit;
    r = getrlimit(RLIMIT_NPROC, &current_nproc_limit);
    assert(r == 0);
    
    struct rlimit new_nproc_limit = current_nproc_limit;
    new_nproc_limit.rlim_cur = the_limit;
    r = setrlimit(RLIMIT_NPROC, &new_nproc_limit);
    assert(r == 0);

    int want_n = 20;
    int got_n = want_n;
    r = toku_thread_pool_run(pool, 0, &got_n, f, nullptr);
    if (r == 0)
        assert(want_n == got_n);
    else {
        assert(r == EWOULDBLOCK);
        assert(got_n <= want_n);
    }

    r = setrlimit(RLIMIT_NPROC, &current_nproc_limit);
    assert(r == 0);

    if (verbose)
        toku_thread_pool_print(pool, stderr);
    toku_thread_pool_destroy(&pool);
    return got_n > 0;
}

int main(int argc, char *argv[]) {
    // parse args
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] != '-')
            break;
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbose = verbose+1;
            continue;
        }
        if (strcmp(arg, "-q") == 0) {
            verbose = verbose > 0 ? verbose-1 : 0;
            continue;
        }
        return usage();
    }
    // set increasing nproc limits until the test succeeds in hitting the limit after > 0 threads are created
    for (int i = 0; 1; i++) {
        if (dotest(i))
            break;
    }
    return 0;
}
