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
#include <toku_pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <util/context.h>

//TODO: update comment, this is from rwlock.h

namespace toku {

class frwlock {
public:

    void init(toku_mutex_t *const mutex);
    void deinit(void);

    void write_lock(bool expensive);
    bool try_write_lock(bool expensive);
    void write_unlock(void);
    // returns true if acquiring a write lock will be expensive
    bool write_lock_is_expensive(void);

    void read_lock(void);
    bool try_read_lock(void);
    void read_unlock(void);
    // returns true if acquiring a read lock will be expensive
    bool read_lock_is_expensive(void);

    uint32_t users(void) const;
    uint32_t blocked_users(void) const;
    uint32_t writers(void) const;
    uint32_t blocked_writers(void) const;
    uint32_t readers(void) const;
    uint32_t blocked_readers(void) const;

private:
    struct queue_item {
        toku_cond_t *cond;
        struct queue_item *next;
    };

    bool queue_is_empty(void) const;
    void enq_item(queue_item *const item);
    toku_cond_t *deq_item(void);
    void maybe_signal_or_broadcast_next(void);
    void maybe_signal_next_writer(void);

    toku_mutex_t *m_mutex;

    uint32_t m_num_readers;
    uint32_t m_num_writers;
    uint32_t m_num_want_write;
    uint32_t m_num_want_read;
    uint32_t m_num_signaled_readers;
    // number of writers waiting that are expensive
    // MUST be < m_num_want_write
    uint32_t m_num_expensive_want_write;
    // bool that states if the current writer is expensive
    // if there is no current writer, then is false
    bool m_current_writer_expensive;
    // bool that states if waiting for a read
    // is expensive
    // if there are currently no waiting readers, then set to false
    bool m_read_wait_expensive;
    // thread-id of the current writer
    int m_current_writer_tid;
    // context id describing the context of the current writer blocking
    // new readers (either because this writer holds the write lock or
    // is the first to want the write lock).
    context_id m_blocking_writer_context_id;
    
    toku_cond_t m_wait_read;
    queue_item m_queue_item_read;
    bool m_wait_read_is_in_queue;

    queue_item *m_wait_head;
    queue_item *m_wait_tail;
};

ENSURE_POD(frwlock);

} // namespace toku

// include the implementation here
// #include "frwlock.cc"
