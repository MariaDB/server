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

#include "test.h"
#include "cachetable/cachetable.h"
#include <inttypes.h>

/* Test for #2755.  The ft_loader is using too much VM. */
static void test_cachetable_reservation (long size) {
    CACHETABLE ct;
    {
        toku_cachetable_create(&ct, size, ZERO_LSN, NULL);
    }
    {
        uint64_t r0 = toku_cachetable_reserve_memory(ct, 0.5, 0);
        uint64_t r0_bound = size/2 + size/16;
        uint64_t r1 = toku_cachetable_reserve_memory(ct, 0.5, 0);
        uint64_t r1_bound = r0_bound/2;
        uint64_t r2 = toku_cachetable_reserve_memory(ct, 0.5, 0);
        uint64_t r2_bound = r1_bound/2;
        if (verbose) printf("%10ld: r0=%10" PRIu64 " r1=%10" PRIu64 " r2=%10" PRIu64 "\n", size, r0, r1, r2);
        assert(r0 <= r0_bound);
        assert(r1 <= r1_bound);
        assert(r2 <= r2_bound);
        assert(r1 <= r0);
        assert(r2 <= r1);

        long unreservable_part = size * 0.25;
        assert(r0 <= (size - unreservable_part)*0.5);
        assert(r1 <= (size - unreservable_part - r0)*0.5);
        assert(r2 <= (size - unreservable_part - r0 -1)*0.5);
        toku_cachetable_release_reserved_memory(ct, r0);
        toku_cachetable_release_reserved_memory(ct, r1);
        toku_cachetable_release_reserved_memory(ct, r2);
    }
    {
        toku_cachetable_close(&ct);
    }
    
}

int test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    test_cachetable_reservation(1L<<28);
    test_cachetable_reservation(1LL<<33);
    test_cachetable_reservation(3L<<28);
    test_cachetable_reservation((3L<<28) - 107);
    return 0;
}
