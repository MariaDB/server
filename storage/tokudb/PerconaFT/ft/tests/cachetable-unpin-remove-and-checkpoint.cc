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

//
// This test exposed a bug (#3970) caught only by Valgrind.
// freed memory was being accessed by toku_test_cachetable_unpin_and_remove
//
static void *run_end_chkpt(void *arg) {
    assert(arg == NULL);
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    toku_cachetable_end_checkpoint(
        cp, 
        NULL, 
        NULL,
        NULL
        );
    return arg;
}

static void
run_test (void) {
    const int test_limit = 12;
    int r;
    ct = NULL;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    create_dummy_functions(f1);

    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    void* v1;
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    toku_test_cachetable_unpin(
        f1, 
        make_blocknum(1), 
        toku_cachetable_hash(f1, make_blocknum(1)),
        CACHETABLE_DIRTY,
        make_pair_attr(8)
        );

    // now this should mark the pair for checkpoint
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    toku_cachetable_begin_checkpoint(cp, NULL);
    r = toku_cachetable_get_and_pin(f1,
                                    make_blocknum(1),
                                    toku_cachetable_hash(f1, make_blocknum(1)),
                                    &v1,
                                    wc,
                                    def_fetch,
                                    def_pf_req_callback,
                                    def_pf_callback,
                                    true,
                                    NULL);

    toku_pthread_t mytid;
    r = toku_pthread_create(
        toku_uninstrumented, &mytid, nullptr, run_end_chkpt, nullptr);
    assert(r == 0);

    // give checkpoint thread a chance to start waiting on lock
    sleep(1);
    r = toku_test_cachetable_unpin_and_remove(f1, make_blocknum(1), NULL, NULL);
    assert(r==0);

    void* ret;
    r = toku_pthread_join(mytid, &ret);
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
