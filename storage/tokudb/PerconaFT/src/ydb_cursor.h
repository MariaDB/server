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

// This file defines the public interface to the ydb library

typedef enum {
    YDB_C_LAYER_STATUS_NUM_ROWS = 0             /* number of rows in this status array */
} ydb_c_lock_layer_status_entry;

typedef struct {
    bool initialized;
    TOKU_ENGINE_STATUS_ROW_S status[YDB_C_LAYER_STATUS_NUM_ROWS];
} YDB_C_LAYER_STATUS_S, *YDB_C_LAYER_STATUS;

void ydb_c_layer_get_status(YDB_C_LAYER_STATUS statp);

int toku_c_get(DBC * c, DBT * key, DBT * data, uint32_t flag);
int toku_c_getf_set(DBC *c, uint32_t flag, DBT *key, YDB_CALLBACK_FUNCTION f, void *extra);

int toku_db_cursor(DB *db, DB_TXN *txn, DBC **c, uint32_t flags);
int toku_db_cursor_internal(DB *db, DB_TXN * txn, DBC *c, uint32_t flags, int is_temporary_cursor);

int toku_c_close(DBC *c);
int toku_c_close_internal(DBC *c);
