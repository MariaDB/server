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

/* The goal of this test.  Make sure that inserts stay behind deletes. */


#include "test.h"

#include <ft-cachetable-wrappers.h>
#include "ft-flusher.h"
#include "ft-flusher-internal.h"
#include "cachetable/checkpoint.h"

static TOKUTXN const null_txn = 0;

enum { NODESIZE = 1024, KSIZE=NODESIZE-100, TOKU_PSIZE=20 };

CACHETABLE ct;
FT_HANDLE t;

bool checkpoint_called;
bool checkpoint_callback_called;
toku_pthread_t checkpoint_tid;


// callback functions for toku_ft_flush_some_child
static bool
dont_destroy_bn(void* UU(extra))
{
    return false;
}

static bool recursively_flush_should_not_happen(FTNODE UU(child), void* UU(extra)) {
    assert(false);
}

static int child_to_flush(FT UU(h), FTNODE parent, void* UU(extra)) {
    assert(parent->height == 1);
    assert(parent->n_children == 2);
    return 0;
}

static void dummy_update_status(FTNODE UU(child), int UU(dirtied), void* UU(extra)) {
}


static void checkpoint_callback(void* UU(extra)) {
    usleep(1*1024*1024);
    checkpoint_callback_called = true;
}


static void *do_checkpoint(void *arg) {
    // first verify that checkpointed_data is correct;
    if (verbose) printf("starting a checkpoint\n");
    CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
    int r = toku_checkpoint(cp, NULL, checkpoint_callback, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert_zero(r);
    if (verbose) printf("completed a checkpoint\n");
    return arg;
}


static void flusher_callback(int state, void* extra) {
    int desired_state = *(int *)extra;
    if (verbose) {
        printf("state %d\n", state);
    }
    if (state == desired_state) {
        checkpoint_called = true;
        int r = toku_pthread_create(toku_uninstrumented,
                                    &checkpoint_tid,
                                    nullptr,
                                    do_checkpoint,
                                    nullptr);
        assert_zero(r);
        while (!checkpoint_callback_called) {
            usleep(1 * 1024 * 1024);
        }
    }
}

static void
doit (int state) {
    BLOCKNUM node_root;
    BLOCKNUM node_leaves[2];

    int r;
    checkpoint_called = false;
    checkpoint_callback_called = false;

    toku_flusher_thread_set_callback(flusher_callback, &state);
    
    toku_cachetable_create(&ct, 500*1024*1024, ZERO_LSN, nullptr);
    unlink("foo2.ft_handle");
    unlink("bar2.ft_handle");
    // note the basement node size is 5 times the node size
    // this is done to avoid rebalancing when writing a leaf
    // node to disk
    r = toku_open_ft_handle("foo2.ft_handle", 1, &t, NODESIZE, 5*NODESIZE, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    toku_testsetup_initialize();  // must precede any other toku_testsetup calls

    r = toku_testsetup_leaf(t, &node_leaves[0], 1, NULL, NULL);
    assert(r==0);

    r = toku_testsetup_leaf(t, &node_leaves[1], 1, NULL, NULL);
    assert(r==0);

    char* pivots[1];
    pivots[0] = toku_strdup("kkkkk");
    int pivot_len = 6;

    r = toku_testsetup_nonleaf(t, 1, &node_root, 2, node_leaves, pivots, &pivot_len);
    assert(r==0);

    r = toku_testsetup_root(t, node_root);
    assert(r==0);

    r = toku_testsetup_insert_to_leaf(
        t,
        node_leaves[0],
        "a",
        2,
        NULL,
        0
        );
    assert_zero(r);
    r = toku_testsetup_insert_to_leaf(
        t,
        node_leaves[1],
        "z",
        2,
        NULL,
        0
        );
    assert_zero(r);


    // at this point, we have inserted two leafentries,
    // one in each leaf node. A flush should invoke a merge
    struct flusher_advice fa;
    flusher_advice_init(
        &fa,
        child_to_flush,
        dont_destroy_bn,
        recursively_flush_should_not_happen,
        default_merge_child,
        dummy_update_status,
        default_pick_child_after_split,
        NULL
        );

    // hack to get merge going
    FTNODE node = NULL;
    toku_pin_node_with_min_bfe(&node, node_leaves[0], t);
    BLB_SEQINSERT(node, node->n_children-1) = false;
    toku_unpin_ftnode(t->ft, node);
    toku_pin_node_with_min_bfe(&node, node_leaves[1], t);
    BLB_SEQINSERT(node, node->n_children-1) = false;
    toku_unpin_ftnode(t->ft, node);

    
    ftnode_fetch_extra bfe;
    bfe.create_for_min_read(t->ft);
    toku_pin_ftnode_with_dep_nodes(
        t->ft, 
        node_root,
        toku_cachetable_hash(t->ft->cf, node_root),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        0,
        NULL,
        &node,
        true
        );
    assert(node->height == 1);
    assert(node->n_children == 2);

    // do the flush
    toku_ft_flush_some_child(t->ft, node, &fa);
    assert(checkpoint_callback_called);

    // now let's pin the root again and make sure it is has merged
    toku_pin_ftnode_with_dep_nodes(
        t->ft, 
        node_root,
        toku_cachetable_hash(t->ft->cf, node_root),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        0,
        NULL,
        &node,
        true
        );
    assert(node->height == 1);
    assert(node->n_children == 1);
    toku_unpin_ftnode(t->ft, node);

    void *ret;
    r = toku_pthread_join(checkpoint_tid, &ret); 
    assert_zero(r);

    //
    // now the dictionary has been checkpointed
    // copy the file to something with a new name,
    // open it, and verify that the state of what is
    // checkpointed is what we expect
    //

    r = system("cp foo2.ft_handle bar2.ft_handle ");
    assert_zero(r);

    FT_HANDLE c_ft;
    // note the basement node size is 5 times the node size
    // this is done to avoid rebalancing when writing a leaf
    // node to disk
    r = toku_open_ft_handle("bar2.ft_handle", 0, &c_ft, NODESIZE, 5*NODESIZE, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    //
    // now pin the root, verify that the state is what we expect
    //
    bfe.create_for_full_read(c_ft->ft);
    toku_pin_ftnode_with_dep_nodes(
        c_ft->ft, 
        node_root,
        toku_cachetable_hash(c_ft->ft->cf, node_root),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        0,
        NULL,
        &node,
        true
        );
    assert(node->height == 1);
    assert(!node->dirty);
    BLOCKNUM left_child, right_child;
    // cases where we expect the checkpoint to contain the merge
    if (state == ft_flush_aflter_merge || state == flt_flush_before_unpin_remove) {
        assert(node->n_children == 1);
        left_child = BP_BLOCKNUM(node,0);
    }
    else if (state == flt_flush_before_merge || state == flt_flush_before_pin_second_node_for_merge) {
        assert(node->n_children == 2);
        left_child = BP_BLOCKNUM(node,0);
        right_child = BP_BLOCKNUM(node,1);
    }
    else {
        assert(false);
    }
    toku_unpin_ftnode(c_ft->ft, node);

    // now let's verify the leaves are what we expect
    if (state == flt_flush_before_merge || state == flt_flush_before_pin_second_node_for_merge) {
        toku_pin_ftnode_with_dep_nodes(
            c_ft->ft, 
            left_child,
            toku_cachetable_hash(c_ft->ft->cf, left_child),
            &bfe,
            PL_WRITE_EXPENSIVE, 
            0,
            NULL,
            &node,
            true
            );
        assert(node->height == 0);
        assert(!node->dirty);
        assert(node->n_children == 1);
        assert(BLB_DATA(node, 0)->num_klpairs() == 1);
        toku_unpin_ftnode(c_ft->ft, node);

        toku_pin_ftnode_with_dep_nodes(
            c_ft->ft, 
            right_child,
            toku_cachetable_hash(c_ft->ft->cf, right_child),
            &bfe,
            PL_WRITE_EXPENSIVE, 
            0,
            NULL,
            &node,
            true
            );
        assert(node->height == 0);
        assert(!node->dirty);
        assert(node->n_children == 1);
        assert(BLB_DATA(node, 0)->num_klpairs() == 1);
        toku_unpin_ftnode(c_ft->ft, node);
    }
    else if (state == ft_flush_aflter_merge || state == flt_flush_before_unpin_remove) {
        toku_pin_ftnode_with_dep_nodes(
            c_ft->ft, 
            left_child,
            toku_cachetable_hash(c_ft->ft->cf, left_child),
            &bfe,
            PL_WRITE_EXPENSIVE, 
            0,
            NULL,
            &node,
            true
            );
        assert(node->height == 0);
        assert(!node->dirty);
        assert(node->n_children == 1);
        assert(BLB_DATA(node, 0)->num_klpairs() == 2);
        toku_unpin_ftnode(c_ft->ft, node);
    }
    else {
        assert(false);
    }


    DBT k;
    struct check_pair pair1 = {2, "a", 0, NULL, 0};
    r = toku_ft_lookup(c_ft, toku_fill_dbt(&k, "a", 2), lookup_checkf, &pair1);
    assert(r==0);
    struct check_pair pair2 = {2, "z", 0, NULL, 0};
    r = toku_ft_lookup(c_ft, toku_fill_dbt(&k, "z", 2), lookup_checkf, &pair2);
    assert(r==0);


    r = toku_close_ft_handle_nolsn(t, 0);    assert(r==0);
    r = toku_close_ft_handle_nolsn(c_ft, 0);    assert(r==0);
    toku_cachetable_close(&ct);
    toku_free(pivots[0]);
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    default_parse_args(argc, argv);
    doit(flt_flush_before_merge);
    doit(flt_flush_before_pin_second_node_for_merge);
    doit(flt_flush_before_unpin_remove);
    doit(ft_flush_aflter_merge);
    return 0;
}
