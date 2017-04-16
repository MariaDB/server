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

#include <portability/toku_config.h>
#include <stdio.h>
#include <toku_stdint.h>
#include <unistd.h>
#include <toku_assert.h>
#include "toku_os.h"
#if defined(HAVE_SYSCALL_H)
# include <syscall.h>
#endif
#if defined(HAVE_SYS_SYSCALL_H)
# include <sys/syscall.h>
#endif
#if defined(HAVE_PTHREAD_NP_H)
# include <pthread_np.h>
#endif
#if defined(HAVE_PTHREAD_H)
# include <pthread.h>
#endif

// since we implement the same thing here as in toku_os_gettid, this test
// is pretty pointless
static int gettid(void) {
#if defined(HAVE_PTHREAD_THREADID_NP)
    uint64_t result;
    pthread_threadid_np(NULL, &result);
    return (int) result;
#elif defined(__NR_gettid)
    return syscall(__NR_gettid);
#elif defined(SYS_gettid)
    return syscall(SYS_gettid);
#elif defined(HAVE_PTHREAD_GETTHREADID_NP)
    return pthread_getthreadid_np();
#else
# error "no implementation of gettid available"
#endif
}

int main(void) {
    assert(toku_os_getpid() == getpid());
    assert(toku_os_gettid() == gettid());
    return 0;
}
