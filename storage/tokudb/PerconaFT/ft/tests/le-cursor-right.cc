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

// test the LE_CURSOR toku_le_cursor_is_key_greater function
// - LE_CURSOR at neg infinity
// - LE_CURSOR at pos infinity
// - LE_CURSOR somewhere else


#include "cachetable/checkpoint.h"
#include "le-cursor.h"
#include "test.h"

static TOKUTXN const null_txn = 0;

static int
get_next_callback(uint32_t keylen, const void *key, uint32_t vallen UU(), const void *val UU(), void *extra, bool lock_only) {
    DBT *CAST_FROM_VOIDP(key_dbt, extra);
    if (!lock_only) {
        toku_dbt_set(keylen, key, key_dbt, NULL);
    }
    return 0;
}

static int
le_cursor_get_next(LE_CURSOR cursor, DBT *val) {
    int r = toku_le_cursor_next(cursor, get_next_callback, val);
    return r;
}

static int 
test_keycompare(DB* UU(desc), const DBT *a, const DBT *b) {
    return toku_keycompare(a->data, a->size, b->data, b->size);
}

// create a tree and populate it with n rows
static void
create_populate_tree(const char *logdir, const char *fname, int n) {
    if (verbose) fprintf(stderr, "%s %s %s %d\n", __FUNCTION__, logdir, fname, n);
    int error;

    TOKULOGGER logger = NULL;
    error = toku_logger_create(&logger);
    assert(error == 0);
    error = toku_logger_open(logdir, logger);
    assert(error == 0);
    CACHETABLE ct = NULL;
    toku_cachetable_create(&ct, 0, ZERO_LSN, logger);
    toku_logger_set_cachetable(logger, ct);
    error = toku_logger_open_rollback(logger, ct, true);
    assert(error == 0);

    TOKUTXN txn = NULL;
    error = toku_txn_begin_txn(NULL, NULL, &txn, logger, TXN_SNAPSHOT_NONE, false);
    assert(error == 0);

    FT_HANDLE ft = NULL;
    error = toku_open_ft_handle(fname, 1, &ft, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, txn, test_keycompare);
    assert(error == 0);

    error = toku_txn_commit_txn(txn, true, NULL, NULL);
    assert(error == 0);
    toku_txn_close_txn(txn);

    txn = NULL;
    error = toku_txn_begin_txn(NULL, NULL, &txn, logger, TXN_SNAPSHOT_NONE, false);
    assert(error == 0);

    // insert keys 0, 1, 2, .. (n-1)
    for (int i = 0; i < n; i++) {
        int k = toku_htonl(i);
        int v = i;
        DBT key;
        toku_fill_dbt(&key, &k, sizeof k);
        DBT val;
        toku_fill_dbt(&val, &v, sizeof v);
        toku_ft_insert(ft, &key, &val, txn);
    }

    error = toku_txn_commit_txn(txn, true, NULL, NULL);
    assert(error == 0);
    toku_txn_close_txn(txn);

    error = toku_close_ft_handle_nolsn(ft, NULL);
    assert(error == 0);

    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    error = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert(error == 0);
    toku_logger_close_rollback(logger);
    assert(error == 0);

    error = toku_checkpoint(cp, logger, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert(error == 0);

    toku_logger_shutdown(logger);

    error = toku_logger_close(&logger);
    assert(error == 0);

    toku_cachetable_close(&ct);
}

// test toku_le_cursor_is_key_greater when the LE_CURSOR is positioned at +infinity
static void 
test_pos_infinity(const char *fname, int n) {
    if (verbose) fprintf(stderr, "%s %s %d\n", __FUNCTION__, fname, n);
    int error;

    CACHETABLE ct = NULL;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);

    FT_HANDLE ft = NULL;
    error = toku_open_ft_handle(fname, 1, &ft, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, test_keycompare);
    assert(error == 0);

    // position the cursor at -infinity
    LE_CURSOR cursor = NULL;
    error = toku_le_cursor_create(&cursor, ft, NULL);
    assert(error == 0);

    for (int i = 0; i < 2*n; i++) {
        int k = toku_htonl(i);
        DBT key;
        toku_fill_dbt(&key, &k, sizeof k);
        int right = toku_le_cursor_is_key_greater_or_equal(cursor, &key);
        assert(right == false);
    }
        
    toku_le_cursor_close(cursor);

    error = toku_close_ft_handle_nolsn(ft, 0);
    assert(error == 0);

    toku_cachetable_close(&ct);
}

// test toku_le_cursor_is_key_greater when the LE_CURSOR is positioned at -infinity
static void 
test_neg_infinity(const char *fname, int n) {
    if (verbose) fprintf(stderr, "%s %s %d\n", __FUNCTION__, fname, n);
    int error;

    CACHETABLE ct = NULL;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);

    FT_HANDLE ft = NULL;
    error = toku_open_ft_handle(fname, 1, &ft, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, test_keycompare);
    assert(error == 0);

    // position the LE_CURSOR at +infinity
    LE_CURSOR cursor = NULL;
    error = toku_le_cursor_create(&cursor, ft, NULL);
    assert(error == 0);

    DBT key;
    toku_init_dbt(&key); key.flags = DB_DBT_REALLOC;
    DBT val;
    toku_init_dbt(&val); val.flags = DB_DBT_REALLOC;

    int i;
    for (i = n-1; ; i--) {
        error = le_cursor_get_next(cursor, &key);
        if (error != 0) 
            break;
        
        assert(key.size == sizeof (int));
        int ii = *(int *)key.data;
        assert((int) toku_htonl(i) == ii);
    }
    assert(i == -1);

    toku_destroy_dbt(&key);
    toku_destroy_dbt(&val);

    for (i = 0; i < 2*n; i++) {
        int k = toku_htonl(i);
        DBT key2;
        toku_fill_dbt(&key2, &k, sizeof k);
        int right = toku_le_cursor_is_key_greater_or_equal(cursor, &key2);
        assert(right == true);
    }

    toku_le_cursor_close(cursor);

    error = toku_close_ft_handle_nolsn(ft, 0);
    assert(error == 0);

    toku_cachetable_close(&ct);
}

// test toku_le_cursor_is_key_greater when the LE_CURSOR is positioned in between -infinity and +infinity
static void 
test_between(const char *fname, int n) {
    if (verbose) fprintf(stderr, "%s %s %d\n", __FUNCTION__, fname, n);
    int error;

    CACHETABLE ct = NULL;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);

    FT_HANDLE ft = NULL;
    error = toku_open_ft_handle(fname, 1, &ft, 1<<12, 1<<9, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, test_keycompare);
    assert(error == 0);

    // position the LE_CURSOR at +infinity
    LE_CURSOR cursor = NULL;
    error = toku_le_cursor_create(&cursor, ft, NULL);
    assert(error == 0);

    DBT key;
    toku_init_dbt(&key); key.flags = DB_DBT_REALLOC;
    DBT val;
    toku_init_dbt(&val); val.flags = DB_DBT_REALLOC;

    int i;
    for (i = 0; ; i++) {
        // move the LE_CURSOR forward
        error = le_cursor_get_next(cursor, &key);
        if (error != 0) 
            break;
        
        assert(key.size == sizeof (int));
        int ii = *(int *)key.data;
        assert((int) toku_htonl(n-i-1) == ii);

        // test 0 .. i-1 
        for (int j = 0; j <= i; j++) {
            int k = toku_htonl(n-j-1);
            DBT key2;
            toku_fill_dbt(&key2, &k, sizeof k);
            int right = toku_le_cursor_is_key_greater_or_equal(cursor, &key2);
            assert(right == true);
        }

        // test i .. n
        for (int j = i+1; j < n; j++) {
            int k = toku_htonl(n-j-1);
            DBT key2;
            toku_fill_dbt(&key2, &k, sizeof k);
            int right = toku_le_cursor_is_key_greater_or_equal(cursor, &key2);
            assert(right == false);
        }

    }
    assert(i == n);

    toku_destroy_dbt(&key);
    toku_destroy_dbt(&val);

    toku_le_cursor_close(cursor);

    error = toku_close_ft_handle_nolsn(ft, 0);
    assert(error == 0);

    toku_cachetable_close(&ct);
}

static void
init_logdir(const char *logdir) {
    int error;

    toku_os_recursive_delete(logdir);
    error = toku_os_mkdir(logdir, 0777);
    assert(error == 0);
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    int r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU);
    assert_zero(r);

    char logdir[TOKU_PATH_MAX+1];
    toku_path_join(logdir, 2, TOKU_TEST_FILENAME, "logdir");
    init_logdir(logdir);
    int error = chdir(logdir);
    assert(error == 0);

    const int n = 10;
    create_populate_tree(".", "ftfile", n);
    test_pos_infinity("ftfile", n);
    test_neg_infinity("ftfile", n);
    test_between("ftfile", n);

    return 0;
}
