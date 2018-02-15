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

#include "test.h"

#include <stdio.h>
#include <stdlib.h>

#include <toku_pthread.h>
#include <unistd.h>
#include <memory.h>
#include <sys/stat.h>
#include <db.h>

#include "threaded_stress_test_helpers.h"

toku_pthread_t checkpoint_tid;
static int cnt = 0;
static bool starting_a_chkpt = false;

int state_to_crash = 0;

static void *do_checkpoint_and_crash(void *arg) {
    // first verify that checkpointed_data is correct;
    DB_ENV* CAST_FROM_VOIDP(env, arg);
    if (verbose) printf("starting a checkpoint\n");
    int r = env->txn_checkpoint(env, 0, 0, 0); assert(r==0);
    if (verbose) printf("completed a checkpoint, about to crash\n");
    toku_hard_crash_on_purpose();
    return arg;
}

static void flt_callback(int flt_state, void* extra) {
    cnt++;
        if (verbose) printf("flt_state!! %d\n", flt_state);
        if (cnt > 0 && !starting_a_chkpt && flt_state == state_to_crash) {
            starting_a_chkpt = true;
            if (verbose)
                printf("flt_state %d\n", flt_state);
            int r = toku_pthread_create(toku_uninstrumented,
                                        &checkpoint_tid,
                                        nullptr,
                                        do_checkpoint_and_crash,
                                        extra);
            assert(r == 0);
            usleep(2 * 1000 * 1000);
        }
}


static void
stress_table(DB_ENV *env, DB **dbp, struct cli_args *cli_args) {
    //
    // the threads that we want:
    //   - one thread constantly updating random values
    //   - one thread doing table scan with bulk fetch
    //   - one thread doing table scan without bulk fetch
    //   - one thread doing random point queries
    //

    if (verbose) printf("starting creation of pthreads\n");
    const int num_threads = 1;
    struct arg myargs[num_threads];
    for (int i = 0; i < num_threads; i++) {
        arg_init(&myargs[i], dbp, env, cli_args);
    }

    // make the guy that updates the db
    struct update_op_args uoe = get_update_op_args(cli_args, NULL);
    myargs[0].operation_extra = &uoe;
    myargs[0].operation = update_op;
    //myargs[0].update_pad_frequency = 0;

    db_env_set_flusher_thread_callback(flt_callback, env);
    run_workers(myargs, num_threads, cli_args->num_seconds, true, cli_args);
}

static int
run_recover_flt_test(int argc, char *const argv[]) {
    struct cli_args args = get_default_args();
    // make test time arbitrarily high because we expect a crash
    args.num_seconds = 1000000000;
    if (state_to_crash == 1) {
        // Getting flt_state 1 (inbox flush) requires a larger tree with more messages floating in it
        args.num_elements = 100000;
        args.disperse_keys = true;
        args.key_size = 8;
        args.val_size = 192;
    } else {
        args.num_elements = 2000;
    }
    // we want to induce a checkpoint
    args.env_args.checkpointing_period = 0;
    args.env_args.cachetable_size = 20 * 1024 * 1024;
    parse_stress_test_args(argc, argv, &args);
    if (args.do_test_and_crash) {
        stress_test_main(&args);
    }
    if (args.do_recover) {
        stress_recover(&args);
    }
    return 0;
}
