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

/***********
 * The purpose of this file is to implement the high-level logic for 
 * taking a checkpoint.
 *
 * There are three locks used for taking a checkpoint.  They are listed below.
 *
 * NOTE: The reader-writer locks may be held by either multiple clients 
 *       or the checkpoint function.  (The checkpoint function has the role
 *       of the writer, the clients have the reader roles.)
 *
 *  - multi_operation_lock
 *    This is a new reader-writer lock.
 *    This lock is held by the checkpoint function only for as long as is required to 
 *    to set all the "pending" bits and to create the checkpoint-in-progress versions
 *    of the header and translation table (btt).
 *    The following operations must take the multi_operation_lock:
 *     - any set of operations that must be atomic with respect to begin checkpoint
 *
 *  - checkpoint_safe_lock
 *    This is a new reader-writer lock.
 *    This lock is held for the entire duration of the checkpoint.
 *    It is used to prevent more than one checkpoint from happening at a time
 *    (the checkpoint function is non-re-entrant), and to prevent certain operations
 *    that should not happen during a checkpoint.  
 *    The following operations must take the checkpoint_safe lock:
 *       - delete a dictionary
 *       - rename a dictionary
 *    The application can use this lock to disable checkpointing during other sensitive
 *    operations, such as making a backup copy of the database.
 *
 * Once the "pending" bits are set and the snapshots are taken of the header and btt,
 * most normal database operations are permitted to resume.
 *
 *
 *
 *****/

#include <time.h>

#include "portability/toku_portability.h"
#include "portability/toku_atomic.h"

#include "ft/cachetable/cachetable.h"
#include "ft/cachetable/checkpoint.h"
#include "ft/ft.h"
#include "ft/logger/log-internal.h"
#include "ft/logger/recover.h"
#include "util/frwlock.h"
#include "util/status.h"

void
toku_checkpoint_get_status(CACHETABLE ct, CHECKPOINT_STATUS statp) {
    cp_status.init();
    CP_STATUS_VAL(CP_PERIOD) = toku_get_checkpoint_period_unlocked(ct);
    *statp = cp_status;
}

static LSN last_completed_checkpoint_lsn;

static toku_mutex_t checkpoint_safe_mutex;
static toku::frwlock checkpoint_safe_lock;
static toku_pthread_rwlock_t multi_operation_lock;
static toku_pthread_rwlock_t low_priority_multi_operation_lock;

static bool initialized = false;     // sanity check
static volatile bool locked_mo = false;       // true when the multi_operation write lock is held (by checkpoint)
static volatile bool locked_cs = false;       // true when the checkpoint_safe write lock is held (by checkpoint)
static volatile uint64_t toku_checkpoint_begin_long_threshold = 1000000; // 1 second
static volatile uint64_t toku_checkpoint_end_long_threshold = 1000000 * 60; // 1 minute

// Note following static functions are called from checkpoint internal logic only,
// and use the "writer" calls for locking and unlocking.

static void
multi_operation_lock_init(void) {
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
#if defined(HAVE_PTHREAD_RWLOCKATTR_SETKIND_NP)
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#else
    // TODO: need to figure out how to make writer-preferential rwlocks
    // happen on osx
#endif
    toku_pthread_rwlock_init(&multi_operation_lock, &attr);
    toku_pthread_rwlock_init(&low_priority_multi_operation_lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    locked_mo = false;
}

static void
multi_operation_lock_destroy(void) {
    toku_pthread_rwlock_destroy(&multi_operation_lock);
    toku_pthread_rwlock_destroy(&low_priority_multi_operation_lock);
}

static void 
multi_operation_checkpoint_lock(void) {
    toku_pthread_rwlock_wrlock(&low_priority_multi_operation_lock);
    toku_pthread_rwlock_wrlock(&multi_operation_lock);   
    locked_mo = true;
}

static void 
multi_operation_checkpoint_unlock(void) {
    locked_mo = false;
    toku_pthread_rwlock_wrunlock(&multi_operation_lock);
    toku_pthread_rwlock_wrunlock(&low_priority_multi_operation_lock);    
}

static void
checkpoint_safe_lock_init(void) {
    toku_mutex_init(&checkpoint_safe_mutex, NULL);
    checkpoint_safe_lock.init(&checkpoint_safe_mutex);
    locked_cs = false;
}

static void
checkpoint_safe_lock_destroy(void) {
    checkpoint_safe_lock.deinit();
    toku_mutex_destroy(&checkpoint_safe_mutex);
}

static void 
checkpoint_safe_checkpoint_lock(void) {
    toku_mutex_lock(&checkpoint_safe_mutex);
    checkpoint_safe_lock.write_lock(false);
    toku_mutex_unlock(&checkpoint_safe_mutex);
    locked_cs = true;
}

static void 
checkpoint_safe_checkpoint_unlock(void) {
    locked_cs = false;
    toku_mutex_lock(&checkpoint_safe_mutex);
    checkpoint_safe_lock.write_unlock();
    toku_mutex_unlock(&checkpoint_safe_mutex);
}

// toku_xxx_client_(un)lock() functions are only called from client code,
// never from checkpoint code, and use the "reader" interface to the lock functions.

void 
toku_multi_operation_client_lock(void) {
    if (locked_mo)
        (void) toku_sync_fetch_and_add(&CP_STATUS_VAL(CP_CLIENT_WAIT_ON_MO), 1);
    toku_pthread_rwlock_rdlock(&multi_operation_lock);   
}

void 
toku_multi_operation_client_unlock(void) {
    toku_pthread_rwlock_rdunlock(&multi_operation_lock); 
}

void toku_low_priority_multi_operation_client_lock(void) {
    toku_pthread_rwlock_rdlock(&low_priority_multi_operation_lock);
}

void toku_low_priority_multi_operation_client_unlock(void) {
    toku_pthread_rwlock_rdunlock(&low_priority_multi_operation_lock);
}

void 
toku_checkpoint_safe_client_lock(void) {
    if (locked_cs)
        (void) toku_sync_fetch_and_add(&CP_STATUS_VAL(CP_CLIENT_WAIT_ON_CS), 1);
    toku_mutex_lock(&checkpoint_safe_mutex);
    checkpoint_safe_lock.read_lock();
    toku_mutex_unlock(&checkpoint_safe_mutex);
    toku_multi_operation_client_lock();
}

void 
toku_checkpoint_safe_client_unlock(void) {
    toku_mutex_lock(&checkpoint_safe_mutex);
    checkpoint_safe_lock.read_unlock();
    toku_mutex_unlock(&checkpoint_safe_mutex);
    toku_multi_operation_client_unlock();
}

// Initialize the checkpoint mechanism, must be called before any client operations.
void
toku_checkpoint_init(void) {
    multi_operation_lock_init();
    checkpoint_safe_lock_init();
    initialized = true;
}

void
toku_checkpoint_destroy(void) {
    multi_operation_lock_destroy();
    checkpoint_safe_lock_destroy();
    initialized = false;
}

#define SET_CHECKPOINT_FOOTPRINT(x) CP_STATUS_VAL(CP_FOOTPRINT) = footprint_offset + x


// Take a checkpoint of all currently open dictionaries
int 
toku_checkpoint(CHECKPOINTER cp, TOKULOGGER logger,
                void (*callback_f)(void*),  void * extra,
                void (*callback2_f)(void*), void * extra2,
                checkpoint_caller_t caller_id) {
    int footprint_offset = (int) caller_id * 1000;

    assert(initialized);

    (void) toku_sync_fetch_and_add(&CP_STATUS_VAL(CP_WAITERS_NOW), 1);
    checkpoint_safe_checkpoint_lock();
    (void) toku_sync_fetch_and_sub(&CP_STATUS_VAL(CP_WAITERS_NOW), 1);

    if (CP_STATUS_VAL(CP_WAITERS_NOW) > CP_STATUS_VAL(CP_WAITERS_MAX))
        CP_STATUS_VAL(CP_WAITERS_MAX) = CP_STATUS_VAL(CP_WAITERS_NOW);  // threadsafe, within checkpoint_safe lock

    SET_CHECKPOINT_FOOTPRINT(10);
    multi_operation_checkpoint_lock();
    SET_CHECKPOINT_FOOTPRINT(20);
    toku_ft_open_close_lock();
    
    SET_CHECKPOINT_FOOTPRINT(30);
    CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_BEGIN) = time(NULL);
    uint64_t t_checkpoint_begin_start = toku_current_time_microsec();
    toku_cachetable_begin_checkpoint(cp, logger);
    uint64_t t_checkpoint_begin_end = toku_current_time_microsec();

    toku_ft_open_close_unlock();
    multi_operation_checkpoint_unlock();

    SET_CHECKPOINT_FOOTPRINT(40);
    if (callback_f) {
        callback_f(extra);      // callback is called with checkpoint_safe_lock still held
    }

    uint64_t t_checkpoint_end_start = toku_current_time_microsec();
    toku_cachetable_end_checkpoint(cp, logger, callback2_f, extra2);
    uint64_t t_checkpoint_end_end = toku_current_time_microsec();

    SET_CHECKPOINT_FOOTPRINT(50);
    if (logger) {
        last_completed_checkpoint_lsn = logger->last_completed_checkpoint_lsn;
        toku_logger_maybe_trim_log(logger, last_completed_checkpoint_lsn);
        CP_STATUS_VAL(CP_LAST_LSN) = last_completed_checkpoint_lsn.lsn;
    }

    SET_CHECKPOINT_FOOTPRINT(60);
    CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_END) = time(NULL);
    CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_BEGIN_COMPLETE) = CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_BEGIN);
    CP_STATUS_VAL(CP_CHECKPOINT_COUNT)++;
    uint64_t duration = t_checkpoint_begin_end - t_checkpoint_begin_start;
    CP_STATUS_VAL(CP_BEGIN_TIME) += duration;
    if (duration >= toku_checkpoint_begin_long_threshold) {
        CP_STATUS_VAL(CP_LONG_BEGIN_TIME) += duration;
        CP_STATUS_VAL(CP_LONG_BEGIN_COUNT) += 1;
    }
    duration = t_checkpoint_end_end - t_checkpoint_end_start;
    CP_STATUS_VAL(CP_END_TIME) += duration;
    if (duration >= toku_checkpoint_end_long_threshold) {
        CP_STATUS_VAL(CP_LONG_END_TIME) += duration;
        CP_STATUS_VAL(CP_LONG_END_COUNT) += 1;
    }
    CP_STATUS_VAL(CP_TIME_CHECKPOINT_DURATION) += (uint64_t) ((time_t) CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_END)) - ((time_t) CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_BEGIN));
    CP_STATUS_VAL(CP_TIME_CHECKPOINT_DURATION_LAST) = (uint64_t) ((time_t) CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_END)) - ((time_t) CP_STATUS_VAL(CP_TIME_LAST_CHECKPOINT_BEGIN));
    CP_STATUS_VAL(CP_FOOTPRINT) = 0;

    checkpoint_safe_checkpoint_unlock();
    return 0;
}

#include <toku_race_tools.h>
void __attribute__((__constructor__)) toku_checkpoint_helgrind_ignore(void);
void
toku_checkpoint_helgrind_ignore(void) {
    TOKU_VALGRIND_HG_DISABLE_CHECKING(&cp_status, sizeof cp_status);
    TOKU_VALGRIND_HG_DISABLE_CHECKING(&locked_mo, sizeof locked_mo);
    TOKU_VALGRIND_HG_DISABLE_CHECKING(&locked_cs, sizeof locked_cs);
}

#undef SET_CHECKPOINT_FOOTPRINT
