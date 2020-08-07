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

// generate a tree with duplicate pivots and check that ft->verify finds them


#include <ft-cachetable-wrappers.h>
#include "test.h"

static FTNODE
make_node(FT_HANDLE ft, int height) {
    FTNODE node = NULL;
    int n_children = (height == 0) ? 1 : 0;
    toku_create_new_ftnode(ft, &node, height, n_children);
    if (n_children) BP_STATE(node,0) = PT_AVAIL;
    return node;
}

static void
append_leaf(FTNODE leafnode, void *key, size_t keylen, void *val, size_t vallen) {
    assert(leafnode->height == 0);

    DBT thekey; toku_fill_dbt(&thekey, key, keylen);
    DBT theval; toku_fill_dbt(&theval, val, vallen);

    // get an index that we can use to create a new leaf entry
    uint32_t idx = BLB_DATA(leafnode, 0)->num_klpairs();

    // apply an insert to the leaf node
    MSN msn = next_dummymsn();
    ft_msg msg(&thekey, &theval, FT_INSERT, msn, toku_xids_get_root_xids());
    txn_gc_info gc_info(nullptr, TXNID_NONE, TXNID_NONE, false);
    toku_ft_bn_apply_msg_once(
        BLB(leafnode, 0),
        msg,
        idx,
        keylen,
        NULL,
        &gc_info,
        NULL,
        NULL,
        NULL);

    // don't forget to dirty the node
    leafnode->set_dirty();
}

static void
populate_leaf(FTNODE leafnode, int seq, int n, int *minkey, int *maxkey) {
    for (int i = 0; i < n; i++) {
        int k = htonl(seq + i);
        int v = seq + i;
        append_leaf(leafnode, &k, sizeof k, &v, sizeof v);
    }
    *minkey = htonl(seq);
    *maxkey = htonl(seq + n - 1);
}

static FTNODE
make_tree(FT_HANDLE ft, int height, int fanout, int nperleaf, int *seq, int *minkey, int *maxkey) {
    FTNODE node;
    if (height == 0) {
        node = make_node(ft, 0);
        populate_leaf(node, *seq, nperleaf, minkey, maxkey);
        *seq += nperleaf;
    } else {
        node = make_node(ft, height);
        int minkeys[fanout], maxkeys[fanout];
        for (int childnum = 0; childnum < fanout; childnum++) {
            FTNODE child = make_tree(ft, height-1, fanout, nperleaf, seq, &minkeys[childnum], &maxkeys[childnum]);
            if (childnum == 0) {
                toku_ft_nonleaf_append_child(node, child, NULL);
            } else {
                int k = maxkeys[0]; // use duplicate pivots, should result in a broken tree
                DBT pivotkey;
                toku_ft_nonleaf_append_child(node, child, toku_fill_dbt(&pivotkey, &k, sizeof k));
            }
            toku_unpin_ftnode(ft->ft, child);
        }
        *minkey = minkeys[0];
        *maxkey = maxkeys[0];
        for (int i = 1; i < fanout; i++) {
            if (memcmp(minkey, &minkeys[i], sizeof minkeys[i]) > 0)
                *minkey = minkeys[i];
            if (memcmp(maxkey, &maxkeys[i], sizeof maxkeys[i]) < 0)
                *maxkey = maxkeys[i];
        }
    }
    return node;
}

static UU() void
deleted_row(UU() DB *db, UU() DBT *key, UU() DBT *val) {
}

static void 
test_make_tree(int height, int fanout, int nperleaf, int do_verify) {
    int r;

    // cleanup
    const char *fname = TOKU_TEST_FILENAME;
    r = unlink(fname);
    if (r != 0) {
        assert(r == -1);
        assert(get_error_errno() == ENOENT);
    }

    // create a cachetable
    CACHETABLE ct = NULL;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);

    // create the ft
    TOKUTXN null_txn = NULL;
    FT_HANDLE ft = NULL;
    r = toku_open_ft_handle(fname, 1, &ft, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r == 0);

    // make a tree
    int seq = 0, minkey, maxkey;
    FTNODE newroot = make_tree(ft, height, fanout, nperleaf, &seq, &minkey, &maxkey);

    // discard the old root block
    // set the new root to point to the new tree
    toku_ft_set_new_root_blocknum(ft->ft, newroot->blocknum);

    // unpin the new root
    toku_unpin_ftnode(ft->ft, newroot);

    if (do_verify) {
        r = toku_verify_ft(ft);
        assert(r != 0);
    }

    // flush to the file system
    r = toku_close_ft_handle_nolsn(ft, 0);     
    assert(r == 0);

    // shutdown the cachetable
    toku_cachetable_close(&ct);
}

static int
usage(void) {
    return 1;
}

int
test_main (int argc , const char *argv[]) {
    int height = 1;
    int fanout = 3;
    int nperleaf = 8;
    int do_verify = 1;
    initialize_dummymsn();
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(arg, "-q") == 0) {
            verbose = 0;
            continue;
        }
        if (strcmp(arg, "--height") == 0 && i+1 < argc) {
            height = atoi(argv[++i]);
            continue;
        }
        if (strcmp(arg, "--fanout") == 0 && i+1 < argc) {
            fanout = atoi(argv[++i]);
            continue;
        }
        if (strcmp(arg, "--nperleaf") == 0 && i+1 < argc) {
            nperleaf = atoi(argv[++i]);
            continue;
        }
        if (strcmp(arg, "--verify") == 0 && i+1 < argc) {
            do_verify = atoi(argv[++i]);
            continue;
        }
        return usage();
    }
    test_make_tree(height, fanout, nperleaf, do_verify);
    return 0;
}
