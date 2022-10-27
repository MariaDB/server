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

#include <test.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <toku_portability.h>
#include <toku_assert.h>
#include <toku_os.h>

static void
check_snprintf(int i) {
    char buf_before[8];
    char target[5];
    char buf_after[8];
    memset(target, 0xFF, sizeof(target));
    memset(buf_before, 0xFF, sizeof(buf_before));
    memset(buf_after, 0xFF, sizeof(buf_after));
    int64_t n = 1;
    
    int j;
    for (j = 0; j < i; j++) n *= 10;

    int bytes = snprintf(target, sizeof target, "%" PRId64, n);
    assert(bytes==i+1 ||
           (i+1>=(int)(sizeof target) && bytes>=(int)(sizeof target)));
    if (bytes>=(int)(sizeof target)) {
        //Overflow prevented by snprintf
        assert(target[sizeof target - 1] == '\0');
        assert(strlen(target)==sizeof target-1);
    }
    else {
        assert(target[bytes] == '\0');
        assert(strlen(target)==(size_t)bytes);
    }
}


int test_main(int argc __attribute__((__unused__)), char *const argv[] __attribute__((__unused__))) {
    int i;
    for (i = 0; i < 8; i++) {
        check_snprintf(i);
    }
    return 0;
}

