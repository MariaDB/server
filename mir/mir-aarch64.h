/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   A common include file for mir-aarch64.c and mir-gen-aarch64.c
*/

#include "mir.h"

#define TARGET_NOP 0xd503201f

#define HREG_EL(h) h##_HARD_REG
#define REP_SEP ,
enum {
  REP8 (HREG_EL, R0, R1, R2, R3, R4, R5, R6, R7),
  REP8 (HREG_EL, R8, R9, R10, R11, R12, R13, R14, R15),
  REP8 (HREG_EL, R16, R17, R18, R19, R20, R21, R22, R23),
  REP8 (HREG_EL, R24, R25, R26, R27, R28, R29, R30, SP),
  ZR_HARD_REG = SP_HARD_REG,
  REP8 (HREG_EL, V0, V1, V2, V3, V4, V5, V6, V7),
  REP8 (HREG_EL, V8, V9, V10, V11, V12, V13, V14, V15),
  REP8 (HREG_EL, V16, V17, V18, V19, V20, V21, V22, V23),
  REP8 (HREG_EL, V24, V25, V26, V27, V28, V29, V30, V31),
};
#undef REP_SEP

static const char *const target_hard_reg_names[] = {
  "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12",
  "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25",
  "r26", "r27", "r28", "r29", "r30", "sp",  "v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",
  "v7",  "v8",  "v9",  "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19",
  "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
};

#define MAX_HARD_REG V31_HARD_REG

/* Hard regs not used in machinized code, preferably call used ones. */
static const MIR_reg_t TEMP_INT_HARD_REG1 = R9_HARD_REG, TEMP_INT_HARD_REG2 = R10_HARD_REG;
static const MIR_reg_t TEMP_FLOAT_HARD_REG1 = V16_HARD_REG, TEMP_FLOAT_HARD_REG2 = V17_HARD_REG;
static const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = V16_HARD_REG, TEMP_DOUBLE_HARD_REG2 = V17_HARD_REG;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG1 = V16_HARD_REG;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG2 = V17_HARD_REG;

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  return MIR_fp_type_p (type) ? hard_reg >= V0_HARD_REG : hard_reg < V0_HARD_REG;
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
#if defined(__APPLE__)
  if (hard_reg == R18_HARD_REG) return TRUE;
#endif
  return (hard_reg == R29_HARD_REG /*FP*/ || hard_reg == SP_HARD_REG
          || hard_reg == TEMP_INT_HARD_REG1 || hard_reg == TEMP_INT_HARD_REG2
          || hard_reg == TEMP_FLOAT_HARD_REG1 || hard_reg == TEMP_FLOAT_HARD_REG2
          || hard_reg == TEMP_DOUBLE_HARD_REG1 || hard_reg == TEMP_DOUBLE_HARD_REG2
          || hard_reg == TEMP_LDOUBLE_HARD_REG1 || hard_reg == TEMP_LDOUBLE_HARD_REG2);
}

static int target_locs_num (MIR_reg_t loc, MIR_type_t type) {
  return loc > MAX_HARD_REG && type == MIR_T_LD ? 2 : 1;
}
