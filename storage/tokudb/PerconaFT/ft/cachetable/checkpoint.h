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

#include <stdint.h>

#include "ft/cachetable/cachetable.h"

//Effect: Change [end checkpoint (n) - begin checkpoint (n+1)] delay to
//        new_period seconds.  0 means disable.
void toku_set_checkpoint_period(CACHETABLE ct, uint32_t new_period);

uint32_t toku_get_checkpoint_period_unlocked(CACHETABLE ct);


/******
 *
 * NOTE: checkpoint_safe_lock is highest level lock
 *       multi_operation_lock is next level lock
 *       ydb_big_lock is next level lock
 *
 *       Locks must always be taken in this sequence (highest level first).
 *
 */


/****** 
 * Client code must hold the checkpoint_safe lock during the following operations:
 *       - delete a dictionary via DB->remove
 *       - delete a dictionary via DB_TXN->abort(txn) (where txn created a dictionary)
 *       - rename a dictionary //TODO: Handlerton rename needs to take this
 *                             //TODO: Handlerton rename needs to be recoded for transaction recovery
 *****/

void toku_checkpoint_safe_client_lock(void);

void toku_checkpoint_safe_client_unlock(void);



/****** 
 * These functions are called from the ydb level.
 * Client code must hold the multi_operation lock during the following operations:
 *       - insertion into multiple indexes
 *       - replace into (simultaneous delete/insert on a single key)
 *****/

void toku_multi_operation_client_lock(void);
void toku_low_priority_multi_operation_client_lock(void);

void toku_multi_operation_client_unlock(void);
void toku_low_priority_multi_operation_client_unlock(void);


// Initialize the checkpoint mechanism, must be called before any client operations.
// Must pass in function pointers to take/release ydb lock.
void toku_checkpoint_init(void);

void toku_checkpoint_destroy(void);

typedef enum {SCHEDULED_CHECKPOINT  = 0,   // "normal" checkpoint taken on checkpoint thread
              CLIENT_CHECKPOINT     = 1,   // induced by client, such as FLUSH LOGS or SAVEPOINT
              INDEXER_CHECKPOINT    = 2,
              STARTUP_CHECKPOINT    = 3,
              UPGRADE_CHECKPOINT    = 4,
              RECOVERY_CHECKPOINT   = 5,
              SHUTDOWN_CHECKPOINT   = 6} checkpoint_caller_t;

// Take a checkpoint of all currently open dictionaries
// Callbacks are called during checkpoint procedure while checkpoint_safe lock is still held.
// Callbacks are primarily intended for use in testing.
// caller_id identifies why the checkpoint is being taken.
int toku_checkpoint(CHECKPOINTER cp, struct tokulogger *logger,
                    void (*callback_f)(void *extra), void *extra,
                    void (*callback2_f)(void *extra2), void *extra2,
                    checkpoint_caller_t caller_id);

/******
 * These functions are called from the ydb level.
 * They return status information and have no side effects.
 * Some status information may be incorrect because no locks are taken to collect status.
 * (If checkpoint is in progress, it may overwrite status info while it is being read.)
 *****/
void toku_checkpoint_get_status(CACHETABLE ct, CHECKPOINT_STATUS stat);
