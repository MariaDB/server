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

#include <algorithm>

#include <string.h>

#include "toku_portability.h"
#include "portability/memory.h"
#include "portability/toku_assert.h"
#include "portability/toku_stdint.h"
#include "portability/toku_stdlib.h"

#include "ft/serialize/block_allocator.h"
#include "ft/serialize/rbtree_mhs.h"

#if defined(TOKU_DEBUG_PARANOID) && TOKU_DEBUG_PARANOID
#define VALIDATE() Validate()
#else
#define VALIDATE()
#endif

void BlockAllocator::CreateInternal(uint64_t reserve_at_beginning,
                                    uint64_t alignment) {
    // the alignment must be at least 512 and aligned with 512 to work with
    // direct I/O
    invariant(alignment >= 512 && (alignment % 512) == 0);

    _reserve_at_beginning = reserve_at_beginning;
    _alignment = alignment;
    _n_blocks = 0;
    _n_bytes_in_use = reserve_at_beginning;
    _tree = new MhsRbTree::Tree(alignment);
}

void BlockAllocator::Create(uint64_t reserve_at_beginning, uint64_t alignment) {
    CreateInternal(reserve_at_beginning, alignment);
    _tree->Insert({reserve_at_beginning, MAX_BYTE});
    VALIDATE();
}

void BlockAllocator::Destroy() {
    delete _tree;
}

void BlockAllocator::CreateFromBlockPairs(uint64_t reserve_at_beginning,
                                          uint64_t alignment,
                                          struct BlockPair *translation_pairs,
                                          uint64_t n_blocks) {
    CreateInternal(reserve_at_beginning, alignment);
    _n_blocks = n_blocks;

    struct BlockPair *XMALLOC_N(n_blocks, pairs);
    memcpy(pairs, translation_pairs, n_blocks * sizeof(struct BlockPair));
    std::sort(pairs, pairs + n_blocks);

    if (pairs[0]._offset > reserve_at_beginning) {
        _tree->Insert(
            {reserve_at_beginning, pairs[0]._offset - reserve_at_beginning});
    }
    for (uint64_t i = 0; i < _n_blocks; i++) {
        // Allocator does not support size 0 blocks. See
        // block_allocator_free_block.
        invariant(pairs[i]._size > 0);
        invariant(pairs[i]._offset >= _reserve_at_beginning);
        invariant(pairs[i]._offset % _alignment == 0);

        _n_bytes_in_use += pairs[i]._size;

        MhsRbTree::OUUInt64 free_size(MAX_BYTE);
        MhsRbTree::OUUInt64 free_offset(pairs[i]._offset + pairs[i]._size);
        if (i < n_blocks - 1) {
            MhsRbTree::OUUInt64 next_offset(pairs[i + 1]._offset);
            invariant(next_offset >= free_offset);
            free_size = next_offset - free_offset;
            if (free_size == 0)
                continue;
        }
        _tree->Insert({free_offset, free_size});
    }
    toku_free(pairs);
    VALIDATE();
}

// Effect: align a value by rounding up.
static inline uint64_t Align(uint64_t value, uint64_t ba_alignment) {
    return ((value + ba_alignment - 1) / ba_alignment) * ba_alignment;
}

// Effect: Allocate a block. The resulting block must be aligned on the
// ba->alignment (which to make direct_io happy must be a positive multiple of
// 512).
void BlockAllocator::AllocBlock(uint64_t size,
                                uint64_t *offset) {
    // Allocator does not support size 0 blocks. See block_allocator_free_block.
    invariant(size > 0);

    _n_bytes_in_use += size;
    *offset = _tree->Remove(size);

    _n_blocks++;
    VALIDATE();
}

// To support 0-sized blocks, we need to include size as an input to this
// function.
// All 0-sized blocks at the same offset can be considered identical, but
// a 0-sized block can share offset with a non-zero sized block.
// The non-zero sized block is not exchangable with a zero sized block (or vice
// versa), so inserting 0-sized blocks can cause corruption here.
void BlockAllocator::FreeBlock(uint64_t offset, uint64_t size) {
    VALIDATE();
    _n_bytes_in_use -= size;
    _tree->Insert({offset, size});
    _n_blocks--;
    VALIDATE();
}

uint64_t BlockAllocator::AllocatedLimit() const {
    MhsRbTree::Node *max_node = _tree->MaxNode();
    return rbn_offset(max_node).ToInt();
}

// Effect: Consider the blocks in sorted order.  The reserved block at the
// beginning is number 0.  The next one is number 1 and so forth.
// Return the offset and size of the block with that number.
// Return 0 if there is a block that big, return nonzero if b is too big.
int BlockAllocator::NthBlockInLayoutOrder(uint64_t b,
                                          uint64_t *offset,
                                          uint64_t *size) {
    MhsRbTree::Node *x, *y;
    if (b == 0) {
        *offset = 0;
        *size = _reserve_at_beginning;
        return 0;
    } else if (b > _n_blocks) {
        return -1;
    } else {
        x = _tree->MinNode();
        for (uint64_t i = 1; i <= b; i++) {
            y = x;
            x = _tree->Successor(x);
        }
        *size = (rbn_offset(x) - (rbn_offset(y) + rbn_size(y))).ToInt();
        *offset = (rbn_offset(y) + rbn_size(y)).ToInt();
        return 0;
    }
}

struct VisUnusedExtra {
    TOKU_DB_FRAGMENTATION _report;
    uint64_t _align;
};

static void VisUnusedCollector(void *extra,
                               MhsRbTree::Node *node,
                               uint64_t UU(depth)) {
    struct VisUnusedExtra *v_e = (struct VisUnusedExtra *)extra;
    TOKU_DB_FRAGMENTATION report = v_e->_report;
    uint64_t alignm = v_e->_align;

    MhsRbTree::OUUInt64 offset = rbn_offset(node);
    MhsRbTree::OUUInt64 size = rbn_size(node);
    MhsRbTree::OUUInt64 answer_offset(Align(offset.ToInt(), alignm));
    uint64_t free_space = (offset + size - answer_offset).ToInt();
    if (free_space > 0) {
        report->unused_bytes += free_space;
        report->unused_blocks++;
        if (free_space > report->largest_unused_block) {
            report->largest_unused_block = free_space;
        }
    }
}
// Requires: report->file_size_bytes is filled in
// Requires: report->data_bytes is filled in
// Requires: report->checkpoint_bytes_additional is filled in
void BlockAllocator::UnusedStatistics(TOKU_DB_FRAGMENTATION report) {
    invariant(_n_bytes_in_use ==
              report->data_bytes + report->checkpoint_bytes_additional);

    report->unused_bytes = 0;
    report->unused_blocks = 0;
    report->largest_unused_block = 0;
    struct VisUnusedExtra extra = {report, _alignment};
    _tree->InOrderVisitor(VisUnusedCollector, &extra);
}

void BlockAllocator::Statistics(TOKU_DB_FRAGMENTATION report) {
    report->data_bytes = _n_bytes_in_use;
    report->data_blocks = _n_blocks;
    report->file_size_bytes = 0;
    report->checkpoint_bytes_additional = 0;
    UnusedStatistics(report);
}

struct ValidateExtra {
    uint64_t _bytes;
    MhsRbTree::Node *_pre_node;
};
static void VisUsedBlocksInOrder(void *extra,
                                 MhsRbTree::Node *cur_node,
                                 uint64_t UU(depth)) {
    struct ValidateExtra *v_e = (struct ValidateExtra *)extra;
    MhsRbTree::Node *pre_node = v_e->_pre_node;
    // verify no overlaps
    if (pre_node) {
        invariant(rbn_size(pre_node) > 0);
        invariant(rbn_offset(cur_node) >
                  rbn_offset(pre_node) + rbn_size(pre_node));
        MhsRbTree::OUUInt64 used_space =
            rbn_offset(cur_node) - (rbn_offset(pre_node) + rbn_size(pre_node));
        v_e->_bytes += used_space.ToInt();
    } else {
        v_e->_bytes += rbn_offset(cur_node).ToInt();
    }
    v_e->_pre_node = cur_node;
}

void BlockAllocator::Validate() const {
    _tree->ValidateBalance();
    _tree->ValidateMhs();
    struct ValidateExtra extra = {0, nullptr};
    _tree->InOrderVisitor(VisUsedBlocksInOrder, &extra);
    invariant(extra._bytes == _n_bytes_in_use);
}
