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
// This test is a form of stress that does operations on a single dictionary:
// We create a dictionary bigger than the cachetable (around 4x greater).
// Then, we spawn a bunch of pthreads that do the following:
//  - scan dictionary forward with bulk fetch
//  - scan dictionary forward slowly
//  - scan dictionary backward with bulk fetch
//  - scan dictionary backward slowly
//  - Grow the dictionary with insertions
//  - do random point queries into the dictionary
// With the small cachetable, this should produce quite a bit of churn in reading in and evicting nodes.
// If the test runs to completion without crashing, we consider it a success. It also tests that snapshots
// work correctly by verifying that table scans sum their vals to 0.
//
// This does NOT test:
//  - splits and merges
//  - multiple DBs
//
// Variables that are interesting to tweak and run:
//  - small cachetable
//  - number of elements
//

static void
stress_table(DB_ENV *env, DB **dbp, struct cli_args *cli_args) {
    //
    // the threads that we want:
    //   - one (or more) thread(s) constantly updating random values, wrapped in a persistent parent transaction.

    if (verbose) printf("starting creation of pthreads\n");
    const int num_threads = cli_args->num_update_threads;
    struct arg myargs[num_threads];
    for (int i = 0; i < num_threads; i++) {
        arg_init(&myargs[i], dbp, env, cli_args);
    }

    struct update_op_args uoe = get_update_op_args(cli_args, NULL);
    // make the guy that updates the db
    for (int i = 0; i < cli_args->num_update_threads; ++i) {
        myargs[i].operation_extra = &uoe;
        myargs[i].operation = update_op;
        myargs[i].do_prepare = true;
        myargs[i].wrap_in_parent = true;
    }

    run_workers(myargs, num_threads, cli_args->num_seconds, true, cli_args);
}

int
test_main(int argc, char *const argv[]) {
    struct cli_args args = get_default_args();
    args.num_seconds = 5;
    //args.txn_size = 64;   // 100 * 256 is more than enough to spill (4096) byte rollback nodes for parent and child.
    //args.val_size = 512;  // Large values to overflow a rollback log node fast.
    //args.env_args.node_size = 4*1024*1024;  // Large nodes to prevent spending much time
    //args.env_args.basement_node_size = 128*1024;  // Large nodes to prevent spending much time
    args.env_args.checkpointing_period = 1;
    parse_stress_test_args(argc, argv, &args);
    if (args.do_test_and_crash) {
        stress_test_main(&args);
    }
    if (args.do_recover) {
        stress_recover(&args);
    }
    return 0;
}
