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

static const char *fname = TOKU_TEST_FILENAME;

static TOKUTXN const null_txn = 0;

static int
save_data (uint32_t UU(keylen), const void *UU(key), uint32_t vallen, const void *val, void *v, bool lock_only) {
    if (lock_only) return 0;
    assert(key!=NULL);
    void **CAST_FROM_VOIDP(vp, v);
    *vp = toku_memdup(val, vallen);
    return 0;
}


// Verify that different cursors return different data items when a DBT is initialized to all zeros (no flags)
// Note: The ft test used to implement DBTs with per-cursor allocated space, but there isn't any such thing any more
// so this test is a little bit obsolete.
static void test_multiple_ft_cursor_dbts(int n) {
    if (verbose) printf("test_multiple_ft_cursors:%d\n", n);

    int r;
    CACHETABLE ct;
    FT_HANDLE ft;
    FT_CURSOR cursors[n];

    unlink(fname);

    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);

    r = toku_open_ft_handle(fname, 1, &ft, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    int i;
    for (i=0; i<n; i++) {
	DBT kbt,vbt;
	char key[10],val[10];
	snprintf(key, sizeof key, "k%04d", i);
	snprintf(val, sizeof val, "v%04d", i);
	toku_ft_insert(ft,
                       toku_fill_dbt(&kbt, key, 1+strlen(key)),
                       toku_fill_dbt(&vbt, val, 1+strlen(val)),
                       0);
    }

    for (i=0; i<n; i++) {
        r = toku_ft_cursor(ft, &cursors[i], NULL, false, false);
        assert(r == 0);
    }

    void *ptrs[n];
    for (i=0; i<n; i++) {
	DBT kbt;
	char key[10];
	snprintf(key, sizeof key, "k%04d", i);
	r = toku_ft_cursor_get(cursors[i],
				toku_fill_dbt(&kbt, key, 1+strlen(key)),
				save_data,
				&ptrs[i],
				DB_SET);
	assert(r == 0);
    }

    for (i=0; i<n; i++) {
	int j;
	for (j=i+1; j<n; j++) {
	    assert(strcmp((char*)ptrs[i],(char*)ptrs[j])!=0);
	}
    }

    for (i=0; i<n; i++) {
        toku_ft_cursor_close(cursors[i]);
        assert(r == 0);
	toku_free(ptrs[i]);
    }

    r = toku_close_ft_handle_nolsn(ft, 0);
    assert(r==0);

    toku_cachetable_close(&ct);
}

static void test_ft_cursor(void) {
    test_multiple_ft_cursor_dbts(1);
    test_multiple_ft_cursor_dbts(2);
    test_multiple_ft_cursor_dbts(3);
}


int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);
    test_ft_cursor();
    if (verbose) printf("test ok\n");
    return 0;
}
