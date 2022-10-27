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

#include <toku_portability.h>
#include "toku_assert.h"

#include <stdlib.h>
#include <stdio.h>
#if defined(HAVE_MALLOC_H)
# include <malloc.h>
#elif defined(HAVE_SYS_MALLOC_H)
# include <sys/malloc.h>
#endif
#include <dlfcn.h>
#include <execinfo.h>

// These are statically allocated so that the backtrace can run without any calls to malloc()
#define N_POINTERS 1000
static void *backtrace_pointers[N_POINTERS];

static uint64_t engine_status_num_rows = 0;

typedef void (*malloc_stats_fun_t)(void);
static malloc_stats_fun_t malloc_stats_f;

void
toku_assert_init(void)
{
    malloc_stats_f = (malloc_stats_fun_t) dlsym(RTLD_DEFAULT, "malloc_stats");
}

// Function pointers are zero by default so asserts can be used by ft-layer tests without an environment.
static int (*toku_maybe_get_engine_status_text_p)(char* buff, int buffsize) = 0;
static int (*toku_maybe_err_engine_status_p)(void) = 0;
static void (*toku_maybe_set_env_panic_p)(int code, const char* msg) = 0;

void toku_assert_set_fpointers(int (*toku_maybe_get_engine_status_text_pointer)(char*, int),
                               int (*toku_maybe_err_engine_status_pointer)(void),
			       void (*toku_maybe_set_env_panic_pointer)(int, const char*),
                               uint64_t num_rows) {
    toku_maybe_get_engine_status_text_p = toku_maybe_get_engine_status_text_pointer;
    toku_maybe_err_engine_status_p = toku_maybe_err_engine_status_pointer;
    toku_maybe_set_env_panic_p = toku_maybe_set_env_panic_pointer;
    engine_status_num_rows = num_rows;
}

bool toku_gdb_dump_on_assert = false;
void (*do_assert_hook)(void) = NULL;

void db_env_do_backtrace_errfunc(toku_env_err_func errfunc, const void *env) {
    // backtrace
    int n = backtrace(backtrace_pointers, N_POINTERS);
    errfunc(env, 0, "Backtrace: (Note: toku_do_assert=0x%p)\n", toku_do_assert);
    char **syms = backtrace_symbols(backtrace_pointers, n);
    if (syms) {
        for (char **symstr = syms; symstr != NULL && (symstr - syms) < n; ++symstr) {
            errfunc(env, 0, *symstr);
        }
        free(syms);
    }

    if (engine_status_num_rows && toku_maybe_err_engine_status_p) {
	toku_maybe_err_engine_status_p();
    } else {
	errfunc(env, 0, "Engine status function not available\n");
    }
    errfunc(env, 0, "Memory usage:\n");
    if (malloc_stats_f) {
        malloc_stats_f();
    }

    if (do_assert_hook) do_assert_hook();
    if (toku_gdb_dump_on_assert) {
        toku_try_gdb_stack_trace(nullptr);
    }
}

void db_env_do_backtrace(FILE *outf) {
    // backtrace
    int n = backtrace(backtrace_pointers, N_POINTERS);
    fprintf(outf, "Backtrace: (Note: toku_do_assert=0x%p)\n", toku_do_assert); fflush(outf);
    backtrace_symbols_fd(backtrace_pointers, n, fileno(outf));

    fflush(outf);
    
    if (engine_status_num_rows && toku_maybe_get_engine_status_text_p) {
	int buffsize = engine_status_num_rows * 128;  // assume 128 characters per row (gross overestimate, should be safe)
	char buff[buffsize];	
	toku_maybe_get_engine_status_text_p(buff, buffsize);  
	fprintf(outf, "Engine status:\n%s\n", buff);
    } else {
	fprintf(outf, "Engine status function not available\n");
    }
    fprintf(outf, "Memory usage:\n");
    fflush(outf);	    // just in case malloc_stats() crashes, we still want engine status (and to know that malloc_stats() failed)
    if (malloc_stats_f) {
        malloc_stats_f();
    }
    fflush(outf);

    if (do_assert_hook) do_assert_hook();
    if (toku_gdb_dump_on_assert) {
        toku_try_gdb_stack_trace(nullptr);
    }
}

__attribute__((noreturn))
static void toku_do_backtrace_abort(void) {
    db_env_do_backtrace(stderr);
    abort();
}


static void
set_panic_if_not_panicked(int caller_errno, char * msg) {
    int code = caller_errno ? caller_errno : -1;
    if (toku_maybe_set_env_panic_p) {
	toku_maybe_set_env_panic_p(code, msg);
    }
}


#define MSGLEN 1024

void 
toku_do_assert_fail (const char *expr_as_string, const char *function, const char *file, int line, int caller_errno) {
    char msg[MSGLEN];
    snprintf(msg, MSGLEN, "%s:%d %s: Assertion `%s' failed (errno=%d)\n", file, line, function, expr_as_string, caller_errno);
    perror(msg);
    set_panic_if_not_panicked(caller_errno, msg);
    toku_do_backtrace_abort();
}

void 
toku_do_assert_zero_fail (uintptr_t expr, const char *expr_as_string, const char *function, const char *file, int line, int caller_errno) {
    char msg[MSGLEN];
    snprintf(msg, MSGLEN, "%s:%d %s: Assertion `%s == 0' failed (errno=%d) (%s=%" PRIuPTR ")\n", file, line, function, expr_as_string, caller_errno, expr_as_string, expr);
    perror(msg);
    set_panic_if_not_panicked(caller_errno, msg);
    toku_do_backtrace_abort();
}

void
toku_do_assert_expected_fail (uintptr_t expr, uintptr_t expected, const char *expr_as_string, const char *function, const char *file, int line, int caller_errno) {
    char msg[MSGLEN];
    snprintf(msg, MSGLEN, "%s:%d %s: Assertion `%s == %" PRIuPTR "' failed (errno=%d) (%s=%" PRIuPTR ")\n", file, line, function, expr_as_string, expected, caller_errno, expr_as_string, expr);
    perror(msg);
    set_panic_if_not_panicked(caller_errno, msg);
    toku_do_backtrace_abort();
}

void 
toku_do_assert(int expr, const char *expr_as_string, const char *function, const char* file, int line, int caller_errno) {
    if (expr == 0)
        toku_do_assert_fail(expr_as_string, function, file, line, caller_errno);
}

