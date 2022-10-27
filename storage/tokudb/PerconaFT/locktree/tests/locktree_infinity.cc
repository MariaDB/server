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

// test that ranges with infinite endpoints work
void locktree_unit_test::test_infinity(void) {
    locktree lt;

    DICTIONARY_ID dict_id = { 1 };
    lt.create(nullptr, dict_id, dbt_comparator);

    int r;
    TXNID txnid_a = 1001;
    TXNID txnid_b = 2001;
    const DBT *zero = get_dbt(0);
    const DBT *one = get_dbt(1);
    const DBT *two = get_dbt(2);
    const DBT *five = get_dbt(5);
    const DBT min_int = min_dbt();
    const DBT max_int = max_dbt();

    // txn A will lock -inf, 5.
    r = lt.acquire_write_lock(txnid_a, toku_dbt_negative_infinity(), five, nullptr, false);
    invariant(r == 0);
    // txn B will fail to get any lock <= 5, even min_int
    r = lt.acquire_write_lock(txnid_b, five, five, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, zero, one, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, &min_int, &min_int, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, toku_dbt_negative_infinity(), &min_int, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);

    lt.remove_overlapping_locks_for_txnid(txnid_a, toku_dbt_negative_infinity(), five);

    // txn A will lock 1, +inf
    r = lt.acquire_write_lock(txnid_a, one, toku_dbt_positive_infinity(), nullptr, false);
    invariant(r == 0);
    // txn B will fail to get any lock >= 1, even max_int
    r = lt.acquire_write_lock(txnid_b, one, one, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, two, five, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, &max_int, &max_int, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, &max_int, toku_dbt_positive_infinity(), nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);

    lt.remove_overlapping_locks_for_txnid(txnid_a, toku_dbt_negative_infinity(), five);

    // txn A will lock -inf, +inf
    r = lt.acquire_write_lock(txnid_a, toku_dbt_negative_infinity(), toku_dbt_positive_infinity(), nullptr, false);
    invariant(r == 0);
    // txn B will fail to get any lock
    r = lt.acquire_write_lock(txnid_b, zero, one, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, two, five, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, &min_int, &min_int, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, &min_int, &max_int, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, &max_int, &max_int, nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, toku_dbt_negative_infinity(), toku_dbt_negative_infinity(), nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, toku_dbt_negative_infinity(), toku_dbt_positive_infinity(), nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);
    r = lt.acquire_write_lock(txnid_b, toku_dbt_positive_infinity(), toku_dbt_positive_infinity(), nullptr, false);
    invariant(r == DB_LOCK_NOTGRANTED);

    lt.remove_overlapping_locks_for_txnid(txnid_a, toku_dbt_negative_infinity(), toku_dbt_positive_infinity());

    lt.release_reference();
    lt.destroy();
}

} /* namespace toku */

int main(void) {
    toku::locktree_unit_test test;
    test.test_infinity();
    return 0;
}
