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

bool pf_called;
static bool true_pf_req_callback(void* UU(ftnode_pv), void* UU(read_extraargs)) {
  if (pf_called) return false;
  return true;
}

static int true_pf_callback(void* UU(ftnode_pv), void* UU(dd), void* UU(read_extraargs), int UU(fd), PAIR_ATTR* sizep) {
    *sizep = make_pair_attr(9);
    pf_called = true;
    return 0;
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
run_test (pair_lock_type lock_type) {
    const int test_limit = 12;
    struct unlockers unlockers = {true, unlock_dummy, NULL, NULL};
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    r = toku_cachetable_get_and_pin_with_dep_pairs(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, lock_type, NULL, 0, NULL, NULL);
    cachefile_kibbutz_enq(f1, kibbutz_work, f1);
    reset_unlockers(&unlockers);
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, PL_WRITE_EXPENSIVE, NULL, &unlockers);
    // to fix #5393, we changed behavior on full fetch where if we 
    // requested a PL_WRITE_CHEAP, and had to grab a PL_WRITE_EXPENSIVE for
    // a full fetch, we keep it as a PL_WRITE_EXPENSIVE because downgrading back
    // was too big a pain.
    if (lock_type == PL_WRITE_EXPENSIVE || lock_type == PL_WRITE_CHEAP) {
        assert(r == TOKUDB_TRY_AGAIN); assert(!unlockers.locked);
    }
    else {
        assert(r == 0); assert(unlockers.locked);        
        r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8)); assert(r==0);
    }

    // now do the same test with a partial fetch required
    pf_called = false;
    r = toku_cachetable_get_and_pin_with_dep_pairs(f1, make_blocknum(1), 1, &v1, wc, def_fetch, true_pf_req_callback, true_pf_callback, lock_type, NULL, 0, NULL, NULL);
    assert(pf_called);
    cachefile_kibbutz_enq(f1, kibbutz_work, f1);
    reset_unlockers(&unlockers);
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, def_fetch, def_pf_req_callback, def_pf_callback, PL_WRITE_EXPENSIVE, NULL, &unlockers);
    if (lock_type == PL_WRITE_EXPENSIVE) {
        assert(r == TOKUDB_TRY_AGAIN); assert(!unlockers.locked);
    }
    else {
        assert(r == 0); assert(unlockers.locked);        
        r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8)); assert(r==0);
    }
    
    toku_cachetable_verify(ct);
    toku_cachefile_close(&f1, false, ZERO_LSN); 
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
  default_parse_args(argc, argv);
  run_test(PL_READ);
  run_test(PL_WRITE_CHEAP);
  run_test(PL_WRITE_EXPENSIVE);
  return 0;
}
