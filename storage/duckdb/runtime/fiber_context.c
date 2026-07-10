/*
  Copyright 2011, 2012 Kristian Nielsen and Monty Program Ab
            2016 MariaDB Corporation AB

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
  Fiber context implementation for DuckDB cross-engine scan.

  Copied from libmariadb/libmariadb/ma_context.c with libmariadb-
  specific dependencies removed and functions renamed from
  my_context_* to fiber_context_*.
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "fiber_context.h"


#ifdef FIBER_USE_UCONTEXT

typedef void (*uc_func_t)(void);

union pass_void_ptr_as_2_int {
  int a[2];
  void *p;
};

static void
fiber_context_spawn_internal(int i0, int i1)
{
  int err;
  struct fiber_context *c;
  union pass_void_ptr_as_2_int u;

  u.a[0]= i0;
  u.a[1]= i1;
  c= (struct fiber_context *)u.p;

  (*c->user_func)(c->user_data);
  c->active= 0;
  err= setcontext(&c->base_context);
  fprintf(stderr, "fiber_context: setcontext() failed: %d (errno=%d)\n",
          err, errno);
}


int
fiber_context_continue(struct fiber_context *c)
{
  int err;

  if (!c->active)
    return 0;

  err= swapcontext(&c->base_context, &c->spawned_context);
  if (err)
  {
    fprintf(stderr, "fiber_context: swapcontext() failed: %d (errno=%d)\n",
            err, errno);
    return -1;
  }

  return c->active;
}


int
fiber_context_spawn(struct fiber_context *c, void (*f)(void *), void *d)
{
  int err;
  union pass_void_ptr_as_2_int u;

  err= getcontext(&c->spawned_context);
  if (err)
    return -1;
  c->spawned_context.uc_stack.ss_sp= c->stack;
  c->spawned_context.uc_stack.ss_size= c->stack_size;
  c->spawned_context.uc_link= NULL;
  c->user_func= f;
  c->user_data= d;
  c->active= 1;
  u.a[1]= 0;
  u.p= c;
#ifdef __clang__
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wcast-function-type-strict"
#endif
  makecontext(&c->spawned_context, (uc_func_t)fiber_context_spawn_internal, 2,
              u.a[0], u.a[1]);
#ifdef __clang__
# pragma clang diagnostic pop
#endif

  return fiber_context_continue(c);
}


int
fiber_context_yield(struct fiber_context *c)
{
  int err;

  if (!c->active)
    return -1;

  err= swapcontext(&c->spawned_context, &c->base_context);
  if (err)
    return -1;
  return 0;
}

int
fiber_context_init(struct fiber_context *c, size_t stack_size)
{
  memset(c, 0, sizeof(*c));
  if (!(c->stack= malloc(stack_size)))
    return -1;
  c->stack_size= stack_size;
  return 0;
}

void
fiber_context_destroy(struct fiber_context *c)
{
  if (c->stack)
    free(c->stack);
}

#endif  /* FIBER_USE_UCONTEXT */


#ifdef FIBER_USE_X86_64_GCC_ASM

int
fiber_context_spawn(struct fiber_context *c, void (*f)(void *), void *d)
{
  int ret;

  __asm__ __volatile__
    (
     "movq %%rsp, (%[save])\n\t"
     "movq %[stack], %%rsp\n\t"
#if defined(__GCC_HAVE_DWARF2_CFI_ASM) || (defined(__clang__) && __clang_major__ < 13)
     ".cfi_escape 0x07, 16\n\t"
#endif
     "movq %%rbp, 8(%[save])\n\t"
     "movq %%rbx, 16(%[save])\n\t"
     "movq %%r12, 24(%[save])\n\t"
     "movq %%r13, 32(%[save])\n\t"
     "movq %%r14, 40(%[save])\n\t"
     "movq %%r15, 48(%[save])\n\t"
     "leaq 1f(%%rip), %%rax\n\t"
     "leaq 2f(%%rip), %%rcx\n\t"
     "movq %%rax, 56(%[save])\n\t"
     "movq %%rcx, 64(%[save])\n\t"
     "callq *%[f]\n\t"
     "jmpq *56(%[save])\n"
     "1:\n\t"
     "movq (%[save]), %%rsp\n\t"
     "xorl %[ret], %[ret]\n\t"
     "jmp 3f\n"
     "2:\n\t"
     "movl $1, %[ret]\n"
     "3:\n"
     : [ret] "=a" (ret),
       [f] "+S" (f),
       [d] "+D" (d)
     : [stack] "a" (c->stack_top),
       [save] "b" (&c->save[0])
     : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc"
  );

  return ret;
}

int
fiber_context_continue(struct fiber_context *c)
{
  int ret;

  __asm__ __volatile__
    (
     "movq (%[save]), %%rax\n\t"
     "movq %%rsp, (%[save])\n\t"
     "movq %%rax, %%rsp\n\t"
     "movq 8(%[save]), %%rax\n\t"
     "movq %%rbp, 8(%[save])\n\t"
     "movq %%rax, %%rbp\n\t"
     "movq 24(%[save]), %%rax\n\t"
     "movq %%r12, 24(%[save])\n\t"
     "movq %%rax, %%r12\n\t"
     "movq 32(%[save]), %%rax\n\t"
     "movq %%r13, 32(%[save])\n\t"
     "movq %%rax, %%r13\n\t"
     "movq 40(%[save]), %%rax\n\t"
     "movq %%r14, 40(%[save])\n\t"
     "movq %%rax, %%r14\n\t"
     "movq 48(%[save]), %%rax\n\t"
     "movq %%r15, 48(%[save])\n\t"
     "movq %%rax, %%r15\n\t"

     "leaq 1f(%%rip), %%rax\n\t"
     "leaq 2f(%%rip), %%rcx\n\t"
     "movq %%rax, 56(%[save])\n\t"
     "movq 64(%[save]), %%rax\n\t"
     "movq %%rcx, 64(%[save])\n\t"

     "movq 16(%[save]), %%rcx\n\t"
     "movq %%rbx, 16(%[save])\n\t"
     "movq %%rcx, %%rbx\n\t"

     "jmpq *%%rax\n"
     "1:\n\t"
     "movq (%[save]), %%rsp\n\t"
     "movq 8(%[save]), %%rbp\n\t"
     "movq 24(%[save]), %%r12\n\t"
     "movq 32(%[save]), %%r13\n\t"
     "movq 40(%[save]), %%r14\n\t"
     "movq 48(%[save]), %%r15\n\t"
     "xorl %[ret], %[ret]\n\t"
     "jmp 3f\n"
     "2:\n\t"
     "movl $1, %[ret]\n"
     "3:\n"
     : [ret] "=a" (ret)
     : [save] "b" (&c->save[0])
     : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc"
        );

  return ret;
}

int
fiber_context_yield(struct fiber_context *c)
{
  uint64_t *save= &c->save[0];
  __asm__ __volatile__
    (
     "movq (%[save]), %%rax\n\t"
     "movq %%rsp, (%[save])\n\t"
     "movq %%rax, %%rsp\n\t"
     "movq 8(%[save]), %%rax\n\t"
     "movq %%rbp, 8(%[save])\n\t"
     "movq %%rax, %%rbp\n\t"
     "movq 16(%[save]), %%rax\n\t"
     "movq %%rbx, 16(%[save])\n\t"
     "movq %%rax, %%rbx\n\t"
     "movq 24(%[save]), %%rax\n\t"
     "movq %%r12, 24(%[save])\n\t"
     "movq %%rax, %%r12\n\t"
     "movq 32(%[save]), %%rax\n\t"
     "movq %%r13, 32(%[save])\n\t"
     "movq %%rax, %%r13\n\t"
     "movq 40(%[save]), %%rax\n\t"
     "movq %%r14, 40(%[save])\n\t"
     "movq %%rax, %%r14\n\t"
     "movq 48(%[save]), %%rax\n\t"
     "movq %%r15, 48(%[save])\n\t"
     "movq %%rax, %%r15\n\t"
     "movq 64(%[save]), %%rax\n\t"
     "leaq 1f(%%rip), %%rcx\n\t"
     "movq %%rcx, 64(%[save])\n\t"

     "jmpq *%%rax\n"

     "1:\n"
     : [save] "+D" (save)
     :
     : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "memory", "cc"
     );
  return 0;
}

int
fiber_context_init(struct fiber_context *c, size_t stack_size)
{
  memset(c, 0, sizeof(*c));

  if (!(c->stack_bot= malloc(stack_size)))
    return -1;
  /*
    The x86_64 ABI specifies 16-byte stack alignment.
    Also put two zero words at the top of the stack.
  */
  c->stack_top= (void *)
    (( ((intptr_t)c->stack_bot + stack_size) & ~(intptr_t)0xf) - 16);
  memset(c->stack_top, 0, 16);

  return 0;
}

void
fiber_context_destroy(struct fiber_context *c)
{
  if (c->stack_bot)
    free(c->stack_bot);
}

#endif  /* FIBER_USE_X86_64_GCC_ASM */


#ifdef FIBER_USE_I386_GCC_ASM

int
fiber_context_spawn(struct fiber_context *c, void (*f)(void *), void *d)
{
  int ret;

  __asm__ __volatile__
    (
     "movl %%esp, (%[save])\n\t"
     "movl %[stack], %%esp\n\t"
#if defined(__GCC_HAVE_DWARF2_CFI_ASM) || (defined(__clang__) && __clang_major__ < 13)
     ".cfi_escape 0x07, 8\n\t"
#endif
     "pushl %[d]\n\t"
     "movl %%ebp, 4(%[save])\n\t"
     "movl %%ebx, 8(%[save])\n\t"
     "movl %%esi, 12(%[save])\n\t"
     "movl %%edi, 16(%[save])\n\t"
     "call 1f\n"
     "1:\n\t"
     "popl %%eax\n\t"
     "addl $(2f-1b), %%eax\n\t"
     "movl %%eax, 20(%[save])\n\t"
     "addl $(3f-2f), %%eax\n\t"
     "movl %%eax, 24(%[save])\n\t"
     "call *%[f]\n\t"
     "jmp *20(%[save])\n"
     "2:\n\t"
     "movl (%[save]), %%esp\n\t"
     "xorl %[ret], %[ret]\n\t"
     "jmp 4f\n"
     "3:\n\t"
     "movl $1, %[ret]\n"
     "4:\n"
     : [ret] "=a" (ret),
       [f] "+c" (f),
       [d] "+d" (d)
     : [stack] "a" (c->stack_top),
       [save] "D" (&c->save[0])
     : "memory", "cc"
  );

  return ret;
}

int
fiber_context_continue(struct fiber_context *c)
{
  int ret;

  __asm__ __volatile__
    (
     "movl (%[save]), %%eax\n\t"
     "movl %%esp, (%[save])\n\t"
     "movl %%eax, %%esp\n\t"
     "movl 4(%[save]), %%eax\n\t"
     "movl %%ebp, 4(%[save])\n\t"
     "movl %%eax, %%ebp\n\t"
     "movl 8(%[save]), %%eax\n\t"
     "movl %%ebx, 8(%[save])\n\t"
     "movl %%eax, %%ebx\n\t"
     "movl 12(%[save]), %%eax\n\t"
     "movl %%esi, 12(%[save])\n\t"
     "movl %%eax, %%esi\n\t"

     "movl 24(%[save]), %%eax\n\t"
     "call 1f\n"
     "1:\n\t"
     "popl %%ecx\n\t"
     "addl $(2f-1b), %%ecx\n\t"
     "movl %%ecx, 20(%[save])\n\t"
     "addl $(3f-2f), %%ecx\n\t"
     "movl %%ecx, 24(%[save])\n\t"

     "movl 16(%[save]), %%ecx\n\t"
     "movl %%edi, 16(%[save])\n\t"
     "movl %%ecx, %%edi\n\t"

     "jmp *%%eax\n"
     "2:\n\t"
     "movl (%[save]), %%esp\n\t"
     "movl 4(%[save]), %%ebp\n\t"
     "movl 8(%[save]), %%ebx\n\t"
     "movl 12(%[save]), %%esi\n\t"
     "movl 16(%[save]), %%edi\n\t"
     "xorl %[ret], %[ret]\n\t"
     "jmp 4f\n"
     "3:\n\t"
     "movl $1, %[ret]\n"
     "4:\n"
     : [ret] "=a" (ret)
     : [save] "D" (&c->save[0])
     : "ecx", "edx", "memory", "cc"
        );

  return ret;
}

int
fiber_context_yield(struct fiber_context *c)
{
  uint64_t *save= &c->save[0];
  __asm__ __volatile__
    (
     "movl (%[save]), %%eax\n\t"
     "movl %%esp, (%[save])\n\t"
     "movl %%eax, %%esp\n\t"
     "movl 4(%[save]), %%eax\n\t"
     "movl %%ebp, 4(%[save])\n\t"
     "movl %%eax, %%ebp\n\t"
     "movl 8(%[save]), %%eax\n\t"
     "movl %%ebx, 8(%[save])\n\t"
     "movl %%eax, %%ebx\n\t"
     "movl 12(%[save]), %%eax\n\t"
     "movl %%esi, 12(%[save])\n\t"
     "movl %%eax, %%esi\n\t"
     "movl 16(%[save]), %%eax\n\t"
     "movl %%edi, 16(%[save])\n\t"
     "movl %%eax, %%edi\n\t"

     "movl 24(%[save]), %%eax\n\t"
     "call 1f\n"
     "1:\n\t"
     "popl %%ecx\n\t"
     "addl $(2f-1b), %%ecx\n\t"
     "movl %%ecx, 24(%[save])\n\t"

     "jmp *%%eax\n"

     "2:\n"
     : [save] "+d" (save)
     :
     : "eax", "ecx", "memory", "cc"
     );
  return 0;
}

int
fiber_context_init(struct fiber_context *c, size_t stack_size)
{
  memset(c, 0, sizeof(*c));
  if (!(c->stack_bot= malloc(stack_size)))
    return -1;
  c->stack_top= (void *)
    (( ((intptr_t)c->stack_bot + stack_size) & ~(intptr_t)0xf) - 16);
  memset(c->stack_top, 0, 16);

  return 0;
}

void
fiber_context_destroy(struct fiber_context *c)
{
  if (c->stack_bot)
    free(c->stack_bot);
}

#endif  /* FIBER_USE_I386_GCC_ASM */


#ifdef FIBER_USE_AARCH64_GCC_ASM

#if defined __clang_major__ && __clang_major__ >= 12
# define BTI_J_STR "bti j"
#else
# define BTI_J_STR ".inst 0xd503249f"
#endif

int
fiber_context_spawn(struct fiber_context *c, void (*f)(void *), void *d)
{
  register int ret asm("w0");
  register void (*f_reg)(void *) asm("x1") = f;
  register void *d_reg asm("x2") = d;
  register void *stack asm("x13") = c->stack_top;
  register const uint64_t *save asm("x19") = &c->save[0];

  __asm__ __volatile__
    (
     "mov x10, sp\n\t"
     "mov sp, %[stack]\n\t"
#if defined(__GCC_HAVE_DWARF2_CFI_ASM) || (defined(__clang__) && __clang_major__ < 13)
     ".cfi_escape 0x07, 30\n\t"
#endif
     "stp x19, x20, [%[save], #0]\n\t"
     "stp x21, x22, [%[save], #16]\n\t"
     "stp x23, x24, [%[save], #32]\n\t"
     "stp x25, x26, [%[save], #48]\n\t"
     "stp x27, x28, [%[save], #64]\n\t"
     "stp x29, x10, [%[save], #80]\n\t"
     "stp d8, d9,   [%[save], #96]\n\t"
     "stp d10, d11, [%[save], #112]\n\t"
     "stp d12, d13, [%[save], #128]\n\t"
     "stp d14, d15, [%[save], #144]\n\t"
     "adr x10, 1f\n\t"
     "adr x11, 2f\n\t"
     "stp x10, x11, [%[save], #160]\n\t"

     "mov x0, %[d]\n\t"
     "blr %[f]\n\t"
     "ldr x11, [%[save], #160]\n\t"
     "br x11\n"
     "1:\n\t"
     BTI_J_STR "\n\t"
     "ldr x10, [%[save], #88]\n\t"
     "mov sp, x10\n\t"
     "mov %w[ret], #0\n\t"
     "b 3f\n"
     "2:\n\t"
     BTI_J_STR "\n\t"
     "mov %w[ret], #1\n"
     "3:\n"
     : [ret] "=r" (ret),
       [f] "+r" (f_reg),
       [d] "+r" (d_reg),
       [stack] "+r" (stack)
     : [save] "r" (save)
     : "x3", "x4", "x5", "x6", "x7",
       "x9", "x10", "x11", "x14", "x15",
#if defined(__linux__) && !defined(__ANDROID__)
       "x18",
#endif
       "x30",
       "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
       "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
       "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
       "memory", "cc"
  );

  return ret;
}

int
fiber_context_continue(struct fiber_context *c)
{
  register int ret asm("w0");
  register const uint64_t *save asm("x19") = &c->save[0];

  __asm__ __volatile__
    (
     "ldp x13, x11, [%[save], #0]\n\t"
     "stp x19, x20, [%[save], #0]\n\t"
     "mov x20, x11\n\t"

     "ldp x10, x11, [%[save], #16]\n\t"
     "stp x21, x22, [%[save], #16]\n\t"
     "mov x21, x10\n\t"
     "mov x22, x11\n\t"

     "ldp x10, x11, [%[save], #32]\n\t"
     "stp x23, x24, [%[save], #32]\n\t"
     "mov x23, x10\n\t"
     "mov x24, x11\n\t"

     "ldp x10, x11, [%[save], #48]\n\t"
     "stp x25, x26, [%[save], #48]\n\t"
     "mov x25, x10\n\t"
     "mov x26, x11\n\t"

     "ldp x10, x11, [%[save], #64]\n\t"
     "stp x27, x28, [%[save], #64]\n\t"
     "mov x27, x10\n\t"
     "mov x28, x11\n\t"

     "ldp x10, x11, [%[save], #80]\n\t"
     "mov x14, sp\n\t"
     "stp x29, x14, [%[save], #80]\n\t"
     "mov x29, x10\n\t"
     "mov sp, x11\n\t"

     "ldp d0, d1, [%[save], #96]\n\t"
     "stp d8, d9, [%[save], #96]\n\t"
     "fmov d8, d0\n\t"
     "fmov d9, d1\n\t"

     "ldp d0, d1, [%[save], #112]\n\t"
     "stp d10, d11, [%[save], #112]\n\t"
     "fmov d10, d0\n\t"
     "fmov d11, d1\n\t"

     "ldp d0, d1, [%[save], #128]\n\t"
     "stp d12, d13, [%[save], #128]\n\t"
     "fmov d12, d0\n\t"
     "fmov d13, d1\n\t"

     "ldp d0, d1, [%[save], #144]\n\t"
     "stp d14, d15, [%[save], #144]\n\t"
     "fmov d14, d0\n\t"
     "fmov d15, d1\n\t"

     "adr x10, 1f\n\t"
     "adr x11, 2f\n\t"
     "ldr x15, [%[save], #168]\n\t"
     "stp x10, x11, [%[save], #160]\n\t"
     "mov x19, x13\n\t"
     "br x15\n"
     "1:\n\t"
     BTI_J_STR "\n\t"
     "ldr x20, [%[save], #8]\n\t"
     "ldp x21, x22, [%[save], #16]\n\t"
     "ldp x23, x24, [%[save], #32]\n\t"
     "ldp x25, x26, [%[save], #48]\n\t"
     "ldp x27, x28, [%[save], #64]\n\t"
     "ldp x29, x10, [%[save], #80]\n\t"
     "mov sp, x10\n\t"
     "ldp d8, d9, [%[save], #96]\n\t"
     "ldp d10, d11, [%[save], #112]\n\t"
     "ldp d12, d13, [%[save], #128]\n\t"
     "ldp d14, d15, [%[save], #144]\n\t"
     "mov %w[ret], #0\n\t"
     "b 3f\n"
     "2:\n\t"
     BTI_J_STR "\n\t"
     "mov %w[ret], #1\n"
     "3:\n"
     : [ret] "=r" (ret)
     : [save] "r" (save)
     : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
       "x9", "x10", "x11", "x12", "x13", "x14", "x15",
#if defined(__linux__) && !defined(__ANDROID__)
       "x18",
#endif
       "x30",
       "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
       "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
       "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
       "memory", "cc"
        );

  return ret;
}

int
fiber_context_yield(struct fiber_context *c)
{
  register const uint64_t *save asm("x19") = &c->save[0];
  __asm__ __volatile__
    (
     "ldp x13, x11, [%[save], #0]\n\t"
     "stp x19, x20, [%[save], #0]\n\t"
     "mov x20, x11\n\t"

     "ldp x10, x11, [%[save], #16]\n\t"
     "stp x21, x22, [%[save], #16]\n\t"
     "mov x21, x10\n\t"
     "mov x22, x11\n\t"

     "ldp x10, x11, [%[save], #32]\n\t"
     "stp x23, x24, [%[save], #32]\n\t"
     "mov x23, x10\n\t"
     "mov x24, x11\n\t"

     "ldp x10, x11, [%[save], #48]\n\t"
     "stp x25, x26, [%[save], #48]\n\t"
     "mov x25, x10\n\t"
     "mov x26, x11\n\t"

     "ldp x10, x11, [%[save], #64]\n\t"
     "stp x27, x28, [%[save], #64]\n\t"
     "mov x27, x10\n\t"
     "mov x28, x11\n\t"

     "ldp x10, x11, [%[save], #80]\n\t"
     "mov x14, sp\n\t"
     "stp x29, x14, [%[save], #80]\n\t"
     "mov x29, x10\n\t"
     "mov sp, x11\n\t"

     "ldp d0, d1, [%[save], #96]\n\t"
     "stp d8, d9, [%[save], #96]\n\t"
     "fmov d8, d0\n\t"
     "fmov d9, d1\n\t"

     "ldp d0, d1, [%[save], #112]\n\t"
     "stp d10, d11, [%[save], #112]\n\t"
     "fmov d10, d0\n\t"
     "fmov d11, d1\n\t"

     "ldp d0, d1, [%[save], #128]\n\t"
     "stp d12, d13, [%[save], #128]\n\t"
     "fmov d12, d0\n\t"
     "fmov d13, d1\n\t"

     "ldp d0, d1, [%[save], #144]\n\t"
     "stp d14, d15, [%[save], #144]\n\t"
     "fmov d14, d0\n\t"
     "fmov d15, d1\n\t"

     "ldr x11, [%[save], #168]\n\t"
     "adr x10, 1f\n\t"
     "str x10, [%[save], #168]\n\t"
     "mov x19, x13\n\t"
     "br x11\n"

     "1:\n"
     BTI_J_STR "\n\t"
     :
     : [save] "r" (save)
     : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
       "x9", "x10", "x11", "x12", "x13", "x14", "x15",
#if defined(__linux__) && !defined(__ANDROID__)
       "x18",
#endif
       "x30",
       "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
       "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
       "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
       "memory", "cc"
     );
  return 0;
}

int
fiber_context_init(struct fiber_context *c, size_t stack_size)
{
  memset(c, 0, sizeof(*c));

  if (!(c->stack_bot= malloc(stack_size)))
    return -1;
  c->stack_top= (void *)
    (( ((intptr_t)c->stack_bot + stack_size) & ~(intptr_t)0xf) - 16);
  memset(c->stack_top, 0, 16);

  return 0;
}

void
fiber_context_destroy(struct fiber_context *c)
{
  if (c->stack_bot)
    free(c->stack_bot);
}

#endif  /* FIBER_USE_AARCH64_GCC_ASM */


#ifdef FIBER_USE_WIN32_FIBERS

#include <windows.h>

int
fiber_context_yield(struct fiber_context *c)
{
  c->return_value= 1;
  SwitchToFiber(c->app_fiber);
  return 0;
}

static void WINAPI
fiber_context_trampoline(void *p)
{
  struct fiber_context *c= (struct fiber_context *)p;
  for(;;)
  {
    (*(c->user_func))(c->user_arg);
    c->return_value= 0;
    SwitchToFiber(c->app_fiber);
  }
}

int
fiber_context_init(struct fiber_context *c, size_t stack_size)
{
  memset(c, 0, sizeof(*c));
  c->lib_fiber= CreateFiber(stack_size, fiber_context_trampoline, c);
  if (c->lib_fiber)
    return 0;
  return -1;
}

void
fiber_context_destroy(struct fiber_context *c)
{
  if (c->lib_fiber)
  {
    DeleteFiber(c->lib_fiber);
    c->lib_fiber= NULL;
  }
}

int
fiber_context_spawn(struct fiber_context *c, void (*f)(void *), void *d)
{
  c->user_func= f;
  c->user_arg= d;
  return fiber_context_continue(c);
}

int
fiber_context_continue(struct fiber_context *c)
{
  void *current_fiber= IsThreadAFiber() ? GetCurrentFiber()
                                        : ConvertThreadToFiber(c);
  c->app_fiber= current_fiber;
  SwitchToFiber(c->lib_fiber);
  return c->return_value;
}

#endif  /* FIBER_USE_WIN32_FIBERS */


#ifdef FIBER_CONTEXT_DISABLE

int fiber_context_continue(struct fiber_context *c) { return -1; }
int fiber_context_spawn(struct fiber_context *c, void (*f)(void *), void *d) { return -1; }
int fiber_context_yield(struct fiber_context *c) { return -1; }
int fiber_context_init(struct fiber_context *c, size_t stack_size) { return -1; }
void fiber_context_destroy(struct fiber_context *c) { }

#endif
