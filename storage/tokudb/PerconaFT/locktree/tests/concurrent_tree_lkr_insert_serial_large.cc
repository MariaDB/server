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

#include <portability/toku_pthread.h>

#include "concurrent_tree_unit_test.h"

namespace toku {

// This is intended to be a black-box test for the concurrent_tree's
// ability to rebalance in the face of many serial insertions.
// If the code survives many inserts, it is considered successful.
void concurrent_tree_unit_test::test_lkr_insert_serial_large(void) {
    comparator cmp;
    cmp.create(compare_dbts, nullptr);

    concurrent_tree tree;
    tree.create(&cmp);

    // prepare and acquire the infinte range
    concurrent_tree::locked_keyrange lkr;
    lkr.prepare(&tree);
    lkr.acquire(keyrange::get_infinite_range());

    // 128k keys should be fairly stressful.
    // a bad tree will flatten and die way earlier than 128k inserts.
    // a good tree will rebalance and reach height logn(128k) ~= 17,
    // survival the onslaught of inserts.
    const uint64_t num_keys = 128 * 1024;

    // populate the tree with all the keys
    for (uint64_t i = 0; i < num_keys; i++) {
        DBT k;
        toku_fill_dbt(&k, &i, sizeof(i));
        keyrange range;
        range.create(&k, &k);
        lkr.insert(range, i);
    }

    // remove all of the keys
    for (uint64_t i = 0; i < num_keys; i++) {
        DBT k;
        toku_fill_dbt(&k, &i, sizeof(i));
        keyrange range;
        range.create(&k, &k);
        lkr.remove(range);
    }

    lkr.release();
    tree.destroy();
    cmp.destroy();
}

} /* namespace toku */

int main(void) {
    toku::concurrent_tree_unit_test test;
    test.test_lkr_insert_serial_large();
    return 0;
}
