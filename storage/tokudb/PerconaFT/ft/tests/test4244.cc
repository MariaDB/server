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

static TOKUTXN const null_txn = 0;

enum { NODESIZE = 1024, KSIZE=NODESIZE-100, TOKU_PSIZE=20 };

CACHETABLE ct;
FT_HANDLE t;
const char *fname = TOKU_TEST_FILENAME;

static void
doit (void) {
    BLOCKNUM node_leaf, node_internal, node_root;

    int r;
    
    toku_cachetable_create(&ct, 500*1024*1024, ZERO_LSN, nullptr);
    unlink(fname);
    r = toku_open_ft_handle(fname, 1, &t, NODESIZE, NODESIZE/2, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    toku_testsetup_initialize();  // must precede any other toku_testsetup calls

    r = toku_testsetup_leaf(t, &node_leaf, 1, NULL, NULL);
    assert(r==0);

    r = toku_testsetup_nonleaf(t, 1, &node_internal, 1, &node_leaf, 0, 0);
    assert(r==0);

    r = toku_testsetup_nonleaf(t, 1, &node_root, 1, &node_internal, 0, 0);
    assert(r==0);

    r = toku_testsetup_root(t, node_root);
    assert(r==0);

    // make a 1MB val
    uint32_t big_val_size = 1000000;
    char* XCALLOC_N(big_val_size, big_val);
    DBT k,v;
    memset(&k, 0, sizeof(k));
    memset(&v, 0, sizeof(v));
    for (int i = 0; i < 100; i++) {
        toku_ft_insert(t,
                       toku_fill_dbt(&k, "hello", 6),
                       toku_fill_dbt(&v, big_val, big_val_size),
                       null_txn);
    }
    toku_free(big_val);


    // at this point, we have inserted 100MB of messages, if bug exists,
    // then node_internal should be huge
    // we pin it and verify that it is not
    FTNODE node;
    ftnode_fetch_extra bfe;
    bfe.create_for_full_read(t->ft);
    toku_pin_ftnode(
        t->ft, 
        node_internal,
        toku_cachetable_hash(t->ft->cf, node_internal),
        &bfe,
        PL_WRITE_EXPENSIVE, 
        &node,
        true
        );
    assert(node->n_children == 1);
    // simply assert that the buffer is less than 50MB,
    // we inserted 100MB of data in there.
    assert(toku_bnc_nbytesinbuf(BNC(node, 0)) < 50*1000*1000);
    toku_unpin_ftnode(t->ft, node);

    r = toku_close_ft_handle_nolsn(t, 0);    assert(r==0);
    toku_cachetable_close(&ct);
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    doit();
    return 0;
}
