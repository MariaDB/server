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
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>

#include <tokudb_math.h>
using namespace tokudb;

static void test(int length_bits) {
    printf("%s %d\n", __FUNCTION__, length_bits);
    uint64_t max = (1ULL << length_bits) - 1;
    for (uint64_t x = 0; x <= max; x++) {
        for (uint64_t y = 0; y <= max; y++) {
            bool over;
            uint64_t n = uint_add(x, y, max, &over);
            printf("%"PRIu64" %"PRIu64" %"PRIu64"\n", x, y, n);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            test(atoi(argv[i]));
        }
    }
    return 0;
}
