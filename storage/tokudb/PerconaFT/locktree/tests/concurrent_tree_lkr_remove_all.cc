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

#include "concurrent_tree_unit_test.h"

namespace toku {

// remove_all on a locked keyrange should properly remove everything
// from the tree and account correctly for the amount of memory released.
void concurrent_tree_unit_test::test_lkr_remove_all(void) {
    comparator cmp;
    cmp.create(compare_dbts, nullptr);

    // we'll test a tree that has values 0..20
    const uint64_t min = 0;
    const uint64_t max = 20;

    // remove_all should work regardless of how the
    // data was inserted into the tree, so we test it
    // on a tree whose elements were populated starting
    // at each value 0..20 (so we get different rotation
    // behavior for each starting value in the tree).
    for (uint64_t start = min; start <= max; start++) {
        concurrent_tree tree;
        concurrent_tree::locked_keyrange lkr;

        tree.create(&cmp);
        populate_tree(&tree, start, min, max);
        invariant(!tree.is_empty());

        lkr.prepare(&tree);
        invariant(lkr.m_subtree->is_root());
        invariant(!lkr.m_subtree->is_empty());

        // remove_all() from the locked keyrange and assert that
        // the number of elements and memory removed is correct.
        lkr.remove_all();

        invariant(lkr.m_subtree->is_empty());
        invariant(tree.is_empty());
        invariant_null(tree.m_root.m_right_child.ptr);
        invariant_null(tree.m_root.m_left_child.ptr);

        lkr.release();
        tree.destroy();
    }

    cmp.destroy();
}

} /* namespace toku */

int main(void) {
    toku::concurrent_tree_unit_test test;
    test.test_lkr_remove_all();
    return 0;
}
