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

void manager_unit_test::test_create_destroy(void) {
    locktree_manager mgr;
    lt_create_cb create_callback = (lt_create_cb) (long) 1;
    lt_destroy_cb destroy_callback = (lt_destroy_cb) (long) 2;
    lt_escalate_cb escalate_callback = (lt_escalate_cb) (long) 3;
    void *extra = (void *) (long) 4;
    mgr.create(create_callback, destroy_callback, escalate_callback, extra);

    invariant(mgr.m_max_lock_memory == locktree_manager::DEFAULT_MAX_LOCK_MEMORY);
    invariant(mgr.m_current_lock_memory == 0);
    invariant(mgr.m_escalation_count == 0);
    invariant(mgr.m_escalation_time == 0);
    invariant(mgr.m_escalation_latest_result == 0);

    invariant(mgr.m_locktree_map.size() == 0);
    invariant(mgr.m_lt_create_callback == create_callback);
    invariant(mgr.m_lt_destroy_callback == destroy_callback);
    invariant(mgr.m_lt_escalate_callback == escalate_callback);
    invariant(mgr.m_lt_escalate_callback_extra == extra);

    mgr.mutex_lock();
    mgr.mutex_unlock();

    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::manager_unit_test test;
    test.test_create_destroy();
    return 0; 
}
