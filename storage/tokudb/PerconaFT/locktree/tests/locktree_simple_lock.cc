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

// test simple, non-overlapping read locks and then write locks
void locktree_unit_test::test_simple_lock(void) {
    locktree_manager mgr;
    mgr.create(nullptr, nullptr, nullptr, nullptr);

    DICTIONARY_ID dict_id = { .dictid = 1 };
    locktree *lt = mgr.get_lt(dict_id, dbt_comparator, nullptr);

    int r;
    TXNID txnid_a = 1001;
    TXNID txnid_b = 2001;
    TXNID txnid_c = 3001;
    TXNID txnid_d = 4001;
    const DBT *one = get_dbt(1);
    const DBT *two = get_dbt(2);
    const DBT *three = get_dbt(3);
    const DBT *four = get_dbt(4);

    for (int test_run = 0; test_run < 2; test_run++) {
        // test_run == 0 means test with read lock
        // test_run == 1 means test with write lock
#define ACQUIRE_LOCK(txn, left, right, conflicts) \
        test_run == 0 ? lt->acquire_read_lock(txn, left, right, conflicts, false) \
            : lt->acquire_write_lock(txn, left, right, conflicts, false)

        // four txns, four points
        r = ACQUIRE_LOCK(txnid_a, one, one, nullptr);
        invariant(r == 0);
        r = ACQUIRE_LOCK(txnid_b, two, two, nullptr);
        invariant(r == 0);
        r = ACQUIRE_LOCK(txnid_c, three, three, nullptr);
        invariant(r == 0);
        r = ACQUIRE_LOCK(txnid_d, four, four, nullptr);
        invariant(r == 0);
        locktree_test_release_lock(lt, txnid_a, one, one);
        locktree_test_release_lock(lt, txnid_b, two, two);
        locktree_test_release_lock(lt, txnid_c, three, three);
        locktree_test_release_lock(lt, txnid_d, four, four);
        invariant(no_row_locks(lt));

        // two txns, two ranges
        r = ACQUIRE_LOCK(txnid_c, one, two, nullptr);
        invariant(r == 0);
        r = ACQUIRE_LOCK(txnid_b, three, four, nullptr);
        invariant(r == 0);
        locktree_test_release_lock(lt, txnid_c, one, two);
        locktree_test_release_lock(lt, txnid_b, three, four);
        invariant(no_row_locks(lt));

        // two txns, one range, one point
        r = ACQUIRE_LOCK(txnid_c, three, four, nullptr);
        invariant(r == 0);
        r = ACQUIRE_LOCK(txnid_d, one, one, nullptr);
        invariant(r == 0);
        locktree_test_release_lock(lt, txnid_c, three, four);
        locktree_test_release_lock(lt, txnid_d, one, one);
        invariant(no_row_locks(lt));

#undef ACQUIRE_LOCK
    }

    const int64_t num_locks = 10000;

    int64_t *keys = (int64_t *) toku_malloc(num_locks * sizeof(int64_t));
    for (int64_t i = 0; i < num_locks; i++) {
        keys[i] = i; 
    }
    for (int64_t i = 0; i < num_locks; i++) {
        int64_t k = rand() % num_locks; 
        int64_t tmp = keys[k];
        keys[k] = keys[i];
        keys[i] = tmp;
    }


    r = mgr.set_max_lock_memory((num_locks + 1) * 500);
    invariant_zero(r);

    DBT k;
    k.ulen = 0;
    k.size = sizeof(keys[0]);
    k.flags = DB_DBT_USERMEM;

    for (int64_t i = 0; i < num_locks; i++) {
        k.data = (void *) &keys[i];
        r = lt->acquire_read_lock(txnid_a, &k, &k, nullptr, false);
        invariant(r == 0);
    }

    for (int64_t i = 0; i < num_locks; i++) {
        k.data = (void *) &keys[i];
        locktree_test_release_lock(lt, txnid_a, &k, &k);
    }

    toku_free(keys);

    mgr.release_lt(lt);
    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::locktree_unit_test test;
    test.test_simple_lock();
    return 0;
}
