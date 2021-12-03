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

#ident \
    "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "lock_request.h"
#include <iostream>
#include <thread>
#include "locktree.h"
#include "test.h"

// Test FT-633, the data race on the lock request between ::start and ::retry
// This test is non-deterministic.  It uses sleeps at 2 critical places to
// expose the data race on the lock requests state.

namespace toku {

    static void locker_callback(void) { usleep(10000); }

    static void run_locker(locktree *lt, TXNID txnid, const DBT *key) {
        int i;
        for (i = 0; i < 1000; i++) {
            lock_request request;
            request.create();

            request.set(lt, txnid, key, key, lock_request::type::WRITE, false);

            // set the test callbacks
            request.set_start_test_callback(locker_callback);
            request.set_retry_test_callback(locker_callback);

            // try to acquire the lock
            int r = request.start();
            if (r == DB_LOCK_NOTGRANTED) {
                // wait for the lock to be granted
                r = request.wait(10 * 1000);
            }

            if (r == 0) {
                // release the lock
                range_buffer buffer;
                buffer.create();
                buffer.append(key, key);
                lt->release_locks(txnid, &buffer);
                buffer.destroy();

                // retry pending lock requests
                lock_request::retry_all_lock_requests(lt);
            }

            request.destroy();
            request.clearmem(0xab);

            toku_pthread_yield();
            if ((i % 10) == 0)
                std::cerr << std::this_thread::get_id() << " " << i
                          << std::endl;
        }
    }

} /* namespace toku */

int main(void) {
    toku::locktree lt;
    DICTIONARY_ID dict_id = {1};
    lt.create(nullptr, dict_id, toku::dbt_comparator);

    const DBT *one = toku::get_dbt(1);

    const int n_workers = 2;
    std::thread worker[n_workers];
    for (int i = 0; i < n_workers; i++) {
        worker[i] = std::thread(toku::run_locker, &lt, i, one);
    }
    for (int i = 0; i < n_workers; i++) {
        worker[i].join();
    }

    lt.release_reference();
    lt.destroy();
    return 0;
}

