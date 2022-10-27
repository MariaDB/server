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

#include <toku_portability.h>
#include <string.h>

#include "test.h"

#include "ft/ule.h"
#include "ft/ule-internal.h"

static void init_empty_ule(ULE ule) {
    ule->num_cuxrs = 0;
    ule->num_puxrs = 0;
    ule->uxrs = ule->uxrs_static;
}

static void add_committed_entry(ULE ule, DBT *val, TXNID xid) {
    uint32_t index = ule->num_cuxrs;
    ule->num_cuxrs++;
    ule->uxrs[index].type   = XR_INSERT;
    ule->uxrs[index].vallen = val->size;
    ule->uxrs[index].valp   = val->data;
    ule->uxrs[index].xid    = xid;
}

//Test all the different things that can happen to a
//committed leafentry (logical equivalent of a committed insert).
static void
run_test(void) {
    ULE_S ule_initial;
    ULE ule = &ule_initial;
    ule_initial.uxrs = ule_initial.uxrs_static;
    int r;
    DBT key;
    DBT val;
    uint64_t key_data = 1;
    uint64_t val_data_one = 1;
    uint64_t val_data_two = 2;
    uint64_t val_data_three = 3;
    uint32_t keysize = 8;
    uint32_t valsize = 8;

    toku_fill_dbt(&key, &key_data, keysize);
    toku_fill_dbt(&val, &val_data_one, valsize);

    // test case where we apply a message and the innermost child_id
    // is the same as the innermost committed TXNID    
    XIDS root_xids = toku_xids_get_root_xids();
    TXNID root_txnid = 1000;
    TXNID child_id = 10;
    XIDS msg_xids_1;
    XIDS msg_xids_2;
    r = toku_xids_create_child(root_xids, &msg_xids_1, root_txnid);
    assert(r==0);
    r = toku_xids_create_child(msg_xids_1, &msg_xids_2, child_id);
    assert(r==0);

    init_empty_ule(&ule_initial);
    add_committed_entry(&ule_initial, &val, 0);
    val.data = &val_data_two;
    // make the TXNID match the child id of xids
    add_committed_entry(&ule_initial, &val, 10);

    // now do the application of xids to the ule    
    // do a commit
    {
        ft_msg msg(&key, &val, FT_COMMIT_ANY, ZERO_MSN, msg_xids_2);
        test_msg_modify_ule(&ule_initial, msg);
        assert(ule->num_cuxrs == 2);
        assert(ule->uxrs[0].xid == TXNID_NONE);
        assert(ule->uxrs[1].xid == 10);
        assert(ule->uxrs[0].valp == &val_data_one);
        assert(ule->uxrs[1].valp == &val_data_two);
    }

    // do an abort
    {
        ft_msg msg(&key, &val, FT_ABORT_ANY, ZERO_MSN, msg_xids_2);
        test_msg_modify_ule(&ule_initial, msg);
        assert(ule->num_cuxrs == 2);
        assert(ule->uxrs[0].xid == TXNID_NONE);
        assert(ule->uxrs[1].xid == 10);
        assert(ule->uxrs[0].valp == &val_data_one);
        assert(ule->uxrs[1].valp == &val_data_two);
    }

    // do an insert
    val.data = &val_data_three;
    {
        ft_msg msg(&key, &val, FT_INSERT, ZERO_MSN, msg_xids_2);
        test_msg_modify_ule(&ule_initial, msg);
        // now that message applied, verify that things are good
        assert(ule->num_cuxrs == 2);
        assert(ule->num_puxrs == 2);
        assert(ule->uxrs[0].xid == TXNID_NONE);
        assert(ule->uxrs[1].xid == 10);
        assert(ule->uxrs[2].xid == 1000);
        assert(ule->uxrs[3].xid == 10);
        assert(ule->uxrs[0].valp == &val_data_one);
        assert(ule->uxrs[1].valp == &val_data_two);
        assert(ule->uxrs[2].type == XR_PLACEHOLDER);
        assert(ule->uxrs[3].valp == &val_data_three);
    } 

    toku_xids_destroy(&msg_xids_2);
    toku_xids_destroy(&msg_xids_1);
    toku_xids_destroy(&root_xids);

}


int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    run_test();
    return 0;
}

