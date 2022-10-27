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
#include <toku_assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include "toku_portability.h"
#include <portability/toku_path.h>

int main(void) {
    int fd = toku_os_lock_file(TOKU_TEST_FILENAME);
    assert(fd != -1);
    pid_t pid = fork();
    assert(pid != -1);
    if (pid == 0) {
        int fd2 = toku_os_lock_file(TOKU_TEST_FILENAME);
        assert(fd2 == -1);
	return 0;
    } else {
        int status;
        pid_t wpid = waitpid(-1, &status, 0);
	assert(wpid == pid);
	assert(status == 0);
    }

    int r = toku_os_unlock_file(fd);
    assert(r == 0);

    return 0;
}
