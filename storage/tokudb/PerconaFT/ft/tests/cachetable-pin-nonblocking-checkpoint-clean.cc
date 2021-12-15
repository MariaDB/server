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
#include "cachetable-test.h"

CACHETABLE ct;

CACHEFILE f1;

static void
run_test (void) {
    const int test_limit = 20;
    int r;
    ct = NULL;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    f1 = NULL;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    create_dummy_functions(f1);

    void* v1;
    void* v2;
    
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, def_write_callback(NULL), def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8)); assert(r==0);

    for (int i = 0; i < 20; i++) {
        r = toku_cachetable_get_and_pin(f1, make_blocknum(2), 2, &v2, def_write_callback(NULL), def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
        r = toku_test_cachetable_unpin(f1, make_blocknum(2), 2, CACHETABLE_CLEAN, make_pair_attr(8)); assert(r==0);
    }

    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v2, def_write_callback(NULL), def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    toku_cachetable_begin_checkpoint(cp, NULL);
    // mark nodes as pending a checkpoint, so that get_and_pin_nonblocking on block 1 will return TOKUDB_TRY_AGAIN
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8)); assert(r==0);

    r = toku_cachetable_get_and_pin_nonblocking(
        f1,
        make_blocknum(1),
        1,
        &v1,
        def_write_callback(NULL),
        def_fetch,
        def_pf_req_callback,
        def_pf_callback,
        PL_WRITE_EXPENSIVE,
        NULL,
        NULL
        );
    assert(r==0);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8)); assert(r==0);
    
    toku_cachetable_end_checkpoint(
        cp, 
        NULL, 
        NULL,
        NULL
        );
    assert(r==0);
    
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
