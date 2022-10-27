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

#ident                                                                         \
    "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

/* The goal of this test.  Make sure that inserts stay behind deletes. */

#include "test.h"

#include "cachetable/checkpoint.h"
#include "ft-flusher-internal.h"
#include "ft-flusher.h"
#include <ft-cachetable-wrappers.h>

static TOKUTXN const null_txn = 0;

enum { NODESIZE = 1024, KSIZE = NODESIZE - 100, TOKU_PSIZE = 20 };

CACHETABLE ct;
FT_HANDLE ft;
const char *fname = TOKU_TEST_FILENAME;

static int update_func(DB *UU(db), const DBT *key, const DBT *old_val,
                       const DBT *UU(extra),
                       void (*set_val)(const DBT *new_val, void *set_extra),
                       void *set_extra) {
  DBT new_val;
  assert(old_val->size > 0);
  if (verbose) {
    printf("applying update to %s\n", (char *)key->data);
  }
  toku_init_dbt(&new_val);
  set_val(&new_val, set_extra);
  return 0;
}

static void doit() {
  BLOCKNUM node_leaf;
  BLOCKNUM node_root;
  BLOCKNUM node_internal;
  int r;

  toku_cachetable_create(&ct, 500 * 1024 * 1024, ZERO_LSN, nullptr);
  unlink(fname);
  r = toku_open_ft_handle(fname, 1, &ft, NODESIZE, NODESIZE / 2,
                          TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn,
                          toku_builtin_compare_fun);
  assert(r == 0);

  ft->options.update_fun = update_func;
  ft->ft->update_fun = update_func;

  toku_testsetup_initialize(); // must precede any other toku_testsetup calls
  char *pivots[1];
  pivots[0] = toku_strdup("kkkkk");
  int pivot_len = 6;
  r = toku_testsetup_leaf(ft, &node_leaf, 2, pivots, &pivot_len);
  assert(r == 0);

  toku_free(pivots[0]);

  r = toku_testsetup_nonleaf(ft, 1, &node_internal, 1, &node_leaf, 0, 0);
  assert(r == 0);

  r = toku_testsetup_nonleaf(ft, 2, &node_root, 1, &node_internal, 0, 0);
  assert(r == 0);

  r = toku_testsetup_root(ft, node_root);
  assert(r == 0);

  r = toku_testsetup_insert_to_leaf(ft, node_leaf,
                                    "a", // key
                                    2,   // keylen
                                    "aa", 3);
  assert(r == 0);

  r = toku_testsetup_insert_to_leaf(ft, node_leaf,
                                    "z", // key
                                    2,   // keylen
                                    "zz", 3);
  assert(r == 0);
  char filler[400];
  memset(filler, 0, sizeof(filler));
  // now we insert filler data so that the rebalance
  // keeps it at two nodes
  r = toku_testsetup_insert_to_leaf(ft, node_leaf,
                                    "b", // key
                                    2,   // keylen
                                    filler, sizeof(filler));
  assert(r == 0);
  r = toku_testsetup_insert_to_leaf(ft, node_leaf,
                                    "y", // key
                                    2,   // keylen
                                    filler, sizeof(filler));
  assert(r == 0);

  r = toku_testsetup_insert_to_nonleaf(ft, node_internal, FT_INSERT,
                                       "a", // key
                                       2,   // keylen
                                       "yy", 3);
  assert(r == 0);

  r = toku_testsetup_insert_to_nonleaf(ft, node_root, FT_INSERT,
                                       "a", // key
                                       2,   // keylen
                                       "zz", 3);
  assert(r == 0);

  // at this point of time, the logical row count will be 6. This has to be
  // manually set up as the tests work under the interface of the ft_send_msg
  ft->ft->in_memory_logical_rows = 6;
  // now run a checkpoint to get everything clean
  CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
  r = toku_checkpoint(cp, NULL, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);
  assert_zero(r);
  // now do a lookup on one of the keys, this should bring a leaf node up to
  // date
  DBT k;
  struct check_pair pair = {2, "a", 3, "zz", 0};
  r = toku_ft_lookup(ft, toku_fill_dbt(&k, "a", 2), lookup_checkf, &pair);
  assert(r == 0);
  assert(ft->ft->in_memory_logical_rows == 4);
  FTNODE node;
  // now lock and release the leaf node to make sure it is what we expect it to
  // be.
  toku_pin_node_with_min_bfe(&node, node_leaf, ft);
  for (int i = 0; i < 20; i++) {
    toku_ftnode_pe_callback(node, make_pair_attr(0xffffffff), ft->ft,
                            def_pe_finalize_impl, nullptr);
  }
  toku_unpin_ftnode(ft->ft, node);
  assert(ft->ft->in_memory_logical_rows == 6);

  r = toku_close_ft_handle_nolsn(ft, 0);
  assert(r == 0);
  toku_cachetable_close(&ct);
}

int test_main(int argc __attribute__((__unused__)),
              const char *argv[] __attribute__((__unused__))) {
  default_parse_args(argc, argv);
  doit();
  return 0;
}
