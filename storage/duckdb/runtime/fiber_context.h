/*
  Copyright 2011 Kristian Nielsen and Monty Program Ab
            2015, 2022 MariaDB Corporation AB

  This file is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  Fiber context for DuckDB cross-engine scan.

  Copied from libmariadb/include/ma_context.h and
  libmariadb/libmariadb/ma_context.c with libmariadb-specific
  dependencies removed (mysql_async_context, ma_global.h, DBUG,
  boost::context).  Only the core co-routine primitives are kept.
*/

#ifndef FIBER_CONTEXT_H
#define FIBER_CONTEXT_H

#include <stdint.h>
#include <stddef.h>

/*
  Platform selection — same priority order as ma_context.h but without
  boost::context and ASAN overrides (those need extra source files).
*/
#ifdef _WIN32
#define FIBER_USE_WIN32_FIBERS 1
#elif defined(__GNUC__) && __GNUC__ >= 3 && defined(__x86_64__) && !defined(__ILP32__)
#define FIBER_USE_X86_64_GCC_ASM
#elif defined(__GNUC__) && __GNUC__ >= 3 && defined(__i386__)
#define FIBER_USE_I386_GCC_ASM
#elif defined(__GNUC__) && __GNUC__ >= 3 && defined(__aarch64__)
#define FIBER_USE_AARCH64_GCC_ASM
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#if defined(_POSIX_VERSION)
#define FIBER_USE_UCONTEXT
#endif
#endif

#if !defined(FIBER_USE_WIN32_FIBERS) && \
    !defined(FIBER_USE_X86_64_GCC_ASM) && \
    !defined(FIBER_USE_I386_GCC_ASM) && \
    !defined(FIBER_USE_AARCH64_GCC_ASM) && \
    !defined(FIBER_USE_UCONTEXT)
#define FIBER_CONTEXT_DISABLE
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef FIBER_USE_WIN32_FIBERS
struct fiber_context {
  void (*user_func)(void *);
  void *user_arg;
  void *app_fiber;
  void *lib_fiber;
  int return_value;
};
#endif

#ifdef FIBER_USE_UCONTEXT
#include <ucontext.h>

struct fiber_context {
  void (*user_func)(void *);
  void *user_data;
  void *stack;
  size_t stack_size;
  ucontext_t base_context;
  ucontext_t spawned_context;
  int active;
};
#endif

#ifdef FIBER_USE_X86_64_GCC_ASM
struct fiber_context {
  uint64_t save[9];
  void *stack_top;
  void *stack_bot;
  int active;
};
#endif

#ifdef FIBER_USE_I386_GCC_ASM
struct fiber_context {
  uint64_t save[7];
  void *stack_top;
  void *stack_bot;
  int active;
};
#endif

#ifdef FIBER_USE_AARCH64_GCC_ASM
struct fiber_context {
  uint64_t save[22];
  void *stack_top;
  void *stack_bot;
  int active;
};
#endif

#ifdef FIBER_CONTEXT_DISABLE
struct fiber_context {
  int dummy;
};
#endif

/*
  Initialize a fiber context object.
  Returns 0 on success, non-zero on failure.
*/
extern int fiber_context_init(struct fiber_context *c, size_t stack_size);

/* Free a fiber context object, deallocating any resources used. */
extern void fiber_context_destroy(struct fiber_context *c);

/*
  Spawn a fiber context.  The fiber will run the supplied user function,
  passing the supplied user data pointer.

  The user function may call fiber_context_yield(), which will cause this
  function to return 1.  Then later fiber_context_continue() may be called,
  which will resume the fiber by returning from the previous
  fiber_context_yield() call.

  When the user function returns, this function returns 0.
  In case of error, -1 is returned.
*/
extern int fiber_context_spawn(struct fiber_context *c,
                               void (*f)(void *), void *d);

/*
  Suspend a fiber started with fiber_context_spawn.

  When fiber_context_yield() is called, execution immediately returns from
  the last fiber_context_spawn() or fiber_context_continue() call.  Then
  when later fiber_context_continue() is called, execution resumes by
  returning from this fiber_context_yield() call.

  Returns 0 if ok, -1 in case of error.
*/
extern int fiber_context_yield(struct fiber_context *c);

/*
  Resume a suspended fiber.

  Each time it is suspended, this function returns 1.  When the originally
  spawned user function returns, this function returns 0.
  In case of error, -1 is returned.
*/
extern int fiber_context_continue(struct fiber_context *c);

#ifdef __cplusplus
}
#endif

#endif /* FIBER_CONTEXT_H */
