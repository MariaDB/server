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
 * to use the xids and nothing else.  
 * Internal requirements of the xids logic do not belong here.
 *
 * xids is (abstractly) an immutable list of nested transaction ids, accessed only
 * via the functions in this file.  
 *
 * See design documentation for nested transactions at
 * TokuWiki/Imp/TransactionsOverview.
 */

#pragma once

#include "ft/txn/txn.h"
#include "ft/serialize/rbuf.h"
#include "ft/serialize/wbuf.h"

/* The number of transaction ids stored in the xids structure is 
 * represented by an 8-bit value.  The value 255 is reserved. 
 * The constant MAX_NESTED_TRANSACTIONS is one less because
 * one slot in the packed leaf entry is used for the implicit
 * root transaction (id 0).
 */
enum {
    MAX_NESTED_TRANSACTIONS = 253,
    MAX_TRANSACTION_RECORDS = MAX_NESTED_TRANSACTIONS + 1
};

// Variable size list of transaction ids (known in design doc as xids<>).
// ids[0] is the outermost transaction.
// ids[num_xids - 1] is the innermost transaction.
// Should only be accessed by accessor functions toku_xids_xxx, not directly.

// If the xids struct is unpacked, the compiler aligns the ids[] and we waste a lot of space
struct __attribute__((__packed__)) XIDS_S {
    // maximum value of MAX_TRANSACTION_RECORDS - 1 because transaction 0 is implicit
    uint8_t num_xids; 
    TXNID ids[];
};
typedef struct XIDS_S *XIDS;

// Retrieve an XIDS representing the root transaction.
XIDS toku_xids_get_root_xids(void);

bool toku_xids_can_create_child(XIDS xids);

void toku_xids_cpy(XIDS target, XIDS source);

//Creates an XIDS representing this transaction.
//You must pass in an XIDS representing the parent of this transaction.
int toku_xids_create_child(XIDS parent_xids, XIDS *xids_p, TXNID this_xid);

// The following two functions (in order) are equivalent to toku_xids_create child,
// but allow you to do most of the work without knowing the new xid.
int toku_xids_create_unknown_child(XIDS parent_xids, XIDS *xids_p);
void toku_xids_finalize_with_child(XIDS xids, TXNID this_xid);

void toku_xids_create_from_buffer(struct rbuf *rb, XIDS *xids_p);

void toku_xids_destroy(XIDS *xids_p);

TXNID toku_xids_get_xid(XIDS xids, uint8_t index);

uint8_t toku_xids_get_num_xids(XIDS xids);

TXNID toku_xids_get_innermost_xid(XIDS xids);
TXNID toku_xids_get_outermost_xid(XIDS xids);

// return size in bytes
uint32_t toku_xids_get_size(XIDS xids);

uint32_t toku_xids_get_serialize_size(XIDS xids);

unsigned char *toku_xids_get_end_of_array(XIDS xids);

void wbuf_nocrc_xids(struct wbuf *wb, XIDS xids);

void toku_xids_fprintf(FILE* fp, XIDS xids);
