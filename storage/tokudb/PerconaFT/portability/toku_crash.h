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
#include <stdlib.h>
#include <signal.h>
#include "toku_assert.h"

//Simulate as hard a crash as possible.
//Choices:
//  raise(SIGABRT)
//  kill -SIGKILL $pid
//  divide by 0
//  null dereference
//  abort()
//  assert(false) (from <assert.h>)
//  assert(false) (from <toku_assert.h>)
//
//Linux:
//  abort() and both assert(false) cause FILE buffers to be flushed and written to disk: Unacceptable
//
//kill -SIGKILL $pid is annoying (and so far untested)
//
//raise(SIGABRT) has the downside that perhaps it could be caught?
//I'm choosing raise(SIGABRT), followed by divide by 0, followed by null dereference, followed by all the others just in case one gets caught.
static void __attribute__((unused, noreturn))
toku_hard_crash_on_purpose(void) {
    raise(SIGKILL); //Does not flush buffers on linux; cannot be caught.
    {
        int zero = 0;
        int infinity = 1/zero;
        fprintf(stderr, "Force use of %d\n", infinity);
        fflush(stderr); //Make certain the string is calculated.
    }
    {
        void * intothevoid = NULL;
        (*(int*)intothevoid)++;
        fprintf(stderr, "Force use of *(%p) = %d\n", intothevoid, *(int*)intothevoid);
        fflush(stderr);
    }
    abort();
    fprintf(stderr, "This line should never be printed\n");
    fflush(stderr);
}

// Similar to toku_hard_crash_on_purpose, but the goal isn't to crash hard, the primary goal is to get a corefile, the secondary goal is to terminate in any way possible.
// We don't really care if buffers get flushed etc, in fact they may as well flush since there may be useful output in stdout or stderr.
//
// By default, the following signals generate cores:
//  Linux, from signal(7):
//     SIGQUIT       3       Core
//     SIGILL        4       Core
//     SIGABRT       6       Core
//     SIGFPE        8       Core
//     SIGSEGV      11       Core
//
//  Darwin and FreeBSD, from signal(3):
//     3     SIGQUIT      create core image
//     4     SIGILL       create core image
//     5     SIGTRAP      create core image
//     6     SIGABRT      create core image
//     7     SIGEMT       create core image
//     8     SIGFPE       create core image
//     10    SIGBUS       create core image
//     11    SIGSEGV      create core image
//     12    SIGSYS       create core image
//
// We'll raise these in some sequence (common ones first), then try emulating the things that would cause these signals to be raised, then eventually just try to die normally and then loop like abort does.
// Start with a toku assert because that hopefully prints a stacktrace.
static void __attribute__((unused, noreturn))
toku_crash_and_dump_core_on_purpose(void) {
    assert(false);
    invariant(0);
    raise(SIGQUIT);
    raise(SIGILL);
    raise(SIGABRT);
    raise(SIGFPE);
    raise(SIGSEGV);
#if defined(__FreeBSD__) || defined(__APPLE__)
    raise(SIGTRAP);
    raise(SIGEMT);
    raise(SIGBUS);
    raise(SIGSYS);
#endif
    abort();
    {
        int zero = 0;
        int infinity = 1/zero;
        fprintf(stderr, "Force use of %d\n", infinity);
        fflush(stderr); //Make certain the string is calculated.
    }
    {
        void * intothevoid = NULL;
        (*(int*)intothevoid)++;
        fprintf(stderr, "Force use of *(%p) = %d\n", intothevoid, *(int*)intothevoid);
        fflush(stderr);
    }
    raise(SIGKILL);
    while (true) {
        // don't return
    }
}

void toku_try_gdb_stack_trace(const char *gdb_path);
