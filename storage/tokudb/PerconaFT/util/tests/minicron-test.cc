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
#include "test.h"
#include "util/minicron.h"
#include <unistd.h>

#include <string.h>
#include <stdlib.h>

static double
tdiff (struct timeval *a, struct timeval *b) {
    return (a->tv_sec-b->tv_sec) + (a->tv_usec-b->tv_usec)*1e-6;
}

struct timeval starttime;
static double elapsed (void) {
    struct timeval now;
    gettimeofday(&now, 0);
    return tdiff(&now, &starttime);
}

static int 
#ifndef GCOV
__attribute__((__noreturn__))
#endif
never_run (void *a) {
    assert(a==0);
    assert(0);
#if defined(GCOV)
    return 0;
#endif
}

// Can we start something with period=0 (the function should never run) and shut it down.
static void*
test1 (void* v)
{
    struct minicron m;
    memset(&m, 0, sizeof(struct minicron));
    int r = toku_minicron_setup(&m, 0, never_run, 0);   assert(r==0);
    sleep(1);
    r = toku_minicron_shutdown(&m);                     assert(r==0);
    return v;
}

// Can we start something with period=10 and shut it down after 2 seconds (the function should never run) .
static void*
test2 (void* v)
{
    struct minicron m;
    memset(&m, 0, sizeof(struct minicron));
    int r = toku_minicron_setup(&m, 10000, never_run, 0);   assert(r==0);
    sleep(2);
    r = toku_minicron_shutdown(&m);                     assert(r==0);
    return v;
}

struct tenx {
    struct timeval tv;
    int counter;
};

static int
run_5x (void *v) {
    struct tenx *CAST_FROM_VOIDP(tx, v);
    struct timeval now;
    gettimeofday(&now, 0);
    double diff = tdiff(&now, &tx->tv);
    if (verbose) printf("T=%f tx->counter=%d\n", diff, tx->counter);
    // We only verify that the timer was not premature.  
    // Sometimes it will be delayed, but there's no good way to test it and nothing we can do about it.
    if (!(diff>0.5 + tx->counter)) {
      printf("T=%f tx->counter=%d\n", diff, tx->counter);
      assert(0);
    }
    tx->counter++;
    return 0;
}

// Start something with period=1 and run it a few times
static void*
test3 (void* v)
{
    struct minicron m;
    struct tenx tx;
    gettimeofday(&tx.tv, 0);
    tx.counter=0;
    memset(&m, 0, sizeof(struct minicron));
    int r = toku_minicron_setup(&m, 1000, run_5x, &tx);   assert(r==0);
    sleep(5);
    r = toku_minicron_shutdown(&m);                     assert(r==0);
    assert(tx.counter>=4 && tx.counter<=5); // after 5 seconds it could have run 4 or 5 times.
    return v;
}

static int
run_3sec (void *v) {
    if (verbose) printf("start3sec at %.6f\n", elapsed());
    int *CAST_FROM_VOIDP(counter, v);
    (*counter)++;
    sleep(3);
    if (verbose) printf("end3sec at %.6f\n", elapsed());
    return 0;
}

// make sure that if f is really slow that it doesn't run too many times
static void*
test4 (void *v) {
    struct minicron m;
    int counter = 0;
    memset(&m, 0, sizeof(struct minicron));
    int r = toku_minicron_setup(&m, 2000, run_3sec, &counter); assert(r==0);
    sleep(10);
    r = toku_minicron_shutdown(&m);                     assert(r==0);
    assert(counter==3);
    return v;
}

static void*
test5 (void *v) {
    struct minicron m;
    int counter = 0;
    memset(&m, 0, sizeof(struct minicron));
    int r = toku_minicron_setup(&m, 10000, run_3sec, &counter); assert(r==0);
    toku_minicron_change_period(&m, 2000);
    sleep(10);
    r = toku_minicron_shutdown(&m);                     assert(r==0);
    assert(counter==3);
    return v;
}

static void*
test6 (void *v) {
    struct minicron m;
    memset(&m, 0, sizeof(struct minicron));
    int r = toku_minicron_setup(&m, 5000, never_run, 0); assert(r==0);
    toku_minicron_change_period(&m, 0);
    sleep(7);
    r = toku_minicron_shutdown(&m);                          assert(r==0);
    return v;
}

// test that we actually run once per period, even if the execution is long
static void*
test7 (void *v) {
    struct minicron m;
    memset(&m, 0, sizeof(struct minicron));
    int counter = 0;
    int r = toku_minicron_setup(&m, 5000, run_3sec, &counter); assert(r==0);
    sleep(17);
    r = toku_minicron_shutdown(&m);                     assert(r==0);
    assert(counter==3);
    return v;
}

typedef void*(*ptf)(void*);
int
test_main (int argc, const char *argv[]) {
    default_parse_args(argc,argv);
    gettimeofday(&starttime, 0);

    ptf testfuns[] = {test1, test2, test3,
                      test4,
                      test5,
                      test6,
                      test7
    };
#define N (sizeof(testfuns)/sizeof(testfuns[0]))
    toku_pthread_t tests[N];

    unsigned int i;
    for (i = 0; i < N; i++) {
        int r = toku_pthread_create(
            toku_uninstrumented, tests + i, nullptr, testfuns[i], nullptr);
        assert(r == 0);
    }
    for (i = 0; i < N; i++) {
        void *v;
        int r=toku_pthread_join(tests[i], &v);
        assert(r==0);
        assert(v==0);
    }
    return 0;
}
