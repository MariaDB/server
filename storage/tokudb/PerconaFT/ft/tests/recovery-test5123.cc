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

#include "toku_os.h"
#include "cachetable/checkpoint.h"

#include "test-ft-txns.h"

static void test_5123(void) {
    TOKULOGGER logger;
    CACHETABLE ct;
    test_setup(TOKU_TEST_FILENAME, &logger, &ct);

    int r;
    TXNID_PAIR one = { (TXNID)1, TXNID_NONE};
    TXNID_PAIR two = { (TXNID)2, TXNID_NONE};
    TXNID_PAIR three = { (TXNID)3, TXNID_NONE};

    toku_log_xbegin(logger, NULL, false, one, TXNID_PAIR_NONE);
    toku_log_xbegin(logger, NULL, false, three, TXNID_PAIR_NONE);
    toku_log_xbegin(logger, NULL, false, two, TXNID_PAIR_NONE);

    toku_log_xcommit(logger, NULL, false, NULL, two);

    toku_logger_close_rollback(logger);

    toku_cachetable_close(&ct);
    // "Crash"
    r = toku_logger_close(&logger);
    CKERR(r);
    ct = NULL;
    logger = NULL;

    // "Recover"
    test_setup_and_recover(TOKU_TEST_FILENAME, &logger, &ct);

    shutdown_after_recovery(&logger, &ct);
}

int test_main (int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    test_5123();
    return 0;
}
