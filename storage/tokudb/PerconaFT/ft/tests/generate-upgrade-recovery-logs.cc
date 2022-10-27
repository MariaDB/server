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

// Generate a recovery log with a checkpoint and an optional shutdown log entry.
// These logs will be used later to test recovery.

#include "test.h"

static void generate_recovery_log(const char *testdir, bool do_shutdown) {
    int r;

    // setup the test dir
    toku_os_recursive_delete(testdir);
    r = toku_os_mkdir(testdir, S_IRWXU);
    CKERR(r);

    // open the log
    TOKULOGGER logger;
    r = toku_logger_create(&logger);
    CKERR(r);
    r = toku_logger_open(testdir, logger);
    CKERR(r);

    // log checkpoint
    LSN beginlsn;
    toku_log_begin_checkpoint(logger, &beginlsn, false, 0, 0);
    toku_log_end_checkpoint(logger, nullptr, false, beginlsn, 0, 0, 0);

    // log shutdown
    if (do_shutdown) {
        toku_log_shutdown(logger, nullptr, true, 0, 0);
    }

    r = toku_logger_close(&logger);
    CKERR(r);
}

int test_main(int argc, const char *argv[]) {
    bool do_shutdown = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            if (verbose > 0)
                verbose--;
            continue;
        }
        if (strcmp(argv[i], "--clean") == 0) {
            do_shutdown = true;
            continue;
        }
        if (strcmp(argv[i], "--dirty") == 0) {
            do_shutdown = false;
            continue;
        }
    }
    char testdir[256];
    sprintf(testdir, "upgrade-recovery-logs-%d-%s", TOKU_LOG_VERSION, do_shutdown ? "clean" : "dirty");
    generate_recovery_log(testdir, do_shutdown);
    return 0;
}
