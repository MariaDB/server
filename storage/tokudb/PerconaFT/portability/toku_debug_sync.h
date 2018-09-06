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

struct tokutxn;

#if defined(MYSQL_TOKUDB_ENGINE) && MYSQL_TOKUDB_ENGINE && \
    defined(ENABLED_DEBUG_SYNC) && ENABLED_DEBUG_SYNC

/*
  the below macros are defined in my_global.h, which is included in m_string.h,
  the same macros are defined in TokuSetupCompiler.cmake as compiler options,
  undefine them here to avoid build errors
*/
#undef __STDC_FORMAT_MACROS
#undef __STDC_LIMIT_MACROS

#include "m_string.h"
#include "debug_sync.h"

void toku_txn_get_client_id(struct tokutxn *txn,
                            uint64_t *client_id,
                            void **client_extra);

inline void toku_debug_sync(struct tokutxn *txn, const char *sync_point_name) {
    uint64_t client_id;
    void *client_extra;
    THD *thd;

    toku_txn_get_client_id(txn, &client_id, &client_extra);
    thd = reinterpret_cast<THD *>(client_extra);
    DEBUG_SYNC(thd, sync_point_name);
}

#else // defined(ENABLED_DEBUG_SYNC)

inline void toku_debug_sync(struct tokutxn *, const char *) {};

#endif // defined(ENABLED_DEBUG_SYNC)
