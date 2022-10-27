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

static TOKUTXN const null_txn = 0;

static void test5 (void) {
    int r;
    FT_HANDLE t;
    int limit=100000;
    int *values;
    int i;
    CACHETABLE ct;
    const char *fname = TOKU_TEST_FILENAME;
    
    MALLOC_N(limit,values);
    for (i=0; i<limit; i++) values[i]=-1;
    unlink(fname);
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &t, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);   assert(r==0);
    for (i=0; i<limit/2; i++) {
	char key[100],val[100];
	int rk = random()%limit;
	int rv = random();
	if (i%1000==0 && verbose) { printf("w"); fflush(stdout); }
	values[rk] = rv;
	snprintf(key, 100, "key%d", rk);
	snprintf(val, 100, "val%d", rv);
	DBT k,v;
	toku_ft_insert(t, toku_fill_dbt(&k, key, 1+strlen(key)), toku_fill_dbt(&v, val, 1+strlen(val)), null_txn);
    }
    if (verbose) printf("\n");
    for (i=0; i<limit/2; i++) {
	int rk = random()%limit;
	if (values[rk]>=0) {
	    char key[100], valexpected[100];
	    DBT k;
	    if (i%1000==0 && verbose) { printf("r"); fflush(stdout); }
	    snprintf(key, 100, "key%d", rk);
	    snprintf(valexpected, 100, "val%d", values[rk]);
	    struct check_pair pair = {(uint32_t) (1+strlen(key)), key, (uint32_t) (1+strlen(valexpected)), valexpected, 0};
	    r = toku_ft_lookup(t, toku_fill_dbt(&k, key, 1+strlen(key)), lookup_checkf, &pair);
	    assert(r==0);
	    assert(pair.call_count==1);
	}
    }
    if (verbose) printf("\n");
    toku_free(values);
    r = toku_verify_ft(t);         assert(r==0);
    r = toku_close_ft_handle_nolsn(t, 0);       assert(r==0);
    toku_cachetable_close(&ct);
    
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);

    test5();
    
    if (verbose) printf("test ok\n");
    return 0;
}
