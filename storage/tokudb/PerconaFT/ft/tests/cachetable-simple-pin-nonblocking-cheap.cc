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
  if (w) {
    assert(c);
    assert(keep);
  }
}

static void kibbutz_work(void *fe_v)
{
    CACHEFILE CAST_FROM_VOIDP(f1, fe_v);
    sleep(2);
    int r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8));
    assert(r==0);
    remove_background_job_from_cf(f1);    
}

static void
unlock_dummy (void* UU(v)) {
}

static void reset_unlockers(UNLOCKERS unlockers) {
    unlockers->locked = true;
}

static void
run_case_that_should_succeed(CACHEFILE f1, pair_lock_type first_lock, pair_lock_type second_lock) {
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = flush;
    struct unlockers unlockers = {true, unlock_dummy, NULL, NULL};
    int r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, first_lock, NULL, NULL);
    assert(r==0);
    cachefile_kibbutz_enq(f1, kibbutz_work, f1);
    reset_unlockers(&unlockers);
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, second_lock, NULL, &unlockers);
    assert(r==0); assert(unlockers.locked);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8)); assert(r==0);
}

static void
run_case_that_should_fail(CACHEFILE f1, pair_lock_type first_lock, pair_lock_type second_lock) {
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = flush;
    struct unlockers unlockers = {true, unlock_dummy, NULL, NULL};
    int r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, first_lock, NULL, NULL);
    assert(r==0);
    cachefile_kibbutz_enq(f1, kibbutz_work, f1);
    reset_unlockers(&unlockers);
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, second_lock, NULL, &unlockers);
    assert(r == TOKUDB_TRY_AGAIN); assert(!unlockers.locked);
}


static void
run_test (void) {
    // sometimes the cachetable evictor runs during the test.  this sometimes causes cachetable pair locking contention,
    // which results with a TOKUDB_TRY_AGAIN error occurring.  unfortunately, the test does not expect this and fails.
    // set cachetable size limit to a value big enough so that the cachetable evictor is not triggered during the test.
    const int test_limit = 100;

    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = flush;
    //
    // test that if we are getting a PAIR for the first time that TOKUDB_TRY_AGAIN is returned
    // because the PAIR was not in the cachetable.
    //
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, PL_WRITE_EXPENSIVE, NULL, NULL);
    assert(r==TOKUDB_TRY_AGAIN);


    run_case_that_should_succeed(f1, PL_READ, PL_WRITE_CHEAP);
    run_case_that_should_succeed(f1, PL_READ, PL_WRITE_EXPENSIVE);

    run_case_that_should_succeed(f1, PL_WRITE_CHEAP, PL_READ);
    run_case_that_should_succeed(f1, PL_WRITE_CHEAP, PL_WRITE_CHEAP);
    run_case_that_should_succeed(f1, PL_WRITE_CHEAP, PL_WRITE_EXPENSIVE);

    run_case_that_should_fail(f1, PL_WRITE_EXPENSIVE, PL_READ);
    run_case_that_should_fail(f1, PL_WRITE_EXPENSIVE, PL_WRITE_CHEAP);
    run_case_that_should_fail(f1, PL_WRITE_EXPENSIVE, PL_WRITE_EXPENSIVE);
    
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
