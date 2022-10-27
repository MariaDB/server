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
#include <inttypes.h>
#include <sys/stat.h>
#include <db.h>


static uint64_t
size_from (uint32_t gbytes, uint32_t bytes) {
    return ((uint64_t)gbytes << 30) + bytes;
}

static inline void
size_to (uint64_t s, uint32_t *gbytes, uint32_t *bytes) {
    *gbytes = s >> 30;
    *bytes = s & ((1<<30) - 1);
}

static inline void
expect_le (uint64_t a, uint32_t gbytes, uint32_t bytes) {
    uint64_t b = size_from(gbytes, bytes);
    if (a != b && verbose)
        printf("WARNING: expect %" PRIu64 " got %" PRIu64 "\n", a, b);
    assert(a <= b);
}
 

static void
test_cachesize (void) {
#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 3
    int r;
    DB_ENV *env;
    uint32_t gbytes, bytes; int ncache;

    r = db_env_create(&env, 0); assert(r == 0);
    r = env->get_cachesize(env, &gbytes, &bytes, &ncache); assert(r == 0);
    if (verbose) printf("default %u %u %d\n", gbytes, bytes, ncache);

    r = env->set_cachesize(env, 0, 0, 1); assert(r == 0);
    r = env->get_cachesize(env, &gbytes, &bytes, &ncache); assert(r == 0);
    if (verbose) printf("minimum %u %u %d\n", gbytes, bytes, ncache);
    uint64_t minsize = size_from(gbytes, bytes);

    uint64_t s = 1; size_to(s, &gbytes, &bytes);
    while (gbytes <= 32) {
        r = env->set_cachesize(env, gbytes, bytes, ncache); 
        if (r != 0) {
            if (verbose) printf("max %u %u\n", gbytes, bytes);
            break;
        }
        assert(r == 0);
        r = env->get_cachesize(env, &gbytes, &bytes, &ncache); assert(r == 0);
        assert(ncache == 1);
        if (s <= minsize)
            expect_le(minsize, gbytes, bytes);
        else
            expect_le(s, gbytes, bytes);
        s *= 2; size_to(s, &gbytes, &bytes);
    }
    r = env->close(env, 0); assert(r == 0);
#endif
}


int
test_main(int argc, char *const argv[]) {
    parse_args(argc, argv);
    test_cachesize();

    return 0;
}
