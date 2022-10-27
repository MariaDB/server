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

#pragma once

#include "test.h"
#include "locktree_unit_test.h"

#include "lock_request.h"

namespace toku {

class lock_request_unit_test {
public:
    // create and set the object's internals, destroy should not crash.
    void test_create_destroy(void);

    // make setting keys and getting them back works properly.
    // at a high level, we want to make sure keys are copied
    // when appropriate and plays nice with +/- infinity.
    void test_get_set_keys(void);

    // starting a lock request without immediate success should get
    // stored in the lock request set as pending.
    void test_start_pending(void);

    // make sure deadlocks are detected when a lock request starts
    void test_start_deadlock(void);

    // test that the get_wait_time callback works
    void test_wait_time_callback(void);

private:
    // releases a single range lock and retries all lock requests.
    // this is kind of like what the ydb layer does, except that
    // the ydb layer releases all of a txn's locks at once using
    // lt->release_locks(), not individually using lt->remove_overlapping_locks_for_txnid).
    void release_lock_and_retry_requests(locktree *lt,
            TXNID txnid, const DBT *left_key, const DBT * right_key) {
        locktree_unit_test::locktree_test_release_lock(lt, txnid, left_key, right_key);
        lock_request::retry_all_lock_requests(lt);
    }
};

}
