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
    int64_t max = (1ULL << (length_bits-1)) - 1;
    for (int64_t x = -max-1; x <= max; x++) {
        for (int64_t y = -max-1; y <= max; y++) {
            bool over;
            int64_t n = int_add(x, y, length_bits, &over);
            printf("%"PRId64" %"PRId64" %"PRId64" %u\n", x, y, n, over);
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
