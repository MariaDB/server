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

static void ba_alloc(block_allocator *ba, uint64_t size, uint64_t *answer) {
    ba->validate();
    uint64_t actual_answer;
    const uint64_t heat = random() % 2;
    ba->alloc_block(512 * size, heat, &actual_answer);
    ba->validate();

    assert(actual_answer%512==0);
    *answer = actual_answer/512;
}

static void ba_free(block_allocator *ba, uint64_t offset) {
    ba->validate();
    ba->free_block(offset * 512);
    ba->validate();
}

static void ba_check_l(block_allocator *ba, uint64_t blocknum_in_layout_order,
                       uint64_t expected_offset, uint64_t expected_size) {
    uint64_t actual_offset, actual_size;
    int r = ba->get_nth_block_in_layout_order(blocknum_in_layout_order, &actual_offset, &actual_size);
    assert(r==0);
    assert(expected_offset*512 == actual_offset);
    assert(expected_size  *512 == actual_size);
}

static void ba_check_none(block_allocator *ba, uint64_t blocknum_in_layout_order) {
    uint64_t actual_offset, actual_size;
    int r = ba->get_nth_block_in_layout_order(blocknum_in_layout_order, &actual_offset, &actual_size);
    assert(r==-1);
}


// Simple block allocator test
static void test_ba0(block_allocator::allocation_strategy strategy) {
    block_allocator allocator;
    block_allocator *ba = &allocator;
    ba->create(100*512, 1*512);
    ba->set_strategy(strategy);
    assert(ba->allocated_limit()==100*512);

    uint64_t b2, b3, b4, b5, b6, b7;
    ba_alloc(ba, 100, &b2);     
    ba_alloc(ba, 100, &b3);     
    ba_alloc(ba, 100, &b4);     
    ba_alloc(ba, 100, &b5);     
    ba_alloc(ba, 100, &b6);     
    ba_alloc(ba, 100, &b7);     
    ba_free(ba, b2);
    ba_alloc(ba, 100, &b2);  
    ba_free(ba, b4);         
    ba_free(ba, b6);         
    uint64_t b8, b9;
    ba_alloc(ba, 100, &b4);    
    ba_free(ba, b2);           
    ba_alloc(ba, 100, &b6);    
    ba_alloc(ba, 100, &b8);    
    ba_alloc(ba, 100, &b9);    
    ba_free(ba, b6);           
    ba_free(ba, b7);           
    ba_free(ba, b8);           
    ba_alloc(ba, 100, &b6);    
    ba_alloc(ba, 100, &b7);    
    ba_free(ba, b4);           
    ba_alloc(ba, 100, &b4);    

    ba->destroy();
}

// Manually to get coverage of all the code in the block allocator.
static void
test_ba1(block_allocator::allocation_strategy strategy, int n_initial) {
    block_allocator allocator;
    block_allocator *ba = &allocator;
    ba->create(0*512, 1*512);
    ba->set_strategy(strategy);

    int n_blocks=0;
    uint64_t blocks[1000];
    for (int i = 0; i < 1000; i++) {
	if (i < n_initial || random() % 2 == 0) {
	    if (n_blocks < 1000) {
		ba_alloc(ba, 1, &blocks[n_blocks]);
		//printf("A[%d]=%ld\n", n_blocks, blocks[n_blocks]);
		n_blocks++;
	    } 
	} else {
	    if (n_blocks > 0) {
		int blocknum = random()%n_blocks;
		//printf("F[%d]%ld\n", blocknum, blocks[blocknum]);
		ba_free(ba, blocks[blocknum]);
		blocks[blocknum]=blocks[n_blocks-1];
		n_blocks--;
	    }
	}
    }
    
    ba->destroy();
}
    
// Check to see if it is first fit or best fit.
static void
test_ba2 (void)
{
    block_allocator allocator;
    block_allocator *ba = &allocator;
    uint64_t b[6];
    enum { BSIZE = 1024 };
    ba->create(100*512, BSIZE*512);
    ba->set_strategy(block_allocator::BA_STRATEGY_FIRST_FIT);
    assert(ba->allocated_limit()==100*512);

    ba_check_l    (ba, 0, 0, 100);
    ba_check_none (ba, 1);

    ba_alloc (ba, 100, &b[0]);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1, BSIZE, 100);
    ba_check_none (ba, 2);

    ba_alloc (ba, BSIZE + 100, &b[1]);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1,   BSIZE,       100);
    ba_check_l    (ba, 2, 2*BSIZE, BSIZE + 100);
    ba_check_none (ba, 3);

    ba_alloc (ba, 100, &b[2]);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1,   BSIZE,       100);
    ba_check_l    (ba, 2, 2*BSIZE, BSIZE + 100);
    ba_check_l    (ba, 3, 4*BSIZE,       100);
    ba_check_none (ba, 4);

    ba_alloc (ba, 100, &b[3]);
    ba_alloc (ba, 100, &b[4]);
    ba_alloc (ba, 100, &b[5]);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1,   BSIZE,       100);
    ba_check_l    (ba, 2, 2*BSIZE, BSIZE + 100);
    ba_check_l    (ba, 3, 4*BSIZE,       100);
    ba_check_l    (ba, 4, 5*BSIZE,       100);
    ba_check_l    (ba, 5, 6*BSIZE,       100);
    ba_check_l    (ba, 6, 7*BSIZE,       100);
    ba_check_none (ba, 7);
   
    ba_free (ba, 4*BSIZE);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1,   BSIZE,       100);
    ba_check_l    (ba, 2, 2*BSIZE, BSIZE + 100);
    ba_check_l    (ba, 3, 5*BSIZE,       100);
    ba_check_l    (ba, 4, 6*BSIZE,       100);
    ba_check_l    (ba, 5, 7*BSIZE,       100);
    ba_check_none (ba, 6);

    uint64_t b2;
    ba_alloc(ba, 100, &b2);
    assert(b2==4*BSIZE);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1,   BSIZE,       100);
    ba_check_l    (ba, 2, 2*BSIZE, BSIZE + 100);
    ba_check_l    (ba, 3, 4*BSIZE,       100);
    ba_check_l    (ba, 4, 5*BSIZE,       100);
    ba_check_l    (ba, 5, 6*BSIZE,       100);
    ba_check_l    (ba, 6, 7*BSIZE,       100);
    ba_check_none (ba, 7);

    ba_free (ba,   BSIZE);
    ba_free (ba, 5*BSIZE);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1, 2*BSIZE, BSIZE + 100);
    ba_check_l    (ba, 2, 4*BSIZE,       100);
    ba_check_l    (ba, 3, 6*BSIZE,       100);
    ba_check_l    (ba, 4, 7*BSIZE,       100);
    ba_check_none (ba, 5);

    // This alloc will allocate the first block after the reserve space in the case of first fit.
    uint64_t b3;
    ba_alloc(ba, 100, &b3);
    assert(b3==  BSIZE);      // First fit.
    // if (b3==5*BSIZE) then it is next fit.

    // Now 5*BSIZE is free
    uint64_t b5;
    ba_alloc(ba, 100, &b5);
    assert(b5==5*BSIZE);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1,   BSIZE,       100);
    ba_check_l    (ba, 2, 2*BSIZE, BSIZE + 100);
    ba_check_l    (ba, 3, 4*BSIZE,       100);
    ba_check_l    (ba, 4, 5*BSIZE,       100);
    ba_check_l    (ba, 5, 6*BSIZE,       100);
    ba_check_l    (ba, 6, 7*BSIZE,       100);
    ba_check_none (ba, 7);

    // Now all blocks are busy
    uint64_t b6, b7, b8;
    ba_alloc(ba, 100, &b6);
    ba_alloc(ba, 100, &b7);
    ba_alloc(ba, 100, &b8);
    assert(b6==8*BSIZE);
    assert(b7==9*BSIZE);
    assert(b8==10*BSIZE);
    ba_check_l    (ba, 0, 0, 100);
    ba_check_l    (ba, 1,   BSIZE,       100);
    ba_check_l    (ba, 2, 2*BSIZE, BSIZE + 100);
    ba_check_l    (ba, 3, 4*BSIZE,       100);
    ba_check_l    (ba, 4, 5*BSIZE,       100);
    ba_check_l    (ba, 5, 6*BSIZE,       100);
    ba_check_l    (ba, 6, 7*BSIZE,       100);
    ba_check_l    (ba, 7, 8*BSIZE,       100);
    ba_check_l    (ba, 8, 9*BSIZE,       100);
    ba_check_l    (ba, 9, 10*BSIZE,       100);
    ba_check_none (ba, 10);
    
    ba_free(ba, 9*BSIZE);
    ba_free(ba, 7*BSIZE);
    uint64_t b9;
    ba_alloc(ba, 100, &b9);
    assert(b9==7*BSIZE);

    ba_free(ba, 5*BSIZE);
    ba_free(ba, 2*BSIZE);
    uint64_t b10, b11;
    ba_alloc(ba, 100, &b10);
    assert(b10==2*BSIZE);
    ba_alloc(ba, 100, &b11);
    assert(b11==3*BSIZE);
    ba_alloc(ba, 100, &b11);
    assert(b11==5*BSIZE);

    ba->destroy();
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    enum block_allocator::allocation_strategy strategies[] = {
        block_allocator::BA_STRATEGY_FIRST_FIT,
        block_allocator::BA_STRATEGY_BEST_FIT,
        block_allocator::BA_STRATEGY_PADDED_FIT,
        block_allocator::BA_STRATEGY_HEAT_ZONE,
    };
    for (size_t i = 0; i < sizeof(strategies) / sizeof(strategies[0]); i++) {
        test_ba0(strategies[i]);
        test_ba1(strategies[i], 0);
        test_ba1(strategies[i], 10);
        test_ba1(strategies[i], 20);
    }
    test_ba2();
    return 0;
}
