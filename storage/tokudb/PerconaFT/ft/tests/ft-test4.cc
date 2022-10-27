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
#include <toku_time.h>

static const char *fname = TOKU_TEST_FILENAME;

static TOKUTXN const null_txn = 0;

static void test4 (int nodesize, int count) {
    FT_HANDLE t;
    int r;
    struct timeval t0,t1;
    int i;
    CACHETABLE ct;
    gettimeofday(&t0, 0);
    unlink(fname);
    
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &t, nodesize, nodesize / 8, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun); assert(r==0);
    for (i=0; i<count; i++) {
	char key[100],val[100];
	int rv = random();
	DBT k,v;
	snprintf(key,100,"hello%d",rv);
	snprintf(val,100,"there%d",i);
	toku_ft_insert(t, toku_fill_dbt(&k, key, 1+strlen(key)), toku_fill_dbt(&v, val, 1+strlen(val)), null_txn);
    }
    r = toku_verify_ft(t); assert(r==0);
    r = toku_close_ft_handle_nolsn(t, 0);        assert(r==0);
    toku_cachetable_close(&ct);
    
    gettimeofday(&t1, 0);
    {
	double diff = toku_tdiff(&t1, &t0);
	if (verbose) printf("random insertions: blocksize=%d %d insertions in %.3f seconds, %.2f insertions/second\n", nodesize, count, diff, count/diff);
    }
}

static void ft_blackbox_test (void) {
    test4(2048, 1<<14);

    if (0) {

	if (verbose) printf("test4 slow\n");
	test4(2048, 1<<15);

	//if (verbose) toku_pma_show_stats();

	test4(1<<15, 1024);

	test4(1<<18, 1<<20);

	// Once upon a time srandom(8) caused this test to fail.
	srandom(8); test4(2048, 1<<15);
    }
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);

    ft_blackbox_test();
    
    if (verbose) printf("test ok\n");
    return 0;
}
