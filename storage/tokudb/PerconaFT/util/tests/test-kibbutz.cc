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
#include <util/kibbutz.h>

#include <memory.h>
#include <stdio.h>

#define ND 10
#define NT 4
bool done[ND];

static void dowork (void *idv) {
    int *CAST_FROM_VOIDP(idp, idv);
    int id = *idp;
    if (verbose) printf("s%d\n", id);
    assert(!done[id]);
    sleep(1);
    done[id] = true;
    sleep(1);
    if (verbose) printf("d%d\n", id);
}

static void kibbutz_test (bool parent_finishes_first) {
    KIBBUTZ k = NULL;
    int r = toku_kibbutz_create(NT, &k);
    assert(r == 0);
    if (verbose) printf("create\n");
    int ids[ND];
    for (int i=0; i<ND; i++) {
	done[i]=false;
	ids[i] =i;
    }
    for (int i=0; i<ND; i++) {
	if (verbose) printf("e%d\n", i);
	toku_kibbutz_enq(k, dowork, &ids[i]);
    }
    if (!parent_finishes_first) {
	sleep((ND+2*NT)/NT);
    }
    toku_kibbutz_destroy(k);
    for (int i=0; i<ND; i++) assert(done[i]);
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);
    
    kibbutz_test(false);
    kibbutz_test(true);
    if (verbose) printf("test ok\n");
    return 0;
}


