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

/* Purpose of this file is to provide the world with everything necessary
 * to use the nested transaction logic and nothing else.  No internal
 * requirements of the nested transaction logic belongs here.
 */

#pragma once

#include "leafentry.h"
#include "txn/txn_manager.h"
#include <util/mempool.h>

// opaque handles used by outside world (i.e. indexer)
typedef struct ule *ULEHANDLE;	
typedef struct uxr *UXRHANDLE;

// create a ULE by copying the contents of the given leafentry
ULEHANDLE toku_ule_create(LEAFENTRY le);

void toku_ule_free(ULEHANDLE ule_p);

uint64_t ule_num_uxrs(ULEHANDLE ule);
uint32_t ule_get_num_committed(ULEHANDLE ule);
uint32_t ule_get_num_provisional(ULEHANDLE ule);
UXRHANDLE ule_get_uxr(ULEHANDLE ule, uint64_t ith);
int ule_is_committed(ULEHANDLE ule, uint64_t ith);
int ule_is_provisional(ULEHANDLE ule, uint64_t ith);

bool uxr_is_insert(UXRHANDLE uxr);
bool uxr_is_delete(UXRHANDLE uxr);
bool uxr_is_placeholder(UXRHANDLE uxr);
void *uxr_get_val(UXRHANDLE uxr);
uint32_t uxr_get_vallen(UXRHANDLE uxr);
TXNID uxr_get_txnid(UXRHANDLE uxr);

//1 does much slower debugging
#define GARBAGE_COLLECTION_DEBUG 0
