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
#include "bndata.h"

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

static void
le_overwrite(bn_data* bn, uint32_t idx, const  char *key, int keysize, const char *val, int valsize) {
    LEAFENTRY r = NULL;
    uint32_t size_needed = LE_CLEAN_MEMSIZE(valsize);
    void *maybe_free = nullptr;
    bn->get_space_for_overwrite(
        idx, 
        key,
        keysize,
        keysize, // old_keylen
        size_needed, // old_le_size
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


class bndata_bugfix_test {
public:
    void
    run_test(void) {
        //    struct ft_handle source_ft;
        struct ftnode sn;
    
        // just copy this code from a previous test
        // don't care what it does, just want to get a node up and running
        sn.flags = 0x11223344;
        sn.blocknum.b = 20;
        sn.layout_version = FT_LAYOUT_VERSION;
        sn.layout_version_original = FT_LAYOUT_VERSION;
        sn.height = 0;
        sn.n_children = 2;
        sn.set_dirty();
        sn.oldest_referenced_xid_known = TXNID_NONE;
        MALLOC_N(sn.n_children, sn.bp);
        DBT pivotkey;
        sn.pivotkeys.create_from_dbts(toku_fill_dbt(&pivotkey, "b", 2), 1);
        BP_STATE(&sn,0) = PT_AVAIL;
        BP_STATE(&sn,1) = PT_AVAIL;
        set_BLB(&sn, 0, toku_create_empty_bn());
        set_BLB(&sn, 1, toku_create_empty_bn());
        le_add_to_bn(BLB_DATA(&sn, 0), 0, "a", 2, "aval", 5);
        le_add_to_bn(BLB_DATA(&sn, 0), 1, "b", 2, "bval", 5);
        le_add_to_bn(BLB_DATA(&sn, 1), 0, "x", 2, "xval", 5);
    
        // now this is the test. If I keep getting space for overwrite
        // like crazy, it should expose the bug
        bn_data* bnd = BLB_DATA(&sn, 0);
        size_t old_size = bnd->m_buffer_mempool.size;
        if (verbose) printf("frag size: %zu\n", bnd->m_buffer_mempool.frag_size);
        if (verbose) printf("size: %zu\n", bnd->m_buffer_mempool.size);
        for (uint32_t i = 0; i < 1000000; i++) {
            le_overwrite(bnd, 0, "a", 2, "aval", 5);
        }
        if (verbose) printf("frag size: %zu\n", bnd->m_buffer_mempool.frag_size);
        if (verbose) printf("size: %zu\n", bnd->m_buffer_mempool.size);
        size_t new_size = bnd->m_buffer_mempool.size;
        // just a crude test to make sure we did not grow unbounded.
        // if this assert ever fails, revisit the code and see what is going
        // on. It may be that some algorithm has changed.
        assert(new_size < 5*old_size);
    
        toku_destroy_ftnode_internals(&sn);
    }
};

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    bndata_bugfix_test t;
    t.run_test();
    return 0;
}
