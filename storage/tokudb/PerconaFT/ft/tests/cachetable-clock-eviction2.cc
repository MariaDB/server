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

bool flush_may_occur;
long expected_bytes_to_free;

static void
flush (CACHEFILE f __attribute__((__unused__)),
       int UU(fd),
       CACHEKEY k  __attribute__((__unused__)),
       void *v,
       void** UU(dd),
       void *e     __attribute__((__unused__)),
       PAIR_ATTR s      __attribute__((__unused__)),
       PAIR_ATTR* new_size      __attribute__((__unused__)),
       bool w      __attribute__((__unused__)),
       bool keep,
       bool c      __attribute__((__unused__)),
        bool UU(is_clone)
       ) {
    assert(flush_may_occur);
    if (!keep) {
        int* CAST_FROM_VOIDP(foo, v);
        assert(*foo == 3);
        toku_free(v);
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
    int* XMALLOC(foo);
    *value = foo;
    *sizep = make_pair_attr(4);
    *foo = 4;
    return 0;
}

static void
other_flush (CACHEFILE f __attribute__((__unused__)),
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
}

static int 
pe_callback (
    void *ftnode_pv, 
    PAIR_ATTR UU(bytes_to_free), 
    void* extraargs __attribute__((__unused__)),
    void (*finalize)(PAIR_ATTR bytes_freed, void *extra),
    void *finalize_extra
    ) 
{
    expected_bytes_to_free--;
    int* CAST_FROM_VOIDP(foo, ftnode_pv);
    int blah = *foo;
    *foo = blah-1;
    finalize(make_pair_attr(bytes_to_free.size-1), finalize_extra);
    return 0;
}

static int 
other_pe_callback (
    void *ftnode_pv __attribute__((__unused__)), 
    PAIR_ATTR bytes_to_free __attribute__((__unused__)), 
    void* extraargs __attribute__((__unused__)),
    void (*finalize)(PAIR_ATTR bytes_freed, void *extra),
    void *finalize_extra
    ) 
{
    finalize(bytes_to_free, finalize_extra);
    return 0;
}

static void
cachetable_test (void) {
    const int test_limit = 16;
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
    for (int i = 0; i < 100000; i++) {
      CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
      wc.flush_callback = flush;
      wc.pe_callback = pe_callback;
      r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
      r = toku_test_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, make_pair_attr(4));
    }
    for (int i = 0; i < 8; i++) {
      CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
      wc.flush_callback = flush;
      wc.pe_callback = pe_callback;
      r = toku_cachetable_get_and_pin(f1, make_blocknum(2), 2, &v2, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
      r = toku_test_cachetable_unpin(f1, make_blocknum(2), 2, CACHETABLE_CLEAN, make_pair_attr(4));
    }
    for (int i = 0; i < 4; i++) {
      CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
      wc.flush_callback = flush;
      wc.pe_callback = pe_callback;
      r = toku_cachetable_get_and_pin(f1, make_blocknum(3), 3, &v2, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
      r = toku_test_cachetable_unpin(f1, make_blocknum(3), 3, CACHETABLE_CLEAN, make_pair_attr(4));
    }
    for (int i = 0; i < 2; i++) {
      CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
      wc.flush_callback = flush;
      wc.pe_callback = pe_callback;
      r = toku_cachetable_get_and_pin(f1, make_blocknum(4), 4, &v2, wc, fetch, def_pf_req_callback, def_pf_callback, true, NULL);
      r = toku_test_cachetable_unpin(f1, make_blocknum(4), 4, CACHETABLE_CLEAN, make_pair_attr(4));
    }
    flush_may_occur = false;
    expected_bytes_to_free = 4;
    CACHETABLE_WRITE_CALLBACK wc = def_write_callback(NULL);
    wc.flush_callback = other_flush;
    wc.pe_callback = other_pe_callback;
    toku_cachetable_put(f1, make_blocknum(5), 5, NULL, make_pair_attr(4), wc, put_callback_nop);
    ct->ev.signal_eviction_thread();
    usleep(1*1024*1024);
    flush_may_occur = true;
    r = toku_test_cachetable_unpin(f1, make_blocknum(5), 5, CACHETABLE_CLEAN, make_pair_attr(4));
    ct->ev.signal_eviction_thread();
    usleep(1*1024*1024);
    assert(expected_bytes_to_free == 0);


    toku_cachefile_close(&f1, false, ZERO_LSN);
    toku_cachetable_close(&ct);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    cachetable_test();
    return 0;
}
