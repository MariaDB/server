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

// Test the first case for the bug in #1308 (ft-serialize.c:33 does the cast wrong)

#include "test.h"

#include <string.h>

#include <toku_portability.h>
#include "../ft-ops.h" 

#define FNAME "test1308a.data"

#define BUFSIZE (16<<20)

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__)))
{
    unlink(FNAME);
    
    int fd;
    {

	static uint64_t buf [BUFSIZE]; // make this static because it's too big to fit on the stack.

	fd = open(FNAME, O_CREAT+O_RDWR+O_BINARY, 0777);
	assert(fd>=0);
	memset(buf, 0, sizeof(buf));
	uint64_t i;
	for (i=0; i<(1LL<<32); i+=BUFSIZE) {
	    toku_os_full_write(fd, buf, BUFSIZE);
	}
    }
    int64_t file_size;
    {
        int r = toku_os_get_file_size(fd, &file_size);
        assert(r==0);
    }
    {
        int64_t size_after;
	toku_maybe_preallocate_in_file(fd, 1000, file_size, &size_after);
        assert(size_after == file_size);
    }
    int64_t file_size2;
    {
        int r = toku_os_get_file_size(fd, &file_size2);
        assert(r==0);
    }
    assert(file_size==file_size2);
    close(fd);

    unlink(FNAME);
    return 0;
}
