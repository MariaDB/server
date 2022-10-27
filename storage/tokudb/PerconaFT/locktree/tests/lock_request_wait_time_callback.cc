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

#include "lock_request_unit_test.h"

namespace toku {

static const uint64_t my_lock_wait_time = 10 * 1000; // 10 sec

// make sure deadlocks are detected when a lock request starts
void lock_request_unit_test::test_wait_time_callback(void) {
    int r;
    locktree lt;

    DICTIONARY_ID dict_id = { 1 };
    lt.create(nullptr, dict_id, dbt_comparator);

    TXNID txnid_a = 1001;
    lock_request request_a;
    request_a.create();

    TXNID txnid_b = 2001;
    lock_request request_b;
    request_b.create();

    const DBT *one = get_dbt(1);
    const DBT *two = get_dbt(2);

    // a locks 'one'
    request_a.set(&lt, txnid_a, one, one, lock_request::type::WRITE, false);
    r = request_a.start();
    assert_zero(r);

    // b tries to lock 'one'
    request_b.set(&lt, txnid_b, one, two, lock_request::type::WRITE, false);
    r = request_b.start();
    assert(r == DB_LOCK_NOTGRANTED);
    uint64_t t_start = toku_current_time_microsec();
    r = request_b.wait(my_lock_wait_time);
    uint64_t t_end = toku_current_time_microsec();
    assert(r == DB_LOCK_NOTGRANTED);
    assert(t_end > t_start);
    uint64_t t_delta = t_end - t_start;
    assert(t_delta >= my_lock_wait_time);
    request_b.destroy();

    release_lock_and_retry_requests(&lt, txnid_a, one, one);
    request_a.destroy();

    lt.release_reference();
    lt.destroy();
}

} /* namespace toku */

int main(void) {
    toku::lock_request_unit_test test;
    test.test_wait_time_callback();
    return 0;
}

