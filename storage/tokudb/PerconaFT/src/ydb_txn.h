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

// begin, commit, and abort use the multi operation lock 
// internally to synchronize with begin checkpoint. callers
// should not hold the multi operation lock.

int toku_txn_begin(DB_ENV *env, DB_TXN * stxn, DB_TXN ** txn, uint32_t flags);

void toku_txn_note_db_row_lock(DB_TXN *txn, DB *db, const DBT *left_key, const DBT *right_key);

int locked_txn_commit(DB_TXN *txn, uint32_t flags);

int locked_txn_abort(DB_TXN *txn);

void toku_keep_prepared_txn_callback(DB_ENV *env, TOKUTXN tokutxn);

bool toku_is_big_txn(DB_TXN *txn);
bool toku_is_big_tokutxn(TOKUTXN tokutxn);

// Test-only function
extern "C" void toku_increase_last_xid(DB_ENV *env, uint64_t increment) __attribute__((__visibility__("default")));
