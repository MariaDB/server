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

/* The purpose of this file is to provide access to the ft_msg,
 * which is the ephemeral version of the messages that lives in
 * a message buffer.
 */
#pragma once

#include <db.h>

#include "portability/toku_assert.h"
#include "portability/toku_stdint.h"

#include "ft/txn/xids.h"

// Message Sequence Number (MSN)
typedef struct __toku_msn { uint64_t msn; } MSN;

// dummy used for message construction, to be filled in when msg is applied to tree
static const MSN ZERO_MSN = { .msn = 0 };

// first 2^62 values reserved for messages created before Dr. No (for upgrade)
static const MSN MIN_MSN = { .msn = 1ULL << 62 };
static const MSN MAX_MSN = { .msn = UINT64_MAX };

/* tree command types */
enum ft_msg_type {
    FT_NONE = 0,
    FT_INSERT = 1,
    FT_DELETE_ANY = 2,  // Delete any matching key.  This used to be called FT_DELETE.
    //FT_DELETE_BOTH = 3,
    FT_ABORT_ANY = 4,   // Abort any commands on any matching key.
    //FT_ABORT_BOTH  = 5, // Abort commands that match both the key and the value
    FT_COMMIT_ANY  = 6,
    //FT_COMMIT_BOTH = 7,
    FT_COMMIT_BROADCAST_ALL = 8, // Broadcast to all leafentries, (commit all transactions).
    FT_COMMIT_BROADCAST_TXN = 9, // Broadcast to all leafentries, (commit specific transaction).
    FT_ABORT_BROADCAST_TXN  = 10, // Broadcast to all leafentries, (commit specific transaction).
    FT_INSERT_NO_OVERWRITE = 11,
    FT_OPTIMIZE = 12,             // Broadcast
    FT_OPTIMIZE_FOR_UPGRADE = 13, // same as FT_OPTIMIZE, but record version number in leafnode
    FT_UPDATE = 14,
    FT_UPDATE_BROADCAST_ALL = 15
};

static inline bool
ft_msg_type_applies_once(enum ft_msg_type type)
{
    bool ret_val;
    switch (type) {
    case FT_INSERT_NO_OVERWRITE:
    case FT_INSERT:
    case FT_DELETE_ANY:
    case FT_ABORT_ANY:
    case FT_COMMIT_ANY:
    case FT_UPDATE:
        ret_val = true;
        break;
    case FT_COMMIT_BROADCAST_ALL:
    case FT_COMMIT_BROADCAST_TXN:
    case FT_ABORT_BROADCAST_TXN:
    case FT_OPTIMIZE:
    case FT_OPTIMIZE_FOR_UPGRADE:
    case FT_UPDATE_BROADCAST_ALL:
    case FT_NONE:
        ret_val = false;
        break;
    default:
        assert(false);
    }
    return ret_val;
}

static inline bool
ft_msg_type_applies_all(enum ft_msg_type type)
{
    bool ret_val;
    switch (type) {
    case FT_NONE:
    case FT_INSERT_NO_OVERWRITE:
    case FT_INSERT:
    case FT_DELETE_ANY:
    case FT_ABORT_ANY:
    case FT_COMMIT_ANY:
    case FT_UPDATE:
        ret_val = false;
        break;
    case FT_COMMIT_BROADCAST_ALL:
    case FT_COMMIT_BROADCAST_TXN:
    case FT_ABORT_BROADCAST_TXN:
    case FT_OPTIMIZE:
    case FT_OPTIMIZE_FOR_UPGRADE:
    case FT_UPDATE_BROADCAST_ALL:
        ret_val = true;
        break;
    default:
        assert(false);
    }
    return ret_val;
}

static inline bool
ft_msg_type_does_nothing(enum ft_msg_type type)
{
    return (type == FT_NONE);
}

class ft_msg {
public:
    ft_msg(const DBT *key, const DBT *val, enum ft_msg_type t, MSN m, XIDS x);

    enum ft_msg_type type() const;

    MSN msn() const;

    XIDS xids() const;

    const DBT *kdbt() const;

    const DBT *vdbt() const;

    size_t total_size() const;

    void serialize_to_wbuf(struct wbuf *wb, bool is_fresh) const;

    // deserialization goes through a static factory function so the ft msg
    // API stays completely const and there's no default constructor
    static ft_msg deserialize_from_rbuf(struct rbuf *rb, XIDS *xids, bool *is_fresh);

    // Version 13/14 messages did not have an msn - so `m' is the MSN
    // that will be assigned to the message that gets deserialized.
    static ft_msg deserialize_from_rbuf_v13(struct rbuf *rb, MSN m, XIDS *xids);

private:
    const DBT _key;
    const DBT _val;
    enum ft_msg_type _type;
    MSN _msn;
    XIDS _xids;
};

// For serialize / deserialize

#include "ft/serialize/wbuf.h"

static inline void wbuf_MSN(struct wbuf *wb, MSN msn) {
    wbuf_ulonglong(wb, msn.msn);
}

#include "ft/serialize/rbuf.h"

static inline MSN rbuf_MSN(struct rbuf *rb) {
    MSN msn = { .msn = rbuf_ulonglong(rb) };
    return msn;
}
