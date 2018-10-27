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



//
// This test verifies that if a node is pinned by a thread
// doing get_and_pin_nonblocking while another thread is trying
// to unpin_and_remove it, that nothing bad happens.
//

CACHEFILE f1;
PAIR p1;
PAIR p2;


static int
fetch_one(CACHEFILE f        __attribute__((__unused__)),
       PAIR UU(p),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       uint32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       void **dd     __attribute__((__unused__)),
       PAIR_ATTR *sizep        __attribute__((__unused__)),
       int  *dirtyp,
       void *extraargs    __attribute__((__unused__))
       ) {
    *dirtyp = 0;
    *value = NULL;
    *sizep = make_pair_attr(8);
    assert(k.b == 1);
    p1 = p;
    return 0;
}

static int
fetch_two (CACHEFILE f        __attribute__((__unused__)),
       PAIR UU(p),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       uint32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       void **dd     __attribute__((__unused__)),
       PAIR_ATTR *sizep        __attribute__((__unused__)),
       int  *dirtyp,
       void *extraargs    __attribute__((__unused__))
       ) {
    *dirtyp = 0;
    *value = NULL;
    *sizep = make_pair_attr(8);
    assert(k.b == 2);
    p2 = p;
    return 0;
}

toku_pthread_t unpin_and_remove_tid;

static void *unpin_and_remove_one(void *UU(arg)) {
    int r = toku_cachetable_unpin_and_remove(
        f1, 
        p1, 
        NULL, 
        NULL
        );
    assert_zero(r);
    return arg;
}

static void
unpin_two (void* UU(v)) {
    int r = toku_cachetable_unpin_ct_prelocked_no_flush(
        f1,
        p2,
        CACHETABLE_DIRTY,
        make_pair_attr(8)
        );
    assert_zero(r);

    // at this point, we have p1 pinned, want to start a thread to do an
    // unpin_and_remove
    // on p1
    r = toku_pthread_create(toku_uninstrumented,
                            &unpin_and_remove_tid,
                            nullptr,
                            unpin_and_remove_one,
                            nullptr);
    assert_zero(r);
    // sleep to give a chance for the unpin_and_remove to get going
    usleep(512*1024);
}

static void *repin_one(void *UU(arg)) {
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    struct unlockers unlockers = {true, unpin_two, NULL, NULL};
    void* v1;
    int r = toku_cachetable_get_and_pin_nonblocking(
        f1,
        make_blocknum(1),
        1,
        &v1,
        wc,
        def_fetch,
        def_pf_req_callback,
        def_pf_callback,
        PL_WRITE_EXPENSIVE,
        NULL,
        &unlockers
        );
    assert(r == TOKUDB_TRY_AGAIN);
    return arg;
}



static void
cachetable_test (void) {
    const int test_limit = 1000;
    int r;
    toku_pair_list_set_lock_size(2); // set two bucket mutexes
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);

    // bring pairs 1 and 2 into memory, then unpin
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, fetch_one, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(2), 2, &v1, wc, fetch_two, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);

    toku_pthread_t tid1;
    r = toku_pthread_create(
        toku_uninstrumented, &tid1, nullptr, repin_one, nullptr);
    assert_zero(r);

    void *ret;
    r = toku_pthread_join(tid1, &ret); 
    assert_zero(r);
    r = toku_pthread_join(unpin_and_remove_tid, &ret); 
    assert_zero(r);

    toku_cachetable_verify(ct);
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    // test ought to run bunch of times in hope of hitting bug
    uint32_t num_test_runs = 1;
    for (uint32_t i = 0; i < num_test_runs; i++) {
        if (verbose) {
            printf("starting test run %" PRIu32 " \n", i);
        }
        cachetable_test();
    }
    return 0;
}
