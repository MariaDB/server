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

#include "ft/ft.h"
#include "ft/ft-cachetable-wrappers.h"
#include "ft/ft-internal.h"
#include "ft/ft-flusher.h"
#include "ft/serialize/ft_node-serialize.h"
#include "ft/node.h"
#include "ft/ule.h"

// dummymsn needed to simulate msn because messages are injected at a lower level than toku_ft_root_put_msg()
#define MIN_DUMMYMSN ((MSN) {(uint64_t)1 << 62})
static MSN dummymsn;      
static int testsetup_initialized = 0;


// Must be called before any other test_setup_xxx() functions are called.
void
toku_testsetup_initialize(void) {
    if (testsetup_initialized == 0) {
        testsetup_initialized = 1;
        dummymsn = MIN_DUMMYMSN;
    }
}

static MSN
next_dummymsn(void) {
    ++(dummymsn.msn);
    return dummymsn;
}


bool ignore_if_was_already_open;
int toku_testsetup_leaf(FT_HANDLE ft_handle, BLOCKNUM *blocknum, int n_children, char **keys, int *keylens) {
    FTNODE node;
    assert(testsetup_initialized);
    toku_create_new_ftnode(ft_handle, &node, 0, n_children);
    for (int i = 0; i < n_children; i++) {
        BP_STATE(node, i) = PT_AVAIL;
    }

    DBT *XMALLOC_N(n_children - 1, pivotkeys);
    for (int i = 0; i + 1 < n_children; i++) {
        toku_memdup_dbt(&pivotkeys[i], keys[i], keylens[i]);
    }
    node->pivotkeys.create_from_dbts(pivotkeys, n_children - 1);
    for (int i = 0; i + 1 < n_children; i++) {
        toku_destroy_dbt(&pivotkeys[i]);
    }
    toku_free(pivotkeys);

    *blocknum = node->blocknum;
    toku_unpin_ftnode(ft_handle->ft, node);
    return 0;
}

// Don't bother to clean up carefully if something goes wrong.  (E.g., it's OK to have malloced stuff that hasn't been freed.)
int toku_testsetup_nonleaf (FT_HANDLE ft_handle, int height, BLOCKNUM *blocknum, int n_children, BLOCKNUM *children, char **keys, int *keylens) {
    FTNODE node;
    assert(testsetup_initialized);
    toku_create_new_ftnode(ft_handle, &node, height, n_children);
    for (int i = 0; i < n_children; i++) {
        BP_BLOCKNUM(node, i) = children[i];
        BP_STATE(node,i) = PT_AVAIL;
    }
    DBT *XMALLOC_N(n_children - 1, pivotkeys);
    for (int i = 0; i + 1 < n_children; i++) {
        toku_memdup_dbt(&pivotkeys[i], keys[i], keylens[i]);
    }
    node->pivotkeys.create_from_dbts(pivotkeys, n_children - 1);
    for (int i = 0; i + 1 < n_children; i++) {
        toku_destroy_dbt(&pivotkeys[i]);
    }
    toku_free(pivotkeys);

    *blocknum = node->blocknum;
    toku_unpin_ftnode(ft_handle->ft, node);
    return 0;
}

int toku_testsetup_root(FT_HANDLE ft_handle, BLOCKNUM blocknum) {
    assert(testsetup_initialized);
    ft_handle->ft->h->root_blocknum = blocknum;
    return 0;
}

int toku_testsetup_get_sersize(FT_HANDLE ft_handle, BLOCKNUM diskoff) // Return the size on disk
{
    assert(testsetup_initialized);
    void *node_v;
    ftnode_fetch_extra bfe;
    bfe.create_for_full_read(ft_handle->ft);
    int r  = toku_cachetable_get_and_pin(
        ft_handle->ft->cf, diskoff,
        toku_cachetable_hash(ft_handle->ft->cf, diskoff),
        &node_v,
        NULL,
        get_write_callbacks_for_node(ft_handle->ft),
        toku_ftnode_fetch_callback,
        toku_ftnode_pf_req_callback,
        toku_ftnode_pf_callback,
        true,
        &bfe
        );
    assert(r==0);
    FTNODE CAST_FROM_VOIDP(node, node_v);
    int size = toku_serialize_ftnode_size(node);
    toku_unpin_ftnode(ft_handle->ft, node);
    return size;
}

int toku_testsetup_insert_to_leaf (FT_HANDLE ft_handle, BLOCKNUM blocknum, const char *key, int keylen, const char *val, int vallen) {
    void *node_v;
    int r;

    assert(testsetup_initialized);

    ftnode_fetch_extra bfe;
    bfe.create_for_full_read(ft_handle->ft);
    r = toku_cachetable_get_and_pin(
        ft_handle->ft->cf,
        blocknum,
        toku_cachetable_hash(ft_handle->ft->cf, blocknum),
        &node_v,
        NULL,
        get_write_callbacks_for_node(ft_handle->ft),
	toku_ftnode_fetch_callback,
        toku_ftnode_pf_req_callback,
        toku_ftnode_pf_callback,
        true,
	&bfe
	);
    if (r!=0) return r;
    FTNODE CAST_FROM_VOIDP(node, node_v);
    toku_verify_or_set_counts(node);
    assert(node->height==0);

    DBT kdbt, vdbt;
    ft_msg msg(
        toku_fill_dbt(&kdbt, key, keylen),
        toku_fill_dbt(&vdbt, val, vallen),
        FT_INSERT,
        next_dummymsn(),
        toku_xids_get_root_xids());

    static size_t zero_flow_deltas[] = { 0, 0 };
    txn_gc_info gc_info(nullptr, TXNID_NONE, TXNID_NONE, true);
    toku_ftnode_put_msg(
        ft_handle->ft->cmp,
        ft_handle->ft->update_fun,
        node,
        -1,
        msg,
        true,
        &gc_info,
        zero_flow_deltas,
        NULL,
        NULL);

    toku_verify_or_set_counts(node);

    toku_unpin_ftnode(ft_handle->ft, node);
    return 0;
}

static int
testhelper_string_key_cmp(DB *UU(e), const DBT *a, const DBT *b)
{
    char *CAST_FROM_VOIDP(s, a->data), *CAST_FROM_VOIDP(t, b->data);
    return strcmp(s, t);
}


void
toku_pin_node_with_min_bfe(FTNODE* node, BLOCKNUM b, FT_HANDLE t)
{
    ftnode_fetch_extra bfe;
    bfe.create_for_min_read(t->ft);
    toku_pin_ftnode(
        t->ft, 
        b,
        toku_cachetable_hash(t->ft->cf, b),
        &bfe,
        PL_WRITE_EXPENSIVE,
        node,
        true
        );
}

int toku_testsetup_insert_to_nonleaf (FT_HANDLE ft_handle, BLOCKNUM blocknum, enum ft_msg_type msgtype, const char *key, int keylen, const char *val, int vallen) {
    void *node_v;
    int r;

    assert(testsetup_initialized);

    ftnode_fetch_extra bfe;
    bfe.create_for_full_read(ft_handle->ft);
    r = toku_cachetable_get_and_pin(
        ft_handle->ft->cf,
        blocknum,
        toku_cachetable_hash(ft_handle->ft->cf, blocknum),
        &node_v,
        NULL,
        get_write_callbacks_for_node(ft_handle->ft),
	toku_ftnode_fetch_callback,
        toku_ftnode_pf_req_callback,
        toku_ftnode_pf_callback,
        true,
	&bfe
        );
    if (r!=0) return r;
    FTNODE CAST_FROM_VOIDP(node, node_v);
    assert(node->height>0);

    DBT k;
    int childnum = toku_ftnode_which_child(node, toku_fill_dbt(&k, key, keylen), ft_handle->ft->cmp);

    XIDS xids_0 = toku_xids_get_root_xids();
    MSN msn = next_dummymsn();
    toku::comparator cmp;
    cmp.create(testhelper_string_key_cmp, nullptr);
    toku_bnc_insert_msg(BNC(node, childnum), key, keylen, val, vallen, msgtype, msn, xids_0, true, cmp);
    cmp.destroy();
    // Hack to get the test working. The problem is that this test
    // is directly queueing something in a FIFO instead of 
    // using ft APIs.
    node->max_msn_applied_to_node_on_disk = msn;
    node->dirty = 1;
    // Also hack max_msn_in_ft
    ft_handle->ft->h->max_msn_in_ft = msn;

    toku_unpin_ftnode(ft_handle->ft, node);
    return 0;
}
