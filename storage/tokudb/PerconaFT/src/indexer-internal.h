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

#include <ft/txn/txn_state.h>
#include <toku_pthread.h>

// the indexer_commit_keys is an ordered set of keys described by a DBT in the keys array.
// the array is a resizable array with max size "max_keys" and current size "current_keys".
// the ordered set is used by the hotindex undo function to collect the commit keys.
struct indexer_commit_keys {
    int max_keys;        // max number of keys
    int current_keys;    // number of valid keys
    DBT *keys;           // the variable length keys array
};

// a ule and all of its provisional txn info
// used by the undo-do algorithm to gather up ule provisional info in
// a cursor callback that provides exclusive access to the source DB
// with respect to txn commit and abort
struct ule_prov_info {
    // these are pointers to the allocated leafentry and ule needed to calculate
    // provisional info. we only borrow them - whoever created the provisional info
    // is responsible for cleaning up the leafentry and ule when done.
    LEAFENTRY le;
    ULEHANDLE ule;
    void* key;
    uint32_t keylen;
    // provisional txn info for the ule
    uint32_t num_provisional;
    uint32_t num_committed;
    TXNID *prov_ids;
    TOKUTXN *prov_txns;
    TOKUTXN_STATE *prov_states;
};

struct __toku_indexer_internal {
    DB_ENV *env;
    DB_TXN *txn;
    toku_mutex_t indexer_lock;
    toku_mutex_t indexer_estimate_lock;
    DBT position_estimate;
    DB *src_db;
    int N;
    DB **dest_dbs; /* [N] */
    uint32_t indexer_flags;
    void (*error_callback)(DB *db, int i, int err, DBT *key, DBT *val, void *error_extra);
    void *error_extra;
    int  (*poll_func)(void *poll_extra, float progress);
    void *poll_extra;
    uint64_t estimated_rows; // current estimate of table size
    uint64_t loop_mod;       // how often to call poll_func
    LE_CURSOR lec;
    FILENUM  *fnums; /* [N] */
    FILENUMS filenums;

    // undo state
    struct indexer_commit_keys commit_keys; // set of keys to commit
    DBT_ARRAY *hot_keys;
    DBT_ARRAY *hot_vals;

    // test functions
    int (*undo_do)(DB_INDEXER *indexer, DB *hotdb, DBT* key, ULEHANDLE ule);
    TOKUTXN_STATE (*test_xid_state)(DB_INDEXER *indexer, TXNID xid);
    void (*test_lock_key)(DB_INDEXER *indexer, TXNID xid, DB *hotdb, DBT *key);
    int (*test_delete_provisional)(DB_INDEXER *indexer, DB *hotdb, DBT *hotkey, XIDS xids);
    int (*test_delete_committed)(DB_INDEXER *indexer, DB *hotdb, DBT *hotkey, XIDS xids);
    int (*test_insert_provisional)(DB_INDEXER *indexer, DB *hotdb, DBT *hotkey, DBT *hotval, XIDS xids);
    int (*test_insert_committed)(DB_INDEXER *indexer, DB *hotdb, DBT *hotkey, DBT *hotval, XIDS xids);
    int (*test_commit_any)(DB_INDEXER *indexer, DB *db, DBT *key, XIDS xids);

    // test flags
    int test_only_flags;
};

void indexer_undo_do_init(DB_INDEXER *indexer);

void indexer_undo_do_destroy(DB_INDEXER *indexer);

int indexer_undo_do(DB_INDEXER *indexer, DB *hotdb, struct ule_prov_info *prov_info, DBT_ARRAY *hot_keys, DBT_ARRAY *hot_vals);
