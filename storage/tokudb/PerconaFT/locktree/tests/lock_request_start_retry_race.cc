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

#include <iostream>
#include "test.h"
#include "locktree.h"
#include "lock_request.h"

// Test FT-633, the data race on the lock request between ::start and ::retry
// This test is non-deterministic.  It uses sleeps at 2 critical places to
// expose the data race on the lock requests state.

namespace toku {

struct locker_arg {
    locktree *_lt;
    TXNID _id;
    const DBT *_key;

    locker_arg(locktree *lt, TXNID id, const DBT *key) : _lt(lt), _id(id), _key(key) {
    }
};

static void locker_callback(void) {
    usleep(10000);
}

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
        memset(&request, 0xab, sizeof request);

        toku_pthread_yield();
        if ((i % 10) == 0)
            std::cout << toku_pthread_self() << " " << i << std::endl;
    }
}

static void *locker(void *v_arg) {
    locker_arg *arg = static_cast<locker_arg *>(v_arg);
    run_locker(arg->_lt, arg->_id, arg->_key);
    return arg;
}

} /* namespace toku */

int main(void) {
    int r;

    toku::locktree lt;
    DICTIONARY_ID dict_id = { 1 };
    lt.create(nullptr, dict_id, toku::dbt_comparator);

    const DBT *one = toku::get_dbt(1);

    const int n_workers = 2;
    toku_pthread_t ids[n_workers];
    for (int i = 0; i < n_workers; i++) {
        toku::locker_arg *arg = new toku::locker_arg(&lt, i, one);
        r = toku_pthread_create(&ids[i], nullptr, toku::locker, arg);
        assert_zero(r);
    }
    for (int i = 0; i < n_workers; i++) {
        void *ret;
        r = toku_pthread_join(ids[i], &ret);
        assert_zero(r);
        toku::locker_arg *arg = static_cast<toku::locker_arg *>(ret);
        delete arg;
    }

    lt.release_reference();
    lt.destroy();
    return 0;
}

