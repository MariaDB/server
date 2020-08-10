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
static void merge_should_not_happen(struct flusher_advice* UU(fa),
                              FT UU(h),
                              FTNODE UU(parent),
                              int UU(childnum),
                              FTNODE UU(child),
                              void* UU(extra))
{
    assert(false);
}

static bool recursively_flush_should_not_happen(FTNODE UU(child), void* UU(extra)) {
    assert(false);
}

static int child_to_flush(FT UU(h), FTNODE parent, void* UU(extra)) {
    assert(parent->height == 1);
    assert(parent->n_children == 1);
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
    bool after_child_pin = *(bool *)extra;
    if (verbose) {
        printf("state %d\n", state);
    }
    if ((state == flt_flush_before_child_pin && !after_child_pin) ||
        (state == ft_flush_aflter_child_pin && after_child_pin)) {
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
doit (bool after_child_pin) {
    BLOCKNUM node_leaf, node_root;

    int r;
    checkpoint_called = false;
    checkpoint_callback_called = false;

    toku_flusher_thread_set_callback(flusher_callback, &after_child_pin);
    
    toku_cachetable_create(&ct, 500*1024*1024, ZERO_LSN, nullptr);
    unlink("foo1.ft_handle");
    r = toku_open_ft_handle("foo1.ft_handle", 1, &t, NODESIZE, NODESIZE/2, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    toku_testsetup_initialize();  // must precede any other toku_testsetup calls

    r = toku_testsetup_leaf(t, &node_leaf, 1, NULL, NULL);
    assert(r==0);

    r = toku_testsetup_nonleaf(t, 1, &node_root, 1, &node_leaf, 0, 0);
    assert(r==0);

    r = toku_testsetup_root(t, node_root);
    assert(r==0);


    r = toku_testsetup_insert_to_nonleaf(
        t, 
        node_root, 
        FT_INSERT,
        "a",
        2,
        NULL,
        0
        );

    // at this point, we have inserted a message into
    // the root, and we wish to flush it, the leaf
    // should be empty

    struct flusher_advice fa;
    flusher_advice_init(
        &fa,
        child_to_flush,
        dont_destroy_bn,
        recursively_flush_should_not_happen,
        merge_should_not_happen,
        dummy_update_status,
        default_pick_child_after_split,
        NULL
        );
    
    FTNODE node = NULL;
    ftnode_fetch_extra bfe;
    bfe.create_for_min_read(t->ft);
    toku_pin_ftnode(
        t->ft, 
        node_root,
        toku_cachetable_hash(t->ft->cf, node_root),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        &node,
        true
        );
    assert(node->height == 1);
    assert(node->n_children == 1);
    assert(toku_bnc_nbytesinbuf(BNC(node, 0)) > 0);

    // do the flush
    toku_ft_flush_some_child(t->ft, node, &fa);
    assert(checkpoint_callback_called);

    // now let's pin the root again and make sure it is flushed
    toku_pin_ftnode(
        t->ft, 
        node_root,
        toku_cachetable_hash(t->ft->cf, node_root),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        &node,
        true
        );
    assert(node->height == 1);
    assert(node->n_children == 1);
    assert(toku_bnc_nbytesinbuf(BNC(node, 0)) == 0);
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

    r = system("cp foo1.ft_handle bar1.ft_handle ");
    assert_zero(r);

    FT_HANDLE c_ft;
    r = toku_open_ft_handle("bar1.ft_handle", 0, &c_ft, NODESIZE, NODESIZE/2, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    //
    // now pin the root, verify that we have a message in there, and that it is clean
    //
    bfe.create_for_full_read(c_ft->ft);
    toku_pin_ftnode(
        c_ft->ft, 
        node_root,
        toku_cachetable_hash(c_ft->ft->cf, node_root),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        &node,
        true
        );
    assert(node->height == 1);
    assert(!node->dirty());
    assert(node->n_children == 1);
    if (after_child_pin) {
        assert(toku_bnc_nbytesinbuf(BNC(node, 0)) == 0);
    }
    else {
        assert(toku_bnc_nbytesinbuf(BNC(node, 0)) > 0);
    }
    toku_unpin_ftnode(c_ft->ft, node);

    toku_pin_ftnode(
        c_ft->ft, 
        node_leaf,
        toku_cachetable_hash(c_ft->ft->cf, node_root),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        &node,
        true
        );
    assert(node->height == 0);
    assert(!node->dirty());
    assert(node->n_children == 1);
    if (after_child_pin) {
        assert(BLB_NBYTESINDATA(node,0) > 0);
    }
    else {
        assert(BLB_NBYTESINDATA(node,0) == 0);
    }
    toku_unpin_ftnode(c_ft->ft, node);

    struct check_pair pair1 = {2, "a", 0, NULL, 0};
    DBT k;
    r = toku_ft_lookup(c_ft, toku_fill_dbt(&k, "a", 2), lookup_checkf, &pair1);
    assert(r==0);


    r = toku_close_ft_handle_nolsn(t, 0);    assert(r==0);
    r = toku_close_ft_handle_nolsn(c_ft, 0);    assert(r==0);
    toku_cachetable_close(&ct);
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    default_parse_args(argc, argv);
    doit(false);
    doit(true);
    return 0;
}
