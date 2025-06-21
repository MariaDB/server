/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include <stdlib.h>
#include "mir-code-alloc.h"

#ifdef __GNUC__
#define CODE_ALLOC_UNUSED __attribute__ ((unused))
#else
#define CODE_ALLOC_UNUSED
#endif

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>

static inline int get_native_mem_protect_flags (MIR_mem_protect_t prot) {
  return prot == PROT_WRITE_EXEC ?
#if defined(__riscv)
    (PROT_WRITE | PROT_READ | PROT_EXEC)
#else
    (PROT_WRITE | PROT_EXEC)
#endif
    : (PROT_READ | PROT_EXEC);
}

#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

static int default_mem_protect (void *addr, size_t len, MIR_mem_protect_t prot, void *user_data CODE_ALLOC_UNUSED) {
  int native_prot = get_native_mem_protect_flags (prot);
#if !defined(__APPLE__) || !defined(__aarch64__)
  return mprotect (addr, len, native_prot);
#else
  if ((native_prot & PROT_WRITE) && pthread_jit_write_protect_supported_np ())
    pthread_jit_write_protect_np (FALSE);
  if (native_prot & PROT_READ) {
    if (pthread_jit_write_protect_supported_np ()) pthread_jit_write_protect_np (TRUE);
    sys_icache_invalidate (addr, len);
  } else if (0) {
    if (mprotect (addr, len, native_prot) != 0) {
      perror ("mem_protect");
      fprintf (stderr, "good bye!\n");
      exit (1);
    }
  }
  return 0;
#endif
}

static int default_mem_unmap (void *addr, size_t len, void *user_data CODE_ALLOC_UNUSED) {
  return munmap (addr, len);
}

static void *default_mem_map (size_t len, void *user_data CODE_ALLOC_UNUSED) {
#if defined(__APPLE__) && defined(__aarch64__)
  return mmap (NULL, len, PROT_EXEC | PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT,
               -1, 0);
#else
  return mmap (NULL, len, PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
}
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int default_mem_protect (void *addr, size_t len, MIR_mem_protect_t prot, void *user_data CODE_ALLOC_UNUSED) {
  int native_prod = prot == PROT_WRITE_EXEC ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
  DWORD old_prot = 0;
  return VirtualProtect (addr, len, native_prod, &old_prot) ? 0 : -1;
}

static int default_mem_unmap (void *addr, size_t len, void *user_data CODE_ALLOC_UNUSED) {
  return VirtualFree (addr, len, MEM_RELEASE) ? 0 : -1;
}

static void *default_mem_map (size_t len, void *user_data CODE_ALLOC_UNUSED) {
  return VirtualAlloc (NULL, len, MEM_COMMIT, PAGE_EXECUTE);
}
#endif

static struct MIR_code_alloc default_code_alloc = {
  .mem_map = default_mem_map,
  .mem_unmap = default_mem_unmap,
  .mem_protect = default_mem_protect,
  .user_data = NULL
};
