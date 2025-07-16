#include "../mir.h"
#include "api-loop.h"
#include "../real-time.h"

#include <inttypes.h>

int main (void) {
  MIR_module_t m;
  MIR_item_t func;
  double start_time;
  const int64_t n_iter = 10000000;
  MIR_context_t ctx = MIR_init ();

  func = create_mir_func_with_loop (ctx, &m);
#if MIR_INTERP_DEBUG
  fprintf (stderr, "++++++ Loop before simplification:\n");
  MIR_output (ctx, stderr);
#endif
  MIR_load_module (ctx, m);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
#if MIR_INTERP_DEBUG
  fprintf (stderr, "++++++ Loop after simplification:\n");
  MIR_output (ctx, stderr);
#endif
  start_time = real_sec_time ();
#if MIR_C_INTERFACE
  typedef int64_t (*loop_func) (int64_t);
  MIR_set_interp_interface (ctx, func);
  int64_t res = ((loop_func) func->addr) (n_iter);
  fprintf (stderr, "C interface test (%" PRId64 ") -> %" PRId64 ": %.3f sec\n", n_iter, res,
           real_sec_time () - start_time);
#else
  MIR_val_t val;

  val.i = n_iter;
  MIR_interp (ctx, func, &val, 1, val);
  fprintf (stderr, "test (%" PRId64 ") -> %" PRId64 ": %.3f sec\n", n_iter, val.i,
           real_sec_time () - start_time);
#endif
  MIR_finish (ctx);
  return 0;
}
