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

#include "ft/logger/log-internal.h"
#include "ft/txn/txn_child_manager.h"

//
// initialized a txn_child_manager,
// when called, root->txnid.parent_id64 may not yet be set
//
void txn_child_manager::init(TOKUTXN root) {
    invariant(root->txnid.child_id64 == TXNID_NONE);
    invariant(root->parent == NULL);
    m_root = root;
    m_last_xid = TXNID_NONE;
    ZERO_STRUCT(m_mutex);

    toku_pthread_mutexattr_t attr;
    toku_mutexattr_init(&attr);
    toku_mutexattr_settype(&attr, TOKU_MUTEX_ADAPTIVE);
    toku_mutex_init(&m_mutex, &attr);
    toku_mutexattr_destroy(&attr);
}

void txn_child_manager::destroy() {
    toku_mutex_destroy(&m_mutex);
}

void txn_child_manager::start_child_txn_for_recovery(TOKUTXN child, TOKUTXN parent, TXNID_PAIR txnid) {
    invariant(parent->txnid.parent_id64 == m_root->txnid.parent_id64);
    invariant(txnid.parent_id64 == m_root->txnid.parent_id64);

    child->txnid = txnid;
    toku_mutex_lock(&m_mutex);
    if (txnid.child_id64 > m_last_xid) {
        m_last_xid = txnid.child_id64;
    }
    parent->child = child;
    toku_mutex_unlock(&m_mutex);
}

void txn_child_manager::start_child_txn(TOKUTXN child, TOKUTXN parent) {
    invariant(parent->txnid.parent_id64 == m_root->txnid.parent_id64);
    child->txnid.parent_id64 = m_root->txnid.parent_id64;
    toku_mutex_lock(&m_mutex);
    
    ++m_last_xid;
    // Here we ensure that the child_id64 is never equal to the parent_id64
    // We do this to make this feature work more easily with the XIDs
    // struct and message application. The XIDs struct stores the parent id
    // as the first TXNID, and subsequent TXNIDs store child ids. So, if we
    // have a case where the parent id is the same as the child id, we will
    // have to do some tricky maneuvering in the message application code
    // in ule.cc. So, to lessen the probability of bugs, we ensure that the
    // parent id is not the same as the child id.
    if (m_last_xid == m_root->txnid.parent_id64) {
        ++m_last_xid;
    }
    child->txnid.child_id64 = m_last_xid;

    parent->child = child;
    toku_mutex_unlock(&m_mutex);
}

void txn_child_manager::finish_child_txn(TOKUTXN child) {
    invariant(child->txnid.parent_id64 == m_root->txnid.parent_id64);
    toku_mutex_lock(&m_mutex);
    child->parent->child = NULL;
    toku_mutex_unlock(&m_mutex);
}

void txn_child_manager::suspend() {
    toku_mutex_lock(&m_mutex);
}

void txn_child_manager::resume() {
    toku_mutex_unlock(&m_mutex);
}

void txn_child_manager::find_tokutxn_by_xid_unlocked(TXNID_PAIR xid, TOKUTXN* result) {
    invariant(xid.parent_id64 == m_root->txnid.parent_id64);
    TOKUTXN curr_txn = m_root;
    while (curr_txn != NULL) {
        if (xid.child_id64 == curr_txn->txnid.child_id64) {
            *result = curr_txn;
            break;
        }
        curr_txn = curr_txn->child;
    }
}

int txn_child_manager::iterate(txn_mgr_iter_callback cb, void* extra) { 
    TOKUTXN curr_txn = m_root; 
    int ret = 0; 
    toku_mutex_lock(&m_mutex); 
    while (curr_txn != NULL) { 
        ret = cb(curr_txn, extra); 
        if (ret != 0) { 
            break; 
        } 
        curr_txn = curr_txn->child; 
    } 
    toku_mutex_unlock(&m_mutex); 
    return ret; 
} 

