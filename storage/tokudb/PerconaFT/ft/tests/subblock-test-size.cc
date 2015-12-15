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

// test the choose sub block size function

#include <toku_portability.h>
#include "test.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "serialize/sub_block.h"

static void
test_sub_block_size(int total_size) {
    if (verbose)
        printf("%s:%d %d\n", __FUNCTION__, __LINE__, total_size);
    int r;
    int sub_block_size, n_sub_blocks;
    r = choose_sub_block_size(total_size, 0, &sub_block_size, &n_sub_blocks);
    assert(r == EINVAL);
    for (int i = 1; i < max_sub_blocks; i++) {
        r = choose_sub_block_size(total_size, i, &sub_block_size, &n_sub_blocks);
        assert(r == 0);
        assert(0 <= n_sub_blocks && n_sub_blocks <= i);
        assert(total_size <= n_sub_blocks * sub_block_size);
    }
}

int
test_main (int argc, const char *argv[]) {
    int i;
    for (i=1; i<argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-v") == 0)
            verbose++;
    }
    test_sub_block_size(0);
    for (int total_size = 1; total_size <= 4*1024*1024; total_size *= 2) {
        test_sub_block_size(total_size);
    }
    return 0;
}
