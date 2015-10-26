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

#include <db.h>

#include "ft/ft-internal.h"

/* an ft cursor is represented as a kv pair in a tree */
struct ft_cursor {
    FT_HANDLE ft_handle;
    DBT key, val;             // The key-value pair that the cursor currently points to
    DBT range_lock_left_key, range_lock_right_key;
    bool prefetching;
    bool left_is_neg_infty, right_is_pos_infty;
    enum cursor_read_type read_type; // true if query is reading from a snapshot, false otherwise
    bool is_leaf_mode;
    bool disable_prefetching;
    bool is_temporary;
    int out_of_range_error;
    int direction;
    TOKUTXN ttxn;
    FT_CHECK_INTERRUPT_CALLBACK interrupt_cb;
    void *interrupt_cb_extra;
};
typedef struct ft_cursor *FT_CURSOR;

enum ft_search_direction_e {
    FT_SEARCH_LEFT = 1,  /* search left -> right, finds min xy as defined by the compare function */
    FT_SEARCH_RIGHT = 2, /* search right -> left, finds max xy as defined by the compare function */
};

struct ft_search;

/* the search compare function should return 0 for all xy < kv and 1 for all xy >= kv
   the compare function should be a step function from 0 to 1 for a left to right search
   and 1 to 0 for a right to left search */

typedef int (*ft_search_compare_func_t)(const struct ft_search &, const DBT *);

/* the search object contains the compare function, search direction, and the kv pair that
   is used in the compare function.  the context is the user's private data */

struct ft_search {
    ft_search_compare_func_t compare;
    enum ft_search_direction_e direction;
    const DBT *k;
    void *context;
    
    // To fix #3522, we need to remember the pivots that we have searched unsuccessfully.
    // For example, when searching right (left), we call search->compare() on the ith pivot key.  If search->compare(0 returns
    //  nonzero, then we search the ith subtree.  If that subsearch returns DB_NOTFOUND then maybe the key isn't present in the
    //  tree.  But maybe we are doing a DB_NEXT (DB_PREV), and everything was deleted.  So we remember the pivot, and later we
    //  will only search subtrees which contain keys that are bigger than (less than) the pivot.
    // The code is a kludge (even before this fix), and interacts strangely with the TOKUDB_FOUND_BUT_REJECTED (which is there
    //  because a failed DB_GET we would keep searching the rest of the tree).  We probably should write the various lookup
    //  codes (NEXT, PREV, CURRENT, etc) more directly, and we should probably use a binary search within a node to search the
    //  pivots so that we can support a larger fanout.
    // These changes (3312+3522) also (probably) introduce an isolation error (#3529).
    //  We must make sure we lock the right range for proper isolation level.
    //  There's probably a bug in which the following could happen.
    //      Thread A:  Searches through deleted keys A,B,D,E and finds nothing, so searches the next leaf, releasing the YDB lock.
    //      Thread B:  Inserts key C, and acquires the write lock, then commits.
    //      Thread A:  Resumes, searching F,G,H and return success.  Thread A then read-locks the range A-H, and doesn't notice
    //        the value C inserted by thread B.  Thus a failure of serialization.
    //     See #3529.
    // There also remains a potential thrashing problem.  When we get a TOKUDB_TRY_AGAIN, we unpin everything.  There's
    //   no guarantee that we will get everything pinned again.  We ought to keep nodes pinned when we retry, except that on the
    //   way out with a DB_NOTFOUND we ought to unpin those nodes.  See #3528.
    DBT pivot_bound;
    const DBT *k_bound;
};

/* initialize the search compare object */
static inline ft_search *ft_search_init(ft_search *search, ft_search_compare_func_t compare,
                                        enum ft_search_direction_e direction, 
                                        const DBT *k, const DBT *k_bound, void *context) {
    search->compare = compare;
    search->direction = direction;
    search->k = k;
    search->context = context;
    toku_init_dbt(&search->pivot_bound);
    search->k_bound = k_bound;
    return search;
}

static inline void ft_search_finish(ft_search *search) {
    toku_destroy_dbt(&search->pivot_bound);
}


int toku_ft_cursor_create(FT_HANDLE ft_handle, FT_CURSOR cursor, TOKUTXN txn,
                          enum cursor_read_type read_type,
                          bool disable_prefetching,
                          bool is_temporary);

void toku_ft_cursor_destroy(FT_CURSOR cursor);

int toku_ft_lookup(FT_HANDLE ft_h, DBT *k, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)  __attribute__ ((warn_unused_result));

void toku_ft_cursor_set_prefetching(FT_CURSOR cursor);

bool toku_ft_cursor_prefetching(FT_CURSOR cursor);

bool toku_ft_cursor_not_set(FT_CURSOR cursor);

void toku_ft_cursor_set_leaf_mode(FT_CURSOR cursor);

void toku_ft_cursor_remove_restriction(FT_CURSOR cursor);

void toku_ft_cursor_set_check_interrupt_cb(FT_CURSOR cursor, FT_CHECK_INTERRUPT_CALLBACK cb, void *extra);

int toku_ft_cursor_is_leaf_mode(FT_CURSOR cursor);

void toku_ft_cursor_set_range_lock(FT_CURSOR, const DBT *, const DBT *, bool, bool, int);

int toku_ft_cursor_first(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

int toku_ft_cursor_last(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

int toku_ft_cursor_next(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

int toku_ft_cursor_prev(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

int toku_ft_cursor_current(FT_CURSOR cursor, int op, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

int toku_ft_cursor_set(FT_CURSOR cursor, DBT *key, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

int toku_ft_cursor_set_range(FT_CURSOR cursor, DBT *key, DBT *key_bound, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

int toku_ft_cursor_set_range_reverse(FT_CURSOR cursor, DBT *key, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) __attribute__ ((warn_unused_result));

bool toku_ft_cursor_uninitialized(FT_CURSOR cursor) __attribute__ ((warn_unused_result));

void toku_ft_cursor_peek(FT_CURSOR cursor, const DBT **pkey, const DBT **pval);

int toku_ft_cursor_check_restricted_range(FT_CURSOR cursor, const void *key, uint32_t keylen);

int toku_ft_cursor_shortcut(FT_CURSOR cursor, int direction, uint32_t index, bn_data *bd,
                            FT_GET_CALLBACK_FUNCTION getf, void *getf_v,
                            uint32_t *keylen, void **key, uint32_t *vallen, void **val);

// used by get_key_after_bytes
int toku_ft_cursor_compare_one(const ft_search &search, const DBT *x);
int toku_ft_cursor_compare_set_range(const ft_search &search, const DBT *x);

// deprecated, should only be used by tests, and eventually removed
int toku_ft_cursor(FT_HANDLE ft_handle, FT_CURSOR *ftcursor_p, TOKUTXN txn, bool, bool) __attribute__ ((warn_unused_result));
void toku_ft_cursor_close(FT_CURSOR cursor);
int toku_ft_cursor_get(FT_CURSOR cursor, DBT *key, FT_GET_CALLBACK_FUNCTION getf, void *getf_v, int get_flags);
int toku_ft_cursor_delete(FT_CURSOR cursor, int flags, TOKUTXN txn);
