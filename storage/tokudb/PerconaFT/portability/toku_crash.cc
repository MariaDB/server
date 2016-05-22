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

#include <unistd.h>
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <sys/wait.h>
#include <toku_race_tools.h>
#include "toku_crash.h"
#include "toku_atomic.h"

enum { MAX_GDB_ARGS = 128 };

static void
run_gdb(pid_t parent_pid, const char *gdb_path) {
    // 3 bytes per intbyte, null byte
    char pid_buf[sizeof(pid_t) * 3 + 1];
    char exe_buf[sizeof(pid_buf) + sizeof("/proc//exe")];

    // Get pid and path to executable.
    int n;
    n = snprintf(pid_buf, sizeof(pid_buf), "%d", parent_pid);
    invariant(n >= 0 && n < (int)sizeof(pid_buf));
    n = snprintf(exe_buf, sizeof(exe_buf), "/proc/%d/exe", parent_pid);
    invariant(n >= 0 && n < (int)sizeof(exe_buf));

    dup2(2, 1); // redirect output to stderr
    // Arguments are not dynamic due to possible security holes.
    execlp(gdb_path, gdb_path, "--batch", "-n",
           "-ex", "thread",
           "-ex", "bt",
           "-ex", "bt full",
           "-ex", "thread apply all bt",
           "-ex", "thread apply all bt full",
           exe_buf, pid_buf,
           NULL);
}

static void
intermediate_process(pid_t parent_pid, const char *gdb_path) {
    // Disable generating of core dumps
#if defined(HAVE_SYS_PRCTL_H)
    prctl(PR_SET_DUMPABLE, 0, 0, 0);
#endif
    pid_t worker_pid = fork();
    if (worker_pid < 0) {
        perror("spawn gdb fork: ");
        goto failure;
    }
    if (worker_pid == 0) {
        // Child (debugger)
        run_gdb(parent_pid, gdb_path);
        // Normally run_gdb will not return.
        // In case it does, kill the process.
        goto failure;
    } else {
        pid_t timeout_pid = fork();
        if (timeout_pid < 0) {
            perror("spawn timeout fork: ");
            kill(worker_pid, SIGKILL);
            goto failure;
        }

        if (timeout_pid == 0) {
            sleep(5);  // Timeout of 5 seconds
            goto success;
        } else {
            pid_t exited_pid = wait(NULL);  // Wait for first child to exit
            if (exited_pid == worker_pid) {
                // Kill slower child
                kill(timeout_pid, SIGKILL);
                goto success;
            } else if (exited_pid == timeout_pid) {
                // Kill slower child
                kill(worker_pid, SIGKILL);
                goto failure;  // Timed out.
            } else {
                perror("error while waiting for gdb or timer to end: ");
                //Some failure.  Kill everything.
                kill(timeout_pid, SIGKILL);
                kill(worker_pid, SIGKILL);
                goto failure;
            }
        }
    }
success:
    _exit(EXIT_SUCCESS);
failure:
    _exit(EXIT_FAILURE);
}

static void
spawn_gdb(const char *gdb_path) {
    pid_t parent_pid = getpid();
#if defined(HAVE_SYS_PRCTL_H)
    // On systems that require permission for the same user to ptrace,
    // give permission for this process and (more importantly) all its children to debug this process.
    prctl(PR_SET_PTRACER, parent_pid, 0, 0, 0);
#endif
    fprintf(stderr, "Attempting to use gdb @[%s] on pid[%d]\n", gdb_path, parent_pid);
    fflush(stderr);
    int intermediate_pid = fork();
    if (intermediate_pid < 0) {
        perror("spawn_gdb intermediate process fork: ");
    } else if (intermediate_pid == 0) {
        intermediate_process(parent_pid, gdb_path);
    } else {
        waitpid(intermediate_pid, NULL, 0);
    }
}

void
toku_try_gdb_stack_trace(const char *gdb_path) {
    char default_gdb_path[] = "/usr/bin/gdb";
    static bool started = false;
    if (RUNNING_ON_VALGRIND) {
        fprintf(stderr, "gdb stack trace skipped due to running under valgrind\n");
        fflush(stderr);
    } else if (toku_sync_bool_compare_and_swap(&started, false, true)) {
        spawn_gdb(gdb_path ? gdb_path : default_gdb_path);
    }
}

