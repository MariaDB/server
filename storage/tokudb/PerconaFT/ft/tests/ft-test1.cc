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

static void test1 (void) {
    FT_HANDLE t;
    int r;
    CACHETABLE ct;
    const char *fname = TOKU_TEST_FILENAME;
    DBT k,v;
    
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    unlink(fname);
    r = toku_open_ft_handle(fname, 1, &t, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);
    toku_ft_insert(t, toku_fill_dbt(&k, "hello", 6), toku_fill_dbt(&v, "there", 6), null_txn);
    assert(r==0);
    {
	struct check_pair pair = {6, "hello", 6, "there", 0};
	r = toku_ft_lookup(t, toku_fill_dbt(&k, "hello", 6), lookup_checkf, &pair);
	assert(r==0);
	assert(pair.call_count==1);
    }
    r = toku_close_ft_handle_nolsn(t, 0);              assert(r==0);
    toku_cachetable_close(&ct);
    
    if (verbose) printf("test1 ok\n");
}
int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);
     if (verbose) printf("test1\n");
    test1();
    
    if (verbose) printf("test1 ok\n");
    return 0;
}
