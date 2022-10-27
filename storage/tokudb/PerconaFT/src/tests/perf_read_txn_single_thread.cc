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

// The intent of this test is to measure how fast a single thread can 
// commit and create transactions when there exist N transactions.

DB_TXN** txns;
int num_txns;

static int commit_and_create_txn(
    DB_TXN* UU(txn), 
    ARG arg, 
    void* UU(operation_extra), 
    void* UU(stats_extra)
    ) 
{
    int rand_txn_id = random() % num_txns;
    int r = txns[rand_txn_id]->commit(txns[rand_txn_id], 0);
    CKERR(r);
    r = arg->env->txn_begin(arg->env, 0, &txns[rand_txn_id], arg->txn_flags | DB_TXN_READ_ONLY); 
    CKERR(r);
    return 0;
}

static void
stress_table(DB_ENV* env, DB** dbp, struct cli_args *cli_args) {
    if (verbose) printf("starting running of stress\n");

    num_txns = cli_args->txn_size;
    XCALLOC_N(num_txns, txns);
    for (int i = 0; i < num_txns; i++) {
        int r = env->txn_begin(env, 0, &txns[i], DB_TXN_SNAPSHOT); 
        CKERR(r);
    }
    
    struct arg myarg;
    arg_init(&myarg, dbp, env, cli_args);
    myarg.operation = commit_and_create_txn;
    
    run_workers(&myarg, 1, cli_args->num_seconds, false, cli_args);

    for (int i = 0; i < num_txns; i++) {
        int chk_r = txns[i]->commit(txns[i], 0);
        CKERR(chk_r);
    }
    toku_free(txns);
    num_txns = 0;
}

int
test_main(int argc, char *const argv[]) {
    num_txns = 0;
    txns = NULL;
    struct cli_args args = get_default_args_for_perf();
    parse_stress_test_args(argc, argv, &args);
    args.single_txn = true;
    // this test is all about transactions, make the DB small
    args.num_elements = 1;
    args.num_DBs= 1;    
    perf_test_main(&args);
    return 0;
}
