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
    BLOCKNUM blocknum,
    uint32_t fullhash,
    void* UU(extraargs)
    )
{
    PAIR_ATTR attr = make_pair_attr(8);
    attr.cache_pressure_size = 0;
    int r = toku_test_cachetable_unpin(f1, blocknum, fullhash, CACHETABLE_CLEAN, attr);
    my_cleaner_callback_called = true;
    return r;
}

// point of this test is to have two pairs that have the same fullhash, 
// and therefore, the same bucket mutex
static void
run_test (void) {
    const int test_limit = 1000;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    my_cleaner_callback_called = false;

    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    
    void* vs[5];
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.cleaner_callback = my_cleaner_callback;
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &vs[0],
                                    wc,
                                    def_fetch,
                                    def_pf_req_callback,
                                    def_pf_callback,
                                    true, 
                                    NULL);
    PAIR_ATTR attr = make_pair_attr(8);
    attr.cache_pressure_size = 100;
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, attr);

    r = toku_cachetable_get_and_pin(f1, make_blocknum(2), 1, &vs[1],
                                    wc,
                                    def_fetch,
                                    def_pf_req_callback,
                                    def_pf_callback,
                                    true, 
                                    NULL);
    attr = make_pair_attr(8);
    attr.cache_pressure_size = 50;
    r = toku_test_cachetable_unpin(f1, make_blocknum(2), 1, CACHETABLE_CLEAN, attr);

    toku_cleaner_thread_for_test(ct);

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
