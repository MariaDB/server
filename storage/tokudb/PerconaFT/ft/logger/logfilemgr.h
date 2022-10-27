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

#include <ft/log_header.h>

// this is the basic information we need to keep per logfile
struct toku_logfile_info {
    int64_t index;
    LSN maxlsn;
    uint32_t version;
};
typedef struct toku_logfile_info *TOKULOGFILEINFO;

struct toku_logfilemgr;
typedef struct toku_logfilemgr *TOKULOGFILEMGR;

int toku_logfilemgr_create(TOKULOGFILEMGR *lfm);
int toku_logfilemgr_destroy(TOKULOGFILEMGR *lfm);

int toku_logfilemgr_init(TOKULOGFILEMGR lfm, const char *log_dir, TXNID *last_xid_if_clean_shutdown);
int toku_logfilemgr_num_logfiles(TOKULOGFILEMGR lfm);
int toku_logfilemgr_add_logfile_info(TOKULOGFILEMGR lfm, TOKULOGFILEINFO lf_info);
TOKULOGFILEINFO toku_logfilemgr_get_oldest_logfile_info(TOKULOGFILEMGR lfm);
void toku_logfilemgr_delete_oldest_logfile_info(TOKULOGFILEMGR lfm);
LSN toku_logfilemgr_get_last_lsn(TOKULOGFILEMGR lfm);
void toku_logfilemgr_update_last_lsn(TOKULOGFILEMGR lfm, LSN lsn);

void toku_logfilemgr_print(TOKULOGFILEMGR lfm);
