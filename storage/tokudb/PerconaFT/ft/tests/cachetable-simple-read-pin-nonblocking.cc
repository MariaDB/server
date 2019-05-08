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
bool fetch_called;
CACHEFILE f1;

static int
sleep_fetch (CACHEFILE f        __attribute__((__unused__)),
       PAIR UU(p),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       uint32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       void **dd     __attribute__((__unused__)),
       PAIR_ATTR *sizep        __attribute__((__unused__)),
       int  *dirtyp,
       void *extraargs    __attribute__((__unused__))
       ) {
    sleep(2);
    *dirtyp = 0;
    *value = NULL;
    *sizep = make_pair_attr(8);
    fetch_called = true;
    return 0;
}

static bool sleep_pf_req_callback(void* UU(ftnode_pv), void* UU(read_extraargs)) {
  return true;
}

static int sleep_pf_callback(void* UU(ftnode_pv), void* UU(disk_data), void* UU(read_extraargs), int UU(fd), PAIR_ATTR* sizep) {
   sleep(2);
  *sizep = make_pair_attr(8);
  pf_called = true;
  return 0;
}

static void *run_expensive_pf(void *arg) {
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    int r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, sleep_fetch, sleep_pf_req_callback, sleep_pf_callback, PL_READ, NULL, NULL);
    assert(r == TOKUDB_TRY_AGAIN);
    assert(pf_called);
    return arg;
}

static void *run_expensive_fetch(void *arg) {
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    int r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, sleep_fetch, sleep_pf_req_callback, sleep_pf_callback, PL_READ, NULL, NULL);
    assert(fetch_called);
    assert(r == TOKUDB_TRY_AGAIN);
    return arg;
}


static void
run_test (void) {
    const int test_limit = 20;
    int r;
    void *ret;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);
    
    void* v1;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);

    toku_pthread_t fetch_tid;
    fetch_called = false;
    r = toku_pthread_create(
        toku_uninstrumented, &fetch_tid, nullptr, run_expensive_fetch, nullptr);
    sleep(1);
    r = toku_cachetable_get_and_pin(f1,
                                    make_blocknum(1),
                                    1,
                                    &v1,
                                    wc,
                                    sleep_fetch,
                                    def_pf_req_callback,
                                    def_pf_callback,
                                    false,
                                    NULL);
    assert_zero(r);
    assert(fetch_called);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8));
    assert(r==0);
    r = toku_pthread_join(fetch_tid, &ret); 
    assert_zero(r);

    // call with may_modify_node = false twice, make sure we can get it
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, sleep_fetch, def_pf_req_callback, def_pf_callback, PL_READ, NULL, NULL);
    assert_zero(r);
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, wc, sleep_fetch, def_pf_req_callback, def_pf_callback, PL_READ, NULL, NULL);
    assert_zero(r);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8));
    assert(r==0);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8));
    assert(r==0);

    toku_pthread_t pf_tid;
    pf_called = false;
    r = toku_pthread_create(
        toku_uninstrumented, &pf_tid, nullptr, run_expensive_pf, nullptr);
    sleep(1);
    r = toku_cachetable_get_and_pin(f1,
                                    make_blocknum(1),
                                    1,
                                    &v1,
                                    wc,
                                    sleep_fetch,
                                    def_pf_req_callback,
                                    def_pf_callback,
                                    false,
                                    NULL);
    assert_zero(r);
    assert(pf_called);
    r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(8));
    assert(r==0);
    
    r = toku_pthread_join(pf_tid, &ret); 
    assert_zero(r);

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
