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

#include "logger/logcursor.h"
#include "test.h"

const int N = 2;

int
test_main (int argc, const char *argv[]) {
    default_parse_args(argc, argv);

    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);    assert(r==0);
    TOKULOGGER logger;
    LSN lsn = ZERO_LSN;

    int helloseq = 0;

    // create N log files with a hello message
    for (int i=0; i<N; i++) {
        r = toku_logger_create(&logger);
        assert(r == 0);

        r = toku_logger_open(TOKU_TEST_FILENAME, logger);
        assert(r == 0);

        char str[32];
        sprintf(str, "hello%d", helloseq++);
        BYTESTRING bs0 = { .len = (uint32_t) strlen(str), .data = str };
        toku_log_comment(logger, &lsn, 0, 0, bs0);

        r = toku_logger_close(&logger);
        assert(r == 0);
    }    

    // create N empty log files
    for (int i=0; i<N; i++) {
        r = toku_logger_create(&logger);
        assert(r == 0);

        r = toku_logger_open(TOKU_TEST_FILENAME, logger);
        assert(r == 0);

        r = toku_logger_close(&logger);
        assert(r == 0);
    }

    // create N log files with a hello message
    for (int i=0; i<N; i++) {
        r = toku_logger_create(&logger);
        assert(r == 0);

        r = toku_logger_open(TOKU_TEST_FILENAME, logger);
        assert(r == 0);

        char str[32];
        sprintf(str, "hello%d", helloseq++);
        BYTESTRING bs0 = { .len = (uint32_t) strlen(str), .data = str };
        toku_log_comment(logger, &lsn, 0, 0, bs0);

        r = toku_logger_close(&logger);
        assert(r == 0);
    }

    // verify the log forwards
    TOKULOGCURSOR lc = NULL;
    struct log_entry *le;

    r = toku_logcursor_create(&lc, TOKU_TEST_FILENAME);
    assert(r == 0 && lc != NULL);

    helloseq = 0;
    for (int i=0; i<2*N; i++) {

        r = toku_logcursor_next(lc, &le);
        assert(r == 0 && le->cmd == LT_comment);
        char expect[32];
        sprintf(expect, "hello%d", helloseq++);
        assert(le->u.comment.comment.len == strlen(expect) && memcmp(le->u.comment.comment.data, expect, le->u.comment.comment.len) == 0);
    }

    r = toku_logcursor_next(lc, &le);
    assert(r != 0);

    r = toku_logcursor_destroy(&lc);
    assert(r == 0 && lc == NULL);

    // verify the log backwards
    r = toku_logcursor_create(&lc, TOKU_TEST_FILENAME);
    assert(r == 0 && lc != NULL);

    helloseq = 2*N;
    for (int i=0; i<2*N; i++) {

        r = toku_logcursor_prev(lc, &le);
        assert(r == 0 && le->cmd == LT_comment);
        char expect[32];
        sprintf(expect, "hello%d", --helloseq);
        assert(le->u.comment.comment.len == strlen(expect) && memcmp(le->u.comment.comment.data, expect, le->u.comment.comment.len) == 0);
    }

    r = toku_logcursor_prev(lc, &le);
    assert(r != 0);

    r = toku_logcursor_destroy(&lc);
    assert(r == 0 && lc == NULL);

    toku_os_recursive_delete(TOKU_TEST_FILENAME);

    return 0;
}
