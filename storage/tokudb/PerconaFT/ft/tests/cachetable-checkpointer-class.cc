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
#include "cachetable/cachetable-internal.h"
#include "cachetable-test.h"

//
// Wrapper for the checkpointer and necessary
// data to run the tests.
//
struct checkpointer_test {
  checkpointer m_cp;
  pair_list m_pl;

  // Tests
  void test_begin_checkpoint();
  void test_pending_bits();
  void test_end_checkpoint();

  // Test Helper
  void add_pairs(struct cachefile *cf,
    ctpair pairs[],
    uint32_t count,
    uint32_t k);
};

static void init_cachefile(CACHEFILE cf, int which_cf, bool for_checkpoint) {
    memset(cf, 0, sizeof(*cf));
    create_dummy_functions(cf);
    cf->fileid = { 0, (unsigned) which_cf };
    cf->filenum = { (unsigned) which_cf };
    cf->for_checkpoint = for_checkpoint;
}

//------------------------------------------------------------------------------
// test_begin_checkpoint() -
//
// Description:
//
void checkpointer_test::test_begin_checkpoint() {
    cachefile_list cfl;
    ZERO_STRUCT(cfl);
    cfl.init();

    cachetable ctbl;
    ZERO_STRUCT(ctbl);
    ctbl.list.init();

    ZERO_STRUCT(m_cp);
    m_cp.init(&ctbl.list, NULL, &ctbl.ev, &cfl);

    // 1. Call checkpoint with NO cachefiles.
    m_cp.begin_checkpoint();

    // 2. Call checkpoint with ONE cachefile.
    //cachefile cf;
    struct cachefile cf;
    init_cachefile(&cf, 0, false);
    m_cp.m_cf_list->add_cf_unlocked(&cf);

    m_cp.begin_checkpoint();
    assert(m_cp.m_checkpoint_num_files == 1);
    assert(cf.for_checkpoint == true);
    m_cp.m_cf_list->remove_cf(&cf);

    // 3. Call checkpoint with MANY cachefiles.
    const uint32_t count = 3;
    struct cachefile cfs[count];
    for (uint32_t i = 0; i < count; ++i) {
        init_cachefile(&cfs[i], i, false);
        create_dummy_functions(&cfs[i]);
        m_cp.m_cf_list->add_cf_unlocked(&cfs[i]);
    }

    m_cp.begin_checkpoint();
    assert(m_cp.m_checkpoint_num_files == count);
    for (uint32_t i = 0; i < count; ++i) {
        assert(cfs[i].for_checkpoint == true);
        cfl.remove_cf(&cfs[i]);
    }
    ctbl.list.destroy();
    m_cp.destroy();
    cfl.destroy();
}

//------------------------------------------------------------------------------
// test_pending_bits() -
//
// Description:
//
void checkpointer_test::test_pending_bits() {
    cachefile_list cfl;
    ZERO_STRUCT(cfl);
    cfl.init();

    cachetable ctbl;
    ZERO_STRUCT(ctbl);
    ctbl.list.init();

    ZERO_STRUCT(m_cp);
    m_cp.init(&ctbl.list, NULL, &ctbl.ev, &cfl);

    //
    // 1. Empty hash chain.
    //
    m_cp.turn_on_pending_bits();

    //
    // 2. One entry in pair chain
    //
    struct cachefile cf;
    cf.cachetable = &ctbl;
    init_cachefile(&cf, 0, true);
    m_cp.m_cf_list->add_cf_unlocked(&cf);
    create_dummy_functions(&cf);

    CACHEKEY k;
    k.b = 0;
    uint32_t hash = toku_cachetable_hash(&cf, k);

    ctpair p;
    CACHETABLE_WRITE_CALLBACK cb;

    pair_attr_s attr;
    attr.size = 0;
    attr.nonleaf_size = 0;
    attr.leaf_size = 0;
    attr.rollback_size = 0;
    attr.cache_pressure_size = 0;
    attr.is_valid = true;

    ZERO_STRUCT(p);
    pair_init(&p,
        &cf,
        k,
        NULL,
        attr,
        CACHETABLE_CLEAN,
        hash,
        cb,
        NULL,
        &ctbl.list);

    m_cp.m_list->put(&p);

    m_cp.turn_on_pending_bits();
    assert(p.checkpoint_pending);
    m_cp.m_list->evict_completely(&p);

    //
    // 3. Many hash chain entries.
    //
    const uint32_t count = 3;
    ctpair pairs[count];
    ZERO_ARRAY(pairs);
    add_pairs(&cf, pairs, count, 0);

    m_cp.turn_on_pending_bits();

    for (uint32_t i = 0; i < count; ++i) {
        assert(pairs[i].checkpoint_pending);
    }
    for (uint32_t i = 0; i < count; ++i) {
        CACHEKEY key;
        key.b = i;
        uint32_t full_hash = toku_cachetable_hash(&cf, key);
        PAIR pp = m_cp.m_list->find_pair(&cf, key, full_hash);
        assert(pp);
        m_cp.m_list->evict_completely(pp);
    }

    ctbl.list.destroy();
    m_cp.destroy();
    cfl.remove_cf(&cf);
    cfl.destroy();
}

//------------------------------------------------------------------------------
// add_pairs() -
//
// Description: Adds data (pairs) to the list referenced in the checkpoitner.
//
void checkpointer_test::add_pairs(struct cachefile *cf,
    ctpair pairs[],
    uint32_t count,
    uint32_t k)
{
    pair_attr_s attr;
    attr.size = 0;
    attr.nonleaf_size = 0;
    attr.leaf_size = 0;
    attr.rollback_size = 0;
    attr.cache_pressure_size = 0;
    attr.is_valid = true;
    CACHETABLE_WRITE_CALLBACK cb;
    ZERO_STRUCT(cb);  // All nullptr

    for (uint32_t i = k; i < count + k; ++i) {
        CACHEKEY key;
        key.b = i;
        uint32_t full_hash = toku_cachetable_hash(cf, key);
        pair_init(&(pairs[i]),
            cf,
            key,
            nullptr,
            attr,
            CACHETABLE_CLEAN,
            full_hash,
            cb,
            nullptr,
            m_cp.m_list);

        m_cp.m_list->put(&pairs[i]);
    }
}

//------------------------------------------------------------------------------
// get_number_pending_pairs() -
//
// Description: Helper function that iterates over pending list, and returns
//   the number of pairs discovered.
//
static uint32_t get_number_pending_pairs(pair_list *list)
{
    PAIR p;
    uint32_t count = 0;
    PAIR head = list->m_pending_head;
    while((p = list->m_pending_head) != 0)
    {
        list->m_pending_head = list->m_pending_head->pending_next;
        count++;
    }

    list->m_pending_head = head;
    return count;
}

//------------------------------------------------------------------------------
// test_end_checkpoint() -
//
// Description:  Adds pairs to the list, before and after a checkpoint.
//
void checkpointer_test::test_end_checkpoint() {
    // 1. Init test.
    cachetable ctbl;
    ZERO_STRUCT(ctbl);
    ctbl.list.init();

    cachefile_list cfl;
    ZERO_STRUCT(cfl);
    cfl.init();

    struct cachefile cf;
    init_cachefile(&cf, 0, true);

    ZERO_STRUCT(m_cp);
    m_cp.init(&ctbl.list, NULL, &ctbl.ev, &cfl);
    m_cp.m_cf_list->add_cf_unlocked(&cf);

    // 2. Add data before running checkpoint.
    const uint32_t count = 6;
    ctpair pairs[count];
    ZERO_ARRAY(pairs);
    add_pairs(&cf, pairs, count / 2, 0);
    assert(m_cp.m_list->m_n_in_table == count / 2);

    // 3. Call begin checkpoint.
    m_cp.begin_checkpoint();
    assert(m_cp.m_checkpoint_num_files == 1);
    for (uint32_t i = 0; i < count / 2; ++i)
    {
        assert(pairs[i].checkpoint_pending);
    }

    // 4. Add new data between starting and stopping checkpoint.
    add_pairs(&cf, pairs, count / 2, count / 2);
    assert(m_cp.m_list->m_n_in_table == count);
    for (uint32_t i = count / 2; i < count / 2; ++i)
    {
        assert(!pairs[i].checkpoint_pending);
    }

    uint32_t pending_pairs = 0;
    pending_pairs = get_number_pending_pairs(m_cp.m_list);
    assert(pending_pairs == count / 2);

    // 5. Call end checkpoint
    m_cp.end_checkpoint(NULL, NULL);

    pending_pairs = get_number_pending_pairs(m_cp.m_list);
    assert(pending_pairs == 0);

    // Verify that none of the pairs are pending a checkpoint.
    for (uint32_t i = 0; i < count; ++i)
    {
        assert(!pairs[i].checkpoint_pending);
    }

    // 6. Cleanup
    for (uint32_t i = 0; i < count; ++i) {
        CACHEKEY key;
        key.b = i;
        uint32_t full_hash = toku_cachetable_hash(&cf, key);
        PAIR pp = m_cp.m_list->find_pair(&cf, key, full_hash);
        assert(pp);
        m_cp.m_list->evict_completely(pp);
    }
    cfl.remove_cf(&cf);
    m_cp.destroy();
    ctbl.list.destroy();
    cfl.destroy();
}


//------------------------------------------------------------------------------
// test_main() -
//
// Description:
//
int
test_main(int argc, const char *argv[]) {
    int r = 0;
    default_parse_args(argc, argv);
    checkpointer_test cp_test;

    // Run the tests.
    cp_test.test_begin_checkpoint();
    cp_test.test_pending_bits();
    cp_test.test_end_checkpoint();

    return r;
}
