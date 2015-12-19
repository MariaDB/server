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

#include "manager_unit_test.h"
#include "locktree_unit_test.h"
#include "lock_request_unit_test.h"

namespace toku {

static void assert_status(LTM_STATUS ltm_status, const char *keyname, uint64_t v) {
    TOKU_ENGINE_STATUS_ROW key_status = NULL;
    // lookup keyname in status
    for (int i = 0; ; i++) {
        TOKU_ENGINE_STATUS_ROW status = &ltm_status->status[i];
        if (status->keyname == NULL)
            break;
        if (strcmp(status->keyname, keyname) == 0) {
            key_status = status;
            break;
        }
    }
    assert(key_status);
    assert(key_status->value.num == v);
}

void manager_unit_test::test_status(void) {
    locktree_manager mgr;
    mgr.create(nullptr, nullptr, nullptr, nullptr);

    LTM_STATUS_S status;
    mgr.get_status(&status);
    assert_status(&status, "LTM_WAIT_COUNT", 0);
    assert_status(&status, "LTM_TIMEOUT_COUNT", 0);

    DICTIONARY_ID dict_id = { .dictid = 1 };
    locktree *lt = mgr.get_lt(dict_id, dbt_comparator, nullptr);
    int r;
    TXNID txnid_a = 1001;
    TXNID txnid_b = 2001;
    const DBT *one = get_dbt(1);

    // txn a write locks one
    r = lt->acquire_write_lock(txnid_a, one, one, nullptr, false);
    assert(r == 0);

    // txn b tries to write lock one, conflicts, waits, and fails to lock one
    lock_request request_b;
    request_b.create();
    request_b.set(lt, txnid_b, one, one, lock_request::type::WRITE, false);
    r = request_b.start();
    assert(r == DB_LOCK_NOTGRANTED);
    r = request_b.wait(1000);
    assert(r == DB_LOCK_NOTGRANTED);
    request_b.destroy();

    range_buffer buffer;
    buffer.create();
    buffer.append(one, one);
    lt->release_locks(txnid_a, &buffer);
    buffer.destroy();

    assert(lt->m_rangetree->is_empty() && lt->m_sto_buffer.is_empty());

    // assert that wait counters incremented
    mgr.get_status(&status);
    assert_status(&status, "LTM_WAIT_COUNT", 1);
    assert_status(&status, "LTM_TIMEOUT_COUNT", 1);

    // assert that wait counters are persistent after the lock tree is destroyed
    mgr.release_lt(lt);
    mgr.get_status(&status);
    assert_status(&status, "LTM_WAIT_COUNT", 1);
    assert_status(&status, "LTM_TIMEOUT_COUNT", 1);

    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::manager_unit_test test;
    test.test_status();
    return 0;
}
