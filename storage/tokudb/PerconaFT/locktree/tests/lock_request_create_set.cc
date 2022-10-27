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

// create and set the object's internals, destroy should not crash.
void lock_request_unit_test::test_create_destroy(void) {
    lock_request request;
    request.create();

    invariant(request.m_txnid == TXNID_NONE);
    invariant(request.m_left_key == nullptr);
    invariant(request.m_right_key == nullptr);
    invariant(request.m_left_key_copy.flags == 0);
    invariant(request.m_left_key_copy.data == nullptr);
    invariant(request.m_right_key_copy.flags == 0);
    invariant(request.m_right_key_copy.data == nullptr);

    invariant(request.m_type == lock_request::type::UNKNOWN);
    invariant(request.m_lt == nullptr);

    invariant(request.m_complete_r == 0);
    invariant(request.m_state == lock_request::state::UNINITIALIZED);

    request.destroy();
}

}

int main(void) {
    toku::lock_request_unit_test test;
    test.test_create_destroy();
    return 0;
}

