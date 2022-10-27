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

// test recovery of "hello" comments

#include "test.h"


static int 
run_test(void) {
    // leave this many bytes in file
    const int magic_begin_end_checkpoint_sz = 8 // "tokulogg" magic 8 byte header
                                             +4 // version
                                             +toku_log_begin_checkpoint_overhead
                                             +toku_log_end_checkpoint_overhead;

    int r;
    int trim = 1;
    toku_struct_stat st;

    while ( 1 ) {
        // setup the test dir
        toku_os_recursive_delete(TOKU_TEST_FILENAME);
        r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU); assert(r == 0);

        // create the log
        TOKULOGGER logger;
        BYTESTRING hello  = { (uint32_t) strlen("hello"), (char *) "hello" };
        BYTESTRING world  = { (uint32_t) strlen("world"), (char *) "world" };
        BYTESTRING there  = { (uint32_t) strlen("there"), (char *) "there" };
        r = toku_logger_create(&logger); assert(r == 0);
        r = toku_logger_open(TOKU_TEST_FILENAME, logger); assert(r == 0);
        LSN beginlsn;
        // all logs must contain a valid checkpoint
        toku_log_begin_checkpoint(logger, &beginlsn, true, 0, 0);
        toku_log_end_checkpoint(logger, NULL, true, beginlsn, 0, 0, 0);
        toku_log_comment(logger, NULL, true, 0, hello);
        toku_log_comment(logger, NULL, true, 0, world);
        toku_log_begin_checkpoint(logger, &beginlsn, true, 0, 0);
        toku_log_end_checkpoint(logger, NULL, true, beginlsn, 0, 0, 0);
        toku_log_comment(logger, NULL, true, 0, hello);
        toku_log_comment(logger, NULL, true, 0, there);
        toku_logger_close(&logger);

        // redirect stderr
        int devnul = open(DEV_NULL_FILE, O_WRONLY);
        assert(devnul>=0);
        r = toku_dup2(devnul, fileno(stderr)); 	    assert(r==fileno(stderr));
        r = close(devnul);                      assert(r==0);

        char fname[TOKU_PATH_MAX + 1];
        sprintf(fname,
                "%s/%s%d",
                TOKU_TEST_FILENAME,
                "log000000000000.tokulog",
                TOKU_LOG_VERSION);

        r = toku_stat(fname, &st, toku_uninstrumented);
        assert(r == 0);
        if (st.st_size - trim > magic_begin_end_checkpoint_sz) {
            r = truncate(fname, st.st_size - trim);
            CKERR(r);
        }
        else
            break;
        // run recovery
        r = tokuft_recover(NULL,
			   NULL_prepared_txn_callback,
			   NULL_keep_cachetable_callback,
			   NULL_logger,
			   TOKU_TEST_FILENAME, TOKU_TEST_FILENAME, 0, 0, 0, NULL, 0); 
        assert(r == 0);
        
        trim += 1;
    }
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    return 0;
}

int
test_main(int UU(argc), const char *UU(argv[])) {
    int r;
    r = run_test();
    return r;
}
