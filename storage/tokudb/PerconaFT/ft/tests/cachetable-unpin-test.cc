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


static void
cachetable_unpin_test (int n) {
    const int test_limit = 2*n;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    int i;
    for (i=1; i<=n; i++) {
        uint32_t hi;
        CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
        hi = toku_cachetable_hash(f1, make_blocknum(i));
        toku_cachetable_put(f1, make_blocknum(i), hi, (void *)(long)i, make_pair_attr(1), wc, put_callback_nop);
        assert(toku_cachefile_count_pinned(f1, 0) == i);

        void *v;
        r = toku_cachetable_maybe_get_and_pin(f1, make_blocknum(i), hi, PL_WRITE_EXPENSIVE, &v);
        assert(r == -1);
        assert(toku_cachefile_count_pinned(f1, 0) == i);

        //r = toku_test_cachetable_unpin(f1, make_blocknum(i), hi, CACHETABLE_CLEAN, 1);
        //assert(r == 0);
        assert(toku_cachefile_count_pinned(f1, 0) == i);
    }
    for (i=n; i>0; i--) {
        uint32_t hi;
        hi = toku_cachetable_hash(f1, make_blocknum(i));
        r = toku_test_cachetable_unpin(f1, make_blocknum(i), hi, CACHETABLE_CLEAN, make_pair_attr(1));
        assert(r == 0);
        assert(toku_cachefile_count_pinned(f1, 0) == i-1);
    }
    assert(toku_cachefile_count_pinned(f1, 1) == 0);
    toku_cachetable_verify(ct);

    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

enum unpin_evictor_test_type {
    unpin_increase,
    unpin_decrease,
    unpin_invalid_attr
};

static void
unpin_and_evictor_test(enum unpin_evictor_test_type test_type) {
    int r;
    CACHETABLE ct;
    int test_limit = 4;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    evictor_test_helpers::set_hysteresis_limits(&ct->ev, test_limit, test_limit);
    evictor_test_helpers::disable_ev_thread(&ct->ev);

    void* value2;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    // this should put in the cachetable a pair of size 8
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
    //
    // now we unpin, 
    // if we increase the size, we should catch a sleep
    // if we don't increase the size, there should be no sleep
    // if we pass in an invalid pair_attr, there should be no sleep.
    //
    uint64_t old_num_ev_runs = 0;
    uint64_t new_num_ev_runs = 0;
    if (test_type == unpin_increase) {
        old_num_ev_runs = evictor_test_helpers::get_num_eviction_runs(&ct->ev);
        r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_DIRTY, make_pair_attr(9));
        new_num_ev_runs = evictor_test_helpers::get_num_eviction_runs(&ct->ev);
        assert(new_num_ev_runs > old_num_ev_runs);
    }
    else if (test_type == unpin_decrease || test_type == unpin_invalid_attr) {
        old_num_ev_runs = evictor_test_helpers::get_num_eviction_runs(&ct->ev);
        r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_DIRTY, make_pair_attr(8));
        new_num_ev_runs = evictor_test_helpers::get_num_eviction_runs(&ct->ev);
        assert(new_num_ev_runs == old_num_ev_runs);
    }
    else {
        assert(false);
    }

    toku_cachetable_verify(ct);
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    cachetable_unpin_test(8);
    unpin_and_evictor_test(unpin_increase);
    unpin_and_evictor_test(unpin_decrease);
    unpin_and_evictor_test(unpin_invalid_attr);
    return 0;
}
