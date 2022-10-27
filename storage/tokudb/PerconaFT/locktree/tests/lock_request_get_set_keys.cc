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

#include "lock_request_unit_test.h"

namespace toku {

// make setting keys and getting them back works properly.
// at a high level, we want to make sure keys are copied
// when appropriate and plays nice with +/- infinity.
void lock_request_unit_test::test_get_set_keys(void) {
    lock_request request;
    request.create();

    locktree *const null_lt = nullptr;

    TXNID txnid_a = 1001;

    const DBT *one = get_dbt(1);
    const DBT *two = get_dbt(2);
    const DBT *neg_inf = toku_dbt_negative_infinity();
    const DBT *pos_inf = toku_dbt_negative_infinity();

    // request should not copy dbts for neg/pos inf, so get_left
    // and get_right should return the same pointer given
    request.set(null_lt, txnid_a, neg_inf, pos_inf, lock_request::type::WRITE, false);
    invariant(request.get_left_key() == neg_inf);
    invariant(request.get_right_key() == pos_inf);

    // request should make copies of non-infinity-valued keys.
    request.set(null_lt, txnid_a, neg_inf, one, lock_request::type::WRITE, false);
    invariant(request.get_left_key() == neg_inf);
    invariant(request.get_right_key() == one);

    request.set(null_lt, txnid_a, two, pos_inf, lock_request::type::WRITE, false);
    invariant(request.get_left_key() == two);
    invariant(request.get_right_key() == pos_inf);

    request.set(null_lt, txnid_a, one, two, lock_request::type::WRITE, false);
    invariant(request.get_left_key() == one);
    invariant(request.get_right_key() == two);

    request.destroy();
}

}

int main(void) {
    toku::lock_request_unit_test test;
    test.test_get_set_keys();
    return 0;
}

