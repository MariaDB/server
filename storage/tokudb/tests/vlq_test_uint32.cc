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

static void test_vlq_uint32(void) {
    printf("%u\n", 0);
    for (uint32_t v = 0; v < (1<<7); v++) {
        unsigned char b[5];
        size_t out_s = tokudb::vlq_encode_ui<uint32_t>(v, b, sizeof b);
        assert(out_s == 1);
        uint32_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, out_s);
        assert(in_s == 1 && n == v);
    }

    printf("%u\n", 1<<7);
    for (uint32_t v = (1<<7); v < (1<<14); v++) {
        unsigned char b[5];
        size_t out_s = tokudb::vlq_encode_ui<uint32_t>(v, b, sizeof b);
        assert(out_s == 2);
        uint32_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, out_s);
        assert(in_s == 2 && n == v);
    }

    printf("%u\n", 1<<14);
    for (uint32_t v = (1<<14); v < (1<<21); v++) {
        unsigned char b[5];
        size_t out_s = tokudb::vlq_encode_ui<uint32_t>(v, b, sizeof b);
        assert(out_s == 3);
        uint32_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, out_s);
        assert(in_s == 3 && n == v);
    }

    printf("%u\n", 1<<21);
    for (uint32_t v = (1<<21); v < (1<<28); v++) {
        unsigned char b[5];
        size_t out_s = tokudb::vlq_encode_ui<uint32_t>(v, b, sizeof b);
        assert(out_s == 4);
        uint32_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, out_s);
        assert(in_s == 4 && n == v);
    }

    printf("%u\n", 1<<28);
    for (uint32_t v = (1<<28); v != 0; v++) {
        unsigned char b[5];
        size_t out_s = tokudb::vlq_encode_ui<uint32_t>(v, b, sizeof b);
        assert(out_s == 5);
        uint32_t n;
        size_t in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, out_s);
        assert(in_s == 5 && n == v);
    }
}

int main(void) {
    test_vlq_uint32();
    return 0;
}
