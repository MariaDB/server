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

#include "test.h"

static DB_TXN *txn1, *txn2, *txn3;
static uint64_t txnid1, txnid2, txnid3;

struct iterate_extra {
    iterate_extra() : n(0) {
        visited_txn[0] = false;
        visited_txn[1] = false;
        visited_txn[2] = false;
    }
    int n;
    bool visited_txn[3];
};

static int iterate_callback(DB_TXN *txn,
                            iterate_row_locks_callback iterate_locks,
                            void *locks_extra, void *extra) {
    uint64_t txnid = txn->id64(txn);
    uint64_t client_id; void *client_extra;
    txn->get_client_id(txn, &client_id, &client_extra);
    iterate_extra *info = reinterpret_cast<iterate_extra *>(extra);
    DB *db;
    DBT left_key, right_key;
    int r = iterate_locks(&db, &left_key, &right_key, locks_extra);
    invariant(r == DB_NOTFOUND);
    if (txnid == txnid1) {
        assert(!info->visited_txn[0]);
        invariant(client_id == 0);
        info->visited_txn[0] = true;
    } else if (txnid == txnid2) {
        assert(!info->visited_txn[1]);
        invariant(client_id == 1);
        info->visited_txn[1] = true;
    } else if (txnid == txnid3) {
        assert(!info->visited_txn[2]);
        invariant(client_id == 2);
        info->visited_txn[2] = true;
    }
    info->n++;
    return 0;
}

int test_main(int UU(argc), char *const UU(argv[])) {
    int r;
    const int env_flags = DB_INIT_MPOOL | DB_CREATE | DB_THREAD |
        DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN | DB_PRIVATE;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, 0755); CKERR(r);

    DB_ENV *env;
    r = db_env_create(&env, 0); CKERR(r);
    r = env->iterate_live_transactions(env, iterate_callback, NULL);
    assert(r == EINVAL);
    r = env->open(env, TOKU_TEST_FILENAME, env_flags, 0755); CKERR(r);

    r = env->txn_begin(env, NULL, &txn1, 0); CKERR(r);
    txn1->set_client_id(txn1, 0, nullptr);
    txnid1 = txn1->id64(txn1);
    r = env->txn_begin(env, NULL, &txn2, 0); CKERR(r);
    txn2->set_client_id(txn2, 1, nullptr);
    txnid2 = txn2->id64(txn2);
    r = env->txn_begin(env, NULL, &txn3, 0); CKERR(r);
    txn3->set_client_id(txn3, 2, nullptr);
    txnid3 = txn3->id64(txn3);

    {
        iterate_extra e;
        r = env->iterate_live_transactions(env, iterate_callback, &e); CKERR(r);
        assert(e.visited_txn[0]);
        assert(e.visited_txn[1]);
        assert(e.visited_txn[2]);
        assert(e.n == 3);
    }

    r = txn1->commit(txn1, 0); CKERR(r);
    r = txn2->abort(txn2); CKERR(r);
    {
        iterate_extra e;
        r = env->iterate_live_transactions(env, iterate_callback, &e); CKERR(r);
        assert(!e.visited_txn[0]);
        assert(!e.visited_txn[1]);
        assert(e.visited_txn[2]);
        assert(e.n == 1);
    }

    r = txn3->commit(txn3, 0); CKERR(r);
    {
        iterate_extra e;
        r = env->iterate_live_transactions(env, iterate_callback, &e); CKERR(r);
        assert(!e.visited_txn[0]);
        assert(!e.visited_txn[1]);
        assert(!e.visited_txn[2]);
        assert(e.n == 0);
    }

    r = env->close(env, 0); CKERR(r);
    return 0;
}
