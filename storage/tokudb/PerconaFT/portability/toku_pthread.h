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

#include <pthread.h>
#include <time.h>
#include <stdint.h>

#include "toku_assert.h"

typedef pthread_attr_t toku_pthread_attr_t;
typedef pthread_t toku_pthread_t;
typedef pthread_mutexattr_t toku_pthread_mutexattr_t;
typedef pthread_mutex_t toku_pthread_mutex_t;
typedef pthread_condattr_t toku_pthread_condattr_t;
typedef pthread_cond_t toku_pthread_cond_t;
typedef pthread_rwlock_t toku_pthread_rwlock_t;
typedef pthread_rwlockattr_t  toku_pthread_rwlockattr_t;
typedef pthread_key_t toku_pthread_key_t;
typedef struct timespec toku_timespec_t;

#ifndef TOKU_PTHREAD_DEBUG
# define TOKU_PTHREAD_DEBUG 0
#endif

typedef struct toku_mutex {
    pthread_mutex_t pmutex;
#if TOKU_PTHREAD_DEBUG
    pthread_t owner; // = pthread_self(); // for debugging
    bool locked;
    bool valid;
#endif
} toku_mutex_t;

typedef struct toku_mutex_aligned {
    toku_mutex_t aligned_mutex __attribute__((__aligned__(64)));
} toku_mutex_aligned_t;

// Initializing with {} will fill in a struct with all zeros.
// But you may also need a pragma to suppress the warnings, as follows
//
//   #pragma GCC diagnostic push
//   #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
//   toku_mutex_t foo = ZERO_MUTEX_INITIALIZER;
//   #pragma GCC diagnostic pop
//
// In general it will be a lot of busy work to make this codebase compile
// cleanly with -Wmissing-field-initializers

# define ZERO_MUTEX_INITIALIZER {}

#if TOKU_PTHREAD_DEBUG
# define TOKU_MUTEX_INITIALIZER { .pmutex = PTHREAD_MUTEX_INITIALIZER, .owner = 0, .locked = false, .valid = true }
#else
# define TOKU_MUTEX_INITIALIZER { .pmutex = PTHREAD_MUTEX_INITIALIZER }
#endif

// Darwin doesn't provide adaptive mutexes
#if defined(__APPLE__)
# define TOKU_MUTEX_ADAPTIVE PTHREAD_MUTEX_DEFAULT
# if TOKU_PTHREAD_DEBUG
#  define TOKU_ADAPTIVE_MUTEX_INITIALIZER { .pmutex = PTHREAD_MUTEX_INITIALIZER, .owner = 0, .locked = false, .valid = true }
# else
#  define TOKU_ADAPTIVE_MUTEX_INITIALIZER { .pmutex = PTHREAD_MUTEX_INITIALIZER }
# endif
#else // __FreeBSD__, __linux__, at least
# define TOKU_MUTEX_ADAPTIVE PTHREAD_MUTEX_ADAPTIVE_NP
# if TOKU_PTHREAD_DEBUG
#  define TOKU_ADAPTIVE_MUTEX_INITIALIZER { .pmutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP, .owner = 0, .locked = false, .valid = true }
# else
#  define TOKU_ADAPTIVE_MUTEX_INITIALIZER { .pmutex = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP }
# endif
#endif

static inline void
toku_mutex_init(toku_mutex_t *mutex, const toku_pthread_mutexattr_t *attr) {
    int r = pthread_mutex_init(&mutex->pmutex, attr);
    assert_zero(r);
#if TOKU_PTHREAD_DEBUG
    mutex->locked = false;
    invariant(!mutex->valid);
    mutex->valid = true;
    mutex->owner = 0;
#endif
}

static inline void
toku_mutexattr_init(toku_pthread_mutexattr_t *attr) {
    int r = pthread_mutexattr_init(attr);
    assert_zero(r);
}

static inline void
toku_mutexattr_settype(toku_pthread_mutexattr_t *attr, int type) {
    int r = pthread_mutexattr_settype(attr, type);
    assert_zero(r);
}

static inline void
toku_mutexattr_destroy(toku_pthread_mutexattr_t *attr) {
    int r = pthread_mutexattr_destroy(attr);
    assert_zero(r);
}

static inline void
toku_mutex_destroy(toku_mutex_t *mutex) {
#if TOKU_PTHREAD_DEBUG
    invariant(mutex->valid);
    mutex->valid = false;
    invariant(!mutex->locked);
#endif
    int r = pthread_mutex_destroy(&mutex->pmutex);
    assert_zero(r);
}

static inline void
toku_mutex_lock(toku_mutex_t *mutex) {
    int r = pthread_mutex_lock(&mutex->pmutex);
    assert_zero(r);
#if TOKU_PTHREAD_DEBUG
    invariant(mutex->valid);
    invariant(!mutex->locked);
    invariant(mutex->owner == 0);
    mutex->locked = true;
    mutex->owner = pthread_self();
#endif
}

static inline int
toku_mutex_trylock(toku_mutex_t *mutex) {
    int r = pthread_mutex_trylock(&mutex->pmutex);
#if TOKU_PTHREAD_DEBUG
    if (r == 0) {
        invariant(mutex->valid);
        invariant(!mutex->locked);
        invariant(mutex->owner == 0);
        mutex->locked = true;
        mutex->owner = pthread_self();
    }
#endif
    return r;
}

static inline void
toku_mutex_unlock(toku_mutex_t *mutex) {
#if TOKU_PTHREAD_DEBUG
    invariant(mutex->owner == pthread_self());
    invariant(mutex->valid);
    invariant(mutex->locked);
    mutex->locked = false;
    mutex->owner = 0;
#endif
    int r = pthread_mutex_unlock(&mutex->pmutex);
    assert_zero(r);
}

#if TOKU_PTHREAD_DEBUG
static inline void
toku_mutex_assert_locked(const toku_mutex_t *mutex) {
    invariant(mutex->locked);
    invariant(mutex->owner == pthread_self());
}
#else
static inline void
toku_mutex_assert_locked(const toku_mutex_t *mutex __attribute__((unused))) {
}
#endif

// asserting that a mutex is unlocked only makes sense
// if the calling thread can guaruntee that no other threads
// are trying to lock this mutex at the time of the assertion
// 
// a good example of this is a tree with mutexes on each node.
// when a node is locked the caller knows that no other threads
// can be trying to lock its childrens' mutexes. the children
// are in one of two fixed states: locked or unlocked.
#if TOKU_PTHREAD_DEBUG
static inline void
toku_mutex_assert_unlocked(toku_mutex_t *mutex) {
    invariant(mutex->owner == 0);
    invariant(!mutex->locked);
}
#else
static inline void
toku_mutex_assert_unlocked(toku_mutex_t *mutex __attribute__((unused))) {
}
#endif

typedef struct toku_cond {
    pthread_cond_t pcond;
} toku_cond_t;

// Same considerations as for ZERO_MUTEX_INITIALIZER apply
#define ZERO_COND_INITIALIZER {}

#define TOKU_COND_INITIALIZER {PTHREAD_COND_INITIALIZER}

static inline void
toku_cond_init(toku_cond_t *cond, const toku_pthread_condattr_t *attr) {
    int r = pthread_cond_init(&cond->pcond, attr);
    assert_zero(r);
}

static inline void
toku_cond_destroy(toku_cond_t *cond) {
    int r = pthread_cond_destroy(&cond->pcond);
    assert_zero(r);
}

static inline void
toku_cond_wait(toku_cond_t *cond, toku_mutex_t *mutex) {
#if TOKU_PTHREAD_DEBUG
    invariant(mutex->locked);
    mutex->locked = false;
    mutex->owner = 0;
#endif
    int r = pthread_cond_wait(&cond->pcond, &mutex->pmutex);
    assert_zero(r);
#if TOKU_PTHREAD_DEBUG
    invariant(!mutex->locked);
    mutex->locked = true;
    mutex->owner = pthread_self();
#endif
}

static inline int 
toku_cond_timedwait(toku_cond_t *cond, toku_mutex_t *mutex, toku_timespec_t *wakeup_at) {
#if TOKU_PTHREAD_DEBUG
    invariant(mutex->locked);
    mutex->locked = false;
    mutex->owner = 0;
#endif
    int r = pthread_cond_timedwait(&cond->pcond, &mutex->pmutex, wakeup_at);
#if TOKU_PTHREAD_DEBUG
    invariant(!mutex->locked);
    mutex->locked = true;
    mutex->owner = pthread_self();
#endif
    return r;
}

static inline void 
toku_cond_signal(toku_cond_t *cond) {
    int r = pthread_cond_signal(&cond->pcond);
    assert_zero(r);
}

static inline void
toku_cond_broadcast(toku_cond_t *cond) {
    int r =pthread_cond_broadcast(&cond->pcond);
    assert_zero(r);
}

int 
toku_pthread_yield(void) __attribute__((__visibility__("default")));

static inline toku_pthread_t 
toku_pthread_self(void) {
    return pthread_self();
}

static inline void
toku_pthread_rwlock_init(toku_pthread_rwlock_t *__restrict rwlock, const toku_pthread_rwlockattr_t *__restrict attr) {
    int r = pthread_rwlock_init(rwlock, attr);
    assert_zero(r);
}

static inline void
toku_pthread_rwlock_destroy(toku_pthread_rwlock_t *rwlock) {
    int r = pthread_rwlock_destroy(rwlock);
    assert_zero(r);
}

static inline void
toku_pthread_rwlock_rdlock(toku_pthread_rwlock_t *rwlock) {
    int r = pthread_rwlock_rdlock(rwlock);
    assert_zero(r);
}

static inline void
toku_pthread_rwlock_rdunlock(toku_pthread_rwlock_t *rwlock) {
    int r = pthread_rwlock_unlock(rwlock);
    assert_zero(r);
}

static inline void
toku_pthread_rwlock_wrlock(toku_pthread_rwlock_t *rwlock) {
    int r = pthread_rwlock_wrlock(rwlock);
    assert_zero(r);
}

static inline void
toku_pthread_rwlock_wrunlock(toku_pthread_rwlock_t *rwlock) {
    int r = pthread_rwlock_unlock(rwlock);
    assert_zero(r);
}

static inline int 
toku_pthread_create(toku_pthread_t *thread, const toku_pthread_attr_t *attr, void *(*start_function)(void *), void *arg) {
    return pthread_create(thread, attr, start_function, arg);
}

static inline int 
toku_pthread_join(toku_pthread_t thread, void **value_ptr) {
    return pthread_join(thread, value_ptr);
}

static inline int 
toku_pthread_detach(toku_pthread_t thread) {
    return pthread_detach(thread);
}

static inline int 
toku_pthread_key_create(toku_pthread_key_t *key, void (*destroyf)(void *)) {
    return pthread_key_create(key, destroyf);
}

static inline int 
toku_pthread_key_delete(toku_pthread_key_t key) {
    return pthread_key_delete(key);
}

static inline void *
toku_pthread_getspecific(toku_pthread_key_t key) {
    return pthread_getspecific(key);
}

static inline int 
toku_pthread_setspecific(toku_pthread_key_t key, void *data) {
    return pthread_setspecific(key, data);
}
