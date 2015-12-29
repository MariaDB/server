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

typedef struct ft_pqueue_node_t
{
    DBT   *key;
    DBT   *val;
    int      i;
} pqueue_node_t;

typedef struct ft_pqueue_t
{
    size_t size;
    size_t avail;
    size_t step;

    int which_db;
    DB *db;  // needed for compare function
    ft_compare_func compare;
    pqueue_node_t **d;
    int dup_error;

    struct error_callback_s *error_callback;

} pqueue_t;

int pqueue_init(pqueue_t **result, size_t n, int which_db, DB *db, ft_compare_func compare, struct error_callback_s *err_callback);
void pqueue_free(pqueue_t *q);
size_t pqueue_size(pqueue_t *q);
int pqueue_insert(pqueue_t *q, pqueue_node_t *d);
int pqueue_pop(pqueue_t *q, pqueue_node_t **d);
