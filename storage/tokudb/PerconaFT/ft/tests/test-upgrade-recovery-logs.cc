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

// Test that recovery works correctly on a recovery log in a log directory.

#include "test.h"
#include <libgen.h>

static void run_recovery(const char *testdir) {
    int r;

    int log_version;
    char shutdown[32+1];
    r = sscanf(testdir, "upgrade-recovery-logs-%d-%32s", &log_version, shutdown);
    assert(r == 2);

    char **logfiles = nullptr;
    int n_logfiles = 0;
    r = toku_logger_find_logfiles(testdir, &logfiles, &n_logfiles);
    CKERR(r);
    assert(n_logfiles > 0);

    FILE *f = fopen(logfiles[n_logfiles-1], "r");
    assert(f);
    uint32_t real_log_version;
    r = toku_read_logmagic(f, &real_log_version);
    CKERR(r);
    assert((uint32_t)log_version == (uint32_t)real_log_version);
    r = fclose(f);
    CKERR(r);

    toku_logger_free_logfiles(logfiles, n_logfiles);

    // test needs recovery
    r = tokuft_needs_recovery(testdir, false);
    if (strcmp(shutdown, "clean") == 0) {
        CKERR(r); // clean does not need recovery
    } else if (strncmp(shutdown, "dirty", 5) == 0) {
        CKERR2(r, 1); // dirty needs recovery
    } else {
        CKERR(EINVAL);
    }

    // test maybe upgrade log
    LSN lsn_of_clean_shutdown;
    bool upgrade_in_progress;
    r = toku_maybe_upgrade_log(testdir, testdir, &lsn_of_clean_shutdown, &upgrade_in_progress);
    if (strcmp(shutdown, "dirty") == 0 && log_version <= 24) {
        CKERR2(r, TOKUDB_UPGRADE_FAILURE); // we don't support dirty upgrade from versions <= 24
        return;
    } else {
        CKERR(r);
    }

    if (!verbose) {
        // redirect stderr
        int devnul = open(DEV_NULL_FILE, O_WRONLY);
        assert(devnul >= 0);
        int rr = toku_dup2(devnul, fileno(stderr));
        assert(rr == fileno(stderr));
        rr = close(devnul);
        assert(rr == 0);
    }

    // run recovery
    if (r == 0) {
        r = tokuft_recover(NULL,
                           NULL_prepared_txn_callback,
                           NULL_keep_cachetable_callback,
                           NULL_logger, testdir, testdir, 0, 0, 0, NULL, 0);
        CKERR(r);
    }
}

int test_main(int argc, const char *argv[]) {
    int i = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            if (verbose > 0)
                verbose--;
            continue;
        }
        break;
    }
    if (i < argc) {
        const char *full_test_dir = argv[i];
        const char *test_dir = basename((char *)full_test_dir);
        if (strcmp(full_test_dir, test_dir) != 0) {
            int r;
            char cmd[32 + strlen(full_test_dir) + strlen(test_dir)];
            sprintf(cmd, "rm -rf %s", test_dir);
            r = system(cmd);
            CKERR(r);
            sprintf(cmd, "cp -r %s %s", full_test_dir, test_dir);
            r = system(cmd);
            CKERR(r);
        }
        run_recovery(test_dir);
    }
    return 0;
}
