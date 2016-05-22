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

namespace toku {

void manager_unit_test::test_params(void) {
    int r;
    locktree_manager mgr;
    mgr.create(nullptr, nullptr, nullptr, nullptr);

    uint64_t new_max_lock_memory = 15307752356;
    r = mgr.set_max_lock_memory(new_max_lock_memory);
    invariant(r == 0);
    invariant(mgr.get_max_lock_memory() == new_max_lock_memory);

    mgr.m_current_lock_memory = 100000;
    r = mgr.set_max_lock_memory(mgr.m_current_lock_memory - 1);
    invariant(r == EDOM);
    invariant(mgr.get_max_lock_memory() == new_max_lock_memory);

    mgr.m_current_lock_memory = 0;
    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::manager_unit_test test;
    test.test_params();
    return 0; 
}
