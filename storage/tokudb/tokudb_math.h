/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#if !defined(_TOKUDB_MATH_H)
#define _TOKUDB_MATH_H

namespace tokudb {

// Add and subtract ints with overflow detection.
// Overflow detection adapted from "Hackers Delight", Henry S. Warren

// Return a bit mask for bits 0 .. length_bits-1
TOKUDB_UNUSED(static uint64_t uint_mask(uint length_bits));
static uint64_t uint_mask(uint length_bits) {
    return length_bits == 64 ? ~0ULL : (1ULL<<length_bits)-1;
}

// Return the highest unsigned int with a given number of bits
TOKUDB_UNUSED(static uint64_t uint_high_endpoint(uint length_bits));
static uint64_t uint_high_endpoint(uint length_bits) {
    return uint_mask(length_bits);
}

// Return the lowest unsigned int with a given number of bits
TOKUDB_UNUSED(static uint64_t uint_low_endpoint(uint length_bits));
static uint64_t uint_low_endpoint(TOKUDB_UNUSED(uint length_bits)) {
    return 0;
}

// Add two unsigned integers with max maximum value.
// If there is an overflow then set the sum to the max.
// Return the sum and the overflow.
TOKUDB_UNUSED(static uint64_t uint_add(
    uint64_t x,
    uint64_t y,
    uint length_bits,
    bool* over));
static uint64_t uint_add(uint64_t x, uint64_t y, uint length_bits, bool *over) {
    uint64_t mask = uint_mask(length_bits);
    assert_always((x & ~mask) == 0);
    assert_always((y & ~mask) == 0);
    uint64_t s = (x + y) & mask;
    *over = s < x;     // check for overflow
    return s;
}

// Subtract two unsigned ints with max maximum value.
// If there is an over then set the difference to 0.
// Return the difference and the overflow.
TOKUDB_UNUSED(static uint64_t uint_sub(
    uint64_t x,
    uint64_t y,
    uint length_bits,
    bool* over));
static uint64_t uint_sub(uint64_t x, uint64_t y, uint length_bits, bool *over) {
    uint64_t mask = uint_mask(length_bits);
    assert_always((x & ~mask) == 0);
    assert_always((y & ~mask) == 0);
    uint64_t s = (x - y) & mask;
    *over = s > x;    // check for overflow
    return s;
}

// Return the highest int with a given number of bits
TOKUDB_UNUSED(static int64_t int_high_endpoint(uint length_bits));
static int64_t int_high_endpoint(uint length_bits) {
    return (1ULL<<(length_bits-1))-1;
}

// Return the lowest int with a given number of bits
TOKUDB_UNUSED(static int64_t int_low_endpoint(uint length_bits));
static int64_t int_low_endpoint(uint length_bits) {
    int64_t mask = uint_mask(length_bits);
    return (1ULL<<(length_bits-1)) | ~mask;
}

// Sign extend to 64 bits an int with a given number of bits
TOKUDB_UNUSED(static int64_t int_sign_extend(int64_t n, uint length_bits));
static int64_t int_sign_extend(int64_t n, uint length_bits) {
    if (n & (1ULL<<(length_bits-1)))
        n |= ~uint_mask(length_bits);
    return n;
}

// Add two signed ints with max maximum value.
// If there is an overflow then set the sum to the max or the min of the int range,
// depending on the sign bit.
// Sign extend to 64 bits.
// Return the sum and the overflow.
TOKUDB_UNUSED(static int64_t int_add(
    int64_t x,
    int64_t y,
    uint length_bits,
    bool* over));
static int64_t int_add(int64_t x, int64_t y, uint length_bits, bool *over) {
    int64_t mask = uint_mask(length_bits);
    int64_t n = (x + y) & mask;
    *over = (((n ^ x) & (n ^ y)) >> (length_bits-1)) & 1;    // check for overflow
    if (n & (1LL<<(length_bits-1)))
        n |= ~mask;    // sign extend
    return n;
}

// Subtract two signed ints.
// If there is an overflow then set the sum to the max or the min of the int range,
// depending on the sign bit.
// Sign extend to 64 bits.
// Return the sum and the overflow.
TOKUDB_UNUSED(static int64_t int_sub(
    int64_t x,
    int64_t y,
    uint length_bits,
    bool* over));
static int64_t int_sub(int64_t x, int64_t y, uint length_bits, bool *over) {
    int64_t mask = uint_mask(length_bits);
    int64_t n = (x - y) & mask;
    *over = (((x ^ y) & (n ^ x)) >> (length_bits-1)) & 1;    // check for overflow
    if (n & (1LL<<(length_bits-1)))
        n |= ~mask;    // sign extend
    return n;
}

} // namespace tokudb

#endif
