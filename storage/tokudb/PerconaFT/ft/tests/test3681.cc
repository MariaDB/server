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

// Test for #3681: iibench hangs.  The scenario is
//  * Thread 1 calls root_put_msg, get_and_pin_root, 1 holds read lock on the root.
//  * Thread 2 calls checkpoint, marks the root for checkpoint.
//  * Thread 2 calls end_checkpoint, tries to write lock the root, sets want_write, and blocks on the rwlock because there is a reader.
//  * Thread 1 calls apply_msg_to_in_memory_leaves, calls get_and_pin_if_in_memory, tries to get a read lock on the root node and blocks on the rwlock because there is a write request on the lock.


#include "cachetable/checkpoint.h"
#include "test.h"

CACHETABLE ct;
FT_HANDLE t;

static TOKUTXN const null_txn = 0;

volatile bool done = false;

static void setup (void) {
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    const char *fname = TOKU_TEST_FILENAME;
    unlink(fname);
    { int r = toku_open_ft_handle(fname, 1, &t, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);         assert(r==0); }
}


static void finish (void) {
    { int r = toku_close_ft_handle_nolsn(t, 0);                                                                       assert(r==0); };
    toku_cachetable_close(&ct);
}

static void *starta (void *n) {
    assert(n==NULL);
    for (int i=0; i<10000; i++) {
	DBT k,v;
	char ks[20], vs[20];
	snprintf(ks, sizeof(ks), "hello%03d", i);
	snprintf(vs, sizeof(vs), "there%03d", i);
	toku_ft_insert(t, toku_fill_dbt(&k, ks, strlen(ks)), toku_fill_dbt(&v, vs, strlen(vs)), null_txn);
	usleep(1);
    }
    done = true;
    return NULL;
}
static void *startb (void *n) {
    assert(n==NULL);
    int count=0;
    while (!done) {
        CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
        int r = toku_checkpoint(cp, NULL, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT); assert(r==0);
        count++;
    }
    printf("count=%d\n", count);
    return NULL;
}

static void test3681(void) {
    setup();
    toku_pthread_t a, b;
    {
        int r;
        r = toku_pthread_create(
            toku_uninstrumented, &a, nullptr, starta, nullptr);
        assert(r == 0);
    }
    {
        int r;
        r = toku_pthread_create(
            toku_uninstrumented, &b, nullptr, startb, nullptr);
        assert(r == 0);
    }
    {
        int r;
        void *v;
        r = toku_pthread_join(a, &v);
        assert(r == 0);
        assert(v == NULL);
    }
    {
        int r;
        void *v;
        r = toku_pthread_join(b, &v);
        assert(r == 0);
        assert(v == NULL);
    }
    finish();
}

int test_main (int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    test3681();
    return 0;
}

