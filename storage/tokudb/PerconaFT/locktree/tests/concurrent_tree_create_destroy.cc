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

static comparator cmp;

// test that creating a concurrent tree puts it in a valid, empty state.
// the root node should be properly marked and have the correct comparator.
void concurrent_tree_unit_test::test_create_destroy(void) {
    concurrent_tree tree;
    tree.create(&cmp);

    invariant(tree.m_root.is_root());
    invariant(tree.m_root.is_empty());
    invariant(tree.m_root.m_cmp == &cmp);
    invariant_null(tree.m_root.m_left_child.ptr);
    invariant_null(tree.m_root.m_right_child.ptr);

    invariant(tree.is_empty());

    tree.destroy();
}

} /* namespace toku */

int main(void) {
    toku::concurrent_tree_unit_test test;
    test.test_create_destroy();
    return 0;
}
