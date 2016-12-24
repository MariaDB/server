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

#include <stdio.h>
#include <stdlib.h>

#include <toku_pthread.h>
#include <unistd.h>
#include <memory.h>
#include <sys/stat.h>
#include <db.h>

#include "threaded_stress_test_helpers.h"

//
// This test is a micro stress test that does multithreaded updates on a fixed size table.
// There is also a thread that scans the table with bulk fetch, ensuring the sum is zero.
//
// This test is targeted at stressing the locktree, hence the small table and many update threads.
//

static int UU() lock_escalation_op(DB_TXN *UU(txn), ARG arg, void* operation_extra, void *UU(stats_extra)) {
    invariant_null(operation_extra);
    if (!arg->cli->nolocktree) {
        toku_env_run_lock_escalation_for_test(arg->env);
    }
    return 0;
}

static int iterate_requests(DB *db, uint64_t txnid,
                            const DBT *left_key, const DBT *right_key,
                            uint64_t blocking_txnid,
                            uint64_t UU(start_time),
                            void *extra) {
    invariant_null(extra);
    invariant(db != nullptr);
    invariant(txnid > 0);
    invariant(left_key != nullptr);
    invariant(right_key != nullptr);
    invariant(blocking_txnid > 0);
    invariant(txnid != blocking_txnid);
    if (rand() % 5 == 0) {
        usleep(100);
    }
    return 0;
}

static int UU() iterate_pending_lock_requests_op(DB_TXN *UU(txn), ARG arg, void *UU(operation_extra), void *UU(stats_extra)) {
    DB_ENV *env = arg->env;
    int r = env->iterate_pending_lock_requests(env, iterate_requests, nullptr);
    invariant_zero(r);
    return r;
}

static int iterate_txns(DB_TXN *txn,
                        iterate_row_locks_callback iterate_locks,
                        void *locks_extra, void *extra) {
    uint64_t txnid = txn->id64(txn);
    uint64_t client_id; void *client_extra;
    txn->get_client_id(txn, &client_id, &client_extra);
    invariant_null(extra);
    invariant(txnid > 0);
    invariant(client_id == 0);
    DB *db;
    DBT left_key, right_key;
    while (iterate_locks(&db, &left_key, &right_key, locks_extra) == 0) {
        invariant_notnull(db);
        invariant_notnull(left_key.data);
        invariant(left_key.size > 0);
        invariant_notnull(right_key.data);
        invariant(right_key.size > 0);
        if (rand() % 5 == 0) {
            usleep(50);
        }
        memset(&left_key, 0, sizeof(DBT));
        memset(&right_key, 0, sizeof(DBT));
    }
    return 0;
}

static int UU() iterate_live_transactions_op(DB_TXN *UU(txn), ARG arg, void *UU(operation_extra), void *UU(stats_extra)) {
    DB_ENV *env = arg->env;
    int r = env->iterate_live_transactions(env, iterate_txns, nullptr);
    invariant_zero(r);
    return r;
}

static void
stress_table(DB_ENV *env, DB **dbp, struct cli_args *cli_args) {

    if (verbose) printf("starting creation of pthreads\n");
    const int non_update_threads = 4;
    const int num_threads = non_update_threads + cli_args->num_update_threads;
    struct arg myargs[num_threads];
    for (int i = 0; i < num_threads; i++) {
        arg_init(&myargs[i], dbp, env, cli_args);
    }
    struct scan_op_extra soe[1];

    // make the forward fast scanner
    soe[0].fast = true;
    soe[0].fwd = true;
    soe[0].prefetch = false;
    myargs[0].operation_extra = &soe[0];
    myargs[0].operation = scan_op;

    myargs[1].sleep_ms = 15L * 1000;
    myargs[1].operation_extra = nullptr;
    myargs[1].operation = lock_escalation_op;

    myargs[2].sleep_ms = 1L * 1000;
    myargs[2].operation_extra = nullptr;
    myargs[2].operation = iterate_pending_lock_requests_op;

    myargs[3].sleep_ms = 1L * 1000;
    myargs[3].operation_extra = nullptr;
    myargs[3].operation = iterate_live_transactions_op;

    // make the threads that update the db
    struct update_op_args uoe = get_update_op_args(cli_args, NULL);
    for (int i = non_update_threads; i < num_threads; ++i) {
        myargs[i].operation_extra = &uoe;
        myargs[i].operation = update_op;
        myargs[i].do_prepare = false;
        // the first three threads will prelock ranges before
        // doing sequential updates. the rest of the threads
        // will take point write locks on update as usual.
        // this ensures both ranges and points are stressed.
        myargs[i].prelock_updates = i < 5 ? true : false;
    }

    run_workers(myargs, num_threads, cli_args->num_seconds, false, cli_args);
}

int
test_main(int argc, char *const argv[]) {
    struct cli_args args = get_default_args();
    // default args for first, then parse any overrides
    args.num_update_threads = 8;
    args.num_elements = 512;
    args.txn_size = 16;
    parse_stress_test_args(argc, argv, &args);

    // we expect to get lock_notgranted op failures, and we
    // don't want the overhead of fsync on small txns
    args.crash_on_operation_failure = false;
    args.env_args.sync_period = 100; // speed up the test by not fsyncing very often
    stress_test_main(&args);
    return 0;
}
