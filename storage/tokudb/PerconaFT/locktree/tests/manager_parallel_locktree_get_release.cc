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

// This test crashes prior to the FT-600 fix.

#include "manager_unit_test.h"

namespace toku {

static int my_cmp(DB *UU(db), const DBT *UU(a), const DBT *UU(b)) {
    return 0;
}

static void my_test(locktree_manager *mgr) {
    toku::comparator my_comparator;
    my_comparator.create(my_cmp, nullptr);
    DICTIONARY_ID a = { 42 };
    for (int i=0; i<100000; i++) {
        locktree *alt = mgr->get_lt(a, my_comparator, nullptr);
        invariant_notnull(alt);
        mgr->release_lt(alt);
    }
    my_comparator.destroy();
}

static void *my_tester(void *arg) {
    locktree_manager *mgr = (locktree_manager *) arg;
    my_test(mgr);
    return arg;
}

void manager_unit_test::test_reference_release_lt(void) {
    int r;
    locktree_manager mgr;
    mgr.create(nullptr, nullptr, nullptr, nullptr);
    const int nthreads = 2;
    pthread_t ids[nthreads];
    for (int i = 0; i < nthreads; i++) {
        r = toku_pthread_create(
            toku_uninstrumented, &ids[i], nullptr, my_tester, &mgr);
        assert(r == 0);
    }
    for (int i = 0; i < nthreads; i++) {
        void *ret;
        r = toku_pthread_join(ids[i], &ret);
        assert(r == 0);
    }
    my_test(&mgr);
    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::manager_unit_test test;
    test.test_reference_release_lt();
    return 0; 
}
