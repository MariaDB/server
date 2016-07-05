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

#include <my_global.h>
#include <ft/log_header.h>

#include "log-internal.h"
#include "logger/logcursor.h"
#include "cachetable/checkpoint.h"

static uint64_t footprint = 0;  // for debug and accountability

uint64_t
toku_log_upgrade_get_footprint(void) {
    return footprint;
}

// Footprint concept here is that each function increments a different decimal digit.
// The cumulative total shows the path taken for the upgrade.
// Each function must have a single return for this to work.
#define FOOTPRINT(x) function_footprint=(x*footprint_increment)
#define FOOTPRINTSETUP(increment) uint64_t function_footprint = 0; uint64_t footprint_increment=increment;
#define FOOTPRINTCAPTURE footprint+=function_footprint;


// return 0 if clean shutdown, TOKUDB_UPGRADE_FAILURE if not clean shutdown
static int
verify_clean_shutdown_of_log_version_current(const char *log_dir, LSN * last_lsn, TXNID *last_xid) {
    int rval = TOKUDB_UPGRADE_FAILURE;
    TOKULOGCURSOR cursor = NULL;
    int r;
    FOOTPRINTSETUP(100);

    FOOTPRINT(1);

    r = toku_logcursor_create(&cursor, log_dir);
    assert(r == 0);
    struct log_entry *le = NULL;
    r = toku_logcursor_last(cursor, &le);
    if (r == 0) {
        FOOTPRINT(2);
        if (le->cmd==LT_shutdown) {
            LSN lsn = le->u.shutdown.lsn;
            if (last_lsn) {
                *last_lsn = lsn;
            }
            if (last_xid) {
                *last_xid = le->u.shutdown.last_xid;
            }
            rval = 0;
        }
    }
    r = toku_logcursor_destroy(&cursor);
    assert(r == 0);
    FOOTPRINTCAPTURE;
    return rval;
}


// return 0 if clean shutdown, TOKUDB_UPGRADE_FAILURE if not clean shutdown
static int
verify_clean_shutdown_of_log_version_old(const char *log_dir, LSN * last_lsn, TXNID *last_xid, uint32_t version) {
    int rval = TOKUDB_UPGRADE_FAILURE;
    int r;
    FOOTPRINTSETUP(10);

    FOOTPRINT(1);

    int n_logfiles;
    char **logfiles;
    r = toku_logger_find_logfiles(log_dir, &logfiles, &n_logfiles);
    if (r!=0) return r;

    char *basename;
    TOKULOGCURSOR cursor;
    struct log_entry *entry;
    // Only look at newest log
    // basename points to first char after last / in file pathname
    basename = strrchr(logfiles[n_logfiles-1], '/') + 1;
    uint32_t version_name;
    long long index = -1;
    r = sscanf(basename, "log%lld.tokulog%u", &index, &version_name);
    assert(r==2);  // found index and version
    invariant(version_name == version);
    assert(version>=TOKU_LOG_MIN_SUPPORTED_VERSION);
    assert(version< TOKU_LOG_VERSION); //Must be old
    // find last LSN
    r = toku_logcursor_create_for_file(&cursor, log_dir, basename);
    if (r != 0) {
        goto cleanup_no_logcursor;
    }
    r = toku_logcursor_last(cursor, &entry);
    if (r != 0) {
        goto cleanup;
    }
    FOOTPRINT(2);
    //TODO: Remove this special case once FT_LAYOUT_VERSION_19 (and older) are not supported.
    if (version <= FT_LAYOUT_VERSION_19) {
        if (entry->cmd==LT_shutdown_up_to_19) {
            LSN lsn = entry->u.shutdown_up_to_19.lsn;
            if (last_lsn) {
                *last_lsn = lsn;
            }
            if (last_xid) {
                // Use lsn as last_xid.
                *last_xid = lsn.lsn;
            }
            rval = 0;
        }
    }
    else if (entry->cmd==LT_shutdown) {
        LSN lsn = entry->u.shutdown.lsn;
        if (last_lsn) {
            *last_lsn = lsn;
        }
        if (last_xid) {
            *last_xid = entry->u.shutdown.last_xid;
        }
        rval = 0;
    }
cleanup:
    r = toku_logcursor_destroy(&cursor);
    assert(r == 0);
cleanup_no_logcursor:
    toku_logger_free_logfiles(logfiles, n_logfiles);
    FOOTPRINTCAPTURE;
    return rval;
}


static int
verify_clean_shutdown_of_log_version(const char *log_dir, uint32_t version, LSN *last_lsn, TXNID *last_xid) {
    // return 0 if clean shutdown, TOKUDB_UPGRADE_FAILURE if not clean shutdown
    int r = 0;
    FOOTPRINTSETUP(1000);

    if (version < TOKU_LOG_VERSION)  {
        FOOTPRINT(1);
        r = verify_clean_shutdown_of_log_version_old(log_dir, last_lsn, last_xid, version);
    }
    else {
        FOOTPRINT(2);
        assert(version == TOKU_LOG_VERSION);
        r = verify_clean_shutdown_of_log_version_current(log_dir, last_lsn, last_xid);
    }
    FOOTPRINTCAPTURE;
    return r;
}


// Actually create a log file of the current version, making the environment be of the current version.
// TODO: can't fail
static int
upgrade_log(const char *env_dir, const char *log_dir, LSN last_lsn, TXNID last_xid) { // the real deal
    int r;
    FOOTPRINTSETUP(10000);

    LSN initial_lsn = last_lsn;
    initial_lsn.lsn++;
    CACHETABLE ct;
    TOKULOGGER logger;

    FOOTPRINT(1);

    { //Create temporary environment
        toku_cachetable_create(&ct, 1<<25, initial_lsn, NULL);
        toku_cachetable_set_env_dir(ct, env_dir);
        r = toku_logger_create(&logger);
        assert(r == 0);
        toku_logger_set_cachetable(logger, ct);
        r = toku_logger_open_with_last_xid(log_dir, logger, last_xid);
        assert(r==0);
    }
    { //Checkpoint
        CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
        r = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, UPGRADE_CHECKPOINT); //fsyncs log dir
        assert(r == 0);
    }
    { //Close cachetable and logger
        toku_logger_shutdown(logger);
        toku_cachetable_close(&ct);
        r = toku_logger_close(&logger);
        assert(r==0);
    }
    {
        r = verify_clean_shutdown_of_log_version(log_dir, TOKU_LOG_VERSION, NULL, NULL);
        assert(r==0);
    }
    FOOTPRINTCAPTURE;
    return 0;
}

// If log on disk is old (environment is old) and clean shutdown, then create log of current version,
// which will make the environment of the current version (and delete the old logs).
int
toku_maybe_upgrade_log(const char *env_dir, const char *log_dir, LSN * lsn_of_clean_shutdown, bool * upgrade_in_progress) {
    int r;
    int lockfd = -1;
    FOOTPRINTSETUP(100000);

    footprint = 0;
    *upgrade_in_progress = false;  // set true only if all criteria are met and we're actually doing an upgrade

    FOOTPRINT(1);
    r = toku_recover_lock(log_dir, &lockfd);
    if (r != 0) {
        goto cleanup_no_lock;
    }
    FOOTPRINT(2);
    assert(log_dir);
    assert(env_dir);

    uint32_t version_of_logs_on_disk;
    bool found_any_logs;
    r = toku_get_version_of_logs_on_disk(log_dir, &found_any_logs, &version_of_logs_on_disk);
    if (r != 0) {
        goto cleanup;
    }
    FOOTPRINT(3);
    if (!found_any_logs)
        r = 0; //No logs means no logs to upgrade.
    else if (version_of_logs_on_disk > TOKU_LOG_VERSION)
        r = TOKUDB_DICTIONARY_TOO_NEW;
    else if (version_of_logs_on_disk < TOKU_LOG_MIN_SUPPORTED_VERSION)
        r = TOKUDB_DICTIONARY_TOO_OLD;
    else if (version_of_logs_on_disk == TOKU_LOG_VERSION)
        r = 0; //Logs are up to date
    else {
        FOOTPRINT(4);
        LSN last_lsn = ZERO_LSN;
        TXNID last_xid = TXNID_NONE;
        r = verify_clean_shutdown_of_log_version(log_dir, version_of_logs_on_disk, &last_lsn, &last_xid);
        if (r != 0) {
            if (version_of_logs_on_disk >= TOKU_LOG_VERSION_25 &&
                version_of_logs_on_disk <= TOKU_LOG_VERSION_29 &&
                TOKU_LOG_VERSION_29 == TOKU_LOG_VERSION) {
                r = 0; // can do recovery on dirty shutdown
            } else {
                fprintf(stderr, "Cannot upgrade PerconaFT version %d database.", version_of_logs_on_disk);
                fprintf(stderr, "  Previous improper shutdown detected.\n");
            }
            goto cleanup;
        }
        FOOTPRINT(5);
        *lsn_of_clean_shutdown = last_lsn;
        *upgrade_in_progress = true;
        r = upgrade_log(env_dir, log_dir, last_lsn, last_xid);
    }
cleanup:
    {
        //Clean up
        int rc;
        rc = toku_recover_unlock(lockfd);
        if (r==0) r = rc;
    }
cleanup_no_lock:
    FOOTPRINTCAPTURE;
    return r;
}

