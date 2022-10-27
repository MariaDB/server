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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>

#include <toku_portability.h>
#include <toku_assert.h>
#include <portability/toku_pthread.h>
#include <portability/toku_time.h>
#include <util/frwlock.h>
#include <util/rwlock.h>
#include "rwlock_condvar.h"

// We need to manually intialize partitioned counters so that the
// ones automatically incremented by the frwlock get handled properly.
#include <util/partitioned_counter.h>

toku_mutex_t mutex;
toku::frwlock w;

static void grab_write_lock(bool expensive) {
    toku_mutex_lock(&mutex);
    w.write_lock(expensive);
    toku_mutex_unlock(&mutex);
}

static void release_write_lock(void) {
    toku_mutex_lock(&mutex);
    w.write_unlock();
    toku_mutex_unlock(&mutex);
}

static void grab_read_lock(void) {
    toku_mutex_lock(&mutex);
    w.read_lock();
    toku_mutex_unlock(&mutex);
}

static void release_read_lock(void) {
    toku_mutex_lock(&mutex);
    w.read_unlock();
    toku_mutex_unlock(&mutex);
}

static void *do_cheap_wait(void *arg) {
    grab_write_lock(false);
    release_write_lock();
    return arg;
}

static void *do_expensive_wait(void *arg) {
    grab_write_lock(true);
    release_write_lock();
    return arg;
}

static void *do_read_wait(void *arg) {
    grab_read_lock();
    release_read_lock();
    return arg;
}

static void launch_cheap_waiter(void) {
    toku_pthread_t tid;
    int r = toku_pthread_create(
        toku_uninstrumented, &tid, nullptr, do_cheap_wait, nullptr);
    assert_zero(r);
    toku_pthread_detach(tid);
    sleep(1);
}

static void launch_expensive_waiter(void) {
    toku_pthread_t tid;
    int r = toku_pthread_create(
        toku_uninstrumented, &tid, nullptr, do_expensive_wait, nullptr);
    assert_zero(r);
    toku_pthread_detach(tid);
    sleep(1);
}

static void launch_reader(void) {
    toku_pthread_t tid;
    int r = toku_pthread_create(
        toku_uninstrumented, &tid, nullptr, do_read_wait, nullptr);
    assert_zero(r);
    toku_pthread_detach(tid);
    sleep(1);
}

static bool locks_are_expensive(void) {
    toku_mutex_lock(&mutex);
    assert(w.write_lock_is_expensive() == w.read_lock_is_expensive());
    bool is_expensive = w.write_lock_is_expensive();
    toku_mutex_unlock(&mutex);
    return is_expensive;
}

static void test_write_cheapness(void) {
    toku_mutex_init(toku_uninstrumented, &mutex, nullptr);
    w.init(&mutex);

    // single expensive write lock
    grab_write_lock(true);
    assert(locks_are_expensive());
    release_write_lock();
    assert(!locks_are_expensive());

    // single cheap write lock
    grab_write_lock(false);
    assert(!locks_are_expensive());
    release_write_lock();
    assert(!locks_are_expensive());

    // multiple read locks
    grab_read_lock();
    assert(!locks_are_expensive());
    grab_read_lock();
    grab_read_lock();
    assert(!locks_are_expensive());
    release_read_lock();
    release_read_lock();
    release_read_lock();
    assert(!locks_are_expensive());

    // expensive write lock and cheap writers waiting
    grab_write_lock(true);
    launch_cheap_waiter();
    assert(locks_are_expensive());
    launch_cheap_waiter();
    launch_cheap_waiter();
    assert(locks_are_expensive());
    release_write_lock();
    sleep(1);
    assert(!locks_are_expensive());

    // cheap write lock and expensive writer waiter
    grab_write_lock(false);
    launch_expensive_waiter();
    assert(locks_are_expensive());
    release_write_lock();
    sleep(1);

    // expensive write lock and expensive waiter
    grab_write_lock(true);
    launch_expensive_waiter();
    assert(locks_are_expensive());
    release_write_lock();
    sleep(1);

    // cheap write lock and cheap waiter
    grab_write_lock(false);
    launch_cheap_waiter();
    assert(!locks_are_expensive());
    release_write_lock();
    sleep(1);

    // read lock held and cheap waiter
    grab_read_lock();
    launch_cheap_waiter();
    assert(!locks_are_expensive());
    // add expensive waiter
    launch_expensive_waiter();
    assert(locks_are_expensive());
    release_read_lock();
    sleep(1);

    // read lock held and expensive waiter
    grab_read_lock();
    launch_expensive_waiter();
    assert(locks_are_expensive());
    // add expensive waiter
    launch_cheap_waiter();
    assert(locks_are_expensive());
    release_read_lock();
    sleep(1);

    // cheap write lock held and waiting read
    grab_write_lock(false);
    launch_reader();
    assert(!locks_are_expensive());
    launch_expensive_waiter();
    toku_mutex_lock(&mutex);
    assert(w.write_lock_is_expensive());
    // tricky case here, because we have a launched reader
    // that should be in the queue, a new read lock
    // should piggy back off that
    assert(!w.read_lock_is_expensive());
    toku_mutex_unlock(&mutex);
    release_write_lock();
    sleep(1);

    // expensive write lock held and waiting read
    grab_write_lock(true);
    launch_reader();
    assert(locks_are_expensive());
    launch_cheap_waiter();
    assert(locks_are_expensive());
    release_write_lock();
    sleep(1);

    w.deinit();
    toku_mutex_destroy(&mutex);
}

int main (int UU(argc), const char* UU(argv[])) {
    // Ultra ugly. We manually init/destroy partitioned counters
    // and context because normally toku_ft_layer_init() does that
    // for us, but we don't want to initialize everything.
    partitioned_counters_init();
    toku_context_status_init();
    test_write_cheapness();
    toku_context_status_destroy();
    partitioned_counters_destroy();
    return 0;
}
