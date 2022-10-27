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
#include <ft/ft-flusher.h>

// Promotion tracks the rightmost blocknum in the FT when a message
// is successfully promoted to a non-root leaf node on the right extreme.
//
// This test verifies that a split or merge of the rightmost leaf properly
// maintains the rightmost blocknum (which is constant - the pair's swap values,
// like the root blocknum).

static void test_split_merge(void) {
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

    // We have a root blocknum, but no rightmost blocknum yet.
    FT ft = ft_handle->ft;
    invariant(ft->h->root_blocknum.b != RESERVED_BLOCKNUM_NULL);
    invariant(ft->rightmost_blocknum.b == RESERVED_BLOCKNUM_NULL);

    int k;
    DBT key, val;
    const int val_size = 1 * 1024 * 1024;
    char *XMALLOC_N(val_size, val_buf);
    memset(val_buf, 'x', val_size);
    toku_fill_dbt(&val, val_buf, val_size);

    // Insert 16 rows (should induce a few splits)
    const int rows_to_insert = 16;
    for (int i = 0; i < rows_to_insert; i++) {
        k = toku_htonl(i);
        toku_fill_dbt(&key, &k, sizeof(k));
        toku_ft_insert(ft_handle, &key, &val, NULL);
    }

    // rightmost blocknum should be set, because the root split and promotion
    // did a rightmost insertion directly into the rightmost leaf, lazily
    // initializing the rightmost blocknum.
    invariant(ft->rightmost_blocknum.b != RESERVED_BLOCKNUM_NULL);

    BLOCKNUM root_blocknum = ft->h->root_blocknum;
    FTNODE root_node;
    ftnode_fetch_extra bfe;
    bfe.create_for_full_read(ft);
    toku_pin_ftnode(ft, root_blocknum,
                   toku_cachetable_hash(ft->cf, ft->h->root_blocknum),
                   &bfe, PL_WRITE_EXPENSIVE, &root_node, true);
    // root blocknum should be consistent
    invariant(root_node->blocknum.b == ft->h->root_blocknum.b);
    // root should have split at least once, and it should now be at height 1
    invariant(root_node->n_children > 1);
    invariant(root_node->height == 1);
    // rightmost blocknum should no longer be the root, since the root split
    invariant(ft->h->root_blocknum.b != ft->rightmost_blocknum.b);
    // the right child should have the rightmost blocknum
    invariant(BP_BLOCKNUM(root_node, root_node->n_children - 1).b == ft->rightmost_blocknum.b);

    BLOCKNUM rightmost_blocknum_before_merge = ft->rightmost_blocknum;
    const int num_children_before_merge = root_node->n_children;

    // delete the last 6 rows.
    // - 1mb each, so 6mb deleted 
    // - should be enough to delete the entire rightmost leaf + some of its neighbor
    const int rows_to_delete = 6;
    toku_unpin_ftnode(ft, root_node);
    for (int i = 0; i < rows_to_delete; i++) {
        k = toku_htonl(rows_to_insert - i);
        toku_fill_dbt(&key, &k, sizeof(k));
        toku_ft_delete(ft_handle, &key, NULL);
    }
    toku_pin_ftnode(ft, root_blocknum,
                    toku_cachetable_hash(ft->cf, root_blocknum),
                    &bfe, PL_WRITE_EXPENSIVE, &root_node, true);

    // - rightmost leaf should be fusible after those deletes (which were promoted directly to the leaf)
    FTNODE rightmost_leaf;
    toku_pin_ftnode(ft, rightmost_blocknum_before_merge,
                   toku_cachetable_hash(ft->cf, rightmost_blocknum_before_merge),
                   &bfe, PL_WRITE_EXPENSIVE, &rightmost_leaf, true);
    invariant(toku_ftnode_get_reactivity(ft, rightmost_leaf) == RE_FUSIBLE);
    toku_unpin_ftnode(ft, rightmost_leaf);

    // - merge the rightmost child now that it's fusible
    toku_ft_merge_child(ft, root_node, root_node->n_children - 1);
    toku_pin_ftnode(ft, root_blocknum,
                   toku_cachetable_hash(ft->cf, root_blocknum),
                   &bfe, PL_WRITE_EXPENSIVE, &root_node, true);

    // the merge should have worked, and the root should still be at height 1
    invariant(root_node->n_children < num_children_before_merge);
    invariant(root_node->height == 1);
    // the rightmost child of the root has the rightmost blocknum
    invariant(BP_BLOCKNUM(root_node, root_node->n_children - 1).b == ft->rightmost_blocknum.b);
    // the value for rightmost blocknum itself should not have changed
    // (we keep it constant, like the root blocknum)
    invariant(rightmost_blocknum_before_merge.b == ft->rightmost_blocknum.b);

    toku_unpin_ftnode(ft, root_node);

    toku_free(val_buf);
    toku_ft_handle_close(ft_handle);
    toku_cachetable_close(&ct);
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
}

int test_main(int argc, const char *argv[]) {
    default_parse_args(argc, argv);
    test_split_merge();
    return 0;
}
