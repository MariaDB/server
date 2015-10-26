/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
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

#include "ft/msg.h"
#include "ft/txn/xids.h"
#include "util/dbt.h"

class message_buffer {
public:
    void create();

    void clone(message_buffer *dst);

    void destroy();

    // effect: deserializes a message buffer from the given rbuf
    // returns: *fresh_offsets (etc) malloc'd to be num_entries large and
    //          populated with *nfresh (etc) offsets in the message buffer
    // requires: if fresh_offsets (etc) != nullptr, then nfresh != nullptr
    void deserialize_from_rbuf(struct rbuf *rb,
                               int32_t **fresh_offsets, int32_t *nfresh,
                               int32_t **stale_offsets, int32_t *nstale,
                               int32_t **broadcast_offsets, int32_t *nbroadcast);

    // effect: deserializes a message buffer whose messages are at version 13/14
    // returns: similar to deserialize_from_rbuf(), excpet there are no stale messages
    //          and each message is assigned a sequential value from *highest_unused_msn_for_upgrade,
    //          which is modified as needed using toku_sync_fech_and_sub()
    // returns: the highest MSN assigned to any message in this buffer
    // requires: similar to deserialize_from_rbuf(), and highest_unused_msn_for_upgrade != nullptr
    MSN deserialize_from_rbuf_v13(struct rbuf *rb,
                                  MSN *highest_unused_msn_for_upgrade,
                                  int32_t **fresh_offsets, int32_t *nfresh,
                                  int32_t **broadcast_offsets, int32_t *nbroadcast);

    void enqueue(const ft_msg &msg, bool is_fresh, int32_t *offset);

    void set_freshness(int32_t offset, bool is_fresh);

    bool get_freshness(int32_t offset) const;

    ft_msg get_message(int32_t offset, DBT *keydbt, DBT *valdbt) const;

    void get_message_key_msn(int32_t offset, DBT *key, MSN *msn) const;

    int num_entries() const;

    size_t buffer_size_in_use() const;

    size_t memory_size_in_use() const;

    size_t memory_footprint() const;

    template <typename F>
    int iterate(F &fn) const {
        for (int32_t offset = 0; offset < _memory_used; ) {
            DBT k, v;
            const ft_msg msg = get_message(offset, &k, &v);
            bool is_fresh = get_freshness(offset);
            int r = fn(msg, is_fresh);
            if (r != 0) {
                return r;
            }
            offset += msg_memsize_in_buffer(msg);
        }
        return 0;
    }

    bool equals(message_buffer *other) const;

    void serialize_to_wbuf(struct wbuf *wb) const;

    static size_t msg_memsize_in_buffer(const ft_msg &msg);

private:
    void _resize(size_t new_size);

    // If this isn't packged, the compiler aligns the xids array and we waste a lot of space
    struct __attribute__((__packed__)) buffer_entry {
        unsigned int  keylen;
        unsigned int  vallen;
        unsigned char type;
        bool          is_fresh;
        MSN           msn;
        XIDS_S        xids_s;
    };

    struct buffer_entry *get_buffer_entry(int32_t offset) const;

    int   _num_entries;
    char *_memory;       // An array of bytes into which buffer entries are embedded.
    int   _memory_size;  // How big is _memory
    int   _memory_used;  // How many bytes are in use?
    size_t _memory_usable; // a cached result of toku_malloc_usable_size(_memory).
};
