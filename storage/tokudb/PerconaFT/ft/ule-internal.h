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

/* Purpose of this file is to provide the test programs with internal 
 * ule mechanisms that do not belong in the public interface.
 */

#pragma once

//1 does much slower debugging
#define ULE_DEBUG 0

/////////////////////////////////////////////////////////////////////////////////
// Following data structures are the unpacked format of a leafentry. 
//   * ule is the unpacked leaf entry, that contains an array of unpacked
//     transaction records
//   * uxr is the unpacked transaction record
//


//Types of transaction records.
enum {XR_INSERT      = 1,
      XR_DELETE      = 2,
      XR_PLACEHOLDER = 3};

typedef struct uxr {     // unpacked transaction record
    uint8_t   type;     // delete/insert/placeholder
    uint32_t  vallen;   // number of bytes in value
    void *    valp;     // pointer to value  (Where is value really stored?)
    TXNID     xid;      // transaction id
    // Note: when packing ule into a new leafentry, will need
    //       to copy actual data from valp to new leafentry
} UXR_S, *UXR;


// Unpacked Leaf Entry is of fixed size because it's just on the 
// stack and we care about ease of access more than the memory footprint.
typedef struct ule {     // unpacked leaf entry
    uint32_t  num_puxrs;   // how many of uxrs[] are provisional
    uint32_t  num_cuxrs;   // how many of uxrs[] are committed
    UXR_S     uxrs_static[MAX_TRANSACTION_RECORDS*2];    // uxrs[0] is oldest committed (txn commit time, not txn start time), uxrs[num_cuxrs] is outermost provisional value (if any exist/num_puxrs > 0)
    UXR       uxrs;                                      //If num_cuxrs < MAX_TRANSACTION_RECORDS then &uxrs_static[0].
                                                         //Otherwise we use a dynamically allocated array of size num_cuxrs + 1 + MAX_TRANSATION_RECORD.
} ULE_S, *ULE;



void test_msg_modify_ule(ULE ule, const ft_msg &msg);


//////////////////////////////////////////////////////////////////////////////////////
//Functions exported for test purposes only (used internally for non-test purposes).
void le_unpack(ULE ule,  LEAFENTRY le);
int
le_pack(ULE ule, // data to be packed into new leafentry
        bn_data* data_buffer,
        uint32_t idx,
        void* keyp,
        uint32_t keylen,
        uint32_t old_keylen,
        uint32_t old_le_size,
        LEAFENTRY * const new_leafentry_p, // this is what this function creates
        void **const maybe_free
        );


size_t le_memsize_from_ule (ULE ule);
void ule_cleanup(ULE ule);
