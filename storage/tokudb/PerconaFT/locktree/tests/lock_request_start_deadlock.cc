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

// make sure deadlocks are detected when a lock request starts
void lock_request_unit_test::test_start_deadlock(void) {
    int r;
    locktree lt;

    // something short
    const uint64_t lock_wait_time = 10;

    DICTIONARY_ID dict_id = { 1 };
    lt.create(nullptr, dict_id, dbt_comparator);

    TXNID txnid_a = 1001;
    TXNID txnid_b = 2001;
    TXNID txnid_c = 3001;
    lock_request request_a;
    lock_request request_b;
    lock_request request_c;
    request_a.create();
    request_b.create();
    request_c.create();

    const DBT *one = get_dbt(1);
    const DBT *two = get_dbt(2);

    // start and succeed 1,1 for A and 2,2 for B.
    request_a.set(&lt, txnid_a, one, one, lock_request::type::WRITE, false);
    r = request_a.start();
    invariant_zero(r);
    request_b.set(&lt, txnid_b, two, two, lock_request::type::WRITE, false);
    r = request_b.start();
    invariant_zero(r);

    // txnid A should not be granted a lock on 2,2, so it goes pending.
    request_a.set(&lt, txnid_a, two, two, lock_request::type::WRITE, false);
    r = request_a.start();
    invariant(r == DB_LOCK_NOTGRANTED);

    // if txnid B wants a lock on 1,1 it should deadlock with A
    request_b.set(&lt, txnid_b, one, one, lock_request::type::WRITE, false);
    r = request_b.start();
    invariant(r == DB_LOCK_DEADLOCK);

    // txnid C should not deadlock on either of these - it should just time out.
    request_c.set(&lt, txnid_c, one, one, lock_request::type::WRITE, false);
    r = request_c.start();
    invariant(r == DB_LOCK_NOTGRANTED);
    r = request_c.wait(lock_wait_time);
    invariant(r == DB_LOCK_NOTGRANTED);
    request_c.set(&lt, txnid_c, two, two, lock_request::type::WRITE, false);
    r = request_c.start();
    invariant(r == DB_LOCK_NOTGRANTED);
    r = request_c.wait(lock_wait_time);
    invariant(r == DB_LOCK_NOTGRANTED);

    // release locks for A and B, then wait on A's request which should succeed
    // since B just unlocked and should have completed A's pending request.
    release_lock_and_retry_requests(&lt, txnid_a, one, one);
    release_lock_and_retry_requests(&lt, txnid_b, two, two);
    r = request_a.wait(lock_wait_time);
    invariant_zero(r);
    release_lock_and_retry_requests(&lt, txnid_a, two, two);

    request_a.destroy();
    request_b.destroy();
    request_c.destroy();

    lt.release_reference();
    lt.destroy();
}

} /* namespace toku */

int main(void) {
    toku::lock_request_unit_test test;
    test.test_start_deadlock();
    return 0;
}

