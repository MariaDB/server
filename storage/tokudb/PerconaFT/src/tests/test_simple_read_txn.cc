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


static void test_read_txn_creation(DB_ENV* env, uint32_t iso_flags) {
    int r;
    DB_TXN* parent_txn = NULL;
    DB_TXN* child_txn = NULL;
    r = env->txn_begin(env, 0, &parent_txn, iso_flags);
    CKERR(r);
    r = env->txn_begin(env, parent_txn, &child_txn, iso_flags | DB_TXN_READ_ONLY);
    CKERR2(r, EINVAL);
    r = env->txn_begin(env, parent_txn, &child_txn, iso_flags);
    CKERR(r);    
    r = child_txn->commit(child_txn, 0);
    CKERR(r);
    r = parent_txn->commit(parent_txn, 0);
    CKERR(r);

    r = env->txn_begin(env, 0, &parent_txn, iso_flags | DB_TXN_READ_ONLY);
    CKERR(r);
    r = env->txn_begin(env, parent_txn, &child_txn, iso_flags | DB_TXN_READ_ONLY);
    CKERR(r);
    r = child_txn->commit(child_txn, 0);
    CKERR(r);
    r = env->txn_begin(env, parent_txn, &child_txn, iso_flags);
    CKERR(r);    
    r = child_txn->commit(child_txn, 0);
    CKERR(r);
    r = parent_txn->commit(parent_txn, 0);
    CKERR(r);

}

int test_main(int argc, char * const argv[])
{
    int r;
    DB_ENV * env;
    (void) argc;
    (void) argv;

    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, 0755); { int chk_r = r; CKERR(chk_r); }

    // set things up
    r = db_env_create(&env, 0); 
    CKERR(r);
    r = env->open(env, TOKU_TEST_FILENAME, DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE, 0755); 
    CKERR(r);

    test_read_txn_creation(env, 0);
    test_read_txn_creation(env, DB_TXN_SNAPSHOT);
    test_read_txn_creation(env, DB_READ_COMMITTED);
    test_read_txn_creation(env, DB_READ_UNCOMMITTED);

    r = env->close(env, 0); 
    CKERR(r);

    return 0;
}
