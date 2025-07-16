#include <string.h>

#ifndef _WIN32
#define FLAGS "819000"
#else
#define FLAGS "8190"
#endif

#ifdef TEST_INTERP_SIEVE
#define ITER "100"
#else
#define ITER "1000"
#endif

MIR_item_t create_mir_func_sieve (MIR_context_t ctx, size_t *len, MIR_module_t *m_res) {
  MIR_module_t m;
  const char *str
    = "\n\
m_sieve: module\n\
sieve:   func i64\n\
         local i64:iter, i64:count, i64:i, i64:k, i64:prime, i64:flags\n\
         alloca flags, " FLAGS
      "\n\
         mov iter, 0\n\
loop:    bge fin, iter, " ITER
      "\n\
         mov count, 0;  mov i, 0\n\
loop2:   mov u8:(flags, i)::n1, 1;  add i, i, 1\n\
         blt loop2, i, " FLAGS
      "\n\
         mov i, 2\n\
loop3:   beq cont3, u8:(flags,i):c, 0\n\
         add prime, i, 1; add k, i, prime\n\
loop4:   bge fin4, k, " FLAGS
      "\n\
         mov u8:(flags, k):c:n2, 0;  add k, k, prime\n\
         jmp loop4\n\
fin4:    add count, count, 1\n\
cont3:   add i, i, 1\n\
         blt loop3, i, " FLAGS
      "\n\
         add iter, iter, 1\n\
         jmp loop\n\
fin:     ret count\n\
         endfunc\n\
         endmodule\n\
";

  if (len != NULL) *len = strlen (str);
  MIR_scan_string (ctx, str);
  m = DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
  if (m_res != NULL) *m_res = m;
  return DLIST_TAIL (MIR_item_t, m->items);
}
