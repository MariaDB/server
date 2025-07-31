/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   A common include file for mir-ppc64.c and mir-gen-ppc64.c
*/

#include "mir.h"

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "pp64 big endian is not supported anymore"
#endif

#define PPC64_STACK_HEADER_SIZE 32
#define PPC64_TOC_OFFSET 24
#define PPC64_FUNC_DESC_LEN 0

#define TARGET_NOP (24 << (32 - 6)) /* ori 0,0,0 */

#define HREG_EL(h) h##_HARD_REG
#define REP_SEP ,
enum {
  REP8 (HREG_EL, R0, R1, R2, R3, R4, R5, R6, R7),
  REP8 (HREG_EL, R8, R9, R10, R11, R12, R13, R14, R15),
  REP8 (HREG_EL, R16, R17, R18, R19, R20, R21, R22, R23),
  REP8 (HREG_EL, R24, R25, R26, R27, R28, R29, R30, R31),
  REP8 (HREG_EL, F0, F1, F2, F3, F4, F5, F6, F7),
  REP8 (HREG_EL, F8, F9, F10, F11, F12, F13, F14, F15),
  REP8 (HREG_EL, F16, F17, F18, F19, F20, F21, F22, F23),
  REP8 (HREG_EL, F24, F25, F26, F27, F28, F29, F30, F31),
  HREG_EL (LR),
};
#undef REP_SEP

static const char *const target_hard_reg_names[] = {
  "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12",
  "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25",
  "r26", "r27", "r28", "r29", "r30", "r31", "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",
  "f7",  "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19",
  "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31", "lr",
};

#define MAX_HARD_REG LR_HARD_REG
static const MIR_reg_t SP_HARD_REG = R1_HARD_REG;
static const MIR_reg_t FP_HARD_REG = R31_HARD_REG;

/* Hard regs not used in machinized code, preferably call used ones. */
static const MIR_reg_t TEMP_INT_HARD_REG1 = R11_HARD_REG, TEMP_INT_HARD_REG2 = R12_HARD_REG;
static const MIR_reg_t TEMP_FLOAT_HARD_REG1 = F11_HARD_REG, TEMP_FLOAT_HARD_REG2 = F12_HARD_REG;
static const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = F11_HARD_REG, TEMP_DOUBLE_HARD_REG2 = F12_HARD_REG;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG1 = F11_HARD_REG;  //???
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG2 = F12_HARD_REG;

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  if (type == MIR_T_LD) return FALSE;
  return MIR_fp_type_p (type) ? F0_HARD_REG <= hard_reg && hard_reg <= F31_HARD_REG
                              : hard_reg < F0_HARD_REG;
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
  return (hard_reg == FP_HARD_REG || hard_reg == SP_HARD_REG
          || hard_reg == LR_HARD_REG
          /* don't bother to allocate R0 as it has special meaning for base reg and of addi: */
          || hard_reg == R0_HARD_REG || hard_reg == R2_HARD_REG || hard_reg == R13_HARD_REG
          || hard_reg == TEMP_INT_HARD_REG1 || hard_reg == TEMP_INT_HARD_REG2
          || hard_reg == TEMP_FLOAT_HARD_REG1 || hard_reg == TEMP_FLOAT_HARD_REG2
          || hard_reg == TEMP_DOUBLE_HARD_REG1 || hard_reg == TEMP_DOUBLE_HARD_REG2
          || hard_reg == TEMP_LDOUBLE_HARD_REG1 || hard_reg == TEMP_LDOUBLE_HARD_REG2);
}

static int target_locs_num (MIR_reg_t loc MIR_UNUSED, MIR_type_t type) {
  return /*loc > MAX_HARD_REG && */ type == MIR_T_LD ? 2 : 1;
}

static inline void push_insn (VARR (uint8_t) * insn_varr, uint32_t insn) {
  uint8_t *p = (uint8_t *) &insn;
  for (size_t i = 0; i < 4; i++) VARR_PUSH (uint8_t, insn_varr, p[i]);
}

static const int PPC_JUMP_OPCODE = 18;

#define LI_OPCODE 14
#define LIS_OPCODE 15
#define ORI_OPCODE 24
#define ORIS_OPCODE 25
#define XOR_OPCODE 31
static inline void ppc64_gen_address (VARR (uint8_t) * insn_varr, unsigned int reg, void *p) {
  uint64_t a = (uint64_t) p;
  if ((a >> 32) == 0) {
    if (((a >> 31) & 1) == 0) { /* lis r,0,Z2 */
      push_insn (insn_varr, (LIS_OPCODE << 26) | (reg << 21) | (0 << 16) | ((a >> 16) & 0xffff));
    } else { /* xor r,r,r; oris r,r,Z2 */
      push_insn (insn_varr,
                 (XOR_OPCODE << 26) | (316 << 1) | (reg << 21) | (reg << 16) | (reg << 11));
      push_insn (insn_varr, (ORIS_OPCODE << 26) | (reg << 21) | (reg << 16) | ((a >> 16) & 0xffff));
    }
  } else {
    if ((a >> 47) != 0) {
      /* lis r,0,Z0; [ori r,r,Z1]; rldicr r,r,32,31; [oris r,r,Z2]; [ori r,r,Z3]: */
      push_insn (insn_varr, (LIS_OPCODE << 26) | (reg << 21) | (0 << 16) | (a >> 48));
      if (((a >> 32) & 0xffff) != 0)
        push_insn (insn_varr,
                   (ORI_OPCODE << 26) | (reg << 21) | (reg << 16) | ((a >> 32) & 0xffff));
    } else {
      /* li r,0,Z1; rldicr r,r,32,31; [oris r,r,Z2]; [ori r,r,Z3]: */
      push_insn (insn_varr, (LI_OPCODE << 26) | (reg << 21) | (0 << 16) | ((a >> 32) & 0xffff));
    }
    push_insn (insn_varr, (30 << 26) | (reg << 21) | (reg << 16) | 0x07c6);
    if (((a >> 16) & 0xffff) != 0)
      push_insn (insn_varr, (ORIS_OPCODE << 26) | (reg << 21) | (reg << 16) | ((a >> 16) & 0xffff));
  }
  if ((a & 0xffff) != 0)
    push_insn (insn_varr, (ORI_OPCODE << 26) | (reg << 21) | (reg << 16) | (a & 0xffff));
}
