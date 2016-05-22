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

static const enum toku_compression_method compression_method = TOKU_DEFAULT_COMPRESSION_METHOD;

static TOKUTXN const null_txn = 0;

static void test3 (int nodesize, int basementnodesize, int count) {
    FT_HANDLE t;
    int r;
    struct timeval t0,t1;
    int i;
    CACHETABLE ct;
    
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    gettimeofday(&t0, 0);
    unlink(fname);
    r = toku_open_ft_handle(fname, 1, &t, nodesize, basementnodesize, compression_method, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);
    for (i=0; i<count; i++) {
	char key[100],val[100];
	DBT k,v;
	snprintf(key,100,"hello%d",i);
	snprintf(val,100,"there%d",i);
	toku_ft_insert(t, toku_fill_dbt(&k, key, 1+strlen(key)), toku_fill_dbt(&v, val, 1+strlen(val)), null_txn);
    }
    r = toku_verify_ft(t); assert(r==0);
    r = toku_close_ft_handle_nolsn(t, 0);        assert(r==0);
    toku_cachetable_close(&ct);
    
    gettimeofday(&t1, 0);
    {
	double diff = toku_tdiff(&t1, &t0);
	if (verbose) printf("serial insertions: blocksize=%d %d insertions in %.3f seconds, %.2f insertions/second\n", nodesize, count, diff, count/diff);
    }
}

static void ft_blackbox_test (void) {
    if (verbose) printf("test3 slow\n");

    test3(2048, 512, 1<<15);
    if (verbose) printf("test3 fast\n");

    //if (verbose) toku_pma_show_stats();

    test3(1<<15, 1<<12, 1024);
    if (verbose) printf("test3 fast\n");

    test3(1<<18, 1<<15, 1<<20);


//    test3(1<<19, 1<<16, 1<<20);

//    test3(1<<20, 1<<17, 1<<20);

//    test3(1<<20, 1<<17, 1<<21);

//    test3(1<<20, 1<<17, 1<<22);

}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);

    ft_blackbox_test();
    
    if (verbose) printf("test ok\n");
    return 0;
}
