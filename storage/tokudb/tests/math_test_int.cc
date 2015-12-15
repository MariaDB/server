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

static int64_t sign_extend(uint length_bits, int64_t n) {
    return n | ~((1ULL<<(length_bits-1))-1);
}

static void test_int_range(uint length_bits) {
    assert(int_high_endpoint(length_bits) == (int64_t)((1ULL<<(length_bits-1))-1));
    assert(int_low_endpoint(length_bits) == sign_extend(length_bits, 1ULL<<(length_bits-1)));
}

static void test_int8() {
    printf("%s\n", __FUNCTION__);
    test_int_range(8);
    int64_t max = (1LL << 7);
    for (int64_t x = -max; x <= max-1; x++) {
        for (int64_t y = -max; y <= max-1; y++) {
            bool over;
            int64_t n, m;
            n = int_add(x, y, 8, &over);
            m = x + y;
            if (m > max-1)
                assert(over);
            else if (m < -max)
                assert(over);
            else
                assert(!over && n == m);
            n = int_sub(x, y, 8, &over);
            m = x - y;
            if (m > max-1)
                assert(over);
            else if (m < -max)
                assert(over);
            else
                assert(!over && n == m);
        }
    }
}

static void test_int16() {
    printf("%s\n", __FUNCTION__);
    test_int_range(16);
    int64_t max = (1LL << 15);
    for (int64_t x = -max; x <= max-1; x++) {
        for (int64_t y = -max; y <= max-1; y++) {
            bool over;
            int64_t n, m;
            n = int_add(x, y, 16, &over);
            m = x + y;
            if (m > max-1)
                assert(over);
            else if (m < -max)
                assert(over);
            else
                assert(!over && n == m);
            n = int_sub(x, y, 16, &over);
            m = x - y;
            if (m > max-1)
                assert(over);
            else if (m < -max)
                assert(over);
            else
                assert(!over && n == m);
        }
    }
}

static void test_int24() {
    printf("%s\n", __FUNCTION__);
    test_int_range(24);
    int64_t s;
    bool over;

    s = int_add(1, (1ULL<<23)-1, 24, &over); assert(over);
    s = int_add((1ULL<<23)-1, 1, 24, &over); assert(over);
    s = int_sub(-1, (1ULL<<23), 24, &over); assert(!over && s == (1ULL<<23)-1);
    s = int_sub((1ULL<<23), 1, 24, &over); assert(over);

    s = int_add(0, 0, 24, &over); assert(!over && s == 0);
    s = int_sub(0, 0, 24, &over); assert(!over && s == 0);
    s = int_add(0, -1, 24, &over); assert(!over && s == -1);
    s = int_sub(0, 1, 24, &over); assert(!over && s == -1);
    s = int_add(0, (1ULL<<23), 24, &over); assert(!over && (s & ((1ULL<<24)-1)) == (1ULL<<23));
    s = int_sub(0, (1ULL<<23)-1, 24, &over); assert(!over && (s & ((1ULL<<24)-1)) == (1ULL<<23)+1);

    s = int_add(-1, 0, 24, &over); assert(!over && s == -1);
    s = int_add(-1, 1, 24, &over); assert(!over && s == 0);
    s = int_sub(-1, -1, 24, &over); assert(!over && s == 0);
    s = int_sub(-1, (1ULL<<23)-1, 24, &over); assert(!over && (s & ((1ULL<<24)-1)) == (1ULL<<23));
}

static void test_int32() {
    printf("%s\n", __FUNCTION__);
    test_int_range(32);
    int64_t s;
    bool over;

    s = int_add(1, (1ULL<<31)-1, 32, &over); assert(over);
    s = int_add((1ULL<<31)-1, 1, 32, &over); assert(over);
    s = int_sub(-1, (1ULL<<31), 32, &over); assert(s == (1ULL<<31)-1 && !over);
    s = int_sub((1ULL<<31), 1, 32, &over); assert(over);

    s = int_add(0, 0, 32, &over); assert(s == 0 && !over);
    s = int_sub(0, 0, 32, &over); assert(s == 0 && !over);
    s = int_add(0, -1, 32, &over); assert(s == -1 && !over);
    s = int_sub(0, 1, 32, &over); assert(s == -1 && !over);
    s = int_add(0, (1ULL<<31), 32, &over); assert((s & ((1ULL<<32)-1)) == (1ULL<<31) && !over);
    s = int_sub(0, (1ULL<<31)-1, 32, &over); assert((s & ((1ULL<<32)-1)) == (1ULL<<31)+1 && !over);

    s = int_add(-1, 0, 32, &over); assert(s == -1 && !over);
    s = int_add(-1, 1, 32, &over); assert(s == 0 && !over);
    s = int_sub(-1, -1, 32, &over); assert(s == 0 && !over);
    s = int_sub(-1, (1ULL<<31)-1, 32, &over); assert((s & ((1ULL<<32)-1)) == (1ULL<<31) && !over);
}

static void test_int64() {
    printf("%s\n", __FUNCTION__);
    test_int_range(64);
    int64_t s;
    bool over;

    s = int_add(1, (1ULL<<63)-1, 64, &over); assert(over);
    s = int_add((1ULL<<63)-1, 1, 64, &over); assert(over);
    s = int_sub(-1, (1ULL<<63), 64, &over); assert(s == (1ULL<<63)-1 && !over);
    s = int_sub((1ULL<<63), 1, 64, &over); assert(over);

    s = int_add(0, 0, 64, &over); assert(s == 0 && !over);
    s = int_sub(0, 0, 64, &over); assert(s == 0 && !over);
    s = int_add(0, -1, 64, &over); assert(s == -1 && !over);
    s = int_sub(0, 1, 64, &over); assert(s == -1 && !over);
    s = int_add(0, (1ULL<<63), 64, &over); assert(s == (int64_t)(1ULL<<63) && !over);
    s = int_sub(0, (1ULL<<63)-1, 64, &over); assert(s == (int64_t)((1ULL<<63)+1) && !over);

    s = int_add(-1, 0, 64, &over); assert(s == -1 && !over);
    s = int_add(-1, 1, 64, &over); assert(s == 0 && !over);
    s = int_sub(-1, -1, 64, &over); assert(s == 0 && !over);
    s = int_sub(-1, (1ULL<<63)-1, 64, &over); assert(s == (int64_t)(1ULL<<63) && !over);
}

static void test_int_sign(uint length_bits) {
    printf("%s %u\n", __FUNCTION__, length_bits);
    int64_t n;
    
    n = int_high_endpoint(length_bits);
    assert(int_sign_extend(n, length_bits) == n);
    n = (1ULL<<(length_bits-1));
    assert(int_sign_extend(n, length_bits) == -n);
}

static void test_int_sign() {
    test_int_sign(8);
    test_int_sign(16);
    test_int_sign(24);
    test_int_sign(32);
    test_int_sign(64);
}

int main() {
    test_int_sign();
    test_int8();
    test_int16();
    test_int24();
    test_int32();
    test_int64();
    return 0;
}

