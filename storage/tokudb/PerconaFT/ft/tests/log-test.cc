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

int
test_main (int argc __attribute__((__unused__)),
	  const char *argv[] __attribute__((__unused__))) {
    int r;
    long long lognum;
    char logname[TOKU_PATH_MAX+1];
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);    assert(r==0);
    r = toku_logger_find_next_unused_log_file(TOKU_TEST_FILENAME,&lognum);
    assert(r==0 && lognum==0LL);

    mode_t mode = S_IRWXU + S_IRWXG + S_IRWXO;
    sprintf(logname, "%s/log01.tokulog%d", TOKU_TEST_FILENAME, TOKU_LOG_VERSION);
    r = open(logname, O_WRONLY + O_CREAT + O_BINARY, mode); assert(r>=0);
    r = close(r); assert(r==0);

    r = toku_logger_find_next_unused_log_file(TOKU_TEST_FILENAME,&lognum);
    assert(r==0 && lognum==2LL);
    
    sprintf(logname, "%s/log123456789012345.tokulog%d", TOKU_TEST_FILENAME, TOKU_LOG_VERSION);
    r = open(logname, O_WRONLY + O_CREAT + O_BINARY, mode); assert(r>=0);
    r = close(r); assert(r==0);
    r = toku_logger_find_next_unused_log_file(TOKU_TEST_FILENAME,&lognum);
    assert(r==0 && lognum==123456789012346LL);

    sprintf(logname, "%s/log3.tokulog%d", TOKU_TEST_FILENAME, TOKU_LOG_VERSION);
    r = open(logname, O_WRONLY + O_CREAT + O_BINARY, mode); assert(r>=0);
    r = close(r); assert(r==0);
    r = toku_logger_find_next_unused_log_file(TOKU_TEST_FILENAME,&lognum);
    assert(r==0 && lognum==123456789012346LL);

    toku_os_recursive_delete(TOKU_TEST_FILENAME);

    return 0;
}

