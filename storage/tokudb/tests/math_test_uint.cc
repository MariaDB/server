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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <tokudb_math.h>
using namespace tokudb;

static void test_uint_range(uint length_bits) {
    assert(uint_low_endpoint(length_bits) == 0);
    if (length_bits == 64)
        assert(uint_high_endpoint(length_bits) == ~0ULL);
    else
        assert(uint_high_endpoint(length_bits) == (1ULL<<length_bits)-1);
}

static void test_uint8() {
    printf("%s\n", __FUNCTION__);
    test_uint_range(8);
    bool over;
    uint8_t n;
    uint64_t m;
    for (uint64_t x = 0; x <= (1ULL<<8)-1; x++) {
        for (uint64_t y = 0; y <= (1ULL<<8)-1; y++) {
            n = uint_add(x, y, 8, &over);
            m = x + y;
            if (m > (1ULL<<8)-1)
                assert(over);
            else 
                assert(!over && n == (m % 256));
            n = uint_sub(x, y, 8, &over);
            m = x - y;
            if (m > x)
                assert(over);
            else
                assert(!over && n == (m % 256));
        }
    }
}

static void test_uint16() {
    printf("%s\n", __FUNCTION__);
    test_uint_range(16);
    bool over;
    uint16_t n;
    uint64_t m;
    for (uint64_t x = 0; x <= (1ULL<<16)-1; x++) {
        for (uint64_t y = 0; y <= (1ULL<<16)-1; y++) {
            n = uint_add(x, y, 16, &over);
            m = x + y;
            if (m > (1ULL<<16)-1)
                assert(over);
            else
                assert(!over && n == (m % (1ULL<<16)));
            n = uint_sub(x, y, 16, &over);
            m = x - y;
            if (m > x)
                assert(over);
            else
                assert(!over && n == (m % (1ULL<<16)));
        }
    }
}

static void test_uint24() {
    printf("%s\n", __FUNCTION__);
    test_uint_range(24);
    bool over;
    uint64_t s;

    s = uint_add((1ULL<<24)-1, (1ULL<<24)-1, 24, &over); assert(over);
    s = uint_add((1ULL<<24)-1, 1, 24, &over); assert(over);
    s = uint_add((1ULL<<24)-1, 0, 24, &over); assert(!over && s == (1ULL<<24)-1);
    s = uint_add(0, 1, 24, &over); assert(!over && s == 1);
    s = uint_add(0, 0, 24, &over); assert(!over && s == 0);
    s = uint_sub(0, 0, 24, &over); assert(!over && s == 0);
    s = uint_sub(0, 1, 24, &over); assert(over);
    s = uint_sub(0, (1ULL<<24)-1, 24, &over); assert(over);
    s = uint_sub((1ULL<<24)-1, (1ULL<<24)-1, 24, &over); assert(!over && s == 0);
}

static void test_uint32() {
    printf("%s\n", __FUNCTION__);
    test_uint_range(32);
    bool over;
    uint64_t s;

    s = uint_add((1ULL<<32)-1, (1ULL<<32)-1, 32, &over); assert(over);
    s = uint_add((1ULL<<32)-1, 1, 32, &over); assert(over);
    s = uint_add((1ULL<<32)-1, 0, 32, &over); assert(!over && s == (1ULL<<32)-1);
    s = uint_add(0, 1, 32, &over); assert(!over && s == 1);
    s = uint_add(0, 0, 32, &over); assert(!over && s == 0);
    s = uint_sub(0, 0, 32, &over); assert(!over && s == 0);
    s = uint_sub(0, 1, 32, &over); assert(over);
    s = uint_sub(0, (1ULL<<32)-1, 32, &over); assert(over);
    s = uint_sub((1ULL<<32)-1, (1ULL<<32)-1, 32, &over); assert(!over && s == 0);
}

static void test_uint64() {
    printf("%s\n", __FUNCTION__);
    test_uint_range(64);
    bool over;
    uint64_t s;

    s = uint_add(~0ULL, ~0ULL, 64, &over); assert(over);
    s = uint_add(~0ULL, 1, 64, &over); assert(over);
    s = uint_add(~0ULL, 0, 64, &over); assert(!over && s == ~0ULL);
    s = uint_add(0, 1, 64, &over); assert(!over && s == 1);
    s = uint_add(0, 0, 64, &over); assert(!over && s == 0);
    s = uint_sub(0, 0, 64, &over); assert(!over && s == 0);
    s = uint_sub(0, 1, 64, &over); assert(over);
    s = uint_sub(0, ~0ULL, 64, &over); assert(over);
    s = uint_sub(~0ULL, ~0ULL, 64, &over); assert(!over && s == 0);
}

int main() {
    test_uint8();
    test_uint16();
    test_uint24();
    test_uint32();
    test_uint64();
    return 0;
}

