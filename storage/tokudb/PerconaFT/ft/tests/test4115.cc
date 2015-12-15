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

// Test toku_ft_handle_stat64 to make sure it works even if the comparison function won't allow an arbitrary prefix of the key to work.


#include "test.h"

#include <unistd.h>

static TOKUTXN const null_txn = 0;

const char *fname = TOKU_TEST_FILENAME;
CACHETABLE ct;
FT_HANDLE t;
int keysize = 9;

static int dont_allow_prefix (DB *db __attribute__((__unused__)), const DBT *a, const DBT *b) {
    assert(a->size==9 && b->size==9);
    return toku_keycompare(a->data, a->size, b->data, b->size);
}

static void close_ft_and_ct (void) {
    int r;
    r = toku_close_ft_handle_nolsn(t, 0);          assert(r==0);
    toku_cachetable_close(&ct);
}

static void open_ft_and_ct (bool unlink_old) {
    int r;
    if (unlink_old) unlink(fname);
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &t, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);   assert(r==0);
    toku_ft_set_bt_compare(t, dont_allow_prefix);
}

static void test_4115 (void) {
    uint64_t limit=30000;
    open_ft_and_ct(true);
    for (uint64_t i=0; i<limit; i++) {
	char key[100],val[100];
	snprintf(key, 100, "%08llu", (unsigned long long)2*i+1);
	snprintf(val, 100, "%08llu", (unsigned long long)2*i+1);
	DBT k,v;
	toku_ft_insert(t, toku_fill_dbt(&k, key, 1+strlen(key)), toku_fill_dbt(&v,val, 1+strlen(val)), null_txn);
    }
    struct ftstat64_s s;
    toku_ft_handle_stat64(t, NULL, &s);
    assert(s.nkeys>0);
    assert(s.dsize>0);
    close_ft_and_ct();
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);

    test_4115();

    if (verbose) printf("test ok\n");
    return 0;
}

