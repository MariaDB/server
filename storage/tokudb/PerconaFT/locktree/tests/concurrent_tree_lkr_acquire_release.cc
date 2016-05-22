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

void concurrent_tree_unit_test::test_lkr_acquire_release(void) {
    comparator cmp;
    cmp.create(compare_dbts, nullptr);

    // we'll test a tree that has values 0..20
    const uint64_t min = 0;
    const uint64_t max = 20;

    // acquire/release should work regardless of how the
    // data was inserted into the tree, so we test it
    // on a tree whose elements were populated starting
    // at each value 0..20 (so we get different rotation
    // behavior for each starting value in the tree).
    for (uint64_t start = min; start <= max; start++) {
        concurrent_tree tree;
        tree.create(&cmp);
        populate_tree(&tree, start, min, max);
        invariant(!tree.is_empty());

        for (uint64_t i = 0; i <= max; i++) {
            concurrent_tree::locked_keyrange lkr;
            lkr.prepare(&tree);
            invariant(lkr.m_tree == &tree);
            invariant(lkr.m_subtree->is_root());

            keyrange range;
            range.create(get_dbt(i), get_dbt(i));
            lkr.acquire(range);
            // the tree is not empty so the subtree root should not be empty
            invariant(!lkr.m_subtree->is_empty());

            // if the subtree root does not overlap then one of its children
            // must exist and have an overlapping range.
            if (!lkr.m_subtree->m_range.overlaps(cmp, range)) {
                treenode *left = lkr.m_subtree->m_left_child.ptr;
                treenode *right = lkr.m_subtree->m_right_child.ptr;
                if (left != nullptr) {
                    // left exists, so if it does not overlap then the right must
                    if (!left->m_range.overlaps(cmp, range)) {
                        invariant_notnull(right);
                        invariant(right->m_range.overlaps(cmp, range));
                    }
                } else {
                    // no left child, so the right must exist and be overlapping
                    invariant_notnull(right);
                    invariant(right->m_range.overlaps(cmp, range));
                }
            }

            lkr.release();
        }

        // remove everything one by one and then destroy
        keyrange range;
        concurrent_tree::locked_keyrange lkr;
        lkr.prepare(&tree);
        invariant(lkr.m_subtree->is_root());
        range.create(get_dbt(min), get_dbt(max));
        lkr.acquire(range);
        invariant(lkr.m_subtree->is_root());
        for (uint64_t i = 0; i <= max; i++) {
            range.create(get_dbt(i), get_dbt(i));
            lkr.remove(range);
        }
        lkr.release();
        tree.destroy();
    }

    cmp.destroy();
}

} /* namespace toku */

int main(void) {
    toku::concurrent_tree_unit_test test;
    test.test_lkr_acquire_release();
    return 0;
}
