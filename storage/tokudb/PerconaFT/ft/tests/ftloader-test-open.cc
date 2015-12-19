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

// The purpose of this test is to find memory leaks in the ft_loader_open function.  Right now, it finds leaks in some very simple
// cases. 

#define DONT_DEPRECATE_MALLOC
#include "test.h"
#include "loader/loader.h"
#include "loader/loader-internal.h"
#include "memory.h"
#include <portability/toku_path.h>


static int my_malloc_count = 0;
static int my_malloc_trigger = 0;

static void set_my_malloc_trigger(int n) {
    my_malloc_count = 0;
    my_malloc_trigger = n;
}

static void *my_malloc(size_t n) {
    my_malloc_count++;
    if (my_malloc_count == my_malloc_trigger) {
        errno = ENOSPC;
        return NULL;
    } else
        return os_malloc(n);
}

static int my_compare(DB *UU(desc), const DBT *UU(akey), const DBT *UU(bkey)) {
    return EINVAL;
}

static void test_loader_open(int ndbs) {
    int r;
    FTLOADER loader;

    // open the ft_loader. this runs the extractor.
    FT_HANDLE fts[ndbs];
    DB* dbs[ndbs];
    const char *fnames[ndbs];
    ft_compare_func compares[ndbs];
    for (int i = 0; i < ndbs; i++) {
        fts[i] = NULL;
        dbs[i] = NULL;
        fnames[i] = "";
        compares[i] = my_compare;
    }

    toku_set_func_malloc(my_malloc);

    int i;
    for (i = 0; ; i++) {
        set_my_malloc_trigger(i+1);

        r = toku_ft_loader_open(&loader, NULL, NULL, NULL, ndbs, fts, dbs, fnames, compares, "", ZERO_LSN, nullptr, true, 0, false, true);
        if (r == 0)
            break;
    }

    if (verbose) printf("i=%d\n", i);
    
    r = toku_ft_loader_abort(loader, true);
    assert(r == 0);
}

int test_main (int argc, const char *argv[]) {
    const char *progname=argv[0];
    argc--; argv++;
    while (argc>0) {
	if (strcmp(argv[0],"-v")==0) {
	    verbose=1;
	} else if (strcmp(argv[0],"-q")==0) {
	    verbose=0;
	} else if (argc!=1) {
	    fprintf(stderr, "Usage:\n %s [-v] [-q]\n", progname);
	    exit(1);
	}
        else {
            break;
        }
	argc--; argv++;
    }

    test_loader_open(0);
    test_loader_open(1);

    return 0;
}

