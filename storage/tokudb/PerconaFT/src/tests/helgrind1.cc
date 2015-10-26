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
// The helgrind1.tdbrun test should fail.  This is merely a check to verify that helgrind actually notices a race.

#include <pthread.h>
int x;

static void *starta(void* ignore __attribute__((__unused__))) {
    if (verbose) printf("%s %d\n", __FUNCTION__, x);
    x++;
    return 0;
}
static void *startb(void* ignore __attribute__((__unused__))) {
    if (verbose) printf("%s %d\n", __FUNCTION__, x);
    x++;
    return 0;
}

int
test_main (int argc, char * const argv[]) {
    parse_args(argc, argv);
    pthread_t a,b;
    { int x_l = pthread_create(&a, NULL, starta, NULL); assert(x_l==0); }
    { int x_l = pthread_create(&b, NULL, startb, NULL); assert(x_l==0); }
    { int x_l = pthread_join(a, NULL);           assert(x_l==0); }
    { int x_l = pthread_join(b, NULL);           assert(x_l==0); }
    return 0;
}
