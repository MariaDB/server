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

#include <stdlib.h>
#include <ft/comparator.h>

static int MAGIC = 49;
static DBT dbt_a;
static DBT dbt_b;
static DESCRIPTOR expected_desc;

static int magic_compare(DB *db, const DBT *a, const DBT *b) {
    invariant(db && a && b);
    invariant(db->cmp_descriptor == expected_desc);
    invariant(a == &dbt_a);
    invariant(b == &dbt_b);
    return MAGIC;
}

static void test_desc(void) {
    int c;
    toku::comparator cmp;
    DESCRIPTOR_S d1, d2;

    // create with d1, make sure it gets used
    cmp.create(magic_compare, &d1);
    expected_desc = &d1;
    c = cmp(&dbt_a, &dbt_b);
    invariant(c == MAGIC);

    // set desc to d2, make sure it gets used
    toku::comparator cmp2;
    cmp2.create(magic_compare, &d2);
    cmp.inherit(cmp2);
    expected_desc = &d2;
    c = cmp(&dbt_a, &dbt_b);
    invariant(c == MAGIC);
    cmp2.destroy();

    // go back to using d1, but using the create_from API
    toku::comparator cmp3, cmp4;
    cmp3.create(magic_compare, &d1); // cmp3 has d1
    cmp4.create_from(cmp3); // cmp4 should get d1 from cmp3
    expected_desc = &d1;
    c = cmp3(&dbt_a, &dbt_b);
    invariant(c == MAGIC);
    c = cmp4(&dbt_a, &dbt_b);
    invariant(c == MAGIC);
    cmp3.destroy();
    cmp4.destroy();

    cmp.destroy();
}

static int dont_compare_me_bro(DB *db, const DBT *a, const DBT *b) {
    abort();
    return db && a && b;
}

static void test_infinity(void) {
    int c;
    toku::comparator cmp;
    cmp.create(dont_compare_me_bro, nullptr);

    // make sure infinity-valued end points compare as expected
    // to an arbitrary (uninitialized!) dbt. the comparison function
    // should never be called and thus the dbt never actually read.
    DBT arbitrary_dbt;

    c = cmp(&arbitrary_dbt, toku_dbt_positive_infinity());
    invariant(c < 0);
    c = cmp(toku_dbt_negative_infinity(), &arbitrary_dbt);
    invariant(c < 0);

    c = cmp(toku_dbt_positive_infinity(), &arbitrary_dbt);
    invariant(c > 0);
    c = cmp(&arbitrary_dbt, toku_dbt_negative_infinity());
    invariant(c > 0);

    c = cmp(toku_dbt_negative_infinity(), toku_dbt_negative_infinity());
    invariant(c == 0);
    c = cmp(toku_dbt_positive_infinity(), toku_dbt_positive_infinity());
    invariant(c == 0);

    cmp.destroy();
}

int main(void) {
    test_desc();
    test_infinity();
    return 0;
}
