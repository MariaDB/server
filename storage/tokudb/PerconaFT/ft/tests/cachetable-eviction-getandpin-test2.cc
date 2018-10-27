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

/* verify that get_and_pin waits while a prefetch block is pending */

#include "test.h"


static void 
pe_est_callback(
    void* UU(ftnode_pv), 
    void* UU(dd),
    long* bytes_freed_estimate, 
    enum partial_eviction_cost *cost, 
    void* UU(write_extraargs)
    )
{
    *bytes_freed_estimate = 7;
    *cost = PE_EXPENSIVE;
}

static int 
pe_callback (
    void *ftnode_pv __attribute__((__unused__)), 
    PAIR_ATTR bytes_to_free __attribute__((__unused__)), 
    void* extraargs __attribute__((__unused__)),
    void (*finalize)(PAIR_ATTR new_attr, void *extra),
    void *finalize_extra
    ) 
{
    sleep(3);
    finalize(make_pair_attr(bytes_to_free.size - 7), finalize_extra);
    return 0;
}

static uint64_t tdelta_usec(struct timeval *tend, struct timeval *tstart) {
    uint64_t t = tend->tv_sec * 1000000 + tend->tv_usec;
    t -= tstart->tv_sec * 1000000 + tstart->tv_usec;
    return t;
}

static void cachetable_prefetch_maybegetandpin_test (void) {
    const int test_limit = 12;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    evictor_test_helpers::disable_ev_thread(&ct->ev);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    CACHEKEY key = make_blocknum(0);
    uint32_t fullhash = toku_cachetable_hash(f1, make_blocknum(0));

    // let's get and pin this node a bunch of times to drive up the clock count
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.pe_est_callback = pe_est_callback;
    wc.pe_callback = pe_callback;
    for (int i = 0; i < 20; i++) {
        void* value;
        r = toku_cachetable_get_and_pin(
            f1, 
            key, 
            fullhash, 
            &value, 
            wc, 
            def_fetch,
            def_pf_req_callback,
            def_pf_callback,
            true, 
            0
            );
        assert(r==0);
        r = toku_test_cachetable_unpin(f1, key, fullhash, CACHETABLE_DIRTY, make_pair_attr(8));
    }
    
    struct timeval tstart;
    gettimeofday(&tstart, NULL);

    // fetch another block, causing an eviction of the first block we made above
    void* value2;
    r = toku_cachetable_get_and_pin(
        f1,
        make_blocknum(1),
        1,
        &value2,
        wc, 
        def_fetch,
        def_pf_req_callback,
        def_pf_callback,
        true, 
        0
        );
    assert(r==0);
    ct->ev.signal_eviction_thread();
    usleep(1*1024*1024);        
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8));

        
    toku_cachetable_verify(ct);

    void *v = 0;
    // now verify that the block we are trying to evict may be pinned
    r = toku_cachetable_get_and_pin_nonblocking(
        f1, 
        key, 
        fullhash, 
        &v, 
        wc, 
        def_fetch, 
        def_pf_req_callback, 
        def_pf_callback, 
        PL_WRITE_EXPENSIVE,
        NULL, 
        NULL
        );
    assert(r==TOKUDB_TRY_AGAIN);
    r = toku_cachetable_get_and_pin(
        f1, 
        key, 
        fullhash, 
        &v, 
        wc, 
        def_fetch, 
        def_pf_req_callback, 
        def_pf_callback, 
        true, 
        NULL
        );
    assert(r == 0 && v == 0);
    PAIR_ATTR attr;
    r = toku_cachetable_get_attr(f1, key, fullhash, &attr);
    assert(r == 0 && attr.size == 1);

    struct timeval tend; 
    gettimeofday(&tend, NULL);

    assert(tdelta_usec(&tend, &tstart) >= 2000000); 
    if (verbose) printf("time %" PRIu64 " \n", tdelta_usec(&tend, &tstart));
    toku_cachetable_verify(ct);

    r = toku_test_cachetable_unpin(f1, key, fullhash, CACHETABLE_CLEAN, make_pair_attr(1));
    assert(r == 0);
    toku_cachetable_verify(ct);

    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    cachetable_prefetch_maybegetandpin_test();
    return 0;
}
