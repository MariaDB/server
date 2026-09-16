/* RISC-V CRC-32 runtime detection.

This file is compiled with the normal settings. The Zbc-accelerated core is
in crc32c_riscv_zbc.cc, the only compilation unit for which the ISA extension
is enabled. That way rv_zbc_supported(), which may run on a CPU that does not
implement Zbc, cannot be compiled for the extension; enabling it for a whole
compilation unit was the cause of a past problem (MDEV-24745).

rv_zbc_supported() is also called from an indirect function (ifunc) resolver.
A resolver runs at load time, before libc has been initialised, so it must not
use anything that depends on that: fopen() on /proc/cpuinfo, as an earlier
version of this probe did, faults there. Only the riscv_hwprobe system call is
used instead. Since glibc 2.40 the dynamic linker passes a pointer to
__riscv_hwprobe() as an argument to a RISC-V resolver; on older versions that
argument is NULL and the system call is made directly.
*/

#include <stddef.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifdef __has_include
# if __has_include(<sys/hwprobe.h>)
#  include <sys/hwprobe.h>
#  define HAVE_SYS_HWPROBE_H 1
# endif
#endif

#ifndef HAVE_SYS_HWPROBE_H
/* Fallback for a C library that does not provide <sys/hwprobe.h> (glibc
before 2.40). The structure and the constants are copied from the Linux UAPI
header <asm/hwprobe.h>. */
struct riscv_hwprobe { long long key; unsigned long long value; };
# define RISCV_HWPROBE_KEY_IMA_EXT_0 4
#endif

#ifndef RISCV_HWPROBE_EXT_ZBC
# define RISCV_HWPROBE_EXT_ZBC (1ULL << 7)
#endif

#ifndef SYS_riscv_hwprobe
# ifdef __NR_riscv_hwprobe
#  define SYS_riscv_hwprobe __NR_riscv_hwprobe
# else
#  define SYS_riscv_hwprobe 258 /* riscv_hwprobe, if <sys/syscall.h> is old */
# endif
#endif

typedef unsigned (*my_crc32_t)(unsigned, const void *, size_t);

/* Whether the Zbc extension is available. Safe to call from an ifunc
   resolver: it does not touch any C library state that is not yet
   initialised.

   hwprobe is the __riscv_hwprobe pointer that the dynamic linker passes to a
   resolver (glibc 2.40 and later), or NULL on an older C library. */
extern "C" int rv_zbc_supported(void *hwprobe)
{
  struct riscv_hwprobe p;
  p.key= RISCV_HWPROBE_KEY_IMA_EXT_0;
  p.value= 0;
#ifdef HAVE_SYS_HWPROBE_H
  if (hwprobe != NULL)
  {
    unsigned long long value= 0;
    if (__riscv_hwprobe_one(reinterpret_cast<__riscv_hwprobe_t>(hwprobe),
                            RISCV_HWPROBE_KEY_IMA_EXT_0, &value) != 0)
      return 0;
    return (value & RISCV_HWPROBE_EXT_ZBC) != 0;
  }
#else
  (void) hwprobe;
#endif
  if (syscall(SYS_riscv_hwprobe, &p, (size_t) 1, (size_t) 0, NULL, 0) != 0)
    return 0;
  return (p.value & RISCV_HWPROBE_EXT_ZBC) != 0;
}

extern "C" unsigned crc32c_riscv_zbc(unsigned, const void *, size_t);

extern "C" const char *crc32c_riscv_impl(my_crc32_t c)
{
  if (c == crc32c_riscv_zbc)
    return "Using RISC-V Zbc carry-less multiply instructions";
  return NULL;
}
