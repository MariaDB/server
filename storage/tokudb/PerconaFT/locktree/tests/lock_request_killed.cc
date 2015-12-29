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

// test the kill callback.  the lock wait is killed 1/2 of the way through the wait.

#include "lock_request_unit_test.h"

namespace toku {

const uint64_t my_lock_wait_time = 10 * 1000; // 10 seconds
const uint64_t my_killed_time = 1 * 1000;

static int killed_calls = 0;
static uint64_t t_last_kill;
static uint64_t t_do_kill;

static int my_killed_callback(void) {
    uint64_t t_now = toku_current_time_microsec();
    assert(t_now >= t_last_kill);
    assert(t_now - t_last_kill >= my_killed_time * 1000 / 2); // div by 2 for valgrind which is not very accurate
    t_last_kill = t_now;
    killed_calls++;
    if (t_now >= t_do_kill)
        return 1;
    else
        return 0;
}

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

    // a locks 'one'
    request_a.set(&lt, txnid_a, one, one, lock_request::type::WRITE, false);
    r = request_a.start();
    assert_zero(r);

    // b tries to lock 'one'
    request_b.set(&lt, txnid_b, one, one, lock_request::type::WRITE, false);
    r = request_b.start();
    assert(r == DB_LOCK_NOTGRANTED);

    uint64_t t_start = toku_current_time_microsec();
    t_last_kill = t_start;
    t_do_kill = t_start + my_lock_wait_time * 1000 / 2;
    r = request_b.wait(my_lock_wait_time, my_killed_time, my_killed_callback);
    assert(r == DB_LOCK_NOTGRANTED);

    uint64_t t_end = toku_current_time_microsec();
    assert(t_end > t_start);
    uint64_t t_delta = t_end - t_start;
    // fprintf(stderr, "delta=%" PRIu64 "\n", t_delta);
    assert(t_delta >= my_lock_wait_time / 2);

    // fprintf(stderr, "killed_calls=%d\n", killed_calls);
    assert(killed_calls > 0);

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

