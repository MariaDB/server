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

// Initialize the ydb library globals.  
// Called when the ydb library is loaded.
int toku_ydb_init(void);

// Called when the ydb library is unloaded.
void toku_ydb_destroy(void);

// db_env_create for the trace library
int db_env_create_toku10(DB_ENV **, uint32_t) __attribute__((__visibility__("default")));

// db_create for the trace library
int db_create_toku10(DB **, DB_ENV *, uint32_t) __attribute__((__visibility__("default")));

// test only function
extern "C" int toku_test_db_redirect_dictionary(DB * db, const char * dname_of_new_file, DB_TXN *dbtxn) __attribute__((__visibility__("default")));

extern "C" uint64_t toku_test_get_latest_lsn(DB_ENV *env) __attribute__((__visibility__("default")));

// test-only function
extern "C" int toku_test_get_checkpointing_user_data_status(void) __attribute__((__visibility__("default")));
