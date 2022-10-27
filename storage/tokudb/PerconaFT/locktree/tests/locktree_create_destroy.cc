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

#include "locktree_unit_test.h"

namespace toku {

// test simple create and destroy of the locktree
void locktree_unit_test::test_create_destroy(void) {
    locktree lt;
    DICTIONARY_ID dict_id = { 1 };

    lt.create(nullptr, dict_id, dbt_comparator);

    lt_lock_request_info *info = lt.get_lock_request_info();
    invariant_notnull(info);
    toku_mutex_lock(&info->mutex);
    toku_mutex_unlock(&info->mutex);

    invariant(lt.m_dict_id.dictid == dict_id.dictid);
    invariant(lt.m_reference_count == 1);
    invariant(lt.m_rangetree != nullptr);
    invariant(lt.m_userdata == nullptr);
    invariant(info->pending_lock_requests.size() == 0);
    invariant(lt.m_sto_end_early_count == 0);
    invariant(lt.m_sto_end_early_time == 0);

    lt.release_reference();
    lt.destroy();
}

} /* namespace toku */

int main(void) {
    toku::locktree_unit_test test;
    test.test_create_destroy();
    return 0;
}
