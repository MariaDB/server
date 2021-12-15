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

int num_entries;
bool flush_may_occur;
int expected_flushed_key;
bool check_flush;

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
    /* Do nothing */
    if (check_flush && !keep) {
        if (verbose) { printf("FLUSH: %d write_me %d expected %d\n", (int)k.b, w, expected_flushed_key); }
        assert(flush_may_occur);
        assert(!w);
        assert(expected_flushed_key == (int)k.b);
        expected_flushed_key--;
    }
}

static int
fetch (CACHEFILE f        __attribute__((__unused__)),
       PAIR UU(p),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       uint32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       void** UU(dd),
       PAIR_ATTR *sizep        __attribute__((__unused__)),
       int  *dirtyp,
       void *extraargs    __attribute__((__unused__))
       ) {
    *dirtyp = 0;
    *value = NULL;
    *sizep = make_pair_attr(1);
    return 0;
}

static void
cachetable_test (void) {
    const int test_limit = 4;
    num_entries = 0;
    int r;
    CACHETABLE ct;
    toku_cachetable_create(&ct, test_limit, ZERO_LSN, nullptr);
    const char *fname1 = TOKU_TEST_FILENAME;
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    void* v1;
    void* v2;
    flush_may_occur = false;
    check_flush = true;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = flush;
    for (int i = 0; i < 100000; i++) {
      r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
        r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(1));
    }
    for (int i = 0; i < 8; i++) {
      r = toku_cachetable_get_and_pin(f1, make_blocknum(2), 2, &v2, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
        r = toku_test_cachetable_unpin(f1, make_blocknum(2), 2, CACHETABLE_CLEAN, make_pair_attr(1));
    }
    for (int i = 0; i < 4; i++) {
      r = toku_cachetable_get_and_pin(f1, make_blocknum(3), 3, &v2, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
        r = toku_test_cachetable_unpin(f1, make_blocknum(3), 3, CACHETABLE_CLEAN, make_pair_attr(1));
    }
    for (int i = 0; i < 2; i++) {
      r = toku_cachetable_get_and_pin(f1, make_blocknum(4), 4, &v2, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
        r = toku_test_cachetable_unpin(f1, make_blocknum(4), 4, CACHETABLE_CLEAN, make_pair_attr(1));
    }
    flush_may_occur = true;
    expected_flushed_key = 4;
    toku_cachetable_put(f1, make_blocknum(5), 5, NULL, make_pair_attr(1), wc, put_callback_nop);
    ct->ev.signal_eviction_thread();
    usleep(1*1024*1024);
    
    flush_may_occur = true;
    r = toku_test_cachetable_unpin(f1, make_blocknum(5), 5, CACHETABLE_CLEAN, make_pair_attr(2));
    ct->ev.signal_eviction_thread();
    usleep(1*1024*1024);

    check_flush = false;
    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    cachetable_test();
    return 0;
}
