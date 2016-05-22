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

int test_main(int argc, char * const argv[])
{
    int r;
    DB * db;
    DB_ENV * db_env;
    (void) argc;
    (void) argv;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, 0755); { int chk_r = r; CKERR(chk_r); }

    // set things up
    r = db_env_create(&db_env, 0); { int chk_r = r; CKERR(chk_r); }
    r = db_env->open(db_env, TOKU_TEST_FILENAME, DB_CREATE|DB_INIT_MPOOL|DB_PRIVATE, 0755); { int chk_r = r; CKERR(chk_r); }
    r = db_create(&db, db_env, 0); { int chk_r = r; CKERR(chk_r); }
    r = db->open(db, NULL, "db", NULL, DB_BTREE, DB_CREATE, 0644); { int chk_r = r; CKERR(chk_r); }

    // - does not test low bounds, so a 0 byte key is "okay"
    // - assuming 32k keys and 32mb values are the max
    uint32_t max_key, max_val;
    db->get_max_row_size(db, &max_key, &max_val);
    // assume it is a red flag for the key to be outside the 16-32kb range
    assert(max_key >= 16*1024);
    assert(max_key <= 32*1024);
    // assume it is a red flag for the value to be outside the 16-32mb range
    assert(max_val >= 16*1024*1024);
    assert(max_val <= 32*1024*1024);

    // clean things up
    r = db->close(db, 0); { int chk_r = r; CKERR(chk_r); }
    r = db_env->close(db_env, 0); { int chk_r = r; CKERR(chk_r); }

    return 0;
}
