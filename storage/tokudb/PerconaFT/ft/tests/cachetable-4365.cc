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

CACHEFILE f1;

static void *pin_nonblocking(void *arg) {    
    void* v1;
    int r = toku_cachetable_get_and_pin_nonblocking(
        f1, 
        make_blocknum(1), 
        toku_cachetable_hash(f1, make_blocknum(1)), 
        &v1, 
        def_write_callback(NULL), def_fetch, def_pf_req_callback, def_pf_callback, 
        PL_WRITE_EXPENSIVE,
        NULL, 
        NULL
        );
    assert(r==TOKUDB_TRY_AGAIN);
    return arg;
}

static void *put_same_key(void *arg) {
    toku_cachetable_put(
        f1, 
        make_blocknum(1),
        toku_cachetable_hash(f1,make_blocknum(1)),
        NULL, 
        make_pair_attr(4),
        def_write_callback(NULL),
        put_callback_nop
        );
    return arg;
}

toku_pthread_t put_tid;

static void test_remove_key(CACHEKEY *UU(cachekey),
                            bool UU(for_checkpoint),
                            void *UU(extra)) {
    int r = toku_pthread_create(
        toku_uninstrumented, &put_tid, nullptr, put_same_key, nullptr);
    assert_zero(r);
}

static void
cachetable_test (void) {
  const int test_limit = 12;
  int r;
  CACHETABLE ct;
  toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
  const char *fname1 = TOKU_TEST_FILENAME;
  unlink(fname1);
  r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

  void* v1;
  r = toku_cachetable_get_and_pin(f1,
                                  make_blocknum(1),
                                  toku_cachetable_hash(f1, make_blocknum(1)),
                                  &v1,
                                  def_write_callback(nullptr),
                                  def_fetch,
                                  def_pf_req_callback,
                                  def_pf_callback,
                                  true,
                                  nullptr);
  toku_pthread_t pin_nonblocking_tid;
  r = toku_pthread_create(toku_uninstrumented,
                          &pin_nonblocking_tid,
                          nullptr,
                          pin_nonblocking,
                          nullptr);
  assert_zero(r);
  // sleep 3 seconds
  usleep(3 * 1024 * 1024);
  r = toku_test_cachetable_unpin_and_remove(f1, make_blocknum(1), test_remove_key, NULL);
  assert_zero(r);
  
  void *ret;
  r = toku_pthread_join(pin_nonblocking_tid, &ret); 
  assert_zero(r);
  r = toku_pthread_join(put_tid, &ret); 
  assert_zero(r);

  r = toku_test_cachetable_unpin(f1, make_blocknum(1), toku_cachetable_hash(f1, make_blocknum(1)), CACHETABLE_CLEAN, make_pair_attr(2));
  
  toku_cachetable_verify(ct);
  toku_cachefile_close(&f1, false, ZERO_LSN);
  toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
  default_parse_args(argc, argv);
  for (int i = 0; i < 20; i++) {
      cachetable_test();
  }
  return 0;
}
