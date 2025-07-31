#include <stdlib.h>
#include <stdarg.h>
typedef struct T {
  int a[100];
} T;
static void* new_insn (void* ctx, void* code, int nops, va_list argp) {
  T* insn_ops = malloc (sizeof (T) * nops);

  for (int i = 0; i < nops; i++) insn_ops[i] = va_arg (argp, T);
  va_end (argp);
  return insn_ops;
}
int main (void) { return 0; }
