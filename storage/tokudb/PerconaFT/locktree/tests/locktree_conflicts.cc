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

#include <toku_time.h>

__attribute__((__unused__))
static long current_time_usec(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_usec + t.tv_sec * 1000000;
}

namespace toku {

// test write lock conflicts when read or write locks exist
// test read lock conflicts when write locks exist
void locktree_unit_test::test_conflicts(void) {
    locktree lt;

    DICTIONARY_ID dict_id = { 1 };
    lt.create(nullptr, dict_id, dbt_comparator);

    int r;
    TXNID txnid_a = 1001;
    TXNID txnid_b = 2001;
    const DBT *zero = get_dbt(0);
    const DBT *one = get_dbt(1);
    const DBT *two = get_dbt(2);
    const DBT *three = get_dbt(3);
    const DBT *four = get_dbt(4);
    const DBT *five = get_dbt(5);

    for (int test_run = 0; test_run < 2; test_run++) {
        // test_run == 0 means test with read lock
        // test_run == 1 means test with write lock
#define ACQUIRE_LOCK(txn, left, right, conflicts) \
        test_run == 0 ? lt.acquire_read_lock(txn, left, right, conflicts, false) \
            : lt.acquire_write_lock(txn, left, right, conflicts, false)

        // acquire some locks for txnid_a
        r = ACQUIRE_LOCK(txnid_a, one, one, nullptr);
        invariant(r == 0);
        r = ACQUIRE_LOCK(txnid_a, three, four, nullptr);
        invariant(r == 0);

#undef ACQUIRE_LOCK

        for (int sub_test_run = 0; sub_test_run < 2; sub_test_run++) {
        // sub_test_run == 0 means test read lock on top of write lock
        // sub_test_run == 1 means test write lock on top of write lock
        // if test_run == 0, then read locks exist. only test write locks.
#define ACQUIRE_LOCK(txn, left, right, conflicts) \
        sub_test_run == 0 && test_run == 1 ? \
            lt.acquire_read_lock(txn, left, right, conflicts, false) \
          : lt.acquire_write_lock(txn, left, right, conflicts, false)
            // try to get point write locks for txnid_b, should fail
            r = ACQUIRE_LOCK(txnid_b, one, one, nullptr);
            invariant(r == DB_LOCK_NOTGRANTED);
            r = ACQUIRE_LOCK(txnid_b, three, three, nullptr);
            invariant(r == DB_LOCK_NOTGRANTED);
            r = ACQUIRE_LOCK(txnid_b, four, four, nullptr);
            invariant(r == DB_LOCK_NOTGRANTED);

            // try to get some overlapping range write locks for txnid_b, should fail
            r = ACQUIRE_LOCK(txnid_b, zero, two, nullptr);
            invariant(r == DB_LOCK_NOTGRANTED);
            r = ACQUIRE_LOCK(txnid_b, four, five, nullptr);
            invariant(r == DB_LOCK_NOTGRANTED);
            r = ACQUIRE_LOCK(txnid_b, two, three, nullptr);
            invariant(r == DB_LOCK_NOTGRANTED);
#undef ACQUIRE_LOCK
        }

        lt.remove_overlapping_locks_for_txnid(txnid_a, one, one);
        lt.remove_overlapping_locks_for_txnid(txnid_a, three, four);
        invariant(no_row_locks(&lt));
    }

    lt.release_reference();
    lt.destroy();
}

} /* namespace toku */

int main(void) {
    toku::locktree_unit_test test;
    test.test_conflicts();
    return 0;
}
