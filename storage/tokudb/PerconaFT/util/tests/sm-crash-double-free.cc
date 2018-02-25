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

// force a race between the scoped malloc global destructor and a thread variable destructor

#define TOKU_SCOPED_MALLOC_DEBUG 1
#include <toku_portability.h>
#include <toku_assert.h>
#include <toku_pthread.h>
#include <toku_race_tools.h>
#include <util/scoped_malloc.h>

volatile int state = 0;

static void sm_test(void) {
    toku::scoped_malloc a(1);
}

static void *sm_test_f(void *arg) {
    sm_test();
    state = 1;
    while (state != 2) sleep(1);
    return arg;
}

int main(void) {
    TOKU_VALGRIND_HG_DISABLE_CHECKING(&state, sizeof state);
    state = 0;
    toku_scoped_malloc_init();
    toku_pthread_t tid;
    int r;
    r = toku_pthread_create(
        toku_uninstrumented, &tid, nullptr, sm_test_f, nullptr);
    assert_zero(r);
    void *ret;
    while (state != 1)
        sleep(1);
    toku_scoped_malloc_destroy_set();
    state = 2;
    r = toku_pthread_join(tid, &ret);
    assert_zero(r);
    toku_scoped_malloc_destroy_key();
    return 0;
}
