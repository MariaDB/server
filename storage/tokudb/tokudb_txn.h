/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#ifndef _TOKUDB_TXN_H
#define _TOKUDB_TXN_H

#include "hatoku_defines.h"
#include "tokudb_debug.h"
#include "tokudb_sysvars.h"

typedef enum {
    hatoku_iso_not_set = 0,
    hatoku_iso_read_uncommitted,
    hatoku_iso_read_committed,
    hatoku_iso_repeatable_read,
    hatoku_iso_serializable
} HA_TOKU_ISO_LEVEL;

typedef struct st_tokudb_stmt_progress {
    ulonglong inserted;
    ulonglong updated;
    ulonglong deleted;
    ulonglong queried;
    bool using_loader;
} tokudb_stmt_progress;

typedef struct st_tokudb_trx_data {
    DB_TXN* all;
    DB_TXN* stmt;
    DB_TXN* sp_level;
    DB_TXN* sub_sp_level;
    uint tokudb_lock_count;
    uint create_lock_count;
    tokudb_stmt_progress stmt_progress;
    bool checkpoint_lock_taken;
    LIST* handlers;
} tokudb_trx_data;

extern char* tokudb_data_dir;
extern const char* ha_tokudb_ext;

inline void reset_stmt_progress(tokudb_stmt_progress* val) {
    val->deleted = 0;
    val->inserted = 0;
    val->updated = 0;
    val->queried = 0;
}

inline int get_name_length(const char* name) {
    int n = 0;
    const char* newname = name;
    n += strlen(newname);
    n += strlen(ha_tokudb_ext);
    return n;
}

//
// returns maximum length of path to a dictionary
//
inline int get_max_dict_name_path_length(const char* tablename) {
    int n = 0;
    n += get_name_length(tablename);
    n += 1; //for the '-'
    n += MAX_DICT_NAME_LEN;
    return n;
}

inline void make_name(
    char* newname,
    size_t newname_len,
    const char* tablename,
    const char* dictname) {

    assert_always(tablename);
    assert_always(dictname);
    size_t real_size = snprintf(
        newname,
        newname_len,
        "%s-%s",
        tablename,
        dictname);
    assert_always(real_size < newname_len);
}

inline int txn_begin(
    DB_ENV* env,
    DB_TXN* parent,
    DB_TXN** txn,
    uint32_t flags,
    THD* thd) {

    *txn = NULL;
    int r = env->txn_begin(env, parent, txn, flags);
    if (r == 0 && thd) {
        DB_TXN* this_txn = *txn;
        this_txn->set_client_id(this_txn, thd_get_thread_id(thd), thd);
    }
    TOKUDB_TRACE_FOR_FLAGS(
        TOKUDB_DEBUG_TXN,
        "begin txn %p %p %u r=%d",
        parent,
        *txn,
        flags,
        r);
    return r;
}

inline void commit_txn(DB_TXN* txn, uint32_t flags) {
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_TXN, "commit txn %p", txn);
    int r = txn->commit(txn, flags);
    if (r != 0) {
        sql_print_error(
            "tried committing transaction %p and got error code %d",
            txn,
            r);
    }
    assert_always(r == 0);
}

inline void abort_txn(DB_TXN* txn) {
    TOKUDB_TRACE_FOR_FLAGS(TOKUDB_DEBUG_TXN, "abort txn %p", txn);
    int r = txn->abort(txn);
    if (r != 0) {
        sql_print_error(
            "tried aborting transaction %p and got error code %d",
            txn,
            r);
    }
    assert_always(r == 0);
}

#endif // _TOKUDB_TXN_H
