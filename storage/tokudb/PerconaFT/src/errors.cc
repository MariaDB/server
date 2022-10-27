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

/**
  \file errors.c
  \brief Error handling
 
  The error handling routines for ydb
*/

#include <toku_portability.h>
#include <stdio.h>
#include <stdarg.h>

#include "ydb-internal.h"

/** Checks whether the environment has panicked */
int toku_env_is_panicked(DB_ENV *dbenv /**< The environment to check */) {
    if (dbenv==0) return 0;
    return dbenv->i->is_panicked;
}

/* Prints an error message to a file specified by env (or stderr),
   preceded by the environment's error prefix. */
static void toku__ydb_error_file(const DB_ENV *env, bool use_stderr, 
                                  char errmsg[]) {
    /* Determine the error file to use */
    FILE *CAST_FROM_VOIDP(efile, env->i->errfile);
    if (efile==NULL && env->i->errcall==0 && use_stderr) efile = stderr;

    /* Print out on a file */
    if (efile) {
        if (env->i->errpfx) fprintf(efile, "%s: ", env->i->errpfx);
	fprintf(efile, "%s", errmsg);
    }
}

/**  

     Prints out environment errors, adjusting to a variety of options 
     and formats. 
     The printout format can be controlled to print the following optional 
     messages:
     - The environment error message prefix
     - User-supplied prefix obtained by printing ap with the
       fmt string
     - The standard db error string
     The print out takes place via errcall (if set), errfile (if set),
     or stderr if neither is set (and the user so toggles the printout).
     Both errcall and errfile can be set.
     The error message is truncated to approximately 4,000 characters.

     \param env   The environment that the error refers to. 
     \param error The error code
     \param include_stderrstring Controls whether the standard db error 
                  string should be included in the print out
     \param use_stderr_if_nothing_else Toggles the use of stderr.
     \param fmt   Output format for optional prefix arguments (must be NULL
                  if the prefix is empty)
     \param ap    Optional prefix
*/
void toku_ydb_error_all_cases(const DB_ENV * env, 
                              int error, 
                              bool include_stderrstring, 
                              bool use_stderr_if_nothing_else, 
                              const char *fmt, va_list ap) {
    /* Construct the error message */
    char buf [4000];
    int count=0;
    if (fmt) count=vsnprintf(buf, sizeof(buf), fmt, ap);
    if (include_stderrstring) {
        count+=snprintf(&buf[count], sizeof(buf)-count, ": %s", 
                        db_strerror(error));
    }

    /* Print via errcall */
    if (env->i->errcall) env->i->errcall(env, env->i->errpfx, buf);

    /* Print out on a file */
    toku__ydb_error_file(env, use_stderr_if_nothing_else, buf);
}

/** Handle all the error cases (but don't do the default thing.) 
    \param dbenv  The environment that is subject to errors
    \param error  The error code
    \param fmt    The format string for additional variable arguments to
                  be printed   */
int toku_ydb_do_error (const DB_ENV *dbenv, int error, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    toku_ydb_error_all_cases(dbenv, error, false, false, fmt, ap);
    va_end(ap);
    return error;
}

/** Handle errors on an environment, 
    \param dbenv  The environment that is subject to errors
    \param error  The error code
    \param fmt    The format string for additional variable arguments to
                  be printed   */
void toku_env_err(const DB_ENV * env, int error, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    toku_ydb_error_all_cases(env, error, false, true, fmt, ap);
    va_end(ap);
}
