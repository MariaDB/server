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

// The abstraction:
//
// queue.h implements a queue suitable for a producer-consumer relationship between two pthreads.
// The enqueue/dequeue operation is fairly heavyweight (involving pthread condition variables) so it may be useful
// to enqueue large chunks rather than small chunks.
// It probably won't work right to have two consumer threads.
//
// Every item inserted into the queue has a weight.  If the weight
// gets too big, then the queue blocks on trying to insert more items.
// The weight can be used to limit the total number of items in the
// queue (weight of each item=1) or the total memory consumed by queue
// items (weight of each item is its size).  Or the weight's could all be
// zero for an unlimited queue.

typedef struct queue *QUEUE;

int toku_queue_create (QUEUE *q, uint64_t weight_limit);
// Effect: Create a queue with a given weight limit.  The queue is initially empty.

int toku_queue_enq (QUEUE q, void *item, uint64_t weight, uint64_t *total_weight_after_enq);
// Effect: Insert ITEM of weight WEIGHT into queue.  If the resulting contents weight too much then block (don't return) until the total weight is low enough.
// If total_weight_after_enq!=NULL then return the current weight of the items in the queue (after finishing blocking on overweight, and after enqueueing the item).
// If successful return 0.
// If an error occurs, return the error number, and the state of the queue is undefined.  The item may have been enqueued or not, and in fact the queue may be badly corrupted if the condition variables go awry.  If it's just a matter of out-of-memory, then the queue is probably OK.
// Requires: There is only a single consumer. (We wake up the consumer using a pthread_cond_signal (which is suitable only for single consumers.)

int toku_queue_eof (QUEUE q);
// Effect: Inform the queue that no more values will be inserted.  After all the values that have been inserted are dequeued, further dequeue operations will return EOF.
// Returns 0 on success.   On failure, things are pretty bad (likely to be some sort of mutex failure).

int toku_queue_deq (QUEUE q, void **item, uint64_t *weight, uint64_t *total_weight_after_deq);
// Effect: Wait until the queue becomes nonempty.  Then dequeue and return the oldest item.  The item and its weight are returned in *ITEM.
// If weight!=NULL then return the item's weight in *weight.
// If total_weight_after_deq!=NULL then return the current weight of the items in the queue (after dequeuing the item).
// Return 0 if an item is returned.
// Return EOF is we no more items will be returned.
// Usage note: The queue should be destroyed only after any consumers will no longer look at it (for example, they saw EOF).

int toku_queue_destroy (QUEUE q);
// Effect: Destroy the queue.
// Requires: The queue must be empty and no consumer should try to dequeue after this (one way to do this is to make sure the consumer saw EOF).
// Returns 0 on success.   If the queue is not empty, returns EINVAL.  Other errors are likely to be bad (some sort of mutex or condvar failure).

