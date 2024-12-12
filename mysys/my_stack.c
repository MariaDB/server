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
  size_t stack_size;
#if defined(HAVE_PTHREAD_GETATTR_NP) && !defined(_AIX)
  /* POSIX-compliant system (Linux, macOS, etc.) */
  pthread_attr_t attr;
  pthread_t thread= pthread_self();

  /* Get the thread attributes */
  if (pthread_getattr_np(thread, &attr) == 0)
  {
    /* Get stack base and size */
    void *low_addr, *high_addr= NULL;
    if (pthread_attr_getstack(&attr, &low_addr, &stack_size) == 0)
    {
      high_addr= (char *) low_addr + stack_size;
#if STACK_DIRECTION < 0
      *stack_start= high_addr;
      *stack_end= low_addr;
#else
      *stack_start= low_addr;
      *stack_end= high_addr;
#endif
    }
    pthread_attr_destroy(&attr); /* Clean up */
    if (high_addr)
      return;
  }
#endif
  /* Platform does not have pthread_getattr_np, or fallback */
  *stack_start= my_get_stack_pointer(fallback_stack_start);
  stack_size= (fallback_stack_size -
               MY_MIN(fallback_stack_size, MY_STACK_SAFE_MARGIN));
  *stack_end= (char *)(*stack_start) + stack_size * STACK_DIRECTION;
}
