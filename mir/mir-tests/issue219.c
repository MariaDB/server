#include "mir.h"
#include "mir-gen.h"

MIR_module_t MIR_get_last_module (MIR_context_t ctx) {
  return DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
}

static const char* ir_add
  = "m_add: module\n\
export add\n\
add: func i32, i32: a0, i32: a1\n\
local i64: r0\n\
add r0, a0, a1\n\
ret r0\n\
endfunc\n\
endmodule\n\
";
static const char* ir_p2
  = "m_1p1: module\n\
import add\n\
proto_add: proto i32, i32: ax, i32: ay\n\
export p2\n\
p2: func i32\n\
local i64: r1\n\
inline proto_add, add, r1, 1, 1\n\
ret r1\n\
endfunc\n\
endmodule\n\
";

int main (void) {
  void* ctx = MIR_init ();
  MIR_gen_init (ctx, 1);
  MIR_scan_string (ctx, ir_add);
  void* m_add = MIR_get_last_module (ctx);
  MIR_load_module (ctx, m_add);
  MIR_link (ctx, MIR_set_gen_interface, NULL);

  MIR_scan_string (ctx, ir_p2);
  void* m_1p1 = MIR_get_last_module (ctx);
  MIR_load_module (ctx, m_1p1);
  MIR_link (ctx, MIR_set_gen_interface, NULL);
}
