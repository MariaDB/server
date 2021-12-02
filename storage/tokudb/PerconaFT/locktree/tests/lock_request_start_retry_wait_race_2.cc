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
#include <pthread.h>
#include <iostream>
#include <thread>
#include "locktree.h"
#include "test.h"

// Suppose that 2 threads are running a lock acquire, release, retry sequence.
// There is a race between the acquire and the release with 2 threads.
// If thread 1 acquires a lock, and thread 2 tries to acquire the same lock and
// fails, thread 1 may release its lock and retry pending lock requests BEFORE
// thread 2 adds itself to the pending lock requests. If this happens, then
// thread 2 will HANG until its lock timer expires even when the lock it is
// waiting for is FREE.

// This test exposes this problem as a test hang.  If the race is fixed, then
// the test runs to completion.

namespace toku {

    static void start_before_pending(void) { usleep(10000); }

    static void run_locker(locktree *lt,
                           TXNID txnid,
                           const DBT *key,
                           pthread_barrier_t *b) {
        for (int i = 0; i < 100000; i++) {
            int r;
            r = pthread_barrier_wait(b);
            assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);

            lock_request request;
            request.create();
            request.set(lt, txnid, key, key, lock_request::type::WRITE, false);

            // if the callback is included, then the race is easy to reproduce.
            // Otherwise, several test runs may be required before the race
            // happens.
            request.set_start_before_pending_test_callback(
                start_before_pending);

            // try to acquire the lock
            r = request.start();
            if (r == DB_LOCK_NOTGRANTED) {
                // wait for the lock to be granted
                r = request.wait(1000 * 1000);
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
    pthread_barrier_t b;
    int r = pthread_barrier_init(&b, nullptr, n_workers);
    assert(r == 0);
    for (int i = 0; i < n_workers; i++) {
        worker[i] = std::thread(toku::run_locker, &lt, i, one, &b);
    }
    for (int i = 0; i < n_workers; i++) {
        worker[i].join();
    }
    r = pthread_barrier_destroy(&b);
    assert(r == 0);
    lt.release_reference();
    lt.destroy();
    return 0;
}
