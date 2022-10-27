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

#include <stdio.h>

// A toku_thread is toku_pthread that can be cached.
struct toku_thread;

// Run a function f on a thread
// This function setups up the thread to run function f with argument arg and then wakes up
// the thread to run it.
void toku_thread_run(struct toku_thread *thread, void *(*f)(void *arg), void *arg);

// A toku_thread_pool is a pool of toku_threads.  These threads can be allocated from the pool
// and can run an arbitrary function.
struct toku_thread_pool;

typedef struct toku_thread_pool *THREADPOOL;

// Create a new threadpool
// Effects: a new threadpool is allocated and initialized. the number of threads in the threadpool is limited to max_threads.  
// If max_threads == 0 then there is no limit on the number of threads in the pool.
// Initially, there are no threads in the pool. Threads are allocated by the _get or _run functions.
// Returns: if there are no errors, the threadpool is set and zero is returned.  Otherwise, an error number is returned.
int toku_thread_pool_create(struct toku_thread_pool **threadpoolptr, int max_threads);

// Destroy a threadpool
// Effects: the calling thread joins with all of the threads in the threadpool.
// Effects: the threadpool memory is freed.
// Returns: the threadpool is set to null.
void toku_thread_pool_destroy(struct toku_thread_pool **threadpoolptr);

// Get the current number of threads in the thread pool
int toku_thread_pool_get_current_threads(struct toku_thread_pool *pool);

// Get one or more threads from the thread pool
// dowait indicates whether or not the caller blocks waiting for threads to free up
// nthreads on input determines the number of threads that are wanted
// nthreads on output indicates the number of threads that were allocated
// toku_thread_return on input supplies an array of thread pointers (all NULL).  This function returns the threads
// that were allocated in the array.
int toku_thread_pool_get(struct toku_thread_pool *pool, int dowait, int *nthreads, struct toku_thread **toku_thread_return);

// Run a function f on one or more threads allocated from the thread pool
int toku_thread_pool_run(struct toku_thread_pool *pool, int dowait, int *nthreads, void *(*f)(void *arg), void *arg);

// Print the state of the thread pool
void toku_thread_pool_print(struct toku_thread_pool *pool, FILE *out);
