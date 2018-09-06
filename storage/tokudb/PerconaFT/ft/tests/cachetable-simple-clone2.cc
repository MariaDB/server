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

bool clone_called;
bool check_flush;
bool flush_expected;
bool flush_called;

static void 
clone_callback(void* UU(value_data), void** cloned_value_data, long* clone_size, PAIR_ATTR* new_attr, bool UU(for_checkpoint), void* UU(write_extraargs))
{
    *cloned_value_data = (void *)1;
    new_attr->is_valid = false;
    clone_called = true;
    *clone_size = 8;
}

static void
flush (
    CACHEFILE f __attribute__((__unused__)),
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
    ) 
{  
    if (w && check_flush) {
        assert(flush_expected);
        flush_called = true;
    }
}

//
// test the following things for simple cloning:
//  - verifies that after the checkpoint ends, the PAIR is properly
//     dirty or clean based on the second unpin
//
static void
test_clean (enum cachetable_dirty dirty, bool cloneable) {
    const int test_limit = 200;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    create_dummy_functions(f1);
    check_flush = false;
    
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.clone_callback = cloneable ? clone_callback : NULL;
    wc.flush_callback = flush;
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_DIRTY, make_pair_attr(8));
    
    // begin checkpoint, since pair is clean, we should not 
    // have the clone called
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    toku_cachetable_begin_checkpoint(cp, NULL);
    assert_zero(r);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    
    // at this point, there should be no more dirty writes
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, dirty, make_pair_attr(8));
    usleep(2*1024*1024);
    toku_cachetable_end_checkpoint(
        cp, 
        NULL, 
        NULL,
        NULL
    );
    
    check_flush = true;
    flush_expected = (dirty == CACHETABLE_DIRTY) ? true : false;
    flush_called = false;
    
    toku_cachetable_verify(ct);
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
    if (flush_expected) assert(flush_called);
}

int
test_main(int argc, const char *argv[]) {
  default_parse_args(argc, argv);
  test_clean(CACHETABLE_CLEAN, true);
  test_clean(CACHETABLE_DIRTY, true);
  test_clean(CACHETABLE_CLEAN, false);
  test_clean(CACHETABLE_DIRTY, false);
  return 0;
}
