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
#include "toku_os.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <toku_assert.h>
#include <toku_pthread.h>
#include "util/queue.h"

static int verbose=1;

static int count_0 = 0;
static uint64_t e_max_weight=0, d_max_weight = 0; // max weight seen by enqueue thread and dequeue thread respectively.

static void *start_0 (void *arg) {
    QUEUE q = (QUEUE)arg;
    void *item;
    uint64_t weight;
    long count = 0;
    while (1) {
	uint64_t this_max_weight;
	int r=toku_queue_deq(q, &item, &weight, &this_max_weight);
	if (r==EOF) break;
	assert(r==0);
	if (this_max_weight>d_max_weight) d_max_weight=this_max_weight;
	long v = (long)item;
	//printf("D(%ld)=%ld %ld\n", v, this_max_weight, d_max_weight);
	assert(v==count);
	count_0++;
	count++;
    }
    return NULL;
}

static void enq (QUEUE q, long v, uint64_t weight) {
    uint64_t this_max_weight;
    int r = toku_queue_enq(q, (void*)v, (weight==0)?0:1, &this_max_weight);
    assert(r==0);
    if (this_max_weight>e_max_weight) e_max_weight=this_max_weight;
    //printf("E(%ld)=%ld %ld\n", v, this_max_weight, e_max_weight);
}

static void queue_test_0 (uint64_t weight)
// Test a queue that can hold WEIGHT items.
{
    //printf("\n");
    count_0 = 0;
    e_max_weight = 0;
    d_max_weight = 0;
    QUEUE q;
    int r;
    r = toku_queue_create(&q, weight);
    assert(r == 0);
    toku_pthread_t thread;
    r = toku_pthread_create(toku_uninstrumented, &thread, nullptr, start_0, q);
    assert(r == 0);
    enq(q, 0L, weight);
    enq(q, 1L, weight);
    enq(q, 2L, weight);
    enq(q, 3L, weight);
    sleep(1);
    enq(q, 4L, weight);
    enq(q, 5L, weight);
    r = toku_queue_eof(q);                                      assert(r==0);
    void *result;
    r = toku_pthread_join(thread, &result);	           assert(r==0);
    assert(result==NULL);
    assert(count_0==6);
    r = toku_queue_destroy(q);
    assert(d_max_weight <= weight);
    assert(e_max_weight <= weight);
}


static void parse_args (int argc, const char *argv[]) {
    const char *progname=argv[0];
    argc--; argv++;
    while (argc>0) {
	if (strcmp(argv[0],"-v")==0) {
	    verbose++;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose--;
	} else {
	    fprintf(stderr, "Usage:\n %s [-v] [-q]\n", progname);
	    exit(1);
	}
	argc--; argv++;
    }
    if (verbose<0) verbose=0;
}

int main (int argc, const char *argv[]) {
    parse_args(argc, argv);
    queue_test_0(0LL);
    queue_test_0(1LL);
    queue_test_0(2LL);
    return 0;
}
