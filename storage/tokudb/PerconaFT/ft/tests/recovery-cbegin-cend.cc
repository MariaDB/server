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


static int 
run_test(void) {
    int r;

    // setup the test dir
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU); assert(r == 0);

    // put begin and end checkpoint into separate log files
    TOKULOGGER logger;
    r = toku_logger_create(&logger); assert(r == 0);
    r = toku_logger_open(TOKU_TEST_FILENAME, logger); assert(r == 0);
    LSN beginlsn;
    toku_log_begin_checkpoint(logger, &beginlsn, false, 0, 0);
    r = toku_logger_close(&logger); assert(r == 0);

    r = toku_logger_create(&logger); assert(r == 0);
    r = toku_logger_open(TOKU_TEST_FILENAME, logger); assert(r == 0);
    toku_log_end_checkpoint(logger, NULL, true, beginlsn, 0, 0, 0);
    r = toku_logger_close(&logger); assert(r == 0);

    // run recovery
    r = tokuft_recover(NULL,
		       NULL_prepared_txn_callback,
		       NULL_keep_cachetable_callback,
		       NULL_logger, TOKU_TEST_FILENAME, TOKU_TEST_FILENAME, 0, 0, 0, NULL, 0); 
    assert(r == 0);

    toku_os_recursive_delete(TOKU_TEST_FILENAME);

    return 0;
}

int
test_main(int UU(argc), const char *UU(argv[])) {
    int r;
    r = run_test();
    return r;
}
