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
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <toku_assert.h>
#include <toku_portability.h>

static void test_stat(const char *dirname, int result, int ex_errno) {
    int r;
    toku_struct_stat buf;
    r = toku_stat(dirname, &buf);
    //printf("stat %s %d %d\n", dirname, r, errno); fflush(stdout);
    assert(r==result);
    if (r!=0) assert(get_maybe_error_errno() == ex_errno);
}

int main(void) {
    int r;

    test_stat(".", 0, 0);
    test_stat("./", 0, 0);

    r = system("rm -rf testdir"); assert(r==0);
    test_stat("testdir", -1, ENOENT);
    test_stat("testdir/", -1, ENOENT);
    test_stat("testdir/foo", -1, ENOENT);
    test_stat("testdir/foo/", -1, ENOENT);
    r = toku_os_mkdir("testdir", S_IRWXU);
    assert(r == 0);
    test_stat("testdir/foo", -1, ENOENT);
    test_stat("testdir/foo/", -1, ENOENT);
    r = system("touch testdir/foo"); assert(r==0);
    test_stat("testdir/foo", 0, 0);
    test_stat("testdir/foo/", -1, ENOTDIR);

    test_stat("testdir", 0, 0);

    test_stat("./testdir", 0, 0);

    test_stat("./testdir/", 0, 0);

    test_stat("/", 0, 0);

    test_stat("/usr", 0, 0);
    test_stat("/usr/", 0, 0);

    return 0;
}
