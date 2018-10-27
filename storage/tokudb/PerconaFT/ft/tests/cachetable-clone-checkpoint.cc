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

static void 
clone_callback(void* UU(value_data), void** cloned_value_data, long* clone_size, PAIR_ATTR* new_attr, bool UU(for_checkpoint), void* UU(write_extraargs))
{
    *cloned_value_data = (void *)1;
    *clone_size = 8;
    new_attr->is_valid = false;
}

bool clone_flush_started;
bool clone_flush_completed;
CACHETABLE ct;

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
    bool is_clone
    ) 
{  
    if (is_clone) {
        clone_flush_started = true;
        usleep(4*1024*1024);
        clone_flush_completed = true;
    }
}

static void *run_end_checkpoint(void *arg) {
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    toku_cachetable_end_checkpoint(
        cp, 
        NULL, 
        NULL,
        NULL
        );
    return arg;
}

//
// this test verifies that a PAIR that undergoes a checkpoint on the checkpoint thread is still pinnable while being written out
//
static void
cachetable_test (void) {
    const int test_limit = 200;
    int r;
    ct = NULL;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    create_dummy_functions(f1);

    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = flush;
    wc.clone_callback = clone_callback;
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_DIRTY, make_pair_attr(8));
    assert_zero(r);
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    toku_cachetable_begin_checkpoint(cp, NULL);

    clone_flush_started = false;
    clone_flush_completed = false;
    toku_pthread_t checkpoint_tid;
    r = toku_pthread_create(toku_uninstrumented,
                            &checkpoint_tid,
                            nullptr,
                            run_end_checkpoint,
                            nullptr);
    assert_zero(r);

    usleep(1 * 1024 * 1024);

    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, true, NULL);
    assert_zero(r);
    assert(clone_flush_started && !clone_flush_completed);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8));
    assert_zero(r);
    
    void *ret;
    r = toku_pthread_join(checkpoint_tid, &ret); 
    assert_zero(r);
    assert(clone_flush_started && clone_flush_completed);

    toku_cachetable_verify(ct);
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
  default_parse_args(argc, argv);
  cachetable_test();
  return 0;
}
