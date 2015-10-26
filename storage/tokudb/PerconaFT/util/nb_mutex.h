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

typedef struct nb_mutex *NB_MUTEX;
struct nb_mutex {
    struct rwlock lock;
};

// initialize an nb mutex
static __attribute__((__unused__))
void
nb_mutex_init(NB_MUTEX nb_mutex) {
    rwlock_init(&nb_mutex->lock);
}

// destroy a read write lock
static __attribute__((__unused__))
void
nb_mutex_destroy(NB_MUTEX nb_mutex) {
    rwlock_destroy(&nb_mutex->lock);
}

// obtain a write lock
// expects: mutex is locked
static inline void nb_mutex_lock(NB_MUTEX nb_mutex, toku_mutex_t *mutex) {
    rwlock_write_lock(&nb_mutex->lock, mutex);
}

// release a write lock
// expects: mutex is locked

static inline void nb_mutex_unlock(NB_MUTEX nb_mutex) {
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
