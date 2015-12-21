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

// test that an update calls back into the update function

#include "test.h"

const int envflags = DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE;

DB_ENV *env;

uint32_t four_byte_desc = 101;
uint64_t eight_byte_desc = 10101;


static void assert_desc_four (DB* db) {
    assert(db->descriptor->dbt.size == sizeof(four_byte_desc));
    assert(*(uint32_t *)(db->descriptor->dbt.data) == four_byte_desc);
}
static void assert_desc_eight (DB* db) {
    assert(db->descriptor->dbt.size == sizeof(eight_byte_desc));
    assert(*(uint32_t *)(db->descriptor->dbt.data) == eight_byte_desc);
}

static void checkpoint_callback_1(void * extra) {
    assert(extra == NULL);
    DB* db = NULL;

    DBT change_descriptor;
    memset(&change_descriptor, 0, sizeof(change_descriptor));
    change_descriptor.size = sizeof(eight_byte_desc);
    change_descriptor.data = &eight_byte_desc;

    { int chk_r = db_create(&db, env, 0); CKERR(chk_r); }
    { int chk_r = db->open(db, NULL, "foo.db", NULL, DB_BTREE, 0, 0666); CKERR(chk_r); }
    assert_desc_four(db);
    IN_TXN_COMMIT(env, NULL, txn_change, 0, {
            { int chk_r = db->change_descriptor(db, txn_change, &change_descriptor, 0); CKERR(chk_r); }
            assert_desc_eight(db);
        });
    assert_desc_eight(db);
    { int chk_r = db->close(db,0); CKERR(chk_r); }
}

static void setup (void) {
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    { int chk_r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(chk_r); }
    { int chk_r = db_env_create(&env, 0); CKERR(chk_r); }
    db_env_set_checkpoint_callback(checkpoint_callback_1, NULL);
    env->set_errfile(env, stderr);
    { int chk_r = env->open(env, TOKU_TEST_FILENAME, envflags, S_IRWXU+S_IRWXG+S_IRWXO); CKERR(chk_r); }
}

static void cleanup (void) {
    { int chk_r = env->close(env, 0); CKERR(chk_r); }
}

static void run_test(void) {
    DB* db = NULL;
    
    DBT orig_desc;
    memset(&orig_desc, 0, sizeof(orig_desc));
    orig_desc.size = sizeof(four_byte_desc);
    orig_desc.data = &four_byte_desc;
    // verify we can only set a descriptor with version 1
    IN_TXN_COMMIT(env, NULL, txn_create, 0, {
            { int chk_r = db_create(&db, env, 0); CKERR(chk_r); }
            assert(db->descriptor == NULL);
            { int chk_r = db->open(db, txn_create, "foo.db", NULL, DB_BTREE, DB_CREATE, 0666); CKERR(chk_r); }
            { int chk_r = db->change_descriptor(db, txn_create, &orig_desc, 0); CKERR(chk_r); }
            assert_desc_four(db);
        });
    assert_desc_four(db);
    { int chk_r = db->close(db,0); CKERR(chk_r); }

    { int chk_r = db_create(&db, env, 0); CKERR(chk_r); }
    { int chk_r = db->open(db, NULL, "foo.db", NULL, DB_BTREE, 0, 0666); CKERR(chk_r); }    
    assert_desc_four(db);
    { int chk_r = db->close(db,0); CKERR(chk_r); }

    { int chk_r = env->txn_checkpoint(env, 0, 0, 0); CKERR(chk_r); }

    { int chk_r = db_create(&db, env, 0); CKERR(chk_r); }
    { int chk_r = db->open(db, NULL, "foo.db", NULL, DB_BTREE, 0, 0666); CKERR(chk_r); }    
    assert_desc_eight(db);
    { int chk_r = db->close(db,0); CKERR(chk_r); }

    db = NULL;
}

int test_main (int argc, char * const argv[]) {
    parse_args(argc, argv);
    setup();
    run_test();
    cleanup();
    return 0;
}
