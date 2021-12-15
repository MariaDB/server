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

// verify that closing the cachetable with prefetches in progress works

#include "test.h"

bool check_flush;
bool expect_full_flush;
bool expect_pe;

static void
flush (CACHEFILE f __attribute__((__unused__)),
       int UU(fd),
       CACHEKEY k  __attribute__((__unused__)),
       void *v     __attribute__((__unused__)),
       void** UU(dd),
       void *e     __attribute__((__unused__)),
       PAIR_ATTR s      __attribute__((__unused__)),
       PAIR_ATTR* new_size      __attribute__((__unused__)),
       bool w      __attribute__((__unused__)),
       bool keep   __attribute__((__unused__)),
       bool c      __attribute__((__unused__)),
        bool UU(is_clone)
       ) {
    assert(expect_full_flush);
    sleep(2);
}

static int fetch_calls = 0;

static int
fetch (CACHEFILE f        __attribute__((__unused__)),
       PAIR UU(p),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       uint32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       void** UU(dd),
       PAIR_ATTR *sizep        __attribute__((__unused__)),
       int  *dirtyp       __attribute__((__unused__)),
       void *extraargs    __attribute__((__unused__))
       ) {

    fetch_calls++;

    *value = 0;
    *sizep = make_pair_attr(8);
    *dirtyp = 0;

    return 0;
}

static void 
pe_est_callback(
    void* UU(ftnode_pv),
    void* UU(dd), 
    long* bytes_freed_estimate, 
    enum partial_eviction_cost *cost, 
    void* UU(write_extraargs)
    )
{
    *bytes_freed_estimate = 0;
    *cost = PE_EXPENSIVE;
}

static void cachetable_eviction_full_test (void) {
    const int test_limit = 12;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    CACHEKEY key = make_blocknum(0);
    uint32_t fullhash = toku_cachetable_hash(f1, make_blocknum(0));

    void* value1;
    void* value2;
    //
    // let's pin a node multiple times
    // and really bring up its clock count
    //
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = flush;
    wc.pe_est_callback = pe_est_callback;
    for (int i = 0; i < 20; i++) {
        r = toku_cachetable_get_and_pin(
            f1, 
            key, 
            fullhash, 
            &value1, 
            wc, 
            fetch,
            def_pf_req_callback,
            def_pf_callback,
            true, 
            0
            );
        assert(r==0);
        r = toku_test_cachetable_unpin(f1, key, fullhash, CACHETABLE_DIRTY, make_pair_attr(1));
        assert(r == 0);
    }
    expect_full_flush = true;
    // now pin a different, causing an eviction
    wc.flush_callback = def_flush;
    wc.pe_est_callback = pe_est_callback;
    r = toku_cachetable_get_and_pin(
        f1, 
        make_blocknum(1), 
        1, 
        &value2, 
        wc, 
        fetch,
        def_pf_req_callback,
        def_pf_callback,
        true, 
        0
        );
    assert(r==0);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(1));
    assert(r == 0);
    toku_cachetable_verify(ct);

    // close with the eviction in progress. the close should block until
    // all of the reads and writes are complete.
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    cachetable_eviction_full_test();
    return 0;
}
