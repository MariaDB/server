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

// ensure that small transactions do not get stalled by a big transaction that has lots of locks
// ./locktree_escalation_big7_small1 --stalls 100 --max_lock_memory 1000000000 --verbose

using namespace toku;

static int verbose = 0;
static int killed = 0;
static pthread_t big_id, small_id;

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

static void run_big_txn(locktree_manager *mgr UU(), locktree **lt, int n_lt, TXNID txn_id) {
    int64_t last_i = -1;
    for (int64_t i = 0; !killed; i++) {
        for (int j = 0; j < n_lt; j++) {
            uint64_t t_start = toku_current_time_microsec();
            int r = locktree_write_lock(lt[j], txn_id, i, i, true);
            assert(r == 0);
            last_i = i;
            uint64_t t_end = toku_current_time_microsec();
            uint64_t t_duration = t_end - t_start;
            if (t_duration > 100000) {
                printf("%u %s %" PRId64 " %" PRIu64 "\n", toku_os_gettid(), __FUNCTION__, i, t_duration);
            }
        }
        toku_pthread_yield();
    }
    if (last_i != -1)
        for (int j = 0; j < n_lt; j++)
            locktree_release_lock(lt[j], txn_id, 0, last_i); // release the range 0 .. last_i
    if (verbose)
        printf("%u %s %" PRId64 "\n", toku_os_gettid(), __FUNCTION__, last_i);
}

struct big_arg {
    locktree_manager *mgr;
    locktree **lt;
    int n_lt;
    TXNID txn_id;
};

static void *big_f(void *_arg) {
    struct big_arg *arg = (struct big_arg *) _arg;
    assert(pthread_equal(pthread_self(), big_id));
    printf("%u %s\n", toku_os_gettid(), __FUNCTION__);
    run_big_txn(arg->mgr, arg->lt, arg->n_lt, arg->txn_id);
    return arg;
}

static void run_small_txn(locktree_manager *mgr UU(), locktree *lt, TXNID txn_id, int64_t k) {
    int64_t i;
    for (i = 0; !killed; i++) {
        uint64_t t_start = toku_current_time_microsec();
        int r = locktree_write_lock(lt, txn_id, k, k, false);
        assert(r == 0);
        uint64_t t_end = toku_current_time_microsec();
        uint64_t t_duration = t_end - t_start;
        if (t_duration > 100000) {
            printf("%u %s %" PRId64 " %" PRIu64 "\n", toku_os_gettid(), __FUNCTION__, i, t_duration);
        }
        locktree_release_lock(lt, txn_id, k, k);
        toku_pthread_yield();
    }
    if (verbose)
        printf("%u %s %" PRId64 "\n", toku_os_gettid(), __FUNCTION__, i);
}

struct small_arg {
    locktree_manager *mgr;
    locktree *lt;
    TXNID txn_id;
    int64_t k;
};

static void *small_f(void *_arg) {
    struct small_arg *arg = (struct small_arg *) _arg;
    printf("%u %s\n", toku_os_gettid(), __FUNCTION__);
    run_small_txn(arg->mgr, arg->lt, arg->txn_id, arg->k);
    return arg;
}

static void e_callback(TXNID txnid, const locktree *lt, const range_buffer &buffer, void *extra) {
    assert(pthread_equal(pthread_self(), big_id));
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
    uint64_t stalls = 1;
    int n_big = 7;
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
    uint64_t next_dict_id = 1;
    DICTIONARY_ID dict_id;
    locktree *big_lt[n_big];
    for (int i = 0; i < n_big; i++) {
        dict_id = { next_dict_id }; next_dict_id++;
        big_lt[i] = mgr.get_lt(dict_id, dbt_comparator, nullptr);
    }

    dict_id = { next_dict_id }; next_dict_id++;
    locktree *small_lt = mgr.get_lt(dict_id, dbt_comparator, nullptr);

    // create the worker threads
    struct big_arg big_arg = {&mgr, big_lt, n_big, 1000};
    r = toku_pthread_create(
        toku_uninstrumented, &big_id, nullptr, big_f, &big_arg);
    assert(r == 0);

    struct small_arg small_arg = {&mgr, small_lt, 2000, 0};
    r = toku_pthread_create(
        toku_uninstrumented, &small_id, nullptr, small_f, &small_arg);
    assert(r == 0);

    // wait for some escalations to occur
    while (get_escalation_count(mgr) < stalls) {
        sleep(1);
    }
    killed = 1;

    // cleanup
    void *ret;
    r = toku_pthread_join(big_id, &ret);
    assert(r == 0);

    r = toku_pthread_join(small_id, &ret);
    assert(r == 0);

    for (int i = 0; i < n_big; i++)
        mgr.release_lt(big_lt[i]);
    mgr.release_lt(small_lt);
    mgr.destroy();

    return 0;
}
