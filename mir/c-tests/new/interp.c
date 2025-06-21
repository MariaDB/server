#include <stdio.h>
/* simple interpreter and execution of program i=0;while (i <= 1000000) { i+=1; } print i; exit 0 */
enum insn_id { MOVI, ADDI, JMP, BGI, PRINT, EXITI };
static int insn_len[] = {3, 4, 2, 4, 2, 2};
typedef long unsigned VALUE;
static int eval (VALUE *program, size_t program_len, VALUE *bp) {
  static void *labels[] = {&&L_MOVI, &&L_ADDI, &&L_JMP, &&L_BGI, &&L_PRINT, &&L_EXITI};
  if (program_len > 0) {
    size_t i, len;
    for (i = 0; i < program_len; i += len) {
      len = insn_len[program[i]];
      program[i] = (VALUE) labels[program[i]]; /* threaded code */
    }
    return 0;
  }
  VALUE *pc = &program[0];
  goto *(void *) pc[0];
L_MOVI:
  bp[pc[1]] = pc[2];
  pc += insn_len[MOVI];
  goto *(void *) pc[0];
L_ADDI:
  bp[pc[1]] = bp[pc[2]] + pc[3];
  pc += insn_len[ADDI];
  goto *(void *) pc[0];
L_JMP:
  pc += (long) pc[1];
  goto *(void *) pc[0];
L_BGI:
  if (bp[pc[2]] > pc[3]) {
    pc += (long) pc[1];
  } else {
    pc += insn_len[BGI];
  }
  goto *(void *) pc[0];
L_PRINT:
  printf ("%ld\n", bp[pc[1]]);
  pc += insn_len[PRINT];
  goto *(void *) pc[0];
L_EXITI:
  return pc[1];
}

static VALUE program[]
  = {MOVI, 0, 0, BGI, 10, 0, 1000000, ADDI, 0, 0, 1, JMP, -8, PRINT, 0, EXITI, 0};

int main () {
  VALUE bp[1];
  eval (program, sizeof (program) / sizeof (VALUE), bp);
  return eval (program, 0, bp);
}
