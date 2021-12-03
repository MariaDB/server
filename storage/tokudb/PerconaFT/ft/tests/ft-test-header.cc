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

// The purpose of this test is to verify that certain information in the 
// ft_header is properly serialized and deserialized. 


static TOKUTXN const null_txn = 0;

static void test_header (void) {
    FT_HANDLE t;
    int r;
    CACHETABLE ct;
    const char *fname = TOKU_TEST_FILENAME;

    // First create dictionary
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    unlink(fname);
    r = toku_open_ft_handle(fname, 1, &t, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);
    // now insert some info into the header
    FT ft = t->ft;
    ft->h->set_dirty();
    // cast away const because we actually want to fiddle with the header
    // in this test
    *((int *) &ft->h->layout_version_original) = 13;
    ft->layout_version_read_from_disk = 14;
    *((uint32_t *) &ft->h->build_id_original) = 1234;
    ft->in_memory_stats          = (STAT64INFO_S) {10, 11};
    ft->h->on_disk_stats            = (STAT64INFO_S) {20, 21};
    r = toku_close_ft_handle_nolsn(t, 0);     assert(r==0);
    toku_cachetable_close(&ct);

    // Now read dictionary back into memory and examine some header fields
    toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
    r = toku_open_ft_handle(fname, 0, &t, 1024, 256, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    ft = t->ft;
    STAT64INFO_S expected_stats = {20, 21};  // on checkpoint, on_disk_stats copied to ft->checkpoint_header->on_disk_stats
    assert(ft->h->layout_version == FT_LAYOUT_VERSION);
    assert(ft->h->layout_version_original == 13);
    assert(ft->layout_version_read_from_disk == FT_LAYOUT_VERSION);
    assert(ft->h->build_id_original == 1234);
    assert(ft->in_memory_stats.numrows == expected_stats.numrows);
    assert(ft->h->on_disk_stats.numbytes  == expected_stats.numbytes);
    r = toku_close_ft_handle_nolsn(t, 0);     assert(r==0);
    toku_cachetable_close(&ct);
}

int
test_main (int argc , const char *argv[]) {
    default_parse_args(argc, argv);
    test_header();
    test_header(); /* Make sure it works twice. Redundant, but it's a very cheap test. */
    if (verbose) printf("test_header ok\n");
    return 0;
}
