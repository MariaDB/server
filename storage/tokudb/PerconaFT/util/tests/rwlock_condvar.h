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

/* Fair readers writer lock implemented using condition variables.
 * This is maintained so that we can measure the performance of a relatively simple implementation (this one) 
 * compared to a fast one that uses compare-and-swap (the one in ../toku_rwlock.c)
 * For now it's only for testing.
 */


// Fair readers/writer locks.  These are fair (meaning first-come first-served.  No reader starvation, and no writer starvation).  And they are
// probably faster than the linux readers/writer locks (pthread_rwlock_t).
struct toku_cv_fair_rwlock_waiter_state; // this structure is used internally.
typedef struct toku_cv_fair_rwlock_s {
    toku_mutex_t                          mutex;
    int                                   state; // 0 means no locks, + is number of readers locked, -1 is a writer
    struct toku_cv_fair_rwlock_waiter_state *waiters_head, *waiters_tail;
} toku_cv_fair_rwlock_t;

void toku_cv_fair_rwlock_init (toku_cv_fair_rwlock_t *rwlock);
void toku_cv_fair_rwlock_destroy (toku_cv_fair_rwlock_t *rwlock);
int toku_cv_fair_rwlock_rdlock (toku_cv_fair_rwlock_t *rwlock);
int toku_cv_fair_rwlock_wrlock (toku_cv_fair_rwlock_t *rwlock);
int toku_cv_fair_rwlock_unlock (toku_cv_fair_rwlock_t *rwlock);

struct toku_cv_fair_rwlock_waiter_state {
    char is_read;
    struct toku_cv_fair_rwlock_waiter_state *next;
    toku_cond_t cond;
};

static __thread struct toku_cv_fair_rwlock_waiter_state waitstate = {0, NULL, {PTHREAD_COND_INITIALIZER} };

void toku_cv_fair_rwlock_init (toku_cv_fair_rwlock_t *rwlock) {
    rwlock->state=0;
    rwlock->waiters_head = NULL;
    rwlock->waiters_tail = NULL;
    toku_mutex_init(&rwlock->mutex, NULL);
}

void toku_cv_fair_rwlock_destroy (toku_cv_fair_rwlock_t *rwlock) {
    toku_mutex_destroy(&rwlock->mutex);
}

int toku_cv_fair_rwlock_rdlock (toku_cv_fair_rwlock_t *rwlock) {
    toku_mutex_lock(&rwlock->mutex);
    if (rwlock->waiters_head!=NULL || rwlock->state<0) {
	// Someone is ahead of me in the queue, or someone has a lock.
	// We use per-thread-state for the condition variable.  A thread cannot get control and try to reuse the waiter state for something else.
	if (rwlock->waiters_tail) {
	    rwlock->waiters_tail->next = &waitstate;
	} else {
	    rwlock->waiters_head = &waitstate;
	}
	rwlock->waiters_tail = &waitstate;
	waitstate.next = NULL;
	waitstate.is_read = 1; 
	do {
	    toku_cond_wait(&waitstate.cond, &rwlock->mutex);
	} while (rwlock->waiters_head!=&waitstate || rwlock->state<0);
	rwlock->state++;
	rwlock->waiters_head=waitstate.next;
	if (waitstate.next==NULL) rwlock->waiters_tail=NULL;
	if (rwlock->waiters_head && rwlock->waiters_head->is_read) {
	    toku_cond_signal(&rwlock->waiters_head->cond);
	}
    } else {
	// No one is waiting, and any holders are readers.
	rwlock->state++;
    }
    toku_mutex_unlock(&rwlock->mutex);
    return 0;
}

int toku_cv_fair_rwlock_wrlock (toku_cv_fair_rwlock_t *rwlock) {
    toku_mutex_lock(&rwlock->mutex);
    if (rwlock->waiters_head!=NULL || rwlock->state!=0) {
	// Someone else is ahead of me, or someone has a lock the lock, so we must wait our turn.
	if (rwlock->waiters_tail) {
	    rwlock->waiters_tail->next = &waitstate;
	} else {
	    rwlock->waiters_head = &waitstate;
	}
	rwlock->waiters_tail = &waitstate;
	waitstate.next = NULL;
	waitstate.is_read = 0;
	do {
	    toku_cond_wait(&waitstate.cond, &rwlock->mutex);
	} while (rwlock->waiters_head!=&waitstate || rwlock->state!=0);
	rwlock->waiters_head = waitstate.next;
	if (waitstate.next==NULL) rwlock->waiters_tail=NULL;
    }
    rwlock->state = -1;
    toku_mutex_unlock(&rwlock->mutex);
    return 0;
}

int toku_cv_fair_rwlock_unlock (toku_cv_fair_rwlock_t *rwlock) {
    toku_mutex_lock(&rwlock->mutex);
    assert(rwlock->state!=0);
    if (rwlock->state>0) {
	rwlock->state--;
    } else {
	rwlock->state=0;
    }
    if (rwlock->state==0 && rwlock->waiters_head) {
	toku_cond_signal(&rwlock->waiters_head->cond);
    } else {
	// printf(" No one to wake\n");
    }
    toku_mutex_unlock(&rwlock->mutex);
    return 0;
}

