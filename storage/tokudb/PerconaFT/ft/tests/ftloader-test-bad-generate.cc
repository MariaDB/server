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

// The purpose of this test is force errors returned from the generate function 

#define DONT_DEPRECATE_MALLOC
#define DONT_DEPRECATE_WRITES
#include "test.h"
#include "loader/loader.h"
#include "loader/loader-internal.h"
#include "ftloader-error-injector.h"
#include "memory.h"
#include <portability/toku_path.h>


static int generate(DB *dest_db, DB *src_db, DBT_ARRAY *dest_keys, DBT_ARRAY *dest_vals, const DBT *src_key, const DBT *src_val) {
    if (verbose) printf("%s %p %p %p %p %p %p\n", __FUNCTION__, dest_db, src_db, dest_keys, dest_vals, src_key, src_val);
    toku_dbt_array_resize(dest_keys, 1);
    toku_dbt_array_resize(dest_vals, 1);
    DBT *dest_key = &dest_keys->dbts[0];
    DBT *dest_val = &dest_vals->dbts[0];

    assert(dest_db == NULL); assert(src_db == NULL);

    int result;
    if (event_count_trigger == event_add_and_fetch()) {
        event_hit();
        result = EINVAL;
    } else {
        copy_dbt(dest_key, src_key);
        copy_dbt(dest_val, src_val);
        result = 0;
    }

    if (verbose) printf("%s %d\n", __FUNCTION__, result);
    return result;
}

static int qsort_compare_ints (const void *a, const void *b) {
    int avalue = *(int*)a;
    int bvalue = *(int*)b;
    if (avalue<bvalue) return -1;
    if (avalue>bvalue) return +1;
    return 0;
}

static int compare_int(DB *desc, const DBT *akey, const DBT *bkey) {
    assert(desc == NULL);
    assert(akey->size == sizeof (int));
    assert(bkey->size == sizeof (int));
    return qsort_compare_ints(akey->data, bkey->data);
}

static void populate_rowset(struct rowset *rowset, int seq, int nrows) {
    for (int i = 0; i < nrows; i++) {
        int k = seq * nrows + i;
        int v = seq * nrows + i;
        DBT key;
        toku_fill_dbt(&key, &k, sizeof k);
        DBT val;
        toku_fill_dbt(&val, &v, sizeof v);
        add_row(rowset, &key, &val);
    }
}

static void test_extractor(int nrows, int nrowsets, bool expect_fail) {
    if (verbose) printf("%s %d %d\n", __FUNCTION__, nrows, nrowsets);

    int r;

    // open the ft_loader. this runs the extractor.
    const int N = 1;
    FT_HANDLE fts[N];
    DB* dbs[N];
    const char *fnames[N];
    ft_compare_func compares[N];
    for (int i = 0; i < N; i++) {
        fts[i] = NULL;
        dbs[i] = NULL;
        fnames[i] = "";
        compares[i] = compare_int;
    }

    FTLOADER loader;
    r = toku_ft_loader_open(&loader, NULL, generate, NULL, N, fts, dbs, fnames, compares, "tempXXXXXX", ZERO_LSN, nullptr, true, 0, false, true);
    assert(r == 0);

    struct rowset *rowset[nrowsets];
    for (int i = 0 ; i < nrowsets; i++) {
        rowset[i] = (struct rowset *) toku_malloc(sizeof (struct rowset));
        assert(rowset[i]);
        init_rowset(rowset[i], toku_ft_loader_get_rowset_budget_for_testing());
        populate_rowset(rowset[i], i, nrows);
    }

    // feed rowsets to the extractor
    for (int i = 0; i < nrowsets; i++) {
        r = toku_queue_enq(loader->primary_rowset_queue, rowset[i], 1, NULL);
        assert(r == 0);
    }

    r = toku_ft_loader_finish_extractor(loader);
    assert(r == 0);
    
    int loader_error;
    r = toku_ft_loader_get_error(loader, &loader_error);
    assert(r == 0);

    assert(expect_fail ? loader_error != 0 : loader_error == 0);

    // abort the ft_loader.  this ends the test
    r = toku_ft_loader_abort(loader, true);
    assert(r == 0);
}

static int nrows = 1;
static int nrowsets = 2;

static int usage(const char *progname) {
    fprintf(stderr, "Usage:\n %s [-h] [-v] [-q] [-s] [-r %d] [--rowsets %d]\n", progname, nrows, nrowsets);
    return 1;
}

int test_main (int argc, const char *argv[]) {
    const char *progname=argv[0];
    argc--; argv++;
    while (argc>0) {
        if (strcmp(argv[0],"-h")==0) {
            return usage(progname);
        } else if (strcmp(argv[0],"-v")==0) {
	    verbose=1;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose=0;
        } else if (strcmp(argv[0],"-r") == 0 && argc >= 1) {
            argc--; argv++;
            nrows = atoi(argv[0]);
        } else if (strcmp(argv[0],"--nrowsets") == 0 && argc >= 1) {
            argc--; argv++;
            nrowsets = atoi(argv[0]);
        } else if (strcmp(argv[0],"-s") == 0) {
            toku_ft_loader_set_size_factor(1);
	} else if (argc!=1) {
            return usage(progname);
	    exit(1);
	}
        else {
            break;
        }
	argc--; argv++;
    }

    // callibrate
    test_extractor(nrows, nrowsets, false);

    // run tests
    int event_limit = event_count;
    if (verbose) printf("event_limit=%d\n", event_limit);

    for (int i = 1; i <= event_limit; i++) {
        reset_event_counts();
        event_count_trigger = i;
        test_extractor(nrows, nrowsets, true);
    }

    return 0;
}

