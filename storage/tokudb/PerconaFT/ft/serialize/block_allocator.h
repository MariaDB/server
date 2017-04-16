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

#pragma once

#include <db.h>

#include "portability/toku_pthread.h"
#include "portability/toku_stdint.h"
#include "portability/toku_stdlib.h"
#include "ft/serialize/rbtree_mhs.h"

// Block allocator.
//
// A block allocator manages the allocation of variable-sized blocks.
// The translation of block numbers to addresses is handled elsewhere.
// The allocation of block numbers is handled elsewhere.
//
// When creating a block allocator we also specify a certain-sized
// block at the beginning that is preallocated (and cannot be allocated or
// freed)
//
// We can allocate blocks of a particular size at a particular location.
// We can free blocks.
// We can determine the size of a block.
#define MAX_BYTE 0xffffffffffffffff
class BlockAllocator {
   public:
    static const size_t BLOCK_ALLOCATOR_ALIGNMENT = 4096;

    // How much must be reserved at the beginning for the block?
    //  The actual header is 8+4+4+8+8_4+8+ the length of the db names + 1
    //  pointer for each root.
    //  So 4096 should be enough.
    static const size_t BLOCK_ALLOCATOR_HEADER_RESERVE = 4096;

    static_assert(BLOCK_ALLOCATOR_HEADER_RESERVE % BLOCK_ALLOCATOR_ALIGNMENT ==
                      0,
                  "block allocator header must have proper alignment");

    static const size_t BLOCK_ALLOCATOR_TOTAL_HEADER_RESERVE =
        BLOCK_ALLOCATOR_HEADER_RESERVE * 2;

    struct BlockPair {
        uint64_t _offset;
        uint64_t _size;
        BlockPair(uint64_t o, uint64_t s) : _offset(o), _size(s) {}
        int operator<(const struct BlockPair &rhs) const {
            return _offset < rhs._offset;
        }
        int operator<(const uint64_t &o) const { return _offset < o; }
    };

    // Effect: Create a block allocator, in which the first RESERVE_AT_BEGINNING
    // bytes are not put into a block.
    //         The default allocation strategy is first fit
    //         (BA_STRATEGY_FIRST_FIT)
    //  All blocks be start on a multiple of ALIGNMENT.
    //  Aborts if we run out of memory.
    // Parameters
    //  reserve_at_beginning (IN)        Size of reserved block at beginning.
    //  This size does not have to be aligned.
    //  alignment (IN)                   Block alignment.
    void Create(uint64_t reserve_at_beginning, uint64_t alignment);

    // Effect: Create a block allocator, in which the first RESERVE_AT_BEGINNING
    // bytes are not put into a block.
    //         The allocator is initialized to contain `n_blocks' of BlockPairs,
    //         taken from `pairs'
    //  All blocks be start on a multiple of ALIGNMENT.
    //  Aborts if we run out of memory.
    // Parameters
    //  pairs,                           unowned array of pairs to copy
    //  n_blocks,                        Size of pairs array
    //  reserve_at_beginning (IN)        Size of reserved block at beginning.
    //  This size does not have to be aligned.
    //  alignment (IN)                   Block alignment.
    void CreateFromBlockPairs(uint64_t reserve_at_beginning,
                              uint64_t alignment,
                              struct BlockPair *pairs,
                              uint64_t n_blocks);

    // Effect: Destroy this block allocator
    void Destroy();

    // Effect: Allocate a block of the specified size at an address chosen by
    // the allocator.
    //  Aborts if anything goes wrong.
    //  The block address will be a multiple of the alignment.
    // Parameters:
    //  size (IN):    The size of the block.  (The size does not have to be
    //  aligned.)
    //  offset (OUT): The location of the block.
    //  block soon (perhaps in the next checkpoint)
    //                Heat values are lexiographically ordered (like integers),
    //                but their specific values are arbitrary
    void AllocBlock(uint64_t size, uint64_t *offset);

    // Effect: Free the block at offset.
    // Requires: There must be a block currently allocated at that offset.
    // Parameters:
    //  offset (IN): The offset of the block.
    void FreeBlock(uint64_t offset, uint64_t size);

    // Effect: Check to see if the block allocator is OK.  This may take a long
    // time.
    // Usage Hints: Probably only use this for unit tests.
    // TODO: Private?
    void Validate() const;

    // Effect: Return the unallocated block address of "infinite" size.
    //  That is, return the smallest address that is above all the allocated
    //  blocks.
    uint64_t AllocatedLimit() const;

    // Effect: Consider the blocks in sorted order.  The reserved block at the
    // beginning is number 0.  The next one is number 1 and so forth.
    //  Return the offset and size of the block with that number.
    //  Return 0 if there is a block that big, return nonzero if b is too big.
    // Rationale: This is probably useful only for tests.
    int NthBlockInLayoutOrder(uint64_t b, uint64_t *offset, uint64_t *size);

    // Effect:  Fill in report to indicate how the file is used.
    // Requires:
    //  report->file_size_bytes is filled in
    //  report->data_bytes is filled in
    //  report->checkpoint_bytes_additional is filled in
    void UnusedStatistics(TOKU_DB_FRAGMENTATION report);

    // Effect: Fill in report->data_bytes with the number of bytes in use
    //         Fill in report->data_blocks with the number of BlockPairs in use
    //         Fill in unused statistics using this->get_unused_statistics()
    // Requires:
    //  report->file_size is ignored on return
    //  report->checkpoint_bytes_additional is ignored on return
    void Statistics(TOKU_DB_FRAGMENTATION report);

    virtual ~BlockAllocator(){};

   private:
    void CreateInternal(uint64_t reserve_at_beginning, uint64_t alignment);

    // How much to reserve at the beginning
    uint64_t _reserve_at_beginning;
    // Block alignment
    uint64_t _alignment;
    // How many blocks
    uint64_t _n_blocks;
    uint64_t _n_bytes_in_use;

    // These blocks are sorted by address.
    MhsRbTree::Tree *_tree;
};
