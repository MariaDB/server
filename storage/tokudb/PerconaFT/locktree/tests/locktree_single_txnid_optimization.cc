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

#include "locktree_unit_test.h"

namespace toku {

// test that the same txn can relock ranges it already owns
// ensure that existing read locks can be upgrading to
// write locks if overlapping and ensure that existing read
// or write locks are consolidated by overlapping relocks.
void locktree_unit_test::test_single_txnid_optimization(void) {
    locktree lt;

    DICTIONARY_ID dict_id = { 1 };
    lt.create(nullptr, dict_id, dbt_comparator);

    const DBT *zero = get_dbt(0);
    const DBT *one = get_dbt(1);
    const DBT *two = get_dbt(2);
    const DBT *three = get_dbt(3);

    int r;
    TXNID txnid_a = 1001;
    TXNID txnid_b = 2001;

    // the single txnid optimization takes advantage of the fact that
    // a locktree with only locks for a single txnid can be unlocked
    // by just destroy every node. if this is implemented incorrectly,
    // then some other txnid's lock might get lost. so test that no
    // matter where txnid b takes its write lock in the middle of a bunch
    // of txnid a locks, the txnid b lock does not get lost.
    for (int where = 0; where < 4; where++) {
        range_buffer buffer;
        buffer.create();

#define lock_and_append_point_for_txnid_a(key) \
        r = lt.acquire_write_lock(txnid_a, key, key, nullptr, false);   \
        invariant_zero(r); \
        buffer.append(key, key);

#define maybe_point_locks_for_txnid_b(i) \
        if (where == i) { \
            r = lt.acquire_write_lock(txnid_b, one, one, nullptr, false);    \
            invariant_zero(r); \
        }

        lock_and_append_point_for_txnid_a(two);
        maybe_point_locks_for_txnid_b(0);

        lock_and_append_point_for_txnid_a(three);
        maybe_point_locks_for_txnid_b(1);

        lock_and_append_point_for_txnid_a(zero);
        maybe_point_locks_for_txnid_b(2);

        lt.release_locks(txnid_a, &buffer);

        // txnid b does not take a lock on iteration 3
        if (where != 3) {
            struct verify_fn_obj {
                TXNID expected_txnid;
                keyrange *expected_range;
                const comparator *cmp;
                bool fn(const keyrange &range, TXNID txnid) {
                    invariant(txnid == expected_txnid);
                    keyrange::comparison c = range.compare(*cmp, *expected_range);
                    invariant(c == keyrange::comparison::EQUALS);
                    return true;
                }
            } verify_fn;
            verify_fn.cmp = &lt.m_cmp;

            keyrange range;
            range.create(one, one);
            verify_fn.expected_txnid = txnid_b;
            verify_fn.expected_range = &range;
            locktree_iterate<verify_fn_obj>(&lt, &verify_fn);
            lt.remove_overlapping_locks_for_txnid(txnid_b, one, one);
        }

        buffer.destroy();
    }

    lt.release_reference();
    lt.destroy();
}

} /* namespace toku */

int main(void) {
    toku::locktree_unit_test test;
    test.test_single_txnid_optimization();
    return 0;
}
