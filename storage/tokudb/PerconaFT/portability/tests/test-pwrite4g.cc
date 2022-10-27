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

/* Verify that toku_os_full_pwrite does the right thing when writing beyond 4GB.  */
#include <test.h>
#include <fcntl.h>
#include <toku_assert.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <portability/toku_path.h>

static int iszero(char *cp, size_t n) {
    size_t i;
    for (i=0; i<n; i++)
        if (cp[i] != 0) 
	    return 0;
    return 1;
}

int test_main(int UU(argc), char *const UU(argv[])) {
    int r;
    unlink(TOKU_TEST_FILENAME);
    int fd = open(TOKU_TEST_FILENAME, O_RDWR | O_CREAT | O_BINARY, S_IRWXU|S_IRWXG|S_IRWXO);
    assert(fd>=0);
    char *XMALLOC_N_ALIGNED(512, 512, buf);
    memset(buf, 0, 512);
    strcpy(buf, "hello");
    int64_t offset = (1LL<<32) + 512;
    toku_os_full_pwrite(fd, buf, 512, offset);
    char newbuf[512];
    r = pread(fd, newbuf, sizeof newbuf, 100);
    assert(r==sizeof newbuf);
    assert(iszero(newbuf, sizeof newbuf));
    r = pread(fd, newbuf, sizeof newbuf, offset);
    assert(r==sizeof newbuf);
    assert(memcmp(newbuf, buf, sizeof newbuf) == 0);
    int64_t fsize;
    r = toku_os_get_file_size(fd, &fsize);
    assert(r == 0);
    assert(fsize > 100 + 512);
    toku_free(buf);
    r = close(fd);
    assert(r==0);
    return 0;
}
