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

// check if write locks are fair

#include <stdio.h>
#include <toku_assert.h>
#include <unistd.h>
#include <pthread.h>
#include <util/frwlock.h>

toku_mutex_t rwlock_mutex;
toku::frwlock rwlock;
volatile int killed = 0;

static void *t1_func(void *arg) {
    int i;
    for (i = 0; !killed; i++) {
        toku_mutex_lock(&rwlock_mutex);
        rwlock.write_lock(false);
        toku_mutex_unlock(&rwlock_mutex);
        usleep(10000);
        toku_mutex_lock(&rwlock_mutex);
        rwlock.write_unlock();
        toku_mutex_unlock(&rwlock_mutex);
    }
    printf("%lu %d\n", (unsigned long) pthread_self(), i);
    return arg;
}

int main(void) {
    int r;

    toku_mutex_init(&rwlock_mutex, NULL);
    rwlock.init(&rwlock_mutex);
    
    const int nthreads = 2;
    pthread_t tids[nthreads];
    for (int i = 0; i < nthreads; i++) {
        r = pthread_create(&tids[i], NULL, t1_func, NULL); 
        assert(r == 0);
    }
    sleep(10);
    killed = 1;
    for (int i = 0; i < nthreads; i++) {
        void *ret;
        r = pthread_join(tids[i], &ret);
        assert(r == 0);
    }

    rwlock.deinit();
    toku_mutex_destroy(&rwlock_mutex);

    return 0;
}
