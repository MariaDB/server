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

#include "ft/serialize/rbtree_mhs.h"
#include "test.h"
#include <algorithm>
#include <vector>
#include <ctime>
#include <cstdlib>

static void test_insert_remove(void) {
    uint64_t i;
    MhsRbTree::Tree *tree = new MhsRbTree::Tree();
    verbose = 0;

    tree->Insert({0, 100});

    for (i = 0; i < 10; i++) {
        tree->Remove(3);
        tree->Remove(2);
    }
    tree->ValidateBalance();
    tree->ValidateMhs();

    for (i = 0; i < 10; i++) {
        tree->Insert({5 * i, 3});
    }
    tree->ValidateBalance();
    tree->ValidateMhs();

    uint64_t offset = tree->Remove(2);
    invariant(offset == 0);
    offset = tree->Remove(10);
    invariant(offset == 50);
    offset = tree->Remove(3);
    invariant(offset == 5);
    tree->ValidateBalance();
    tree->ValidateMhs();

    tree->Insert({48, 2});
    tree->Insert({50, 10});

    tree->ValidateBalance();
    tree->ValidateMhs();

    tree->Insert({3, 7});
    offset = tree->Remove(10);
    invariant(offset == 2);
    tree->ValidateBalance();
    tree->ValidateMhs();
    tree->Dump();
    delete tree;
}

int test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);

    test_insert_remove();
    if (verbose)
        printf("test ok\n");
    return 0;
}
