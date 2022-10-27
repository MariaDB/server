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

void manager_unit_test::test_lt_map(void) {
    locktree_manager mgr;
    mgr.create(nullptr, nullptr, nullptr, nullptr);

    locktree aa;
    locktree bb;
    locktree cc;
    locktree *alt = &aa;
    locktree *blt = &bb;
    locktree *clt = &cc;
    DICTIONARY_ID a = { 1 };
    DICTIONARY_ID b = { 2 };
    DICTIONARY_ID c = { 3 };
    DICTIONARY_ID d = { 4 };
    alt->m_dict_id = a;
    blt->m_dict_id = b;
    clt->m_dict_id = c;

    mgr.locktree_map_put(alt);
    mgr.locktree_map_put(blt);
    mgr.locktree_map_put(clt);

    locktree *lt;

    lt = mgr.locktree_map_find(a);
    invariant(lt == alt);
    lt = mgr.locktree_map_find(c);
    invariant(lt == clt);
    lt = mgr.locktree_map_find(b);
    invariant(lt == blt);

    mgr.locktree_map_remove(alt);
    lt = mgr.locktree_map_find(a);
    invariant(lt == nullptr);
    lt = mgr.locktree_map_find(c);
    invariant(lt == clt);
    lt = mgr.locktree_map_find(b);
    invariant(lt == blt);
    lt = mgr.locktree_map_find(d);
    invariant(lt == nullptr);

    mgr.locktree_map_remove(clt);
    mgr.locktree_map_remove(blt);
    lt = mgr.locktree_map_find(c);
    invariant(lt == nullptr);
    lt = mgr.locktree_map_find(b);
    invariant(lt == nullptr);

    mgr.destroy();
}

} /* namespace toku */

int main(void) {
    toku::manager_unit_test test;
    test.test_lt_map();
    return 0; 
}
