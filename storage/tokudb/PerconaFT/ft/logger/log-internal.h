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

#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <dirent.h>

#include "portability/toku_list.h"
#include "portability/toku_pthread.h"
#include "ft/ft-internal.h"
#include "ft/logger/log.h"
#include "ft/logger/logfilemgr.h"
#include "ft/txn/txn.h"
#include "ft/txn/txn_manager.h"
#include "ft/txn/rollback_log_node_cache.h"

#include "util/memarena.h"
#include "util/omt.h"

using namespace toku;
// Locking for the logger
//  For most purposes we use the big ydb lock.
// To log: grab the buf lock
//  If the buf would overflow, then grab the file lock, swap file&buf, release buf lock, write the file, write the entry, release the file lock
//  else append to buf & release lock

#define LOGGER_MIN_BUF_SIZE (1<<24)

// TODO: Remove mylock, it has no value
struct mylock {
    toku_mutex_t lock;
};

static inline void ml_init(struct mylock *l) {
    toku_mutex_init(&l->lock, 0);
}
static inline void ml_lock(struct mylock *l) {
    toku_mutex_lock(&l->lock);
}
static inline void ml_unlock(struct mylock *l) {
    toku_mutex_unlock(&l->lock);
}
static inline void ml_destroy(struct mylock *l) {
    toku_mutex_destroy(&l->lock);
}

struct logbuf {
    int n_in_buf;
    int buf_size;
    char *buf;
    LSN  max_lsn_in_buf;
};

struct tokulogger {
    struct mylock  input_lock;

    toku_mutex_t output_condition_lock; // if you need both this lock and input_lock, acquire the output_lock first, then input_lock. More typical is to get the output_is_available condition to be false, and then acquire the input_lock.
    toku_cond_t  output_condition;      //
    bool output_is_available;           // this is part of the predicate for the output condition.  It's true if no thread is modifying the output (either doing an fsync or otherwise fiddling with the output).

    bool is_open;
    bool write_log_files;
    bool trim_log_files; // for test purposes
    char *directory;  // file system directory
    DIR *dir; // descriptor for directory
    int fd;
    CACHETABLE ct;
    int lg_max; // The size of the single file in the log.  Default is 100MB.

    // To access these, you must have the input lock
    LSN lsn; // the next available lsn
    struct logbuf inbuf; // data being accumulated for the write

    // To access these, you must have the output condition lock.
    LSN written_lsn; // the last lsn written
    LSN fsynced_lsn; // What is the LSN of the highest fsynced log entry  (accessed only while holding the output lock, and updated only when the output lock and output permission are held)
    LSN last_completed_checkpoint_lsn;     // What is the LSN of the most recent completed checkpoint.
    long long next_log_file_number;
    struct logbuf outbuf; // data being written to the file
    int  n_in_file; // The amount of data in the current file

    // To access the logfilemgr you must have the output condition lock.
    TOKULOGFILEMGR logfilemgr;

    uint32_t write_block_size;       // How big should the blocks be written to various logs?

    uint64_t num_writes_to_disk;         // how many times did we write to disk?
    uint64_t bytes_written_to_disk;        // how many bytes have been written to disk?
    tokutime_t time_spent_writing_to_disk; // how much tokutime did we spend writing to disk?
    uint64_t num_wait_buf_long;            // how many times we waited >= 100ms for the in buf

    CACHEFILE rollback_cachefile;
    rollback_log_node_cache rollback_cache;
    TXN_MANAGER txn_manager;
};

int toku_logger_find_next_unused_log_file(const char *directory, long long *result);
int toku_logger_find_logfiles (const char *directory, char ***resultp, int *n_logfiles);
void toku_logger_free_logfiles (char **logfiles, int n_logfiles);

static inline int
txn_has_current_rollback_log(TOKUTXN txn) {
    return txn->roll_info.current_rollback.b != ROLLBACK_NONE.b;
}

static inline int
txn_has_spilled_rollback_logs(TOKUTXN txn) {
    return txn->roll_info.spilled_rollback_tail.b != ROLLBACK_NONE.b;
}

struct txninfo {
    uint64_t   rollentry_raw_count;  // the total count of every byte in the transaction and all its children.
    uint32_t   num_fts;
    FT *open_fts;
    bool       force_fsync_on_commit;  //This transaction NEEDS an fsync once (if) it commits.  (commit means root txn)
    uint64_t   num_rollback_nodes;
    uint64_t   num_rollentries;
    BLOCKNUM   spilled_rollback_head;
    BLOCKNUM   spilled_rollback_tail;
    BLOCKNUM   current_rollback;
};

static inline int toku_logsizeof_uint8_t (uint32_t v __attribute__((__unused__))) {
    return 1;
}

static inline int toku_logsizeof_uint32_t (uint32_t v __attribute__((__unused__))) {
    return 4;
}

static inline int toku_logsizeof_uint64_t (uint32_t v __attribute__((__unused__))) {
    return 8;
}

static inline int toku_logsizeof_bool (uint32_t v __attribute__((__unused__))) {
    return 1;
}

static inline int toku_logsizeof_FILENUM (FILENUM v __attribute__((__unused__))) {
    return 4;
}

static inline int toku_logsizeof_DISKOFF (DISKOFF v __attribute__((__unused__))) {
    return 8;
}
static inline int toku_logsizeof_BLOCKNUM (BLOCKNUM v __attribute__((__unused__))) {
    return 8;
}

static inline int toku_logsizeof_LSN (LSN lsn __attribute__((__unused__))) {
    return 8;
}

static inline int toku_logsizeof_TXNID (TXNID txnid __attribute__((__unused__))) {
    return 8;
}

static inline int toku_logsizeof_TXNID_PAIR (TXNID_PAIR txnid __attribute__((__unused__))) {
    return 16;
}

static inline int toku_logsizeof_XIDP (XIDP xid) {
    assert(0<=xid->gtrid_length && xid->gtrid_length<=64);
    assert(0<=xid->bqual_length && xid->bqual_length<=64);
    return xid->gtrid_length
	+ xid->bqual_length
	+ 4  // formatID
	+ 1  // gtrid_length
	+ 1; // bqual_length
}

static inline int toku_logsizeof_FILENUMS (FILENUMS fs) {
    static const FILENUM f = {0}; //fs could have .num==0 and then we cannot dereference
    return 4 + fs.num * toku_logsizeof_FILENUM(f);
}

static inline int toku_logsizeof_BYTESTRING (BYTESTRING bs) {
    return 4+bs.len;
}

static inline char *fixup_fname(BYTESTRING *f) {
    assert(f->len>0);
    char *fname = (char*)toku_xmalloc(f->len+1);
    memcpy(fname, f->data, f->len);
    fname[f->len]=0;
    return fname;
}
