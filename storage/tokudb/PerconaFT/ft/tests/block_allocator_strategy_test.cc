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

#include "ft/tests/test.h"

#include "ft/serialize/block_allocator_strategy.h"

static const uint64_t alignment = 4096;

static void test_first_vs_best_fit(void) {
    struct block_allocator::blockpair pairs[] = {
        block_allocator::blockpair(1 * alignment, 6 * alignment),
        // hole between 7x align -> 8x align
        block_allocator::blockpair(8 * alignment, 4 * alignment),
        // hole between 12x align -> 16x align
        block_allocator::blockpair(16 * alignment, 1 * alignment),
        block_allocator::blockpair(17 * alignment, 2 * alignment),
        // hole between 19 align -> 21x align
        block_allocator::blockpair(21 * alignment, 2 * alignment),
    };
    const uint64_t n_blocks = sizeof(pairs) / sizeof(pairs[0]);
    
    block_allocator::blockpair *bp;

    // first fit
    bp = block_allocator_strategy::first_fit(pairs, n_blocks, 100, alignment);
    assert(bp == &pairs[0]);
    bp = block_allocator_strategy::first_fit(pairs, n_blocks, 4096, alignment);
    assert(bp == &pairs[0]);
    bp = block_allocator_strategy::first_fit(pairs, n_blocks, 3 * 4096, alignment);
    assert(bp == &pairs[1]);
    bp = block_allocator_strategy::first_fit(pairs, n_blocks, 5 * 4096, alignment);
    assert(bp == nullptr);

    // best fit
    bp = block_allocator_strategy::best_fit(pairs, n_blocks, 100, alignment);
    assert(bp == &pairs[0]);
    bp = block_allocator_strategy::best_fit(pairs, n_blocks, 4100, alignment);
    assert(bp == &pairs[3]);
    bp = block_allocator_strategy::best_fit(pairs, n_blocks, 3 * 4096, alignment);
    assert(bp == &pairs[1]);
    bp = block_allocator_strategy::best_fit(pairs, n_blocks, 5 * 4096, alignment);
    assert(bp == nullptr);
}

static void test_padded_fit(void) {
    struct block_allocator::blockpair pairs[] = {
        block_allocator::blockpair(1 * alignment, 1 * alignment),
        // 4096 byte hole after bp[0]
        block_allocator::blockpair(3 * alignment, 1 * alignment),
        // 8192 byte hole after bp[1]
        block_allocator::blockpair(6 * alignment, 1 * alignment),
        // 16384 byte hole after bp[2]
        block_allocator::blockpair(11 * alignment, 1 * alignment),
        // 32768 byte hole after bp[3]
        block_allocator::blockpair(17 * alignment, 1 * alignment),
        // 116kb hole after bp[4]
        block_allocator::blockpair(113 * alignment, 1 * alignment),
        // 256kb hole after bp[5]
        block_allocator::blockpair(371 * alignment, 1 * alignment),
    };
    const uint64_t n_blocks = sizeof(pairs) / sizeof(pairs[0]);
    
    block_allocator::blockpair *bp;

    // padding for a 100 byte allocation will be < than standard alignment,
    // so it should fit in the first 4096 byte hole.
    bp = block_allocator_strategy::padded_fit(pairs, n_blocks, 4000, alignment);
    assert(bp == &pairs[0]);

    // Even padded, a 12kb alloc will fit in a 16kb hole
    bp = block_allocator_strategy::padded_fit(pairs, n_blocks, 3 * alignment, alignment);
    assert(bp == &pairs[2]);

    // would normally fit in the 116kb hole but the padding will bring it over
    bp = block_allocator_strategy::padded_fit(pairs, n_blocks, 116 * alignment, alignment);
    assert(bp == &pairs[5]);

    bp = block_allocator_strategy::padded_fit(pairs, n_blocks, 127 * alignment, alignment);
    assert(bp == &pairs[5]);
}

int test_main(int argc, const char *argv[]) {
    (void) argc;
    (void) argv;

    test_first_vs_best_fit();
    test_padded_fit();

    return 0;
}
