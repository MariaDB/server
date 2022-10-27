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

// The intent of this test is to measure the throughput of db->puts
// with multiple threads.

static void
stress_table(DB_ENV* env, DB** dbp, struct cli_args *cli_args) {
    if (verbose) printf("starting creation of pthreads\n");
    const int num_threads = cli_args->num_put_threads;
    struct arg myargs[num_threads];
    operation_t put_op = (cli_args->serial_insert
                          ? serial_put_op
                          : random_put_op_singledb);
    struct serial_put_extra spe[num_threads];
    ZERO_ARRAY(spe);
    for (int i = 0; i < num_threads; i++) {
        arg_init(&myargs[i], dbp, env, cli_args);
        myargs[i].operation = put_op;
        if (cli_args->serial_insert) {
            spe[i].current = cli_args->num_elements;
            myargs[i].operation_extra = &spe[i];
        }
    }
    const bool crash_at_end = false;
    run_workers(myargs, num_threads, cli_args->num_seconds, crash_at_end, cli_args);
}

int
test_main(int argc, char *const argv[]) {
    struct cli_args args = get_default_args_for_perf();
    args.num_elements = 0;  // want to start with empty DBs
    args.key_size = 8;
    args.val_size = 8;
    parse_stress_test_args(argc, argv, &args);
    // when there are multiple threads, its valid for two of them to
    // generate the same key and one of them fail with DB_LOCK_NOTGRANTED
    if (args.num_put_threads > 1) {
        args.crash_on_operation_failure = false;
    }
    perf_test_main(&args);
    return 0;
}
