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

//
// The kibbutz is another threadpool meant to do arbitrary work.
//

typedef struct kibbutz *KIBBUTZ;
//
// create a kibbutz where n_workers is the number of threads in the threadpool
//
int toku_kibbutz_create (int n_workers, KIBBUTZ *kb);
//
// enqueue a workitem in the kibbutz. When the kibbutz is to work on this workitem,
// it calls f(extra). 
// At any time, the kibbutz is operating on at most n_workers jobs. 
// Other enqueued workitems are on a queue. An invariant is 
// that no currently enqueued item was placed on the queue before 
// any item that is currently being operated on. Another way to state
// this is that all items on the queue were placed there before any item
// that is currently being worked on
//
void toku_kibbutz_enq (KIBBUTZ k, void (*f)(void*), void *extra);
//
// get kibbuts status
//
void toku_kibbutz_get_status(KIBBUTZ k,
                             uint64_t *num_threads,
                             uint64_t *num_threads_active,
                             uint64_t *queue_size,
                             uint64_t *max_queue_size,
                             uint64_t *total_items_processed,
                             uint64_t *total_execution_time);
//
// destroys the kibbutz
//
void toku_kibbutz_destroy (KIBBUTZ k);
