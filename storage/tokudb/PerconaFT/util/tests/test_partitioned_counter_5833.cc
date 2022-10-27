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

// Demonstrate a race if #5833 isn't fixed.

#include <pthread.h>
#include <toku_portability.h>
#include <util/partitioned_counter.h>
#include "test.h"


static void pt_create (pthread_t *thread, void *(*f)(void*), void *extra) {
    int r = pthread_create(thread, NULL, f, extra);
    assert(r==0);
}

static void pt_join (pthread_t thread, void *expect_extra) {
    void *result;
    int r = pthread_join(thread, &result);
    assert(r==0);
    assert(result==expect_extra);
}

static int verboseness_cmdarg=0;

static void parse_args (int argc, const char *argv[]) {
    const char *progname = argv[1];
    argc--; argv++;
    while (argc>0) {
	if (strcmp(argv[0], "-v")==0) verboseness_cmdarg++;
	else {
	    printf("Usage: %s [-v]\n", progname);
	    exit(1);
	}
	argc--; argv++;
    }
}

#define NCOUNTERS 2
PARTITIONED_COUNTER array_of_counters[NCOUNTERS];

static void *counter_init_fun(void *tnum_pv) {
    int *tnum_p = (int*)tnum_pv;
    int tnum = *tnum_p;
    assert(0<=tnum  && tnum<NCOUNTERS);
    array_of_counters[tnum] = create_partitioned_counter();
    return tnum_pv;
}

static void do_test_5833(void) {
    pthread_t threads[NCOUNTERS];
    int       tids[NCOUNTERS];
    for (int i=0; i<NCOUNTERS; i++) {
        tids[i] = i;
        pt_create(&threads[i], counter_init_fun, &tids[i]);
    }
    for (int i=0; i<NCOUNTERS; i++) {
        pt_join(threads[i], &tids[i]);
        destroy_partitioned_counter(array_of_counters[i]);
    }
}

int test_main(int argc, const char *argv[]) {
    parse_args(argc, argv);
    do_test_5833();
    return 0;
}
