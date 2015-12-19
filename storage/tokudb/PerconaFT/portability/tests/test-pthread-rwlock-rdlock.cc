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
#include <unistd.h>
#include <string.h>
#include <memory.h>
#include <toku_assert.h>
#include <toku_pthread.h>
#include "test.h"

int test_main(int argc __attribute__((__unused__)), char *const argv[] __attribute__((__unused__))) {
    toku_pthread_rwlock_t rwlock;
    ZERO_STRUCT(rwlock);

    toku_pthread_rwlock_init(&rwlock, NULL);
    toku_pthread_rwlock_rdlock(&rwlock);
    toku_pthread_rwlock_rdlock(&rwlock);
    toku_pthread_rwlock_rdunlock(&rwlock);
    toku_pthread_rwlock_rdunlock(&rwlock);
    toku_pthread_rwlock_destroy(&rwlock);

    return 0;
}

