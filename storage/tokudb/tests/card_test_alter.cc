/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

// test tokudb cardinality in status dictionary
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <memory.h>
#include <errno.h>
#include <sys/stat.h>
#include <db.h>
typedef unsigned long long ulonglong;
#include <tokudb_status.h>
#include <tokudb_buffer.h>

#include "fake_mysql.h"

#if __APPLE__
typedef unsigned long ulong;
#endif
#include <tokudb_card.h>

static void test_no_keys(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_no_keys", txn);
    assert(error == 0);

    const uint keys = 0;
    const uint key_parts = 0;
    TABLE_SHARE s = { MAX_KEY, keys, key_parts, NULL };

    error = tokudb::alter_card(status_db, txn, &s, &s);
    assert(error == 0);

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_keys(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_keys", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 3;
    const uint ta_key_parts = 1;
    const int ta_rec_per_keys = ta_keys * ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        1000, 2000, 3000,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, ta_key_parts, &ta_rec_per_key[0], (char *) "key_a" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_b" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &ta);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[ta_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, ta_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < ta_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == ta_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_drop_0(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_drop_0", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 3;
    const uint ta_key_parts = 1;
    const uint ta_rec_per_keys = ta_keys * ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        1000, 2000, 3000,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, ta_key_parts, &ta_rec_per_key[0], (char *) "key_a" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_b" },
        { 0, ta_key_parts, &ta_rec_per_key[2], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 2;
    const uint tb_key_parts = 1;
    const int tb_rec_per_keys = tb_keys * tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        2000, 3000,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, tb_key_parts, &tb_rec_per_key[0], (char *) "key_b" },
        { 0, tb_key_parts, &tb_rec_per_key[1], (char *) "key_c" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_drop_1(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_drop_1", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 3;
    const uint ta_key_parts = 1;
    const uint ta_rec_per_keys = ta_keys * ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        1000, 2000, 3000,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, ta_key_parts, &ta_rec_per_key[0], (char *) "key_a" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_b" },
        { 0, ta_key_parts, &ta_rec_per_key[2], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 2;
    const uint tb_key_parts = 1;
    const int tb_rec_per_keys = tb_keys * tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        1000, 3000,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, tb_key_parts, &tb_rec_per_key[0], (char *) "key_a" },
        { 0, tb_key_parts, &tb_rec_per_key[1], (char *) "key_c" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_drop_2(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_drop_2", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 3;
    const uint ta_key_parts = 1;
    const uint ta_rec_per_keys = ta_keys * ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        1000, 2000, 3000,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, ta_key_parts, &ta_rec_per_key[0], (char *) "key_a" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_b" },
        { 0, ta_key_parts, &ta_rec_per_key[2], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 2;
    const uint tb_key_parts = 1;
    const int tb_rec_per_keys = tb_keys * tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        1000, 2000,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, tb_key_parts, &tb_rec_per_key[0], (char *) "key_a" },
        { 0, tb_key_parts, &tb_rec_per_key[1], (char *) "key_b" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_drop_1_multiple_parts(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_drop_1_multiple_parts", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 3;
    const uint ta_key_parts = 1+2+3;
    const uint ta_rec_per_keys = ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        1000, 2000, 2001, 3000, 3001, 3002,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, 1, &ta_rec_per_key[0], (char *) "key_a" },
        { 0, 2, &ta_rec_per_key[0+1], (char *) "key_b" },
        { 0, 3, &ta_rec_per_key[0+1+2], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 2;
    const uint tb_key_parts = 1+3;
    const int tb_rec_per_keys = tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        1000, 3000, 3001, 3002,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, 1, &tb_rec_per_key[0], (char *) "key_a" },
        { 0, 3, &tb_rec_per_key[0+1], (char *) "key_c" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_add_0(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_add_0", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 2;
    const uint ta_key_parts = 1;
    const uint ta_rec_per_keys = ta_keys * ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        2000, 3000,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, ta_key_parts, &ta_rec_per_key[0], (char *) "key_b" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 3;
    const uint tb_key_parts = 1;
    const int tb_rec_per_keys = tb_keys * tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        0 /*not computed*/, 2000, 3000,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, tb_key_parts, &tb_rec_per_key[0], (char *) "key_a" },
        { 0, tb_key_parts, &tb_rec_per_key[1], (char *) "key_b" },
        { 0, tb_key_parts, &tb_rec_per_key[2], (char *) "key_c" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_add_1(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_add_1", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 2;
    const uint ta_key_parts = 1;
    const uint ta_rec_per_keys = ta_keys * ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        2000, 3000,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, ta_key_parts, &ta_rec_per_key[0], (char *) "key_b" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 3;
    const uint tb_key_parts = 1;
    const int tb_rec_per_keys = tb_keys * tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        2000, 0 /*not computed*/, 3000,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, tb_key_parts, &tb_rec_per_key[0], (char *) "key_b" },
        { 0, tb_key_parts, &tb_rec_per_key[1], (char *) "key_a" },
        { 0, tb_key_parts, &tb_rec_per_key[2], (char *) "key_c" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_add_2(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_add_2", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 2;
    const uint ta_key_parts = 1;
    const uint ta_rec_per_keys = ta_keys * ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        2000, 3000,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, ta_key_parts, &ta_rec_per_key[0], (char *) "key_b" },
        { 0, ta_key_parts, &ta_rec_per_key[1], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 3;
    const uint tb_key_parts = 1;
    const int tb_rec_per_keys = tb_keys * tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        2000, 3000, 0 /*not computed*/,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, tb_key_parts, &tb_rec_per_key[0], (char *) "key_b" },
        { 0, tb_key_parts, &tb_rec_per_key[1], (char *) "key_c" },
        { 0, tb_key_parts, &tb_rec_per_key[2], (char *) "key_a" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

static void test_add_0_multiple_parts(DB_ENV *env) {
    int error;

    DB_TXN *txn = NULL;
    error = env->txn_begin(env, NULL, &txn, 0);
    assert(error == 0);

    DB *status_db = NULL;
    error = tokudb::create_status(env, &status_db, "status_add_0_multiple_parts", txn);
    assert(error == 0);

    // define tables
    const uint ta_keys = 2;
    const uint ta_key_parts = 3+4;
    const uint ta_rec_per_keys = ta_key_parts;
    uint64_t ta_rec_per_key[ta_rec_per_keys] = {
        2000, 2001, 2002, 3000, 3001, 3002, 3003,
    };
    KEY_INFO ta_key_info[ta_rec_per_keys] = {
        { 0, 3, &ta_rec_per_key[0], (char *) "key_b" },
        { 0, 4, &ta_rec_per_key[3], (char *) "key_c" },
    };
    TABLE_SHARE ta = { MAX_KEY, ta_keys, ta_key_parts, ta_key_info };

    const uint tb_keys = 3;
    const uint tb_key_parts = 2+3+4;
    const int tb_rec_per_keys = tb_key_parts;
    uint64_t tb_rec_per_key[tb_rec_per_keys] = {
        0, 0 /*not computed*/, 2000, 2001, 2002, 3000, 3001, 3002, 3003,
    };
    KEY_INFO tb_key_info[tb_rec_per_keys] = {
        { 0, 2, &tb_rec_per_key[0], (char *) "key_a" },
        { 0, 3, &tb_rec_per_key[0+2], (char *) "key_b" },
        { 0, 4, &tb_rec_per_key[0+2+3], (char *) "key_c" },
    };
    TABLE_SHARE tb = { MAX_KEY, tb_keys, tb_key_parts, tb_key_info };

    // set initial cardinality
    error = tokudb::set_card_in_status(status_db, txn, ta_rec_per_keys, ta_rec_per_key);
    assert(error == 0);

    error = tokudb::alter_card(status_db, txn, &ta, &tb);
    assert(error == 0);

    // verify
    uint64_t current_rec_per_key[tb_rec_per_keys];
    error = tokudb::get_card_from_status(status_db, txn, tb_rec_per_keys, current_rec_per_key);
    assert(error == 0);
    for (uint i = 0; i < tb_rec_per_keys; i++) {
        assert(current_rec_per_key[i] == tb_rec_per_key[i]);
    }

    error = txn->commit(txn, 0);
    assert(error == 0);

    error = tokudb::close_status(&status_db);
    assert(error == 0);
}

int main() {
    int error;

    error = system("rm -rf " __FILE__ ".testdir");
    assert(error == 0);

    error = mkdir(__FILE__ ".testdir", S_IRWXU+S_IRWXG+S_IRWXO);
    assert(error == 0);

    DB_ENV *env = NULL;
    error = db_env_create(&env, 0);
    assert(error == 0);

    error = env->open(env, __FILE__ ".testdir", DB_INIT_MPOOL + DB_INIT_LOG + DB_INIT_LOCK + DB_INIT_TXN + DB_PRIVATE + DB_CREATE, S_IRWXU+S_IRWXG+S_IRWXO);
    assert(error == 0);

    test_no_keys(env);
    test_keys(env);
    test_drop_0(env);
    test_drop_1(env);
    test_drop_2(env);
    test_drop_1_multiple_parts(env);
    test_add_0(env);
    test_add_1(env);
    test_add_2(env);
    test_add_0_multiple_parts(env);

    error = env->close(env, 0);
    assert(error == 0);

    return 0;
}
