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

// verify that closing the cachetable with an in progress prefetch works

#include "test.h"

struct reserve_filenum_test {
  void test_reserve_filenum();
  void test_reserve_filenum_active();
};

void reserve_filenum_test::test_reserve_filenum() {
    cachefile_list cfl;
    cfl.init();

    // set m_next_filenum_to_use.fileid
    cfl.m_next_filenum_to_use.fileid = (UINT32_MAX -2);

    FILENUM fn1 = cfl.reserve_filenum();
    assert(fn1.fileid == (UINT32_MAX - 2));

    FILENUM fn2 = cfl.reserve_filenum();
    assert(fn2.fileid == (UINT32_MAX - 1));

    // skip the reversed value UINT32_MAX and wrap around
    FILENUM fn3 = cfl.reserve_filenum();
    assert(fn3.fileid == 0U);

    FILENUM fn4 = cfl.reserve_filenum();
    assert(fn4.fileid == 1U);

    cfl.destroy();
}

void reserve_filenum_test::test_reserve_filenum_active() {
    cachefile_list cfl;
    cfl.init();

    // start the filenum space to UINT32_MAX - 1
    cfl.m_next_filenum_to_use.fileid = (UINT32_MAX -1);

    // reserve filenum UINT32_MAX-1
    FILENUM fn1 = cfl.reserve_filenum();
    assert(fn1.fileid == (UINT32_MAX - 1));
    cachefile cf1 = {};
    cf1.filenum = fn1;
    cf1.fileid = {0, 1};
    cfl.add_cf_unlocked(&cf1);

    // reset next filenum so that we test skipping UINT32_MAX
    cfl.m_next_filenum_to_use.fileid = (UINT32_MAX -1);

    // reserve filenum 0
    FILENUM fn2 = cfl.reserve_filenum();
    assert(fn2.fileid == 0);

    cachefile cf2 = {};
    cf2.filenum = fn2;
    cf2.fileid = {0, 2};
    cfl.add_cf_unlocked(&cf2);

    cfl.destroy();
}

int
test_main(int argc, const char *argv[]) {
    int r = 0;
    default_parse_args(argc, argv);
    reserve_filenum_test fn_test;

    // Run the tests.
    fn_test.test_reserve_filenum();
    fn_test.test_reserve_filenum_active();

    return r;
}
