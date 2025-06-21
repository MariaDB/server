/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   A common include file for mir-riscv64.c and mir-gen-riscv64.c
*/

#include "mir.h"

#define TARGET_NOP 0x00000013 /* nop:addi zero,zero,0 */

#define HREG_EL(h) h##_HARD_REG
#define REP_SEP ,
enum {
  REP8 (HREG_EL, R0, R1, R2, R3, R4, R5, R6, R7),
  REP8 (HREG_EL, R8, R9, R10, R11, R12, R13, R14, R15),
  REP8 (HREG_EL, R16, R17, R18, R19, R20, R21, R22, R23),
  REP8 (HREG_EL, R24, R25, R26, R27, R28, R29, R30, R31),
  /*Aliases: */ ZERO_HARD_REG = R0_HARD_REG,
  REP7 (HREG_EL, RA, SP, GP, TP, T0, T1, T2),
  REP8 (HREG_EL, FP, S1, A0, A1, A2, A3, A4, A5),
  REP8 (HREG_EL, A6, A7, S2, S3, S4, S5, S6, S7),
  REP8 (HREG_EL, S8, S9, S10, S11, T3, T4, T5, T6),

  REP8 (HREG_EL, F0, F1, F2, F3, F4, F5, F6, F7),
  REP8 (HREG_EL, F8, F9, F10, F11, F12, F13, F14, F15),
  REP8 (HREG_EL, F16, F17, F18, F19, F20, F21, F22, F23),
  REP8 (HREG_EL, F24, F25, F26, F27, F28, F29, F30, F31),
  /* Aliases: */ FT0_HARD_REG = F0_HARD_REG,
  REP7 (HREG_EL, FT1, FT2, FT3, FT4, FT5, FT6, FT7),
  REP8 (HREG_EL, FS0, FS1, FA0, FA1, FA2, FA3, FA4, FA5),
  REP8 (HREG_EL, FA6, FA7, FS2, FS3, FS4, FS5, FS6, FS7),
  REP8 (HREG_EL, FS8, FS9, FS10, FS11, FT8, FT9, FT10, FT11),
};
#undef REP_SEP

static const char *const target_hard_reg_names[] = {
  "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12",
  "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25",
  "r26", "r27", "r28", "r29", "r30", "r31", "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",
  "f7",  "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19",
  "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",
};

#define MAX_HARD_REG F31_HARD_REG

/* Hard regs not used in machinized code, preferably call used ones. */
static const MIR_reg_t TEMP_INT_HARD_REG1 = T5_HARD_REG, TEMP_INT_HARD_REG2 = T6_HARD_REG;
static const MIR_reg_t TEMP_FLOAT_HARD_REG1 = FT10_HARD_REG, TEMP_FLOAT_HARD_REG2 = FT11_HARD_REG;
static const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = FT10_HARD_REG, TEMP_DOUBLE_HARD_REG2 = FT11_HARD_REG;
/* we use only builtins for long double ops: */
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG1 = MIR_NON_VAR;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG2 = MIR_NON_VAR;

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  if (type == MIR_T_LD) return FALSE; /* long double can be in hard regs only for arg passing */
  return MIR_fp_type_p (type) ? hard_reg >= F0_HARD_REG : hard_reg < F0_HARD_REG;
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
  return (hard_reg == ZERO_HARD_REG || hard_reg == FP_HARD_REG || hard_reg == SP_HARD_REG
          || hard_reg == GP_HARD_REG || hard_reg == TP_HARD_REG  // ???
          || hard_reg == TEMP_INT_HARD_REG1 || hard_reg == TEMP_INT_HARD_REG2
          || hard_reg == TEMP_FLOAT_HARD_REG1 || hard_reg == TEMP_FLOAT_HARD_REG2
          || hard_reg == TEMP_DOUBLE_HARD_REG1 || hard_reg == TEMP_DOUBLE_HARD_REG2
          || hard_reg == TEMP_LDOUBLE_HARD_REG1 || hard_reg == TEMP_LDOUBLE_HARD_REG2);
}

static inline int target_locs_num (MIR_reg_t loc, MIR_type_t type) {
  return loc > MAX_HARD_REG && type == MIR_T_LD ? 2 : 1;
}

static const uint32_t j_imm_mask = 0xfffff000;
static inline uint32_t get_j_format_imm (int32_t offset) {
  int d = offset >> 1; /* scale */
  assert (-(1 << 19) <= d && d < (1 << 19));
  return ((d & 0x80000) | ((d & 0x3ff) << 9) | (((d >> 10) & 0x1) << 8) | ((d >> 11) & 0xff)) << 12;
}
