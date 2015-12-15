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

// test explicit generation of a simple template function

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <assert.h>

template <class T> T my_max(T a, T b) {
    return a > b ? a : b;
}

template int my_max(int a, int b);

int main(int argc, char *argv[]) {
    assert(argc == 3);
    int a = atoi(argv[1]);
    int b = atoi(argv[2]);
    int m = my_max<int>(a, b);
    printf("%d %d %d\n", a, b, m);
    assert(m == (a > b ? a : b));
    return 0;
}
