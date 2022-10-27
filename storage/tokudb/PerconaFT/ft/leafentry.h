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

#include <toku_portability.h>

#include <util/mempool.h>
#include <util/omt.h>

#include "ft/txn/txn_manager.h"
#include "ft/serialize/rbuf.h"
#include "ft/msg.h"

/*
    Memory format of packed leaf entry
    CONSTANTS:
        num_uxrs
        keylen
    Run-time-constants
        voffset of val/vallen??? (for le_any_val) This must be small if it is interpreted as voffset = realoffset_of_val - keylen
            GOOD performance optimization.
            ALSO good for simplicity (no having to scan packed version)
        key[]
    variable length
        
        
    Memory format of packed dup leaf entry
    CONSTANTS:
        num_uxrs
        keylen
        vallen
    Run-time-constants
        key[]
        val[]
*/

enum cursor_read_type {
    C_READ_ANY = 0,
    C_READ_SNAPSHOT = 1,
    C_READ_COMMITTED = 2
};

//
// enum of possible values for LEAFENTRY->type field
// LE_CLEAN means that there is a single committed value in a format that saves disk space
// LE_MVCC means that there may be multiple committed values or there are provisional values
//
enum { LE_CLEAN = 0, LE_MVCC = 1 };

// This is an on-disk format.  static_asserts verify everything is packed and aligned correctly.
struct leafentry {
    struct leafentry_clean {
        uint32_t vallen;
        uint8_t  val[0];     //actual val
    }; // For the case where LEAFENTRY->type is LE_CLEAN
    static_assert(4 == sizeof(leafentry::leafentry_clean), "leafentry_clean size is wrong");
    static_assert(4 == __builtin_offsetof(leafentry::leafentry_clean, val), "val is in the wrong place");
    struct __attribute__ ((__packed__)) leafentry_mvcc {
        uint32_t num_cxrs; // number of committed transaction records
        uint8_t  num_pxrs; // number of provisional transaction records
        uint8_t xrs[0];      //then TXNIDs of XRs relevant for reads:
                             //  if provisional XRs exist, store OUTERMOST TXNID
                             //  store committed TXNIDs, from most recently committed to least recently committed (newest first)
                             //then lengths of XRs relevant for reads (length is at most 1<<31, MSB is 1 for insert, 0 for delete):
                             //  if provisional XRs exist (num_pxrs>0), store length and insert/delete flag associated with INNERMOST TXNID
                             //  store length and insert/delete flag associated with each committed TXNID, in same order as above (newest first)
                             //then data of XRs relevant for reads
                             //  if provisional XRs exist (num_pxrs>0), store data associated with INNERMOST provisional TXNID
                             //  store data associated with committed TXNIDs (all committed data, newest committed values first)
                             //if provisional XRs still exist (that is, num_puxrs > 1, so INNERMOST provisional TXNID != OUTERMOST provisional TXNID):
                             //  for OUTERMOST provisional XR:
                             //    1 byte: store type (insert/delete/placeholder)
                             //    4 bytes: length (if type is INSERT, no length stored if placeholder or delete)
                             //    data
                             //  for rest of provisional stack (if num_pxrs > 2), from second-outermost to second-innermost (outermost is stored above, innermost is stored separately):
                             //   8 bytes: TXNID
                             //   1 byte: store type (insert/delete/placeholder)
                             //   4 bytes: length (if type is INSERT)
                             //   data
                             //  for INNERMOST provisional XR:
                             //   8 bytes: TXNID
                             //   (innermost data and length with insert/delete flag are stored above, cannot be a placeholder)
    }; // For the case where LEAFENTRY->type is LE_MVCC
    static_assert(5 == sizeof(leafentry::leafentry_mvcc), "leafentry_mvcc size is wrong");
    static_assert(5 == __builtin_offsetof(leafentry::leafentry_mvcc, xrs), "xrs is in the wrong place");

    uint8_t  type;    // type is LE_CLEAN or LE_MVCC
    //uint32_t keylen;
    union __attribute__ ((__packed__)) {
        struct leafentry_clean clean;
        struct leafentry_mvcc mvcc;
    } u;
};
static_assert(6 == sizeof(leafentry), "leafentry size is wrong");
static_assert(1 == __builtin_offsetof(leafentry, u), "union is in the wrong place");

#define LE_CLEAN_MEMSIZE(_vallen)                       \
    (sizeof(((LEAFENTRY)NULL)->type)            /* type */       \
    +sizeof(((LEAFENTRY)NULL)->u.clean.vallen)  /* vallen */     \
    +(_vallen))                                   /* actual val */

#define LE_MVCC_COMMITTED_HEADER_MEMSIZE                          \
    (sizeof(((LEAFENTRY)NULL)->type)            /* type */        \
    +sizeof(((LEAFENTRY)NULL)->u.mvcc.num_cxrs) /* committed */   \
    +sizeof(((LEAFENTRY)NULL)->u.mvcc.num_pxrs) /* provisional */ \
    +sizeof(TXNID)                              /* transaction */ \
    +sizeof(uint32_t)                           /* length+bit */  \
    +sizeof(uint32_t))                          /* length+bit */ 

#define LE_MVCC_COMMITTED_MEMSIZE(_vallen)    \
    (LE_MVCC_COMMITTED_HEADER_MEMSIZE                \
    +(_vallen))                       /* actual val */


typedef struct leafentry *LEAFENTRY;
typedef struct leafentry_13 *LEAFENTRY_13;

//
// TODO: consistency among names is very poor.
//

// TODO: rename this helper function for deserialization
size_t leafentry_rest_memsize(uint32_t num_puxrs, uint32_t num_cuxrs, uint8_t* start);
size_t leafentry_memsize (LEAFENTRY le); // the size of a leafentry in memory.
size_t leafentry_disksize (LEAFENTRY le); // this is the same as logsizeof_LEAFENTRY.  The size of a leafentry on disk.
void wbuf_nocrc_LEAFENTRY(struct wbuf *w, LEAFENTRY le);
int print_klpair (FILE *outf, const void* key, uint32_t keylen, LEAFENTRY v); // Print a leafentry out in human-readable form.

int le_latest_is_del(LEAFENTRY le); // Return true if it is a provisional delete.
int le_val_is_del(LEAFENTRY le, enum cursor_read_type read_type, TOKUTXN txn); // Returns true if the value that is to be read is empty
bool le_is_clean(LEAFENTRY le); //Return how many xids exist (0 does not count)
bool le_has_xids(LEAFENTRY le, XIDS xids); // Return true transaction represented by xids is still provisional in this leafentry (le's xid stack is a superset or equal to xids)
void*     le_latest_val (LEAFENTRY le); // Return the latest val (return NULL for provisional deletes)
uint32_t le_latest_vallen (LEAFENTRY le); // Return the latest vallen.  Returns 0 for provisional deletes.
void* le_latest_val_and_len (LEAFENTRY le, uint32_t *len);

uint64_t le_outermost_uncommitted_xid (LEAFENTRY le);

//Callback contract:
//      Function checks to see if id is accepted by context.
//  Returns:
//      0:  context ignores this entry, id.
//      TOKUDB_ACCEPT: context accepts id
//      r|r!=0&&r!=TOKUDB_ACCEPT:  Quit early, return r, because something unexpected went wrong (error case)
typedef int(*LE_ITERATE_CALLBACK)(TXNID id, TOKUTXN context, bool is_provisional);

int le_iterate_val(
    LEAFENTRY le,
    LE_ITERATE_CALLBACK f,
    void** valpp,
    uint32_t* vallenp,
    TOKUTXN context);

void le_extract_val(
    LEAFENTRY le,
    // should we return the entire leafentry as the val?
    bool is_leaf_mode,
    enum cursor_read_type read_type,
    TOKUTXN ttxn,
    uint32_t* vallen,
    void** val);

size_t leafentry_disksize_13(LEAFENTRY_13 le);

int toku_le_upgrade_13_14(
    // NULL if there was no stored data.
    LEAFENTRY_13 old_leafentry,
    void** keyp,
    uint32_t* keylen,
    size_t* new_leafentry_memorysize,
    LEAFENTRY *new_leafentry_p);

class bn_data;

int64_t toku_le_apply_msg(
    const ft_msg &msg,
    // NULL if there was no stored data.
    LEAFENTRY old_leafentry,
    // bn_data storing leafentry, if NULL, means there is no bn_data
    bn_data* data_buffer,
    // index in data_buffer where leafentry is stored (and should be replaced
    uint32_t idx,
    uint32_t old_keylen,
    txn_gc_info* gc_info,
    LEAFENTRY *new_leafentry_p,
    int64_t* numbytes_delta_p);

bool toku_le_worth_running_garbage_collection(
    LEAFENTRY le,
    txn_gc_info* gc_info);

void toku_le_garbage_collect(
    LEAFENTRY old_leaf_entry,
    bn_data* data_buffer,
    uint32_t idx,
    void* keyp,
    uint32_t keylen,
    txn_gc_info* gc_info,
    LEAFENTRY* new_leaf_entry,
    int64_t* numbytes_delta_p);
