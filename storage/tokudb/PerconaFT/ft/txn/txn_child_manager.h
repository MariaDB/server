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

#pragma once

// We should be including ft/txn/txn.h here but that header includes this one,
// so we don't.
#include "portability/toku_pthread.h"

class txn_child_manager {
public:
    void init (TOKUTXN root);
    void destroy();
    void start_child_txn_for_recovery(TOKUTXN child, TOKUTXN parent, TXNID_PAIR txnid);
    void start_child_txn(TOKUTXN child, TOKUTXN parent);
    void finish_child_txn(TOKUTXN child);
    void suspend();
    void resume();
    void find_tokutxn_by_xid_unlocked(TXNID_PAIR xid, TOKUTXN* result);
    int iterate(int (*cb)(TOKUTXN txn, void *extra), void* extra);

private:
    TXNID m_last_xid;
    TOKUTXN m_root;
    toku_mutex_t m_mutex;

    friend class txn_child_manager_unit_test;
};


ENSURE_POD(txn_child_manager);
