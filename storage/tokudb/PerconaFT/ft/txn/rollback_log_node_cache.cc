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

#include <memory.h>
#include <portability/toku_portability.h>

#include "txn/rollback_log_node_cache.h"

void rollback_log_node_cache::init (uint32_t max_num_avail_nodes) {
    XMALLOC_N(max_num_avail_nodes, m_avail_blocknums);
    m_max_num_avail = max_num_avail_nodes;
    m_first = 0;
    m_num_avail = 0;
    toku_pthread_mutexattr_t attr;
    toku_mutexattr_init(&attr);
    toku_mutexattr_settype(&attr, TOKU_MUTEX_ADAPTIVE);
    toku_mutex_init(&m_mutex, &attr);
    toku_mutexattr_destroy(&attr);
}

void rollback_log_node_cache::destroy() {
    toku_mutex_destroy(&m_mutex);
    toku_free(m_avail_blocknums);
}

// returns true if rollback log node was successfully added,
// false otherwise
bool rollback_log_node_cache::give_rollback_log_node(TOKUTXN txn, ROLLBACK_LOG_NODE log){
    bool retval = false;
    toku_mutex_lock(&m_mutex);
    if (m_num_avail < m_max_num_avail) {
        retval = true;
        uint32_t index = m_first + m_num_avail;
        if (index >= m_max_num_avail) {
            index -= m_max_num_avail;
        }
        m_avail_blocknums[index].b = log->blocknum.b;
        m_num_avail++;
    }
    toku_mutex_unlock(&m_mutex);
    //
    // now unpin the rollback log node
    //
    if (retval) {
        make_rollback_log_empty(log);
        toku_rollback_log_unpin(txn, log);
    }
    return retval;
}

// if a rollback log node is available, will set log to it,
// otherwise, will set log to NULL and caller is on his own
// for getting a rollback log node
void rollback_log_node_cache::get_rollback_log_node(TOKUTXN txn, ROLLBACK_LOG_NODE* log){
    BLOCKNUM b = ROLLBACK_NONE;
    toku_mutex_lock(&m_mutex);
    if (m_num_avail > 0) {
        b.b = m_avail_blocknums[m_first].b;
        m_num_avail--;
        if (++m_first >= m_max_num_avail) {
            m_first = 0;
        }
    }
    toku_mutex_unlock(&m_mutex);
    if (b.b != ROLLBACK_NONE.b) {
        toku_get_and_pin_rollback_log(txn, b, log);
        invariant(rollback_log_is_unused(*log));
    } else {
        *log = NULL;
    }
}

