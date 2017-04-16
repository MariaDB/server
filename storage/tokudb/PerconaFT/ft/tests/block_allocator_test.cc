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

#include "test.h"

static void ba_alloc(BlockAllocator *ba, uint64_t size, uint64_t *answer) {
    ba->Validate();
    uint64_t actual_answer;
    ba->AllocBlock(512 * size, &actual_answer);
    ba->Validate();

    invariant(actual_answer % 512 == 0);
    *answer = actual_answer / 512;
}

static void ba_free(BlockAllocator *ba, uint64_t offset, uint64_t size) {
    ba->Validate();
    ba->FreeBlock(offset * 512, 512 * size);
    ba->Validate();
}

static void ba_check_l(BlockAllocator *ba,
                       uint64_t blocknum_in_layout_order,
                       uint64_t expected_offset,
                       uint64_t expected_size) {
    uint64_t actual_offset, actual_size;
    int r = ba->NthBlockInLayoutOrder(
        blocknum_in_layout_order, &actual_offset, &actual_size);
    invariant(r == 0);
    invariant(expected_offset * 512 == actual_offset);
    invariant(expected_size * 512 == actual_size);
}

static void ba_check_none(BlockAllocator *ba,
                          uint64_t blocknum_in_layout_order) {
    uint64_t actual_offset, actual_size;
    int r = ba->NthBlockInLayoutOrder(
        blocknum_in_layout_order, &actual_offset, &actual_size);
    invariant(r == -1);
}

// Simple block allocator test
static void test_ba0() {
    BlockAllocator allocator;
    BlockAllocator *ba = &allocator;
    ba->Create(100 * 512, 1 * 512);
    invariant(ba->AllocatedLimit() == 100 * 512);

    uint64_t b2, b3, b4, b5, b6, b7;
    ba_alloc(ba, 100, &b2);
    ba_alloc(ba, 100, &b3);
    ba_alloc(ba, 100, &b4);
    ba_alloc(ba, 100, &b5);
    ba_alloc(ba, 100, &b6);
    ba_alloc(ba, 100, &b7);
    ba_free(ba, b2, 100);
    ba_alloc(ba, 100, &b2);
    ba_free(ba, b4, 100);
    ba_free(ba, b6, 100);
    uint64_t b8, b9;
    ba_alloc(ba, 100, &b4);
    ba_free(ba, b2, 100);
    ba_alloc(ba, 100, &b6);
    ba_alloc(ba, 100, &b8);
    ba_alloc(ba, 100, &b9);
    ba_free(ba, b6, 100);
    ba_free(ba, b7, 100);
    ba_free(ba, b8, 100);
    ba_alloc(ba, 100, &b6);
    ba_alloc(ba, 100, &b7);
    ba_free(ba, b4, 100);
    ba_alloc(ba, 100, &b4);

    ba->Destroy();
}

// Manually to get coverage of all the code in the block allocator.
static void test_ba1(int n_initial) {
    BlockAllocator allocator;
    BlockAllocator *ba = &allocator;
    ba->Create(0 * 512, 1 * 512);

    int n_blocks = 0;
    uint64_t blocks[1000];
    for (int i = 0; i < 1000; i++) {
        if (i < n_initial || random() % 2 == 0) {
            if (n_blocks < 1000) {
                ba_alloc(ba, 1, &blocks[n_blocks]);
                // printf("A[%d]=%ld\n", n_blocks, blocks[n_blocks]);
                n_blocks++;
            }
        } else {
            if (n_blocks > 0) {
                int blocknum = random() % n_blocks;
                // printf("F[%d]=%ld\n", blocknum, blocks[blocknum]);
                ba_free(ba, blocks[blocknum], 1);
                blocks[blocknum] = blocks[n_blocks - 1];
                n_blocks--;
            }
        }
    }

    ba->Destroy();
}

// Check to see if it is first fit or best fit.
static void test_ba2(void) {
    BlockAllocator allocator;
    BlockAllocator *ba = &allocator;
    uint64_t b[6];
    enum { BSIZE = 1024 };
    ba->Create(100 * 512, BSIZE * 512);
    invariant(ba->AllocatedLimit() == 100 * 512);

    ba_check_l(ba, 0, 0, 100);
    ba_check_none(ba, 1);

    ba_alloc(ba, 100, &b[0]);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_none(ba, 2);

    ba_alloc(ba, BSIZE + 100, &b[1]);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_l(ba, 2, 2 * BSIZE, BSIZE + 100);
    ba_check_none(ba, 3);

    ba_alloc(ba, 100, &b[2]);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_l(ba, 2, 2 * BSIZE, BSIZE + 100);
    ba_check_l(ba, 3, 4 * BSIZE, 100);
    ba_check_none(ba, 4);

    ba_alloc(ba, 100, &b[3]);
    ba_alloc(ba, 100, &b[4]);
    ba_alloc(ba, 100, &b[5]);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_l(ba, 2, 2 * BSIZE, BSIZE + 100);
    ba_check_l(ba, 3, 4 * BSIZE, 100);
    ba_check_l(ba, 4, 5 * BSIZE, 100);
    ba_check_l(ba, 5, 6 * BSIZE, 100);
    ba_check_l(ba, 6, 7 * BSIZE, 100);
    ba_check_none(ba, 7);

    ba_free(ba, 4 * BSIZE, 100);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_l(ba, 2, 2 * BSIZE, BSIZE + 100);
    ba_check_l(ba, 3, 5 * BSIZE, 100);
    ba_check_l(ba, 4, 6 * BSIZE, 100);
    ba_check_l(ba, 5, 7 * BSIZE, 100);
    ba_check_none(ba, 6);

    uint64_t b2;
    ba_alloc(ba, 100, &b2);
    invariant(b2 == 4 * BSIZE);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_l(ba, 2, 2 * BSIZE, BSIZE + 100);
    ba_check_l(ba, 3, 4 * BSIZE, 100);
    ba_check_l(ba, 4, 5 * BSIZE, 100);
    ba_check_l(ba, 5, 6 * BSIZE, 100);
    ba_check_l(ba, 6, 7 * BSIZE, 100);
    ba_check_none(ba, 7);

    ba_free(ba, BSIZE, 100);
    ba_free(ba, 5 * BSIZE, 100);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, 2 * BSIZE, BSIZE + 100);
    ba_check_l(ba, 2, 4 * BSIZE, 100);
    ba_check_l(ba, 3, 6 * BSIZE, 100);
    ba_check_l(ba, 4, 7 * BSIZE, 100);
    ba_check_none(ba, 5);

    // This alloc will allocate the first block after the reserve space in the
    // case of first fit.
    uint64_t b3;
    ba_alloc(ba, 100, &b3);
    invariant(b3 == BSIZE);  // First fit.
    // if (b3==5*BSIZE) then it is next fit.

    // Now 5*BSIZE is free
    uint64_t b5;
    ba_alloc(ba, 100, &b5);
    invariant(b5 == 5 * BSIZE);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_l(ba, 2, 2 * BSIZE, BSIZE + 100);
    ba_check_l(ba, 3, 4 * BSIZE, 100);
    ba_check_l(ba, 4, 5 * BSIZE, 100);
    ba_check_l(ba, 5, 6 * BSIZE, 100);
    ba_check_l(ba, 6, 7 * BSIZE, 100);
    ba_check_none(ba, 7);

    // Now all blocks are busy
    uint64_t b6, b7, b8;
    ba_alloc(ba, 100, &b6);
    ba_alloc(ba, 100, &b7);
    ba_alloc(ba, 100, &b8);
    invariant(b6 == 8 * BSIZE);
    invariant(b7 == 9 * BSIZE);
    invariant(b8 == 10 * BSIZE);
    ba_check_l(ba, 0, 0, 100);
    ba_check_l(ba, 1, BSIZE, 100);
    ba_check_l(ba, 2, 2 * BSIZE, BSIZE + 100);
    ba_check_l(ba, 3, 4 * BSIZE, 100);
    ba_check_l(ba, 4, 5 * BSIZE, 100);
    ba_check_l(ba, 5, 6 * BSIZE, 100);
    ba_check_l(ba, 6, 7 * BSIZE, 100);
    ba_check_l(ba, 7, 8 * BSIZE, 100);
    ba_check_l(ba, 8, 9 * BSIZE, 100);
    ba_check_l(ba, 9, 10 * BSIZE, 100);
    ba_check_none(ba, 10);

    ba_free(ba, 9 * BSIZE, 100);
    ba_free(ba, 7 * BSIZE, 100);
    uint64_t b9;
    ba_alloc(ba, 100, &b9);
    invariant(b9 == 7 * BSIZE);

    ba_free(ba, 5 * BSIZE, 100);
    ba_free(ba, 2 * BSIZE, BSIZE + 100);
    uint64_t b10, b11;
    ba_alloc(ba, 100, &b10);
    invariant(b10 == 2 * BSIZE);
    ba_alloc(ba, 100, &b11);
    invariant(b11 == 3 * BSIZE);
    ba_alloc(ba, 100, &b11);
    invariant(b11 == 5 * BSIZE);

    ba->Destroy();
}

int test_main(int argc __attribute__((__unused__)),
              const char *argv[] __attribute__((__unused__))) {
    test_ba0();
    test_ba1(0);
    test_ba1(10);
    test_ba1(20);
    test_ba2();
    return 0;
}
