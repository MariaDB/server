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
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <tokudb_vlq.h>

namespace tokudb {
    template size_t vlq_encode_ui(uint32_t n, void *p, size_t s);
    template size_t vlq_decode_ui(uint32_t *np, void *p, size_t s);
    template size_t vlq_encode_ui(uint64_t n, void *p, size_t s);
    template size_t vlq_decode_ui(uint64_t *np, void *p, size_t s);
};

// test a slice of the number space where the slice is described by
// a start number and a stride through the space.
static void test_vlq_uint64(uint64_t start, uint64_t stride) {
    printf("%u\n", 0);
    for (uint64_t v = 0 + start; v < (1<<7); v += stride) {
        unsigned char b[10];
        size_t out_s = tokudb::vlq_encode_ui<uint64_t>(v, b, sizeof b);
        assert(out_s == 1);
        uint64_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint64_t>(&n, b, out_s);
        assert(in_s == 1);
        assert(n == v);
    }

    printf("%u\n", 1<<7);
    for (uint64_t v = (1<<7) + start; v < (1<<14); v += stride) {
        unsigned char b[10];
        size_t out_s = tokudb::vlq_encode_ui<uint64_t>(v, b, sizeof b);
        assert(out_s == 2);
        uint64_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint64_t>(&n, b, out_s);
        assert(in_s == 2);
        assert(n == v);
    }

    printf("%u\n", 1<<14);
    for (uint64_t v = (1<<14) + start; v < (1<<21); v += stride) {
        unsigned char b[10];
        size_t out_s = tokudb::vlq_encode_ui<uint64_t>(v, b, sizeof b);
        assert(out_s == 3);
        uint64_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint64_t>(&n, b, out_s);
        assert(in_s == 3);
        assert(n == v);
    }

    printf("%u\n", 1<<21);
    for (uint64_t v = (1<<21) + start; v < (1<<28); v += stride) {
        unsigned char b[10];
        size_t out_s = tokudb::vlq_encode_ui<uint64_t>(v, b, sizeof b);
        assert(out_s == 4);
        uint64_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint64_t>(&n, b, out_s);
        assert(in_s == 4);
        assert(n == v);
    }

    printf("%u\n", 1<<28);
#if USE_OPENMP
#pragma omp parallel num_threads(4)
#pragma omp for
#endif
    for (uint64_t v = (1<<28) + start; v < (1ULL<<35); v += stride) {
        unsigned char b[10];
        size_t out_s = tokudb::vlq_encode_ui<uint64_t>(v, b, sizeof b);
        assert(out_s == 5);
        uint64_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint64_t>(&n, b, out_s);
        assert(in_s == 5);
        assert(n == v);
    }
}

int main(int argc, char *argv[]) {
    uint64_t start = 0, stride = 1;
    if (argc == 3) {
        start = atoll(argv[1]);
        stride = atoll(argv[2]);
    }
    test_vlq_uint64(start, stride);
    return 0;
}
