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

#include <db.h>
#include <errno.h>

#include "portability/memory.h"
#include "portability/toku_portability.h"

#include "ft/logger/recover.h"
#include "ft/txn/rollback.h"
#include "ft/txn/txn.h"
#include "util/bytestring.h"

struct roll_entry;

static inline void toku_free_TXNID(TXNID txnid __attribute__((__unused__))) {}
static inline void toku_free_TXNID_PAIR(TXNID_PAIR txnid __attribute__((__unused__))) {}

static inline void toku_free_LSN(LSN lsn __attribute__((__unused__))) {}
static inline void toku_free_uint64_t(uint64_t u __attribute__((__unused__))) {}
static inline void toku_free_uint32_t(uint32_t u __attribute__((__unused__))) {}
static inline void toku_free_uint8_t(uint8_t u __attribute__((__unused__))) {}
static inline void toku_free_FILENUM(FILENUM u __attribute__((__unused__))) {}
static inline void toku_free_BLOCKNUM(BLOCKNUM u __attribute__((__unused__))) {}
static inline void toku_free_bool(bool u __attribute__((__unused__))) {}
static inline void toku_free_XIDP(XIDP xidp) { toku_free(xidp); }
static inline void toku_free_BYTESTRING(BYTESTRING val) { toku_free(val.data); }
static inline void toku_free_FILENUMS(FILENUMS val) { toku_free(val.filenums); }

int toku_maybe_upgrade_log (const char *env_dir, const char *log_dir, LSN * lsn_of_clean_shutdown, bool * upgrade_in_progress);
uint64_t toku_log_upgrade_get_footprint(void);
