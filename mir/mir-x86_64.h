/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   A common include file for mir-x86_64.c and mir-gen-x86_64.c
*/

#include "mir.h"

#define HREG_EL(h) h##_HARD_REG
#define REP_SEP ,
enum {
  REP8 (HREG_EL, AX, CX, DX, BX, SP, BP, SI, DI),
  REP8 (HREG_EL, R8, R9, R10, R11, R12, R13, R14, R15),
  REP8 (HREG_EL, XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7),
  REP8 (HREG_EL, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15),
  REP2 (HREG_EL, ST0, ST1),
};
#undef REP_SEP

static const char *const target_hard_reg_names[] = {
  "rax",   "rcx",   "rdx",   "rbx",   "rsp",   "rbp",  "rsi",  "rdi",  "r8",
  "r9",    "r10",   "r11",   "r12",   "r13",   "r14",  "r15",  "xmm0", "xmm1",
  "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7", "xmm8", "xmm9", "xmm10",
  "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "st0",  "st1",
};

#define MAX_HARD_REG ST1_HARD_REG

/* Hard regs not used in machinized code, preferably call used ones. */
static const MIR_reg_t TEMP_INT_HARD_REG1 = R10_HARD_REG, TEMP_INT_HARD_REG2 = R11_HARD_REG;
#ifndef _WIN32
static const MIR_reg_t TEMP_FLOAT_HARD_REG1 = XMM8_HARD_REG, TEMP_FLOAT_HARD_REG2 = XMM9_HARD_REG;
static const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = XMM8_HARD_REG, TEMP_DOUBLE_HARD_REG2 = XMM9_HARD_REG;
#else
static const MIR_reg_t TEMP_FLOAT_HARD_REG1 = XMM4_HARD_REG, TEMP_FLOAT_HARD_REG2 = XMM5_HARD_REG;
static const MIR_reg_t TEMP_DOUBLE_HARD_REG1 = XMM4_HARD_REG, TEMP_DOUBLE_HARD_REG2 = XMM5_HARD_REG;
#endif
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG1 = MIR_NON_VAR;
static const MIR_reg_t TEMP_LDOUBLE_HARD_REG2 = MIR_NON_VAR;

static inline int target_hard_reg_type_ok_p (MIR_reg_t hard_reg, MIR_type_t type) {
  assert (hard_reg <= MAX_HARD_REG);
  /* For LD we need x87 stack regs and it is too complicated so no
     hard register allocation for LD: */
  if (type == MIR_T_LD) return FALSE;
  return MIR_int_type_p (type) ? hard_reg < XMM0_HARD_REG : hard_reg >= XMM0_HARD_REG;
}

static inline int target_fixed_hard_reg_p (MIR_reg_t hard_reg) {
  assert (hard_reg <= MAX_HARD_REG);
  return (hard_reg == BP_HARD_REG || hard_reg == SP_HARD_REG || hard_reg == TEMP_INT_HARD_REG1
          || hard_reg == TEMP_INT_HARD_REG2 || hard_reg == TEMP_FLOAT_HARD_REG1
          || hard_reg == TEMP_FLOAT_HARD_REG2 || hard_reg == TEMP_DOUBLE_HARD_REG1
          || hard_reg == TEMP_DOUBLE_HARD_REG2 || hard_reg == ST0_HARD_REG
          || hard_reg == ST1_HARD_REG);
}

static inline int target_locs_num (MIR_reg_t loc, MIR_type_t type) {
  return loc > MAX_HARD_REG && type == MIR_T_LD ? 2 : 1;
}
