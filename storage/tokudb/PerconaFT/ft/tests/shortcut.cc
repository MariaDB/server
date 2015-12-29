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
CACHETABLE ct;
FT_HANDLE ft;
FT_CURSOR cursor;

static int test_ft_cursor_keycompare(DB *db __attribute__((unused)), const DBT *a, const DBT *b) {
    return toku_keycompare(a->data, a->size, b->data, b->size);
}
int
test_main (int argc __attribute__((__unused__)), const char *argv[]  __attribute__((__unused__))) {
    int r;

    unlink(fname);

    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &ft, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, test_ft_cursor_keycompare);   assert(r==0);
    r = toku_ft_cursor(ft, &cursor, NULL, false, false);               assert(r==0);

    int i;
    for (i=0; i<1000; i++) {
	char string[100];
	snprintf(string, sizeof(string), "%04d", i);
	DBT key,val;
	toku_ft_insert(ft, toku_fill_dbt(&key, string, 5), toku_fill_dbt(&val, string, 5), 0);
    }

    {
	struct check_pair pair = {5, "0000", 5, "0000", 0};
	r = toku_ft_cursor_get(cursor, NULL, lookup_checkf, &pair, DB_NEXT);    assert(r==0); assert(pair.call_count==1);
    }
    {
	struct check_pair pair = {5, "0001", 5, "0001", 0};
	r = toku_ft_cursor_get(cursor, NULL, lookup_checkf, &pair, DB_NEXT);    assert(r==0); assert(pair.call_count==1);
    }

    // This will invalidate due to the root counter bumping, but the OMT itself will still be valid.
    {
	DBT key, val;
	toku_ft_insert(ft, toku_fill_dbt(&key, "d", 2), toku_fill_dbt(&val, "w", 2), 0);
    }

    {
	struct check_pair pair = {5, "0002", 5, "0002", 0};
	r = toku_ft_cursor_get(cursor, NULL, lookup_checkf, &pair, DB_NEXT);    assert(r==0); assert(pair.call_count==1);
    }

    toku_ft_cursor_close(cursor);
    r = toku_close_ft_handle_nolsn(ft, 0);                                                               assert(r==0);
    toku_cachetable_close(&ct);
    return 0;
}
