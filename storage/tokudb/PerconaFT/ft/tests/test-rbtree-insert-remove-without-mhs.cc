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

#define N 1000000
std::vector<MhsRbTree::Node::BlockPair> input_vector;
MhsRbTree::Node::BlockPair old_vector[N];

static int myrandom(int i) { return std::rand() % i; }

static void generate_random_input() {
    std::srand(unsigned(std::time(0)));

    // set some values:
    for (uint64_t i = 0; i < N; ++i) {
        MhsRbTree::Node::BlockPair bp = {i+1, 0};
        input_vector.push_back(bp);
        old_vector[i] = bp;
    }
    // using built-in random generator:
    std::random_shuffle(input_vector.begin(), input_vector.end(), myrandom);
}

static void test_insert_remove(void) {
    int i;
    MhsRbTree::Tree *tree = new MhsRbTree::Tree();
    verbose = 0;
    generate_random_input();
    if (verbose) {
        printf("\n we are going to insert the following block offsets\n");
        for (i = 0; i < N; i++)
            printf("%" PRIu64 "\t", input_vector[i]._offset.ToInt());
    }
    for (i = 0; i < N; i++) {
        tree->Insert(input_vector[i]);
        // tree->ValidateBalance();
    }
    tree->ValidateBalance();
    MhsRbTree::Node::BlockPair *p_bps = &old_vector[0];
    tree->ValidateInOrder(p_bps);
    printf("min node of the tree:%" PRIu64 "\n",
           rbn_offset(tree->MinNode()).ToInt());
    printf("max node of the tree:%" PRIu64 "\n",
           rbn_offset(tree->MaxNode()).ToInt());

    for (i = 0; i < N; i++) {
        // tree->ValidateBalance();
        tree->RawRemove(input_vector[i]._offset.ToInt());
    }

    tree->Destroy();
    delete tree;
}

int test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);

    test_insert_remove();
    if (verbose)
        printf("test ok\n");
    return 0;
}
