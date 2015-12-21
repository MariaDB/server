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

// Two big txn's grab alternating locks in a single lock tree.
// Eventually lock escalation runs.
// Since the locks can not be consolidated, the out of locks error should be returned.

using namespace toku;

static int verbose = 0;

static inline void locktree_release_lock(locktree *lt, TXNID txn_id, int64_t left_k, int64_t right_k) {
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
    uint64_t max_lock_memory = 1000000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose++;
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

    const TXNID txn_a = 10;
    const TXNID txn_b = 100;

    // create lock trees
    DICTIONARY_ID dict_id = { .dictid = 1 };
    locktree *lt = mgr.get_lt(dict_id, dbt_comparator, nullptr);

    int64_t last_i = -1;
    for (int64_t i = 0; ; i++) {
        if (verbose)
            printf("%" PRId64 "\n", i);
        int64_t k = 2*i;
        r = locktree_write_lock(lt, txn_a, k, k, true);
        if (r != 0) {
            assert(r == TOKUDB_OUT_OF_LOCKS);
            break;
        }
        last_i = i;
        r = locktree_write_lock(lt, txn_b, k+1, k+1, true);
        if (r != 0) {
            assert(r == TOKUDB_OUT_OF_LOCKS);
            break;
        }
    }

    // wait for an escalation to occur
    assert(get_escalation_count(mgr) > 0);

    if (last_i != -1) {
        locktree_release_lock(lt, txn_a, 0, 2*last_i);
        locktree_release_lock(lt, txn_b, 0, 2*last_i+1);
    }

    mgr.release_lt(lt);
    mgr.destroy();

    return 0;
}
