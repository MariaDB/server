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


// create a ft and put n rows into it
// write the ft to the file
// verify the rows in the ft
static void test_sub_block(int n) {
    if (verbose) printf("%s:%d %d\n", __FUNCTION__, __LINE__, n);

    const char *fname = TOKU_TEST_FILENAME;
    const int nodesize = 4*1024*1024;
    const int basementnodesize = 128*1024;
    const enum toku_compression_method compression_method = TOKU_DEFAULT_COMPRESSION_METHOD;

    TOKUTXN const null_txn = 0;

    int error;
    CACHETABLE ct;
    FT_HANDLE ft;
    int i;

    unlink(fname);

    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);

    error = toku_open_ft_handle(fname, true, &ft, nodesize, basementnodesize, compression_method, ct, null_txn, toku_builtin_compare_fun);
    assert(error == 0);

    // insert keys 0, 1, 2, .. (n-1)
    for (i=0; i<n; i++) {
        int k = toku_htonl(i);
        int v = i;
	DBT key, val;
        toku_fill_dbt(&key, &k, sizeof k);
        toku_fill_dbt(&val, &v, sizeof v);
        toku_ft_insert(ft, &key, &val, 0);
        assert(error == 0);
    }

    // write to the file
    error = toku_close_ft_handle_nolsn(ft, 0);
    assert(error == 0);

    // verify the ft by walking a cursor through the rows
    error = toku_open_ft_handle(fname, false, &ft, nodesize, basementnodesize, compression_method, ct, null_txn, toku_builtin_compare_fun);
    assert(error == 0);

    FT_CURSOR cursor;
    error = toku_ft_cursor(ft, &cursor, NULL, false, false);
    assert(error == 0);

    for (i=0; ; i++) {
        int k = htonl(i);
        int v = i;
	struct check_pair pair = {sizeof k, &k, sizeof v, &v, 0};	
        error = toku_ft_cursor_get(cursor, NULL, lookup_checkf, &pair, DB_NEXT);
        if (error != 0) {
	    assert(pair.call_count==0);
            break;
	}
	assert(pair.call_count==1);
    }
    assert(i == n);

    toku_ft_cursor_close(cursor);

    error = toku_close_ft_handle_nolsn(ft, 0);
    assert(error == 0);

    toku_cachetable_close(&ct);
}

int test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);

    const int meg = 1024*1024;
    const int row = 32;
    const int rowspermeg = meg/row;

    test_sub_block(1);
    test_sub_block(rowspermeg-1);
    int i;
    for (i=1; i<8; i++)
        test_sub_block(rowspermeg*i);
    
    if (verbose) printf("test ok\n");
    return 0;
}
