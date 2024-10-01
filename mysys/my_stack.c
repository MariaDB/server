/* Copyright 2024 MariaDB corporation AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/


/*
  Get start and end of stack

  Note that the code depends on STACK_DIRECTION that is defined by cmake.
  In general, STACK_DIRECTION should be 1 except in case of
  an upward-growing stack that can happen on sparc or hpux platforms.
*/


#include "my_global.h"
#include "my_sys.h"
#include "my_stack_alloc.h"

#ifdef _WIN_
#include <windows.h>
#include <processthreadsapi.h>
#include <winnt.h>
#endif

/* Get start and end of stack */

extern void my_get_stack_bounds(void **stack_start, void **stack_end,
                                void *fallback_stack_start,
                                size_t fallback_stack_size)
{
#if defined(__GNUC__) || defined(__clang__) /* GCC or Clang compilers */
  size_t stack_size;
#if defined(HAVE_PTHREAD_GETATTR_NP)
  /* POSIX-compliant system (Linux, macOS, etc.) */
  pthread_attr_t attr;
  pthread_t thread= pthread_self();
  void *stack_base;

  /* Get the thread attributes */
  if (pthread_getattr_np(thread, &attr) == 0)
  {
    /* Get stack base and size */
    pthread_attr_getstack(&attr, &stack_base, &stack_size);
    /*
      stack_base points to start of the stack region to which the
      stack grows to
    */
    *stack_start= stack_base - stack_size * STACK_DIRECTION;
    pthread_attr_destroy(&attr); /* Clean up */
  }
  else
  {
    /*
      Fallback:
      Use the current stack pointer as an approximation of the start
    */
    *stack_start= my_get_stack_pointer(fallback_stack_start);
    stack_size= (fallback_stack_size -
                 MY_MIN(fallback_stack_size, MY_STACK_SAFE_MARGIN));
  }
#else
  /* Platform does not have pthread_getattr_np */
  *stack_start= my_get_stack_pointer(fallback_stack_start);
  stack_size= (fallback_stack_size -
               MY_MIN(fallback_stack_size, MY_STACK_SAFE_MARGIN));
#endif /* defined(HAVE_PTHREAD_GETATTR_NP) */
  *stack_end= *stack_start + stack_size * STACK_DIRECTION;

#elif defined(_MSC_VER) && defined(_WIN32)
  /* Windows platform (MSVC) */
  NT_TIB* teb= (NT_TIB*)NtCurrentTeb();

  *stack_start= teb->StackBase;  /* Start of the stack */
  *stack_end= teb->StackLimit;   /* End of the stack (stack limit) */
#else
  /* Unsupported platform / compiler */
  *stack_start= my_get_stack_pointer(fallback_stack_start);
  *stack_end= (*stack_start +
               (fallback_stack_size -
                MY_MIN(fallback_stack_size, MY_STACK_SAFE_MARGIN)) *
               STACK_DIRECTON);
#endif /* defined(__GNUC__) || defined(__clang__) */
}
