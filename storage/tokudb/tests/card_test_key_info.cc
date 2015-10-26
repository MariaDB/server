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

static void test_no_keys() {
    TABLE_SHARE s = { 0, 0, 0, NULL };
    TABLE t = { &s, NULL };
    assert(tokudb::compute_total_key_parts(&s) == 0);
    tokudb::set_card_in_key_info(&t, 0, NULL);
}

static void test_simple_pk() {
    const uint keys = 1;
    const uint key_parts = 1;
    uint64_t pk_rec_per_key[keys] = { 0 };
    KEY_INFO pk = { 0, key_parts, pk_rec_per_key, (char *) "PRIMARY" };
    TABLE_SHARE s = { 0, keys, key_parts, &pk };
    TABLE t = { &s, &pk };
    assert(tokudb::compute_total_key_parts(&s) == key_parts);
    uint64_t computed_rec_per_key[keys] = { 2 };
    tokudb::set_card_in_key_info(&t, keys, computed_rec_per_key);
    assert(t.key_info[0].rec_per_key[0] == 1);
}

static void test_pk_2() {
    const uint keys = 1;
    const uint key_parts = 2;
    uint64_t pk_rec_per_key[keys * key_parts] = { 0 };
    KEY_INFO pk = { 0, key_parts, pk_rec_per_key, (char *) "PRIMARY" };
    TABLE_SHARE s = { 0, keys, key_parts, &pk };
    TABLE t = { &s, &pk };
    assert(tokudb::compute_total_key_parts(&s) == key_parts);
    uint64_t computed_rec_per_key[keys * key_parts] = { 2, 3 };
    tokudb::set_card_in_key_info(&t, keys * key_parts, computed_rec_per_key);
    assert(t.key_info[0].rec_per_key[0] == 2);
    assert(t.key_info[0].rec_per_key[1] == 1);
}

static void test_simple_sk() {
    const uint keys = 1;
    const uint key_parts = 1;
    uint64_t sk_rec_per_key[keys] = { 0 };
    KEY_INFO sk = { 0, keys, sk_rec_per_key, (char *) "KEY" };
    TABLE_SHARE s = { MAX_KEY, keys, key_parts, &sk };
    TABLE t = { &s, &sk };
    assert(tokudb::compute_total_key_parts(&s) == 1);
    uint64_t computed_rec_per_key[keys] = { 2 };
    tokudb::set_card_in_key_info(&t, keys, computed_rec_per_key);
    assert(t.key_info[0].rec_per_key[0] == 2);
}

static void test_simple_unique_sk() {
    const uint keys = 1;
    uint64_t sk_rec_per_key[keys] = { 0 };
    KEY_INFO sk = { HA_NOSAME, keys, sk_rec_per_key, (char *) "KEY" };
    TABLE_SHARE s = { MAX_KEY, keys, keys, &sk };
    TABLE t = { &s, &sk };
    assert(tokudb::compute_total_key_parts(&s) == 1);
    uint64_t computed_rec_per_key[keys] = { 2 };
    tokudb::set_card_in_key_info(&t, keys, computed_rec_per_key);
    assert(t.key_info[0].rec_per_key[0] == 1);
}

static void test_simple_pk_sk() {
    const uint keys = 2;
    uint64_t rec_per_key[keys] = { 0 };
    KEY_INFO key_info[keys] = {
        { 0, 1, &rec_per_key[0], (char *) "PRIMARY" },
        { 0, 1, &rec_per_key[1], (char *) "KEY" },
    };
    TABLE_SHARE s = { 0, keys, keys, key_info };
    TABLE t = { &s, key_info };
    assert(tokudb::compute_total_key_parts(&s) == 2);
    uint64_t computed_rec_per_key[keys] = { 100, 200 };
    tokudb::set_card_in_key_info(&t, keys, computed_rec_per_key);
    assert(t.key_info[0].rec_per_key[0] == 1);
    assert(t.key_info[1].rec_per_key[0] == 200);
}

static void test_simple_sk_pk() {
    const uint keys = 2;
    uint64_t rec_per_key[keys] = { 0 };
    KEY_INFO key_info[keys] = {
        { 0, 1, &rec_per_key[0], (char *) "KEY" },
        { 0, 1, &rec_per_key[1], (char *) "PRIMARY" },
    };
    TABLE_SHARE s = { 1, keys, keys, key_info };
    TABLE t = { &s, key_info };
    assert(tokudb::compute_total_key_parts(&s) == 2);
    uint64_t computed_rec_per_key[keys] = { 100, 200 };
    tokudb::set_card_in_key_info(&t, keys, computed_rec_per_key);
    assert(t.key_info[0].rec_per_key[0] == 100);
    assert(t.key_info[1].rec_per_key[0] == 1);
}

int main() {
    test_no_keys();
    test_simple_pk();
    test_pk_2();
    test_simple_sk();
    test_simple_unique_sk();
    test_simple_pk_sk();
    test_simple_sk_pk();
    return 0;
}
