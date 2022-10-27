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

#include "ft/txn/rollback.h"

class rollback_log_node_cache {
public:
    void init (uint32_t max_num_avail_nodes);
    void destroy();
    // returns true if rollback log node was successfully added,
    // false otherwise
    bool give_rollback_log_node(TOKUTXN txn, ROLLBACK_LOG_NODE log);
    // if a rollback log node is available, will set log to it,
    // otherwise, will set log to NULL and caller is on his own
    // for getting a rollback log node
    void get_rollback_log_node(TOKUTXN txn, ROLLBACK_LOG_NODE* log);

private:
    BLOCKNUM* m_avail_blocknums;
    uint32_t m_first;
    uint32_t m_num_avail;
    uint32_t m_max_num_avail;
    toku_mutex_t m_mutex;
};

ENSURE_POD(rollback_log_node_cache);
