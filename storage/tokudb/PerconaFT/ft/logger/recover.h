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

#include <db.h>
#include <errno.h>

#include "portability/memory.h"
#include "portability/toku_portability.h"

#include "ft/comparator.h"
#include "ft/ft-ops.h"
#include "util/x1764.h"

typedef void (*prepared_txn_callback_t)(DB_ENV *env, struct tokutxn *txn);
typedef void (*keep_cachetable_callback_t)(DB_ENV *env, struct cachetable *ct);

// Run tokuft recovery from the log
// Returns 0 if success
int tokuft_recover(DB_ENV *env,
		   prepared_txn_callback_t prepared_txn_callback,
		   keep_cachetable_callback_t keep_cachetable_callback,
		   struct tokulogger *logger,
		   const char *env_dir,
                   const char *log_dir,
                   ft_compare_func bt_compare,
                   ft_update_func update_function,
                   generate_row_for_put_func generate_row_for_put,
                   generate_row_for_del_func generate_row_for_del,
                   size_t cachetable_size);

// Effect: Check the tokuft logs to determine whether or not we need to run recovery.
// If the log is empty or if there is a clean shutdown at the end of the log, then we
// dont need to run recovery.
// Returns: true if we need recovery, otherwise false.
int tokuft_needs_recovery(const char *logdir, bool ignore_empty_log);

// Return 0 if recovery log exists, ENOENT if log is missing
int tokuft_recover_log_exists(const char * log_dir);

// For test only - set callbacks for recovery testing
void toku_recover_set_callback (void (*)(void*), void*);
void toku_recover_set_callback2 (void (*)(void*), void*);

extern int tokuft_recovery_trace;

int toku_recover_lock (const char *lock_dir, int *lockfd);

int toku_recover_unlock(int lockfd);
