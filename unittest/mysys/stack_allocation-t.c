/* Copyright (c) 2024, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <my_global.h>
#include <my_sys.h>
#include <my_stack_alloc.h>
#include <my_pthread.h>
#include <my_alloca.h>
#include <tap.h>

/*
  Test of stack detection
  The test is run with a stacks of STACK_ALLOC_SMALL_BLOCK_SIZE+1 and
  STACK_ALLOC_SMALL_BLOCK_SIZE-1.
  This is becasue of the function alloc_on_stack() has
  different limits of how much it will allocate from the stack
  based on the allocation size.
*/

/*
  Common stack size in MariaDB. Cannot be bigger than system default
  stack (common is 8M)
*/
size_t my_stack_size= 299008;
size_t stack_allocation_total= 0;
extern long call_counter;
long call_counter;

ATTRIBUTE_NOINLINE
int test_stack(void *stack_start, void *stack_end, int iteration, size_t stack_allocation)
{
  void *res, *stack;
  my_bool must_be_freed;

  stack= my_get_stack_pointer(&must_be_freed);
  if (stack_start < stack_end)
  {
    if (stack < stack_start || stack > stack_end)
      return 1;
  }
  else
  {
    if (stack < stack_end || stack > stack_start)
      return 1;
  }
  alloc_on_stack(stack_end, res, must_be_freed, stack_allocation);
  bfill(res, stack_allocation, (char) iteration);
  if (!must_be_freed)
  {
    stack_allocation_total+= stack_allocation;
    test_stack(stack_start, stack_end, iteration+1, stack_allocation);
  }
  else
    my_free(res);              /* Was allocated with my_malloc */
  call_counter++;              /* Avoid tail recursion optimization */
  return 0;
}

void test_stack_detection(int stage, size_t stack_allocation)
{
  void *stack_start, *stack_end;
  int res;
  my_get_stack_bounds(&stack_start, &stack_end,
                      (void*) &stack_start, my_stack_size);
  stack_allocation_total= 0;
  res= test_stack(stack_start, stack_end, 1, stack_allocation);
  if (!res)
    ok(1, "%llu bytes allocated on stack of size %ld with %lu alloc size",
       (unsigned long long) stack_allocation_total,
       (long) available_stack_size(stack_start, stack_end),
       (unsigned long) stack_allocation);
  else
    ok(0, "stack checking failed");
}

pthread_handler_t thread_stack_check(void *arg __attribute__((unused)))
{
  my_thread_init();
  test_stack_detection(1, STACK_ALLOC_SMALL_BLOCK_SIZE-1);
  test_stack_detection(2, STACK_ALLOC_SMALL_BLOCK_SIZE+1);
  my_thread_end();
  return 0;
}

int main(int argc __attribute__((unused)), char **argv)
{
  pthread_attr_t thr_attr;
  pthread_t check_thread;
  void *value;

  MY_INIT(argv[0]);

  plan(4);
  test_stack_detection(3, STACK_ALLOC_SMALL_BLOCK_SIZE-1);
  test_stack_detection(4, STACK_ALLOC_SMALL_BLOCK_SIZE+1);

  /* Create a thread and run the same test */
  (void) pthread_attr_init(&thr_attr);
  pthread_attr_setscope(&thr_attr,PTHREAD_SCOPE_SYSTEM);
  (void) my_setstacksize(&thr_attr, my_stack_size);
  pthread_create(&check_thread, &thr_attr, thread_stack_check, 0);
  pthread_join(check_thread, &value);
  (void) pthread_attr_destroy(&thr_attr);

  my_end(0);
  return exit_status();
}
