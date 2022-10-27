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

#include <portability/toku_atomic.h>

static toku_mutex_t event_mutex = TOKU_MUTEX_INITIALIZER;
static void lock_events(void) {
    toku_mutex_lock(&event_mutex);
}
static void unlock_events(void) {
    toku_mutex_unlock(&event_mutex);
}
static int event_count, event_count_trigger;

__attribute__((__unused__))
static void reset_event_counts(void) {
    lock_events();
    event_count = event_count_trigger = 0;
    unlock_events();
}

__attribute__((__unused__))
static void event_hit(void) {
}

__attribute__((__unused__))
static int event_add_and_fetch(void) {
    lock_events();
    int r = ++event_count;
    unlock_events();
    return r;
}

static int do_user_errors = 0;

__attribute__((__unused__))
static int loader_poll_callback(void *UU(extra), float UU(progress)) {
    int r;
    if (do_user_errors && event_count_trigger == event_add_and_fetch()) {
        event_hit();
        r = 1;
    } else {
        r = 0;
    }
    return r;
}

static int do_write_errors = 0;

__attribute__((__unused__))
static size_t bad_fwrite (const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t r;
    if (do_write_errors && event_count_trigger == event_add_and_fetch()) {
        event_hit();
	errno = ENOSPC;
	r = (size_t) -1;
    } else {
	r = fwrite(ptr, size, nmemb, stream);
	if (r!=nmemb) {
	    errno = ferror(stream);
	}
    }
    return r;
}

__attribute__((__unused__))
static ssize_t bad_write(int fd, const void * bp, size_t len) {
    ssize_t r;
    if (do_write_errors && event_count_trigger == event_add_and_fetch()) {
        event_hit();
	errno = ENOSPC;
	r = -1;
    } else {
	r = write(fd, bp, len);
    }
    return r;
}

__attribute__((__unused__))
static ssize_t bad_pwrite(int fd, const void * bp, size_t len, toku_off_t off) {
    ssize_t r;
    if (do_write_errors && event_count_trigger == event_add_and_fetch()) {
        event_hit();
	errno = ENOSPC;
	r = -1;
    } else {
	r = pwrite(fd, bp, len, off);
    }
    return r;
}

static int do_malloc_errors = 0;
static int my_malloc_count = 0, my_big_malloc_count = 0;
static int my_realloc_count = 0, my_big_realloc_count = 0;
static size_t my_big_malloc_limit = 64*1024;
   
__attribute__((__unused__))
static void reset_my_malloc_counts(void) {
    my_malloc_count = my_big_malloc_count = 0;
    my_realloc_count = my_big_realloc_count = 0;
}

__attribute__((__unused__))
static void *my_malloc(size_t n) {
    (void) toku_sync_fetch_and_add(&my_malloc_count, 1); // my_malloc_count++;
    if (n >= my_big_malloc_limit) {
        (void) toku_sync_fetch_and_add(&my_big_malloc_count, 1); // my_big_malloc_count++;
        if (do_malloc_errors) {
            if (event_add_and_fetch() == event_count_trigger) {
                event_hit();
                errno = ENOMEM;
                return NULL;
            }
        }
    }
    return malloc(n);
}

static int do_realloc_errors = 0;

__attribute__((__unused__))
static void *my_realloc(void *p, size_t n) {
    (void) toku_sync_fetch_and_add(&my_realloc_count, 1); // my_realloc_count++;
    if (n >= my_big_malloc_limit) {
        (void) toku_sync_fetch_and_add(&my_big_realloc_count, 1); // my_big_realloc_count++;
        if (do_realloc_errors) {
            if (event_add_and_fetch() == event_count_trigger) {
                event_hit();
                errno = ENOMEM;
                return NULL;
            }
        }
    }
    return realloc(p, n);
}
