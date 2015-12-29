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

// verify that the log file trimmer does not delete the log file containing the
// begin checkpoint when the checkpoint log entries span multiple log files.

#include "logger/logcursor.h"
#include "test.h"

int
test_main (int argc __attribute__((__unused__)),
	  const char *argv[] __attribute__((__unused__))) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);    assert(r==0);

    TOKULOGGER logger;
    r = toku_logger_create(&logger); assert(r == 0);
    r = toku_logger_set_lg_max(logger, 32); assert(r == 0);
    r = toku_logger_open(TOKU_TEST_FILENAME, logger); assert(r == 0);
    BYTESTRING hello = (BYTESTRING) { 5, (char *) "hello"};
    LSN comment_lsn;
    toku_log_comment(logger, &comment_lsn, true, 0, hello);
    LSN begin_lsn;
    toku_log_begin_checkpoint(logger, &begin_lsn, true, 0, 0);
    LSN end_lsn;
    toku_log_end_checkpoint(logger, &end_lsn, true, begin_lsn, 0, 0, 0);
    toku_logger_maybe_trim_log(logger, begin_lsn);
    r = toku_logger_close(&logger); assert(r == 0);

    // verify all log entries prior the begin checkpoint are trimmed
    TOKULOGCURSOR lc = NULL;
    r = toku_logcursor_create(&lc, TOKU_TEST_FILENAME); assert(r == 0);
    struct log_entry *le = NULL;
    r = toku_logcursor_first(lc, &le); assert(r == 0);
    assert(le->cmd == LT_begin_checkpoint);
    r = toku_logcursor_destroy(&lc); assert(r == 0);
    
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    return 0;
}
