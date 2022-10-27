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

#include <stdio.h>
#include <toku_stdint.h>
#include <unistd.h>
#include <toku_assert.h>
#include <string.h>
#include "toku_os.h"

int main(int argc, char *const argv[]) {
    int verbose = 0;
    int i;
    for (i=1; i<argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            verbose = 0;
            continue;
        }
    }
        
    // get the data size
    uint64_t maxdata;
    int r = toku_os_get_max_process_data_size(&maxdata);
    assert(r == 0);
    if (verbose) printf("maxdata=%" PRIu64 " 0x%" PRIx64 "\n", maxdata, maxdata);

    // check the data size
#if defined(__x86_64__) || defined(__aarch64__) || defined(__powerpc64__)
    assert(maxdata > (1ULL << 32));
#elif __i386__
    assert(maxdata < (1ULL << 32));
#else
    #error
#endif

    return 0;
}
