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

#include <toku_assert.h>
#include <toku_portability.h>
#include <toku_instrumentation.h>

/* Readers/writers locks implementation
 *
 *****************************************
 *     Overview
 *****************************************
 *
 * PerconaFT employs readers/writers locks for the ephemeral locks (e.g.,
 * on FT nodes) Why not just use the toku_pthread_rwlock API?
 *
 *   1) we need multiprocess rwlocks (not just multithreaded)
 *
 *   2) pthread rwlocks are very slow since they entail a system call
 *   (about 2000ns on a 2GHz T2500.)
 *
 *     Related: We expect the common case to be that the lock is
 *     granted
 *
 *   3) We are willing to employ machine-specific instructions (such
 *   as atomic exchange, and mfence, each of which runs in about
 *   10ns.)
 *
 *   4) We want to guarantee nonstarvation (many rwlock
 *   implementations can starve the writers because another reader
 *   comes * along before all the other readers have unlocked.)
 *
 *****************************************
 *      How it works
 *****************************************
 *
 * We arrange that the rwlock object is in the address space of both
 * threads or processes.  For processes we use mmap().
 *
 * The rwlock struct comprises the following fields
 *
 *    a long mutex field (which is accessed using xchgl() or other
 *    machine-specific instructions.  This is a spin lock.
 *
 *    a read counter (how many readers currently have the lock?)
 *
 *    a write boolean (does a writer have the lock?)
 *
 *    a singly linked list of semaphores for waiting requesters.  This
 *    list is sorted oldest requester first.  Each list element
 *    contains a semaphore (which is provided by the requestor) and a
 *    boolean indicating whether it is a reader or a writer.
 *
 * To lock a read rwlock:
 *
 *    1) Acquire the mutex.
 *
 *    2) If the linked list is not empty or the writer boolean is true
 *    then
 *
 *       a) initialize your semaphore (to 0),
 *       b) add your list element to the end of the list (with  rw="read")
 *       c) release the mutex
 *       d) wait on the semaphore
 *       e) when the semaphore release, return success.
 *
 *    3) Otherwise increment the reader count, release the mutex, and
 *    return success.
 *
 * To lock the write rwlock is almost the same.
 *     1) Acquire the mutex
 *     2) If the list is not empty or the reader count is nonzero
 *        a) initialize semaphore
 *        b) add to end of list (with rw="write")
 *        c) release mutex
 *        d) wait on the semaphore
 *        e) return success when the semaphore releases
 *     3) Otherwise set writer=true, release mutex and return success.
 *
 * To unlock a read rwlock:
 *     1) Acquire mutex
 *     2) Decrement reader count
 *     3) If the count is still positive or the list is empty then
 *        return success
 *     4) Otherwise (count==zero and the list is nonempty):
 *        a) If the first element of the list is a reader:
 *            i) while the first element is a reader:
 *                 x) pop the list
 *                 y) increment the reader count
 *                 z) increment the semaphore (releasing it for some waiter)
 *            ii) return success
 *        b) Else if the first element is a writer
 *            i) pop the list
 *            ii) set writer to true
 *            iii) increment the semaphore
 *            iv) return success
 */

//Use case:
// A read lock is acquired by threads that get and pin an entry in the
// cachetable. A write lock is acquired by the writer thread when an entry
// is evicted from the cachetable and is being written storage.

//Use case:
// General purpose reader writer lock with properties:
// 1. multiple readers, no writers
// 2. one writer at a time
// 3. pending writers have priority over pending readers

// An external mutex must be locked when using these functions.  An alternate
// design would bury a mutex into the rwlock itself.  While this may
// increase parallelism at the expense of single thread performance, we
// are experimenting with a single higher level lock.

extern toku_instr_key *rwlock_cond_key;
extern toku_instr_key *rwlock_wait_read_key;
extern toku_instr_key *rwlock_wait_write_key;

typedef struct st_rwlock *RWLOCK;
struct st_rwlock {
    int reader;     // the number of readers
    int want_read;  // the number of blocked readers
    toku_cond_t wait_read;
    int writer;                  // the number of writers
    int want_write;              // the number of blocked writers
    toku_cond_t wait_write;
    toku_cond_t *wait_users_go_to_zero;
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_pthread_rwlock_t prwlock;
#endif
};

// returns: the sum of the number of readers, pending readers, writers, and
// pending writers

static inline int rwlock_users(RWLOCK rwlock) {
    return rwlock->reader + rwlock->want_read + rwlock->writer +
           rwlock->want_write;
}

#if defined(TOKU_MYSQL_WITH_PFS)
#define rwlock_init(K, R) inline_rwlock_init(K, R)
#else
#define rwlock_init(K, R) inline_rwlock_init(R)
#endif

// initialize a read write lock
static inline __attribute__((__unused__)) void inline_rwlock_init(
#if defined(TOKU_MYSQL_WITH_PFS)
    const toku_instr_key &rwlock_instr_key,
#endif
    RWLOCK rwlock) {
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_pthread_rwlock_init(rwlock_instr_key, &rwlock->prwlock, nullptr);
#endif
    rwlock->reader = rwlock->want_read = 0;
    rwlock->writer = rwlock->want_write = 0;
    toku_cond_init(toku_uninstrumented, &rwlock->wait_read, nullptr);
    toku_cond_init(toku_uninstrumented, &rwlock->wait_write, nullptr);
    rwlock->wait_users_go_to_zero = NULL;
}

// destroy a read write lock

static inline __attribute__((__unused__)) void rwlock_destroy(RWLOCK rwlock) {
    paranoid_invariant(rwlock->reader == 0);
    paranoid_invariant(rwlock->want_read == 0);
    paranoid_invariant(rwlock->writer == 0);
    paranoid_invariant(rwlock->want_write == 0);
    toku_cond_destroy(&rwlock->wait_read);
    toku_cond_destroy(&rwlock->wait_write);
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_pthread_rwlock_destroy(&rwlock->prwlock);
#endif
}

// obtain a read lock
// expects: mutex is locked

static inline void rwlock_read_lock(RWLOCK rwlock, toku_mutex_t *mutex) {
#ifdef TOKU_MYSQL_WITH_PFS
    /* Instrumentation start */
    toku_rwlock_instrumentation rwlock_instr;
    // TODO: pull location information up to caller
    toku_instr_rwlock_rdlock_wait_start(
        rwlock_instr, rwlock->prwlock, __FILE__, __LINE__);

#endif

    paranoid_invariant(!rwlock->wait_users_go_to_zero);
    if (rwlock->writer || rwlock->want_write) {
        rwlock->want_read++;
        while (rwlock->writer || rwlock->want_write) {
            toku_cond_wait(&rwlock->wait_read, mutex);
        }
        rwlock->want_read--;
    }
    rwlock->reader++;
#ifdef TOKU_MYSQL_WITH_PFS
    /* Instrumentation end */
    toku_instr_rwlock_wrlock_wait_end(rwlock_instr, 0);
#endif
}

// release a read lock
// expects: mutex is locked

static inline void rwlock_read_unlock(RWLOCK rwlock) {
#ifdef TOKU_MYSQL_WITH_PFS
    toku_instr_rwlock_unlock(rwlock->prwlock);
#endif
    paranoid_invariant(rwlock->reader > 0);
    paranoid_invariant(rwlock->writer == 0);
    rwlock->reader--;
    if (rwlock->reader == 0 && rwlock->want_write) {
        toku_cond_signal(&rwlock->wait_write);
    }
    if (rwlock->wait_users_go_to_zero && rwlock_users(rwlock) == 0) {
        toku_cond_signal(rwlock->wait_users_go_to_zero);
    }
}

// obtain a write lock
// expects: mutex is locked

static inline void rwlock_write_lock(RWLOCK rwlock, toku_mutex_t *mutex) {
#ifdef TOKU_MYSQL_WITH_PFS
    /* Instrumentation start */
    toku_rwlock_instrumentation rwlock_instr;
    toku_instr_rwlock_wrlock_wait_start(
        rwlock_instr, rwlock->prwlock, __FILE__, __LINE__);
#endif
    paranoid_invariant(!rwlock->wait_users_go_to_zero);
    if (rwlock->reader || rwlock->writer) {
        rwlock->want_write++;
        while (rwlock->reader || rwlock->writer) {
            toku_cond_wait(&rwlock->wait_write, mutex);
        }
        rwlock->want_write--;
    }
    rwlock->writer++;
#if defined(TOKU_MYSQL_WITH_PFS)
    /* Instrumentation end */
    toku_instr_rwlock_wrlock_wait_end(rwlock_instr, 0);
#endif
}

// release a write lock
// expects: mutex is locked

static inline void rwlock_write_unlock(RWLOCK rwlock) {
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_instr_rwlock_unlock(rwlock->prwlock);
#endif
    paranoid_invariant(rwlock->reader == 0);
    paranoid_invariant(rwlock->writer == 1);
    rwlock->writer--;
    if (rwlock->want_write) {
        toku_cond_signal(&rwlock->wait_write);
    } else if (rwlock->want_read) {
        toku_cond_broadcast(&rwlock->wait_read);
    }    
    if (rwlock->wait_users_go_to_zero && rwlock_users(rwlock) == 0) {
        toku_cond_signal(rwlock->wait_users_go_to_zero);
    }
}

// returns: the number of readers

static inline int rwlock_readers(RWLOCK rwlock) {
    return rwlock->reader;
}

// returns: the number of readers who are waiting for the lock

static inline int rwlock_blocked_readers(RWLOCK rwlock) {
    return rwlock->want_read;
}

// returns: the number of writers who are waiting for the lock

static inline int rwlock_blocked_writers(RWLOCK rwlock) {
    return rwlock->want_write;
}

// returns: the number of writers

static inline int rwlock_writers(RWLOCK rwlock) {
    return rwlock->writer;
}

static inline bool rwlock_write_will_block(RWLOCK rwlock) {
    return (rwlock->writer > 0 || rwlock->reader > 0);
}

static inline int rwlock_read_will_block(RWLOCK rwlock) {
    return (rwlock->writer > 0 || rwlock->want_write > 0);
}

static inline void rwlock_wait_for_users(RWLOCK rwlock, toku_mutex_t *mutex) {
    paranoid_invariant(!rwlock->wait_users_go_to_zero);
    toku_cond_t cond;
    toku_cond_init(toku_uninstrumented, &cond, nullptr);
    while (rwlock_users(rwlock) > 0) {
        rwlock->wait_users_go_to_zero = &cond;
        toku_cond_wait(&cond, mutex);
    }
    rwlock->wait_users_go_to_zero = NULL;
    toku_cond_destroy(&cond);
}

