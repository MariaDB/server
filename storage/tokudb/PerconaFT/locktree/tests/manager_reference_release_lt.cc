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

#include "manager_unit_test.h"

namespace toku {

static int create_cb(locktree *lt, void *extra) {
    lt->set_userdata(extra);
    bool *k = (bool *) extra;
    invariant(!(*k));
    (*k) = true;
    return 0;
}

static void destroy_cb(locktree *lt) {
    bool *k = (bool *) lt->get_userdata();
    invariant(*k);
    (*k) = false;
}

static int my_cmp(DB *UU(db), const DBT *UU(a), const DBT *UU(b)) {
    return 0;
}

void manager_unit_test::test_reference_release_lt(void) {
    locktree_manager mgr;
    mgr.create(create_cb, destroy_cb, nullptr, nullptr);
    toku::comparator my_comparator;
    my_comparator.create(my_cmp, nullptr);

    DICTIONARY_ID a = { 0 };
    DICTIONARY_ID b = { 1 };
    DICTIONARY_ID c = { 2 };
    bool aok = false;
    bool bok = false;
    bool cok = false;

    locktree *alt = mgr.get_lt(a, my_comparator, &aok);
    invariant_notnull(alt);
    locktree *blt = mgr.get_lt(b, my_comparator, &bok);
    invariant_notnull(alt);
    locktree *clt = mgr.get_lt(c, my_comparator, &cok);
    invariant_notnull(alt);

    // three distinct locktrees should have been returned
    invariant(alt != blt && alt != clt && blt != clt);

    // on create callbacks should have been called
    invariant(aok);
    invariant(bok);
    invariant(cok);

    // add 3 refs. b should still exist.
    mgr.reference_lt(blt);
    mgr.reference_lt(blt);
    mgr.reference_lt(blt);
    invariant(bok);
    // remove 3 refs. b should still exist.
    mgr.release_lt(blt);
    mgr.release_lt(blt);
    mgr.release_lt(blt);
    invariant(bok);

    // get another handle on a and b, they shoudl be the same
    // as the original alt and blt
    locktree *blt2 = mgr.get_lt(b, my_comparator, &bok);
    invariant(blt2 == blt);
    locktree *alt2 = mgr.get_lt(a, my_comparator, &aok);
    invariant(alt2 == alt);

    // remove one ref from everything. c should die. a and b are ok.
    mgr.release_lt(alt);
    mgr.release_lt(blt);
    mgr.release_lt(clt);
    invariant(aok);
    invariant(bok);
    invariant(!cok);

    // release a and b. both should die.
    mgr.release_lt(blt2);
    mgr.release_lt(alt2);
    invariant(!aok);
    invariant(!bok);
    
    my_comparator.destroy();
    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::manager_unit_test test;
    test.test_reference_release_lt();
    return 0; 
}
