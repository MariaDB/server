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

/* The problem with assert.h:  If NDEBUG is set then it doesn't execute the function, if NDEBUG isn't set then we get a branch that isn't taken. */

/* This version will complain if NDEBUG is set. */
/* It evaluates the argument and then calls a function  toku_do_assert() which takes all the hits for the branches not taken. */

#include <portability/toku_config.h>

#include <stdint.h>
#include <errno.h>
#include <stdio.h>

#ifdef NDEBUG
#error NDEBUG should not be set
#endif

inline int get_error_errno(void);

static inline int get_maybe_error_errno(void) { return errno; }

static inline void
set_errno(int new_errno)
{
    errno = new_errno;
}

void toku_assert_init(void) __attribute__((constructor));

void toku_assert_set_fpointers(int (*toku_maybe_get_engine_status_text_pointer)(char*, int),
                               int (*toku_maybe_err_engine_status_pointer)(void),
			       void (*toku_maybe_set_env_panic_pointer)(int, const char*),
                               uint64_t num_rows);

void toku_do_assert(int /*expr*/,const char*/*expr_as_string*/,const char */*fun*/,const char*/*file*/,int/*line*/, int/*errno*/) __attribute__((__visibility__("default")));

void toku_do_assert_fail(const char*/*expr_as_string*/,const char */*fun*/,const char*/*file*/,int/*line*/, int/*errno*/) __attribute__((__visibility__("default"))) __attribute__((__noreturn__));
void toku_do_assert_zero_fail(uintptr_t/*expr*/, const char*/*expr_as_string*/,const char */*fun*/,const char*/*file*/,int/*line*/, int/*errno*/) __attribute__((__visibility__("default"))) __attribute__((__noreturn__));
void toku_do_assert_expected_fail(uintptr_t/*expr*/, uintptr_t /*expected*/, const char*/*expr_as_string*/,const char */*fun*/,const char*/*file*/,int/*line*/, int/*errno*/) __attribute__((__visibility__("default"))) __attribute__((__noreturn__));

// Define GCOV if you want to get test-coverage information that ignores the assert statements.
// #define GCOV

extern void (*do_assert_hook)(void); // Set this to a function you want called after printing the assertion failure message but before calling abort().  By default this is NULL.
// copied here from ydb-internal.h to avoid inclusion hell, the void * is really a DB_ENV but we don't have that type here
typedef void (*toku_env_err_func)(const void * env, int error, const char *fmt, ...);
void db_env_do_backtrace_errfunc(toku_env_err_func errfunc, const void *env);
void db_env_do_backtrace(FILE *outf);

#ifdef assert
#  undef assert
#endif
#if defined(GCOV)
#define assert(expr)      toku_do_assert((expr) != 0, #expr, __FUNCTION__, __FILE__, __LINE__, get_maybe_error_errno())
#define assert_zero(expr) toku_do_assert((expr) == 0, #expr, __FUNCTION__, __FILE__, __LINE__, get_maybe_error_errno())
#define assert_equals(expr, expected) toku_do_assert((expr) == (expected), (expected), #expr, __FUNCTION__, __FILE__, __LINE__, get_maybe_error_errno())
#else
#define assert(expr)      ((expr)      ? (void)0 : toku_do_assert_fail(#expr, __FUNCTION__, __FILE__, __LINE__, get_maybe_error_errno()))
#define assert_zero(expr) ((expr) == 0 ? (void)0 : toku_do_assert_zero_fail((uintptr_t)(expr), #expr, __FUNCTION__, __FILE__, __LINE__, get_maybe_error_errno()))
#define assert_equals(expr, expected) ((expr) == (expected) ? (void)0 : toku_do_assert_expected_fail((uintptr_t)(expr), (uintptr_t)(expected), #expr, __FUNCTION__, __FILE__, __LINE__, get_maybe_error_errno()))
#define assert_null(expr) ((expr) == nullptr ? (void)0 : toku_do_assert_zero_fail((uintptr_t)(expr), #expr, __FUNCTION__, __FILE__, __LINE__, get_maybe_error_errno()))
#endif

#ifdef GCOV
#define WHEN_GCOV(x) x
#define WHEN_NOT_GCOV(x)
#else
#define WHEN_GCOV(x)
#define WHEN_NOT_GCOV(x) x
#endif

#if !defined(__clang__)
#include <type_traits>
# define ENSURE_POD(type) static_assert(std::is_pod<type>::value, #type " isn't POD")
#else
# define ENSURE_POD(type) // TEMP, clang is much more strict about POD.
#endif

#define lazy_assert(a)          assert(a)      // indicates code is incomplete 
#define lazy_assert_zero(a)     assert_zero(a) // indicates code is incomplete 
#define lazy_assert_equals(a, b)     assert_equals(a, b) // indicates code is incomplete 
#define invariant(a)            assert(a)      // indicates a code invariant that must be true
#define invariant_null(a)       assert_null(a) // indicates a code invariant that must be true
#define invariant_notnull(a)    assert(a)      // indicates a code invariant that must be true
#define invariant_zero(a)       assert_zero(a) // indicates a code invariant that must be true
#define invariant_equals(a, b)       assert_equals(a, b) // indicates a code invariant that must be true
#define resource_assert(a)      assert(a)      // indicates resource must be available, otherwise unrecoverable
#define resource_assert_zero(a) assert_zero(a) // indicates resource must be available, otherwise unrecoverable
#define resource_assert_equals(a, b) assert_equals(a, b) // indicates resource must be available, otherwise unrecoverable

#if defined(TOKU_DEBUG_PARANOID) && TOKU_DEBUG_PARANOID
#define paranoid_invariant(a) assert(a)
#define paranoid_invariant_null(a) assert_null(a)
#define paranoid_invariant_notnull(a) assert(a)
#define paranoid_invariant_zero(a) assert_zero(a)
#else // !TOKU_DEBUG_PARANOID
#define paranoid_invariant(a) ((void) 0)
#define paranoid_invariant_null(a) ((void) 0)
#define paranoid_invariant_notnull(a) ((void) 0)
#define paranoid_invariant_zero(a) ((void)0)
#endif

inline int get_error_errno(void) {
    invariant(errno);
    return errno;
}

extern bool toku_gdb_dump_on_assert;
