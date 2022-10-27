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
test_cachetable_def_flush (int n) {
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    const int test_limit = 2*n;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);
    assert_zero(r);
    char fname1[TOKU_PATH_MAX+1];
    unlink(toku_path_join(fname1, 2, TOKU_TEST_FILENAME, "test1.dat"));
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    char fname2[TOKU_PATH_MAX+1];
    unlink(toku_path_join(fname2, 2, TOKU_TEST_FILENAME, "test2.dat"));
    CACHEFILE f2;
    r = toku_cachetable_openf(&f2, ct, fname2, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    // insert keys 0..n-1 
    int i;
    for (i=0; i<n; i++) {
        uint32_t hi;
        hi = toku_cachetable_hash(f1, make_blocknum(i));
        toku_cachetable_put(f1, make_blocknum(i), hi, (void *)(long)i, make_pair_attr(1), wc, put_callback_nop);
        r = toku_test_cachetable_unpin(f1, make_blocknum(i), hi, CACHETABLE_CLEAN, make_pair_attr(1));
        assert(r == 0);
        hi = toku_cachetable_hash(f2, make_blocknum(i));
        toku_cachetable_put(f2, make_blocknum(i), hi, (void *)(long)i, make_pair_attr(1), wc, put_callback_nop);
        r = toku_test_cachetable_unpin(f2, make_blocknum(i), hi, CACHETABLE_CLEAN, make_pair_attr(1));
        assert(r == 0);
    }
    toku_cachetable_verify(ct);

    // verify keys exists
    for (i=0; i<n; i++) {
        uint32_t hi;
        void *v;
        hi = toku_cachetable_hash(f1, make_blocknum(i));
        r = toku_cachetable_maybe_get_and_pin(f1, make_blocknum(i), hi, PL_WRITE_EXPENSIVE, &v);
        assert(r == 0 && v == (void *)(long)i);
        r = toku_test_cachetable_unpin(f1, make_blocknum(i), hi, CACHETABLE_CLEAN, make_pair_attr(1));
        assert(r == 0);
        hi = toku_cachetable_hash(f2, make_blocknum(i));
        r = toku_cachetable_maybe_get_and_pin(f2, make_blocknum(i), hi, PL_WRITE_EXPENSIVE, &v);
        assert(r == 0 && v == (void *)(long)i);
        r = toku_test_cachetable_unpin(f2, make_blocknum(i), hi, CACHETABLE_CLEAN, make_pair_attr(1));
        assert(r == 0);
    }

    // def_flush 
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachefile_verify(f2);

    // verify keys exist in f2
    for (i=0; i<n; i++) {
        uint32_t hi;
        void *v;
        hi = toku_cachetable_hash(f2, make_blocknum(i));
        r = toku_cachetable_maybe_get_and_pin(f2, make_blocknum(i), hi, PL_WRITE_EXPENSIVE, &v);
        assert(r == 0);
        r = toku_test_cachetable_unpin(f2, make_blocknum(i), hi, CACHETABLE_CLEAN, make_pair_attr(1));
        assert(r == 0);
    }

    toku_cachefile_close(&f2, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    test_cachetable_def_flush(8);
    return 0;
}
