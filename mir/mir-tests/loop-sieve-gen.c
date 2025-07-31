#include "../mir-gen.h"
#include "../real-time.h"
#if defined(TEST_GEN_LOOP)
#include "api-loop.h"
#else
#include "scan-sieve.h"
#endif

#include <inttypes.h>
#if !defined(_WIN32)
#include <unistd.h>
uint64_t get_heap () { return (uint64_t) sbrk (0); }
#else
#include <windows.h>
#include <psapi.h>
uint64_t get_heap () {
  HANDLE ph = GetCurrentProcess ();
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo (ph, &pmc, sizeof (pmc))) return pmc.WorkingSetSize;
  return 0;
}
#endif

#ifndef TEST_GEN_DEBUG
#define TEST_GEN_DEBUG 0
#endif

int main (void) {
  const int N = TEST_GEN_DEBUG ? 1 : 1000;
  double start_time = real_usec_time ();
  uint64_t start_heap = get_heap ();
  double start_execution_time;
  MIR_context_t ctx;
  MIR_module_t m;
  MIR_item_t *funcs;
#if defined(TEST_GEN_LOOP)
  uint64_t (*fun) (uint64_t n_iter);
  uint64_t res, arg = 100000000;
#else
  uint64_t (*fun) (void);
  uint64_t res;
#endif
  for (unsigned level = 0; level <= 3; level++) {
    fprintf (stderr, "+++++++++++++Optimize level = %u:\n", level);
    start_heap = get_heap ();
    start_time = real_usec_time ();
    ctx = MIR_init ();
    fprintf (stderr, "MIR_init end -- %.0f usec\n", real_usec_time () - start_time);
    funcs = malloc (sizeof (MIR_item_t) * N);
    for (int i = 0; i < N; i++) {
#if defined(TEST_GEN_LOOP)
      funcs[i] = create_mir_func_with_loop (ctx, &m);
#else
      funcs[i] = create_mir_func_sieve (ctx, NULL, &m);
#endif
#if TEST_GEN_DEBUG
      if (i == 0) {
        fprintf (stderr, "+++++++++++++original MIR:\n");
        MIR_output (ctx, stderr);
      }
#endif
    }
    fprintf (stderr, "MIR %d funcs creation end -- %.0f usec\n", N, real_usec_time () - start_time);
    for (int i = 0; i < N; i++) MIR_load_module (ctx, funcs[i]->module);
    MIR_gen_init (ctx);
    fprintf (stderr, "MIR_init_gen end -- %.0f usec\n", real_usec_time () - start_time);
    MIR_gen_set_optimize_level (ctx, level);
#if TEST_GEN_DEBUG
    MIR_gen_set_debug_file (ctx, stderr);
#endif
    MIR_link (ctx, MIR_set_gen_interface, NULL);
    for (int i = 0; i < N; i++) fun = MIR_gen (ctx, funcs[i]);
    fprintf (stderr, "MIR_gen end (%d funcs) -- %.0f usec\n", N, real_usec_time () - start_time);
#if defined(TEST_GENERATION_ONLY)
    return 0;
#endif
    start_execution_time = real_usec_time ();
#if defined(TEST_GEN_LOOP)
    res = fun (arg);
    fprintf (stderr, "fun (%ld) -> %ld", (long) arg, (long) res);
#else
    res = fun ();
    fprintf (stderr, "sieve () -> %ld", (long) res);
#endif
    fprintf (stderr, " -- call %.0f usec", real_usec_time () - start_execution_time);
    if (level == 0) /* we can accurately meassure memory only for the 1st run */
      fprintf (stderr, ", memory used = %.1f KB\n", (get_heap () - start_heap) / 1000.0);
    else
      fprintf (stderr, "\n");
    MIR_gen_finish (ctx);
    fprintf (stderr, "MIR_finish_gen end -- %.0f usec\n", real_usec_time () - start_time);
    MIR_finish (ctx);
    fprintf (stderr, "MIR_finish end -- %.0f usec\n", real_usec_time () - start_time);
    free (funcs);
  }
  return 0;
}
