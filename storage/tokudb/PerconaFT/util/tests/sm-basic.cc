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

// test that basic scoped malloc works with a thread

#include <toku_portability.h>
#include <toku_assert.h>
#include <toku_pthread.h>
#include <util/scoped_malloc.h>

static void sm_test(void) {
    toku::scoped_malloc a(1);
    {
        toku::scoped_malloc b(2);
        {
            toku::scoped_malloc c(3);
        }
    }
}

static void *sm_test_f(void *arg) {
    sm_test();
    return arg;
}

int main(void) {
    toku_scoped_malloc_init();

    // run the test
    toku_pthread_t tid;
    int r;
    r = toku_pthread_create(
        toku_uninstrumented, &tid, nullptr, sm_test_f, nullptr);
    assert_zero(r);
    void *ret;
    r = toku_pthread_join(tid, &ret);
    assert_zero(r);

    toku_scoped_malloc_destroy();

    return 0;
}
