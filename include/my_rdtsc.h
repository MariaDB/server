/* Copyright (c) 2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Copyright (c) 2019, MariaDB Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  rdtsc3 -- multi-platform timer code
  pgulutzan@mysql.com, 2005-08-29
  modified 2008-11-02
*/

#ifndef MY_RDTSC_H
#define MY_RDTSC_H

# ifndef __has_builtin
#  define __has_builtin(x) 0 /* Compatibility with non-clang compilers */
# endif
# if __has_builtin(__builtin_readcyclecounter)
# elif defined _WIN32
#  include <intrin.h>
# elif defined __i386__ || defined __x86_64__
#  include <x86intrin.h>
# elif defined(__INTEL_COMPILER) && defined(__ia64__) && defined(HAVE_IA64INTRIN_H)
#  include <ia64intrin.h>
# elif defined(HAVE_SYS_TIMES_H) && defined(HAVE_GETHRTIME)
#  include <sys/times.h>
# endif

/**
  Characteristics of a timer.
*/
struct my_timer_unit_info
{
  /** Routine used for the timer. */
  ulonglong routine;
  /** Overhead of the timer. */
  ulonglong overhead;
  /** Frequency of the  timer. */
  ulonglong frequency;
  /** Resolution of the timer. */
  ulonglong resolution;
};

/**
  Characteristics of all the supported timers.
  @sa my_timer_init().
*/
struct my_timer_info
{
  /** Characteristics of the cycle timer. */
  struct my_timer_unit_info cycles;
  /** Characteristics of the nanosecond timer. */
  struct my_timer_unit_info nanoseconds;
  /** Characteristics of the microsecond timer. */
  struct my_timer_unit_info microseconds;
  /** Characteristics of the millisecond timer. */
  struct my_timer_unit_info milliseconds;
  /** Characteristics of the tick timer. */
  struct my_timer_unit_info ticks;
};

typedef struct my_timer_info MY_TIMER_INFO;

C_MODE_START

/**
  A cycle timer.

  On clang we use __builtin_readcyclecounter(), except for AARCH64.
  On other compilers:

  On IA-32 and AMD64, we use the RDTSC instruction.
  On IA-64, we read the ar.itc register.
  On SPARC, we read the tick register.
  On POWER, we read the Time Base Register (which is not really a cycle count
  but a separate counter with less than nanosecond resolution).
  On IBM S/390 System z we use the STCK instruction.
  On ARM, we probably should use the Generic Timer, but should figure out
  how to ensure that it can be accessed.
  On AARCH64, we use the generic timer base register. We override clang
  implementation for aarch64 as it access a PMU register which is not
  guaranteed to be active.
  On RISC-V, we use the rdcycle instruction to read from mcycle register.

  Sadly, we have nothing for the Digital Alpha, MIPS, Motorola m68k,
  HP PA-RISC or other non-mainstream (or obsolete) processors.

  TODO: consider C++11 std::chrono::high_resolution_clock.

  We fall back to gethrtime() where available.

  On the platforms that do not have a CYCLE timer,
  "wait" events are initialized to use NANOSECOND instead of CYCLE
  during performance_schema initialization (at the server startup).

  Linux performance monitor (see "man perf_event_open") can
  provide cycle counter on the platforms that do not have
  other kinds of cycle counters. But we don't use it so far.

  ARM notes
  ---------
  Userspace high precision timing on CNTVCT_EL0 requires that CNTKCTL_EL1
  is set to 1 for each CPU in privileged mode.

  During tests on ARMv7 Debian, perf_even_open() based cycle counter provided
  too low frequency with too high overhead:
  MariaDB [performance_schema]> SELECT * FROM performance_timers;
  +-------------+-----------------+------------------+----------------+
  | TIMER_NAME  | TIMER_FREQUENCY | TIMER_RESOLUTION | TIMER_OVERHEAD |
  +-------------+-----------------+------------------+----------------+
  | CYCLE       | 689368159       | 1                | 970            |
  | NANOSECOND  | 1000000000      | 1                | 308            |
  | MICROSECOND | 1000000         | 1                | 417            |
  | MILLISECOND | 1000            | 1000             | 407            |
  | TICK        | 127             | 1                | 612            |
  +-------------+-----------------+------------------+----------------+
  Therefore, it was decided not to use perf_even_open() on ARM
  (i.e. go without CYCLE and have "wait" events use NANOSECOND by default).

  @return the current timer value, in cycles.
*/
static inline ulonglong my_timer_cycles(void)
{
# if __has_builtin(__builtin_readcyclecounter) && !defined (__aarch64__)
  return __builtin_readcyclecounter();
# elif defined _M_IX86  || defined _M_X64  || defined __i386__ || defined __x86_64__
  return __rdtsc();
#elif defined _M_ARM64
  return _ReadStatusReg(ARM64_CNTVCT);
# elif defined(__INTEL_COMPILER) && defined(__ia64__) && defined(HAVE_IA64INTRIN_H)
  return (ulonglong) __getReg(_IA64_REG_AR_ITC); /* (3116) */
#elif defined(__GNUC__) && defined(__ia64__)
  {
    ulonglong result;
    __asm __volatile__ ("mov %0=ar.itc" : "=r" (result));
    return result;
  }
#elif defined __GNUC__ && defined __powerpc__
  return __builtin_ppc_get_timebase();
#elif defined(__GNUC__) && defined(__sparcv9) && defined(_LP64)
  {
    ulonglong result;
    __asm __volatile__ ("rd %%tick,%0" : "=r" (result));
    return result;
  }
#elif defined(__GNUC__) && defined(__sparc__) && !defined(_LP64)
  {
      union {
              ulonglong wholeresult;
              struct {
                      ulong high;
                      ulong low;
              }       splitresult;
      } result;
    __asm __volatile__ ("rd %%tick,%1; srlx %1,32,%0" : "=r" (result.splitresult.high), "=r" (result.splitresult.low));
    return result.wholeresult;
  }
#elif defined(__GNUC__) && defined(__s390__)
  /* covers both s390 and s390x */
  {
    ulonglong result;
    __asm__ __volatile__ ("stck %0" : "=Q" (result) : : "cc");
    return result;
  }
#elif defined(__GNUC__) && defined (__aarch64__)
  {
    ulonglong result;
    __asm __volatile("mrs	%0, CNTVCT_EL0" : "=&r" (result));
    return result;
  }
#elif defined(__riscv)
  /* Use RDCYCLE (and RDCYCLEH on riscv32) */
  {
# if __riscv_xlen == 32
    ulong result_lo, result_hi0, result_hi1;
    /* Implemented in assembly because Clang insisted on branching. */
    __asm __volatile__(
        "rdcycleh %0\n"
        "rdcycle %1\n"
        "rdcycleh %2\n"
        "sub %0, %0, %2\n"
        "seqz %0, %0\n"
        "sub %0, zero, %0\n"
        "and %1, %1, %0\n"
        : "=r"(result_hi0), "=r"(result_lo), "=r"(result_hi1));
    return (static_cast<ulonglong>(result_hi1) << 32) | result_lo;
# else
    ulonglong result;
    __asm __volatile__("rdcycle %0" : "=r"(result));
    return result;
  }
# endif
#elif defined(HAVE_SYS_TIMES_H) && defined(HAVE_GETHRTIME)
  /* gethrtime may appear as either cycle or nanosecond counter */
  return (ulonglong) gethrtime();
#else
  return 0;
#endif
}

/**
  A nanosecond timer.
  @return the current timer value, in nanoseconds.
*/
ulonglong my_timer_nanoseconds(void);

/**
  A microseconds timer.
  @return the current timer value, in microseconds.
*/
ulonglong my_timer_microseconds(void);

/**
  A millisecond timer.
  @return the current timer value, in milliseconds.
*/
ulonglong my_timer_milliseconds(void);

/**
  A ticks timer.
  @return the current timer value, in ticks.
*/
ulonglong my_timer_ticks(void);

/**
  Timer initialization function.
  @param [out] mti the timer characteristics.
*/
void my_timer_init(MY_TIMER_INFO *mti);

C_MODE_END

#define MY_TIMER_ROUTINE_RDTSC                    5
#define MY_TIMER_ROUTINE_ASM_IA64                 6
#define MY_TIMER_ROUTINE_PPC_GET_TIMEBASE         7
#define MY_TIMER_ROUTINE_GETHRTIME                9
#define MY_TIMER_ROUTINE_READ_REAL_TIME          10
#define MY_TIMER_ROUTINE_CLOCK_GETTIME           11
#define MY_TIMER_ROUTINE_GETTIMEOFDAY            13
#define MY_TIMER_ROUTINE_QUERYPERFORMANCECOUNTER 14
#define MY_TIMER_ROUTINE_GETTICKCOUNT            15
#define MY_TIMER_ROUTINE_TIME                    16
#define MY_TIMER_ROUTINE_TIMES                   17
#define MY_TIMER_ROUTINE_FTIME                   18
#define MY_TIMER_ROUTINE_ASM_GCC_SPARC64         23
#define MY_TIMER_ROUTINE_ASM_GCC_SPARC32         24
#define MY_TIMER_ROUTINE_MACH_ABSOLUTE_TIME      25
#define MY_TIMER_ROUTINE_GETSYSTEMTIMEASFILETIME 26
#define MY_TIMER_ROUTINE_ASM_S390                28
#define MY_TIMER_ROUTINE_AARCH64                 29
#define MY_TIMER_ROUTINE_RISCV                   30

#endif

