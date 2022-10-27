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

#include <string.h>

#include <util/context.h>

namespace toku {

    static const context default_context(CTX_DEFAULT);
    static __thread const context *tl_current_context = &default_context;

    // save the old context, set the current context
    context::context(const context_id id) :
        m_old_ctx(tl_current_context),
        m_id(id) {
        tl_current_context = this;
    }

    // restore the old context
    context::~context() {
        tl_current_context = m_old_ctx;
    }

} // namespace toku

// thread local context

const toku::context *toku_thread_get_context() {
    return toku::tl_current_context;
}

// engine status

static struct context_status context_status;
#define CONTEXT_STATUS_INIT(key, legend) TOKUFT_STATUS_INIT(context_status, key, nullptr, PARCOUNT, "context: " legend, TOKU_ENGINE_STATUS)

void toku_context_status_init(void) {
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_FULL_FETCH,           "tree traversals blocked by a full fetch");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_PARTIAL_FETCH,        "tree traversals blocked by a partial fetch");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_FULL_EVICTION,        "tree traversals blocked by a full eviction");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_PARTIAL_EVICTION,     "tree traversals blocked by a partial eviction");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_MESSAGE_INJECTION,    "tree traversals blocked by a message injection");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_MESSAGE_APPLICATION,  "tree traversals blocked by a message application");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_FLUSH,                "tree traversals blocked by a flush");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_BY_CLEANER,              "tree traversals blocked by a the cleaner thread");
    CONTEXT_STATUS_INIT(CTX_SEARCH_BLOCKED_OTHER,                   "tree traversals blocked by something uninstrumented");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_FULL_FETCH,            "promotion blocked by a full fetch (should never happen)");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_PARTIAL_FETCH,         "promotion blocked by a partial fetch (should never happen)");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_FULL_EVICTION,         "promotion blocked by a full eviction (should never happen)");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_PARTIAL_EVICTION,      "promotion blocked by a partial eviction (should never happen)");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_MESSAGE_INJECTION,     "promotion blocked by a message injection");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_MESSAGE_APPLICATION,   "promotion blocked by a message application");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_FLUSH,                 "promotion blocked by a flush");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_BY_CLEANER,               "promotion blocked by the cleaner thread");
    CONTEXT_STATUS_INIT(CTX_PROMO_BLOCKED_OTHER,                    "promotion blocked by something uninstrumented");
    CONTEXT_STATUS_INIT(CTX_BLOCKED_OTHER,                          "something uninstrumented blocked by something uninstrumented");
    context_status.initialized = true;
}
#undef FS_STATUS_INIT

void toku_context_get_status(struct context_status *status) {
    assert(context_status.initialized);
    *status = context_status;
}

#define STATUS_INC(x, d) increment_partitioned_counter(context_status.status[x].value.parcount, d);

void toku_context_note_frwlock_contention(const context_id blocked, const context_id blocking) {
    assert(context_status.initialized);
    if (blocked != CTX_SEARCH && blocked != CTX_PROMO) {
        // Return early if this event is "unknown"
        STATUS_INC(CTX_BLOCKED_OTHER, 1);
        return;
    }
    switch (blocking) {
    case CTX_FULL_FETCH:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_FULL_FETCH, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_FULL_FETCH, 1);
        }
        break;
    case CTX_PARTIAL_FETCH:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_PARTIAL_FETCH, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_PARTIAL_FETCH, 1);
        }
        break;
    case CTX_FULL_EVICTION:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_FULL_EVICTION, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_FULL_EVICTION, 1);
        }
        break;
    case CTX_PARTIAL_EVICTION:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_PARTIAL_EVICTION, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_PARTIAL_EVICTION, 1);
        }
        break;
    case CTX_MESSAGE_INJECTION:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_MESSAGE_INJECTION, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_MESSAGE_INJECTION, 1);
        }
        break;
    case CTX_MESSAGE_APPLICATION:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_MESSAGE_APPLICATION, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_MESSAGE_APPLICATION, 1);
        }
        break;
    case CTX_FLUSH:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_FLUSH, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_FLUSH, 1);
        }
        break;
    case CTX_CLEANER:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_BY_CLEANER, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_BY_CLEANER, 1);
        }
        break;
    default:
        if (blocked == CTX_SEARCH) {
            STATUS_INC(CTX_SEARCH_BLOCKED_OTHER, 1);
        } else if (blocked == CTX_PROMO) {
            STATUS_INC(CTX_PROMO_BLOCKED_OTHER, 1);
        }
        break;
    }
}

void toku_context_status_destroy(void) {
    for (int i = 0; i < CTX_STATUS_NUM_ROWS; ++i) {
        if (context_status.status[i].type == PARCOUNT) {
            destroy_partitioned_counter(context_status.status[i].value.parcount);
        }
    }
}
