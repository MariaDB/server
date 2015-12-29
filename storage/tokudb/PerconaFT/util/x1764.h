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

#include <toku_stdint.h>

// The x1764 hash is
//   $s = \sum_i a_i*17^i$  where $a_i$ is the $i$th 64-bit number (represented in little-endian format)
// The final 32-bit result is the xor of the high- and low-order bits of s.
// If any odd bytes numbers are left at the end, they are filled in at the low end.


uint32_t toku_x1764_memory (const void *buf, int len);
// Effect: Compute x1764 on the bytes of buf.  Return the 32 bit answer.

uint32_t toku_x1764_memory_simple (const void *buf, int len);
// Effect: Same as toku_x1764_memory, but not highly optimized (more likely to be correct).  Useful for testing the optimized version.


// For incrementally computing an x1764, use the following interfaces.
struct x1764 {
    uint64_t sum;
    uint64_t input;
    int n_input_bytes;
};

void toku_x1764_init(struct x1764 *l);
// Effect: Initialize *l.

void toku_x1764_add (struct x1764 *l, const void *vbuf, int len);
// Effect: Add more bytes to *l.

uint32_t toku_x1764_finish (struct x1764 *l);
// Effect: Return the final 32-bit result.
