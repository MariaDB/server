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

// Make sure that the pending stuff gets checkpointed, but subsequent changes don't, even with concurrent updates.
#include "test.h"
#include <stdio.h>
#include <unistd.h>
#include "cachetable-test.h"
#include "cachetable/checkpoint.h"
#include <portability/toku_atomic.h>

static int N; // how many items in the table
static CACHEFILE cf;
static CACHETABLE ct;
int    *values;

static const int item_size = sizeof(int);

static volatile int n_flush, n_write_me, n_keep_me, n_fetch;

static void
sleep_random (void)
{
    toku_timespec_t req = {.tv_sec  = 0,
			   .tv_nsec = random()%1000000}; //Max just under 1ms
    nanosleep(&req, NULL);
}

int expect_value = 42; // initially 42, later 43

static void
flush (
    CACHEFILE UU(thiscf), 
    int UU(fd), 
    CACHEKEY UU(key), 
    void *value, 
    void** UU(dd),
    void *UU(extraargs), 
    PAIR_ATTR size, 
    PAIR_ATTR* UU(new_size), 
    bool write_me, 
    bool keep_me, 
    bool UU(for_checkpoint),
        bool UU(is_clone)
    )
{
    // printf("f");
    assert(size.size== item_size);
    int *CAST_FROM_VOIDP(v, value);
    if (*v!=expect_value) printf("got %d expect %d\n", *v, expect_value);
    assert(*v==expect_value);
    (void)toku_sync_fetch_and_add(&n_flush, 1);
    if (write_me) (void)toku_sync_fetch_and_add(&n_write_me, 1);
    if (keep_me)  (void)toku_sync_fetch_and_add(&n_keep_me, 1);
    sleep_random();
}

static void*
do_update (void *UU(ignore))
{
    while (n_flush==0); // wait until the first checkpoint ran
    int i;
    for (i=0; i<N; i++) {
	CACHEKEY key = make_blocknum(i);
        uint32_t hi = toku_cachetable_hash(cf, key);
        void *vv;
        CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
        wc.flush_callback = flush;
        int r = toku_cachetable_get_and_pin(cf, key, hi, &vv, wc, fetch_die, def_pf_req_callback, def_pf_callback, true, 0);
	//printf("g");
	assert(r==0);
        PAIR_ATTR attr;
        r = toku_cachetable_get_attr(cf, key, hi, &attr);
        assert(r==0);
	assert(attr.size==sizeof(int));
	int *CAST_FROM_VOIDP(v, vv);
	assert(*v==42);
	*v = 43;
	//printf("[%d]43\n", i);
	r = toku_test_cachetable_unpin(cf, key, hi, CACHETABLE_DIRTY, make_pair_attr(item_size));
	sleep_random();
    }
    return 0;
}

static void*
do_checkpoint (void *UU(v))
{
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    int r = toku_checkpoint(cp, NULL, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert(r == 0);
    return 0;
}

// put n items into the cachetable, mark them dirty, and then concurently
//   do a checkpoint (in which the callback functions are slow)
//   replace the n items with new values
// make sure that the stuff that was checkpointed includes only the old versions
// then do a flush and make sure the new items are written

static void checkpoint_pending(void) {
    if (verbose) { printf("%s:%d n=%d\n", __FUNCTION__, __LINE__, N); fflush(stdout); }
    const int test_limit = N;
    int r;
    toku_cachetable_create(&ct, test_limit*sizeof(int), ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    r = unlink(fname1); if (r!=0) CKERR2(get_error_errno(), ENOENT);
    r = toku_cachetable_openf(&cf, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    create_dummy_functions(cf);
    
    // Insert items into the cachetable. All dirty.
    int i;
    for (i=0; i<N; i++) {
        CACHEKEY key = make_blocknum(i);
        uint32_t hi = toku_cachetable_hash(cf, key);
	values[i] = 42;
        CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
        wc.flush_callback = flush;
        toku_cachetable_put(cf, key, hi, &values[i], make_pair_attr(sizeof(int)), wc, put_callback_nop);
        assert(r == 0);

        r = toku_test_cachetable_unpin(cf, key, hi, CACHETABLE_DIRTY, make_pair_attr(item_size));
        assert(r == 0);
    }

    // the checkpoint should cause n writes, but since n <= the cachetable size,
    // all items should be kept in the cachetable
    n_flush = n_write_me = n_keep_me = n_fetch = 0;
    expect_value = 42;
    // printf("E42\n");
    toku_pthread_t checkpoint_thread, update_thread;
    r = toku_pthread_create(toku_uninstrumented,
                            &checkpoint_thread,
                            nullptr,
                            do_checkpoint,
                            nullptr);
    assert(r == 0);
    r = toku_pthread_create(
        toku_uninstrumented, &update_thread, nullptr, do_update, nullptr);
    assert(r == 0);
    r = toku_pthread_join(checkpoint_thread, 0);
    assert(r == 0);
    r = toku_pthread_join(update_thread, 0);
    assert(r == 0);

    assert(n_flush == N && n_write_me == N && n_keep_me == N);

    // after the checkpoint, all of the items should be 43
    //printf("E43\n");
    n_flush = n_write_me = n_keep_me = n_fetch = 0; expect_value = 43;
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    r = toku_checkpoint(cp, NULL, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert(r == 0);
    assert(n_flush == N && n_write_me == N && n_keep_me == N);

    // a subsequent checkpoint should cause no flushes, or writes since all of the items are clean
    n_flush = n_write_me = n_keep_me = n_fetch = 0;

    r = toku_checkpoint(cp, NULL, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert(r == 0);
    assert(n_flush == 0 && n_write_me == 0 && n_keep_me == 0);

    toku_cachefile_close(&cf, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    {
	struct timeval tv;
	gettimeofday(&tv, 0);
	srandom(tv.tv_sec * 1000000 + tv.tv_usec);
    }	
    {
	int i;
	for (i=1; i<argc; i++) {
	    if (strcmp(argv[i], "-v") == 0) {
		verbose++;
		continue;
	    }
	}
    }
    for (N=1; N<=128; N*=2) {
	int myvalues[N];
	values = myvalues;
        checkpoint_pending();
	//printf("\n");
    }
    return 0;
}
