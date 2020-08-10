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

// it used to be the case that we copied the left and right keys of a
// range to be prelocked but never freed them, this test checks that they
// are freed (as of this time, this happens in ftnode_fetch_extra::destroy())

#include "test.h"


#include <ft-cachetable-wrappers.h>
#include <ft-flusher.h>

// Some constants to be used in calculations below
static const int nodesize = 1024; // Target max node size
static const int eltsize = 64;    // Element size (for most elements)
static const int bnsize = 256;    // Target basement node size
static const int eltsperbn = 256 / 64;  // bnsize / eltsize
static const int keylen = sizeof(long);
// vallen is eltsize - keylen and leafentry overhead
static const int vallen = 64 - sizeof(long) - (sizeof(((LEAFENTRY)NULL)->type)  // overhead from LE_CLEAN_MEMSIZE
                                               +sizeof(uint32_t)
                                               +sizeof(((LEAFENTRY)NULL)->u.clean.vallen));
#define dummy_msn_3884 ((MSN) { (uint64_t) 3884 * MIN_MSN.msn })

static TOKUTXN const null_txn = 0;
static const char *fname = TOKU_TEST_FILENAME;

static void
le_add_to_bn(bn_data* bn, uint32_t idx, const  char *key, int keysize, const char *val, int valsize)
{
    LEAFENTRY r = NULL;
    uint32_t size_needed = LE_CLEAN_MEMSIZE(valsize);
    void *maybe_free = nullptr;
    bn->get_space_for_insert(
        idx, 
        key,
        keysize,
        size_needed,
        &r,
        &maybe_free
        );
    if (maybe_free) {
        toku_free(maybe_free);
    }
    resource_assert(r);
    r->type = LE_CLEAN;
    r->u.clean.vallen = valsize;
    memcpy(r->u.clean.val, val, valsize);
}


static size_t
insert_dummy_value(FTNODE node, int bn, long k, uint32_t idx)
{
    char val[vallen];
    memset(val, k, sizeof val);
    le_add_to_bn(BLB_DATA(node, bn), idx,(char *) &k, keylen, val, vallen);
    return LE_CLEAN_MEMSIZE(vallen) + keylen + sizeof(uint32_t);
}

// TODO: this stuff should be in ft/ft-ops.cc, not in a test.
// it makes it incredibly hard to add things to an ftnode
// when tests hard code initializations...
static void
setup_ftnode_header(struct ftnode *node)
{
    node->flags = 0x11223344;
    node->blocknum.b = 20;
    node->layout_version = FT_LAYOUT_VERSION;
    node->layout_version_original = FT_LAYOUT_VERSION;
    node->height = 0;
    node->set_dirty();
    node->oldest_referenced_xid_known = TXNID_NONE;
}

static void
setup_ftnode_partitions(struct ftnode *node, int n_children, const MSN msn, size_t maxbnsize UU())
{
    node->n_children = n_children;
    node->max_msn_applied_to_node_on_disk = msn;
    MALLOC_N(node->n_children, node->bp);
    for (int bn = 0; bn < node->n_children; ++bn) {
        BP_STATE(node, bn) = PT_AVAIL;
        set_BLB(node, bn, toku_create_empty_bn());
        BLB_MAX_MSN_APPLIED(node, bn) = msn;
    }
    node->pivotkeys.create_empty();
}

static void
verify_basement_node_msns(FTNODE node, MSN expected)
{
    for(int i = 0; i < node->n_children; ++i) {
        assert(expected.msn == BLB_MAX_MSN_APPLIED(node, i).msn);
    }
}

//
// Maximum node size according to the FT: 1024 (expected node size after split)
// Maximum basement node size: 256
// Actual node size before split: 2048
// Actual basement node size before split: 256
// Start by creating 8 basements, then split node, expected result of two nodes with 4 basements each.
static void
test_split_on_boundary(void)
{
    struct ftnode sn;

    int fd = open(fname, O_RDWR|O_CREAT|O_BINARY, S_IRWXU|S_IRWXG|S_IRWXO); assert(fd >= 0);

    int r;

    setup_ftnode_header(&sn);
    const int nelts = 2 * nodesize / eltsize;
    setup_ftnode_partitions(&sn, nelts * eltsize / bnsize, dummy_msn_3884, bnsize);
    for (int bn = 0; bn < sn.n_children; ++bn) {
        long k;
        for (int i = 0; i < eltsperbn; ++i) {
            k = bn * eltsperbn + i;
            insert_dummy_value(&sn, bn, k, i);
        }
        if (bn < sn.n_children - 1) {
            DBT pivotkey;
            sn.pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)), bn);
        }
    }

    unlink(fname);
    CACHETABLE ct;
    FT_HANDLE ft;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &ft, nodesize, bnsize, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun); assert(r==0);

    FTNODE nodea, nodeb;
    DBT splitk;
    // if we haven't done it right, we should hit the assert in the top of move_leafentries
    ftleaf_split(ft->ft, &sn, &nodea, &nodeb, &splitk, true, SPLIT_EVENLY, 0, NULL);

    verify_basement_node_msns(nodea, dummy_msn_3884);
    verify_basement_node_msns(nodeb, dummy_msn_3884);

    toku_unpin_ftnode(ft->ft, nodeb);
    r = toku_close_ft_handle_nolsn(ft, NULL); assert(r == 0);
    toku_cachetable_close(&ct);

    toku_destroy_dbt(&splitk);
    toku_destroy_ftnode_internals(&sn);
}

//
// Maximum node size according to the FT: 1024 (expected node size after split)
// Maximum basement node size: 256 (except the last)
// Actual node size before split: 4095
// Actual basement node size before split: 256 (except the last, of size 2K)
// 
// Start by creating 9 basements, the first 8 being of 256 bytes each,
// and the last with one row of size 2047 bytes.  Then split node,
// expected result is two nodes, one with 8 basement nodes and one
// with 1 basement node.
static void
test_split_with_everything_on_the_left(void)
{
    struct ftnode sn;

    int fd = open(fname, O_RDWR|O_CREAT|O_BINARY, S_IRWXU|S_IRWXG|S_IRWXO); assert(fd >= 0);

    int r;

    setup_ftnode_header(&sn);
    const int nelts = 2 * nodesize / eltsize;
    setup_ftnode_partitions(&sn, nelts * eltsize / bnsize + 1, dummy_msn_3884, 2 * nodesize);
    size_t big_val_size = 0;
    for (int bn = 0; bn < sn.n_children; ++bn) {
        long k;
        if (bn < sn.n_children - 1) {
            for (int i = 0; i < eltsperbn; ++i) {
                k = bn * eltsperbn + i;
                big_val_size += insert_dummy_value(&sn, bn, k, i);
            }
            DBT pivotkey;
            sn.pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)), bn);
        } else {
            k = bn * eltsperbn;
            // we want this to be as big as the rest of our data and a
            // little bigger, so the halfway mark will land inside this
            // value and it will be split to the left
            big_val_size += 100;
            char * XMALLOC_N(big_val_size, big_val);
            memset(big_val, k, big_val_size);
            le_add_to_bn(BLB_DATA(&sn, bn), 0, (char *) &k, keylen, big_val, big_val_size);
            toku_free(big_val);
        }
    }

    unlink(fname);
    CACHETABLE ct;
    FT_HANDLE ft;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &ft, nodesize, bnsize, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun); assert(r==0);

    FTNODE nodea, nodeb;
    DBT splitk;
    // if we haven't done it right, we should hit the assert in the top of move_leafentries
    ftleaf_split(ft->ft, &sn, &nodea, &nodeb, &splitk, true, SPLIT_EVENLY, 0, NULL);

    toku_unpin_ftnode(ft->ft, nodeb);
    r = toku_close_ft_handle_nolsn(ft, NULL); assert(r == 0);
    toku_cachetable_close(&ct);

    toku_destroy_dbt(&splitk);
    toku_destroy_ftnode_internals(&sn);
}


//
// Maximum node size according to the FT: 1024 (expected node size after split)
// Maximum basement node size: 256 (except the last)
// Actual node size before split: 4095
// Actual basement node size before split: 256 (except the last, of size 2K)
// 
// Start by creating 9 basements, the first 8 being of 256 bytes each,
// and the last with one row of size 2047 bytes.  Then split node,
// expected result is two nodes, one with 8 basement nodes and one
// with 1 basement node.
static void
test_split_on_boundary_of_last_node(void)
{
    struct ftnode sn;

    int fd = open(fname, O_RDWR|O_CREAT|O_BINARY, S_IRWXU|S_IRWXG|S_IRWXO); assert(fd >= 0);

    int r;

    setup_ftnode_header(&sn);
    const int nelts = 2 * nodesize / eltsize;
    const size_t maxbnsize = 2 * nodesize;
    setup_ftnode_partitions(&sn, nelts * eltsize / bnsize + 1, dummy_msn_3884, maxbnsize);
    size_t big_val_size = 0;
    for (int bn = 0; bn < sn.n_children; ++bn) {
        long k;
        if (bn < sn.n_children - 1) {
            for (int i = 0; i < eltsperbn; ++i) {
                k = bn * eltsperbn + i;
                big_val_size += insert_dummy_value(&sn, bn, k, i);
            }
            DBT pivotkey;
            sn.pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)), bn);
        } else {
            k = bn * eltsperbn;
            // we want this to be slightly smaller than all the rest of
            // the data combined, so the halfway mark will be just to its
            // left and just this element will end up on the right of the split
            big_val_size -= 1 + (sizeof(((LEAFENTRY)NULL)->type)  // overhead from LE_CLEAN_MEMSIZE
                                 +sizeof(uint32_t) // sizeof(keylen)
                                 +sizeof(((LEAFENTRY)NULL)->u.clean.vallen));
            invariant(big_val_size <= maxbnsize);
            char * XMALLOC_N(big_val_size, big_val);
            memset(big_val, k, big_val_size);
            le_add_to_bn(BLB_DATA(&sn, bn), 0, (char *) &k, keylen, big_val, big_val_size);
            toku_free(big_val);
        }
    }

    unlink(fname);
    CACHETABLE ct;
    FT_HANDLE ft;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &ft, nodesize, bnsize, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun); assert(r==0);

    FTNODE nodea, nodeb;
    DBT splitk;
    // if we haven't done it right, we should hit the assert in the top of move_leafentries
    ftleaf_split(ft->ft, &sn, &nodea, &nodeb, &splitk, true, SPLIT_EVENLY, 0, NULL);

    toku_unpin_ftnode(ft->ft, nodeb);
    r = toku_close_ft_handle_nolsn(ft, NULL); assert(r == 0);
    toku_cachetable_close(&ct);

    toku_destroy_dbt(&splitk);
    toku_destroy_ftnode_internals(&sn);
}

static void
test_split_at_begin(void)
{
    struct ftnode sn;

    int fd = open(fname, O_RDWR|O_CREAT|O_BINARY, S_IRWXU|S_IRWXG|S_IRWXO); assert(fd >= 0);

    int r;

    setup_ftnode_header(&sn);
    const int nelts = 2 * nodesize / eltsize;
    const size_t maxbnsize = 2 * nodesize;
    setup_ftnode_partitions(&sn, nelts * eltsize / bnsize, dummy_msn_3884, maxbnsize);
    size_t totalbytes = 0;
    for (int bn = 0; bn < sn.n_children; ++bn) {
        long k;
        for (int i = 0; i < eltsperbn; ++i) {
            k = bn * eltsperbn + i;
            if (bn == 0 && i == 0) {
                // we'll add the first element later when we know how big
                // to make it
                continue;
            }
            totalbytes += insert_dummy_value(&sn, bn, k, i-1);
        }
        if (bn < sn.n_children - 1) {
            DBT pivotkey;
            sn.pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)), bn);
        }
    }
    {  // now add the first element
        int bn = 0; long k = 0;
        // add a few bytes so the halfway mark is definitely inside this
        // val, which will make it go to the left and everything else to
        // the right
        char val[totalbytes + 3];
        invariant(totalbytes + 3 <= maxbnsize);
        memset(val, k, sizeof val);
        le_add_to_bn(BLB_DATA(&sn, bn), 0, (char *) &k, keylen, val, totalbytes + 3);
        totalbytes += LE_CLEAN_MEMSIZE(totalbytes + 3) + keylen + sizeof(uint32_t);
    }

    unlink(fname);
    CACHETABLE ct;
    FT_HANDLE ft;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &ft, nodesize, bnsize, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun); assert(r==0);

    FTNODE nodea, nodeb;
    DBT splitk;
    // if we haven't done it right, we should hit the assert in the top of move_leafentries
    ftleaf_split(ft->ft, &sn, &nodea, &nodeb, &splitk, true, SPLIT_EVENLY, 0, NULL);

    toku_unpin_ftnode(ft->ft, nodeb);
    r = toku_close_ft_handle_nolsn(ft, NULL); assert(r == 0);
    toku_cachetable_close(&ct);

    toku_destroy_dbt(&splitk);
    toku_destroy_ftnode_internals(&sn);
}

static void
test_split_at_end(void)
{
    struct ftnode sn;

    int fd = open(fname, O_RDWR|O_CREAT|O_BINARY, S_IRWXU|S_IRWXG|S_IRWXO); assert(fd >= 0);

    int r;

    setup_ftnode_header(&sn);
    const int nelts = 2 * nodesize / eltsize;
    const size_t maxbnsize = 2 * nodesize;
    setup_ftnode_partitions(&sn, nelts * eltsize / bnsize, dummy_msn_3884, maxbnsize);
    long totalbytes = 0;
    int bn, i;
    for (bn = 0; bn < sn.n_children; ++bn) {
        long k;
        for (i = 0; i < eltsperbn; ++i) {
            k = bn * eltsperbn + i;
            if (bn == sn.n_children - 1 && i == eltsperbn - 1) {
                // add a few bytes so the halfway mark is definitely inside this
                // val, which will make it go to the left and everything else to
                // the right, which is nothing, so we actually split at the very end
                char val[totalbytes + 3];
                invariant(totalbytes + 3 <= (long) maxbnsize);
                memset(val, k, sizeof val);
                le_add_to_bn(BLB_DATA(&sn, bn), i, (char *) &k, keylen, val, totalbytes + 3);
                totalbytes += LE_CLEAN_MEMSIZE(totalbytes + 3) + keylen + sizeof(uint32_t);
            } else {
                totalbytes += insert_dummy_value(&sn, bn, k, i);
            }
        }
        if (bn < sn.n_children - 1) {
            DBT pivotkey;
            sn.pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)), bn);
        }
    }

    unlink(fname);
    CACHETABLE ct;
    FT_HANDLE ft;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &ft, nodesize, bnsize, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun); assert(r==0);

    FTNODE nodea, nodeb;
    DBT splitk;
    // if we haven't done it right, we should hit the assert in the top of move_leafentries
    ftleaf_split(ft->ft, &sn, &nodea, &nodeb, &splitk, true, SPLIT_EVENLY, 0, NULL);

    toku_unpin_ftnode(ft->ft, nodeb);
    r = toku_close_ft_handle_nolsn(ft, NULL); assert(r == 0);
    toku_cachetable_close(&ct);

    toku_destroy_dbt(&splitk);
    toku_destroy_ftnode_internals(&sn);
}

// Maximum node size according to the FT: 1024 (expected node size after split)
// Maximum basement node size: 256
// Actual node size before split: 2048
// Actual basement node size before split: 256
// Start by creating 9 basements, then split node.
// Expected result of two nodes with 5 basements each.
static void
test_split_odd_nodes(void)
{
    struct ftnode sn;

    int fd = open(fname, O_RDWR|O_CREAT|O_BINARY, S_IRWXU|S_IRWXG|S_IRWXO);
    assert(fd >= 0);

    int r;

    setup_ftnode_header(&sn);
    // This will give us 9 children.
    const int nelts = 2 * (nodesize + 128) / eltsize;
    setup_ftnode_partitions(&sn, nelts * eltsize / bnsize, dummy_msn_3884, bnsize);
    for (int bn = 0; bn < sn.n_children; ++bn) {
        long k;
        for (int i = 0; i < eltsperbn; ++i) {
            k = bn * eltsperbn + i;
            insert_dummy_value(&sn, bn, k, i);
        }
        if (bn < sn.n_children - 1) {
            DBT pivotkey;
            sn.pivotkeys.insert_at(toku_fill_dbt(&pivotkey, &k, sizeof(k)), bn);
        }
    }

    unlink(fname);
    CACHETABLE ct;
    FT_HANDLE ft;
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 1, &ft, nodesize, bnsize, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun); assert(r==0);

    FTNODE nodea, nodeb;
    DBT splitk;
    // if we haven't done it right, we should hit the assert in the top of move_leafentries
    ftleaf_split(ft->ft, &sn, &nodea, &nodeb, &splitk, true, SPLIT_EVENLY, 0, NULL);

    verify_basement_node_msns(nodea, dummy_msn_3884);
    verify_basement_node_msns(nodeb, dummy_msn_3884);

    toku_unpin_ftnode(ft->ft, nodeb);
    r = toku_close_ft_handle_nolsn(ft, NULL); assert(r == 0);
    toku_cachetable_close(&ct);

    toku_destroy_dbt(&splitk);
    toku_destroy_ftnode_internals(&sn);
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {

    test_split_on_boundary();
    test_split_with_everything_on_the_left();
    test_split_on_boundary_of_last_node();
    test_split_at_begin();
    test_split_at_end();
    test_split_odd_nodes();

    return 0;
}
