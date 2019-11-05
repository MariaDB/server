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

static void test_vlq_uint32_error(void) {
    uint32_t n;
    unsigned char b[5];
    size_t out_s, in_s;

    out_s = tokudb::vlq_encode_ui<uint32_t>(128, b, 0);
    assert(out_s == 0);
    out_s = tokudb::vlq_encode_ui<uint32_t>(128, b, 1);
    assert(out_s == 0);
    out_s = tokudb::vlq_encode_ui<uint32_t>(128, b, 2);
    assert(out_s == 2);
    in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, 0);
    assert(in_s == 0);
    in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, 1);
    assert(in_s == 0);
    in_s = tokudb::vlq_decode_ui<uint32_t>(&n, b, 2);
    assert(in_s == 2);
    assert(n == 128);
}

static void test_80000000(void) {
    uint64_t n;
    unsigned char b[10];
    size_t out_s, in_s;
    uint64_t v = 0x80000000;
    out_s = tokudb::vlq_encode_ui<uint64_t>(v, b, sizeof b);
    assert(out_s == 5);
    in_s = tokudb::vlq_decode_ui<uint64_t>(&n, b, out_s);
    assert(in_s == 5);
    assert(n == v);
}

static void test_100000000(void) {
    uint64_t n;
    unsigned char b[10];
    size_t out_s, in_s;
    uint64_t v = 0x100000000;
    out_s = tokudb::vlq_encode_ui<uint64_t>(v, b, sizeof b);
    assert(out_s == 5);
    in_s = tokudb::vlq_decode_ui<uint64_t>(&n, b, out_s);
    assert(in_s == 5);
    assert(n == v);
}   

int main(void) {
    test_vlq_uint32_error();
    test_80000000();
    test_100000000();
    return 0;
}
