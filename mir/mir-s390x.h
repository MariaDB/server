/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   A common include file for mir-s390x.c and mir-gen-s390x.c
*/

#include "mir.h"

#define HREG_EL(h) h##_HARD_REG
#define REP_SEP ,
enum {
  REP8 (HREG_EL, R0, R1, R2, R3, R4, R5, R6, R7),
  REP8 (HREG_EL, R8, R9, R10, R11, R12, R13, R14, R15),
  REP8 (HREG_EL, F0, F1, F2, F3, F4, F5, F6, F7),
  REP8 (HREG_EL, F8, F9, F10, F11, F12, F13, F14, F15),
};
#undef REP_SEP

static const char *const target_hard_reg_names[] = {
  "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10",
  "r11", "r12", "r13", "r14", "r15", "f0",  "f1",  "f2",  "f3",  "f4",  "f5",
  "f6",  "f7",  "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
};

#define MAX_HARD_REG F15_HARD_REG
static const MIR_reg_t SP_HARD_REG = R15_HARD_REG;
static const MIR_reg_t FP_HARD_REG = R11_HARD_REG;

static int target_locs_num (MIR_reg_t loc MIR_UNUSED, MIR_type_t type) {
  return type == MIR_T_LD ? 2 : 1;
}

/* Hard regs not used in machinized code and for passing args, preferably call saved ones. */
static const MIR_reg_t TEMP_INT_HARD_REG1 = R1_HARD_REG, TEMP_INT_HARD_REG2 = R9_HARD_REG;
static const MIR_reg_t TEMP_FLOAT_HARD_REG1 = F8_HARD_REG, TEMP_FLOAT_HARD_REG2 = F10_HARD_REG;
static const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = F8_HARD_REG, TEMP_DOUBLE_HARD_REG2 = F10_HARD_REG;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG1 = F8_HARD_REG;  //???
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG2 = F10_HARD_REG;

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  if (type == MIR_T_LD) /* f0,f1,f4,f5,f8,f9,f12,f13 - pair starts */
    return hard_reg >= F0_HARD_REG && (hard_reg - F0_HARD_REG) % 4 <= 1;
  return MIR_fp_type_p (type) ? F0_HARD_REG <= hard_reg && hard_reg <= F15_HARD_REG
                              : hard_reg < F0_HARD_REG;
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
  return (hard_reg == FP_HARD_REG
          || hard_reg == SP_HARD_REG
          /* don't bother to allocate R0 as it has special meaning for base and index reg: */
          || hard_reg == R0_HARD_REG || hard_reg == TEMP_INT_HARD_REG1
          || hard_reg == TEMP_INT_HARD_REG2 || hard_reg == TEMP_FLOAT_HARD_REG1
          || hard_reg == TEMP_FLOAT_HARD_REG2 || hard_reg == TEMP_DOUBLE_HARD_REG1
          || hard_reg == TEMP_DOUBLE_HARD_REG2 || hard_reg == TEMP_LDOUBLE_HARD_REG1
          || hard_reg == TEMP_LDOUBLE_HARD_REG2);
}
