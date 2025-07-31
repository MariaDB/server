#include "../mir.h"
#include "../mir-gen.h"
#include "../real-time.h"

#if defined(_WIN32)
#define SIZE "8190" /* use smaller stack */
#else
#define SIZE "819000"
#endif

static void create_program (MIR_context_t ctx) {
  const char *str
    = "\n\
m_sieve:  module\n\
          export sieve\n\
sieve:    func i32, i32:N\n\
          local i64:iter, i64:count, i64:i, i64:k, i64:prime, i64:temp, i64:flags\n\
          alloca flags, " SIZE
      "\n\
          mov iter, 0\n\
loop:     bge fin, iter, N\n\
          mov count, 0;  mov i, 0\n\
loop2:    bge fin2, i, " SIZE
      "\n\
          mov u8:(flags, i), 1;  add i, i, 1\n\
          jmp loop2\n\
fin2:     mov i, 0\n\
loop3:    bge fin3, i, " SIZE
      "\n\
          beq cont3, u8:(flags,i), 0\n\
          add temp, i, i;  add prime, temp, 3;  add k, i, prime\n\
loop4:    bge fin4, k, " SIZE
      "\n\
          mov u8:(flags, k), 0;  add k, k, prime\n\
          jmp loop4\n\
fin4:     add count, count, 1\n\
cont3:    add i, i, 1\n\
          jmp loop3\n\
fin3:     add iter, iter, 1\n\
          jmp loop\n\
fin:      ret count\n\
          endfunc\n\
          endmodule\n\
m_ex100:  module\n\
format:   string \"sieve of " SIZE
      " 200 times = %d\\n\"\n\
p_printf: proto p:fmt, i32:r\n\
p_sieve:  proto i32, i32:N\n\
          export ex100\n\
          import sieve, printf\n\
ex100:    func\n\
          local i64:r\n\
          call p_sieve, sieve, r, 200\n\
          call p_printf, printf, format, r\n\
          ret\n\
          endfunc\n\
          endmodule\n\
";

  MIR_scan_string (ctx, str);
}

#include <inttypes.h>

int main (void) {
  double start_time = real_usec_time ();
  MIR_module_t m1, m2;
  MIR_item_t f1, f2;
  MIR_context_t ctx = MIR_init ();

  fprintf (stderr, "MIR_init end -- %.0f usec\n", real_usec_time () - start_time);
  create_program (ctx);
  fprintf (stderr, "MIR program creation end -- %.0f usec\n", real_usec_time () - start_time);
  m1 = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx));
  m2 = DLIST_NEXT (MIR_module_t, m1);
  f1 = DLIST_TAIL (MIR_item_t, m1->items);
  f2 = DLIST_TAIL (MIR_item_t, m2->items);
  MIR_load_module (ctx, m2);
  MIR_load_module (ctx, m1);
  MIR_load_external (ctx, "printf", printf);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
  MIR_gen_init (ctx);
  MIR_gen (ctx, f1);
  MIR_interp (ctx, f2, NULL, 0);
  MIR_gen_finish (ctx);
  MIR_finish (ctx);
  fprintf (stderr, "MIR_finish end -- %.0f usec\n", real_usec_time () - start_time);
  return 0;
}
