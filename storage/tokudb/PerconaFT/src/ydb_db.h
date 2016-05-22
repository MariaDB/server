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

#include <ft/ft.h>

#include "ydb-internal.h"
#include "ydb_txn.h"

typedef enum {
    YDB_LAYER_DIRECTORY_WRITE_LOCKS = 0,        /* total directory write locks taken */
    YDB_LAYER_DIRECTORY_WRITE_LOCKS_FAIL,   /* total directory write locks unable to be taken */
    YDB_LAYER_LOGSUPPRESS,                  /* number of times logs are suppressed for empty table (2440) */
    YDB_LAYER_LOGSUPPRESS_FAIL,             /* number of times unable to suppress logs for empty table (2440) */
    YDB_DB_LAYER_STATUS_NUM_ROWS              /* number of rows in this status array */
} ydb_db_lock_layer_status_entry;

typedef struct {
    bool initialized;
    TOKU_ENGINE_STATUS_ROW_S status[YDB_DB_LAYER_STATUS_NUM_ROWS];
} YDB_DB_LAYER_STATUS_S, *YDB_DB_LAYER_STATUS;

void ydb_db_layer_get_status(YDB_DB_LAYER_STATUS statp);

//
// export the following locktree create/destroy callbacks so
// the environment can pass them to the locktree manager.
//
struct lt_on_create_callback_extra {
    DB_TXN *txn;
    FT_HANDLE ft_handle;
};
int toku_db_lt_on_create_callback(toku::locktree *lt, void *extra);
void toku_db_lt_on_destroy_callback(toku::locktree *lt);

/* db methods */
static inline int db_opened(DB *db) {
    return db->i->opened != 0;
}

static inline const toku::comparator &toku_db_get_comparator(DB *db) {
    return toku_ft_get_comparator(db->i->ft_handle);
}

int toku_db_use_builtin_key_cmp(DB *db);
int toku_db_pre_acquire_fileops_lock(DB *db, DB_TXN *txn);
int toku_db_open_iname(DB * db, DB_TXN * txn, const char *iname, uint32_t flags, int mode);
int toku_db_pre_acquire_table_lock(DB *db, DB_TXN *txn);
int toku_db_get (DB * db, DB_TXN * txn, DBT * key, DBT * data, uint32_t flags);
int toku_db_create(DB ** db, DB_ENV * env, uint32_t flags);
int toku_db_close(DB * db);
int toku_setup_db_internal (DB **dbp, DB_ENV *env, uint32_t flags, FT_HANDLE ft_handle, bool is_open);
int db_getf_set(DB *db, DB_TXN *txn, uint32_t flags, DBT *key, YDB_CALLBACK_FUNCTION f, void *extra);
int autotxn_db_get(DB* db, DB_TXN* txn, DBT* key, DBT* data, uint32_t flags);

//TODO: DB_AUTO_COMMIT.
//TODO: Nowait only conditionally?
//TODO: NOSYNC change to SYNC if DB_ENV has something in set_flags
static inline int 
toku_db_construct_autotxn(DB* db, DB_TXN **txn, bool* changed, bool force_auto_commit) {
    assert(db && txn && changed);
    DB_ENV* env = db->dbenv;
    if (*txn || !(env->i->open_flags & DB_INIT_TXN)) {
        *changed = false;
        return 0;
    }
    bool nosync = (bool)(!force_auto_commit && !(env->i->open_flags & DB_AUTO_COMMIT));
    uint32_t txn_flags = DB_TXN_NOWAIT | (nosync ? DB_TXN_NOSYNC : 0);
    int r = toku_txn_begin(env, NULL, txn, txn_flags);
    if (r!=0) return r;
    *changed = true;
    return 0;
}

static inline int 
toku_db_destruct_autotxn(DB_TXN *txn, int r, bool changed) {
    if (!changed) return r;
    if (r==0) {
        r = locked_txn_commit(txn, 0);
    }
    else {
        locked_txn_abort(txn);
    }
    return r; 
}
