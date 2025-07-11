#include "../mir.h"
#include "scan-hi.h"

static int32_t print (int32_t c) {
  fputc (c, stderr);
  return 1;
}

int main (void) {
  MIR_module_t m;
  MIR_item_t func;
  MIR_val_t val;
  MIR_context_t ctx = MIR_init ();

  MIR_load_external (ctx, "print", print);
  m = create_hi_module (ctx);
  func = DLIST_TAIL (MIR_item_t, m->items);
#if MIR_INTERP_DEBUG
  fprintf (stderr, "\n++++++ Hi func before simplification:\n");
  MIR_output (ctx, stderr);
#endif
  MIR_load_module (ctx, m);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
#if MIR_INTERP_DEBUG
  fprintf (stderr, "++++++ Hi func after simplification:\n");
  MIR_output (ctx, stderr);
#endif
  MIR_interp (ctx, func, &val, 0);
  fprintf (stderr, "func hi returns %ld\n", (long) val.i);
  MIR_finish (ctx);
  return 0;
}
