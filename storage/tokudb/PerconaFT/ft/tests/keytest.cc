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
#include "ft.h"

static void
test_keycompare (void) {
    assert(toku_keycompare("a",1, "a",1)==0);
    assert(toku_keycompare("aa",2, "a",1)>0);
    assert(toku_keycompare("a",1, "aa",2)<0);
    assert(toku_keycompare("b",1, "aa",2)>0);
    assert(toku_keycompare("aa",2, "b",1)<0);
    assert(toku_keycompare("aaaba",5, "aaaba",5)==0);
    assert(toku_keycompare("aaaba",5, "aaaaa",5)>0);
    assert(toku_keycompare("aaaaa",5, "aaaba",5)<0);
    assert(toku_keycompare("aaaaa",3, "aaaba",3)==0);
    assert(toku_keycompare("\000\000\000\a", 4, "\000\000\000\004", 4)>0);
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);

    test_keycompare();
    if (verbose) printf("test ok\n");
    return 0;
}
