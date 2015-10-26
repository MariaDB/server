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

#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include "test.h"


#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif
const double USECS_PER_SEC = 1000000.0;

static int
long_key_cmp(DB *UU(e), const DBT *a, const DBT *b)
{
    const long *CAST_FROM_VOIDP(x, a->data);
    const long *CAST_FROM_VOIDP(y, b->data);
    return (*x > *y) - (*x < *y);
}

static void
run_test(unsigned long eltsize, unsigned long nodesize, unsigned long repeat)
{
    int cur = 0;
    const int n = 1024;
    long keys[n];
    char *vals[n];
    for (int i = 0; i < n; ++i) {
        keys[i] = rand();
        XMALLOC_N(eltsize - (sizeof keys[i]), vals[i]);
        unsigned int j = 0;
        char *val = vals[i];
        for (; j < eltsize - (sizeof keys[i]) - sizeof(int); j += sizeof(int)) {
            int *p = cast_to_typeof(p) &val[j];
            *p = rand();
        }
        for (; j < eltsize - (sizeof keys[i]); ++j) {
            char *p = &val[j];
            *p = (rand() & 0xff);
        }
    }
    XIDS xids_0 = toku_xids_get_root_xids();
    XIDS xids_123;
    int r = toku_xids_create_child(xids_0, &xids_123, (TXNID)123);
    CKERR(r);

    NONLEAF_CHILDINFO bnc;
    long long unsigned nbytesinserted = 0;
    struct timeval t[2];
    gettimeofday(&t[0], NULL);

    toku::comparator cmp;
    cmp.create(long_key_cmp, nullptr);

    for (unsigned int i = 0; i < repeat; ++i) {
        bnc = toku_create_empty_nl();
        for (; toku_bnc_nbytesinbuf(bnc) <= nodesize; ++cur) {
            toku_bnc_insert_msg(bnc,
                                &keys[cur % n], sizeof keys[cur % n],
                                vals[cur % n], eltsize - (sizeof keys[cur % n]),
                                FT_NONE, next_dummymsn(), xids_123, true,
                                cmp); assert_zero(r);
        }
        nbytesinserted += toku_bnc_nbytesinbuf(bnc);
        destroy_nonleaf_childinfo(bnc);
    }

    for (int i = 0; i < n; ++i) {
        toku_free(vals[i]);
        vals[i] = nullptr;
    }

    toku_xids_destroy(&xids_123);

    gettimeofday(&t[1], NULL);
    double dt;
    dt = (t[1].tv_sec - t[0].tv_sec) + ((t[1].tv_usec - t[0].tv_usec) / USECS_PER_SEC);
    double mbrate = ((double) nbytesinserted / (1 << 20)) / dt;
    long long unsigned eltrate = (long) (cur / dt);
    printf("%0.03lf MB/sec\n", mbrate);
    printf("%llu elts/sec\n", eltrate);

    cmp.destroy();
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    unsigned long eltsize, nodesize, repeat;

    initialize_dummymsn();
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <eltsize> <nodesize> <repeat>\n", argv[0]);
        return 2;
    }
    eltsize = strtoul(argv[1], NULL, 0);
    nodesize = strtoul(argv[2], NULL, 0);
    repeat = strtoul(argv[3], NULL, 0);

    run_test(eltsize, nodesize, repeat);

    return 0;
}
