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

#include <stdio.h>
#include "locktree.h"
#include "test.h"

// One client locks 1,2,3...
// The other client locks -1,-2,-3...
// Eventually lock escalation runs.

using namespace toku;

static int verbose = 0;
static int killed = 0;

static void locktree_release_lock(locktree *lt, TXNID txn_id, int64_t left_k, int64_t right_k) {
    range_buffer buffer;
    buffer.create();
    DBT left; toku_fill_dbt(&left, &left_k, sizeof left_k);
    DBT right; toku_fill_dbt(&right, &right_k, sizeof right_k);
    buffer.append(&left, &right);
    lt->release_locks(txn_id, &buffer);
    buffer.destroy();
}

// grab a write range lock on int64 keys bounded by left_k and right_k
static int locktree_write_lock(locktree *lt, TXNID txn_id, int64_t left_k, int64_t right_k, bool big_txn) {
    DBT left; toku_fill_dbt(&left, &left_k, sizeof left_k);
    DBT right; toku_fill_dbt(&right, &right_k, sizeof right_k);
    return lt->acquire_write_lock(txn_id, &left, &right, nullptr, big_txn);
}

static void run_big_txn(locktree_manager *mgr UU(), locktree *lt, TXNID txn_id, int64_t start_i) {
    fprintf(stderr, "%u run_big_txn %p %" PRIu64 " %" PRId64 "\n", toku_os_gettid(), lt, txn_id, start_i);
    int64_t last_i = -1;
    for (int64_t i = start_i; !killed; i++) {
        if (0)
            printf("%u %" PRId64 "\n", toku_os_gettid(), i);
        uint64_t t_start = toku_current_time_microsec();
        int r = locktree_write_lock(lt, txn_id, i, i, true);
        if (r != 0)
            break;
        last_i = i;
        uint64_t t_end = toku_current_time_microsec();
        uint64_t t_duration = t_end - t_start;
        if (t_duration > 100000) {
            printf("%u %s %" PRId64 " %" PRIu64 "\n", toku_os_gettid(), __FUNCTION__, i, t_duration);
        }
        toku_pthread_yield();
    }
    if (last_i != -1)
        locktree_release_lock(lt, txn_id, start_i, last_i); // release the range start_i .. last_i
}

struct arg {
    locktree_manager *mgr;
    locktree *lt;
    TXNID txn_id;
    int64_t start_i;
};

static void *big_f(void *_arg) {
    struct arg *arg = (struct arg *) _arg;
    run_big_txn(arg->mgr, arg->lt, arg->txn_id, arg->start_i);
    return arg;
}

static void e_callback(TXNID txnid, const locktree *lt, const range_buffer &buffer, void *extra) {
    if (verbose)
        printf("%u %s %" PRIu64 " %p %d %p\n", toku_os_gettid(), __FUNCTION__, txnid, lt, buffer.get_num_ranges(), extra);
}

static uint64_t get_escalation_count(locktree_manager &mgr) {
    LTM_STATUS_S ltm_status_test;
    mgr.get_status(&ltm_status_test);

    TOKU_ENGINE_STATUS_ROW key_status = NULL;
    // lookup keyname in status
    for (int i = 0; ; i++) {
        TOKU_ENGINE_STATUS_ROW status = &ltm_status_test.status[i];
        if (status->keyname == NULL)
            break;
        if (strcmp(status->keyname, "LTM_ESCALATION_COUNT") == 0) {
            key_status = status;
            break;
        }
    }
    assert(key_status);
    return key_status->value.num;
}

int main(int argc, const char *argv[]) {
    const int n_big = 2;
    int n_lt = 1;
    uint64_t stalls = 1;
    uint64_t max_lock_memory = 1000000;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "--stalls") == 0 && i+1 < argc) {
            stalls = atoll(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--n_lt") == 0 && i+1 < argc) {
            n_lt = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--max_lock_memory") == 0 && i+1 < argc) {
            max_lock_memory = atoll(argv[++i]);
            continue;
        }
    }

    int r;

    // create a manager
    locktree_manager mgr;
    mgr.create(nullptr, nullptr, e_callback, nullptr);
    mgr.set_max_lock_memory(max_lock_memory);

    // create lock trees
    locktree *lt[n_big];
    for (int i = 0; i < n_lt; i++) {
        DICTIONARY_ID dict_id = { .dictid = (uint64_t) i };
        lt[i] = mgr.get_lt(dict_id, dbt_comparator, nullptr);
        assert(lt[i]);
    }

    // create the worker threads
    struct arg big_arg[n_big];
    pthread_t big_ids[n_big];
    for (int i = 0; i < n_big; i++) {
        big_arg[i] = { &mgr, lt[i % n_lt], (TXNID)(1000+i), i == 0 ? 1 : -1000000000 };
        r = toku_pthread_create(&big_ids[i], nullptr, big_f, &big_arg[i]);
        assert(r == 0);
    }

    // wait for some escalations to occur
    while (get_escalation_count(mgr) < stalls) {
        sleep(1);
    }
    killed = 1;

    // cleanup
    for (int i = 0; i < n_big; i++) {
        void *ret;
        r = toku_pthread_join(big_ids[i], &ret);
        assert(r == 0);
    }
    for (int i = 0; i < n_lt ; i++) {
        mgr.release_lt(lt[i]);
    }
    mgr.destroy();

    return 0;
}
