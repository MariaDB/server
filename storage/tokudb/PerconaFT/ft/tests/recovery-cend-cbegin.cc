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

// run recovery on a log with an incomplete checkpoint 

#include "test.h"


static int 
run_test(void) {
    int r;

    // setup the test dir
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU); assert(r == 0);

    // create the log
    TOKULOGGER logger;
    r = toku_logger_create(&logger); assert(r == 0);
    r = toku_logger_open(TOKU_TEST_FILENAME, logger); assert(r == 0);
    LSN firstbegin = ZERO_LSN;
    toku_log_begin_checkpoint(logger, &firstbegin, true, 0, 0);
    assert(firstbegin.lsn != ZERO_LSN.lsn);
    toku_log_end_checkpoint(logger, NULL, false, firstbegin, 0, 0, 0);
    toku_log_begin_checkpoint(logger, NULL, true, 0, 0);
    r = toku_logger_close(&logger); assert(r == 0);

    if (!verbose) {
        // redirect stderr
        int devnul = open(DEV_NULL_FILE, O_WRONLY);
        assert(devnul >= 0);
        r = toku_dup2(devnul, fileno(stderr)); assert(r == fileno(stderr));
        r = close(devnul); assert(r == 0);
    }

    // run recovery
    r = tokuft_recover(NULL,
		       NULL_prepared_txn_callback,
		       NULL_keep_cachetable_callback,
		       NULL_logger, TOKU_TEST_FILENAME, TOKU_TEST_FILENAME,
                       toku_builtin_compare_fun,
                       NULL, NULL, NULL,
                       0);
    assert(r == 0);

    toku_os_recursive_delete(TOKU_TEST_FILENAME);

    return 0;
}

int
test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    int r;
    r = run_test();
    return r;
}
