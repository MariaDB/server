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
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

pthread_rwlock_t rwlock;
volatile int killed = 0;

static void *t1_func(void *arg) {
    int i;
    for (i = 0; !killed; i++) {
        int r;
        r = pthread_rwlock_wrlock(&rwlock); 
        assert(r == 0);
        usleep(10000);
        r = pthread_rwlock_unlock(&rwlock);
        assert(r == 0);
    }
    printf("%lu %d\n", (unsigned long) pthread_self(), i);
    return arg;
}

int main(void) {
    int r;
#if 0
    rwlock = PTHREAD_RWLOCK_INITIALIZER;
#endif
#if 0
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    r = pthread_rwlock_init(&rwlock, &attr);
#endif
#if 0
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    r = pthread_rwlock_init(&rwlock, &attr);
#endif
#if 1
    r = pthread_rwlock_init(&rwlock, NULL);
    assert(r == 0);
#endif
    
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
    return 0;
}
