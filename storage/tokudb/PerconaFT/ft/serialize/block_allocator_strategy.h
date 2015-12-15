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

#include "ft/serialize/block_allocator.h"

// Block allocation strategy implementations

class block_allocator_strategy {
public:
    static struct block_allocator::blockpair *
    first_fit(struct block_allocator::blockpair *blocks_array,
              uint64_t n_blocks, uint64_t size, uint64_t alignment);

    static struct block_allocator::blockpair *
    best_fit(struct block_allocator::blockpair *blocks_array,
             uint64_t n_blocks, uint64_t size, uint64_t alignment);

    static struct block_allocator::blockpair *
    padded_fit(struct block_allocator::blockpair *blocks_array,
               uint64_t n_blocks, uint64_t size, uint64_t alignment);

    static struct block_allocator::blockpair *
    heat_zone(struct block_allocator::blockpair *blocks_array,
              uint64_t n_blocks, uint64_t size, uint64_t alignment,
              uint64_t heat);
};
