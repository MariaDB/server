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

static DBT *expected_a;
static DBT *expected_b;
static DESCRIPTOR expected_descriptor;
static int expected_comparison_magic = 55;

static int my_compare_dbts(DB *db, const DBT *a, const DBT *b) {
    invariant(db->cmp_descriptor == expected_descriptor);
    (void) a; 
    (void) b;
    return expected_comparison_magic;
}

// test that get/set userdata works, and that get_manager() works
void locktree_unit_test::test_misc(void) {
    locktree lt;
    DICTIONARY_ID dict_id = { 1 };
    toku::comparator my_dbt_comparator;
    my_dbt_comparator.create(my_compare_dbts, nullptr);
    lt.create(nullptr, dict_id, my_dbt_comparator);

    invariant(lt.get_userdata() == nullptr);
    int userdata;
    lt.set_userdata(&userdata);
    invariant(lt.get_userdata() == &userdata);
    lt.set_userdata(nullptr);
    invariant(lt.get_userdata() == nullptr);

    int r;
    DBT dbt_a, dbt_b;
    DESCRIPTOR_S d1, d2;
    expected_a = &dbt_a;
    expected_b = &dbt_b;

    toku::comparator cmp_d1, cmp_d2;
    cmp_d1.create(my_compare_dbts, &d1);
    cmp_d2.create(my_compare_dbts, &d2);

    // make sure the comparator object has the correct
    // descriptor when we set the locktree's descriptor
    lt.set_comparator(cmp_d1);
    expected_descriptor = &d1;
    r = lt.m_cmp(&dbt_a, &dbt_b);
    invariant(r == expected_comparison_magic);
    lt.set_comparator(cmp_d2);
    expected_descriptor = &d2;
    r = lt.m_cmp(&dbt_a, &dbt_b);
    invariant(r == expected_comparison_magic);

    lt.release_reference();
    lt.destroy();

    cmp_d1.destroy();
    cmp_d2.destroy();
    my_dbt_comparator.destroy();
}

} /* namespace toku */

int main(void) {
    toku::locktree_unit_test test;
    test.test_misc();
    return 0;
}
