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

#include <ydb-internal.h>

#include <locktree/lock_request.h>

// Expose the escalate callback to ydb.cc,
// so it can pass the function pointer to the locktree
void toku_db_txn_escalate_callback(TXNID txnid, const toku::locktree *lt, const toku::range_buffer &buffer, void *extra);

int toku_db_get_range_lock(DB *db, DB_TXN *txn, const DBT *left_key, const DBT *right_key,
        toku::lock_request::type lock_type);

int toku_db_start_range_lock(DB *db, DB_TXN *txn, const DBT *left_key, const DBT *right_key,
        toku::lock_request::type lock_type, toku::lock_request *lock_request);

int toku_db_wait_range_lock(DB *db, DB_TXN *txn, toku::lock_request *lock_request);

int toku_db_get_point_write_lock(DB *db, DB_TXN *txn, const DBT *key);

void toku_db_grab_write_lock(DB *db, DBT *key, TOKUTXN tokutxn);

void toku_db_release_lt_key_ranges(DB_TXN *txn, txn_lt_key_ranges *ranges);
