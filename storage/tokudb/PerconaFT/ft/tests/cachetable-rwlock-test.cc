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

#include "test.h"

// test create and destroy

static void test_create_destroy(void) {
    struct st_rwlock the_rwlock, *rwlock = &the_rwlock;

    rwlock_init(toku_uninstrumented, rwlock);
    rwlock_destroy(rwlock);
}

// test read lock and unlock with no writers

static void test_simple_read_lock(int n) {
    struct st_rwlock the_rwlock, *rwlock = &the_rwlock;

    rwlock_init(toku_uninstrumented, rwlock);
    assert(rwlock_readers(rwlock) == 0);
    int i;
    for (i = 1; i <= n; i++) {
        rwlock_read_lock(rwlock, 0);
        assert(rwlock_readers(rwlock) == i);
        assert(rwlock_users(rwlock) == i);
    }
    for (i=n-1; i>=0; i--) {
        rwlock_read_unlock(rwlock);
        assert(rwlock_readers(rwlock) == i);
        assert(rwlock_users(rwlock) == i);
    }
    rwlock_destroy(rwlock);
}

// test write lock and unlock with no readers

static void test_simple_write_lock(void) {
    struct st_rwlock the_rwlock, *rwlock = &the_rwlock;

    rwlock_init(toku_uninstrumented, rwlock);
    assert(rwlock_users(rwlock) == 0);
    rwlock_write_lock(rwlock, 0);
    assert(rwlock_writers(rwlock) == 1);
    assert(rwlock_users(rwlock) == 1);
    rwlock_write_unlock(rwlock);
    assert(rwlock_users(rwlock) == 0);
    rwlock_destroy(rwlock);
}

struct rw_event {
    int e;
    struct st_rwlock the_rwlock;
    toku_mutex_t mutex;
};

static void rw_event_init(struct rw_event *rwe) {
    rwe->e = 0;
    rwlock_init(toku_uninstrumented, &rwe->the_rwlock);
    toku_mutex_init(toku_uninstrumented, &rwe->mutex, nullptr);
}

static void rw_event_destroy(struct rw_event *rwe) {
    rwlock_destroy(&rwe->the_rwlock);
    toku_mutex_destroy(&rwe->mutex);
}

static void *
test_writer_priority_thread (void *arg) {
    struct rw_event *CAST_FROM_VOIDP(rwe, arg);

    toku_mutex_lock(&rwe->mutex);
    rwlock_write_lock(&rwe->the_rwlock, &rwe->mutex);
    rwe->e++; assert(rwe->e == 3);
    toku_mutex_unlock(&rwe->mutex);
    sleep(1);
    toku_mutex_lock(&rwe->mutex);
    rwe->e++; assert(rwe->e == 4);
    rwlock_write_unlock(&rwe->the_rwlock);
    toku_mutex_unlock(&rwe->mutex);
    
    return arg;
}

// test writer priority over new readers

static void
test_writer_priority (void) {
    struct rw_event rw_event, *rwe = &rw_event;
    ZERO_STRUCT(rw_event);
    int r;

    rw_event_init(rwe);
    toku_mutex_lock(&rwe->mutex);
    rwlock_read_lock(&rwe->the_rwlock, &rwe->mutex);
    sleep(1);
    rwe->e++; assert(rwe->e == 1);
    toku_mutex_unlock(&rwe->mutex);

    toku_pthread_t tid;
    r = toku_pthread_create(
        toku_uninstrumented, &tid, 0, test_writer_priority_thread, rwe);
    sleep(1);
    toku_mutex_lock(&rwe->mutex);
    rwe->e++;
    assert(rwe->e == 2);
    toku_mutex_unlock(&rwe->mutex);

    sleep(1);
    toku_mutex_lock(&rwe->mutex);
    rwlock_read_unlock(&rwe->the_rwlock);
    toku_mutex_unlock(&rwe->mutex);
    sleep(1);
    toku_mutex_lock(&rwe->mutex);
    rwlock_read_lock(&rwe->the_rwlock, &rwe->mutex);
    rwe->e++; assert(rwe->e == 5);
    toku_mutex_unlock(&rwe->mutex);
    sleep(1);
    toku_mutex_lock(&rwe->mutex);
    rwlock_read_unlock(&rwe->the_rwlock);
    toku_mutex_unlock(&rwe->mutex);

    void *ret;
    r = toku_pthread_join(tid, &ret); assert(r == 0);

    rw_event_destroy(rwe);
}

// test single writer

static void *
test_single_writer_thread (void *arg) {
    struct rw_event *CAST_FROM_VOIDP(rwe, arg);

    toku_mutex_lock(&rwe->mutex);
    rwlock_write_lock(&rwe->the_rwlock, &rwe->mutex);
    rwe->e++; assert(rwe->e == 3);
    assert(rwlock_writers(&rwe->the_rwlock) == 1);
    rwlock_write_unlock(&rwe->the_rwlock);
    toku_mutex_unlock(&rwe->mutex);
    
    return arg;
}

static void
test_single_writer (void) {
    struct rw_event rw_event, *rwe = &rw_event;
    ZERO_STRUCT(rw_event);
    int r;

    rw_event_init(rwe);
    assert(rwlock_writers(&rwe->the_rwlock) == 0);
    toku_mutex_lock(&rwe->mutex);
    rwlock_write_lock(&rwe->the_rwlock, &rwe->mutex);
    assert(rwlock_writers(&rwe->the_rwlock) == 1);
    sleep(1);
    rwe->e++; assert(rwe->e == 1);
    toku_mutex_unlock(&rwe->mutex);

    toku_pthread_t tid;
    r = toku_pthread_create(
        toku_uninstrumented, &tid, 0, test_single_writer_thread, rwe);
    sleep(1);
    toku_mutex_lock(&rwe->mutex);
    rwe->e++;
    assert(rwe->e == 2);
    assert(rwlock_writers(&rwe->the_rwlock) == 1);
    assert(rwlock_users(&rwe->the_rwlock) == 2);
    rwlock_write_unlock(&rwe->the_rwlock);
    toku_mutex_unlock(&rwe->mutex);

    void *ret;
    r = toku_pthread_join(tid, &ret); assert(r == 0);

    assert(rwlock_writers(&rwe->the_rwlock) == 0);
    rw_event_destroy(rwe);
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    test_create_destroy();
    test_simple_read_lock(0);
    test_simple_read_lock(42);
    test_simple_write_lock();
    test_writer_priority();
    test_single_writer();
    
    return 0;
}
