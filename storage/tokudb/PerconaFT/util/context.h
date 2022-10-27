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

#include <portability/toku_portability.h>

#include <db.h>

#include <util/status.h>

enum context_id {
    CTX_INVALID = -1,
    CTX_DEFAULT = 0,          // default context for when no context is set
    CTX_SEARCH,               // searching for a key at the bottom of the tree
    CTX_PROMO,                // promoting a message down the tree
    CTX_FULL_FETCH,           // performing full fetch (pivots + some partial fetch)
    CTX_PARTIAL_FETCH,        // performing partial fetch
    CTX_FULL_EVICTION,        // running partial eviction
    CTX_PARTIAL_EVICTION,     // running partial eviction
    CTX_MESSAGE_INJECTION,    // injecting a message into a buffer
    CTX_MESSAGE_APPLICATION,  // applying ancestor's messages to a basement node
    CTX_FLUSH,                // flushing a buffer
    CTX_CLEANER               // doing work as the cleaner thread
};

// Note a contention event in engine status
void toku_context_note_frwlock_contention(const context_id blocking, const context_id blocked);

namespace toku {

    // class for tracking what a thread is doing
    //
    // usage:
    //
    // // automatically tag and document what you're doing
    // void my_interesting_function(void) {
    //     toku::context ctx("doing something interesting", INTERESTING_FN_1);
    //     ...
    //     {
    //         toku::context inner_ctx("doing something expensive", EXPENSIVE_FN_1);
    //         my_rwlock.wrlock();
    //         expensive();
    //         my_rwlock.wrunlock();
    //     }
    //     ...
    // }
    //
    // // ... so later you can write code like this.
    // // here, we save some info to help determine why a lock could not be acquired
    // void my_rwlock::wrlock() {
    //     r = try_acquire_write_lock();
    //     if (r == 0) {
    //         m_write_locked_context_id = get_thread_local_context()->get_id();
    //         ...
    //     } else {
    //         if (m_write_locked_context_id == EXPENSIVE_FN_1) {
    //             status.blocked_because_of_expensive_fn_1++;
    //         } else if (...) {
    //            ...
    //         }
    //         ...
    //     }
    // }
    class context {
    public:
        context(const context_id id); 

        ~context();

        context_id get_id() const {
            return m_id;
        }

    private:
        // each thread has a stack of contexts, rooted at the trivial "root context"
        const context *m_old_ctx;
        const context_id m_id;
    };

} // namespace toku

// Get the current context of this thread
const toku::context *toku_thread_get_context();

enum context_status_entry {
    CTX_SEARCH_BLOCKED_BY_FULL_FETCH = 0,
    CTX_SEARCH_BLOCKED_BY_PARTIAL_FETCH,
    CTX_SEARCH_BLOCKED_BY_FULL_EVICTION,
    CTX_SEARCH_BLOCKED_BY_PARTIAL_EVICTION,
    CTX_SEARCH_BLOCKED_BY_MESSAGE_INJECTION,
    CTX_SEARCH_BLOCKED_BY_MESSAGE_APPLICATION,
    CTX_SEARCH_BLOCKED_BY_FLUSH,
    CTX_SEARCH_BLOCKED_BY_CLEANER,
    CTX_SEARCH_BLOCKED_OTHER,
    CTX_PROMO_BLOCKED_BY_FULL_FETCH,
    CTX_PROMO_BLOCKED_BY_PARTIAL_FETCH,
    CTX_PROMO_BLOCKED_BY_FULL_EVICTION,
    CTX_PROMO_BLOCKED_BY_PARTIAL_EVICTION,
    CTX_PROMO_BLOCKED_BY_MESSAGE_INJECTION,
    CTX_PROMO_BLOCKED_BY_MESSAGE_APPLICATION,
    CTX_PROMO_BLOCKED_BY_FLUSH,
    CTX_PROMO_BLOCKED_BY_CLEANER,
    CTX_PROMO_BLOCKED_OTHER,
    CTX_BLOCKED_OTHER,
    CTX_STATUS_NUM_ROWS
};

struct context_status {
    bool initialized;
    TOKU_ENGINE_STATUS_ROW_S status[CTX_STATUS_NUM_ROWS];
};

void toku_context_get_status(struct context_status *status);

void toku_context_status_init(void);
void toku_context_status_destroy(void);
