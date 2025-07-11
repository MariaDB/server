#include "../mir.h"
#include "../real-time.h"
#define TEST_INTERP_SIEVE
#include "scan-sieve.h"

#include <inttypes.h>

int main (void) {
  MIR_module_t m;
  MIR_item_t func;
  double start_time;
  MIR_context_t ctx = MIR_init ();

  func = create_mir_func_sieve (ctx, NULL, &m);
#if MIR_INTERP_DEBUG
  fprintf (stderr, "\n++++++ SIEVE before simplification:\n");
  MIR_output (ctx, stderr);
#endif
  start_time = real_sec_time ();
  MIR_load_module (ctx, m);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
#if MIR_INTERP_DEBUG
  fprintf (stderr, "++++++ SIEVE after simplification:\n");
  MIR_output (ctx, stderr);
#endif
  fprintf (stderr, "Interpreter init finish: %.3f ms\n", (real_sec_time () - start_time) * 1000.0);
  start_time = real_sec_time ();
#if MIR_C_INTERFACE
  typedef int64_t (*loop_func) (void);
  MIR_set_interp_interface (ctx, func);
  int64_t res = ((loop_func) func->addr) ();
  fprintf (stderr, "C interface SIEVE -> %" PRId64 ": %.3f sec\n", res,
           real_sec_time () - start_time);
#else
  MIR_val_t val;
  MIR_interp (ctx, func, &val, 0);
  fprintf (stderr, "SIEVE -> %" PRId64 ": %.3f sec\n", val.i, real_sec_time () - start_time);
#endif
  MIR_finish (ctx);
  return 0;
}
