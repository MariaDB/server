#include "../mir.h"
#include <inttypes.h>
#include "scan-args.h"

static void pri (int64_t c) { printf ("%" PRIx64 "\n", c); }
static void prf (float f) { printf ("%f\n", f); }
static void prd (double d) { printf ("%f\n", d); }

int main (void) {
  MIR_module_t m;
  MIR_item_t func;
  MIR_context_t ctx = MIR_init ();

  MIR_load_external (ctx, "pri", pri);
  MIR_load_external (ctx, "prf", prf);
  MIR_load_external (ctx, "prd", prd);
  m = create_args_module (ctx);
  func = DLIST_TAIL (MIR_item_t, m->items);
  MIR_load_module (ctx, m);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
#if MIR_C_INTERFACE
  typedef void (*arg_func) (int8_t, int16_t, int32_t, int64_t, float, double, uint32_t, uint8_t,
                            uint16_t, int32_t, int64_t, float, float, float, float, float, float,
                            float, double);
  MIR_set_interp_interface (ctx, func);
  ((arg_func) func->addr) (0x01, 0x0002, 0x00000003, 0x100000004, 1.0, 2.0, 0x00000005, 0x06,
                           0x0007, 0x00000008, 0x100000009, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
                           10.0);
#else
  MIR_val_t v[19];
  v[0].i = 0x01;
  v[1].i = 0x0002;
  v[2].i = 0x00000003;
  v[3].i = 0x100000004;
  v[4].f = 1.0;
  v[5].d = 2.0;
  v[6].i = 0x00000005;
  v[7].i = 0x06;
  v[8].i = 0x0007;
  v[9].i = 0x00000008;
  v[10].i = 0x100000009;
  v[11].f = 3.0;
  v[12].f = 4.0;
  v[13].f = 5.0;
  v[14].f = 6.0;
  v[15].f = 7.0;
  v[16].f = 8.0;
  v[17].f = 9.0;
  v[18].d = 10.0;
  MIR_interp_arr (ctx, func, &v[0], 19, v);
#endif
  MIR_finish (ctx);
  return 0;
}
