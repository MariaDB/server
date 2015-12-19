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

#include <toku_portability.h>
#include "toku_os.h"
#include "ft-internal.h"
#include "loader/loader-internal.h"
#include "loader/pqueue.h"

#define pqueue_left(i)   ((i) << 1)
#define pqueue_right(i)  (((i) << 1) + 1)
#define pqueue_parent(i) ((i) >> 1)

int pqueue_init(pqueue_t **result, size_t n, int which_db, DB *db, ft_compare_func compare, struct error_callback_s *err_callback)
{
    pqueue_t *MALLOC(q);
    if (!q) {
        return get_error_errno();
    }

    /* Need to allocate n+1 elements since element 0 isn't used. */
    MALLOC_N(n + 1, q->d);
    if (!q->d) {
        int r = get_error_errno();
        toku_free(q);
        return r;
    }
    q->size = 1;
    q->avail = q->step = (n+1);  /* see comment above about n+1 */

    q->which_db = which_db;
    q->db = db;
    q->compare = compare;
    q->dup_error = 0;

    q->error_callback = err_callback;

    *result = q;
    return 0;
}

void pqueue_free(pqueue_t *q)
{
    toku_free(q->d);
    toku_free(q);
}


size_t pqueue_size(pqueue_t *q)
{
    /* queue element 0 exists but doesn't count since it isn't used. */
    return (q->size - 1);
}

static int pqueue_compare(pqueue_t *q, DBT *next_key, DBT *next_val, DBT *curr_key)
{
    int r = q->compare(q->db, next_key, curr_key);
    if ( r == 0 ) { // duplicate key : next_key == curr_key
        q->dup_error = 1; 
        if (q->error_callback)
            ft_loader_set_error_and_callback(q->error_callback, DB_KEYEXIST, q->db, q->which_db, next_key, next_val);
    }
    return ( r > -1 );
}

static void pqueue_bubble_up(pqueue_t *q, size_t i)
{
    size_t parent_node;
    pqueue_node_t *moving_node = q->d[i];
    DBT *moving_key = moving_node->key;

    for (parent_node = pqueue_parent(i);
         ((i > 1) && pqueue_compare(q, q->d[parent_node]->key, q->d[parent_node]->val, moving_key));
         i = parent_node, parent_node = pqueue_parent(i))
    {
        q->d[i] = q->d[parent_node];
    }

    q->d[i] = moving_node;
}


static size_t pqueue_maxchild(pqueue_t *q, size_t i)
{
    size_t child_node = pqueue_left(i);

    if (child_node >= q->size)
        return 0;

    if ((child_node+1) < q->size &&
        pqueue_compare(q, q->d[child_node]->key, q->d[child_node]->val, q->d[child_node+1]->key))
        child_node++; /* use right child instead of left */

    return child_node;
}


static void pqueue_percolate_down(pqueue_t *q, size_t i)
{
    size_t child_node;
    pqueue_node_t *moving_node = q->d[i];
    DBT *moving_key = moving_node->key;
    DBT *moving_val = moving_node->val;

    while ((child_node = pqueue_maxchild(q, i)) &&
           pqueue_compare(q, moving_key, moving_val, q->d[child_node]->key))
    {
        q->d[i] = q->d[child_node];
        i = child_node;
    }

    q->d[i] = moving_node;
}


int pqueue_insert(pqueue_t *q, pqueue_node_t *d)
{
    size_t i;

    if (!q) return 1;
    if (q->size >= q->avail) return 1;

    /* insert item */
    i = q->size++;
    q->d[i] = d;
    pqueue_bubble_up(q, i);

    if ( q->dup_error ) return DB_KEYEXIST;
    return 0;
}

int pqueue_pop(pqueue_t *q, pqueue_node_t **d)
{
    if (!q || q->size == 1) {
        *d = NULL;
        return 0;
    }

    *d = q->d[1];
    q->d[1] = q->d[--q->size];
    pqueue_percolate_down(q, 1);

    if ( q->dup_error ) return DB_KEYEXIST;
    return 0;
}
