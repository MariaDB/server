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

#include <util/dbt.h>
#include <ft/ft-cachetable-wrappers.h>

// Each FT maintains a sequential insert heuristic to determine if its
// worth trying to insert directly into a well-known rightmost leaf node.
//
// The heuristic is only maintained when a rightmost leaf node is known.
//
// This test verifies that sequential inserts increase the seqinsert score
// and that a single non-sequential insert resets the score.

static void test_seqinsert_heuristic(void) {
    int r = 0;
    char name[TOKU_PATH_MAX + 1];
    toku_path_join(name, 2, TOKU_TEST_FILENAME, "ftdata");
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU); CKERR(r);
    
    FT_HANDLE ft_handle;
    CACHETABLE ct;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(name, 1, &ft_handle,
                            4*1024*1024, 64*1024,
                            TOKU_DEFAULT_COMPRESSION_METHOD, ct, NULL,
                            toku_builtin_compare_fun); CKERR(r);
    FT ft = ft_handle->ft;

    int k;
    DBT key, val;
    const int val_size = 1024 * 1024;
    char *XMALLOC_N(val_size, val_buf);
    memset(val_buf, 'x', val_size);
    toku_fill_dbt(&val, val_buf, val_size);

    // Insert many rows sequentially. This is enough data to:
    // - force the root to split (the righmost leaf will then be known)
    // - raise the seqinsert score high enough to enable direct rightmost injections
    const int rows_to_insert = 200;
    for (int i = 0; i < rows_to_insert; i++) {
        k = toku_htonl(i);
        toku_fill_dbt(&key, &k, sizeof(k));
        toku_ft_insert(ft_handle, &key, &val, NULL);
    }
    invariant(ft->rightmost_blocknum.b != RESERVED_BLOCKNUM_NULL);
    invariant(ft->seqinsert_score == FT_SEQINSERT_SCORE_THRESHOLD);

    // Insert on the left extreme. The seq insert score is high enough
    // that we will attempt to insert into the rightmost leaf. We won't
    // be successful because key 0 won't be in the bounds of the rightmost leaf.
    // This failure should reset the seqinsert score back to 0. 
    k = toku_htonl(0);
    toku_fill_dbt(&key, &k, sizeof(k));
    toku_ft_insert(ft_handle, &key, &val, NULL);
    invariant(ft->seqinsert_score == 0);

    // Insert in the middle. The score should not go up.
    k = toku_htonl(rows_to_insert / 2);
    toku_fill_dbt(&key, &k, sizeof(k));
    toku_ft_insert(ft_handle, &key, &val, NULL);
    invariant(ft->seqinsert_score == 0);

    // Insert on the right extreme. The score should go up.
    k = toku_htonl(rows_to_insert);
    toku_fill_dbt(&key, &k, sizeof(k));
    toku_ft_insert(ft_handle, &key, &val, NULL);
    invariant(ft->seqinsert_score == 1);

    // Insert again on the right extreme again, the score should go up.
    k = toku_htonl(rows_to_insert + 1);
    toku_fill_dbt(&key, &k, sizeof(k));
    toku_ft_insert(ft_handle, &key, &val, NULL);
    invariant(ft->seqinsert_score == 2);

    // Insert close to, but not at, the right extreme. The score should reset.
    // -- the magic number 4 derives from the fact that vals are 1mb and nodes are 4mb
    k = toku_htonl(rows_to_insert - 4);
    toku_fill_dbt(&key, &k, sizeof(k));
    toku_ft_insert(ft_handle, &key, &val, NULL);
    invariant(ft->seqinsert_score == 0);

    toku_free(val_buf);
    toku_ft_handle_close(ft_handle);
    toku_cachetable_close(&ct);
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
}

int test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    test_seqinsert_heuristic();
    return 0;
}
