#include "../mir.h"

int main (void) {
  MIR_module_t m;
  MIR_context_t ctx = MIR_init ();

  MIR_scan_string (ctx,
                   "\n\
m_loop: module\n\
loop:   func i64, i64:limit # a comment\n\
\n\
        local i64:count\n\
        mov count, 0\n\
        bge L1, count, limit\n\
L2:     # a separate label\n\
        add count, count, 1; blt L2, count, limit # 2 insn on a line\n\
L1:     ret count  # label with insn\n\
        endfunc\n\
        endmodule\n\
  ");
  MIR_scan_string (ctx,
                   "\n\
m_sieve: module\n\
sieve:   func i64\n\
         local i64:iter, i64:count, i64:i, i64:k, i64:prime, i64:temp, i64:flags\n\
         alloca flags, 819000\n\
         mov iter, 0\n\
loop:    bge fin, iter, 1000\n\
         mov count, 0;  mov i, 0\n\
loop2:   bgt fin2, i, 819000\n\
         mov u8:(flags, i)::noalias1, 1;  add i, i, 1\n\
         jmp loop2\n\
fin2:    mov i, 0\n\
loop3:   bgt fin3, i, 819000\n\
         beq cont3, u8:(flags,i):alias, 0\n\
         add temp, i, i;  add prime, temp, 3;  add k, i, prime\n\
loop4:   bgt fin4, k, 819000\n\
         mov u8:(flags, k):alias:noalias2, 0;  add k, k, prime\n\
         jmp loop4\n\
fin4:    add count, count, 1\n\
cont3:   add i, i, 1\n\
         jmp loop3\n\
fin3:    add iter, iter, 1\n\
         jmp loop\n\
fin:     ret count\n\
         endfunc\n\
         endmodule\n\
");
  MIR_output (ctx, stderr);
  fprintf (stderr, "+++++++++++++After sieve simplification:\n");
  m = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
  MIR_load_module (ctx, m);
  MIR_link (ctx, MIR_set_interp_interface, NULL);
  MIR_output (ctx, stderr);
  MIR_finish (ctx);
  return 0;
}
