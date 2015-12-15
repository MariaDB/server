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

// This toku_list is intended to be embedded in other data structures.
struct toku_list {
    struct toku_list *next, *prev;
};

static inline int toku_list_num_elements_est(struct toku_list *head) {
    if (head->next == head) return 0;
    if (head->next == head->prev) return 1;
    return 2;
}


static inline void toku_list_init(struct toku_list *head) {
    head->next = head->prev = head;
}

static inline int toku_list_empty(struct toku_list *head) {
    return head->next == head;
}

static inline struct toku_list *toku_list_head(struct toku_list *head) {
    return head->next;
}

static inline struct toku_list *toku_list_tail(struct toku_list *head) {
    return head->prev;
}

static inline void toku_list_insert_between(struct toku_list *a, struct toku_list *toku_list, struct toku_list *b) {

    toku_list->next = a->next;
    toku_list->prev = b->prev;
    a->next = b->prev = toku_list;
}

static inline void toku_list_push(struct toku_list *head, struct toku_list *toku_list) {
    toku_list_insert_between(head->prev, toku_list, head);
}

static inline void toku_list_push_head(struct toku_list *head, struct toku_list *toku_list) {
    toku_list_insert_between(head, toku_list, head->next);
}

static inline void toku_list_remove(struct toku_list *toku_list) {
    struct toku_list *prev = toku_list->prev;
    struct toku_list *next = toku_list->next;
    next->prev = prev;
    prev->next = next;
    toku_list_init(toku_list); // Set the toku_list element to be empty
}

static inline struct toku_list *toku_list_pop(struct toku_list *head) {
    struct toku_list *toku_list = head->prev;
    toku_list_remove(toku_list);
    return toku_list;
}

static inline struct toku_list *toku_list_pop_head(struct toku_list *head) {
    struct toku_list *toku_list = head->next;
    toku_list_remove(toku_list);
    return toku_list;
}

static inline void toku_list_move(struct toku_list *newhead, struct toku_list *oldhead) {
    struct toku_list *first = oldhead->next;
    struct toku_list *last = oldhead->prev;
    // assert(!toku_list_empty(oldhead));
    newhead->next = first;
    newhead->prev = last;
    last->next = first->prev = newhead;
    toku_list_init(oldhead);
}

// Note: Need the extra level of parens in these macros so that
//   toku_list_struct(h, foo, b)->zot
// will work right.  Otherwise the type cast will try to include ->zot, and it will be all messed up.
#if ((defined(__GNUC__) && __GNUC__ >= 4) || defined(__builtin_offsetof) ) && !defined(__clang__) 
#define toku_list_struct(p, t, f) ((t*)((char*)(p) - __builtin_offsetof(t, f)))
#else
#define toku_list_struct(p, t, f) ((t*)((char*)(p) - ((char*)&((t*)0)->f)))
#endif
