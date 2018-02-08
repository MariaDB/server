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

#include "rwlock.h"

//Use case:
// General purpose non blocking mutex with properties:
// 1. one writer at a time

// An external mutex must be locked when using these functions.  An alternate
// design would bury a mutex into the nb_mutex itself.  While this may
// increase parallelism at the expense of single thread performance, we
// are experimenting with a single higher level lock.

extern toku_instr_key *nb_mutex_key;

typedef struct nb_mutex *NB_MUTEX;
struct nb_mutex {
    struct st_rwlock lock;
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_mutex_t toku_mutex;
#endif
};

#if defined(TOKU_MYSQL_WITH_PFS)
#define nb_mutex_init(MK, RK, M)                                 \
    inline_nb_mutex_init(MK, RK, M)
#else
#define nb_mutex_init(MK, RK, M) inline_nb_mutex_init(M)
#endif

// initialize an nb mutex
inline void inline_nb_mutex_init(
#if defined(TOKU_MYSQL_WITH_PFS)
    const toku_instr_key &mutex_instr_key,
    const toku_instr_key &rwlock_instr_key,
#endif
    NB_MUTEX nb_mutex) {
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_mutex_init(mutex_instr_key, &nb_mutex->toku_mutex, nullptr);
#endif
    rwlock_init(rwlock_instr_key, &nb_mutex->lock);
}

// destroy a read write lock
inline void nb_mutex_destroy(NB_MUTEX nb_mutex) {
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_instr_mutex_destroy(nb_mutex->toku_mutex.psi_mutex);
#endif
    rwlock_destroy(&nb_mutex->lock);
}

// obtain a write lock
// expects: mutex is locked
inline void nb_mutex_lock(NB_MUTEX nb_mutex, toku_mutex_t *mutex) {
#ifdef TOKU_MYSQL_WITH_PFS
    toku_mutex_instrumentation mutex_instr;
    toku_instr_mutex_lock_start(mutex_instr,
                                *mutex,
                                __FILE__,
                                __LINE__);  // TODO: pull these to caller?
#endif
    rwlock_write_lock(&nb_mutex->lock, mutex);
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_instr_mutex_lock_end(mutex_instr, 0);
#endif
}

// release a write lock
// expects: mutex is locked

inline void nb_mutex_unlock(NB_MUTEX nb_mutex) {
#if defined(TOKU_MYSQL_WITH_PFS)
    toku_instr_mutex_unlock(nb_mutex->toku_mutex.psi_mutex);
#endif
    rwlock_write_unlock(&nb_mutex->lock);
}

static inline void nb_mutex_wait_for_users(NB_MUTEX nb_mutex, toku_mutex_t *mutex) {
    rwlock_wait_for_users(&nb_mutex->lock, mutex);
}

// returns: the number of writers who are waiting for the lock

static inline int nb_mutex_blocked_writers(NB_MUTEX nb_mutex) {
    return rwlock_blocked_writers(&nb_mutex->lock);
}

// returns: the number of writers

static inline int nb_mutex_writers(NB_MUTEX nb_mutex) {
    return rwlock_writers(&nb_mutex->lock);
}

// returns: the sum of the number of readers, pending readers,
// writers, and pending writers
static inline int nb_mutex_users(NB_MUTEX nb_mutex) {
    return rwlock_users(&nb_mutex->lock);
}
