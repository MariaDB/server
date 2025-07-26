#include "../mir.h"

#include "api-loop.h"
#include "api-memop.h"

int main (void) {
  MIR_module_t m;
  MIR_context_t ctx = MIR_init ();

  create_mir_func_with_loop (ctx, &m);
  create_mir_example2 (ctx, &m);
  MIR_load_module (ctx, m);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
  fprintf (stderr, "Simplified code:\n");
  MIR_output (ctx, stderr);
  MIR_finish (ctx);
  return 0;
}
