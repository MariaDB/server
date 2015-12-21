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

#include <memory.h>

#include <portability/toku_config.h>
#include <portability/toku_time.h>
#include <toku_pthread.h>

#include "kibbutz.h"

// A Kibbutz is a collection of workers and some work to do.
struct todo {
    void (*f)(void *extra);
    void *extra;
    struct todo *next;
    struct todo *prev;
};

struct kid {
    struct kibbutz *k;
};

struct kibbutz {
    toku_mutex_t mutex;
    toku_cond_t  cond;
    bool please_shutdown;
    struct todo *head, *tail; // head is the next thing to do.
    int n_workers;
    pthread_t *workers; // an array of n_workers
    struct kid *ids;    // pass this in when creating a worker so it knows who it is.

    uint64_t threads_active;
    uint64_t queue_size;
    uint64_t max_queue_size;
    uint64_t total_items_processed;
    uint64_t total_execution_time;
};

static void *work_on_kibbutz (void *);

int toku_kibbutz_create (int n_workers, KIBBUTZ *kb_ret) {
    int r = 0;
    *kb_ret = NULL;
    KIBBUTZ XCALLOC(k);
    toku_mutex_init(&k->mutex, NULL);
    toku_cond_init(&k->cond, NULL);
    k->please_shutdown = false;
    k->head = NULL;
    k->tail = NULL;
    k->n_workers = n_workers;
    k->threads_active = 0;
    k->queue_size = 0;
    k->max_queue_size = 0;
    k->total_items_processed = 0;
    k->total_execution_time = 0;
    XMALLOC_N(n_workers, k->workers);
    XMALLOC_N(n_workers, k->ids);
    for (int i = 0; i < n_workers; i++) {
        k->ids[i].k = k;
        r = toku_pthread_create(&k->workers[i], NULL, work_on_kibbutz, &k->ids[i]);
        if (r != 0) {
            k->n_workers = i;
            toku_kibbutz_destroy(k);
            break;
        }
    }
    if (r == 0) {
        *kb_ret = k;
    }
    return r;
}

static void klock (KIBBUTZ k) {
    toku_mutex_lock(&k->mutex);
}
static void kunlock (KIBBUTZ k) {
    toku_mutex_unlock(&k->mutex);
}
static void kwait (KIBBUTZ k) {
    toku_cond_wait(&k->cond, &k->mutex);
}
static void ksignal (KIBBUTZ k) {
    toku_cond_signal(&k->cond);
}

//
// pops the tail of the kibbutz off the list and works on it
// Note that in toku_kibbutz_enq, items are enqueued at the head,
// making the work be done in FIFO order. This is necessary
// to avoid deadlocks in flusher threads.
//
static void *work_on_kibbutz (void *kidv) {
    struct kid *CAST_FROM_VOIDP(kid, kidv);
    KIBBUTZ k = kid->k;
    klock(k);
    while (1) {
        while (k->tail) {
            struct todo *item = k->tail;
            k->tail = item->prev;
            toku_sync_sub_and_fetch(&k->queue_size, 1);
            if (k->tail==NULL) {
                k->head=NULL;
            } else {
                // if there are other things to do, then wake up the next guy, if there is one.
                ksignal(k);
            }
            kunlock(k);
            toku_sync_add_and_fetch(&k->threads_active, 1);
            uint64_t starttime = toku_current_time_microsec();
            item->f(item->extra);
            uint64_t duration = toku_current_time_microsec() - starttime;
            toku_sync_add_and_fetch(&k->total_execution_time, duration);
            toku_sync_add_and_fetch(&k->total_items_processed, 1);
            toku_sync_sub_and_fetch(&k->threads_active, 1);
            toku_free(item);
            klock(k);
            // if there's another item on k->head, then we'll just go grab it now, without waiting for a signal.
        }
        if (k->please_shutdown) {
            // Don't follow this unless the work is all done, so that when we set please_shutdown, all the work finishes before any threads quit.
            ksignal(k); // must wake up anyone else who is waiting, so they can shut down.
            kunlock(k);
            return NULL;
        }
        // There is no work to do and it's not time to shutdown, so wait.
        kwait(k);
    }
}

//
// adds work to the head of the kibbutz 
// Note that in work_on_kibbutz, items are popped off the tail for work,
// making the work be done in FIFO order. This is necessary
// to avoid deadlocks in flusher threads.
//
void toku_kibbutz_enq (KIBBUTZ k, void (*f)(void*), void *extra) {
    struct todo *XMALLOC(td);
    td->f = f;
    td->extra = extra;
    klock(k);
    assert(!k->please_shutdown);
    td->next = k->head;
    td->prev = NULL;
    if (k->head) {
        assert(k->head->prev == NULL);
        k->head->prev = td;
    }
    k->head = td;
    if (k->tail==NULL) k->tail = td;

    uint64_t newsize = toku_sync_add_and_fetch(&k->queue_size, 1);
    // not exactly precise but we'll live with it
    if (newsize > k->max_queue_size) k->max_queue_size = k->queue_size;

    ksignal(k);
    kunlock(k);
}

void toku_kibbutz_get_status(KIBBUTZ k,
                             uint64_t *num_threads,
                             uint64_t *num_threads_active,
                             uint64_t *queue_size,
                             uint64_t *max_queue_size,
                             uint64_t *total_items_processed,
                             uint64_t *total_execution_time) {
    *num_threads = k->n_workers;
    *num_threads_active = k->threads_active;
    *queue_size = k->queue_size;
    *max_queue_size = k->max_queue_size;
    *total_items_processed = k->total_items_processed;
    *total_execution_time = k->total_execution_time / 1000; // return in ms.
}

void toku_kibbutz_destroy (KIBBUTZ k)
// Effect: wait for all the enqueued work to finish, and then destroy the kibbutz.
//  Note: It is an error for to perform kibbutz_enq operations after this is called.
{
    klock(k);
    assert(!k->please_shutdown);
    k->please_shutdown = true;
    ksignal(k); // must wake everyone up to tell them to shutdown.
    kunlock(k);
    for (int i=0; i<k->n_workers; i++) {
        void *result;
        int r = toku_pthread_join(k->workers[i], &result);
        assert(r==0);
        assert(result==NULL);
    }
    toku_free(k->workers);
    toku_free(k->ids);
    toku_cond_destroy(&k->cond);
    toku_mutex_destroy(&k->mutex);
    toku_free(k);
}
