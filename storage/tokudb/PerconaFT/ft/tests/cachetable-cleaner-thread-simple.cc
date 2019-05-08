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
// This test verifies that the cleaner thread doesn't call the callback if
// nothing needs flushing.
//

CACHEFILE f1;
bool my_cleaner_callback_called;

static int
my_cleaner_callback(
    void* UU(ftnode_pv),
    BLOCKNUM UU(blocknum),
    uint32_t UU(fullhash),
    void* UU(extraargs)
    )
{
    assert(blocknum.b == 100);  // everything is pinned so this should never be called
    assert(fullhash == 100);
    PAIR_ATTR attr = make_pair_attr(8);
    attr.cache_pressure_size = 100;
    int r = toku_test_cachetable_unpin(f1, make_blocknum(100), 100, CACHETABLE_CLEAN, attr);
    my_cleaner_callback_called = true;
    return r;
}

static void
run_test (void) {
    const int test_limit = 1000;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    toku_set_cleaner_period(ct, 1);
    my_cleaner_callback_called = false;

    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    
    void* vs[5];
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.cleaner_callback = my_cleaner_callback;
    r = toku_cachetable_get_and_pin(f1, make_blocknum(100), 100, &vs[4],
                                    wc,
                                    def_fetch,
                                    def_pf_req_callback,
                                    def_pf_callback,
                                    true, 
                                    NULL);
    PAIR_ATTR attr = make_pair_attr(8);
    attr.cache_pressure_size = 100;
    r = toku_test_cachetable_unpin(f1, make_blocknum(100), 100, CACHETABLE_CLEAN, attr);

    for (int i = 0; i < 4; ++i) {
        r = toku_cachetable_get_and_pin(f1, make_blocknum(i+1), i+1, &vs[i],
                                        wc,
                                        def_fetch,
                                        def_pf_req_callback,
                                        def_pf_callback,
                                        true, 
                                        NULL);
        assert_zero(r);
        // set cachepressure_size to 0
        attr = make_pair_attr(8);
        attr.cache_pressure_size = 0;
        r = toku_test_cachetable_unpin(f1, make_blocknum(i+1), i+1, CACHETABLE_CLEAN, attr);
        assert_zero(r);
    }

    usleep(4000000);
    assert(my_cleaner_callback_called);

    toku_cachetable_verify(ct);
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
  default_parse_args(argc, argv);
  run_test();
  return 0;
}
