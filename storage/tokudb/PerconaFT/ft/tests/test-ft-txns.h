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

#pragma once

static inline void
test_setup(const char *envdir, TOKULOGGER *loggerp, CACHETABLE *ctp) {
    *loggerp = NULL;
    *ctp = NULL;
    int r;
    toku_os_recursive_delete(envdir);
    r = toku_os_mkdir(envdir, S_IRWXU);
    CKERR(r);

    r = toku_logger_create(loggerp);
    CKERR(r);
    TOKULOGGER logger = *loggerp;

    r = toku_logger_open(envdir, logger);
    CKERR(r);

    toku_cachetable_create(ctp, 0, ZERO_LSN, logger);
    CACHETABLE ct = *ctp;
    toku_cachetable_set_env_dir(ct, envdir);

    toku_logger_set_cachetable(logger, ct);

    r = toku_logger_open_rollback(logger, ct, true);
    CKERR(r);

    CHECKPOINTER cp = toku_cachetable_get_checkpointer(*ctp);
    r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, STARTUP_CHECKPOINT);
    CKERR(r);
}

static inline void
xid_lsn_keep_cachetable_callback (DB_ENV *env, CACHETABLE cachetable) {
    CACHETABLE *CAST_FROM_VOIDP(ctp, (void *) env);
    *ctp = cachetable;
}

static inline void test_setup_and_recover(const char *envdir, TOKULOGGER *loggerp, CACHETABLE *ctp) {
    int r;
    TOKULOGGER logger = NULL;
    CACHETABLE ct = NULL;
    r = toku_logger_create(&logger);
    CKERR(r);

    DB_ENV *CAST_FROM_VOIDP(ctv, (void *) &ct);  // Use intermediate to avoid compiler warning.
    r = tokuft_recover(ctv,
                       NULL_prepared_txn_callback,
                       xid_lsn_keep_cachetable_callback,
                       logger,
                       envdir, envdir, 0, 0, 0, NULL, 0);
    CKERR(r);
    if (!toku_logger_is_open(logger)) {
        //Did not need recovery.
        invariant(ct==NULL);
        r = toku_logger_open(envdir, logger);
        CKERR(r);
        toku_cachetable_create(&ct, 0, ZERO_LSN, logger);
        toku_logger_set_cachetable(logger, ct);
    }
    *ctp = ct;
    *loggerp = logger;
}

static inline void clean_shutdown(TOKULOGGER *loggerp, CACHETABLE *ctp) {
    int r;
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(*ctp);
    r = toku_checkpoint(cp, *loggerp, NULL, NULL, NULL, NULL, SHUTDOWN_CHECKPOINT);
    CKERR(r);

    toku_logger_close_rollback(*loggerp);

    r = toku_checkpoint(cp, *loggerp, NULL, NULL, NULL, NULL, SHUTDOWN_CHECKPOINT);
    CKERR(r);

    toku_logger_shutdown(*loggerp);

    toku_cachetable_close(ctp);

    r = toku_logger_close(loggerp);
    CKERR(r);
}

static inline void shutdown_after_recovery(TOKULOGGER *loggerp, CACHETABLE *ctp) {
    toku_logger_close_rollback(*loggerp);
    toku_cachetable_close(ctp);
    int r = toku_logger_close(loggerp);
    CKERR(r);
}
