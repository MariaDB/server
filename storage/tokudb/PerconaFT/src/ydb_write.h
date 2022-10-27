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

typedef enum {
    YDB_LAYER_NUM_INSERTS = 0,
    YDB_LAYER_NUM_INSERTS_FAIL,
    YDB_LAYER_NUM_DELETES,
    YDB_LAYER_NUM_DELETES_FAIL,
    YDB_LAYER_NUM_UPDATES,
    YDB_LAYER_NUM_UPDATES_FAIL,
    YDB_LAYER_NUM_UPDATES_BROADCAST,
    YDB_LAYER_NUM_UPDATES_BROADCAST_FAIL,
    YDB_LAYER_NUM_MULTI_INSERTS,
    YDB_LAYER_NUM_MULTI_INSERTS_FAIL,
    YDB_LAYER_NUM_MULTI_DELETES,
    YDB_LAYER_NUM_MULTI_DELETES_FAIL,
    YDB_LAYER_NUM_MULTI_UPDATES,
    YDB_LAYER_NUM_MULTI_UPDATES_FAIL,
    YDB_WRITE_LAYER_STATUS_NUM_ROWS              /* number of rows in this status array */
} ydb_write_lock_layer_status_entry;

typedef struct {
    bool initialized;
    TOKU_ENGINE_STATUS_ROW_S status[YDB_WRITE_LAYER_STATUS_NUM_ROWS];
} YDB_WRITE_LAYER_STATUS_S, *YDB_WRITE_LAYER_STATUS;

void ydb_write_layer_get_status(YDB_WRITE_LAYER_STATUS statp);

int toku_db_del(DB *db, DB_TXN *txn, DBT *key, uint32_t flags, bool holds_mo_lock);
int toku_db_put(DB *db, DB_TXN *txn, DBT *key, DBT *val, uint32_t flags, bool holds_mo_lock);
int autotxn_db_del(DB* db, DB_TXN* txn, DBT* key, uint32_t flags);
int autotxn_db_put(DB* db, DB_TXN* txn, DBT* key, DBT* data, uint32_t flags);
int autotxn_db_update(DB *db, DB_TXN *txn, const DBT *key, const DBT *update_function_extra, uint32_t flags);
int autotxn_db_update_broadcast(DB *db, DB_TXN *txn, const DBT *update_function_extra, uint32_t flags);
int env_put_multiple(
    DB_ENV *env,
    DB *src_db,
    DB_TXN *txn,
    const DBT *src_key, const DBT *src_val,
    uint32_t num_dbs,
    DB **db_array,
    DBT_ARRAY *keys, DBT_ARRAY *vals,
    uint32_t *flags_array
    );
int env_del_multiple(
    DB_ENV *env,
    DB *src_db,
    DB_TXN *txn,
    const DBT *src_key,
    const DBT *src_val,
    uint32_t num_dbs,
    DB **db_array,
    DBT_ARRAY *keys,
    uint32_t *flags_array
    );
int env_update_multiple(
    DB_ENV *env,
    DB *src_db,
    DB_TXN *txn,
    DBT *old_src_key, DBT *old_src_data,
    DBT *new_src_key, DBT *new_src_data,
    uint32_t num_dbs,
    DB **db_array,
    uint32_t* flags_array,
    uint32_t num_keys, DBT_ARRAY keys[],
    uint32_t num_vals, DBT_ARRAY vals[]
    );
